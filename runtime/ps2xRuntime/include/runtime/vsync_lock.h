#pragma once

// The emulated vblank's lock to the host display (EeScheduler.cpp lockToHostVsync holds the reasons; this is its
// arithmetic with the clock and the display measurement passed in, so tests can drive it with a simulated display:
// test/vsync_lock_test.cpp). Times are nanoseconds on one monotonic clock.
//
// Issue #1: on a variable-refresh display (G-Sync / FreeSync with VSync on) the display's vblank follows the game's own
// presents, so the lock never reaches its phase and kept moving the game's clock, up to 1.5% (0.25 ms a field), past
// what the sound's rate control absorbs: the music skipped. The release-only lock (3d97a84) releases the lock after two 2 s windows
// mostly out of step; that left skips (4 s at 1.5% outlasts a queue at its priming level; a display whose delay sits
// inside the in-step margin was never released and ran 0.3-0.5% off; hitches more often than every ~0.7 s kept its
// windows from ever being judged). On top of the release-only lock, whose steps it leaves as they are, Mode::Checked asks one
// question: does the display's phase answer the game's clock? On a display with its own refresh, the phase moves each
// field by the step taken plus the drift of the two rates; on one that follows the game's presents it does not move.
// "The phase" is taken from the raw times, net of the scheduler's re-anchors (a deadline later than the last one + a
// field moves it by as much on any display), with whole refreshes taken off only once they are unambiguous (a vblank
// seen late is held with the next one, and one still ambiguous after kPendingFields fields is dropped and the
// judging started over). Then:
//   - not converging: kStuckFields fields in a row at the largest step (either way: near half a refresh jitter and
//     hitches flip the error's side), netting at least half of what they could, over which the phase moved by less
//     than 30% of that, kStuckMisses runs in a row: let go (~1.4 s; a display the lock follows converges within half a
//     refresh, moving as stepped; a run's ends are single samples, so one vblank seen late reads as no answer now and
//     then, not twice running);
//   - too far: the display's drift against the game's clock (the phase as if the lock never stepped, its slope over
//     kDriftFields between medians of kDriftEnds fields at either end) over the line, kFarSpans spans in a row the same
//     way (~5 s; at one refresh a field also one span over 0.8%): let go, in step or not. The line is kSlowLineNs (0.2%) where following holds
//     the game slower and kFastLineNs (0.5%) where it holds it faster: the sound's rate control is proportional, so a
//     slower game drains the queue toward its end (hitches or a slow PC on top then cut) while a faster one fills it,
//     which has room (ps2_audio.cpp: 341 ms, the rate control's 0.5% reached at 170) (the release-only lock follows those too: 60.2 Hz
//     is 0.43%, 60.000 Hz 0.1%). On a display that follows the game this reads as what the lock has been dragging the clock by;
//   - while the lock is not following, a field with no multiple (a variable-refresh display's measured refresh is the
//     game's own pace, past 2% now and then with uneven frames) does not break the judging: its times count, no step.
// A release lasts as the release-only lock's does, until the display is measured afresh (a gap in the measurement: a monitor switch,
// which host_vsync.cpp measures from the start, or no vblank seen for 100 ms), and Mode::Checked also tries again every
// kRetryFields (2 minutes: a release that was wrong is not kept for long; on a display that follows the game a retry
// costs a few seconds of the lock's pull, so a retry let go again within 30 s doubles the interval, to 16 minutes).

#include <cstdint>

namespace ps2x
{
    class VsyncLock
    {
    public:
        static constexpr int64_t kEmulatedFieldNs = 16683400; // NTSC field (1001/60000 s)
        static constexpr int64_t kMaxStepNs = 250000;
        static constexpr int kReleaseFields = 120; // one judged window (2 s)
        static constexpr int kSettleFields = 40;   // not judged after a re-anchor
        static constexpr int64_t kReleaseErrorNs = 8 * (kEmulatedFieldNs * 4 / 1000); // a steady 0.4% step's error
        static constexpr int kStuckFields = 40;
        static constexpr int kStuckMisses = 2; // not converging: runs in a row without the answer
        static constexpr int kDriftFields = 96; // too far: the drift's span (~1.6 s)
        static constexpr int kDriftEnds = 16;   // too far: fields whose median is taken at either end of a span
        static constexpr int64_t kSlowLineNs = kEmulatedFieldNs * 2 / 1000; // too far, holding the game slower: 0.2%
        static constexpr int64_t kFastLineNs = kEmulatedFieldNs * 5 / 1000; // too far, holding it faster: 0.5%
        static constexpr int kFarSpans = 3;      // too far: spans in a row over the line that let go
        static constexpr int kPendingFields = 8; // a late vblank's movement held at most this long
        static constexpr int64_t kMaxJumpNs = 1000000000; // a re-anchor larger than this starts the judging over
        static constexpr int kRetryFields = 7200;         // released: tried again after 2 minutes
        static constexpr int kRetryHeldFields = 1800;     // a retry let go again within 30 s doubles the next interval
        static constexpr int kMaxRetryBackoff = 3;        // up to kRetryFields << 3 (16 minutes)

