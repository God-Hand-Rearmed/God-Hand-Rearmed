// The emulated vblank's lock to the host display: the arithmetic of EeScheduler.cpp lockToHostVsync, with its state in
// an object so test/vsync_lock_test.cpp can run it on simulated displays. vsync_lock.h says what Mode::Checked adds to
// the release-only lock (Mode::Uncapped) and why.
#include "runtime/vsync_lock.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>

namespace ps2x
{
    namespace
    {
        // The median of N samples (a sorted copy).
        template <size_t N> int64_t medianOf(const int64_t (&values)[N])
        {
            int64_t sorted[N];
            std::copy(std::begin(values), std::end(values), sorted);
            std::sort(std::begin(sorted), std::end(sorted));
            return (sorted[(N - 1) / 2] + sorted[N / 2]) / 2;
        }

        // A phase difference, wrapped to half a refresh either way.
        int64_t wrapped(int64_t ns, int64_t periodNs)
        {
            ns %= periodNs;
            if (ns > periodNs / 2)
                ns -= periodNs;
            else if (ns < -periodNs / 2)
                ns += periodNs;
            return ns;
        }
    }

    void VsyncLock::release(Report &report, const char *why, int64_t meanStepNs)
    {
        // let go again soon after a retry: that display does follow the game, try it less often (each retry costs a
        // few seconds of the lock's pull); one that held for a while starts from kRetryFields again
        if (m_mode == Mode::Checked)
            m_retryBackoff = m_sinceRetry < kRetryHeldFields ? std::min(m_retryBackoff + 1, kMaxRetryBackoff) : 0;
        m_sinceRetry = kRetryHeldFields;
        m_released = true;
        m_wasReleased = true;
        m_releasedFields = 0;
        report.released = true;
        report.why = why;
        report.releasedLater = meanStepNs > 0;
        report.releasedPercent = 100.0 * std::abs(static_cast<double>(meanStepNs)) / static_cast<double>(kEmulatedFieldNs);
    }

    int64_t VsyncLock::next(int64_t targetNs, bool haveVblank, int64_t lastVblankNs, int64_t periodNs, Report &report)
    {
        report = Report{};
        const bool checked = m_mode == Mode::Checked;
        // Checked: whether the last call locked (its error and step are the ones before this field's).
        const bool hadLast = m_haveLast;
        m_haveLast = false;
        if (!haveVblank)
        {
            // measured afresh next (a monitor switch, the measurement starting, a stall or a loading screen): lock again
            m_released = false;
            return targetNs;
        }
        if (m_released)
        {
            // Checked: tried again every kRetryFields (a release that was wrong, or a display that changed under it, is
            // not kept for long; on one that follows the game it costs ~1.4 s of letting go again)
            if (!checked || ++m_releasedFields < (kRetryFields << m_retryBackoff))
                return targetNs;
            m_sinceRetry = 0;
            m_released = false; // (hadLast is false: a released call never locks)
        }
        // Refreshes per emulated field: 1 on a 60 Hz display, 2/3/4 at 120/180/240 Hz. 0 = no whole multiple within 2%.
        int64_t multiple = 0;
        for (int64_t n = 1; n <= 8 && multiple == 0; ++n)
            if (periodNs * n >= kEmulatedFieldNs * 98 / 100 && periodNs * n <= kEmulatedFieldNs * 102 / 100)
                multiple = n;
        // A deadline that is not the last one + one field: the scheduler re-anchored (a late field). The lock is pulling
        // the phase back for a while then, so those fields are not judged.
        if (m_lastDeadlineNs != 0 && std::abs(targetNs - m_lastDeadlineNs - kEmulatedFieldNs) > 1000000)
            m_settle = kSettleFields;
        if (m_wasReleased)
        {
            // measured afresh after a release: judge from the start again, and report the display again
            m_wasReleased = false;
            m_reportedMultiple = -1;
            m_windowStepNs = 0;
            m_windowFields = 0;
            m_windowInStep = 0;
            m_windows = 0;
            m_settle = 0;
        }
        report.multiple = multiple;
        if (multiple != m_reportedMultiple)
        {
            m_reportedMultiple = multiple;
            report.displayChanged = true;
        }
        if (multiple == 0)
        {
            // Checked, locked a field ago: a moment of no multiple (a variable-refresh display's uneven pace) does not
            // break the judging: this field's times count with no step.
            if (checked && hadLast)
            {
                if (!checkedField(targetNs, lastVblankNs, periodNs, hadLast, report))
                    return targetNs;
                m_lastDeadlineNs = targetNs;
                m_haveLast = true;
                m_lastTargetNs = targetNs;
                m_lastVblankNs = lastVblankNs;
                m_lastStepNs = 0;
            }
            return targetNs;
        }
        // Where the emulated vblank sits in the host refresh: the 60 Hz phase, as the same fraction of a shorter refresh.
        const int64_t targetPhaseNs = static_cast<int64_t>(m_phase60Ns * static_cast<double>(periodNs) / static_cast<double>(kEmulatedFieldNs));
        int64_t phase = (targetNs - lastVblankNs) % periodNs;
        if (phase < 0)
            phase += periodNs;
        int64_t error = phase - targetPhaseNs;
        if (error > periodNs / 2)
            error -= periodNs;
        else if (error < -periodNs / 2)
            error += periodNs;
        int64_t step = std::clamp<int64_t>(-error / 8, -kMaxStepNs, kMaxStepNs);
        if (checked && !checkedField(targetNs, lastVblankNs, periodNs, hadLast, report))
            return targetNs; // released
        if (m_settle > 0)
            --m_settle;
        else
        {
            m_windowStepNs += step;
            m_windowInStep += std::abs(error) <= kReleaseErrorNs ? 1 : 0;
            ++m_windowFields;
        }
        if (m_windowFields == kReleaseFields)
        {
            const int64_t meanNs = m_windowStepNs / kReleaseFields;
            if (++m_windows > 1 && m_windowInStep * 10 < kReleaseFields && m_mode != Mode::UncappedNoRelease)
                release(report, "out of step", meanNs);
            m_windowStepNs = 0;
            m_windowFields = 0;
            m_windowInStep = 0;
        }
        m_lastDeadlineNs = targetNs + step;
        if (checked && m_sinceRetry < kRetryHeldFields)
            ++m_sinceRetry;
        m_haveLast = checked && !m_released;
        m_lastTargetNs = targetNs;
        m_lastVblankNs = lastVblankNs;
        m_lastStepNs = step;
        report.locked = true;
        report.errorNs = error;
        return targetNs + step;
    }

