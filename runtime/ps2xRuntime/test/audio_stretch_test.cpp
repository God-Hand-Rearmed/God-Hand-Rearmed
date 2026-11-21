// When the audio goes through the time-stretch (runtime/audio_stretch_control.h, ps2_audio.cpp), judged by what the
// player hears on a simulated PC: gaps in the sound per minute.
//
// Issue #5: on a PC that runs 60 FPS mode a little below full speed (a 4-core Haswell: frames of 16-18 ms against a
// 16.68 ms field), each late vblank re-anchors and that real time is emulated time, and sound, lost for good
// (EeScheduler.cpp). The rate control absorbs 0.5%; below that the queue runs dry and the music cuts out again and
// again. At 30 FPS every second field is idle and soaks up the lateness: no cuts. GHR shipped with the stretch off,
// and the stretch only switched on below 97% of an estimate that moved in ~2% steps.
//
// This test simulates the scheduler, the sound queue (ps2_audio.cpp) and the stretch's tempo, and checks:
//   - issue #5 as reported, with the stretch off: 60 FPS cuts out at 16.5-18 ms frames; 30 FPS does not (up to 16 ms
//     frames) or far less;
//   - with StretchControl: no more than one cut a minute up to 18 ms frames, a few at 20 ms;
//   - a PC that keeps up (10-13 ms frames), and stalls (a 150 ms hang every 20 s): the stretch never switches on
//     (it would play attacks twice for nothing);
//   - back at full speed, the stretch switches off again.
// Exit code 0 = all passed. ctest runs it (KAMITE_BUILD_TESTS=ON).
#include "runtime/audio_stretch_control.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

// The real one is the scheduler's counter (EeScheduler.cpp); this test feeds the control its own.
int64_t ps2xEmulatedTimeLostNs() noexcept { return 0; }

namespace
{
    using ps2x::StretchControl;
    constexpr int64_t kFieldNs = 16683400;
    constexpr int64_t kMs = 1000000;
    constexpr double kRate = 48000.0;

