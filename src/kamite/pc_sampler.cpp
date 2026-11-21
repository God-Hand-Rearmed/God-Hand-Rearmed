#include "kamite/pc_sampler.h"
#include "kamite/frame_patches.h"
#include "kamite/git_commit.h"
#include "kamite/guest_symbols.h"
#include "kamite/log.h"
#include "ps2_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <psapi.h>   // K32EnumProcessModules (kernel32 since Windows 7)
#include <timeapi.h> // timeBeginPeriod (left out by WIN32_LEAN_AND_MEAN)
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dbghelp.lib")
#endif

namespace kamite::pcsampler
{
    namespace
    {
        // The game's main thread context, published by the frame hook on the EE thread. The scheduler's own lookup
        // (currentContext: a hash map and a vector of interrupt contexts) changes under a reader on another thread; a
        // guest thread's context itself stays at one address. Its pc changes with every guest instruction, so it is
        // read once into a local: used directly as a map key it was hashed and then stored as two different values
        // (a node in the wrong bucket), which hung the sampler (earlier builds) or crashed the game.
        std::atomic<const R5900Context *> g_mainContext{nullptr};
#if defined(_WIN32)
        std::atomic<HANDLE> g_eeThread{nullptr}; // the host thread that runs the EE (the frame hook's thread)
        std::atomic<uintptr_t> g_eeStackLow{0}, g_eeStackHigh{0}; // its stack, for the stack walk's bounds
#endif

        // The recompiled function holding pc: its first table slot (resume entries inside a function share the owner's
        // pointer), or pc's 256-byte region outside the table (module code).
        uint32_t owner(uint32_t pc)
        {
            const uint32_t base = g_ps2RecompiledFunctionTableBase;
            if (pc < base || pc >= g_ps2RecompiledFunctionTableEnd || (pc & 3u) != 0u)
                return pc & ~0xFFu;
            static std::unordered_map<uint32_t, uint32_t> cache;
            auto cached = cache.find(pc);
            if (cached != cache.end())
                return cached->second;
            uint32_t slot = (pc - base) >> 2;
            const PS2Runtime::RecompiledFunction fn = g_ps2RecompiledFunctionTable[slot];
            uint32_t result = pc;
            if (fn)
            {
                // A long run of one pointer is a shared fallback (module code, stubs), not one function: capped.
                uint32_t steps = 0;
                while (slot > 0u && steps < 4096u && g_ps2RecompiledFunctionTable[slot - 1u] == fn)
                {
                    --slot;
                    ++steps;
                }
                result = steps >= 4096u ? (pc & ~0xFFu) : base + slot * 4u;
            }
            cache.emplace(pc, result);
            return result;
        }

        template <typename Key>
        std::vector<std::pair<Key, uint32_t>> sorted(const std::unordered_map<Key, uint32_t> &counts)
        {
            std::vector<std::pair<Key, uint32_t>> rows(counts.begin(), counts.end());
            std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
            return rows;
        }

#if defined(_WIN32)
        // Host function of an address in the EE thread ("module!function"; GHR.pdb next to GHR.exe for names).
        std::string hostFunction(DWORD64 address, DWORD64 &start)
        {
            HANDLE process = GetCurrentProcess();
            alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256] = {};
            auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            HMODULE module = nullptr;
            char path[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(address), &module))
                GetModuleFileNameA(module, path, MAX_PATH);
            const char *name = std::strrchr(path, '\\') ? std::strrchr(path, '\\') + 1 : path;
            if (SymFromAddr(process, address, &displacement, symbol))
            {
                start = address - displacement;
                return std::string(name) + "!" + symbol->Name;
            }
            start = address & ~0xFFFull;
            char text[64];
            std::snprintf(text, sizeof(text), "+0x%llx", static_cast<unsigned long long>(start - reinterpret_cast<DWORD64>(module)));
            return std::string(name) + text;
        }
