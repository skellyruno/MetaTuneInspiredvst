#include "CorrectionEngine.h"
#include "PitchMath.h"
#include <algorithm>
#include <cmath>

namespace lyra
{
static double onePoleAlpha (double seconds, double sr) noexcept
{
    return seconds > 0 ? 1.0 - std::exp (-1.0 / (seconds * sr)) : 1.0;
}

// Per-stage coefficient of the critically damped pair that reaches 63.2 % at `ms`.
static double criticalAlpha (double ms, double sr) noexcept
{
    return onePoleAlpha (ms / 1000.0 / CorrectionEngine::kCriticalTauFactor, sr);
}

void CorrectionEngine::prepare (double sampleRate, double estimateIntervalSeconds)
{
    sr = sampleRate;
    dt = estimateIntervalSeconds > 0 ? estimateIntervalSeconds : 0.002;
    updateCoefficients();
    reset();
}

void CorrectionEngine::reset() noexcept
{
    selector.reset();
    c = c1 = inMidi = 0;
    prevMidi = vel = velSmooth = lead = sinceEst = 0;
    steadyHops = 0;
    noteAge = candidateAge = now = 0;
    unvoicedTime = 1.0;
    active = onsetNote = false;
    restartVibrato();
    target = confirmed = candidate = TargetSelector::kNoTarget;
    alpha = baseAlpha;
    glideMidi = 0.0;
    glideValid = false;
    octaveCandidateMidi = 0.0;
    octaveCandidateHops = 0;
    motionDirection = motionRunHops = 0;
    correctionLimitCurrent = p.maxCorrectionSemitones;
}

void CorrectionEngine::setParams (const Params& np) noexcept
{
    Params q = np;
    q.speedMs = std::isfinite (q.speedMs) ? std::clamp (q.speedMs, -3.0, 1000.0) : 20.0;
    q.amount = std::isfinite (q.amount) ? std::clamp (q.amount, 0.0, 1.0) : 1.0;
    q.referenceHz = std::isfinite (q.referenceHz) ? std::clamp (q.referenceHz, 400.0, 480.0) : 440.0;
    q.stabilizerMs = std::isfinite (q.stabilizerMs) ? std::clamp (q.stabilizerMs, 0.0, 1000.0) : 0.0;
    q.sustain = std::isfinite (q.sustain) ? std::clamp (q.sustain, -1.0, 1.0) : 0.0;
    q.glideMs = std::isfinite(q.glideMs) ? std::clamp(q.glideMs, 0.0, 500.0) : 0.0;
    q.maxCorrectionSemitones = std::isfinite(q.maxCorrectionSemitones)
        ? std::clamp(q.maxCorrectionSemitones, 0.0, 12.0) : 12.0;
    q.consonantReleaseMs = std::isfinite(q.consonantReleaseMs)
        ? std::clamp(q.consonantReleaseMs, 0.0, 30.0) : 8.0;
    const bool coeffs = q.speedMs != p.speedMs
        || q.skellLive != p.skellLive
        || q.hysteresisCents != p.hysteresisCents
        || q.glideMs != p.glideMs
        || q.consonantReleaseMs != p.consonantReleaseMs;
    // A mode change cannot carry a half-confirmed octave hypothesis.
    if (q.skellLive != p.skellLive)
    {
        octaveCandidateHops = 0;
        motionDirection = motionRunHops = 0;
    }
    p = q;
    selector.setMask (p.mask);
    if (coeffs)
        updateCoefficients();
}

void CorrectionEngine::updateCoefficients() noexcept
{
    baseAlpha = alpha = criticalAlpha (p.speedMs, sr);
    // Consonant Release is independent of retune Speed: fast sung notes may
    // remain locked while a short "s", "t" or breath returns to natural pitch.
    // 0ms selects the fastest click-safe response (1ms minimum), rather
    // than an abrupt single-sample pitch-ratio jump on a consonant.
    // 8ms is a gentle 2-stage fade; negative hard-tune remains instant
    // on voiced notes.
    releaseAlpha = criticalAlpha (std::max(1.0, p.consonantReleaseMs), sr);
    glideAlpha = onePoleAlpha(p.glideMs / 1000.0, sr);
    correctionLimitAlpha = onePoleAlpha(0.004, sr);
    baseHyst = std::max (0.0, p.speedMs < 0 ? p.hysteresisCents * (1.0 + p.speedMs / 3.0) : p.hysteresisCents);
    selector.setHysteresisCents (baseHyst);
}

void CorrectionEngine::analyseVibrato() noexcept
{
    const double turn = kVibratoTurnCents / 100.0;
    if (turnDir == 0) { turnDir = vel < 0 ? -1 : 1; runExt = inMidi; runExtT = now; }
    if ((inMidi - runExt) * turnDir > 0) { runExt = inMidi; runExtT = now; } // extreme still growing
    else if ((runExt - inMidi) * turnDir >= turn)
    {
        // Confirmed turning point: shift it in, then look for the opposite one.
        ext[0] = ext[1]; ext[1] = ext[2]; ext[2] = runExt;
        extT[0] = extT[1]; extT[1] = extT[2]; extT[2] = runExtT;
        extCount = std::min (extCount + 1, 3);
        turnDir = -turnDir;
        runExt = inMidi; runExtT = now;
    }

    vibOn = false;
    if (extCount < 3)
        return;
    const double h1 = extT[1] - extT[0], h2 = extT[2] - extT[1];
    const double s1 = std::abs (ext[1] - ext[0]), s2 = std::abs (ext[2] - ext[1]);
    auto halfCycleOk = [] (double h) { return h >= 0.035 && h <= 0.170; };
    auto swingOk = [] (double s) { return s >= 0.16 && s <= 2.0; };
    // Similar swings: a note change reads as one short swing then a long one, never as vibrato.
    if (! (halfCycleOk (h1) && halfCycleOk (h2) && swingOk (s1) && swingOk (s2) && std::max (s1, s2) <= 1.5 * std::min (s1, s2)
           && now - extT[2] <= 0.170))
        return;
    vibDepth = 0.5 * std::min (s1, s2);
    vibCentre = 0.25 * (ext[0] + 2.0 * ext[1] + ext[2]);
    vibOn = std::abs (inMidi - vibCentre) <= 1.5 * vibDepth + 0.10; // pitch left the band: centre is stale
}

void CorrectionEngine::onEstimate (const PitchEstimate& e, int samplesBeforeRatio) noexcept
{
    active = false;
    inMidi = 0;
    now += dt;
    sinceEst = -samplesBeforeRatio / sr;
    const bool voiced = e.voiced && e.confidence >= p.minConfidence && e.frequency > 0;
    if (! voiced)
    {
        unvoicedTime += dt;
        octaveCandidateHops = 0;
        motionDirection = motionRunHops = 0;
        prevMidi = vel = velSmooth = 0;
        steadyHops = 0;
        restartVibrato();
        if (unvoicedTime >= kOnsetGapSeconds)
        {
            confirmed = TargetSelector::kNoTarget; // phrase ended: the next note is an onset
            glideValid = false; // no glide from a different vocal phrase
        }
        candidate = TargetSelector::kNoTarget;
        candidateAge = 0;
    }
    else
    {
        const double priorGap = unvoicedTime;
        unvoicedTime = 0;
        inMidi = freqToMidi (e.frequency, p.referenceHz);

        // Skell Live rejects a one-hop 2x/0.5x harmonic alias even at
        // negative Speed, where hysteresis intentionally falls to zero.
        // A *real* octave leap survives two successive measurements and is
        // accepted at the next detector hop (usually 2ms). Non-octave
        // intervals, phrase onsets and continuous pitch motion are immediate.
        if (p.skellLive && prevMidi > 0 && priorGap < 0.025
            && std::abs(std::abs(inMidi - prevMidi) - 12.0) < 0.65)
        {
            if (octaveCandidateHops > 0
                && std::abs(inMidi - octaveCandidateMidi) < 0.35)
                ++octaveCandidateHops;
            else
            {
                octaveCandidateMidi = inMidi;
                octaveCandidateHops = 1;
            }
            if (octaveCandidateHops < 2)
                inMidi = prevMidi; // neutral first frame; no false note hop
            else
                octaveCandidateHops = 0;
        }
        else
            octaveCandidateHops = 0;

        const double step = inMidi - prevMidi;
        const bool continuous = prevMidi > 0 && std::abs (step) <= kJumpSemitones;
        vel = continuous ? std::clamp (step / dt, -kMaxVelocity, kMaxVelocity) : 0.0;
        velSmooth = continuous ? (steadyHops == 0 ? vel : velSmooth + 0.25 * (vel - velSmooth)) : 0.0; // ~8 ms average
        steadyHops = continuous ? steadyHops + 1 : 0;
        // A fast melodic run keeps moving in the SAME direction for at
        // least two YIN hops. Alternating vibrato and jitter do not count
        // as a reliable note-change trajectory.
        const int direction = continuous && std::abs(vel) > 5.0
            ? (vel > 0.0 ? 1 : -1) : 0;
        if (p.skellLive && direction != 0)
        {
            motionRunHops = direction == motionDirection
                ? std::min(motionRunHops + 1, 32) : 1;
            motionDirection = direction;
        }
        else
            motionDirection = motionRunHops = 0;
        prevMidi = inMidi;
        lead = winSec > 0 ? std::clamp (0.5 * (winSec + 1.0 / e.frequency) + 1e-4 - delaySec, 0.0, kMaxLeadSeconds) : 0.0;
        if (! continuous)
            restartVibrato();
        analyseVibrato();
        // Skell Live is a distinct performance-controller mode rather
        // than an alternate shifter. Near a fast note boundary the hold
        // shrinks and a bounded 2.5ms slope projection acquires the next
        // note sooner. On sustained/vibrato notes it retains hysteresis to
        // prevent a flurry of note-flips. Negative retune keeps the original
        // immediate zero-hysteresis sound, but still benefits from the
        // octave guard above and lower confidence gating.
        double holdCents = baseHyst;
        double noteForSelection = inMidi;
        if (p.skellLive && !vibOn && p.speedMs >= 0.0)
        {
            const bool movingWithEvidence = continuous && motionRunHops >= 2
                && e.confidence > 0.80f && std::abs(velSmooth) > 5.0;
            // Stronger-than-normal 18c lock on steady notes; open to 3c
            // during real monotonic movement. Skell Live also instantly
            // handles abrupt non-octave jumps (no note-confirmation hold).
            holdCents = movingWithEvidence ? std::min(baseHyst, 3.0)
                                           : std::max(baseHyst, 18.0);
            if (movingWithEvidence)
                noteForSelection += std::clamp(velSmooth * 0.0025, -0.16, 0.16);
        }
        const double h = vibOn && p.hysteresisCents > 0
            ? holdCents * (1.0 + 0.5 * 100.0 * vibDepth / p.hysteresisCents)
            : holdCents;
        selector.setHysteresisCents(
            std::min(h, std::max(holdCents, kVibratoMaxHysteresisCents)));

        const int n = selector.select (noteForSelection);
        if (n == TargetSelector::kNoTarget)
        {
            confirmed = candidate = TargetSelector::kNoTarget;
        }
        else if (p.stabilizerMs <= 0 || confirmed == TargetSelector::kNoTarget || n == confirmed)
        {
            if (n != confirmed) noteAge = 0;
            onsetNote = n != confirmed ? confirmed == TargetSelector::kNoTarget : onsetNote;
            confirmed = n;
            candidate = TargetSelector::kNoTarget;
            candidateAge = 0;
            active = true;
        }
        else
        {
            if (n != candidate) { candidate = n; candidateAge = 0; }
            candidateAge += dt;
            if (candidateAge * 1000.0 >= p.stabilizerMs - 1e-9)
            {
                confirmed = n;
                candidate = TargetSelector::kNoTarget;
                candidateAge = 0;
                noteAge = 0;
                onsetNote = false;
                active = true;
            }
        }
    }

    target = active ? confirmed : TargetSelector::kNoTarget;
    if (active)
    {
        noteAge += dt;
        // The first note of a phrase starts in tune; only subsequent
        // confirmed note-to-note transitions slide between targets.
        if (!glideValid)
        {
            glideMidi = double(target);
            glideValid = true;
        }
    }

    // Effective Speed: Sustain (by note age), then the steady-onset shortcut.
    alpha = baseAlpha;
    flatten = 1.0;
    if (p.speedMs > 0.0 && active)
    {
        double t = p.speedMs;
        if (p.sustain != 0.0)
            t = std::clamp (t * std::exp2 (-3.0 * p.sustain * std::min (1.0, noteAge / kSustainRampSeconds)), 0.05, 3000.0);
        if (onsetNote && noteAge <= kOnsetWindowSeconds && steadyHops >= 2 && std::abs (velSmooth) < kSteadyVelocity)
            t = std::min (t, kOnsetMs);
        if (t != p.speedMs)
            alpha = criticalAlpha (t, sr);
        flatten = std::exp (-t / kVibratoKeepMs);
    }
}

float CorrectionEngine::nextRatio() noexcept
{
    // Slide the *confirmed target note*, not noisy detector measurements.
    // Disabled by default; negative Speed still remains sample-accurate
    // instant tuning when Glide is 0ms.
    double u = 0.0;
    if (active)
    {
        glideMidi += glideAlpha * (double(target) - glideMidi);
        // Pitch of the audio being shifted now (predicted forward), with the kept vibrato taken out.
        double x = inMidi + std::clamp (vel * std::min (lead + sinceEst, kMaxLeadSeconds), -kMaxPredict, kMaxPredict);
        if (vibOn)
            x = vibCentre + flatten * (x - vibCentre);
        u = (glideMidi - x) * p.amount;
    }
    sinceEst += 1.0 / sr;
    // At automation changes, ramp the allowed magnitude over 4ms instead
    // of clipping from e.g. 12 semitones to 0 in one audio sample.
    correctionLimitCurrent += correctionLimitAlpha
        * (p.maxCorrectionSemitones - correctionLimitCurrent);
    const double limit = std::clamp(correctionLimitCurrent, 0.0, 12.0);
    u = std::clamp(u, -limit, limit);
    const double a = active ? alpha : releaseAlpha;
    c1 += a * (u - c1);
    c += a * (c1 - c);
    return float (std::exp2 (std::clamp(c, -limit, limit) / 12.0));
}
} // namespace lyra
