// The main loop's wait for the next frame, without the spin.
//
// func_0030AC40 (the main loop, docs/decomp/findings.md) waits for the next frame in one of four spins (0x30AD98,
// 0x30ADD8, 0x30AE38, 0x30AE78): `while (counter < target) func_002B4BC0(&D_005E79D0);`. The counter is raised only by
// an interrupt handler: D_00753D80 by the vblank-start handler func_0030B148, D_00753D88 by the timer-0 handler at
// 0x30D190 (GameWork +0x578 == 1); the target is GameWork +0x569 (vblanks per game frame) or its copy at +0x570.
// func_002B4BC0 is the movie manager's poll (include/godhand/cMovie.h): it runs the movie server (func_003466A8 ->
// func_00325F20) only while a track plays (+0x14, set by func_002B4A50 each frame), and does nothing otherwise.
//
// So without a movie the spin can only end when an interrupt comes, and runs the guest code meanwhile for nothing: it
// was 15-20% of the guest's PC samples and ~1/10 of the game thread's busy time (claude/perf-enemies.md, Nov 20). The
// hook, after the poll returns and before the counter is read, lets the EE clock run to the next event instead
// (EeScheduler::skipIdleCycles: timers, the IOP and the host pacing go on as during the spin; lower-priority guest
// threads stay waiting, as during the spin). The loop's next checkpoint then takes the interrupt, the handler raises
// the counter, and the loop ends as before. While a movie plays the spin stays: the poll feeds the player every time
// round.
#include "kamite/frame_wait.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "runtime/ee_scheduler.h"

#include <cstdlib>
#include <cstring>

namespace kamite
{
    namespace
    {
        constexpr uint32_t kMovieManager = 0x5E79D0u;    // D_005E79D0 (cMovie)
        constexpr uint32_t kMoviePolls = 0x14u;          // byte: a track played this frame (func_002B4A50)
        constexpr uint32_t kVblankCount = 0x753D80u;     // raised by the vblank-start handler func_0030B148
        constexpr uint32_t kTimerCount = 0x753D88u;      // raised by the timer-0 handler at 0x30D190
        constexpr uint32_t kTargetWord = game::GameWork::address(0x570u);
        constexpr uint32_t kTargetByte = game::GameWork::address(game::GameWork::kVblankDivider);

        bool g_on = true; // KAMITE_FRAME_WAIT=0: the game's own spin

        uint32_t read32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, rdram + address, sizeof(value));
            return value;
        }

        template <uint32_t kCounter, bool kByteTarget>
        void frameWait(uint8_t *rdram, R5900Context *, PS2Runtime *runtime)
        {
            if (!g_on || rdram[kMovieManager + kMoviePolls] != 0u)
                return; // a movie plays: its server runs in the poll every time round
            const uint32_t target = kByteTarget ? rdram[kTargetByte] : read32(rdram, kTargetWord);
            if (read32(rdram, kCounter) >= target) // the loop's own test (sltu)
                return;
            runtime->eeScheduler().skipIdleCycles();
        }
    }

    void installFrameWait()
    {
        const char *value = std::getenv("KAMITE_FRAME_WAIT");
        g_on = !(value && *value == '0');
        ps2xRegisterHostHook("km_frame_wait_vblank_word", &frameWait<kVblankCount, false>); // 0x30ADA0
        ps2xRegisterHostHook("km_frame_wait_vblank", &frameWait<kVblankCount, true>);       // 0x30ADE0
        ps2xRegisterHostHook("km_frame_wait_timer_word", &frameWait<kTimerCount, false>);   // 0x30AE40
        ps2xRegisterHostHook("km_frame_wait_timer", &frameWait<kTimerCount, true>);         // 0x30AE80
        // The hooks' flag is frame_hook (always on, installFramePatches); they check g_on themselves.
        if (!g_on)
            KM_INFO(Boot, "[frame-wait] off (KAMITE_FRAME_WAIT=0): the main loop spins until the next frame");
    }
}