        enum class Mode
        {
            Checked,           // the lock as used
            Uncapped,          // 3d97a84's release-only lock exactly (tests)
            UncappedNoRelease, // the lock of issue #1 (tests: shows what the release prevents)
        };

        // What one call found, for the caller's log lines.
        struct Report
        {
            bool displayChanged = false;  // the refreshes per field changed (or were measured afresh): `multiple`
            int64_t multiple = 0;         // host refreshes per emulated field; 0 = no whole multiple close enough
            bool released = false;        // the lock let go this call
            double releasedPercent = 0.0; // how far it was moving the game's clock when it let go
            bool releasedLater = false;   // which way (later = the game was running slow)
            const char *why = "";         // why it let go
            bool locked = false;          // the deadline was adjusted (error below is valid)
            int64_t errorNs = 0;          // phase error this field (locked only)
        };

        // phase60Ns: where the emulated vblank sits after the host vblank on a 60 Hz display (PS2X_VSYNC_PHASE_MS).
        explicit VsyncLock(double phase60Ns = 7.0e6, Mode mode = Mode::Checked) : m_phase60Ns(phase60Ns), m_mode(mode) {}

        // The deadline for the next emulated vblank. targetNs: the one the scheduler would use (this field's start +
        // one field). haveVblank: the host display is measured (lastVblankNs, periodNs valid); false (another monitor,
        // or none seen for a while) undoes a release: the display is measured afresh.
        int64_t next(int64_t targetNs, bool haveVblank, int64_t lastVblankNs, int64_t periodNs, Report &report);

        bool released() const { return m_released; }

    private:
        void release(Report &report, const char *why, int64_t meanStepNs);
        // Mode::Checked's part of a field; false = released.
        bool checkedField(int64_t targetNs, int64_t lastVblankNs, int64_t periodNs, bool hadLast, Report &report);
        // The phase's free movement this field (freeNs; 0 while a late vblank's is held); false: dropped, ambiguous.
        bool freeMove(int64_t targetNs, int64_t lastVblankNs, int64_t periodNs, int64_t jumpNs, int64_t &freeNs);
        // Adds a field's free movement; true, with the drift per field, when a span is complete.
        bool driftSpan(int64_t freeNs, int64_t &driftNs);

        double m_phase60Ns;
        Mode m_mode;
        bool m_released = false;
        bool m_wasReleased = false;
        int m_releasedFields = 0; // Checked: fields since the release (kRetryFields)
        int m_retryBackoff = 0;   // Checked: retry interval doublings
        int m_sinceRetry = kRetryHeldFields; // Checked: locked fields since the last retry (capped)
        int64_t m_reportedMultiple = -1;
        int64_t m_windowStepNs = 0;
        int64_t m_lastDeadlineNs = 0;
        int m_windowFields = 0;
        int m_windowInStep = 0;
        int m_windows = 0;
        int m_settle = 0;
        // Checked: the last field's deadline asked for, host vblank and step (m_haveLast: there was one, consecutive).
        bool m_haveLast = false;
        int64_t m_lastTargetNs = 0;
        int64_t m_lastVblankNs = 0;
        int64_t m_lastStepNs = 0;
        // Not converging: fields in a row at the largest step, what they stepped, how far the phase moved.
        int m_stuck = 0;
        int64_t m_stuckStepNs = 0;
        int64_t m_stuckMoveNs = 0;
        int m_stuckMisses = 0;
        // Too far: the phase as if the lock never stepped over the span so far, a late vblank's held movement, spans in
        // a row over the line and the last one's drift.
        int64_t m_freeNs = 0;
        int64_t m_pendingNs = 0;
        int m_pendingFields = 0;
        int m_driftFields = 0;
        int64_t m_driftStart[kDriftEnds] = {}, m_driftEnd[kDriftEnds] = {};
        int m_farSpans = 0;
        int64_t m_lastDriftNs = 0;
    };
}
