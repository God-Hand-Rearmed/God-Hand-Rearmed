// VU1 trace replay: an offline check and benchmark for changes to vu1_recomp (runtime/ps2xTest/tools/vu1_recomp.cpp).
// Not part of any build target: it compiles the runtime's VU core into itself (#include "ps2_vu1_core.cpp") and links
// one set of generated VU1 code, so two builds of the generated code can be compared run by run.
//
// Inputs: an F8 VU1 trace (vu1trace_<tick>.bin, written next to GHR.exe with the vu1_<hash>.bin images it needs),
// or the captured images in build/vu1_images for the synthetic mode.
//
//   vu1_replay verify <trace>            compiled code vs the interpreter: runs whose VF/VI registers, VU memory or
//                                        GIF packets differ (0 expected)
//   vu1_replay digest <trace>            one hash per run of the compiled results (diff two builds' outputs)
//   vu1_replay bench <trace> [passes]    time per pass (one pass = the traced frame's VU1 work)
//   vu1_replay survey <trace> [passes]   per (image, entry pc): runs, compiled time and share, cycles, GIF output
//   vu1_replay native <trace> [pc]       the native programs (src/kamite/render) against the interpreter, every field
//   vu1_replay nativebench <trace> [n]   time in the VU call: native programs + AOT vs AOT only
//   vu1_replay nativedump <trace> <out>  the native programs' runs as test vectors for vu1_native_check.cpp
//   vu1_replay chain[native] <trace>     each run's end state here vs the next run's entry state recorded in the game
//                                        (did the game's VU1 leave what this build does?)
//   (the native modes: compile this file with -DVU1_REPLAY_NATIVE -ffp-contract=off -I<repo>/src and link
//   src/kamite/render/*.cpp)
//   vu1_replay synth <image dir> [n]     n random states per image from many entry points, one line per run
//                                        (end pc, E bit, hash without MAC, status, MAC, clip...); SYNTH_VERIFY=1 also
//                                        compares with the interpreter (random states disagree often, as the compiled
//                                        code leaves out stall bookkeeping: compare builds with each other)
//
// Build (Linux, g++ 13; from runtime/): compile vu1_recomp's output (generated/vu1/*.cpp) with
//   g++ -std=c++20 -O2 -msse4.1 -w -c -Ips2xRuntime/include -Ips2xIOP/include -Ips2xRuntime/src/lib/vu <file>
// and this file the same way, then link them all. The VU jobs run with FTZ and DAZ like the game's (main sets them).
#include <pmmintrin.h>
#include <xmmintrin.h>
#include <map>
#include <string>
#include <vector>
#include "ps2_vu1_core.cpp"
#include "ps2_runtime.h"

#include <chrono>
#include <cstdio>
#include <fstream>

const uint32_t g_ps2RecompiledFunctionTableBase = 0u;
const uint32_t g_ps2RecompiledFunctionTableEnd = 0u;
const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0u;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1] = {};

void PS2Memory::submitGifPacket(GifPathId, const unsigned char *, unsigned int, bool, bool) {}
void GS::processGIFPacket(const unsigned char *, unsigned int) {}

void registerGeneratedVu1Programs();
namespace kamite::render
{
    void registerNativeVu1Programs(); // src/kamite/render (native mode only; link those files)
}
#ifndef VU1_REPLAY_NATIVE
void kamite::render::registerNativeVu1Programs() {}
#endif

struct Run
{
    uint32_t maxCycles = 0;
    uint64_t hash = 0;
    VU1InterpreterAccess::TraceState state{};
    std::vector<uint8_t> data; // the input data memory (16 KB)
};

struct Outcome
{
    VU1State state{};
    uint64_t cycle = 0;
    std::vector<uint8_t> data;
    std::vector<std::vector<uint8_t>> packets;
};

