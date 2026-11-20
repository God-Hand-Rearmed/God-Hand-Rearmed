#include "ps2recomp/Emitters/function_emitter.h"
#include "ps2recomp/code_generator.h"
#include "ps2recomp/gif_dma_kick_analyzer.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/r5900_decoder.h"
#include "ps2recomp/recompiler_reporter.h"
#include "ps2recomp/types.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <stdexcept>
#include <unordered_set>

namespace ps2recomp
{
    namespace
    {
        // ---- register-locals mode (PS2X_CODEGEN=locals; runtime side: ps2xRuntime/include/ps2_cg.h) ----------------
        // From Vnnc1/gow-native-pc (GPL-3.0, engine/patches/08-codegen-register-locals.patch), which adapted it from
        // the Killzone port's patch 0017 (drakolordx7/KillzoneRecomp). God Hand: Rearmed: build.py sets
        // PS2X_CODEGEN=locals for generate and modules (KAMITE_CODEGEN=classic: the classic code); the registers are
        // read and written back with PS2_CG_LO / PS2_CG_SET_LO (memcpy, no aliasing through uint64_t*).
        // The function body is generated with the classic macros as usual, in chunks (one instruction, one branch with
        // its delay slot, one label); then every chunk that only computes is rewritten to keep the guest GPRs in C++
        // locals, and every chunk that hands ctx to someone else (calls, returns, syscalls, stubs, exceptions, helpers
        // taking ctx) stays classic between PS2_FLUSH() and PS2_RELOAD(). A function the rewrite cannot handle is
        // emitted classic as a whole.
        // PS2X_CODEGEN_FUNCS=<file> (one hex start address per line) restricts the mode to those functions.
        bool localsModeFor(uint32_t functionStart)
        {
            static const bool enabled = []
            {
                const char *mode = std::getenv("PS2X_CODEGEN");
                return mode && std::string(mode) == "locals";
            }();
            static const std::unordered_set<uint32_t> only = []
            {
                std::unordered_set<uint32_t> set;
                if (const char *file = std::getenv("PS2X_CODEGEN_FUNCS"))
                {
                    std::ifstream in(file);
                    std::string line;
                    while (std::getline(in, line))
                        if (!line.empty() && line[0] != '#')
                            set.insert(static_cast<uint32_t>(std::strtoul(line.c_str(), nullptr, 16)));
                }
                return set;
            }();
            return enabled && (only.empty() || only.contains(functionStart));
        }

        enum class ChunkKind
        {
            Label,       // a label definition or a comment
            Instruction, // one non-branch instruction
            Branch,      // a branch / jump with its delay slot (ControlFlowEmitter)
            Other        // GIF DMA capture / kick
        };

        struct Chunk
        {
            ChunkKind kind;
            std::string text;
        };

        struct LocalsUsage
        {
            std::set<int> used;    // GPRs read or written (low 64 bits)
            std::set<int> written; // GPRs written
            std::set<int> hiUsed;  // GPRs used by 128-bit operations (upper 64 bits as well)
            std::set<int> hiWritten;
        };

        bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
        bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

