// The emulated vblank's lock to the host display (runtime/vsync_lock.h, EeScheduler.cpp lockToHostVsync) on simulated
// displays, judged by what the player hears: the sound queue (ps2_audio.cpp) must never overflow (a skip forward) or
// run dry (a gap).
//
// Issue #1: on a variable-refresh display (G-Sync / FreeSync with VSync on) the display's vblank follows the game's
// own presents; the lock could never close its phase error and moved the game's clock up to 1.5%, past the 0.5% the
// sound's rate control absorbs, and the music skipped about every 15 s. This test runs the lock against:
//   - fixed displays (60, 59.94, 120, 119.88, 240 Hz) with measurement jitter, stalls, hitches, a PC below full speed,
//     sub-ms losses, measurement gaps, re-anchors of every size from 3 to 48 ms, and long hangs (a loading screen):
//     it must stay locked (never let go more than the release-only lock), the game must run at the display's rate, presents
//     must stay clear of the vblank (no closer than the release-only lock's on average), and the sound must never skip;
//   - fixed displays the lock cannot follow without moving the game more than the sound absorbs (59, 60.5, 61 Hz):
//     released, the sound must never skip;
//   - VRR displays (scanout = present + a delay) for every delay from 0 to 16 ms in 0.1 ms steps, alone, with long
//     stalls and with frequent hitches: the game must stay within 0.25% of 59.94 Hz and the sound must never skip
//     (with hitches: the lock adds no skips over running without it);
//   - the old lock (steps of up to 1.5% for as long as it is locked, Mode::UncappedNoRelease): the sound must skip on VRR
//     displays (the bug, reproduced: this proves the test sees it);
//   - displays it must leave alone (144 Hz fixed: no multiple of 59.94 Hz; no measurement), a release kept through a
//     measurement gap and a drifting measured refresh, and a monitor switch after a release (another multiple:
//     measured afresh, locks again);
//   - slow PCs (fields late at random, vblanks seen late), noisy measurements (jitter to +-1 ms, late vblanks), fixed
//     displays either side of 59.94 Hz (held faster up to 0.4%, slower up to 0.2%), uneven VRR frame times, monitor
//     switches;
//   - Mode::Checked's rules one by one on synthetic phase errors: the fast release, too far, the retry, the gaps, the
//     wrap, the window.
// Exit code 0 = all passed. Plain program, no framework: ctest runs it (KAMITE_BUILD_TESTS=ON).
#include "runtime/vsync_lock.h"
#include "runtime/audio_stretch_control.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <string>

// The scheduler's counter (EeScheduler.cpp); this test keeps its own.
int64_t ps2xEmulatedTimeLostNs() noexcept { return 0; }

namespace
{
    using ps2x::VsyncLock;
    constexpr int64_t kFieldNs = VsyncLock::kEmulatedFieldNs;
    constexpr int64_t kMs = 1000000;