// A member specialization has the access VU1InterpreterAccess has (friend of VU1Interpreter): reads m_cycle.
static uint64_t g_cycleOut = 0;
template <>
VU1InterpreterAccess::StepResult VU1InterpreterAccess::fastBlock<0x1ull, 7u>(VU1Interpreter &vu, Vu1FastCtx &)
{
    g_cycleOut = vu.m_cycle;
    return StepResult::Fallback;
}
// pairs mode: the interpreter one instruction pair at a time, recording each pair's pc and the cycle after it (stalls
// included), for fitting a native program's cycle count on data-dependent paths.
struct PairTrace
{
    uint8_t *code = nullptr;
    uint8_t *data = nullptr;
    GS *gs = nullptr;
    uint32_t maxCycles = 0;
    std::vector<std::pair<uint32_t, uint32_t>> pairs; // pc, cycle after the pair (from the entry)
};
static PairTrace g_pairTrace;
template <>
VU1InterpreterAccess::StepResult VU1InterpreterAccess::fastBlock<0x2ull, 7u>(VU1Interpreter &vu, Vu1FastCtx &)
{
    PairTrace &t = g_pairTrace;
    vu.m_activeVuData = t.data;
    vu.m_activeVuDataSize = 0x4000u;
    vu.m_activeGs = t.gs;
    vu.m_activeMemory = nullptr;
    const unsigned csr = _mm_getcsr();
    _mm_setcsr(csr | 0x6000u);
    const uint64_t start = vu.m_cycle, budgetEnd = vu.m_cycle + t.maxCycles;
    StepResult r = StepResult::Budget;
    while (vu.m_cycle < budgetEnd)
    {
        vu1NormShadow.valid = false;
        vu.commitReadyPipelines();
        const uint32_t pc = vu.m_state.pc;
        const Decoded &d = vu.getDecodedInstructionPairForPc(t.code, 0x4000u, nullptr, pc);
        r = stepPair(vu, d, t.data, 0x4000u, *t.gs, nullptr, 0x4000u, budgetEnd);
        t.pairs.push_back({pc, static_cast<uint32_t>(vu.m_cycle - start)});
        if (r != StepResult::Continue)
            break;
    }
    if (r == StepResult::Ended)
        vu.flushPipelines();
    t.pairs.push_back({0xFFFFu, static_cast<uint32_t>(vu.m_cycle - start)});
    _mm_setcsr(csr);
    return r;
}

static uint64_t cycleOf(VU1Interpreter &vu)
{
    Vu1FastCtx *none = nullptr;
    VU1InterpreterAccess::fastBlock<0x1ull, 7u>(vu, *none);
    return g_cycleOut;
}