    int g_failures = 0, g_checks = 0;
    void check(bool ok, const std::string &what)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failures;
            std::printf("FAIL %s\n", what.c_str());
        }
    }

    // ps2_audio.cpp's queue in stereo frames: rate control +-0.5% around 4096, an empty queue a gap until 2048 are back.
    struct SoundQueue
    {
        static constexpr double kRoom = 16384.0 - 512.0, kTarget = 4096.0, kPrime = 2048.0;
        double queued = kPrime;
        bool priming = false;
        int gaps = 0, overflows = 0;
        double fill() const { return (queued - kTarget) / kTarget; }
        void produce(double frames)
        {
            queued += frames;
            if (queued > kRoom)
            {
                ++overflows;
                queued = kPrime;
            }
            if (priming && queued >= kPrime)
                priming = false;
        }
        void play(int64_t ns, bool counting)
        {
            if (priming)
                return;
            const double ratio = 1.0 + std::clamp(fill() * 0.005, -0.005, 0.005);
            queued -= static_cast<double>(ns) * 1e-9 * kRate * ratio;
            if (queued < 0.0)
            {
                gaps += counting ? 1 : 0;
                queued = 0.0;
                priming = true;
            }
        }
    };

    struct Pc
    {
        double frameMs = 12.0; // the mean cost of one game frame on the EE thread
        bool fps60 = true;     // 60 FPS: a frame every field; 30 FPS: every second field (the other nearly idle)
        double stallEveryS = 0.0, stallMs = 0.0;
    };

    struct Result
    {
        double gapsPerMinute = 0.0;
        double speed = 0.0;          // emulated time / real time
        double stretchedShare = 0.0; // the share of fields played through the stretch
        bool stretchedAtEnd = false;
        int overflows = 0;
    };

    // seconds of play; stretchOn = false is the shipped behaviour (PS2X_AUDIO_STRETCH=0).
    Result run(const Pc &pc, double seconds, bool stretchOn, uint32_t seed = 7, double fastAfterS = -1.0)
    {
        std::mt19937 random(seed);
        // Frame cost: lognormal around the mean (10% spread), and 1% of frames 8 ms longer (a heavier scene moment).
        const double sigma = std::sqrt(std::log(1.0 + 0.1 * 0.1));
        std::lognormal_distribution<double> cost(std::log(pc.frameMs) - sigma * sigma / 2.0, sigma);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        StretchControl control;
        SoundQueue sound;
        const int64_t start = 1000 * kMs;
        int64_t deadline = start, workDone = start, previousFire = start, lost = 0, emulated = 0;
        const int fields = static_cast<int>(seconds * 1e9 / static_cast<double>(kFieldNs));
        const int stallEvery = pc.stallEveryS > 0.0 ? static_cast<int>(pc.stallEveryS * 1e9 / static_cast<double>(kFieldNs)) : 0;
        int stretchedFields = 0;
        double held = 0.0; // the stretcher's held input (frames)
        bool wasStretching = false;
        for (int field = 0; field < fields; ++field)
        {
            const double elapsedS = static_cast<double>(previousFire - start) * 1e-9;
            const bool fast = fastAfterS >= 0.0 && elapsedS >= fastAfterS;
            // The vblank fires once its deadline is reached and the EE thread has finished the last field's work.
            const int64_t fire = std::max(deadline, workDone);
            const bool counting = elapsedS >= 5.0; // the first seconds: the queue priming, the control's first windows
            sound.play(fire - previousFire, counting);
            previousFire = fire;
            // EeScheduler.cpp: a late vblank re-anchors; past 2 ms of catch-up the real time is lost emulated time.
            const int64_t hostStart = std::max(deadline, fire - 2 * kMs);
            const int64_t drop = hostStart - deadline;
            if (drop <= 50 * kMs) // ps2xEmulatedTimeLostNs leaves single stalls out
                lost += drop;
            deadline = hostStart + kFieldNs;
            emulated += kFieldNs;
            // This field's sound, through the stretch when it is on. ps2_audio.cpp's TimeStretch (WSOLA) holds its
            // input until it has a piece and the splice search's room (1920 + 720 frames), then makes 1536 frames per
            // piece from tempo x 1536 input frames (tempo = speed nudged by the playable queue's fill, 0.5-1.05). When
            // switched on it takes the queue's last frames back as its first input (up to 2640, all but 64: no 55 ms
            // hole), and its held input is passed through when switched off.
            const double frames = kRate * static_cast<double>(kFieldNs) * 1e-9;
            const bool stretching = stretchOn && control.update(fire, lost, sound.fill());
            stretchedFields += stretching ? 1 : 0;
            if (stretching && !wasStretching)
            {
                held = std::min(std::max(sound.queued - 64.0, 0.0), 2640.0);
                sound.queued -= held;
            }
            if (!stretching && wasStretching)
            {
                sound.produce(held);
                held = 0.0;
            }
            wasStretching = stretching;
            if (stretching)
            {
                held += frames;
                const double tempo = std::clamp(control.speed() * (1.0 + std::clamp(0.2 * sound.fill(), -0.1, 0.1)), 0.5, 1.05);
                while (held >= 2640.0)
                {
                    held -= tempo * 1536.0;
                    sound.produce(1536.0);
                }
            }
            else
                sound.produce(frames);
            // The work of this field (60 FPS: a whole frame; 30 FPS: a frame every second field, ~1 ms otherwise).
            double workMs = (pc.fps60 || field % 2 == 0) ? cost(random) : 1.0;
            if (unit(random) < 0.01)
                workMs += 8.0;
            if (fast)
                workMs = std::min(workMs, 10.0);
            if (stallEvery > 0 && field > 0 && field % stallEvery == 0)
                workMs += pc.stallMs;
            workDone = fire + static_cast<int64_t>(workMs * kMs);
        }
        Result r;
        const double countedMinutes = (seconds - 5.0) / 60.0;
        r.gapsPerMinute = static_cast<double>(sound.gaps) / countedMinutes;
        r.speed = static_cast<double>(emulated) / static_cast<double>(previousFire - start);
        r.stretchedShare = static_cast<double>(stretchedFields) / static_cast<double>(fields);
        r.stretchedAtEnd = control.stretching();
        r.overflows = sound.overflows;
        return r;
    }

    std::string pcName(const Pc &pc)
    {
        char text[96];
        std::snprintf(text, sizeof(text), "%s, %.1f ms frames", pc.fps60 ? "60 FPS" : "30 FPS", pc.frameMs);
        return text;
    }
}

