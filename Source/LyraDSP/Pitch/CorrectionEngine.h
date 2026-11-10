#pragma once
#include "TargetSelector.h"
#include "YinDetector.h"

namespace lyra
{
// Turns pitch estimates into a per-sample pitch ratio: target selection -> note stabilizer ->
// requested correction (semitones, scaled by Amount) -> critically damped smoothing with the Speed
// time constant (modulated by Sustain).
//
// Smoothing the correction offset (not the output pitch) is what makes slow Speed natural: fast input
// movement passes through, because the offset cannot follow it.
//
// Speed mapping (ms):
//   > 0  : two cascaded one-poles (critically damped), each tau = T / 2.146, so the step response
//          1 - (1 + t/tau) e^(-t/tau) reaches 63.2 % at t = T, like the one-pole it replaces, but
//          starts with zero slope and never overshoots: retunes glide in an S-curve instead of
//          jumping off at full rate.
//   == 0 : instant: the correction follows the (predicted) pitch every sample.
//   < 0  : "aggressive" (-3..0): instant, and target hysteresis shrinks linearly to 0 at -3, so
//          the output snaps between notes like a hard-tuned effect. Negative values never reach exp().
//
// Note Stabilizer (causal, adds no latency): when the selected note differs from the confirmed
// target it becomes a candidate. While unconfirmed, the correction releases toward 0 so the natural
// pitch passes (short runs, grace notes and scoops are not snapped to a wrong note). After the
// candidate persists for `stabilizerMs` it is confirmed. A note starting after >= 60 ms of unvoiced
// input is confirmed immediately: only note-to-note transitions wait.
//
// Sustain (-1..1) scales Speed by note age: T_eff = T * 2^(-3 * sustain * min(1, age / 0.5 s)),
// i.e. up to 8x faster (+1) or slower (-1) once a note has been held for 0.5 s. No effect when
// Speed <= 0 (already instant).
//
// Onset: a phrase's first note that starts steady (|slope| < kSteadyVelocity, smoothed over ~8 ms,
// judged from the 2nd continuous hop on)
// is tuned with T_eff = min(T, kOnsetMs) for its first kOnsetWindowSeconds, instead of gliding in
// from its untuned start. A note that starts with a scoop (pitch still moving) keeps T: the scoop
// stays natural.
//
// Unvoiced / pending / no allowed notes: correction releases to 0 with max(T, 20 ms).
//
// Pitch prediction (Speed independent). An estimate describes audio a few ms older than the audio the
// shifter is playing at that moment, so a moving pitch (glide, vibrato) was corrected late. Each
// sample therefore uses the pitch extrapolated along the detector's own slope:
//   p(t) = m + v * (lead + time since the estimate),   v = (m - m_prev) / hop
//   lead = (window + 1/f)/2 + 0.1 ms - shifter delay     (estimate age, YinDetector "Response lag")
// Causal and adds no latency; the per-sample extrapolation also removes the 2 ms correction
// staircase. |m - m_prev| > kJumpSemitones (note jump) or a fresh onset sets v = 0. Lead clamped to
// [0, 5 ms], v to +-kMaxVelocity, the predicted offset to +-kMaxPredict (no measurable cost on real
// vocals; bounds what a single bad estimate can do). Set by setTiming() (0 = no prediction until then).
// Measured on three real vocals, Speed 0: 90th-percentile error -10..-30 % vs no prediction, better
// than the 2 ms HQ lookahead. Smoothing m (alpha-beta, alpha < 1) was tried and was worse: real voice
// movement is larger than the detector's noise, so smoothing only adds lag.
//
// Vibrato analyser. A zigzag detector confirms a pitch peak (trough) once the pitch has fallen
// (risen) kVibratoTurnCents past it. The last three alternating extrema e0, e1, e2 count as vibrato
// when both half-cycles last 35..170 ms (about 3..14 Hz) and both swings are 16..200 cents and within
// 1.5x of each other; then
//   depth A = min(|e1 - e0|, |e2 - e1|) / 2,   centre = (e0 + 2 e1 + e2) / 4   (the cycle's midline)
// A one-way move (glide, note change) never produces alternating extrema, so it is never mistaken
// for vibrato. The analysis restarts on onsets and note jumps (not on target changes: vibrato that
// flips the target must still be recognised, or the hysteresis below could never widen), and is ignored while
// the pitch is outside centre +- (1.5 A + 10 c), so a stale centre can never pull toward a wrong note.
// Uses:
//   - Hysteresis widens with the vibrato: h = min(h0 + A/2, 45 c) (h0 = base hysteresis, scaled
//     like h0 in aggressive Speed). Wide vibrato around a note sung sharp or flat no longer flips the
//     target at every cycle.
//   - Speed > 0 corrects the vibrato's centre and keeps the vibrato: the pitch to correct becomes
//     centre + F * (p - centre), F = exp(-T / kVibratoKeepMs). F = 1 (flat) at Speed 0; at 20 ms the
//     correction removes ~19 % of the vibrato depth, at 50 ms ~1.5 %. The centre itself is tuned with
//     Speed as before. (Before, slow Speed kept vibrato only by lagging behind it, which also
//     reshaped it.)
//   shortcut: kVibratoKeepMs, the turn threshold and the 45 c cap are first estimates tuned on three
//   vocals and synthetic vibrato; revisit with listening feedback.
class CorrectionEngine
{
public:
    struct Params
    {
        double speedMs = 20.0;
        double amount = 1.0;       // 0..1
        double referenceHz = 440.0;
        PitchClassMask mask = kAllNotes;
        double hysteresisCents = 15.0;
        float minConfidence = 0.70f;
        double stabilizerMs = 0.0; // 0 = off
        double sustain = 0.0;      // -1..1
    };

