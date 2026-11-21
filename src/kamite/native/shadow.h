#pragma once

// The shadow harness (claude/native-next.md rung 2; dev builds only, off in KAMITE_RELEASE). A checked call: the
// native runs on a copy of the context and of its footprint's bytes in a scratch RAM, then the slot's previous function
// (the recompiled code, or a hook's trampoline in front of it) runs for real; the context, the footprint's bytes and
// the EE cycles charged are compared: bit for bit, or within the function's declared ulps. Nothing the native does
// reaches guest RAM, the live context or the scheduler. A call the original ended at a checkpoint (a resume pc) is
// compared against the native stopped at each of its checkpoints in turn. A mismatch's whole input and both results
// go to <output>.native.bin (16 per function per run); <output>.native.txt has the counts and the first mismatches'
// differing fields, rewritten every few seconds and at exit. A native with a recorded footprint (footprint = nullptr,
// rung 3) runs once on the scratch with each first touch filled from the real RAM: what it touched is the footprint
// (up to 256 ranges, its maxBytes or 64 KB; more is skipped), and an early exit's rerun starts from those bytes.

#include "kamite/native/native_function.h"

#include <cstdint>
#include <string>

namespace kamite::native::shadow
{
    using GuestFunction = PS2Runtime::RecompiledFunction;

    struct Settings
    {
        // Asked at the first checked call (the frame rate is known by then): <case>.<rate> during a replay, else
        // logs/native_<pid>; ".native.bin" / ".native.txt" are added.
        std::string (*output)() = nullptr;
        std::string (*header)() = nullptr; // the summary's first line (commit, rate, case)
        uint64_t (*cycles)() = nullptr; // EE cycles charged so far (the scheduler's count + the deferred cycles)
        uint32_t loopCycles = 32u;    // PS2X_EE_LOOP_CYCLES
        uint32_t savedPerFunction = 16u;
    };

    void begin(const Settings &settings);
    void add(const NativeFunction &function, GuestFunction held);
    void entry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime); // every shadowed slot's trampoline
    void writeSummary();

    struct Stats
    {
        uint64_t calls = 0;        // entries
        uint64_t checked = 0;      // compared (every Nth)
        uint64_t matched = 0;
        uint64_t declined = 0;     // the native declined (its entry checks): the original runs, nothing to compare
        uint64_t skipped = 0;      // not shadowable: a footprint outside plain RAM, too many ranges or bytes, nested
        uint64_t earlyExits = 0;   // matched with the original stopped at a checkpoint
        uint64_t mismatchCtx = 0, mismatchRam = 0, mismatchCycles = 0;
        uint64_t mismatches = 0;   // calls with any of the three
        uint32_t saved = 0;
        uint32_t worstUlps = 0;    // loose functions: the largest float difference accepted
    };
    const Stats *stats(uint32_t address); // tests
}