    // Mode::Checked's additions (vsync_lock.h). Both ask one question: does the display's phase answer the lock's steps?
    // On a display the lock follows, the phase moves each field by the step taken plus the drift of the two rates; on
    // one that follows the game's presents it does not move. "The phase" is net of the scheduler's re-anchors (a
    // deadline later than the last one + a field moves it by as much on any display), wrapped to half a refresh.
    bool VsyncLock::freeMove(int64_t targetNs, int64_t lastVblankNs, int64_t periodNs, int64_t jumpNs, int64_t &freeNs)
    {
        freeNs = 0;
        m_pendingNs += targetNs - m_lastTargetNs - jumpNs - (lastVblankNs - m_lastVblankNs) - m_lastStepNs;
        if (std::abs(wrapped(m_pendingNs, periodNs)) > periodNs / 3)
        {
            // still ambiguous after kPendingFields (the vblank thread's latency in a long tail, the measured refresh
            // off with it): no answer is right, and taking one wrapped a whole refresh the wrong way, read as 0.9% of
            // drift: false: dropped, the judging starts over
            if (++m_pendingFields < kPendingFields)
                return true;
            m_pendingNs = 0;
            m_pendingFields = 0;
            return false;
        }
        freeNs = wrapped(m_pendingNs, periodNs);
        m_pendingNs = 0;
        m_pendingFields = 0;
        return true;
    }

    // The drift's span: adds a field's free movement; true with the drift per field when a span is complete.
    bool VsyncLock::driftSpan(int64_t freeNs, int64_t &driftNs)
    {
        // (the slope between the medians of kDriftEnds fields at the span's two ends: the free phase telescopes, so only
        // the ends' measurement noise is in it, and a single sample at an end, a vblank seen a few ms late, read as
        // 0.2% over the span; a median does not move for it, and the latency's own mean is at both ends alike)
        m_freeNs += freeNs;
        if (m_driftFields < kDriftEnds)
            m_driftStart[m_driftFields] = m_freeNs;
        else if (m_driftFields >= kDriftFields)
            m_driftEnd[m_driftFields - kDriftFields] = m_freeNs;
        if (++m_driftFields < kDriftFields + kDriftEnds)
            return false;
        driftNs = (medianOf(m_driftEnd) - medianOf(m_driftStart)) / kDriftFields;
        m_freeNs = 0;
        m_driftFields = 0;
        return true;
    }

