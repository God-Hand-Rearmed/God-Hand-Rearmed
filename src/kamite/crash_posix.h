#pragma once

// Native crashes on Linux (and other POSIX systems): the counterpart of main.cpp's Windows crash filter. A fault
// (SIGSEGV, SIGBUS, SIGILL, SIGFPE) or an abort (SIGABRT) writes "[crash]" lines to stderr (the console, or
// KAMITE_LOG_FILE once main.cpp has redirected it there): the signal, the fault address, the instruction pointer,
// the guest's registers when known, and the host stack (backtrace(), function names where the binary exports them).
// The handler runs on an alternate signal stack, so a stack overflow on the main thread is still reported, and makes
// only async-signal-safe calls (write, backtrace after it is primed, _exit).
//
// Afterwards, as on Windows: a hidden run (KAMITE_HEADLESS=1) and every abort exit with `crashExitCode` (main.cpp's
// 6); a normal run's fault goes on to the system's default handling (the signal is raised again with the default
// action, so a core dump or the desktop's crash reporter still sees it).
#include <cstdint>

namespace kamite::crash_posix
{
    // The guest's registers for the report (main.cpp reads the EE's current context); false when there is none.
    using GuestRegisters = bool (*)(uint32_t &pc, uint32_t &ra, uint32_t &sp);

    // Once, early in main (after the log redirect). No-op on Windows.
    void install(bool hiddenRun, int crashExitCode, GuestRegisters guest);
}