    int g_failures = 0;
    int g_checks = 0;
    void check(bool ok, const std::string &what)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failures;
            std::printf("FAIL %s\n", what.c_str());
        }
    }

    // host_vsync.cpp's measurement: the last vblank, and the period smoothed by 1/32 per interval (the first interval
    // as it is; one off by more than 25% ignored: a missed wakeup). No measurement while the last vblank is more than
    // 100 ms old (ps2xHostVsync: the window was not drawn, a stall), which the lock takes as the display measured afresh.
    struct HostVsync
    {
        int64_t last = 0, previous = 0, period = 0;
        void vblank(int64_t t)
        {
            last = t;
            if (previous != 0)
            {
                const int64_t interval = t - previous;
                if (period == 0)
                    period = interval;
                else if (interval > period * 3 / 4 && interval < period * 5 / 4)
                    period += (interval - period) / 32;
            }
            previous = t;
        }
        bool measured(int64_t now) const { return last != 0 && period != 0 && now - last <= 100 * kMs; }
    };

    // ps2_audio.cpp's queue, in stereo frames at 48 kHz: 16384 frames of room (an overflow once a 512-frame submit no longer fits), the rate control aiming at 4096 with at
    // most 0.5% (proportional to the fill), an overflow dropping down to 2048 (a skip forward), an empty queue a gap
    // until 2048 frames are back (the priming). Playing starts at the priming level, as the real queue does.
    struct SoundQueue
    {
        static constexpr double kRoom = 16384.0 - 512.0, kTarget = 4096.0, kPrime = 2048.0, kRate = 48000.0;
        double queued = kPrime;
        bool priming = false;
        int overflows = 0, underruns = 0;
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
        void play(int64_t ns)
        {
            if (priming)
                return;
            const double fill = (queued - kTarget) / kTarget;
            const double ratio = 1.0 + std::clamp(fill * 0.005, -0.005, 0.005);
            queued -= static_cast<double>(ns) * 1e-9 * kRate * ratio;
            if (queued < 0.0)
            {
                ++underruns;
                queued = 0.0;
                priming = true;
            }
        }
    };

    struct Display
    {
        enum Kind
        {
            Fixed,
            Vrr
        } kind = Fixed;
        double hz = 60.0;          // Fixed: the refresh; Vrr: the most it refreshes
        int64_t vrrDelayNs = 0;    // Vrr: scanout this long after the present (render + display latency)
        int64_t jitterNs = 200000; // measurement jitter on each vblank's time (the vblank thread's wakeup)
        double lateShare = 0.0;    // and this share of vblanks seen late, by 30-100% of lateNs (a loaded PC's wakeups)
        int64_t lateNs = 0;
        int64_t expLatencyNs = 0;  // and every vblank seen late by an exponential latency of this mean (a loaded PC)
        double burstAtS = -1.0;    // and burstVblanks vblanks in a row from this time, seen burstNs late
        int burstVblanks = 0;
        int64_t burstNs = 0;
    };

    // Stalls: every `everyS` seconds a field fires `ms` late. Under 100 ms the scheduler re-anchors (EeScheduler.cpp:
    // hostStart = max(deadline, now - 2 ms)) and that much emulated time is gone; from 100 ms the vblank measurement is
    // stale too (no host vblank seen while the window was not drawn) and the lock starts over.
    struct Stalls
    {
        double everyS = 0.0, ms = 0.0;
        double gapEveryS = 0.0, gapMs = 0.0; // no host vblank measured for gapMs while the game runs on time
        double lateShare = 0.0, lateMaxMs = 0.0; // and this share of fields late at random, by up to lateMaxMs (a slow PC)
    };

    struct Result
    {
        bool released = false;
        double releasedAfterS = -1.0;
        int releases = 0;
        int stalls = 0; // stalls that happened
        int overflows = 0, underruns = 0;
        double speed = 0.0; // the game's speed over the last half: emulated time / real time
        int64_t multiple = -1;
        int displayReports = 0;
        int64_t meanAbsErrorNs = 0; // the phase error over the last half (locked fields)
        double stretchedShare = 0.0; // the share of fields the audio's time-stretch would be on (StretchControl, net lost time)
        // Fixed displays: the share of presents (at the field's start) landing within 1 ms before or 0.5 ms after a host
        // vblank, where the compositor's pickup makes an image show twice or not at all (what the lock exists to avoid).
        double collisionShare = 0.0;
    };

    // lockOn = false: no lock at all (the deadline as the scheduler makes it): the baseline a scenario's own hitches give.
    Result run(const Display &display, double seconds, VsyncLock::Mode mode, Stalls stalls = {}, uint32_t seed = 1,
               bool lockOn = true)
    {
        std::mt19937 random(seed);
        std::uniform_int_distribution<int64_t> jitter(-display.jitterNs, display.jitterNs);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        int burstLeft = 0;
        const auto late = [&](std::mt19937 &r)
        {
            if (burstLeft > 0)
            {
                --burstLeft;
                return display.burstNs;
            }
            const int64_t exp = display.expLatencyNs > 0
                                    ? static_cast<int64_t>(-std::log(1.0 - unit(r)) * static_cast<double>(display.expLatencyNs))
                                    : 0;
            if (display.lateShare <= 0.0 || unit(r) >= display.lateShare)
                return exp;
            return exp + static_cast<int64_t>((0.3 + 0.7 * unit(r)) * static_cast<double>(display.lateNs));
        };
        VsyncLock lock(7.0e6, mode);
        HostVsync host;
        SoundQueue sound;
        Result result;
        std::deque<int64_t> pending; // host vblanks not yet seen by the vblank thread
        const int64_t start = 1000 * kMs;
        const int64_t fixedPeriod = static_cast<int64_t>(1e9 / display.hz);
        int64_t nextFixed = start - 37 * kMs; // the display's vblanks run before the game's first field
        int64_t lastScan = 0;
        int64_t deadline = start;
        int64_t previousFire = start;
        const int fields = static_cast<int>(seconds * 1e9 / static_cast<double>(kFieldNs));
        const int stallEvery = stalls.everyS > 0.0 ? static_cast<int>(stalls.everyS * 1e9 / static_cast<double>(kFieldNs)) : 0;
        int64_t halfStartReal = 0, halfEmulated = 0, errorSum = 0, errorFields = 0;
        // The scheduler's net lost-time count (EeScheduler.cpp: the re-anchor's drop, stalls over 50 ms left out, plus
        // the lock's own move) and the stretch control it drives.
        int64_t lostNet = 0;
        ps2x::StretchControl stretch;
        int stretchedFields = 0;
        int collisions = 0, collisionFields = 0;
        const int gapEvery = stalls.gapEveryS > 0.0 ? static_cast<int>(stalls.gapEveryS * 1e9 / static_cast<double>(kFieldNs)) : 0;
        const int gapFields = static_cast<int>(stalls.gapMs * 1e6 / static_cast<double>(kFieldNs));
        for (int field = 0; field < fields; ++field)
        {
            const bool stall = stallEvery > 0 && field > 0 && field % stallEvery == 0;
            result.stalls += stall ? 1 : 0;
            const int64_t randomLate = stalls.lateShare > 0.0 && unit(random) < stalls.lateShare
                                           ? static_cast<int64_t>(unit(random) * stalls.lateMaxMs * static_cast<double>(kMs))
                                           : 0;
            const int64_t fire = deadline + (stall ? static_cast<int64_t>(stalls.ms * kMs) : 0) + randomLate;
            // The host vblanks up to now reach the measurement. During a long stall the window is not drawn: on a
            // VRR display no vblank comes; on a fixed display the vblank thread keeps seeing them unless the stall is a
            // whole-process hang (taken as such from 100 ms: none seen).
            const bool hang = stall && stalls.ms >= 100.0;
            if (display.kind == Display::Fixed)
                for (; nextFixed <= fire; nextFixed += fixedPeriod)
                    if (!(hang && nextFixed > deadline))
                    {
                        if (display.burstAtS >= 0.0 && burstLeft == 0 && nextFixed - start >= static_cast<int64_t>(display.burstAtS * 1e9) &&
                            nextFixed - start < static_cast<int64_t>(display.burstAtS * 1e9) + fixedPeriod)
                            burstLeft = display.burstVblanks;
                        pending.push_back(nextFixed + jitter(random) + late(random));
                    }
            std::sort(pending.begin(), pending.end());
            while (!pending.empty() && pending.front() <= fire)
            {
                host.vblank(pending.front());
                pending.pop_front();
            }
            if (display.kind == Display::Fixed && host.period > 0 && field > 600)
            {
                const int64_t phase = ((fire - host.last) % host.period + host.period) % host.period;
                ++collisionFields;
                collisions += (phase > host.period - kMs || phase < kMs / 2) ? 1 : 0;
            }
            // The sound card played since the last field; the game makes one field of sound.
            sound.play(fire - previousFire);
            sound.produce(SoundQueue::kRate * static_cast<double>(kFieldNs) * 1e-9);
            previousFire = fire;
            // This field's image: on a VRR display it is scanned out a delay after its present, no sooner than the
            // display's fastest refresh allows.
            if (display.kind == Display::Vrr)
            {
                const int64_t scan = std::max(fire + display.vrrDelayNs, lastScan + static_cast<int64_t>(1e9 / display.hz));
                lastScan = scan;
                pending.push_back(scan + jitter(random) + late(random));
            }
            // EeScheduler.cpp: the next field's deadline.
            const int64_t hostStart = std::max(deadline, fire - 2 * kMs);
            const int64_t dropped = hostStart - deadline;
            const bool gap = gapEvery > 0 && field > 0 && field % gapEvery < gapFields;
            VsyncLock::Report report;
            deadline = lock.next(hostStart + kFieldNs, lockOn && !gap && host.measured(fire), host.last, host.period, report);
            lostNet += (dropped <= 50 * kMs ? dropped : 0) + (deadline - (hostStart + kFieldNs));
            stretchedFields += stretch.update(fire, lostNet, sound.fill()) ? 1 : 0;
            if (report.displayChanged)
            {
                ++result.displayReports;
                result.multiple = report.multiple;
            }
            if (report.released)
            {
                ++result.releases;
                if (!result.released)
                {
                    result.released = true;
                    result.releasedAfterS = static_cast<double>(fire - start) * 1e-9;
                }
            }
            if (field == fields / 2)
            {
                halfStartReal = fire;
                halfEmulated = 0;
            }
            if (field >= fields / 2)
            {
                halfEmulated += kFieldNs;
                if (report.locked)
                {
                    errorSum += std::abs(report.errorNs);
                    ++errorFields;
                }
            }
        }
        result.overflows = sound.overflows;
        result.underruns = sound.underruns;
        result.speed = static_cast<double>(halfEmulated) / static_cast<double>(deadline - halfStartReal);
        result.meanAbsErrorNs = errorFields ? errorSum / errorFields : 0;
        result.stretchedShare = static_cast<double>(stretchedFields) / static_cast<double>(fields);
        result.collisionShare = collisionFields ? static_cast<double>(collisions) / static_cast<double>(collisionFields) : 0.0;
        return result;
    }

    std::string name(const Display &d)
    {
        char text[96];
        if (d.kind == Display::Fixed)
            std::snprintf(text, sizeof(text), "fixed %.3f Hz", d.hz);
        else
            std::snprintf(text, sizeof(text), "VRR up to %.0f Hz, scanout %.1f ms after the present", d.hz,
                          static_cast<double>(d.vrrDelayNs) / kMs);
        return text;
    }

    std::string skips(const Result &r)
    {
        return std::to_string(r.overflows) + " overflows, " + std::to_string(r.underruns) + " underruns";
    }

    void fixedDisplays()
    {
        const double fieldsPerS = 1e9 / static_cast<double>(kFieldNs);
        for (double hz : {60.0, 59.94, 120.0, 119.88, 240.0})
        {
            Display d;
            d.hz = hz;
            const int64_t multiple = static_cast<int64_t>(hz / 59.94 + 0.5);
            const double expectedSpeed = (hz / static_cast<double>(multiple)) / fieldsPerS;
            // 3 minutes as it runs.
            const Result r = run(d, 180.0, VsyncLock::Mode::Checked);
            check(!r.released, name(d) + ": stays locked");
            // A slow PC (issue #5's): 5% / 10% of fields late at random by up to 10 ms, every vblank seen late by an
            // exponential latency of 0.6 ms mean. 5 minutes, 10 seeds each: let go at most a few times in all (and
            // tried again within 2 minutes), not on most runs (a run that steps both ways nets too little to judge).
            if (hz == 60.0 || hz == 59.94)
            {
                int releases = 0, runs = 0;
                for (double share : {0.05, 0.10})
                    for (int64_t latency : {int64_t(600000), int64_t(1000000)})
                    for (uint32_t seed = 1; seed <= 10; ++seed)
                    {
                        Display slow = d;
                        slow.jitterNs = 0;
                        slow.expLatencyNs = latency;
                        Stalls late;
                        late.lateShare = share;
                        late.lateMaxMs = 10.0;
                        releases += run(slow, 300.0, VsyncLock::Mode::Checked, late, seed).releases;
                        ++runs;
                    }
                std::printf("  %-26s slow PC (5%% / 10%% of fields late at random, 0.6 / 1 ms vblank latency): let go %d times in %d runs\n",
                            name(d).c_str(), releases, runs);
                check(releases <= 1, name(d) + " on a slow PC (fields late at random, 0.6 / 1 ms vblank latency): let go " +
                                         std::to_string(releases) + " times in " + std::to_string(runs) + " runs (at most 1)");
            }
            // One burst of late vblanks (9, 12, 20 in a row seen 4 / 7 ms late: the vblank thread held up for a moment),
            // at 38 points of a drift span: never let go (one span's end moved, not three spans). And 1% of vblanks 2-5 ms
            // late for 30 minutes, 4 seeds: never let go.
            {
                int burstReleases = 0, bursts = 0;
                for (int vblanks : {9, 12, 20})
                    for (int64_t lateNs : {4 * kMs, 7 * kMs})
                        for (int at = 0; at < 38; ++at)
                        {
                            Display b = d;
                            b.jitterNs = 100000;
                            b.burstAtS = 20.0 + at * 3.0 * static_cast<double>(kFieldNs) * 1e-9;
                            b.burstVblanks = vblanks * multiple;
                            b.burstNs = lateNs;
                            burstReleases += run(b, 40.0, VsyncLock::Mode::Checked, {}, 1 + at).releases;
                            ++bursts;
                        }
                check(burstReleases == 0, name(d) + ": one burst of late vblanks never lets go (" + std::to_string(burstReleases) + " of " +
                                              std::to_string(bursts) + ")");
                int longReleases = 0;
                for (uint32_t seed = 1; seed <= 4; ++seed)
                {
                    Display l = d;
                    l.jitterNs = 100000;
                    l.lateShare = 0.01;
                    l.lateNs = 5 * kMs;
                    longReleases += run(l, 1800.0, VsyncLock::Mode::Checked, {}, seed).releases;
                }
                check(longReleases == 0, name(d) + ": 1% of vblanks up to 5 ms late, 30 minutes x 4: never let go (" +
                                             std::to_string(longReleases) + ")");
            }
            // A loaded PC's vblank thread: every vblank seen late by an exponential latency of 1.5 / 2 ms mean (its long
            // tail several ms), the game near full speed (5% of fields late at random by up to 4 ms). 10 minutes, 6 seeds:
            // let go rarely if at all (each one is undone at the next try, 2 minutes on), and presents no closer to the
            // vblank than the release-only lock by more than a little.
            {
                int releases = 0, runs = 0;
                double checkedCollisions = 0.0, releaseOnlyCollisions = 0.0;
                for (int64_t latency : {int64_t(1500000), int64_t(2000000)})
                    for (uint32_t seed = 1; seed <= 6; ++seed)
                    {
                        Display loaded = d;
                        loaded.jitterNs = 0;
                        loaded.expLatencyNs = latency;
                        Stalls late;
                        late.lateShare = 0.05;
                        late.lateMaxMs = 4.0;
                        const Result c = run(loaded, 600.0, VsyncLock::Mode::Checked, late, seed);
                        const Result u = run(loaded, 600.0, VsyncLock::Mode::Uncapped, late, seed);
                        releases += c.releases;
                        checkedCollisions += c.collisionShare;
                        releaseOnlyCollisions += u.collisionShare;
                        ++runs;
                    }
                std::printf("  %-26s vblank latency 1.5 / 2 ms: let go %d times in %d runs of 10 min; on the vblank %.4f vs the release-only lock %.4f\n",
                            name(d).c_str(), releases, runs, checkedCollisions / runs, releaseOnlyCollisions / runs);
                // (at 240 Hz that latency is half a refresh or more: the release-only lock already has a third of the presents on
                // the vblank; let go now and then there, the presents are about where they were)
                if (multiple <= 2)
                    check(releases <= 2, name(d) + " with 1.5 / 2 ms vblank latency: let go at most twice in 12 runs of 10 minutes (" +
                                             std::to_string(releases) + ")");
                check(checkedCollisions <= releaseOnlyCollisions + (multiple <= 2 ? 0.005 : 0.01) * runs,
                      name(d) + " with 1.5 / 2 ms vblank latency: presents no closer to the vblank than the release-only lock (" +
                          std::to_string(checkedCollisions / runs) + " vs " + std::to_string(releaseOnlyCollisions / runs) + ")");
            }
            // A noisier measurement (a loaded PC's vblank thread: +-0.5 / +-1 ms, or 3% of vblanks seen up to 4 ms late):
            // 5 minutes, 4 seeds each (10 with late vblanks: what the medians are for). Never let go (the release-only lock never is), presents no closer to the
            // vblank than the release-only lock's.
            {
                struct Noise
                {
                    int64_t jitterNs;
                    double lateShare;
                    int64_t lateNs;
                    const char *what;
                };
                for (const Noise &n : {Noise{500000, 0.0, 0, "+-0.5 ms jitter"}, Noise{1000000, 0.0, 0, "+-1 ms jitter"},
                                       Noise{50000, 0.03, 4 * kMs, "3% of vblanks up to 4 ms late"},
                                       Noise{300000, 0.03, 4 * kMs, "+-0.3 ms jitter and 3% of vblanks up to 4 ms late"}})
                    for (uint32_t seed = 1; seed <= (n.lateShare > 0.0 ? 10u : 4u); ++seed)
                    {
                        Display noisy = d;
                        noisy.jitterNs = n.jitterNs;
                        noisy.lateShare = n.lateShare;
                        noisy.lateNs = n.lateNs;
                        const Result p = run(noisy, 300.0, VsyncLock::Mode::Checked, {}, seed);
                        const Result u = run(noisy, 300.0, VsyncLock::Mode::Uncapped, {}, seed);
                        const std::string what = name(d) + " with " + n.what + " (seed " + std::to_string(seed) + ")";
                        check(p.releases == 0, what + ": stays locked (" + std::to_string(p.releases) + " releases)");
                        check(p.collisionShare <= u.collisionShare + 0.002, what + ": presents no closer to the vblank than the release-only lock (" +
                                                                               std::to_string(p.collisionShare) + " vs " +
                                                                               std::to_string(u.collisionShare) + ")");
                    }
            }
            check(r.multiple == multiple, name(d) + ": " + std::to_string(multiple) + " refresh(es) per field");
            check(std::abs(r.speed - expectedSpeed) < 0.0002, name(d) + ": the game runs at the display's rate (speed " +
                                                                   std::to_string(r.speed) + ", expected " +
                                                                   std::to_string(expectedSpeed) + ")");
            check(r.meanAbsErrorNs <= VsyncLock::kReleaseErrorNs, name(d) + ": in step (mean phase error " + std::to_string(r.meanAbsErrorNs) +
                                                                      " ns, the lock's own margin " + std::to_string(VsyncLock::kReleaseErrorNs) + ")");
            check(r.collisionShare == 0.0, name(d) + ": no present lands on a vblank (" + std::to_string(r.collisionShare) + ")");
            check(r.overflows == 0 && r.underruns == 0, name(d) + ": the sound never skips (" + skips(r) + ")");
            // Short stalls (a 30 ms hitch every 20 s: shader compiles, file reads): re-anchored, pulled back, never
            // taken for a display the lock cannot follow.
            const Result h = run(d, 180.0, VsyncLock::Mode::Checked, {20.0, 30.0});
            check(!h.released, name(d) + " with 30 ms stalls: stays locked");
            check(h.collisionShare < 0.01, name(d) + " with 30 ms stalls: presents clear of the vblank (" + std::to_string(h.collisionShare) + ")");
            check(h.overflows == 0 && h.underruns == 0, name(d) + " with 30 ms stalls: the sound never skips (" + skips(h) + ")");
            // Frequent re-anchors (a 6 ms hitch every second: a PC near its limit): the fields after each are not
            // judged; still locked.
            const Result f = run(d, 120.0, VsyncLock::Mode::Checked, {1.0, 6.0});
            check(!f.released, name(d) + " with a hitch every second: stays locked");
            // A PC below full speed (issue #5: an 8 ms late field every 0.3 s, so the scheduler re-anchors before the
            // lock has pulled the phase back): every field after a re-anchor is unjudged, so it stays locked.
            const Result slow = run(d, 120.0, VsyncLock::Mode::Checked, {0.3, 8.0});
            check(!slow.released, name(d) + " re-anchoring every 0.3 s: stays locked (the settle fields)");
            // A PC that loses a little time often (a field 2.6 ms late four times a second: 0.6 ms lost each, past the
            // 2 ms catch-up): each re-anchor is pulled back for free; still locked and in step.
            const Result sub = run(d, 120.0, VsyncLock::Mode::Checked, {0.25, 2.6});
            check(!sub.released, name(d) + " losing 0.6 ms four times a second: stays locked");
            check(sub.meanAbsErrorNs < 800000, name(d) + " losing 0.6 ms four times a second: in step (mean phase error " +
                                                   std::to_string(sub.meanAbsErrorNs) + " ns)");
            // An 8 ms hitch every second: where the lock pulls each loss back (the short way is earlier: 60 Hz) the
            // game's true speed is full and the audio's time-stretch (driven by the scheduler's net lost time) must stay
            // off; where realigning is the short way later (120, 240 Hz) the game really runs ~0.8% slow and the
            // stretch is right to come on. The rule checked: never on while the true speed is at least 99.8%.
            const Result keep = run(d, 120.0, VsyncLock::Mode::Checked, {1.0, 8.0});
            if (keep.speed >= 0.998 * expectedSpeed)
                check(keep.stretchedShare == 0.0, name(d) + " with an 8 ms hitch every second at full speed (" + std::to_string(keep.speed) +
                                                      "): the time-stretch stays off (" + std::to_string(keep.stretchedShare) + ")");
            std::printf("  %-26s 8 ms hitch every second: speed %.4f, stretched %.0f%%\n", name(d).c_str(), keep.speed,
                        keep.stretchedShare * 100.0);
            // Measurement gaps (300 ms without a vblank every 3 s, the game on time): the lock must not book the gap as
            // lost time (it did: a fake 20 ms debt, the game run ahead, the queue overflowing).
            Stalls gaps;
            gaps.gapEveryS = 3.0;
            gaps.gapMs = 300.0;
            const Result g = run(d, 120.0, VsyncLock::Mode::Checked, gaps);
            const Result gNone = run(d, 120.0, VsyncLock::Mode::Checked, gaps, 1, false);
            check(g.overflows + g.underruns <= gNone.overflows + gNone.underruns,
                  name(d) + " with 300 ms measurement gaps: the lock adds no skips (" + skips(g) + "; without it " + skips(gNone) + ")");
            check(std::abs(g.speed - expectedSpeed) < 0.0015, name(d) + " with 300 ms measurement gaps: the game not run ahead (speed " +
                                                                   std::to_string(g.speed) + ")");
            // Re-anchors of every size (3-48 ms every 0.5-3 s; over half a refresh the phase error wraps): never let go
            // more often than the release-only lock lets go, and presents no closer to the vblank than the release-only lock's, on average
            // over the sizes and at each one within 3%.
            {
                double checkedCollisions = 0.0, releaseOnlyCollisions = 0.0, worstExcess = 0.0;
                int sizes = 0;
                for (double every : {0.5, 1.0, 2.0, 3.0})
                    for (double ms = 3.0; ms <= 48.0; ms += 1.5)
                    {
                        const Result big = run(d, 60.0, VsyncLock::Mode::Checked, {every, ms});
                        const Result up = run(d, 60.0, VsyncLock::Mode::Uncapped, {every, ms});
                        checkedCollisions += big.collisionShare;
                        releaseOnlyCollisions += up.collisionShare;
                        worstExcess = std::max(worstExcess, big.collisionShare - up.collisionShare);
                        ++sizes;
                        check(big.releases <= up.releases, name(d) + " with a " + std::to_string(ms).substr(0, 4) + " ms re-anchor every " +
                                                               std::to_string(every).substr(0, 3) + " s: not let go more than the release-only lock (" +
                                                               std::to_string(big.releases) + " vs " + std::to_string(up.releases) + ")");
                    }
                check(checkedCollisions <= releaseOnlyCollisions + 0.001 * sizes,
                      name(d) + " with re-anchors of every size: presents no closer to the vblank than the release-only lock on average (" +
                          std::to_string(checkedCollisions / sizes) + " vs " + std::to_string(releaseOnlyCollisions / sizes) + ")");
                check(worstExcess < 0.03, name(d) + " with re-anchors of every size: at no size much closer (worst excess " +
                                              std::to_string(worstExcess) + ")");
            }
            // Loading screens (a 150 ms hang every 30 s: no vblank seen, the lock starts over each time). A hang makes no
            // sound for longer than the queue holds: one gap each is the hang itself. The lock starting over must add none.
            const Result l = run(d, 180.0, VsyncLock::Mode::Checked, {30.0, 150.0});
            check(l.overflows == 0 && l.underruns <= l.stalls, name(d) + " with 150 ms hangs: no skip beyond the hangs' own gaps (" +
                                                                    skips(l) + ", " + std::to_string(l.stalls) + " hangs)");
            std::printf("  %-26s locked, speed %.5f, phase error %.3f ms; stalls: %s; hitches: %s; hangs: sound %s\n",
                        name(d).c_str(), r.speed, static_cast<double>(r.meanAbsErrorNs) / kMs, h.released ? "RELEASED" : "locked",
                        f.released ? "RELEASED" : "locked", l.overflows + l.underruns > l.stalls ? "SKIPS" : "ok");
        }
        // Displays within the 2% the lock takes for a multiple, but too far from 59.94 Hz to follow without moving the
        // game past what the sound absorbs: it may stay near or let go, but the sound must not skip.
        for (double hz : {60.5, 61.0, 59.0})
        {
            Display d;
            d.hz = hz;
            const Result r = run(d, 180.0, VsyncLock::Mode::Checked);
            check(r.overflows == 0 && r.underruns == 0, name(d) + ": the sound never skips (" + skips(r) + ")");
            check(std::abs(r.speed - 1.0) < 0.0025, name(d) + ": the game stays within 0.25% of 59.94 Hz (speed " +
                                                        std::to_string(r.speed) + ")");
            std::printf("  %-26s %s, speed %.5f, sound %s\n", name(d).c_str(), r.released ? "released" : "near",
                        r.speed, r.overflows + r.underruns ? "SKIPS" : "ok");
        }
    }

    void vrrDisplays()
    {
        int legacySkipping = 0, uncappedSkipping = 0, delays = 0, released = 0;
        double worstSpeed = 0.0;
        for (int tenths = 0; tenths <= 160; ++tenths)
        {
            Display d;
            d.kind = Display::Vrr;
            d.hz = 120.0;
            d.vrrDelayNs = tenths * kMs / 10;
            ++delays;
            const Result r = run(d, 120.0, VsyncLock::Mode::Checked);
            released += r.released ? 1 : 0;
            worstSpeed = std::max(worstSpeed, std::abs(r.speed - 1.0));
            check(r.overflows == 0 && r.underruns == 0, name(d) + ": the sound never skips (" + skips(r) + ")");
            check(r.speed > 0.9975 && r.speed < 1.005, name(d) + ": the game at most 0.25% slower, 0.5% faster than 59.94 Hz (speed " + std::to_string(r.speed) + ")");
            // Loading screens: the measurement goes stale, the release is undone, the lock starts over each time.
            const Result l = run(d, 180.0, VsyncLock::Mode::Checked, {30.0, 150.0});
            check(l.overflows == 0 && l.underruns <= l.stalls, name(d) + " with 150 ms hangs: no skip beyond the hangs' own gaps (" +
                                                                    skips(l) + ", " + std::to_string(l.stalls) + " hangs)");
            // Small hitches often (a field 3.5 ms late: 1.5 ms lost) on a VRR display: the lock must not turn them into
            // skips (re-anchors kept the old lock pulling at full speed: issue #1 again).
            if (tenths % 5 == 0)
                for (double every : {0.3, 0.5, 0.8, 1.2})
                {
                    // Every 0.3 s the hitches alone lose 0.5%, all the rate control absorbs: they may skip by
                    // themselves; the lock must not add any. From 0.5 s on (0.3%) there must be none at all.
                    const Result hz = run(d, 120.0, VsyncLock::Mode::Checked, {every, 3.5});
                    const Result none = run(d, 120.0, VsyncLock::Mode::Checked, {every, 3.5}, 1, false);
                    const std::string what = name(d) + " with a 3.5 ms hitch every " + std::to_string(every).substr(0, 3) + " s";
                    check(hz.overflows + hz.underruns <= none.overflows + none.underruns,
                          what + ": the lock adds no skips (" + skips(hz) + "; without the lock " + skips(none) + ")");
                    if (every >= 0.5)
                        check(hz.overflows == 0 && hz.underruns == 0, what + ": the sound never skips (" + skips(hz) + ")");
                }
            const Result legacy = run(d, 120.0, VsyncLock::Mode::UncappedNoRelease);
            if (legacy.overflows + legacy.underruns > 0)
                ++legacySkipping;
            // The release alone (3d97a84): up to 4 s at 1.5% before it lets go.
            const Result uncapped = run(d, 120.0, VsyncLock::Mode::Uncapped);
            if (uncapped.overflows + uncapped.underruns > 0)
                ++uncappedSkipping;
            if (tenths % 10 == 0)
                std::printf("  %-52s %s, speed %.4f, sound %s; old lock: speed %.4f, %d skips\n", name(d).c_str(),
                            r.released ? ("released after " + std::to_string(r.releasedAfterS).substr(0, 4) + " s").c_str() : "locked",
                            r.speed, r.overflows + r.underruns ? "SKIPS" : "ok", legacy.speed, legacy.overflows + legacy.underruns);
        }
        std::printf("  %d delays: %d released, the game never more than %.3f%% off 59.94 Hz\n", delays, released, worstSpeed * 100.0);
        // Issue #1 reproduced: the old lock drives a VRR display's game off speed and the music skips.
        check(legacySkipping * 2 >= delays, "the old lock skips on most VRR delays (issue #1 reproduced: " + std::to_string(legacySkipping) +
                                                " of " + std::to_string(delays) + ")");
        // And the release-only lock still skips on some: before it lets go the queue can run dry or overflow (a
        // queue that starts at its priming level has ~42 ms of margin; 4 s at 1.5% is 60 ms), and some delays it never
        // lets go of. This is what Mode::Checked's additions are for.
        std::printf("  the release-only lock: the sound skips on %d of %d delays\n", uncappedSkipping, delays);
        check(uncappedSkipping > 0, "the release-only lock still skips on some VRR delays (why Mode::Checked)");
        // Big hitches often (a field 20 ms late every 0.67 s: more than a field lost each time) on a VRR display capped
        // at 60 Hz: the judging spans them (each re-anchor is taken out exactly), released, the lock adds no skips.
        {
            Display d;
            d.kind = Display::Vrr;
            d.hz = 60.0;
            d.vrrDelayNs = 1 * kMs;
            const Result p = run(d, 120.0, VsyncLock::Mode::Checked, {0.67, 20.0});
            const Result none = run(d, 120.0, VsyncLock::Mode::Checked, {0.67, 20.0}, 1, false);
            check(p.released, "VRR capped at 60 Hz with a 20 ms hitch every 0.67 s: released");
            check(p.overflows + p.underruns <= none.overflows + none.underruns,
                  "VRR capped at 60 Hz with a 20 ms hitch every 0.67 s: the lock adds no skips (" + skips(p) + "; without it " + skips(none) + ")");
        }
        // A VRR display capped at 60 Hz (FreeSync 48-60): at the cap it shows a fixed display's refresh, so the earlier
        // the lock can follow it for a while; let go no more often than every few minutes, and no more skips than
        // the release-only lock. 5 minutes at each delay.
        {
            int worstReleases = 0, checkedSkips = 0, releaseOnlySkips = 0;
            for (int tenths = 0; tenths <= 160; tenths += 10)
            {
                Display d;
                d.kind = Display::Vrr;
                d.hz = 60.0;
                d.vrrDelayNs = tenths * kMs / 10;
                const Result p = run(d, 300.0, VsyncLock::Mode::Checked);
                const Result u = run(d, 300.0, VsyncLock::Mode::Uncapped);
                worstReleases = std::max(worstReleases, p.releases);
                checkedSkips += p.overflows + p.underruns;
                releaseOnlySkips += u.overflows + u.underruns;
            }
            check(worstReleases <= 6, "VRR capped at 60 Hz: let go at most 6 times in 5 minutes (" + std::to_string(worstReleases) + ")");
            check(checkedSkips <= releaseOnlySkips, "VRR capped at 60 Hz: no more skips than the release-only lock (" + std::to_string(checkedSkips) +
                                                     " vs " + std::to_string(releaseOnlySkips) + ")");
        }
        // Bigger hitches still (80 ms every 0.6 s: more than any re-anchor the old judging kept, under the 100 ms that
        // makes the measurement stale): judged across them, released.
        {
            Display d;
            d.kind = Display::Vrr;
            d.hz = 144.0;
            d.vrrDelayNs = 2 * kMs;
            const Result p = run(d, 60.0, VsyncLock::Mode::Checked, {0.6, 80.0});
            check(p.released, "VRR with an 80 ms hitch every 0.6 s: released");
        }
        // Uneven frames: a 144 Hz VRR display whose scanout varies +-3 / +-4 ms frame to frame (render times that vary
        // that much). The measured refresh is then the game's noisy pace, at moments no multiple at all: the release
        // must hold (undoing it would start the lock's pull over and over). Every 0.2 ms of delay, 3 minutes, 3 seeds.
        for (int64_t uneven : {3 * kMs, 4 * kMs})
        {
            int checkedSkips = 0, releaseOnlySkips = 0, worstReleases = 0;
            for (int tenths = 0; tenths <= 160; tenths += 2)
                for (uint32_t seed = 1; seed <= 3; ++seed)
                {
                    Display d;
                    d.kind = Display::Vrr;
                    d.hz = 144.0;
                    d.vrrDelayNs = tenths * kMs / 10;
                    d.jitterNs = uneven;
                    const Result p = run(d, 180.0, VsyncLock::Mode::Checked, {}, seed);
                    const Result u = run(d, 180.0, VsyncLock::Mode::Uncapped, {}, seed);
                    checkedSkips += p.overflows + p.underruns;
                    releaseOnlySkips += u.overflows + u.underruns;
                    worstReleases = std::max(worstReleases, p.releases);
                }
            const std::string what = "VRR up to 144 Hz with frames +-" + std::to_string(uneven / kMs) + " ms uneven";
            std::printf("  %s: %d skips (the release-only lock %d), at most %d release(s) a run\n", what.c_str(), checkedSkips,
                        releaseOnlySkips, worstReleases);
            check(checkedSkips == 0, what + ": the sound never skips (" + std::to_string(checkedSkips) + " skips; the release-only lock " +
                                        std::to_string(releaseOnlySkips) + ")");
            // (tried again every 2 minutes: let go again then, at most twice in 3 minutes)
            check(worstReleases <= 2, what + ": released once (and again after the retry), not over and over (" + std::to_string(worstReleases) + ")");
        }
    }

    void leftAlone()
    {
        // 63 Hz: 5% off 60, not within the 2% the lock takes for a multiple.
        {
            Display d63;
            d63.hz = 63.0;
            const Result r63 = run(d63, 10.0, VsyncLock::Mode::Checked);
            check(r63.multiple == 0, "fixed 63 Hz: not a multiple of 59.94 Hz");
        }
        // 144 Hz fixed: no whole multiple of 59.94 Hz within 2%: the deadline is never moved, the game runs at 59.94 Hz.
        Display d;
        d.hz = 144.0;
        const Result r = run(d, 30.0, VsyncLock::Mode::Checked);
        check(r.multiple == 0 && r.displayReports == 1, "fixed 144 Hz: reported once as not a multiple");
        check(std::abs(r.speed - 1.0) < 0.0001, "fixed 144 Hz: the game runs at 59.94 Hz (speed " + std::to_string(r.speed) + ")");

        // No measurement: every deadline as given.
        VsyncLock lock;
        VsyncLock::Report report;
        check(lock.next(123456789, false, 0, 0, report) == 123456789 && !report.locked, "no measurement: the deadline unchanged");

        // A fixed display too far from 59.94 Hz (60.5 Hz: the lock would hold the game 0.9% fast): in step, but its drift
        // over 0.2%: released, too far.
        {
            VsyncLock far60;
            VsyncLock::Report r;
            const int64_t period = static_cast<int64_t>(1e9 / 60.5);
            const int64_t grid = 1000 * kMs - 9 * kMs;
            int64_t t = 1000 * kMs;
            for (int i = 0; i < 2000 && !far60.released(); ++i)
            {
                const int64_t asked2 = t + kFieldNs;
                t = far60.next(asked2, true, grid + ((t - grid) / period) * period, period, r);
            }
            check(far60.released() && std::string(r.why) == "too far from 59.94 Hz",
                  "a fixed 60.5 Hz display: released, too far from 59.94 Hz for the sound (" + std::string(r.why) + ")");
        }

        // A display that follows the game's frames (its vblank always 1 ms after the game's deadline: never where the
        // lock wants it): let go quickly, not after 4 s of 1.5%.
        VsyncLock vrr;
        int64_t deadline = 1000 * kMs, vblank = deadline - 5 * kMs;
        int fieldsToRelease = 0;
        for (int i = 0; i < 2000 && !vrr.released(); ++i)
        {
            vblank = deadline + 1 * kMs;
            deadline = vrr.next(deadline + kFieldNs, true, vblank - kFieldNs, kFieldNs, report);
            fieldsToRelease = i + 1;
        }
        check(vrr.released() && std::string(report.why) == "not converging",
              "a display that follows the game's frames: released, not converging (" + std::string(report.why) + ")");
        check(fieldsToRelease <= 2 * VsyncLock::kStuckFields + 2, "released within two stuck runs, ~1.4 s (" + std::to_string(fieldsToRelease) + " fields; release-only: 240)");
        const int64_t asked = deadline + kFieldNs;
        check(vrr.next(asked, true, vblank, kFieldNs, report) == asked, "released: deadlines left alone");
        vrr.next(asked + kFieldNs, true, vblank, kFieldNs * 102 / 100, report);
        check(vrr.released(), "released: the measured refresh drifting 2% (a variable-refresh display's pace) keeps it");
        vrr.next(asked + 2 * kFieldNs, false, 0, 0, report); // a gap: a monitor switch, or a stall
        check(!vrr.released(), "a measurement gap: measured afresh (released no more)");
        {
            // measured afresh on the same display that follows the game: let go again within ~0.8 s
            int64_t t = asked + 2 * kFieldNs;
            int again = 0;
            for (int i = 0; i < 2000 && !vrr.released(); ++i, ++again)
                t = vrr.next(t + kFieldNs, true, t + kMs, kFieldNs, report);
            check(vrr.released() && again <= 2 * VsyncLock::kStuckFields + 2, "after the gap, the same display: let go again within ~1.4 s (" + std::to_string(again) + " fields)");
        }
        // Tried again after 2 minutes: the release is not kept for the session (a release that was wrong).
        {
            VsyncLock l;
            VsyncLock::Report r;
            int64_t t = 1000 * kMs;
            for (int i = 0; i < 2000 && !l.released(); ++i)
                t = l.next(t + kFieldNs, true, t + kMs, kFieldNs, r);
            int fields = 0;
            const int64_t period = 16666667, grid = t - 3 * kMs;
            for (; fields < 40000 && l.released(); ++fields)
                t = l.next(t + kFieldNs, true, grid + ((t - grid) / period) * period, period, r);
            check(!l.released() && fields >= 7190 && fields <= 7210, "released: tried again after 2 minutes (" + std::to_string(fields) + " fields)");
            for (int i = 0; i < 3600; ++i)
                t = l.next(t + kFieldNs, true, grid + ((t - grid) / period) * period, period, r);
            check(!l.released(), "tried again on a fixed display: kept (a minute)");
        }
        // A display that keeps following the game: each retry is let go again within seconds, so the retries back off
        // (2, 4, 8, then every 16 minutes): an hour is 6 releases, not 30.
        {
            VsyncLock l;
            VsyncLock::Report r;
            int64_t t = 1000 * kMs;
            int releases = 0;
            for (int i = 0; i < 60 * 60 * 60; ++i)
            {
                t = l.next(t + kFieldNs, true, t + kMs, kFieldNs, r);
                releases += r.released ? 1 : 0;
            }
            check(releases >= 4 && releases <= 7, "following the game for an hour: the retries back off (" + std::to_string(releases) + " releases)");
        }
        // The release-only lock (Mode::Uncapped) also undid the release on any gap.
        {
            VsyncLock up(7.0e6, VsyncLock::Mode::Uncapped);
            VsyncLock::Report r;
            int64_t t = 1000 * kMs;
            for (int i = 0; i < 2000 && !up.released(); ++i)
                t = up.next(t + kFieldNs, true, t + kMs, kFieldNs, r);
            const bool releasedFirst = up.released();
            up.next(t + kFieldNs, false, 0, 0, r);
            check(releasedFirst && !up.released(), "the release-only lock: released, then undone by a gap");
        }

        // The lines, on displays whose phase does not answer (each field's error set, in step): the lock steps error/8 a
        // field. Holding the game slower (its vblank before where the lock wants it: the lock steps later) by 0.3%:
        // released, too far, within kFarSpans spans; by 0.1%: kept. Holding it faster by 0.3%: kept (the queue has room);
        // by 0.6%: released.
        const auto inBand = [](int64_t errorNs, int &fields)
        {
            VsyncLock band;
            VsyncLock::Report r;
            const int64_t wanted = static_cast<int64_t>(7.0e6);
            int64_t t = 1000 * kMs;
            fields = 0;
            for (int i = 0; i < 3000 && !band.released(); ++i)
            {
                const int64_t asked2 = t + kFieldNs;
                t = band.next(asked2, true, asked2 - wanted - errorNs, kFieldNs, r);
                fields = i + 1;
            }
            return band.released() ? std::string(r.why) : std::string("kept");
        };
        {
            int fields = 0;
            const std::string why = inBand(-400000, fields);
            check(why == "too far from 59.94 Hz", "held 0.3% slower (in step, no answer): released, too far (" + why + ")");
            check(fields <= VsyncLock::kFarSpans * (VsyncLock::kDriftFields + VsyncLock::kDriftEnds) + 2, "within kFarSpans drift spans (" + std::to_string(fields) + " fields)");
            check(inBand(-130000, fields) == "kept", "held 0.1% slower: kept (the rate control absorbs it)");
            check(inBand(400000, fields) == "kept", "held 0.3% faster: kept (the queue fills, it has room)");
            check(inBand(800000, fields) != "kept", "held 0.6% faster: released (out of step: in step it cannot be, at 0.53 ms of error the step is 0.4%)");
        }
        // Fixed displays either side of 59.94 Hz: 60.10 / 60.15 Hz (0.27% / 0.35% faster, which the release-only lock follows) stay
        // locked at the display's rate; 60.5 Hz (0.9%) is released; on the slow side 59.88 Hz (0.1%) stays locked and
        // 59.80 Hz (0.23%: the queue would sit near empty) is released.
        for (double hz : {60.10, 60.15, 60.5, 59.88, 59.80})
        {
            Display d;
            d.hz = hz;
            const Result r = run(d, 180.0, VsyncLock::Mode::Checked);
            const double expected = hz / (1e9 / static_cast<double>(kFieldNs));
            const std::string what = "fixed " + std::to_string(hz).substr(0, 5) + " Hz";
            const bool follow = hz == 60.10 || hz == 60.15 || hz == 59.88;
            if (follow)
                check(r.releases == 0 && std::abs(r.speed - expected) < 0.0003 && r.collisionShare == 0.0,
                      what + ": locked, at the display's rate (" + std::to_string(r.releases) + " releases, speed " + std::to_string(r.speed) + ")");
            else
                // (3 minutes: released, tried again after 2 minutes and released again within seconds)
                check(r.releases == 2 && std::abs(r.speed - 1.0) < 0.0005 && r.overflows + r.underruns == 0,
                      what + ": released (and again after the retry), 59.94 Hz, the sound never skips (" + std::to_string(r.releases) +
                          " releases, speed " + std::to_string(r.speed) + ")");
        }
        // The release's report: which way and how much the lock was moving the game's clock. A display that follows
        // the game with its vblank 1 ms after the deadline (8 ms early of where the lock wants it): the lock pushes later
        // at its largest step (1.5%).
        {
            VsyncLock l;
            VsyncLock::Report r;
            int64_t t = 1000 * kMs;
            for (int i = 0; i < 2000 && !r.released; ++i)
                t = l.next(t + kFieldNs, true, t + kMs, kFieldNs, r);
            check(r.released && r.releasedLater && std::abs(r.releasedPercent - 1.4985) < 0.01,
                  "the release reports the lock moving the clock later by 1.50% (" + std::to_string(r.releasedPercent) + "%, " +
                      (r.releasedLater ? "later" : "earlier") + ")");
        }
        // A fixed 60.000 Hz display (the lock steps ~17 us a field to follow it): under the too-far line, never released.
        {
            VsyncLock fixed60;
            VsyncLock::Report r;
            const int64_t period = 16666667;
            const int64_t grid = 1000 * kMs - 9 * kMs;
            int64_t t = 1000 * kMs;
            for (int i = 0; i < 6000; ++i)
            {
                const int64_t asked2 = t + kFieldNs;
                const int64_t last = grid + ((t - grid) / period) * period;
                t = fixed60.next(asked2, true, last, period, r);
            }
            check(!fixed60.released(), "a fixed 60.000 Hz display (0.1% of drift): never released (100 s, no jitter)");
        }
        // A fixed display far out of phase at the start (8 ms, the most there is): the largest steps for ~30 fields,
        // the error shrinking: converges, the fast release does not fire.
        {
            VsyncLock far;
            VsyncLock::Report r;
            const int64_t period = 16666667;
            const int64_t grid = 1000 * kMs - 7 * kMs + 8 * kMs; // the lock's phase is 7 ms after a vblank: 8 ms off
            int64_t t = 1000 * kMs;
            for (int i = 0; i < 600; ++i)
            {
                const int64_t asked2 = t + kFieldNs;
                const int64_t last = grid + ((t - grid) / period - (t < grid ? 1 : 0)) * period;
                t = far.next(asked2, true, last, period, r);
            }
            check(!far.released() && std::abs(r.errorNs) < VsyncLock::kReleaseErrorNs,
                  "a fixed display 8 ms out of phase: converges, not released (error " + std::to_string(r.errorNs) + ")");
        }

        // The fast release's answer: a display whose phase moves by a share of each step (a synthetic one, 8 ms off at
        // first). A quarter is no answer (released, not converging); three quarters is one (it converges, kept).
        const auto answering = [](double share)
        {
            VsyncLock partial;
            VsyncLock::Report r;
            const int64_t wanted = static_cast<int64_t>(7.0e6);
            int64_t t = 1000 * kMs;
            double error = 8.0e6 - 1.0e5; // just under half a refresh
            for (int i = 0; i < 600 && !partial.released(); ++i)
            {
                const int64_t asked2 = t + kFieldNs;
                const int64_t next = partial.next(asked2, true, asked2 - wanted - static_cast<int64_t>(error), kFieldNs, r);
                error += share * static_cast<double>(next - asked2); // the display's vblank takes the rest of the step
                t = next;
            }
            return partial.released() ? std::string(r.why) : std::string("kept");
        };
        check(answering(0.25) == "not converging", "a display answering a quarter of each step: released (" + answering(0.25) + ")");
        check(answering(0.75) == "kept", "a display answering three quarters of each step: kept (" + answering(0.75) + ")");

        // The phase error is wrapped to half a refresh: a field just before the next host vblank (15.5 ms after the last
        // on a 60 Hz display) is 8.2 ms early, not 8.5 ms late, and the step is later.
        {
            VsyncLock wrap(7.0e6, VsyncLock::Mode::Uncapped);
            VsyncLock::Report r;
            const int64_t asked2 = 1000 * kMs;
            const int64_t next = wrap.next(asked2, true, asked2 - 15500000, kFieldNs, r);
            check(r.errorNs < -8000000 && next > asked2, "an error past half a refresh wraps: early, stepped later (error " +
                                                              std::to_string(r.errorNs) + ")");
        }

        // The in-step rule (the release-only lock's, shared by both modes): a window with at least 10% of its fields within
        // kReleaseErrorNs keeps the lock; under 10% releases it. Each field's phase error is chosen directly (the vblank
        // placed relative to the asked deadline), which is a display that does not answer the lock's steps: Mode::Checked
        // lets go of that on its own (the tests above), so the rule is checked in Mode::Uncapped.
        const auto inStepShare = [](int inStepEvery, int fields)
        {
            VsyncLock judged(7.0e6, VsyncLock::Mode::Uncapped);
            VsyncLock::Report r;
            const int64_t wanted = static_cast<int64_t>(7.0e6); // the lock's phase at 60 Hz
            int64_t t = 1000 * kMs;
            for (int i = 0; i < fields && !judged.released(); ++i)
            {
                const int64_t asked2 = t + kFieldNs;
                const int64_t error = i % inStepEvery == 0 ? 0 : 3 * kMs; // in step 1 field in inStepEvery, else 3 ms off
                t = judged.next(asked2, true, asked2 - wanted - error, kFieldNs, r);
            }
            return judged.released();
        };
        check(!inStepShare(5, 1200), "20% of the fields in step: stays locked");
        check(inStepShare(20, 1200), "5% of the fields in step: released");
        // The first window is convergence: out of step through it, in step after, never released.
        {
            VsyncLock converging(7.0e6, VsyncLock::Mode::Uncapped);
            VsyncLock::Report r;
            const int64_t wanted = static_cast<int64_t>(7.0e6);
            int64_t t = 1000 * kMs;
            for (int i = 0; i < 600; ++i)
            {
                const int64_t asked2 = t + kFieldNs;
                t = converging.next(asked2, true, asked2 - wanted - (i < 118 ? 3 * kMs : 0), kFieldNs, r);
            }
            check(!converging.released(), "out of step through the first window only (convergence): not released");
        }
    }
}

int main()
{
    std::printf("fixed displays (3 min each; then 30 ms stalls every 20 s, a 6 ms hitch every second, 150 ms hangs every 30 s):\n");
    fixedDisplays();
    std::printf("VRR displays up to 120 Hz, every delay from 0 to 16 ms in 0.1 ms steps (2 min, then 3 min with 150 ms hangs):\n");
    vrrDisplays();
    std::printf("left alone, the fast release, too far, gaps, the wrap, the in-step rule:\n");
    leftAlone();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
