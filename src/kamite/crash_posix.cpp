// Native crash reports on POSIX systems (crash_posix.h).
#include "kamite/crash_posix.h"

#if defined(_WIN32)
namespace kamite::crash_posix
{
    void install(bool, int, GuestRegisters) {}
}
#else
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>

#include <execinfo.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

namespace kamite::crash_posix
{
    namespace
    {
        bool g_hidden = false;
        int g_exitCode = 6;
        GuestRegisters g_guest = nullptr;
        std::atomic<bool> g_entered{false};

        // ---- async-signal-safe output: no stdio, no allocation ----
        void put(const char *text)
        {
            size_t n = std::strlen(text);
            while (n > 0)
            {
                const ssize_t wrote = ::write(STDERR_FILENO, text, n);
                if (wrote <= 0)
                    return;
                text += wrote;
                n -= static_cast<size_t>(wrote);
            }
        }

        void putHex(uint64_t value)
        {
            char text[19] = "0x";
            char digits[16];
            int n = 0;
            do
            {
                digits[n++] = "0123456789abcdef"[value & 0xFu];
                value >>= 4;
            } while (value != 0 && n < 16);
            for (int i = 0; i < n; ++i)
                text[2 + i] = digits[n - 1 - i];
            text[2 + n] = '\0';
            put(text);
        }

        void putDecimal(uint64_t value)
        {
            char text[21];
            int at = 20;
            text[at] = '\0';
            do
            {
                text[--at] = static_cast<char>('0' + value % 10);
                value /= 10;
            } while (value != 0 && at > 0);
            put(text + at);
        }

        uint64_t threadId() { return static_cast<uint64_t>(::syscall(SYS_gettid)); }

        const char *signalName(int signal)
        {
            switch (signal)
            {
            case SIGSEGV:
                return "SIGSEGV (bad memory access)";
            case SIGBUS:
                return "SIGBUS (bad memory access)";
            case SIGILL:
                return "SIGILL (illegal instruction)";
            case SIGFPE:
                return "SIGFPE (arithmetic error)";
            case SIGABRT:
                return "SIGABRT (abort)";
            default:
                return "signal";
            }
        }

        // "[crash]   #<i> <module>(<function>+<offset>) [<address>]", one line per frame: backtrace_symbols_fd
        // writes straight to the descriptor without allocating.
        void putStack(int skip)
        {
            void *frames[48];
            const int count = ::backtrace(frames, 48);
            for (int i = skip; i < count; ++i)
            {
                put("[crash]   #");
                putDecimal(static_cast<uint64_t>(i - skip));
                put(" ");
                ::backtrace_symbols_fd(&frames[i], 1, STDERR_FILENO);
            }
        }

        void putGuest()
        {
            uint32_t pc = 0, ra = 0, sp = 0;
            if (g_guest == nullptr || !g_guest(pc, ra, sp))
                return;
            put("[crash] guest: pc ");
            putHex(pc);
            put(" ra ");
            putHex(ra);
            put(" sp ");
            putHex(sp);
            put("\n");
        }

        void onSignal(int signal, siginfo_t *info, void *context)
        {
            if (g_entered.exchange(true))
                ::_exit(g_exitCode); // a second crash while reporting the first
            put("[crash] ");
            put(signalName(signal));
            put(" on thread ");
            putDecimal(threadId());
            if (signal != SIGABRT && info != nullptr)
            {
                put(" at address ");
                putHex(reinterpret_cast<uintptr_t>(info->si_addr));
            }
#if defined(__x86_64__) && defined(REG_RIP)
            if (context != nullptr && signal != SIGABRT)
            {
                const auto *uc = static_cast<const ucontext_t *>(context);
                const uint64_t ip = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
                put(", instruction ");
                putHex(ip);
                put("\n");
                // A call through a null (or wild) function pointer: the unwinder cannot leave address 0, but the
                // call left its return address on top of the stack.
                const auto *sp = reinterpret_cast<void *const *>(uc->uc_mcontext.gregs[REG_RSP]);
                if (ip < 4096 && sp != nullptr)
                {
                    put("[crash] called from ");
                    ::backtrace_symbols_fd(sp, 1, STDERR_FILENO);
                }
            }
            else
                put("\n");
#else
            (void)context;
            put("\n");
#endif
            putGuest();
            putStack(1);
            // As on Windows: an abort, or any crash in a hidden run, ends with the crash exit code; a normal run's
            // fault goes on to the default action (a core dump, the desktop's crash reporter).
            if (g_hidden || signal == SIGABRT)
                ::_exit(g_exitCode);
            struct sigaction fallback{};
            fallback.sa_handler = SIG_DFL;
            ::sigemptyset(&fallback.sa_mask);
            ::sigaction(signal, &fallback, nullptr);
            ::raise(signal);
        }

        [[noreturn]] void onTerminate()
        {
            std::fprintf(stderr, "[crash] std::terminate on thread %llu\n", static_cast<unsigned long long>(threadId()));
            if (const std::exception_ptr ep = std::current_exception())
            {
                try
                {
                    std::rethrow_exception(ep);
                }
                catch (const std::exception &e)
                {
                    std::fprintf(stderr, "[crash] exception: %s\n", e.what());
                }
                catch (...)
                {
                    std::fprintf(stderr, "[crash] non-std exception\n");
                }
            }
            std::fflush(stderr);
            putStack(1);
            std::fflush(stdout);
            ::_exit(g_exitCode);
        }
    }

    void install(bool hiddenRun, int crashExitCode, GuestRegisters guest)
    {
        g_hidden = hiddenRun;
        g_exitCode = crashExitCode;
        g_guest = guest;
        // backtrace() loads libgcc's unwinder on its first call (that allocates): done here, not in the handler.
        void *prime[2];
        ::backtrace(prime, 2);
        // The main thread's alternate stack: a stack overflow there still gets its report. Never freed (the handler
        // can run until the process ends).
        static char altStack[64 * 1024];
        stack_t stack{};
        stack.ss_sp = altStack;
        stack.ss_size = sizeof(altStack);
        ::sigaltstack(&stack, nullptr);
        struct sigaction action{};
        action.sa_sigaction = &onSignal;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        ::sigemptyset(&action.sa_mask);
        for (const int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
            ::sigaction(signal, &action, nullptr);
        std::set_terminate(&onTerminate);
    }
}
#endif