        size_t countOf(const std::string &text, const char *what)
        {
            size_t count = 0;
            const size_t len = std::char_traits<char>::length(what);
            for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + len))
                ++count;
            return count;
        }

        // Does the statement hand ctx (its GPRs) to code outside this function? The classic GPR macros and the memory
        // accessors only use ctx for themselves, so they do not count.
        bool chunkNeedsSync(const std::string &code)
        {
            static const std::regex ctxArg(R"(([A-Za-z_][A-Za-z_0-9:]*(?:->[A-Za-z_][A-Za-z_0-9]*)?)\s*\(\s*(?:rdram\s*,\s*)?ctx\s*[,)])");
            static const std::unordered_set<std::string> safe = {
                "GPR_U32", "GPR_S32", "GPR_U64", "GPR_S64", "GPR_VEC", "SET_GPR_U32", "SET_GPR_S32", "SET_GPR_ZE32",
                "SET_GPR_U64", "SET_GPR_S64", "SET_GPR_VEC", "runtime->Load8", "runtime->Load16", "runtime->Load32",
                "runtime->Load64", "runtime->Load128", "runtime->Store8", "runtime->Store16", "runtime->Store32",
                "runtime->Store64", "runtime->Store128", "FPU_SET_ACC"};
            for (auto it = std::sregex_iterator(code.begin(), code.end(), ctxArg); it != std::sregex_iterator(); ++it)
            {
                if (!safe.contains((*it)[1].str()))
                    return true;
            }
            if (code.find("ctx->r[") != std::string::npos || code.find("ps2_syscalls::") != std::string::npos ||
                code.find("ps2_stubs::") != std::string::npos)
                return true;
            // runtime calls other than the memory / MMIO accessors and the loop checkpoint
            for (size_t at = code.find("runtime->"); at != std::string::npos; at = code.find("runtime->", at + 1))
            {
                const std::string rest = code.substr(at + 9, 24);
                if (rest.rfind("memory().", 0) != 0 && rest.rfind("Load", 0) != 0 && rest.rfind("Store", 0) != 0 &&
                    rest.rfind("eeCheckpointDue()", 0) != 0)
                    return true;
            }
            return false;
        }

        // Rewrites the classic GPR macros to their locals forms and records the registers:
        //   GPR_U32(ctx, 4) -> L_GPR_U32(4)    SET_GPR_S32(ctx, 4, v) -> L_SET_GPR_S32(4, v)    (register 0: PS2_DISCARD(v))
        // Returns false when the text has a form it does not understand (the chunk then stays classic).
        bool rewriteForLocals(const std::string &in, std::string &out, LocalsUsage &use)
        {
            static const std::unordered_set<std::string> kGet = {"GPR_U32", "GPR_S32", "GPR_U64", "GPR_S64", "GPR_VEC"};
            static const std::unordered_set<std::string> kSet = {"SET_GPR_U32", "SET_GPR_S32", "SET_GPR_ZE32",
                                                                 "SET_GPR_U64", "SET_GPR_S64", "SET_GPR_VEC"};
            LocalsUsage local;
            out.clear();
            out.reserve(in.size());
            const size_t n = in.size();
            size_t i = 0;
            auto skipSpaces = [&](size_t &k)
            {
                while (k < n && (in[k] == ' ' || in[k] == '\t'))
                    ++k;
            };
            while (i < n)
            {
                const char c = in[i];
                if (!isIdentStart(c))
                {
                    out.push_back(c);
                    ++i;
                    continue;
                }
                size_t j = i;
                while (j < n && isIdentChar(in[j]))
                    ++j;
                const std::string ident = in.substr(i, j - i);
                const bool get = kGet.contains(ident);
                const bool set = kSet.contains(ident);
                if (!get && !set)
                {
                    out += ident;
                    i = j;
                    continue;
                }
                size_t k = j;
                skipSpaces(k);
                if (k >= n || in[k] != '(')
                    return false;
                ++k;
                skipSpaces(k);
                if (in.compare(k, 3, "ctx") != 0)
                    return false;
                k += 3;
                skipSpaces(k);
                if (k >= n || in[k] != ',')
                    return false;
                ++k;
                skipSpaces(k);
                size_t d = k;
                while (d < n && std::isdigit(static_cast<unsigned char>(in[d])))
                    ++d;
                if (d == k)
                    return false;
                const int reg = std::atoi(in.substr(k, d - k).c_str());
                if (reg < 0 || reg > 31)
                    return false;
                k = d;
                skipSpaces(k);
                const bool vec = ident.size() >= 3 && ident.compare(ident.size() - 3, 3, "VEC") == 0;
                if (set)
                {
                    if (k >= n || in[k] != ',')
                        return false;
                    ++k;
                    skipSpaces(k);
                    if (reg == 0)
                    {
                        out += "PS2_DISCARD(";
                    }
                    else
                    {
                        out += "L_" + ident + "(" + std::to_string(reg) + ", ";
                        local.used.insert(reg);
                        local.written.insert(reg);
                        if (vec)
                        {
                            local.hiUsed.insert(reg);
                            local.hiWritten.insert(reg);
                        }
                    }
                }
                else
                {
                    if (k >= n || in[k] != ')')
                        return false;
                    out += "L_" + ident + "(" + std::to_string(reg);
                    if (reg != 0)
                    {
                        local.used.insert(reg);
                        if (vec)
                            local.hiUsed.insert(reg);
                    }
                }
                i = k;
            }
            if (out.find("ctx->r[") != std::string::npos)
                return false;
            use.used.insert(local.used.begin(), local.used.end());
            use.written.insert(local.written.begin(), local.written.end());
            use.hiUsed.insert(local.hiUsed.begin(), local.hiUsed.end());
            use.hiWritten.insert(local.hiWritten.begin(), local.hiWritten.end());
            return true;
        }

        // The locals body of a function (prologue, resume switch, body), or an empty string when the function has to
        // stay classic.
        std::string buildLocalsBody(const std::vector<Chunk> &chunks, const std::string &switchText, uint32_t functionEnd,
                                    bool fallsThrough)
        {
            static const std::regex labelDef(R"((^|\n)\s*label_\w+\s*:)");
            static const std::regex gotoStmt(R"(goto\s+(label_\w+)\s*;)");
            static const std::regex checkpointBlock(R"(if \(runtime->eeCheckpointDue\(\)\) \{\s*return;\s*\})");
            LocalsUsage use;
            std::string body;
            std::string rewritten;
            for (const Chunk &chunk : chunks)
            {
                if (chunk.kind == ChunkKind::Label)
                {
                    body += chunk.text;
                    continue;
                }
                if (!chunkNeedsSync(chunk.text))
                {
                    // the only return allowed outside a synced chunk is the loop checkpoint's
                    const std::string text = std::regex_replace(chunk.text, checkpointBlock, "if (PS2_CG_DUE()) { return; }");
                    if (countOf(text, "return") == countOf(text, "PS2_CG_DUE()") &&
                        text.find("eeCheckpointDue") == std::string::npos && rewriteForLocals(text, rewritten, use))
                    {
                        body += rewritten;
                        continue;
                    }
                }
                // Classic text on ctx: the registers are written back before it and read again after it; its jumps into
                // the body read them again first. A label inside it could be entered with ctx out of date, so such a
                // function stays classic.
                if (std::regex_search(chunk.text, labelDef))
                    return {};
                body += "    PS2_FLUSH();\n";
                body += std::regex_replace(chunk.text, gotoStmt, "{ PS2_RELOAD(); goto $1; }");
                body += "    PS2_RELOAD();\n";
            }
            std::ostringstream end;
            end << "    PS2_FLUSH();\n";
            if (fallsThrough)
                end << "    ctx->pc = 0x" << std::hex << functionEnd << std::dec << "u;\n";
            body += end.str();

            std::string p;
            for (int r : use.used)
            {
                p += "    uint64_t gpr" + std::to_string(r) + " = PS2_CG_LO(ctx, " + std::to_string(r) + ");\n";
                if (use.hiUsed.contains(r))
                    p += "    uint64_t gprh" + std::to_string(r) + " = PS2_CG_HI(ctx, " + std::to_string(r) + ");\n";
            }
            p += "    constexpr uint64_t gpr0 = 0; constexpr uint64_t gprh0 = 0; (void)gpr0; (void)gprh0;\n";
            std::string flush, reload;
            for (int r : use.written)
            {
                flush += "PS2_CG_SET_LO(ctx, " + std::to_string(r) + ", gpr" + std::to_string(r) + "), ";
                if (use.hiWritten.contains(r))
                    flush += "PS2_CG_SET_HI(ctx, " + std::to_string(r) + ", gprh" + std::to_string(r) + "), ";
            }
            for (int r : use.used)
            {
                reload += "gpr" + std::to_string(r) + " = PS2_CG_LO(ctx, " + std::to_string(r) + "), ";
                if (use.hiUsed.contains(r))
                    reload += "gprh" + std::to_string(r) + " = PS2_CG_HI(ctx, " + std::to_string(r) + "), ";
            }
            p += "#define PS2_FLUSH_E() (" + flush + "0)\n";
            p += "#define PS2_RELOAD_E() (" + reload + "0)\n";
            p += "#define PS2_FLUSH() ((void)PS2_FLUSH_E())\n";
            p += "#define PS2_RELOAD() ((void)PS2_RELOAD_E())\n";
            return p + "\n" + switchText + body +
                   "#undef PS2_FLUSH\n#undef PS2_RELOAD\n#undef PS2_FLUSH_E\n#undef PS2_RELOAD_E\n";
        }

        thread_local bool t_forceClassic = false; // locals rewrite failed: emit this function classic

        Instruction makeSyntheticDelaySlot(uint32_t address)
        {
            Instruction inst{};
            inst.address = address;
            inst.raw = 0;
            inst.opcode = OPCODE_SPECIAL;
            inst.function = SPECIAL_SLL;
            return inst;
        }
    }

    FunctionEmitter::FunctionEmitter(CodeGenerator &codeGenerator)
        : m_codeGenerator(codeGenerator)
    {
    }

    std::string FunctionEmitter::emit(
        const Function &function,
        const std::vector<Instruction> &instructions,
        bool useHeaders)
    {
        CodeGenerator &cg = m_codeGenerator;
        std::stringstream ss;
        const bool locals = !t_forceClassic && localsModeFor(function.start);
        cg.m_currentFunctionName = function.name;

        if (useHeaders)
        {
            ss << "#include <stdexcept>\n";
            ss << "#include \"ps2_runtime_macros.h\"\n";
            ss << "#include \"ps2_runtime.h\"\n";
            ss << "#include <ps2_recompiled_functions.h>\n";
            ss << "#include <ps2_recompiled_stubs.h>\n\n";
            ss << "#include \"ps2_syscalls.h\"\n";
            ss << "#include \"ps2_stubs.h\"\n\n";
            if (locals)
            {
                ss << "#include \"ps2_cg.h\"\n\n";
            }
            ss << "#ifdef PS2_FUNCTION_LOG_TRACKER\n";
            ss << "#include \"ps2_log.h\"\n";
            ss << "#endif\n\n";
        }

        CodeGenerator::AnalysisResult analysisResult = cg.collectInternalBranchTargets(function, instructions);
        std::vector<uint32_t> resumeTargets(analysisResult.resumeEntryPoints.begin(),
                                            analysisResult.resumeEntryPoints.end());
        auto resumeIt = cg.m_resumeEntryTargetsByOwner.find(function.start);
        if (resumeIt != cg.m_resumeEntryTargetsByOwner.end())
        {
            resumeTargets.insert(resumeTargets.end(), resumeIt->second.begin(), resumeIt->second.end());
        }
        std::sort(resumeTargets.begin(), resumeTargets.end());
        resumeTargets.erase(std::unique(resumeTargets.begin(), resumeTargets.end()), resumeTargets.end());
        for (uint32_t target : resumeTargets)
        {
            analysisResult.entryPoints.insert(target);
        }

        const std::unordered_set<uint32_t> &internalTargets = analysisResult.entryPoints;
        ConstantRegisterState constantRegisters;
        GifDmaKickPlan gifDmaKickPlan{};
        ss << "// Function: " << function.name << "\n";
        ss << "// Address: 0x" << std::hex << function.start << " - 0x" << function.end << std::dec << "\n";

        std::string sanitizedName = cg.getFunctionName(function.start);
        if (sanitizedName.empty())
        {
            std::stringstream nameBuilder;
            nameBuilder << "Errorfunc_" << std::hex << function.start;
            sanitizedName = nameBuilder.str();
        }

        ss << "void " << sanitizedName << "(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {\n";
        ss << "#ifdef PS2_FUNCTION_LOG_TRACKER\n";
        ss << "    PS_LOG_ENTRY(\"" << sanitizedName << "\");\n";
        ss << "#endif\n";
        ss << "\n";
        std::stringstream switchText;
        if (!resumeTargets.empty())
        {
            switchText << "    switch (ctx->pc) {\n";
            for (uint32_t target : resumeTargets)
            {
                switchText << "        case 0x" << std::hex << target << "u: goto label_" << target << ";\n"
                           << std::dec;
            }
            switchText << "        default: break;\n";
            switchText << "    }\n\n";
        }
        const std::string headerText = ss.str();
        if (!locals)
        {
            ss << switchText.str();
            ss << "    ctx->pc = 0x" << std::hex << function.start << "u;\n"
               << std::dec;
            ss << "\n";
        }
        // classic mode writes straight to ss; locals mode collects the same text in chunks, rewritten after the loop
        std::vector<Chunk> chunks;
        auto out = [&](ChunkKind kind, const std::string &text)
        {
            if (!locals)
            {
                ss << text;
                return;
            }
            if (!chunks.empty() && chunks.back().kind == ChunkKind::Label && kind == ChunkKind::Label)
                chunks.back().text += text;
            else
                chunks.push_back({kind, text});
        };
        auto hex = [](uint32_t value)
        {
            std::ostringstream o;
            o << std::hex << value;
            return o.str();
        };

        bool lastInstructionWasControlFlow = false;

        for (size_t i = 0; i < instructions.size(); ++i)
        {
            const Instruction &inst = instructions[i];
            lastInstructionWasControlFlow = inst.hasDelaySlot;

            if (internalTargets.contains(inst.address))
            {
                constantRegisters.clear();
                out(ChunkKind::Label, "label_" + hex(inst.address) + ":\n");
            }

            if (cg.m_emitInstructionComments)
            {
                std::string comment = "    // 0x" + hex(inst.address) + ": 0x" + hex(inst.raw);
                std::string disassembly = R5900Decoder::disassembleInstruction(inst);
                if (!disassembly.empty())
                {
                    comment += "  " + disassembly;
                }
                out(ChunkKind::Label, comment + "\n");
            }

            try
            {
                if (inst.hasDelaySlot)
                {
                    const bool hasDecodedDelaySlot =
                        i + 1 < instructions.size() &&
                        instructions[i + 1].address == inst.address + 4u;

                    Instruction syntheticDelaySlot{};
                    const Instruction *delaySlot = nullptr;
                    if (hasDecodedDelaySlot)
                    {
                        delaySlot = &instructions[i + 1];
                    }
                    else
                    {
                        syntheticDelaySlot = makeSyntheticDelaySlot(inst.address + 4u);
                        delaySlot = &syntheticDelaySlot;
                    }

                    if (hasDecodedDelaySlot && internalTargets.contains(delaySlot->address))
                    {
                        out(ChunkKind::Label, "label_" + hex(delaySlot->address) + ":\n");
                    }

                    if (gifDmaKickPlan.valid &&
                        gifDmaKickPlan.completesInDelaySlot &&
                        gifDmaKickPlan.branchIndex == i &&
                        hasDecodedDelaySlot)
                    {
                        out(ChunkKind::Branch, cg.handleBranchDelaySlots(
                            inst,
                            *delaySlot,
                            function,
                            analysisResult,
                            gifDmaDelaySlotOverride(*delaySlot, gifDmaKickPlan, cg.m_emitInstructionComments)));
                        gifDmaKickPlan = {};
                    }
                    else
                    {
                        out(ChunkKind::Branch, cg.handleBranchDelaySlots(inst, *delaySlot, function, analysisResult));
                    }

                    if (hasDecodedDelaySlot)
                    {
                        ++i; // Skip delay slot instruction (handled inside branch logic)
                    }
                    constantRegisters.clear();
                }
                else
                {
                    if (!gifDmaKickPlan.valid)
                    {
                        gifDmaKickPlan = tryBuildGifDmaKickPlan(instructions, i, constantRegisters, internalTargets);
                    }

                    if (gifDmaKickPlan.suppresses(i))
                    {
                        const size_t slot = gifDmaKickPlan.slotFor(i);
                        std::stringstream gif;
                        emitGifDmaCapture(gif, gifDmaKickPlan, slot, "    ");

                        if (gifDmaKickPlan.completesAt(i))
                        {
                            gif << "    ctx->pc = 0x" << std::hex << inst.address << "u;\n"
                                << std::dec;
                            gif << "    " << gifDmaKickCall(gifDmaKickPlan) << "\n";
                            gifDmaKickPlan = {};
                        }
                        out(ChunkKind::Other, gif.str());

                        updateConstantRegisters(inst, constantRegisters);
                        continue;
                    }

                    const MemoryAccessHint memoryHint = resolveMemoryAccessHint(inst, constantRegisters);
                    const std::string code = cg.translateInstruction(inst, memoryHint);
                    // Locals mode stores ctx->pc only where it can be read: statements that call out of the function,
                    // and the instruction before a branch (the branch's resume-in-delay-slot check compares ctx->pc).
                    const bool beforeBranch = i + 1 < instructions.size() && instructions[i + 1].hasDelaySlot;
                    std::string text;
                    if (!locals || beforeBranch || code.find("runtime->") != std::string::npos || chunkNeedsSync(code))
                    {
                        text = "    ctx->pc = 0x" + hex(inst.address) + "u;\n";
                    }
                    text += "    " + code;
                    if (inst.isMmio)
                    {
                        text += " // MMIO: 0x" + hex(inst.mmioAddress);
                    }
                    text += "\n";
                    out(ChunkKind::Instruction, text);

                    updateConstantRegisters(inst, constantRegisters);
                }
            }
            catch (const std::exception &e)
            {
                if (cg.m_reporter)
                {
                    std::ostringstream msg;
                    msg << "translation failed: " << e.what() << " raw=0x" << std::hex << inst.raw;
                    cg.m_reporter->errorAt("codegen", function.name, inst.address, msg.str());
                }

                throw;
            }
        }

        if (locals)
        {
            const std::string body = buildLocalsBody(chunks, switchText.str(), function.end,
                                                     !instructions.empty() && !lastInstructionWasControlFlow);
            if (!body.empty())
            {
                return headerText + body + "}\n";
            }
            t_forceClassic = true; // a chunk the rewrite cannot handle: the whole function classic
            std::string classic = emit(function, instructions, useHeaders);
            t_forceClassic = false;
            return classic;
        }

        // Fallthrough with no terminating branch: publish the next PC so the EE dispatcher does not re-enter this function.
        if (!instructions.empty() && !lastInstructionWasControlFlow)
        {
            ss << "    ctx->pc = 0x" << std::hex << function.end << "u;\n"
               << std::dec;
        }

        ss << "}\n";
        return ss.str();
    }
}