    bool VsyncLock::checkedField(int64_t targetNs, int64_t lastVblankNs, int64_t periodNs, bool hadLast, Report &report)
    {
        const int64_t jumpNs = targetNs - m_lastDeadlineNs - kEmulatedFieldNs;
        // (a re-anchor of any size is taken out exactly; a gap in the locked fields is not consecutive: hadLast)
        int64_t freeNs = 0;
        const bool consecutive = hadLast && std::abs(jumpNs) < kMaxJumpNs && freeMove(targetNs, lastVblankNs, periodNs, jumpNs, freeNs);
        if (!consecutive)
        {
            m_stuckMisses = 0;
            m_stuck = 0;
            m_stuckStepNs = 0;
            m_stuckMoveNs = 0;
            m_driftFields = 0;
            m_farSpans = 0;
            m_freeNs = 0;
            m_pendingNs = 0;
            m_pendingFields = 0;
            return true;
        }
        // The phase's free movement this field (what it moved less what the lock stepped: the drift of the two rates,
        // plus the measurement's jitter), from the raw times rather than from the two errors: the host vblanks between
        // the fields are a whole number of true refreshes, so the measured period (still settling at first, or a little
        // off) enters only on a field where that number changes, not on every field as a difference of two errors taken
        // modulo it would. The whole refreshes are taken off (wrapped) only once the sum is clearly near a whole number
        // of them: a vblank seen late (by up to half a refresh) is held with the next field's, which is early by as
        // much, instead of wrapping the wrong way and leaving a refresh's step behind.
        const int64_t moveNs = freeNs + m_lastStepNs;

        // Not converging: kStuckFields fields in a row at the largest step (either way: the error of a display that
        // follows the game often sits near half a refresh, where jitter or a hitch flips its side) over which the phase
        // did not move by 30% of what they stepped (a display it follows moves by all of it; on a slow PC the vblank
        // measurement's latency blurs that, at 0.5 a run now and then fell short).
        if (std::abs(m_lastStepNs) == kMaxStepNs)
        {
            ++m_stuck;
            m_stuckStepNs += m_lastStepNs;
            m_stuckMoveNs += moveNs;
            if (m_stuck >= kStuckFields)
            {
                const double stepped = static_cast<double>(m_stuckStepNs), moved = static_cast<double>(m_stuckMoveNs);
                // (only a run that stepped mostly one way: one that changed sides nets too little to tell an answer from
                // the measurement's noise)
                // two runs in a row without the answer: a run's ends are single samples, and a vblank seen a few ms late
                // at one of them reads as no answer once in a while (not twice running)
                if (std::abs(m_stuckStepNs) >= kStuckFields * kMaxStepNs / 2 && moved * stepped < 0.3 * stepped * stepped)
                {
                    if (++m_stuckMisses >= kStuckMisses)
                    {
                        release(report, "not converging", m_stuckStepNs / kStuckFields);
                        return false;
                    }
                }
                else
                    m_stuckMisses = 0;
                m_stuck = 0;
                m_stuckStepNs = 0;
                m_stuckMoveNs = 0;
            }
        }
        else
        {
            m_stuck = 0;
            m_stuckStepNs = 0;
            m_stuckMoveNs = 0;
        }

        // Too far: the display's drift against the game's clock, per field (driftSpan: the phase as if the lock never
        // stepped). Following it moves the game's clock by as much, and the lines are not the same both ways: the sound's
        // rate control is proportional, so a game held slower drains the queue toward its end (hitches or a slow PC on
        // top then cut), one held faster fills it, which has room until the rate control's 0.5% gives out. Slower (the
        // drift negative: the lock steps later) over kSlowLineNs (0.2%), faster over kFastLineNs (0.5%; 60.000 Hz is
        // 0.1%, 60.2 Hz 0.43%), kFarSpans spans in a row the same way (~5 s), or one span over 0.8% at one refresh a field: let go. On a
        // display that follows the game this reads as what the lock has been dragging the clock by.
        int64_t driftNs = 0;
        if (driftSpan(freeNs, driftNs))
        {
            const int64_t line = driftNs < 0 ? kSlowLineNs : kFastLineNs;
            const bool over = std::abs(driftNs) > line;
            m_farSpans = over && (m_farSpans == 0 || (driftNs > 0) == (m_lastDriftNs > 0)) ? m_farSpans + 1 : (over ? 1 : 0);
            m_lastDriftNs = driftNs;
            // or one span over 0.8%, where a refresh is a whole field (a display that follows the game measures at its
            // pace: one); at 120 / 240 Hz no single span decides: there a latency tail of a few ms can wrap a whole
            // (short) refresh the wrong way, ~1% over one span
            if (m_farSpans >= kFarSpans || (std::abs(driftNs) > 4 * kSlowLineNs && periodNs > kEmulatedFieldNs * 3 / 4))
            {
                release(report, "too far from 59.94 Hz", -driftNs);
                return false;
            }
        }
        return true;
    }
}
