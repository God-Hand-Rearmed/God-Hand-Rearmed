#pragma once

// A native EE function: hand-written C++ that replaces one recompiled guest function (claude/native-next.md, rung 0
// rules; rung 2's shadow harness checks it). Each one is described by a NativeFunction and registered by its
// subsystem's file (libvu0.cpp ...); native.cpp installs it, shadow.cpp checks it against the recompiled code.
//
// The rules a native's run() follows (claude/gt3-revisited.md "Rules for native replacements"):
//   - It works on the guest RAM through env.ram (the real RAM, or the shadow harness's scratch copy of the footprint)
//     and on ctx, nothing else: no globals, no runtime calls, no host state.
//   - It returns false, having changed nothing, when it declines a call (huge or special values, a pointer outside
//     plain RAM: the entry checks); the slot's previous function then runs. It returns true when it ran.
//   - EE time is charged where the original charges it: env.checkpoint(32) at each taken backward branch, env.call()
//     at each guest call (8 cycles; ctx->pc set to the callee first). When either returns true (a checkpoint came due)
//     it leaves ctx and RAM exactly as the original has them at that point and returns true: the original's resume
//     entry carries on later.
//   - It leaves ctx as the original does, scratch registers included (the harness compares the whole context), unless
//     its NativeFunction lists a don't-care field with a reason.
//   - Floats run under the game thread's MXCSR (chop + DAZ / FTZ); maths through vu_vec.h only.

#include "ps2_runtime.h"
#include "runtime/ps2_address.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kamite::native
{
    // The guest bytes a call reads or writes, as the function addresses them (before the RAM mask). The harness
    // copies them into its scratch RAM before the native runs and compares them after the original ran.
    struct Footprint
    {
        static constexpr int kMaxRanges = 16;
        struct Range
        {
            uint32_t address;
            uint32_t size;
        };
        Range ranges[kMaxRanges];
        int count = 0;
        bool overflow = false; // more than kMaxRanges: the call is not checked
        void add(uint32_t address, uint32_t size)
        {
            if (count < kMaxRanges)
                ranges[count++] = Range{address, size};
            else
                overflow = true;
        }
    };

    // A recorded footprint (a NativeFunction with footprint == nullptr; rung 3, K's pick): the harness runs the native
    // on its scratch with every first touch of a byte filled from the real RAM and logged; the log is the call's
    // footprint. Ranges are RAM offsets (address & PS2_RAM_MASK). A rerun (an early exit, or shadow_test rerun) starts
    // from the saved bytes: a range it hadn't touched before is filled with zeros and marks it strayed.
    struct Recording
    {
        std::vector<Footprint::Range> ranges;
        std::vector<uint8_t> in;        // each range's bytes as filled (the call's input), in range order
        const uint8_t *real = nullptr;  // the RAM to fill from; nullptr on a rerun
        uint32_t maxBytes = 0;
        uint32_t bytes = 0;
        bool overflow = false;          // more than kMaxRecordedRanges ranges or maxBytes bytes: not checked
        bool strayed = false;           // a rerun touched a byte the first run hadn't
        static constexpr size_t kMaxRecordedRanges = 256;
        void reset(const uint8_t *from, uint32_t limit)
        {
            ranges.clear();
            in.clear();
            real = from;
            maxBytes = limit;
            bytes = 0;
            overflow = strayed = false;
        }
        // recording.cpp: fill [offset, offset + size) on first touch; mark / unmark the ranges' bytes as present (a
        // rerun marks the saved ranges first; the harness unmarks them after every run).
        void touch(uint8_t *scratch, uint32_t offset, uint32_t size);
        void mark() const;
        void unmark() const;
    };

    // Where a native's memory accesses go: the real RAM (RealEnv) or the shadow scratch (ShadowEnv). Both are the
    // runtime's 128 MB backing: a guest address in plain RAM is ram + (address & PS2_RAM_MASK). New natives reach RAM
    // through env.at(offset, size) (the bytes of one access, offset from plainRam): the real RAM's pointer, or the
    // scratch's with a recorded footprint filled on first touch.
    struct RealEnv
    {
        uint8_t *ram;
        PS2Runtime *runtime;
        bool checkpoint(uint32_t cycles) { return runtime->eeCheckpointDue(cycles); }
        bool call() { return runtime->eeCheckpointDue(8u); } // EeScheduler::kGuestDispatchCycles
        uint8_t *at(uint32_t offset, uint32_t) { return ram + offset; }
    };

    struct ShadowEnv
    {
        uint8_t *ram;
        int dueAt = 0;          // the checkpoint (1-based) that comes due in this run; 0 = none
        int checkpoints = 0;    // checkpoints charged so far
        uint64_t charged = 0;   // their EE cycles
        uint32_t loopCycles = 32u; // what a backward branch's 32 stands for (PS2X_EE_LOOP_CYCLES)
        Recording *record = nullptr; // a recorded footprint: fill on first touch
        uint8_t *at(uint32_t offset, uint32_t size)
        {
            if (record)
                record->touch(ram, offset, size);
            return ram + offset;
        }
        bool due()
        {
            ++checkpoints;
            return checkpoints == dueAt;
        }
        bool checkpoint(uint32_t cycles)
        {
            charged += cycles == 32u ? loopCycles : cycles;
            return due();
        }
        bool call()
        {
            charged += 8u;
            return due();
        }
    };

    // A context field the comparison leaves out (with the reason, which the summary prints).
    struct DontCare
    {
        uint32_t offset;
        uint32_t size;
        const char *reason;
    };

    struct NativeFunction
    {
        const char *name;      // the recompiled function's name (kamite_functions.csv)
        uint32_t address;      // its entry
        const char *subsystem; // its switch: KAMITE_NATIVE_<SUBSYSTEM> (unset = checked ones, 1 = all, 0 = none)
        bool checked;          // on by default (0 harness mismatches over all cases, replays identical, faster)
        bool (*runReal)(RealEnv &env, R5900Context *ctx);
        bool (*runShadow)(ShadowEnv &env, R5900Context *ctx);
        void (*footprint)(const R5900Context *ctx, Footprint &fp); // nullptr: recorded (Recording; env.at only)
        uint32_t ulps = 0;     // 0 = bit-exact; else a loose function: float words may differ by this many ulps
        uint32_t every = 1;    // the shadow harness checks every Nth call
        const DontCare *dontCare = nullptr;
        int dontCareCount = 0;
        uint32_t maxBytes = 0; // a recorded footprint's limit (0 = 64 KB); more is skipped
    };

    // Called by each subsystem's register function before install().
    void registerFunction(const NativeFunction &function);

    // The guest address -> offset of a plain-RAM access of `size` bytes, or false for an address the native must decline
    // (scratchpad, I/O, VU memory, BIOS, KSEG2/3, or a range that wraps the backing): what READn / WRITEn take the
    // runtime's slow path for.
    inline bool plainRam(uint32_t address, uint32_t size, uint32_t &offset)
    {
        if (Ps2IsSpecialAddress(address) || Ps2IsSpecialAddress(address + size - 1u))
            return false;
        offset = address & PS2_RAM_MASK;
        return offset + size <= PS2_RAM_MASK + 1u;
    }
}