int main()
{
    std::printf("issue #5 as reported (stretch off, as 0.6.9.1 shipped) and with the stretch control (2 min each):\n");
    for (double ms : {10.0, 13.0, 15.0, 16.0, 16.5, 17.0, 17.5, 18.0, 20.0})
    {
        Pc pc;
        pc.frameMs = ms;
        const Result off60 = run(pc, 125.0, false);
        const Result on60 = run(pc, 125.0, true);
        Pc pc30 = pc;
        pc30.fps60 = false;
        const Result off30 = run(pc30, 125.0, false);
        std::printf("  %5.1f ms frames: speed %.3f; 60 FPS: %5.1f cuts/min stretch off, %4.1f with the control (stretched %3.0f%%); "
                    "30 FPS: %4.1f cuts/min\n",
                    ms, off60.speed, off60.gapsPerMinute, on60.gapsPerMinute, on60.stretchedShare * 100.0, off30.gapsPerMinute);
        if (ms <= 13.0)
        {
            // A PC that keeps up: nothing to fix, and the stretch must stay out of it.
            check(off60.gapsPerMinute == 0.0 && on60.gapsPerMinute == 0.0, pcName(pc) + ": no cuts");
            check(on60.stretchedShare == 0.0, pcName(pc) + ": the stretch never switches on");
        }
        if (ms >= 16.5 && ms <= 18.0)
        {
            check(off60.gapsPerMinute >= 5.0, pcName(pc) + ", stretch off: cuts out (issue #5 reproduced: " +
                                                   std::to_string(off60.gapsPerMinute) + "/min)");
            check(on60.gapsPerMinute <= 1.0, pcName(pc) + ": at most one cut a minute with the control (" +
                                                 std::to_string(on60.gapsPerMinute) + "/min)");
        }
        // Never worse than the stretch off, at any speed (its switch-on costs a 55 ms hole: at the edge of full speed
        // it must not switch on and off and cut more than the rate control alone would).
        check(on60.gapsPerMinute <= off60.gapsPerMinute, pcName(pc) + ": no more cuts with the control than without (" +
                                                             std::to_string(on60.gapsPerMinute) + " vs " +
                                                             std::to_string(off60.gapsPerMinute) + "/min)");
        // "Works fine at 30 FPS": every second field is idle and soaks up the lateness.
        if (ms <= 16.0)
            check(off30.gapsPerMinute == 0.0, pcName(pc30) + ", stretch off: no cuts, as reported");
        else if (ms <= 18.0)
            check(off30.gapsPerMinute * 4.0 <= off60.gapsPerMinute, pcName(pc30) + ", stretch off: far fewer cuts than at 60 FPS");
        if (ms == 20.0)
            check(on60.gapsPerMinute <= 5.0, pcName(pc) + ": a few cuts at most with the control (" +
                                                 std::to_string(on60.gapsPerMinute) + "/min)");
        check(on60.overflows == 0, pcName(pc) + ": the stretch never overfills the queue");
    }

    std::printf("stalls on a PC that keeps up (a 150 ms hang every 20 s):\n");
    {
        Pc pc;
        pc.frameMs = 12.0;
        pc.stallEveryS = 20.0;
        pc.stallMs = 150.0;
        const Result r = run(pc, 125.0, true);
        std::printf("  stretched %.1f%% of the time, %.1f cuts/min (the hangs themselves)\n", r.stretchedShare * 100.0, r.gapsPerMinute);
        check(r.stretchedShare == 0.0, "stalls: the stretch never switches on");
    }

    std::printf("slow, then back at full speed:\n");
    {
        Pc pc;
        pc.frameMs = 18.0;
        const Result r = run(pc, 125.0, true, 7, 60.0);
        std::printf("  stretched %.0f%% of the time, on at the end: %s\n", r.stretchedShare * 100.0, r.stretchedAtEnd ? "yes" : "no");
        check(r.stretchedShare > 0.2 && !r.stretchedAtEnd, "slow then full speed: the stretch switches on, then off again");
    }

    std::printf("the control's rules, window by window:\n");
    {
        constexpr int64_t W = StretchControl::kWindowNs;
        // Feeds whole windows of a given speed; returns whether it is stretching after them.
        struct Feeder
        {
            StretchControl c;
            int64_t now = 1000 * kMs, lost = 0;
            bool windows(int n, double speed, double fill)
            {
                if (now == 1000 * kMs)
                    c.update(now, lost, fill);
                bool on = c.stretching();
                for (int i = 0; i < n; ++i)
                {
                    now += W;
                    lost += static_cast<int64_t>((1.0 - speed) * static_cast<double>(W));
                    on = c.update(now, lost, fill);
                }
                return on;
            }
        };
        {
            Feeder f;
            check(!f.windows(1, 0.980, -0.5), "one slow window (98%): not yet on");
            check(f.windows(1, 0.980, -0.5), "two slow windows in a row at 98%, the queue short: on (the last 5 s average 99.2%)");
            Feeder mild;
            check(!mild.windows(2, 0.990, -0.5), "99% twice: not yet (the last 5 s average 99.6%)");
            check(mild.windows(1, 0.990, -0.5), "99% a third time: on (99.4%)");
            Feeder full;
            check(!full.windows(4, 0.990, 0.2), "slow but the queue at or over its target (the rate control keeping up): not on");
        }
        {
            Feeder f;
            check(!f.windows(4, 0.9955, -0.5), "99.55%: never on (the threshold is 99.5%)");
            check(!f.windows(2, 0.9945, -0.5), "then 99.45% twice: not yet (the last 5 s average 99.51%)");
            check(f.windows(1, 0.9945, -0.5), "99.45% a third time: on (the last 5 s average 99.49%)");
        }
        {
            Feeder f;
            f.windows(2, 0.98, -0.5);
            check(f.windows(4, 0.997, 0.0), "on, then 99.7%: stays on (hysteresis up to 99.8%)");
            check(f.windows(3, 0.9985, 0.0), "on, then 99.85% for 3 s: still on (the last 5 s average 99.79%)");
            check(!f.windows(1, 0.9985, 0.0), "99.85% for a 4th s: off (the last 5 s average 99.82%)");
        }
        {
            // The numbers themselves (not the header's constants, which a change would move with them): off after
            // exactly 5 s at full speed with the queue at its target.
            Feeder f;
            f.windows(2, 0.98, -0.5);
            check(f.windows(4, 1.0, 0.0), "on, then 4 s at full speed: still on");
            check(!f.windows(1, 1.0, 0.0), "on, then 5 s at full speed: off");
        }
        {
            // Losses in bursts (issue #5's critic: 2% of fields late by up to 6 ms, 99.84% on the whole, which the rate
            // control keeps up with): two windows at 99.4% now and then, the rest at full speed. Never on (the last 5 s
            // average 99.76%); and a stretch that was on (a real slowdown before) goes off at 99.84% on the whole.
            Feeder f;
            bool ever = false;
            for (int i = 0; i < 24; ++i)
                ever = f.windows(2, 0.994, -0.5) || ever, ever = f.windows(3, 1.0, -0.5) || ever;
            check(!ever, "losses in bursts, 99.76% on average: never on (2 min)");
            Feeder g;
            check(g.windows(3, 0.97, -0.5), "a real slowdown (97%): on");
            for (int i = 0; i < 3; ++i)
                g.windows(1, 0.992, 0.0), g.windows(4, 1.0, 0.0);
            check(!g.c.stretching(), "then losses in bursts at 99.84% (one window at 99.2% in five): off again");
        }
        {
            // A stall restarts the window: the time it lost is not a slow window. Slow (99%) after it: the first update
            // starts a new window, so one slow window is all there is after 2 updates; not on yet.
            Feeder f;
            f.windows(1, 1.0, -0.5);
            f.now += 300 * kMs;
            f.lost += 300 * kMs;
            f.c.restartWindow();
            check(!f.windows(2, 0.99, -0.5), "a stall, then slow: the stall's loss not counted as a slow window (not on after one)");
            check(f.windows(2, 0.99, -0.5), "and on after the third slow window (the last 5 s average 99.4%)");
            // stalls every few seconds after a slowdown: the stretch still goes off once the game is back at full speed
            Feeder h;
            h.windows(3, 0.97, -0.5);
            for (int i = 0; i < 6; ++i)
            {
                h.windows(2, 1.0, 0.0);
                h.now += 300 * kMs;
                h.lost += 300 * kMs;
                h.c.restartWindow();
            }
            check(!h.c.stretching(), "on, then full speed with a stall every 3 s: off (the stalls do not clear the average)");
            // and a slow streak does not run across a stall: 97%, a stall, 97% is one window, not two
            Feeder g;
            g.windows(1, 0.97, -0.5);
            g.now += 300 * kMs;
            g.lost += 300 * kMs;
            g.c.restartWindow();
            check(!g.windows(2, 0.97, -0.5), "97%, a stall, 97%: not on (the stall breaks the streak)");
        }
        {
            Feeder f;
            f.windows(2, 0.98, -0.5);
            check(f.windows(StretchControl::kAverageWindows + 3, 1.0, 0.5), "back at full speed with the queue 50% over target: stays on until it drains");
            check(f.windows(StretchControl::kAverageWindows + 3, 1.0, 0.3), "and at 30% over target: still on (within 25% to leave)");
            check(!f.windows(StretchControl::kAverageWindows, 1.0, 0.1), "and off once the queue is back near its target");
            Feeder g;
            g.windows(2, 0.98, -0.5);
            check(g.windows(StretchControl::kAverageWindows + 3, 1.0, -0.5), "back at full speed with the queue 50% short: stays on");
        }
        {
            Feeder f;
            f.windows(2, 0.98, -0.5);
            bool on = true;
            for (int i = 0; i < 6; ++i)
                on = f.windows(1, i % 2 ? 1.0 : 0.98, -0.5); // fast, slow, fast, ...: never two fast in a row
            check(on, "alternating slow and fast windows: never two fast in a row, stays on");
        }
        {
            Feeder f;
            f.windows(1, 0.98, -0.5);
            f.c.restartWindow(); // a stall: the next update starts a fresh window
            f.c.update(f.now, f.lost, -0.5);
            check(!f.windows(1, 0.98, -0.5), "slow, a stall, slow: the stall breaks the streak, not on");
            check(f.windows(1, 0.98, -0.5), "then a second slow window after the stall: on");
        }
        {
            Feeder f;
            f.windows(2, -1.0, 0.0); // more time lost than passed (a counter jump)
            check(f.c.speed() >= 0.0, "a window losing more than its length: speed clamped at 0, not negative");
        }
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
