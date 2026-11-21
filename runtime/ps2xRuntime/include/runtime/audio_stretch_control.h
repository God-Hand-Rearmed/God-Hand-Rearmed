#pragma once

// When the audio goes through the time-stretch (ps2_audio.cpp): only while the game runs below full speed.
//
// The emulated SPU2 makes 48000 frames per emulated second. When the PC cannot keep up, the scheduler re-anchors late
// vblanks (EeScheduler.cpp: hostStart = max(deadline, now - 2 ms)) and the real time past the deadline is emulated time
// lost for good, and that much less audio. The sound's rate control absorbs 0.5%; below that the queue runs dry and the
// music cuts out every second or so (issue #5: 60 FPS on a 4-core Haswell, 97-99% speed). The stretch plays the audio
// slower instead (WSOLA, pitch kept).
//
// The speed is measured exactly: 1 - (emulated time lost) / (real time) over 1 s windows, from the scheduler's own
// count (ps2xEmulatedTimeLostNs). The old estimate (audio frames received per window) moved in 512-frame steps, ~2% of
// a 0.5 s window, so it could only switch on below 97%. A single drop over 50 ms is a stall (loading, a shader
// compile): a gap the stretch cannot help, not counted (the old stretch switched on for seconds after a load hitch and
// played every attack twice; that is why GHR had it off).
//
// On: two windows in a row (2 s) below 99.5%, the last kAverageWindows (5 s) below it on average too, with the playable
// queue below its target (the rate control losing). Off: the last 5 s above 99.8% on average and the queue back within
// 25% of its target (the stretch drains a long queue at up to 5%; the fine rate control would take minutes). Between
// 99.5% and 99.8% it stays as it is (hysteresis: no switching back and forth on a game at the edge). The average, not
// each window: losses that come in bursts (a few late fields, 99.8% on the whole, which the rate control keeps up with)
// put a window or two under 99.5% now and then, and kept every 5 s streak from completing: the stretch came on and
// stayed on at full speed.
// queueFill is the playable queue's (ps2_audio.cpp: what the sound card will play, not the stretcher's own input,
// which it holds back 23-55 ms): steering the sum let the playable part sit at 30-60 ms and run dry.

#include <cstdint>

// EeScheduler.cpp: the emulated time lost so far to late vblanks, single drops over 50 ms (stalls) left out.
int64_t ps2xEmulatedTimeLostNs() noexcept;

namespace ps2x
{
    class StretchControl
    {
    public:
        static constexpr int64_t kWindowNs = 1000000000; // 1 s: a few heavy frames in a row are not a slowdown
        static constexpr double kOnBelow = 0.995;
        static constexpr double kOffAbove = 0.998;
        static constexpr int kWindowsToSwitch = 2;
        static constexpr int kAverageWindows = 5; // on and off are judged on the last 5 s's mean speed too (each
                                                  // switch-on costs a 55 ms hole: no flicker at the edge)

        // Feed the time now and the lost-time count now; once a window is complete the decision is updated.
        // queueFill: (queued - target) / target. Returns true while the audio should go through the stretch.
        bool update(int64_t nowNs, int64_t lostNs, double queueFill)
        {
            if (m_windowStartNs == 0)
            {
                m_windowStartNs = nowNs;
                m_lostAtStartNs = lostNs;
                return m_stretching;
            }
            const int64_t windowNs = nowNs - m_windowStartNs;
            if (windowNs < kWindowNs)
                return m_stretching;
            const double windowSpeed =
                1.0 - static_cast<double>(lostNs - m_lostAtStartNs) / static_cast<double>(windowNs);
            m_windowStartNs = nowNs;
            m_lostAtStartNs = lostNs;
            m_speed = windowSpeed < 0.0 ? 0.0 : windowSpeed;
            m_slowWindows = m_speed < kOnBelow ? m_slowWindows + 1 : 0;
            m_recent[m_recentAt] = m_speed;
            m_recentAt = (m_recentAt + 1) % kAverageWindows;
            m_recentCount = m_recentCount < kAverageWindows ? m_recentCount + 1 : kAverageWindows;
            // (windows not seen yet, at the start or after a stall, count as full speed: two slow windows then decide
            // only if they are slow enough to pull the 5 s under the line: 98% does, a burst at 99.4% does not)
            double sum = static_cast<double>(kAverageWindows - m_recentCount);
            for (int i = 0; i < m_recentCount; ++i)
                sum += m_recent[i];
            m_averageSpeed = sum / kAverageWindows;
            // On only while the playable queue is also below its target: the rate control is losing. A game a little
            // under 99.5% with the queue still full is being kept up with, and the stretch's switch-on costs a 55 ms hole
            // (it holds a piece and its splice search before its first output): switching on there made cuts.
            if (!m_stretching && m_slowWindows >= kWindowsToSwitch && m_averageSpeed < kOnBelow && queueFill < 0.0)
                m_stretching = true;
            else if (m_stretching && m_recentCount == kAverageWindows && m_averageSpeed > kOffAbove && queueFill > -0.25 &&
                     queueFill < 0.25)
                m_stretching = false;
            return m_stretching;
        }

        // A stall (no audio for over 50 ms) restarts the window and breaks a streak of slow windows: it says nothing
        // about a sustained speed. The windows before it stay in the average (clearing it too, stalls every few seconds
        // kept the 5 s from ever filling, and a stretch that was on from ever going off).
        void restartWindow()
        {
            m_windowStartNs = 0;
            m_slowWindows = 0;
        }

        bool stretching() const { return m_stretching; }
        double speed() const { return m_speed; } // the last window's speed (the stretch's base tempo)

        void reset() { *this = StretchControl{}; }

    private:
        int64_t m_windowStartNs = 0;
        int64_t m_lostAtStartNs = 0;
        double m_speed = 1.0;
        int m_slowWindows = 0;
        double m_recent[kAverageWindows] = {};
        int m_recentAt = 0, m_recentCount = 0;
        double m_averageSpeed = 1.0;
        bool m_stretching = false;
    };
}