static uint64_t fnv(const void *p, size_t n, uint64_t h = 0xCBF29CE484222325ull)
{
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

static double g_vuSeconds = 0.0;
static bool g_native = false; // runOne(compiled): a native program first where one matches (native mode)
// VU1_REPLAY_CLEAR_STICKY=1 (native mode): every run starts with the status register's sticky Z/S/U/O bits clear, as at
// the start of a session; the native programs set them (src/kamite/render/vu_vec.h kStatusStickyFlags), so those bits
// are left out of the status comparison.
static const bool g_clearSticky = [] {
    const char *v = std::getenv("VU1_REPLAY_CLEAR_STICKY");
    return v && *v == '1';
}();
static uint64_t g_nativeRuns = 0, g_nativeDeclined = 0;
static Outcome runOne(const Run &run, std::vector<uint8_t> &code, bool compiled)
{
    static VU1Interpreter vu(VU1Interpreter::Unit::VU1);
    alignas(64) static uint8_t gsStorage[1 << 16];
    GS &gs = *reinterpret_cast<GS *>(gsStorage);
    Outcome out;
    out.data = run.data;
    VU1InterpreterAccess::traceRestore(vu, run.state);
    if (g_clearSticky)
        vu.state().status &= ~0x3C0u;
    VU1InterpreterAccess::CompiledProgram program = nullptr;
    if (compiled)
    {
        Vu1CompiledRegistry &registry = vu1CompiledRegistry();
        program = findCompiledVu1Image(registry, code.data(), static_cast<uint32_t>(code.size()));
        t_vu1AssembledTable = registry.currentTable;
    }
    Vu1PacketSink sink;
    sink.forward = false;
    packetSink = &sink;
    vu1NormShadow.valid = false;
    const auto t0 = std::chrono::steady_clock::now();
    bool done = false;
    if (compiled && g_native && vu1CompiledRegistry().currentNatives)
        for (const Vu1NativeSlot &slot : *vu1CompiledRegistry().currentNatives)
            if (slot.pc == run.state.state.pc)
            {
                done = VU1InterpreterAccess::runNative(vu, slot.program, code.data(), out.data.data(),
                                                       static_cast<uint32_t>(out.data.size()), gs, nullptr, run.maxCycles);
                ++(done ? g_nativeRuns : g_nativeDeclined);
                break;
            }
    if (!done)
        VU1InterpreterAccess::runLoopWith(vu, code.data(), static_cast<uint32_t>(code.size()), out.data.data(),
                                          static_cast<uint32_t>(out.data.size()), gs, nullptr, run.maxCycles, program);
    g_vuSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    packetSink = nullptr;
    out.state = vu.state();
    out.cycle = cycleOf(vu);
    out.packets = std::move(sink.packets);
    return out;
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: replay verify|digest|bench <trace> [passes]\n");
        return 2;
    }
    // As the game: the VU jobs run with the EE thread's MXCSR (src/main.cpp: FTZ and DAZ on).
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
    const std::string mode = argv[1];
    if (mode == "synth")
    {
        // synth <image dir> [runs per image]: random states from many entry points of every vu1_*.bin image; one digest
        // line per run (compiled code), for comparing two builds of the generated code.
        registerGeneratedVu1Programs();
        const int perImage = argc > 3 ? std::atoi(argv[3]) : 400;
        std::vector<std::filesystem::path> files;
        for (const auto &e : std::filesystem::directory_iterator(argv[2]))
            if (e.path().filename().string().rfind("vu1_", 0) == 0 && e.path().extension() == ".bin")
                files.push_back(e.path());
        std::sort(files.begin(), files.end());
        uint64_t seed = 0x9E3779B97F4A7C15ull;
        const auto rnd = [&seed]() { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
        for (const auto &file : files)
        {
            std::ifstream image(file, std::ios::binary);
            std::vector<uint8_t> code((std::istreambuf_iterator<char>(image)), std::istreambuf_iterator<char>());
            if (code.size() != 0x4000u)
                continue;
            std::vector<uint32_t> pcs;
            for (uint32_t pc = 0; pc < 0x4000u; pc += 8u)
            {
                uint64_t w;
                std::memcpy(&w, &code[pc], 8);
                if (w != 0u)
                    pcs.push_back(pc);
            }
            if (pcs.empty())
                continue;
            uint64_t combined = 0xCBF29CE484222325ull;
            for (int r = 0; r < perImage; ++r)
            {
                Run run;
                run.maxCycles = 2000u + static_cast<uint32_t>(rnd() % 6000u);
                VU1State &st = run.state.state;
                st = VU1State{};
                for (int i = 1; i < 32; ++i)
                    for (int c = 0; c < 4; ++c)
                    {
                        const uint64_t v = rnd();
                        st.vf[i][c] = (v & 3u) == 0u ? static_cast<float>(static_cast<int32_t>(v >> 40) % 512)
                                                     : static_cast<float>(static_cast<int64_t>(v >> 20) % 200000) / 100.0f;
                    }
                st.vf[0][3] = 1.0f;
                for (int i = 1; i < 16; ++i)
                    st.vi[i] = static_cast<int32_t>(rnd() % 0x400u);
                for (int c = 0; c < 4; ++c)
                    st.acc[c] = static_cast<float>(static_cast<int64_t>(rnd() >> 30) % 1000) / 10.0f;
                st.q = 1.0f + static_cast<float>(rnd() % 100u);
                st.p = 0.5f;
                st.i = static_cast<float>(rnd() % 1000u) / 7.0f;
                st.r = static_cast<uint32_t>(rnd()) & 0x7FFFFFu;
                st.pc = (r < static_cast<int>(pcs.size()) && r % 2 == 0) ? pcs[(r * 7919u) % pcs.size()] : pcs[rnd() % pcs.size()];
                st.top = static_cast<uint32_t>(rnd() % 0x400u);
                st.itop = static_cast<uint32_t>(rnd() % 0x400u);
                run.state.cycle = 1000u;
                run.data.resize(0x4000u);
                for (uint32_t w = 0; w < 0x1000u; ++w)
                {
                    const uint64_t v = rnd();
                    uint32_t word;
                    if ((v & 7u) < 3u)
                        word = static_cast<uint32_t>(v >> 32) & 0x3FFu; // small ints (counts, addresses, GIF tags)
                    else
                    {
                        const float f = static_cast<float>(static_cast<int64_t>(v >> 20) % 400000) / 100.0f;
                        std::memcpy(&word, &f, 4);
                    }
                    std::memcpy(&run.data[w * 4u], &word, 4);
                }
                const Outcome fast = runOne(run, code, true);
                if (std::getenv("SYNTH_VERIFY"))
                {
                    static uint64_t compared = 0, differ = 0;
                    const Outcome slow = runOne(run, code, false);
                    ++compared;
                    if (std::memcmp(fast.state.vf, slow.state.vf, sizeof(fast.state.vf)) != 0 ||
                        std::memcmp(fast.state.vi, slow.state.vi, sizeof(fast.state.vi)) != 0 || fast.data != slow.data ||
                        fast.packets != slow.packets)
                    {
                        ++differ;
                        std::printf("DIFF %s run %d pc %04x\n", file.filename().string().c_str(), r, st.pc);
                    }
                    if (r + 1 == perImage)
                        std::fprintf(stderr, "verify so far: %llu compared, %llu differ\n", static_cast<unsigned long long>(compared),
                                     static_cast<unsigned long long>(differ));
                }
                VU1State masked = fast.state;
                const uint32_t status = masked.status, mac = masked.mac, clip = masked.clip;
                masked.status = 0u;
                uint64_t h = fnv(&masked, sizeof(masked));
                h = fnv(&fast.cycle, sizeof(fast.cycle), h);
                h = fnv(fast.data.data(), fast.data.size(), h);
                for (const auto &pk : fast.packets)
                    h = fnv(pk.data(), pk.size(), h);
                VU1State nomac = masked;
                nomac.mac = 0u;
                uint64_t h2 = fnv(&nomac, sizeof(nomac));
                h2 = fnv(&fast.cycle, sizeof(fast.cycle), h2);
                h2 = fnv(fast.data.data(), fast.data.size(), h2);
                for (const auto &pk : fast.packets)
                    h2 = fnv(pk.data(), pk.size(), h2);
                const bool ended = fast.cycle - 1000u < run.maxCycles; // stopped before the budget: the program ended
                std::printf("endpc %04x ebit %d ", fast.state.pc, fast.state.ebit ? 1 : 0);
                std::printf("end %d nomac %016llx ", ended ? 1 : 0, static_cast<unsigned long long>(h2));
                std::printf("st %03x mac %04x clip %06x ", status, mac, clip);
                std::printf("%s %d pc %04x cycles %llu packets %zu %016llx\n", file.filename().string().c_str(), r, st.pc,
                            static_cast<unsigned long long>(fast.cycle - 1000u), fast.packets.size(), static_cast<unsigned long long>(h));
                combined = fnv(&h, 8, combined);
            }
            std::fprintf(stderr, "%s: %016llx (VU time so far %.3f s)\n", file.filename().string().c_str(), static_cast<unsigned long long>(combined), g_vuSeconds);
        }
        return 0;
    }
    const std::filesystem::path tracePath = argv[2];
    registerGeneratedVu1Programs();

    std::ifstream in(tracePath, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<Run> runs;
    std::vector<uint8_t> data(0x4000u, 0u);
    size_t pos = 0;
    while (pos + 24 + sizeof(VU1InterpreterAccess::TraceState) <= bytes.size())
    {
        uint32_t magic;
        std::memcpy(&magic, &bytes[pos], 4);
        if (magic != 0x52543156u)
        {
            std::fprintf(stderr, "bad magic at %zu (TraceState size %zu?)\n", pos, sizeof(VU1InterpreterAccess::TraceState));
            return 1;
        }
        Run run;
        std::memcpy(&run.maxCycles, &bytes[pos + 4], 4);
        std::memcpy(&run.hash, &bytes[pos + 8], 8);
        pos += 24;
        std::memcpy(&run.state, &bytes[pos], sizeof(run.state));
        pos += sizeof(run.state);
        for (;;)
        {
            uint16_t header[2];
            std::memcpy(header, &bytes[pos], 4);
            pos += 4;
            if (header[0] == 0xFFFFu)
                break;
            std::memcpy(&data[header[0] * 16u], &bytes[pos], header[1] * 16u);
            pos += header[1] * 16u;
        }
        run.data = data;
        runs.push_back(std::move(run));
    }
    std::map<uint64_t, std::vector<uint8_t>> images;
    for (const Run &run : runs)
        if (!images.count(run.hash))
        {
            char name[64];
            std::snprintf(name, sizeof(name), "vu1_%016llx.bin", static_cast<unsigned long long>(run.hash));
            std::ifstream image(tracePath.parent_path() / name, std::ios::binary);
            images[run.hash] = std::vector<uint8_t>((std::istreambuf_iterator<char>(image)), std::istreambuf_iterator<char>());
            if (images[run.hash].size() != 0x4000u)
                std::fprintf(stderr, "image %s missing\n", name);
        }
    std::fprintf(stderr, "%zu runs, %zu images\n", runs.size(), images.size());

    if (mode == "verify" || mode == "digest")
    {
        uint64_t mismatch = 0, compiledRuns = 0;
        std::map<uint64_t, uint64_t> perImage;
        for (size_t i = 0; i < runs.size(); ++i)
        {
            std::vector<uint8_t> &code = images[runs[i].hash];
            if (code.size() != 0x4000u)
                continue;
            const Outcome fast = runOne(runs[i], code, true);
            if (mode == "digest")
            {
                uint64_t h = fnv(&fast.state, sizeof(fast.state));
                h = fnv(&fast.cycle, sizeof(fast.cycle), h);
                h = fnv(fast.data.data(), fast.data.size(), h);
                for (const auto &p : fast.packets)
                    h = fnv(p.data(), p.size(), h);
                std::printf("%zu %016llx %016llx %zu\n", i, static_cast<unsigned long long>(runs[i].hash),
                            static_cast<unsigned long long>(h), fast.packets.size());
                continue;
            }
            const Outcome slow = runOne(runs[i], code, false);
            ++compiledRuns;
            const bool same = std::memcmp(fast.state.vf, slow.state.vf, sizeof(fast.state.vf)) == 0 &&
                              std::memcmp(fast.state.vi, slow.state.vi, sizeof(fast.state.vi)) == 0 &&
                              fast.data == slow.data && fast.packets == slow.packets;
            if (!same)
            {
                ++mismatch;
                ++perImage[runs[i].hash];
                std::printf("run %zu image %016llx pc %04x:\n", i, static_cast<unsigned long long>(runs[i].hash), runs[i].state.state.pc);
                int shown = 0;
                for (int r = 0; r < 32 && shown < 8; ++r)
                    for (int c = 0; c < 4; ++c)
                        if (std::memcmp(&fast.state.vf[r][c], &slow.state.vf[r][c], 4) != 0 && shown++ < 8)
                        {
                            uint32_t a, b;
                            std::memcpy(&a, &fast.state.vf[r][c], 4);
                            std::memcpy(&b, &slow.state.vf[r][c], 4);
                            std::printf("  vf%d.%c fast %08x (%g) interp %08x (%g)\n", r, "xyzw"[c], a, fast.state.vf[r][c], b, slow.state.vf[r][c]);
                        }
                for (int r = 0; r < 16; ++r)
                    if (fast.state.vi[r] != slow.state.vi[r])
                        std::printf("  vi%d fast %d interp %d\n", r, fast.state.vi[r], slow.state.vi[r]);
                size_t dataDiff = 0, first = 0;
                for (size_t b = 0; b < fast.data.size(); b += 4)
                    if (std::memcmp(&fast.data[b], &slow.data[b], 4) != 0)
                    {
                        if (!dataDiff)
                            first = b;
                        ++dataDiff;
                    }
                if (dataDiff)
                {
                    uint32_t a, b;
                    std::memcpy(&a, &fast.data[first], 4);
                    std::memcpy(&b, &slow.data[first], 4);
                    std::printf("  data: %zu words differ, first at %04zx: fast %08x interp %08x\n", dataDiff, first, a, b);
                }
                size_t pk = 0, pkDiff = 0;
                for (; pk < std::min(fast.packets.size(), slow.packets.size()); ++pk)
                    if (fast.packets[pk] != slow.packets[pk])
                        ++pkDiff;
                std::printf("  packets: %zu vs %zu, %zu differ\n", fast.packets.size(), slow.packets.size(), pkDiff);
            }
        }
        if (mode == "verify")
        {
            std::printf("%llu runs compared, %llu differ (vf/vi/data/packets)\n", static_cast<unsigned long long>(compiledRuns),
                        static_cast<unsigned long long>(mismatch));
            for (const auto &[h, n] : perImage)
                std::printf("  image %016llx: %llu\n", static_cast<unsigned long long>(h), static_cast<unsigned long long>(n));
        }
        return 0;
    }
    if (mode == "nativedump")
    {
        // nativedump <trace> <out file>: every run a native program takes, as test vectors for vu1_native_check (the
        // native programs on another platform, without the runtime): the code image, the state and data memory at the
        // entry, and the interpreter's state, data memory, packets and cycles at the end.
        vu1NativeRegistry().mode = 1; // every registered program, checked or not
        kamite::render::registerNativeVu1Programs();
        g_native = true;
        FILE *out = argc > 3 ? std::fopen(argv[3], "wb") : nullptr;
        if (!out)
            return 2;
        uint64_t written = 0;
        for (size_t i = 0; i < runs.size(); ++i)
        {
            std::vector<uint8_t> &code = images[runs[i].hash];
            if (code.size() != 0x4000u)
                continue;
            const uint64_t before = g_nativeRuns;
            runOne(runs[i], code, true);
            if (g_nativeRuns == before)
                continue;
            const Outcome slow = runOne(runs[i], code, false);
            const uint32_t magic = 0x4E315556u, packets = static_cast<uint32_t>(slow.packets.size());
            const uint64_t cycles = slow.cycle - runs[i].state.cycle;
            std::fwrite(&magic, 4, 1, out);
            std::fwrite(code.data(), 1, code.size(), out);
            std::fwrite(&runs[i].state.state, sizeof(VU1State), 1, out);
            std::fwrite(runs[i].data.data(), 1, runs[i].data.size(), out);
            std::fwrite(&slow.state, sizeof(VU1State), 1, out);
            std::fwrite(slow.data.data(), 1, slow.data.size(), out);
            std::fwrite(&cycles, 8, 1, out);
            std::fwrite(&packets, 4, 1, out);
            for (const auto &pk : slow.packets)
            {
                const uint32_t size = static_cast<uint32_t>(pk.size());
                std::fwrite(&size, 4, 1, out);
                std::fwrite(pk.data(), 1, size, out);
            }
            ++written;
        }
        std::fclose(out);
        std::printf("%llu runs written (VU1State %zu bytes)\n", static_cast<unsigned long long>(written), sizeof(VU1State));
        return 0;
    }
    if (mode == "chain" || mode == "chainnative")
    {
        // chain <trace>: each run's end state (here) against the next run's entry state (recorded in the game): the
        // registers carry over between runs, so a mismatch means the game's VU1 left something else than this build
        // does. chainnative: the same with the native programs (the game's build had them on).
        if (mode == "chainnative")
        {
            vu1NativeRegistry().mode = 1;
            kamite::render::registerNativeVu1Programs();
            g_native = true;
        }
        uint64_t compared = 0, differ = 0, shown = 0;
        std::map<std::string, uint64_t> fields;
        for (size_t i = 0; i + 1 < runs.size(); ++i)
        {
            std::vector<uint8_t> &code = images[runs[i].hash];
            if (code.size() != 0x4000u)
                continue;
            const uint64_t before = g_nativeRuns;
            const Outcome o = runOne(runs[i], code, true);
            const bool wasNative = g_nativeRuns != before;
            const VU1State &next = runs[i + 1].state.state;
            ++compared;
            std::vector<std::string> notes;
            for (int r = 1; r < 32; ++r)
                if (std::memcmp(o.state.vf[r], next.vf[r], 16) != 0)
                    notes.push_back("vf" + std::to_string(r));
            for (int r = 1; r < 16; ++r)
                if (o.state.vi[r] != next.vi[r])
                    notes.push_back("vi" + std::to_string(r));
            if (std::memcmp(o.state.acc, next.acc, 16) != 0)
                notes.push_back("acc");
            if (std::memcmp(&o.state.q, &next.q, 4) != 0)
                notes.push_back("q");
            if (o.state.clip != next.clip)
                notes.push_back("clip");
            if (o.state.mac != next.mac)
                notes.push_back("mac");
            if (o.state.status != next.status)
                notes.push_back("status");
            for (const int qw : {8, 9, 20, 21, 30}) // the qwords programs keep across runs (the VIF loads batches above)
                if (std::memcmp(&o.data[qw * 16], &runs[i + 1].data[qw * 16], 16) != 0)
                    notes.push_back("data" + std::to_string(qw));
            if (!notes.empty())
            {
                ++differ;
                for (const std::string &n : notes)
                    ++fields[(wasNative ? "native " : "aot ") + n];
                if (shown++ < 15)
                {
                    std::printf("run %zu pc %04x -> %04x%s:", i, runs[i].state.state.pc, next.pc, wasNative ? " (native)" : "");
                    for (const std::string &n : notes)
                        std::printf(" %s", n.c_str());
                    std::printf("\n");
                }
            }
        }
        std::printf("%llu compared, %llu differ (%llu native runs)\n", static_cast<unsigned long long>(compared),
                    static_cast<unsigned long long>(differ), static_cast<unsigned long long>(g_nativeRuns));
        for (const auto &[f, n] : fields)
            std::printf("  %s: %llu\n", f.c_str(), static_cast<unsigned long long>(n));
        return 0;
    }
    if (mode == "native" || mode == "nativebench")
    {
        // native <trace> [entry pc hex]: every run that a native program takes (src/kamite/render) against the
        // interpreter: each differing field, then counts. nativebench <trace> [passes]: time per pass in the VU call
        // (state restore and copies left out), native + AOT for the rest vs AOT only, and the native-taken runs alone.
        vu1NativeRegistry().mode = 1; // every registered program, checked or not
        kamite::render::registerNativeVu1Programs();
        g_native = true;
        if (mode == "nativebench")
        {
            const int passes = argc > 3 ? std::atoi(argv[3]) : 5;
            std::vector<char> taken(runs.size(), 0);
            for (size_t i = 0; i < runs.size(); ++i)
            {
                std::vector<uint8_t> &code = images[runs[i].hash];
                if (code.size() != 0x4000u)
                    continue;
                const uint64_t before = g_nativeRuns;
                runOne(runs[i], code, true);
                taken[i] = g_nativeRuns != before;
            }
            for (int pass = 0; pass < 2; ++pass)
            {
                g_native = pass == 0;
                double all = 0.0, nativeOnly = 0.0;
                for (int p = 0; p < passes; ++p)
                    for (size_t i = 0; i < runs.size(); ++i)
                    {
                        std::vector<uint8_t> &code = images[runs[i].hash];
                        if (code.size() != 0x4000u)
                            continue;
                        const double before = g_vuSeconds; // the VU call only, not the harness's state copies
                        runOne(runs[i], code, true);
                        const double ms = (g_vuSeconds - before) * 1000.0;
                        all += ms;
                        if (taken[i])
                            nativeOnly += ms;
                    }
                std::printf("%s: %.3f ms per pass, the native programs' runs %.3f ms\n", pass == 0 ? "native + AOT" : "AOT only   ",
                            all / passes, nativeOnly / passes);
            }
            return 0;
        }
        const uint32_t onlyPc = argc > 3 ? static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 16)) : ~0u;
        std::map<std::string, uint64_t> fields;
        uint64_t compared = 0, differ = 0, shown = 0;
        for (size_t i = 0; i < runs.size(); ++i)
        {
            std::vector<uint8_t> &code = images[runs[i].hash];
            if (code.size() != 0x4000u || (onlyPc != ~0u && runs[i].state.state.pc != onlyPc))
                continue;
            const uint64_t before = g_nativeRuns;
            const Outcome fast = runOne(runs[i], code, true);
            if (g_nativeRuns == before)
                continue;
            const Outcome slow = runOne(runs[i], code, false);
            ++compared;

            std::vector<std::string> notes;
            const auto note = [&](const std::string &field, const std::string &text) {
                ++fields[field];
                if (notes.size() < 12)
                    notes.push_back(text);
            };
            char line[256];
            for (int r = 0; r < 32; ++r)
                for (int c = 0; c < 4; ++c)
                    if (std::memcmp(&fast.state.vf[r][c], &slow.state.vf[r][c], 4) != 0)
                    {
                        uint32_t a, b;
                        std::memcpy(&a, &fast.state.vf[r][c], 4);
                        std::memcpy(&b, &slow.state.vf[r][c], 4);
                        std::snprintf(line, sizeof(line), "vf%02d.%c native %08x (%g) interp %08x (%g)", r, "xyzw"[c], a,
                                      fast.state.vf[r][c], b, slow.state.vf[r][c]);
                        note("vf" + std::to_string(r), line);
                    }
            for (int r = 0; r < 16; ++r)
                if (fast.state.vi[r] != slow.state.vi[r])
                {
                    std::snprintf(line, sizeof(line), "vi%02d native %d interp %d", r, fast.state.vi[r], slow.state.vi[r]);
                    note("vi" + std::to_string(r), line);
                }
            for (int c = 0; c < 4; ++c)
                if (std::memcmp(&fast.state.acc[c], &slow.state.acc[c], 4) != 0)
                {
                    std::snprintf(line, sizeof(line), "acc.%c native %g interp %g", "xyzw"[c], fast.state.acc[c], slow.state.acc[c]);
                    note("acc", line);
                }
            const auto scalar = [&](const char *name, uint64_t a, uint64_t b) {
                if (a != b)
                {
                    std::snprintf(line, sizeof(line), "%s native %llx interp %llx", name, static_cast<unsigned long long>(a),
                                  static_cast<unsigned long long>(b));
                    note(name, line);
                }
            };
            uint32_t qa, qb, pa, pb, ia, ib;
            std::memcpy(&qa, &fast.state.q, 4);
            std::memcpy(&qb, &slow.state.q, 4);
            std::memcpy(&pa, &fast.state.p, 4);
            std::memcpy(&pb, &slow.state.p, 4);
            std::memcpy(&ia, &fast.state.i, 4);
            std::memcpy(&ib, &slow.state.i, 4);
            scalar("q", qa, qb);
            scalar("p", pa, pb);
            scalar("i", ia, ib);
            scalar("clip", fast.state.clip, slow.state.clip);
            scalar("mac", fast.state.mac, slow.state.mac);
            scalar("status", fast.state.status & (g_clearSticky ? ~0x3C0u : ~0u),
                   slow.state.status & (g_clearSticky ? ~0x3C0u : ~0u));
            scalar("pc", fast.state.pc, slow.state.pc);
            scalar("cycles", fast.cycle - runs[i].state.cycle, slow.cycle - runs[i].state.cycle);
            size_t dataDiff = 0;
            for (size_t b = 0; b < fast.data.size(); b += 4)
                if (std::memcmp(&fast.data[b], &slow.data[b], 4) != 0)
                {
                    if (dataDiff++ == 0)
                    {
                        uint32_t a, c;
                        std::memcpy(&a, &fast.data[b], 4);
                        std::memcpy(&c, &slow.data[b], 4);
                        std::snprintf(line, sizeof(line), "data qword %zu.%c native %08x interp %08x", b / 16, "xyzw"[(b / 4) % 4], a, c);
                        note("data", line);
                    }
                }
            if (fast.packets != slow.packets)
            {
                std::snprintf(line, sizeof(line), "packets: %zu vs %zu", fast.packets.size(), slow.packets.size());
                note("packets", line);
            }
            if (!notes.empty())
            {
                ++differ;
                if (shown++ < 20)
                {
                    std::printf("run %zu image %016llx pc %04x top %03x:\n", i, static_cast<unsigned long long>(runs[i].hash),
                                runs[i].state.state.pc, runs[i].state.state.top);
                    for (const std::string &n : notes)
                        std::printf("  %s\n", n.c_str());
                }
            }
        }
        std::printf("%llu runs native (%llu declined), %llu compared, %llu differ\n", static_cast<unsigned long long>(g_nativeRuns),
                    static_cast<unsigned long long>(g_nativeDeclined), static_cast<unsigned long long>(compared),
                    static_cast<unsigned long long>(differ));
        for (const auto &[f, n] : fields)
            std::printf("  %s: %llu\n", f.c_str(), static_cast<unsigned long long>(n));
        return 0;
    }
    if (mode == "pairs")
    {
        // pairs <trace> <entry pc hex> [image hash prefix]: per run at that entry, "run <i> top <top> cycles <n>" then
        // one line per instruction pair: pc and the cycles it took (its stalls included); "end <cycles>" last (the
        // final XGKICK's transfer included).
        const uint32_t pc = argc > 3 ? static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 16)) : 0u;
        const std::string prefix = argc > 4 ? argv[4] : "";
        alignas(64) static uint8_t gsStorage[1 << 16];
        static VU1Interpreter vu(VU1Interpreter::Unit::VU1);
        for (size_t i = 0; i < runs.size(); ++i)
        {
            char hash[24];
            std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(runs[i].hash));
            std::vector<uint8_t> &code = images[runs[i].hash];
            if (code.size() != 0x4000u || runs[i].state.state.pc != pc || std::string(hash).rfind(prefix, 0) != 0)
                continue;
            std::vector<uint8_t> data = runs[i].data;
            VU1InterpreterAccess::traceRestore(vu, runs[i].state);
            g_pairTrace = PairTrace{code.data(), data.data(), reinterpret_cast<GS *>(gsStorage), runs[i].maxCycles, {}};
            Vu1PacketSink sink;
            sink.forward = false;
            packetSink = &sink;
            Vu1FastCtx *none = nullptr;
            VU1InterpreterAccess::fastBlock<0x2ull, 7u>(vu, *none);
            packetSink = nullptr;
            std::printf("run %zu top %u cycles %u\n", i, runs[i].state.state.top, g_pairTrace.pairs.back().second);
            uint32_t last = 0;
            for (const auto &[p, c] : g_pairTrace.pairs)
            {
                if (p == 0xFFFFu)
                    std::printf("end %u\n", c - last);
                else
                    std::printf("%04x %u\n", p, c - last);
                last = c;
            }
        }
        return 0;
    }
    if (mode == "survey")
    {
        // survey <trace> [passes]: per (image, entry pc): runs, compiled time, VU cycles, GIF packets and bytes.
        const int passes = argc > 3 ? std::atoi(argv[3]) : 5;
        struct Row { uint64_t runs = 0, cycles = 0, packets = 0, bytes = 0; double ms = 0.0; };
        std::map<std::pair<uint64_t, uint32_t>, Row> rows;
        double total = 0.0;
        for (const Run &run : runs)
        {
            std::vector<uint8_t> &code = images[run.hash];
            if (code.size() != 0x4000u)
                continue;
            Row &row = rows[{run.hash, run.state.state.pc}];
            const auto t0 = std::chrono::steady_clock::now();
            Outcome o;
            for (int p = 0; p < passes; ++p)
                o = runOne(run, code, true);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / passes;
            ++row.runs;
            row.ms += ms;
            total += ms;
            row.cycles += o.cycle - run.state.cycle;
            row.packets += o.packets.size();
            for (const auto &pk : o.packets)
                row.bytes += pk.size();
        }
        std::printf("image             entry  runs   ms/pass  share  cycles/run packets/run bytes/run\n");
        for (const auto &[key, row] : rows)
            std::printf("%016llx %04x %6llu %8.3f %5.1f%% %10.0f %10.1f %9.0f\n", static_cast<unsigned long long>(key.first),
                        key.second, static_cast<unsigned long long>(row.runs), row.ms, 100.0 * row.ms / total,
                        double(row.cycles) / row.runs, double(row.packets) / row.runs, double(row.bytes) / row.runs);
        std::printf("total %.3f ms per pass\n", total);
        return 0;
    }
    if (mode == "bench")
    {
        const int passes = argc > 3 ? std::atoi(argv[3]) : 5;
        for (const bool compiled : {true, false})
        {
            const auto start = std::chrono::steady_clock::now();
            uint64_t cycles = 0;
            for (int p = 0; p < (compiled ? passes : 1); ++p)
                for (const Run &run : runs)
                {
                    std::vector<uint8_t> &code = images[run.hash];
                    if (code.size() != 0x4000u)
                        continue;
                    const Outcome o = runOne(run, code, compiled);
                    cycles += o.cycle - run.state.cycle;
                }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            const int n = compiled ? passes : 1;
            std::printf("%s: %.2f ms per pass (%llu VU cycles per pass, %.1f M VU cycles/s)\n", compiled ? "compiled" : "interpreted",
                        ms / n, static_cast<unsigned long long>(cycles / n), cycles / (ms * 1000.0));
        }
        return 0;
    }
    return 2;
}