    static constexpr double kSustainRampSeconds = 0.5;
    static constexpr double kOnsetGapSeconds = 0.060;
    static constexpr double kJumpSemitones = 0.75;
    static constexpr double kMaxLeadSeconds = 0.005;
    static constexpr double kMaxVelocity = 100.0; // st/s
    static constexpr double kMaxPredict = 0.20;   // st: a one-hop detector glitch can never predict a wrong note
    static constexpr double kCriticalTauFactor = 2.146; // (1 + u) e^-u = e^-1  ->  u = 2.146
    static constexpr double kOnsetMs = 8.0, kOnsetWindowSeconds = 0.040, kSteadyVelocity = 6.0; // st/s
    static constexpr double kVibratoTurnCents = 8.0, kVibratoKeepMs = 12.0, kVibratoMaxHysteresisCents = 45.0;

    // estimateInterval: time between onEstimate() calls (the detector hop).
    void prepare (double sampleRate, double estimateIntervalSeconds = 0.002);
    void reset() noexcept;
    void setParams (const Params&) noexcept; // per block; cheap when unchanged
    // Detector window and the shifter's mean delay (s), for the prediction lead.
    void setTiming (double windowSeconds, double shifterDelaySeconds) noexcept { winSec = windowSeconds; delaySec = shifterDelaySeconds; }
    // samplesBeforeRatio: how many nextRatio() calls of the current block precede the estimate's
    // position (it is emitted mid-block; the block's ratios are computed afterwards).
    void onEstimate (const PitchEstimate&, int samplesBeforeRatio = 0) noexcept;
    float nextRatio() noexcept;

    // Diagnostics (audio-thread values; copy out through a queue for the UI).
    double inputMidi() const noexcept { return inMidi; }      // 0 when unvoiced
    int targetNote() const noexcept { return target; }        // TargetSelector::kNoTarget when none
    int pendingNote() const noexcept { return candidate; }    // stabilizer candidate, or kNoTarget
    double noteAgeSeconds() const noexcept { return noteAge; }
    double correctionSemitones() const noexcept { return c; }
    double vibratoDepthCents() const noexcept { return vibOn ? 100.0 * vibDepth : 0.0; } // 0 = none

private:
    void updateCoefficients() noexcept;
    void analyseVibrato() noexcept;
    void restartVibrato() noexcept { extCount = 0; turnDir = 0; vibOn = false; }

    Params p;
    TargetSelector selector;
    double sr = 48000, dt = 0.002, baseAlpha = 1, alpha = 1, releaseAlpha = 1, baseHyst = 15;
    double c = 0, c1 = 0, inMidi = 0; // c1: first smoothing stage
    double prevMidi = 0, vel = 0, velSmooth = 0, lead = 0, sinceEst = 0, winSec = 0, delaySec = 0; // prediction (st, st/s, s); prevMidi 0 = none
    double noteAge = 0, candidateAge = 0, unvoicedTime = 1.0, now = 0;
    // Vibrato analyser: last three extrema (st, s), the running extreme, and the result.
    double ext[3] {}, extT[3] {}, runExt = 0, runExtT = 0, vibDepth = 0, vibCentre = 0, flatten = 1;
    int extCount = 0, turnDir = 0, steadyHops = 0; // turnDir: +1 looking for a peak, -1 for a trough, 0 = start
    bool active = false, onsetNote = false, vibOn = false;
    int target = TargetSelector::kNoTarget, confirmed = TargetSelector::kNoTarget, candidate = TargetSelector::kNoTarget;
};
} // namespace lyra
