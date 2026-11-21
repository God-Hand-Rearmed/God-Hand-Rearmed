// Native EE functions: the registry and their installation (native.h); the shadow harness is shadow.cpp.

#include "kamite/native/native.h"
#include "kamite/native/shadow.h"
#include "kamite/native/chain_digest.h"

#include "kamite/frame_patches.h"
#include "kamite/function_hooks.h"
#include "kamite/git_commit.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define KM_GETPID _getpid
#else
#include <unistd.h>
#define KM_GETPID getpid
#endif

extern uint64_t g_ps2xEeDeferred;   // EeScheduler.cpp: checkpoint cycles not yet accounted

namespace kamite::native
{
    namespace
    {
        std::vector<NativeFunction> &registry()
        {
            static std::vector<NativeFunction> functions;
            return functions;
        }

        // The installed natives: one trampoline per slot (a template instance per index), no lookup on the call.
        constexpr size_t kMaxNatives = 128;
        struct Installed
        {
            NativeFunction fn;
            PS2Runtime::RecompiledFunction held = nullptr;
        };
        std::array<Installed, kMaxNatives> g_installed;
        size_t g_installedCount = 0;

        template <size_t I>
        void nativeEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const Installed &slot = g_installed[I];
            RealEnv env{rdram, runtime};
            if (ctx->pc != slot.fn.address || !slot.fn.runReal(env, ctx))
                slot.held(rdram, ctx, runtime); // declined: the slot's previous function
        }
        template <size_t... I>
        constexpr std::array<PS2Runtime::RecompiledFunction, kMaxNatives> entries(std::index_sequence<I...>)
        {
            return {&nativeEntry<I>...};
        }
        constexpr std::array<PS2Runtime::RecompiledFunction, kMaxNatives> kEntries = entries(std::make_index_sequence<kMaxNatives>{});

        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        std::string upper(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return text;
        }

        // KAMITE_NATIVE_<SUBSYSTEM>: unset = the checked functions, 0 = none, anything else = all.
        bool switchedOn(const NativeFunction &fn, std::string &how)
        {
            const std::string variable = "KAMITE_NATIVE_" + upper(fn.subsystem);
            const char *value = std::getenv(variable.c_str());
            how = variable + "=" + (value && *value ? value : "unset");
            if (!value || !*value)
                return fn.checked;
            return std::strcmp(value, "0") != 0;
        }