#endif
    }

    void noteMainContext(const R5900Context *ctx)
    {
        g_mainContext.store(ctx, std::memory_order_relaxed);
#if defined(_WIN32)
        if (!g_eeThread.load(std::memory_order_relaxed))
        {
            HANDLE handle = nullptr;
            ULONG_PTR low = 0, high = 0;
            GetCurrentThreadStackLimits(&low, &high);
            g_eeStackLow.store(low);
            g_eeStackHigh.store(high);
            if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
                g_eeThread.store(handle);
        }
#endif
    }

#if defined(_WIN32)
    namespace
    {
        // KAMITE_PC_SAMPLE_ALL=1: every thread of the process (the GS thread of pcsx2gs.dll, the VU1 thread, the IOP,
        // the window loop...) sampled about 250 times a second, reported with the main thread's report: per thread its
        // share of time busy (not inside a kernel wait) and its hottest host functions. For finding the frame's
        // bottleneck when the EE thread itself is mostly waiting (the casino, Oct 26).
        struct ThreadSample
        {
            HANDLE handle = nullptr;
            DWORD id = 0;
            std::string name;
            uint32_t samples = 0, busy = 0;
            std::unordered_map<DWORD64, uint32_t> rips;
        };
        std::vector<ThreadSample> g_threads;

        void refreshThreads()
        {
            const DWORD process = GetCurrentProcessId(), self = GetCurrentThreadId();
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return;
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            std::vector<DWORD> seen;
            for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
            {
                if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self)
                    continue;
                seen.push_back(entry.th32ThreadID);
                bool known = false;
                for (const ThreadSample &t : g_threads)
                    known = known || t.id == entry.th32ThreadID;
                if (known)
                    continue;
                HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
                if (!handle)
                    continue;
                ThreadSample t;
                t.handle = handle;
                t.id = entry.th32ThreadID;
                PWSTR description = nullptr;
                if (SUCCEEDED(GetThreadDescription(handle, &description)) && description)
                {
                    char text[128] = {};
                    WideCharToMultiByte(CP_UTF8, 0, description, -1, text, sizeof(text) - 1, nullptr, nullptr);
                    t.name = text;
                    LocalFree(description);
                }
                g_threads.push_back(std::move(t));
            }
            CloseHandle(snapshot);
            for (size_t i = 0; i < g_threads.size();)
            {
                if (std::find(seen.begin(), seen.end(), g_threads[i].id) == seen.end())
                {
                    CloseHandle(g_threads[i].handle);
                    g_threads.erase(g_threads.begin() + static_cast<std::ptrdiff_t>(i));
                }
                else
                    ++i;
            }
        }

        bool isWait(const std::string &function)
        {
            // Kernel waits and the like: the thread is idle there.
            return function.find("ntdll.dll!Zw") != std::string::npos || function.find("ntdll.dll!Nt") != std::string::npos ||
                   function.find("WaitFor") != std::string::npos || function.find("Sleep") != std::string::npos ||
                   function.find("!Zw") != std::string::npos;
        }

        // ---- Guest call chains (rung 1 of the native road, claude/native-next.md) ---------------------------------
        // Recompiled guest calls are host calls, so the EE thread's host stack holds the guest call chain. Each sample
        // walks it while the thread is suspended, with nothing that can take a lock or allocate (the thread may hold
        // the heap or loader lock): each module's own .pdata, searched here, and RtlVirtualUnwind (pure computation).
        // The frames are named once the thread runs again. Chains are counted as folded stacks ("a;b;c count") and
        // written beside the case during a replay (<case>.<30|60>.stacks.txt), else to logs\stacks_<pid>.txt; read
        // offline by tools/perf/subsystems.py. Dev builds only (installFromEnvironment returns in releases).
        constexpr int kMaxFrames = 128;
        constexpr uint32_t kHostToken = 0x80000000u; // | index into g_hostNames: a host function, not a guest one
        constexpr uint32_t kSkipGuest = 0xFFFFFFFFu; // a host function the table holds for several guest functions
        constexpr uint32_t kIdleToken = 0xFFFFFFFEu; // first token of a sample stopped in a kernel wait
        constexpr uint8_t kUnwindChainInfo = 0x4;    // UNW_FLAG_CHAININFO

        struct ModuleRange
        {
            uintptr_t base = 0, end = 0;
            const RUNTIME_FUNCTION *table = nullptr;
            uint32_t count = 0;
        };
        std::vector<ModuleRange> g_modules;                   // by base; changed by the sampler thread, never mid-walk
        std::unordered_map<uintptr_t, uint32_t> g_guestByHost; // host function start -> guest function
        std::unordered_map<uintptr_t, uint32_t> g_tokenByStart;
        std::vector<std::string> g_hostNames;
        std::vector<bool> g_hostIsCode; // recompiled code the table doesn't hold (room modules): kept inside chains
        std::unordered_map<std::string, uint32_t> g_hostIndex;

        struct ChainHash
        {
            size_t operator()(const std::vector<uint32_t> &v) const
            {
                uint64_t h = 1469598103934665603ull;
                for (uint32_t x : v)
                    h = (h ^ x) * 1099511628211ull;
                return static_cast<size_t>(h);
            }
        };
        std::unordered_map<std::vector<uint32_t>, uint32_t, ChainHash> g_chains;
        uint32_t g_chainSamples = 0, g_chainIdle = 0, g_chainFailed = 0;

        void refreshModules()
        {
            HMODULE handles[512];
            DWORD needed = 0;
            if (!K32EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &needed))
                return;
            const DWORD n = std::min<DWORD>(needed / sizeof(HMODULE), 512u);
            std::vector<ModuleRange> modules;
            modules.reserve(n);
            for (DWORD i = 0; i < n; ++i)
            {
                const auto base = reinterpret_cast<uintptr_t>(handles[i]);
                const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
                if (dos->e_magic != IMAGE_DOS_SIGNATURE)
                    continue;
                const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + static_cast<uintptr_t>(dos->e_lfanew));
                if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
                    continue;
                const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
                ModuleRange m;
                m.base = base;
                m.end = base + nt->OptionalHeader.SizeOfImage;
                if (dir.VirtualAddress != 0u && dir.Size >= sizeof(RUNTIME_FUNCTION))
                {
                    m.table = reinterpret_cast<const RUNTIME_FUNCTION *>(base + dir.VirtualAddress);
                    m.count = dir.Size / sizeof(RUNTIME_FUNCTION);
                }
                modules.push_back(m);
            }
            std::sort(modules.begin(), modules.end(), [](const ModuleRange &a, const ModuleRange &b) { return a.base < b.base; });
            g_modules = std::move(modules);
        }

        const ModuleRange *moduleOf(uintptr_t pc)
        {
            auto it = std::upper_bound(g_modules.begin(), g_modules.end(), pc,
                                       [](uintptr_t value, const ModuleRange &m) { return value < m.base; });
            if (it == g_modules.begin())
                return nullptr;
            --it;
            return pc < it->end ? &*it : nullptr;
        }

        const RUNTIME_FUNCTION *entryOf(const ModuleRange &m, uintptr_t pc)
        {
            if (!m.table)
                return nullptr;
            const auto rva = static_cast<DWORD>(pc - m.base);
            uint32_t lo = 0, hi = m.count;
            while (lo < hi)
            {
                const uint32_t mid = lo + (hi - lo) / 2u;
                if (m.table[mid].EndAddress <= rva)
                    lo = mid + 1u;
                else if (m.table[mid].BeginAddress > rva)
                    hi = mid;
                else
                    return &m.table[mid];
            }
            return nullptr;
        }

        // The EE thread's frames, innermost first (frame 0 = where it stopped, the others return addresses). Runs with
        // the thread suspended: no allocation, no locks; the stack's bounds checked before every read.
        int walk(CONTEXT &c, uintptr_t *pcs)
        {
            const uintptr_t low = g_eeStackLow.load(std::memory_order_relaxed);
            const uintptr_t high = g_eeStackHigh.load(std::memory_order_relaxed);
            int n = 0;
            while (n < kMaxFrames && c.Rip != 0u)
            {
                const uintptr_t pc = c.Rip;
                const uintptr_t at = n == 0 ? pc : pc - 1u; // inside the call, not past it
                pcs[n++] = pc;
                const ModuleRange *m = moduleOf(at);
                if (!m || c.Rsp < low || c.Rsp + 8u > high)
                    break;
                if (const RUNTIME_FUNCTION *fe = entryOf(*m, at))
                {
                    void *handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, m->base, at, const_cast<PRUNTIME_FUNCTION>(fe), &c, &handlerData,
                                     &establisher, nullptr);
                }
                else
                {
                    // A leaf function without unwind data: the return address is on top of the stack.
                    c.Rip = *reinterpret_cast<const DWORD64 *>(c.Rsp);
                    c.Rsp += 8u;
                }
                if (c.Rsp < low || c.Rsp > high)
                    break;
            }
            return n;
        }

        int walkGuarded(CONTEXT &c, uintptr_t *pcs)
        {
#if defined(_MSC_VER)
            __try
            {
                return walk(c, pcs);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return -1; // a module unloaded under us, or a stack that didn't unwind: the sample is dropped
            }
#else
            return walk(c, pcs);
#endif
        }

        // Incremental linking puts a jmp rel32 thunk where a function's address is taken: the table may hold those.
        uintptr_t followThunk(uintptr_t p)
        {
            for (int i = 0; i < 2; ++i)
            {
                const auto *b = reinterpret_cast<const uint8_t *>(p);
                if (b[0] != 0xE9u)
                    break;
                int32_t rel = 0;
                std::memcpy(&rel, b + 1, 4);
                p = p + 5u + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
            }
            return p;
        }

        // The generated table (and our replacements in it) as host function start -> guest function. Rebuilt every
        // refresh: FunctionHooks::replace changes slots at run time.
        void buildGuestMap()
        {
            g_guestByHost.clear();
            g_tokenByStart.clear();
            const uint32_t count = g_ps2RecompiledFunctionTableSlotCount;
            for (uint32_t i = 0; i < count; ++i)
            {
                const PS2Runtime::RecompiledFunction fn = g_ps2RecompiledFunctionTable[i];
                if (!fn || (i > 0u && g_ps2RecompiledFunctionTable[i - 1u] == fn))
                    continue;
                const uintptr_t host = followThunk(reinterpret_cast<uintptr_t>(fn));
                const uint32_t guest = g_ps2RecompiledFunctionTableBase + i * 4u;
                auto [it, fresh] = g_guestByHost.emplace(host, guest);
                if (!fresh && it->second != guest)
                    it->second = kSkipGuest; // a shared fallback (stubs, module dispatch): no single guest function
            }
        }

        // The start of the function holding pc (its primary .pdata entry, chained fragments followed), or pc itself.
        uintptr_t functionStart(uintptr_t pc)
        {
            const ModuleRange *m = moduleOf(pc);
            if (!m)
                return pc;
            const RUNTIME_FUNCTION *fe = entryOf(*m, pc);
            if (!fe)
                return pc;
            for (int guard = 0; guard < 8; ++guard)
            {
                if (fe->UnwindData & 1u) // an indirect entry
                {
                    fe = reinterpret_cast<const RUNTIME_FUNCTION *>(m->base + (fe->UnwindData & ~1u));
                    continue;
                }
                const auto *info = reinterpret_cast<const uint8_t *>(m->base + fe->UnwindData);
                if (((info[0] >> 3) & kUnwindChainInfo) == 0u)
                    break;
                const uint32_t codes = info[2];
                fe = reinterpret_cast<const RUNTIME_FUNCTION *>(info + 4 + ((codes + 1u) & ~1u) * 2u);
            }
            return m->base + fe->BeginAddress;
        }

        bool isGeneratedName(const std::string &name)
        {
            // The recompiler names functions <name>_0x<guest address> (boot ELF and room modules alike).
            const size_t at = name.rfind("_0x");
            if (at == std::string::npos || at + 3u >= name.size())
                return false;
            for (size_t i = at + 3u; i < name.size(); ++i)
                if (!std::isxdigit(static_cast<unsigned char>(name[i])))
                    return false;
            return true;
        }

        uint32_t tokenOf(uintptr_t pc)
        {
            const uintptr_t start = functionStart(pc);
            if (auto it = g_tokenByStart.find(start); it != g_tokenByStart.end())
                return it->second;
            uint32_t token = kSkipGuest;
            if (auto guest = g_guestByHost.find(start); guest != g_guestByHost.end() && guest->second != kSkipGuest)
                token = guest->second;
            else
            {
                DWORD64 symbolStart = 0;
                std::string name = hostFunction(pc, symbolStart);
                for (char &ch : name)
                    if (ch == ' ' || ch == ';')
                        ch = '_';
                auto [it, fresh] = g_hostIndex.emplace(name, static_cast<uint32_t>(g_hostNames.size()));
                if (fresh)
                {
                    g_hostIsCode.push_back(isGeneratedName(name));
                    g_hostNames.push_back(std::move(name));
                }
                token = kHostToken | it->second;
            }
            g_tokenByStart.emplace(start, token);
            return token;
        }

        // One walked sample: guest functions (and recompiled module code) outermost first, the runtime's frames between
        // them left out, the innermost frame kept whatever it is (where the time went: a guest function's own code, a
        // runtime helper it called, a kernel wait).
        void countChain(const uintptr_t *pcs, int n, std::vector<uint32_t> &chain)
        {
            ++g_chainSamples;
            if (n <= 0)
            {
                ++g_chainFailed;
                return;
            }
            chain.clear();
            const uint32_t innermost = tokenOf(pcs[0]);
            const bool idle = (innermost & kHostToken) && innermost != kSkipGuest && isWait(g_hostNames[innermost & ~kHostToken]);
            if (idle)
            {
                ++g_chainIdle;
                chain.push_back(kIdleToken);
            }
            for (int i = n - 1; i >= 0; --i)
            {
                const uint32_t token = i == 0 ? innermost : tokenOf(pcs[i] - 1u);
                if (token == kSkipGuest)
                    continue;
                const bool keep = !(token & kHostToken) || g_hostIsCode[token & ~kHostToken] || i == 0;
                if (keep && (chain.empty() || chain.back() != token))
                    chain.push_back(token);
            }
            ++g_chains[chain];
        }

        std::string tokenName(uint32_t token)
        {
            if (token == kIdleToken)
                return "[idle]";
            if (token & kHostToken)
                return "~" + g_hostNames[token & ~kHostToken]; // ~ marks host code (runtime, libraries, the kernel)
            char text[16];
            std::snprintf(text, sizeof(text), "@%08X", token);
            const char *name = symbols::functionAt(token);
            return (name ? std::string(name) : std::string("func")) + text;
        }

        std::filesystem::path chainsPath()
        {
            if (const char *replay = std::getenv("KAMITE_REPLAY_CASE"); replay && *replay)
                return std::filesystem::path(std::string(replay) + (kamite::fps60() ? ".60" : ".30") + ".stacks.txt");
            wchar_t exe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            return std::filesystem::path(exe).parent_path() / "logs" / ("stacks_" + std::to_string(GetCurrentProcessId()) + ".txt");
        }

        // The whole run so far, replaced each time (a reader never sees half a file).
        void writeChains()
        {
            if (g_chains.empty())
                return;
            const std::filesystem::path path = chainsPath();
            std::filesystem::path temp = path;
            temp += ".tmp";
            std::FILE *f = std::fopen(temp.string().c_str(), "wb");
            if (!f)
                return;
            const char *replay = std::getenv("KAMITE_REPLAY_CASE");
            std::fprintf(f, "# GHR stacks v1 commit=%s rate=%d samples=%u idle=%u failed=%u case=%s\n", KAMITE_GIT_COMMIT,
                         kamite::fps60() ? 60 : 30, g_chainSamples, g_chainIdle, g_chainFailed, replay ? replay : "");
            std::string line;
            for (const auto &[chain, count] : g_chains)
            {
                line.clear();
                for (size_t i = 0; i < chain.size(); ++i)
                {
                    if (i)
                        line += ';';
                    line += tokenName(chain[i]);
                }
                std::fprintf(f, "%s %u\n", line.c_str(), count);
            }
            std::fclose(f);
            MoveFileExW(temp.wstring().c_str(), path.wstring().c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }
#endif

    void installFromEnvironment(PS2Runtime &runtime)
    {
        (void)runtime;
#if defined(KAMITE_RELEASE)
        return; // player releases: no profilers or tracers (claude/release.md)
#endif
        const char *value = std::getenv("KAMITE_PC_SAMPLE");
        const int seconds = value ? std::atoi(value) : 0;
        if (seconds <= 0)
            return;
        (void)runtime;
        const char *allValue = std::getenv("KAMITE_PC_SAMPLE_ALL");
        const bool all = allValue && *allValue && *allValue != '0';
        // The guest call chains go with the sampler (rung 1, claude/native-next.md); KAMITE_PC_STACKS=0 leaves them out.
        const char *stacksValue = std::getenv("KAMITE_PC_STACKS");
        const bool stacks = !stacksValue || *stacksValue != '0';
        std::thread([seconds, all, stacks]() {
#if defined(_WIN32)
            timeBeginPeriod(1);
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            SymInitialize(GetCurrentProcess(), nullptr, TRUE);
            std::unordered_map<DWORD64, uint32_t> hostRips;
            auto chainsAt = std::chrono::steady_clock::now();
            auto chainsWriteAt = chainsAt + std::chrono::seconds(1);
            std::vector<uint32_t> chain;
            chain.reserve(kMaxFrames + 1);
            uintptr_t framePcs[kMaxFrames];
#endif
            std::unordered_map<uint32_t, uint32_t> pcs;
            auto reportAt = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            uint32_t samples = 0, none = 0;
#if defined(_WIN32)
            auto refreshAt = std::chrono::steady_clock::now();
            static std::unordered_map<DWORD64, std::pair<DWORD64, std::string>> names; // rip -> function
            const auto nameOf = [](DWORD64 rip) -> const std::string & {
                auto it = names.find(rip);
                if (it == names.end())
                {
                    DWORD64 start = 0;
                    std::string name = hostFunction(rip, start);
                    it = names.emplace(rip, std::make_pair(start, std::move(name))).first;
                }
                return it->second.second;
            };
#endif
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
#if defined(_WIN32)
                if (all)
                {
                    if (std::chrono::steady_clock::now() >= refreshAt)
                    {
                        refreshThreads();
                        refreshAt = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                    }
                    if (samples % 4u == 0u)
                        for (ThreadSample &t : g_threads)
                        {
                            CONTEXT context{};
                            context.ContextFlags = CONTEXT_CONTROL;
                            DWORD64 rip = 0;
                            if (SuspendThread(t.handle) != static_cast<DWORD>(-1))
                            {
                                if (GetThreadContext(t.handle, &context))
                                    rip = context.Rip;
                                ResumeThread(t.handle);
                            }
                            if (rip)
                            {
                                ++t.samples;
                                ++t.rips[rip];
                            }
                        }
                }
#endif
                const R5900Context *ctx = g_mainContext.load(std::memory_order_relaxed);
                ++samples;
                if (ctx)
                {
                    const uint32_t pc = *static_cast<const volatile uint32_t *>(&ctx->pc);
                    ++pcs[pc];
                }
                else
                    ++none;
#if defined(_WIN32)
                // The EE thread's host instruction (and with stacks, its call chain): suspended only for the register
                // read and the walk (nothing allocated meanwhile: it may hold the heap lock).
                if (HANDLE thread = g_eeThread.load(std::memory_order_relaxed))
                {
                    if (stacks && std::chrono::steady_clock::now() >= chainsAt)
                    {
                        refreshModules();
                        buildGuestMap();
                        chainsAt = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                    }
                    alignas(16) CONTEXT context{};
                    context.ContextFlags = stacks ? (CONTEXT_CONTROL | CONTEXT_INTEGER) : CONTEXT_CONTROL;
                    DWORD64 rip = 0;
                    int frames = 0;
                    if (SuspendThread(thread) != static_cast<DWORD>(-1))
                    {
                        if (GetThreadContext(thread, &context))
                        {
                            rip = context.Rip;
                            if (stacks)
                                frames = walkGuarded(context, framePcs);
                        }
                        ResumeThread(thread);
                    }
                    if (rip)
                    {
                        ++hostRips[rip];
                        if (stacks)
                            countChain(framePcs, frames, chain);
                    }
                    if (stacks && std::chrono::steady_clock::now() >= chainsWriteAt)
                    {
                        writeChains();
                        chainsWriteAt = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    }
                }
#endif
                if (std::chrono::steady_clock::now() < reportAt)
                    continue;
                reportAt += std::chrono::seconds(seconds);
                std::unordered_map<uint32_t, uint32_t> functions;
                uint32_t topPc = 0, topPcCount = 0;
                for (const auto &[pc, count] : pcs)
                {
                    functions[owner(pc)] += count;
                    if (count > topPcCount)
                    {
                        topPc = pc;
                        topPcCount = count;
                    }
                }
                const auto top = sorted(functions);
                std::ostringstream line;
                line << "[pc-sample] " << samples << " samples of the main game thread (" << none << " before its first frame); hottest pc 0x" << std::hex
                     << topPc << std::dec << " (" << symbols::describe(topPc) << ") " << (100.0 * topPcCount / std::max(1u, samples)) << "%:";
                for (size_t i = 0; i < top.size() && i < 25u; ++i)
                    line << " " << symbols::describe(top[i].first) << "=" << (1000 * top[i].second / std::max(1u, samples)) / 10.0 << "%";
                KM_INFO(Game, line.str());
#if defined(_WIN32)
                if (all)
                {
                    std::ostringstream out;
                    out << "[pc-sample] all threads (busy = not in a kernel wait):";
                    for (ThreadSample &t : g_threads)
                    {
                        if (t.samples == 0u)
                            continue;
                        std::unordered_map<std::string, uint32_t> functions;
                        uint32_t busy = 0;
                        for (const auto &[rip, count] : t.rips)
                        {
                            const std::string &name = nameOf(rip);
                            functions[name] += count;
                            if (!isWait(name))
                                busy += count;
                        }
                        const auto top = sorted(functions);
                        out << "\n  thread " << t.id << (t.name.empty() ? std::string() : " \"" + t.name + "\"") << ": busy "
                            << (1000 * busy / std::max(1u, t.samples)) / 10.0 << "% of " << t.samples << " samples";
                        if (busy * 20u < t.samples) // mostly idle: the busy share is enough
                        {
                            t.rips.clear();
                            t.samples = t.busy = 0;
                            continue;
                        }
                        for (size_t i = 0; i < top.size() && i < 12u; ++i)
                            out << "\n      " << (1000 * top[i].second / std::max(1u, t.samples)) / 10.0 << "%  " << top[i].first;
                        t.rips.clear();
                        t.samples = t.busy = 0;
                    }
                    KM_INFO(Game, out.str());
                }
                if (!hostRips.empty())
                {
                    std::unordered_map<std::string, uint32_t> hostFunctions;
                    uint32_t hostSamples = 0;
                    for (const auto &[rip, count] : hostRips)
                    {
                        hostFunctions[nameOf(rip)] += count;
                        hostSamples += count;
                    }
                    const auto hostTop = sorted(hostFunctions);
                    std::ostringstream host;
                    host << "[pc-sample] EE host thread, " << hostSamples << " samples:";
                    for (size_t i = 0; i < hostTop.size() && i < 30u; ++i)
                        host << "\n    " << (1000 * hostTop[i].second / std::max(1u, hostSamples)) / 10.0 << "%  " << hostTop[i].first;
                    KM_INFO(Game, host.str());
                    hostRips.clear();
                }
#endif
                pcs.clear();
                samples = none = 0;
            }
        }).detach();
        KM_INFO(Game, "[pc-sample] sampling the game thread (guest PC and host code), a report every " << seconds << " s");
    }
}