        bool listed(const std::string &list, const NativeFunction &fn)
        {
            size_t at = 0;
            while (at <= list.size())
            {
                size_t end = list.find_first_of(", ;", at);
                if (end == std::string::npos)
                    end = list.size();
                const std::string token = lower(list.substr(at, end - at));
                at = end + 1;
                if (token.empty())
                    continue;
                if (token == "all" || token == lower(fn.name) || token == lower(fn.subsystem))
                    return true;
                if (token.rfind("0x", 0) == 0 && std::strtoul(token.c_str(), nullptr, 16) == fn.address)
                    return true;
            }
            return false;
        }

#if !defined(KAMITE_RELEASE)
        PS2Runtime *g_runtime = nullptr;
        uint64_t chargedCycles() { return g_runtime->eeScheduler().currentCycle() + g_ps2xEeDeferred; }
        const char *replayCase()
        {
            const char *replay = std::getenv("KAMITE_REPLAY_CASE");
            return replay && *replay ? replay : nullptr;
        }
        // A replay's rate as test_mode decides it (--fps60 0|1, else the setting): known at boot, unlike fps60(), which
        // follows the case only once it loads.
        int rate()
        {
            if (const char *fps = std::getenv("KAMITE_REPLAY_FPS60"); replayCase() && fps && *fps)
                return *fps == '1' ? 60 : 30;
            return kamite::fps60() ? 60 : 30;
        }
        // Beside the case during a replay, like the sampler's stacks (<case>.<30|60>.native.txt).
        std::string shadowOutput()
        {
            if (const char *replay = replayCase())
                return std::string(replay) + (rate() == 60 ? ".60" : ".30");
            return "logs/native_" + std::to_string(KM_GETPID());
        }
        std::string shadowHeader()
        {
            const char *replay = replayCase();
            return std::string("commit=") + KAMITE_GIT_COMMIT + " rate=" + std::to_string(rate()) + " case=" + (replay ? replay : "");
        }
        bool g_shadowing = false;
#endif
    }

    void registerFunction(const NativeFunction &function)
    {
        registry().push_back(function);
    }

    void installFromEnvironment(PS2Runtime &runtime)
    {
        libvu0::registerFunctions();
        packet::registerFunctions();

#if !defined(KAMITE_RELEASE)
        const bool replaying = replayCase() != nullptr;
        // KAMITE_NATIVE_SHADOW (claude/native-next.md rung 2): unset = during a replay, every native that is not on.
        const char *shadowValue = std::getenv("KAMITE_NATIVE_SHADOW");
        const bool shadowUnset = !shadowValue || !*shadowValue;
        const std::string shadowList = shadowUnset ? "" : lower(shadowValue);
        const bool shadowNone = shadowList == "none" || shadowList == "0";
        // Rung 3's chain digest (chain_digest.h): every replay, whatever is shadowed.
        if (replaying)
            chain_digest::begin(shadowOutput().c_str());
#endif
        for (const NativeFunction &fn : registry())
        {
            std::string how;
            const bool on = switchedOn(fn, how);
            bool shadowed = false;
#if !defined(KAMITE_RELEASE)
            shadowed = !shadowNone && (shadowUnset ? replaying && !on : listed(shadowList, fn));
#endif
            if (!shadowed && !on)
                continue;
            if (!runtime.hasFunction(fn.address))
            {
                std::fprintf(stderr, "[native] %s 0x%08X: no recompiled function there, left out\n", fn.name, fn.address);
                continue;
            }
            const PS2Runtime::RecompiledFunction held = runtime.lookupFunction(fn.address); // a hook's trampoline, or the recompiled function
#if !defined(KAMITE_RELEASE)
            if (shadowed)
            {
                if (!g_shadowing)
                {
                    g_runtime = &runtime;
                    shadow::Settings settings;
                    settings.output = &shadowOutput;
                    settings.header = &shadowHeader;
                    settings.cycles = &chargedCycles;
                    // A backward branch's charge, read as PS2Runtime::eeCheckpointFull reads it.
                    const char *loop = std::getenv("PS2X_EE_LOOP_CYCLES");
                    settings.loopCycles = loop && *loop ? static_cast<uint32_t>(std::max(1, std::atoi(loop))) : 32u;
                    shadow::begin(settings);
                    g_shadowing = true;
                }
                shadow::add(fn, held);
                FunctionHooks::instance().replace(fn.address, fn.name, &shadow::entry, ReplacementStatus::Native,
                                                  "shadow check: native beside the recompiled code");
                std::fprintf(stderr, "[native] %s 0x%08X: shadow-checked (%s)\n", fn.name, fn.address, how.c_str());
                continue;
            }
#endif
            if (g_installedCount == kMaxNatives)
            {
                std::fprintf(stderr, "[native] %s: more than %zu natives, left out\n", fn.name, kMaxNatives);
                continue;
            }
            g_installed[g_installedCount] = Installed{fn, held};
            FunctionHooks::instance().replace(fn.address, fn.name, kEntries[g_installedCount], ReplacementStatus::Native, how);
            ++g_installedCount;
            std::fprintf(stderr, "[native] %s 0x%08X: native (%s)\n", fn.name, fn.address, how.c_str());
        }
    }

    void flush()
    {
#if !defined(KAMITE_RELEASE)
        chain_digest::end();
        if (g_shadowing)
            shadow::writeSummary();
#endif
    }
}
