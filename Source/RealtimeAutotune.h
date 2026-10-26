// RealtimeAutotune.h - header-only, dependency-free (C++17) real-time autotune.
//
// Pieces:
//  * YinDetector   streaming YIN pitch detector with a confidence value
//                  (from MicFX: src/dsp/PitchDetector.h)
//  * PsolaShifter  TD-PSOLA pitch shifter, keeps the singer's formants
//                  (from MicFX: src/dsp/PsolaShifter.h)
//  * Correction    scale snapping, retune speed, strength, humanize, vibrato
//                  preservation, custom scales, reference pitch, voicing threshold
//                  (feature set of PitchDetector-Pro's AutotuneSettings, but actually
//                  implemented)
//
// Derived from MicFX (AGPL-3.0); keep the licence if you redistribute.
//
// Mono or linked-stereo voice processing. No allocation, locks or exceptions in process().
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>
#include "PitchRiskModel.h"
#include "RealtimeLatencyLimits.h"
#include "SnapLiveShifter.h"

namespace autotune
{
// Numerical safety boundary for host audio. Finite DAW samples may still be
// enormous (e.g. after a runaway gain plug-in); passing 1e30f into YIN or a
// power envelope overflows x*x and can poison the detector for many frames.
// The bound is +30.1 dBFS, leaving ordinary 32-bit float audio untouched.
// This is not a limiter for normal audio, only a corruption containment rule.
inline float boundedAudioSample(float sample) noexcept
{
    return std::clamp(std::isfinite(sample) ? sample : 0.0f, -32.0f, 32.0f);
}
//==============================================================================
enum class Scale { Chromatic = 0, Major, Minor, HarmonicMinor, MajorPentatonic,
                   MinorPentatonic, Blues, Dorian, Custom };

inline const char* scaleName (Scale s)
{
    static const char* names[] = { "Chromatic", "Major", "Minor", "Harmonic Minor", "Major Pentatonic",
                                   "Minor Pentatonic", "Blues", "Dorian", "Custom" };
    return names[(int) s];
}

struct Settings
{
    int   key = 0;                  // 0 = C ... 11 = B
    Scale scale = Scale::Chromatic;
    std::array<bool, 12> customScale { { true, false, true, false, true, true,
                                         false, true, false, true, false, true } }; // degrees from key
    float referenceHz = 440.0f;     // tuning of A4
    float retuneMs = 20.0f;         // 0 = instant ("hard tune"), larger = more natural
    float glideMs = 0.0f;          // 0 disables dedicated note-switch glide; up to 500 ms
    float maxCorrectionSemitones = 12.0f; // constrain the largest applied shift
    float strength = 1.0f;          // 0..1 how much of the correction is applied
    float humanize = 0.0f;          // 0..1 tolerance: errors inside humanize*40 cents are left alone
    float vibratoPreserve = 0.7f;   // 0..1 how much natural vibrato survives (1 = all of it)
    float follow = 0.0f;            // 0..1 how closely the target note follows the instantaneous pitch.
                                    // 0 = judge by the slow centre (safe for sung vibrato); 1 = follow every
                                    // movement (best for rapped/spoken vocals; wide vibrato may flip notes)
    float voicingThreshold = 0.85f; // 0..1 detector confidence needed to treat input as pitched
    float noteConfirmMs = 10.0f;     // persistence of a new target; 0 = immediate
    float consonantReleaseMs = 8.0f; // turn off stale correction on lost voicing
    float noteStabilizerMs = 300.0f; // 0 disables slow pitch-centre stabilisation
    bool  precisionHardTune = false; // explicitly opt-in max-retune tracking preset
    bool  lowLatency = true;        // shorter detector hop and smaller PSOLA grains (legacy Live tests)
    bool  ultraLowLatency = false;  // 3.5 ms bounded-delay alternative; Normal keeps PSOLA fidelity
    bool  fixedHorizonVoiced = false; // V1.2 research mode; blocked by strictRealtime
    bool  strictRealtime = false; // V1.3 production: only 3.5ms/8ms bounded-history shifters
    bool  pitchSynchronousLive = true; // V1.12: phase-matched cycle folds + bounded causal vibrato lead in Live
    bool  snapLive = false;         // V1.13: SnapTune-derived delay-line renderer for Live (needs strictRealtime + lowLatency)
    bool  bypass = false;
};

inline int scaleMask (Scale s, const std::array<bool, 12>& custom)
{
    const auto m = [] (std::initializer_list<int> d) { int r = 0; for (int x : d) r |= 1 << x; return r; };

    switch (s)
    {
        case Scale::Chromatic:       return 0xfff;
        case Scale::Major:           return m ({ 0, 2, 4, 5, 7, 9, 11 });
        case Scale::Minor:           return m ({ 0, 2, 3, 5, 7, 8, 10 });
        case Scale::HarmonicMinor:   return m ({ 0, 2, 3, 5, 7, 8, 11 });
        case Scale::MajorPentatonic: return m ({ 0, 2, 4, 7, 9 });
        case Scale::MinorPentatonic: return m ({ 0, 3, 5, 7, 10 });
        case Scale::Blues:           return m ({ 0, 3, 5, 6, 7, 10 });
        case Scale::Dorian:          return m ({ 0, 2, 3, 5, 7, 9, 10 });
        case Scale::Custom:
        {
            int r = 0;
            for (int i = 0; i < 12; ++i) if (custom[(size_t) i]) r |= 1 << i;
            return r != 0 ? r : 0xfff;   // an empty scale would have nothing to snap to
        }
    }
    return 0xfff;
}

//==============================================================================
// Streaming YIN (de Cheveigne & Kawahara, 2002). Runs on a decimated copy of the
// input every ~5 ms over the latest ~14 ms, so it is cheap and adds no audio latency.
class YinDetector
{
public:
    void prepare (double sampleRate, float minHz = 70.0f, float maxHz = 1000.0f)
    {
        // 8:1 anti-aliased analysis at 44.1/48/96 kHz reduces YIN's
        // O(lag*window) cost; retain the undownsampled ring for fine-lag
        // refinement. Silence/noise and out-of-band tones are regression-tested.
        decimation   = sampleRate > 30000.0 ? 8 : (sampleRate > 16000.0 ? 2 : 1);
        // The 63-tap path is selected at 8:1, with a 0.42/8 cycles-per-input-
        // sample cutoff to attenuate out-of-band detector aliases.
        useFirDecimation = decimation > 1;
        tapCount = decimation >= 8 ? 63 : 31;
        analysisRate = sampleRate / decimation;
        maxLag       = (int) std::ceil (analysisRate / minHz);
        minLag       = std::max (2, (int) std::floor (analysisRate / maxHz));
        windowSize   = maxLag;
        hopSize      = std::max (1, (int) (analysisRate * 0.005));

        // The full-rate refinement reads the FIR group delay plus up to
        // `maxLag * decimation` samples of history. Reserve that group delay
        // explicitly; otherwise the lowest notes can wrap onto overwritten
        // samples in the circular buffer.
        fullSize = ((size_t) (windowSize + maxLag + 6) * (size_t) decimation)
                   + (size_t) tapCount + 16;
        fullHistory.assign (decimation > 1 ? fullSize : 0, 0.0f);
        // Windowed-sinc anti-alias low-pass for the decimated analysis path.
        // The full-rate waveform is retained for phase-accurate lag refinement.
        constexpr double pi = 3.14159265358979323846;
        const double cutoff = decimation > 1 ? 0.42 / (double) decimation : 0.5;
        double norm = 0.0;
        decimatorTaps.fill (0.0f); // Fresh FIR after a host sample-rate change.
        for (int i = 0; i < tapCount; ++i)
        {
            const int k = i - (tapCount - 1) / 2;
            const double window = 0.5 + 0.5 * std::cos (2.0 * pi * (double) k / ((double) tapCount - 1.0));
            const double sinc = k == 0 ? 2.0 * cutoff : std::sin (2.0 * pi * cutoff * (double) k) / (pi * (double) k);
            decimatorTaps[(size_t) i] = (float) (window * sinc);
            norm += window * sinc;
        }
        for (int i = 0; i < tapCount; ++i)
            decimatorTaps[(size_t) i] = (float) (decimatorTaps[(size_t) i] / norm);
        history.assign ((size_t) std::max (windowSize + maxLag, (int) std::ceil (analysisRate * 0.105)), 0.0f);
        frame.assign (history.size(), 0.0f);
        difference.assign ((size_t) maxLag + 2, 0.0f);
        reset();
    }

    void reset()
    {
        std::fill (history.begin(), history.end(), 0.0f);
        std::fill (fullHistory.begin(), fullHistory.end(), 0.0f);
        fullWrite = 0;
        writeIndex = decimationCount = samplesSinceAnalysis = 0;
        decimationSum = 0.0f;
        decimatorHistory.fill (0.0f);
        decimatorIndex = 0;
        frequency = confidence = 0.0f;
        rawFrequency = alternateFrequency = 0.0f;
        twoPeriodAperiodicity = -1.0f;
        lastLag = framesSinceGood = 0;
        lastRawLag = 0; subharmonicTransitionHold = 0.0f;
        activeWindowSize = windowSize;
        octaveProposal = octaveProposalFrames = 0;
    }

    // d' (normalised difference) below which a dip counts as pitched; lower = stricter.
    void setThreshold (float t) noexcept { threshold = std::clamp (t, 0.03f, 0.5f); }

    // V1.8: Established voiced notes can use one recently measured cycle
    // (plus a margin) as the YIN difference window, rather than the entire
    // 70-Hz search window. The lag search range and memory remain unchanged.
    // Conservative/full window after reset or unreliable pitch loss.
    void setRealtimeTransitionWindowEnabled(bool on) noexcept { realtimeTransitionWindow = on; }
    // Opt-in for V1.12 Live cycle resampling: a slightly shorter recent
    // YIN difference window after a verified pitch, never on cold onset.
    void setPitchSynchronousWindowEnabled(bool on) noexcept { psrShortWindow = on; }
    // The shorter, newer half of a stable-note YIN window receives more weight
    // in standard Live mode. Precision Hard Tune retains unweighted analysis
    // after adversarial octave-burst tests exposed additional false locks.
    void setRecentTransitionEmphasisEnabled(bool on) noexcept { recentTransitionEmphasis = on; }
    int getLastAnalysisWindowSamples() const noexcept { return activeWindowSize; }

    // Change cadence without reallocating detector history or touching the audio heap.
    void setAnalysisHopSeconds (float seconds) noexcept
    {
        const int newHop = std::max (1, (int) std::lround (analysisRate * std::clamp (seconds, 0.001f, 0.010f)));
        if (newHop == hopSize) return;
        hopSize = newHop;
        samplesSinceAnalysis = std::min (samplesSinceAnalysis, hopSize - 1);
    }

    // Feed one sample. Returns true when a fresh estimate is available.
    bool push (float x) noexcept
    {
        x = boundedAudioSample(x);
        if (decimation > 1)
        {
            fullHistory[fullWrite] = x;
            fullWrite = fullWrite + 1 == fullSize ? 0 : fullWrite + 1;
        }

        // Anti-aliased detector-only downsampling. The FIR state and coefficient
        // table are fixed-size and process() remains allocation-free.
        if (! useFirDecimation) decimationSum += x;
        if (useFirDecimation)
        {
            decimatorHistory[decimatorIndex] = x;
            decimatorIndex = (decimatorIndex + 1) % tapCount;
        }
        if (++decimationCount < decimation)
            return false;
        decimationCount = 0;
        float decimated = 0.0f;
        if (! useFirDecimation)
        {
            decimated = decimationSum / (float) decimation;
            decimationSum = 0.0f;
        }
        if (useFirDecimation)
        {
            decimated = 0.0f;
            // Only valid FIR taps: the 31-tap path used to read all 63 slots,
            // including stale 96-kHz coefficients after re-prepare.
            for (int k = 0; k < tapCount; ++k)
                decimated += decimatorTaps[(size_t) k] * decimatorHistory[(size_t) ((decimatorIndex + k) % tapCount)];
        }
        history[(size_t) writeIndex] = decimated;
        writeIndex = (writeIndex + 1) % (int) history.size();

        if (++samplesSinceAnalysis < hopSize)
            return false;

        samplesSinceAnalysis = 0;
        analyse();
        return true;
    }

    float getFrequency() const noexcept  { return frequency; }   // Hz, 0 = unpitched
    float getConfidence() const noexcept { return confidence; }  // 0..1
    float getRawFrequency() const noexcept { return rawFrequency; } // first qualifying CMNDF valley
    float getAlternateFrequency() const noexcept { return alternateFrequency; } // diagnostic candidate
    // NoteForge-inspired independent two-cycle periodicity evidence. -1 means
    // the analysis buffer cannot contain two complete periods; never infer it.
    float getTwoPeriodAperiodicity() const noexcept { return twoPeriodAperiodicity; }
    float getPeriodInSamples() const noexcept
    {
        return frequency > 0.0f ? (float) (analysisRate * decimation) / frequency : 0.0f;
    }
    float getHopSeconds() const noexcept { return (float) hopSize / (float) analysisRate; }
    int getMaxLag() const noexcept { return maxLag; }
    int getAnalysisWindowSamples() const noexcept { return windowSize; }
    // Debug the detector's actual FIR, including inactive coefficient slots.
    // Important when a DAW changes 96 kHz -> 48 kHz on a live instance.
    float getDecimatorCoefficientSum() const noexcept
    {
        if (! useFirDecimation) return 1.0f;
        float total = 0.0f;
        for (const float coefficient : decimatorTaps) total += coefficient;
        return total;
    }
    // Approximate worst-case YIN pairwise differences, not wall-clock CPU time.
    uint64_t getMaxDifferenceOperationsPerFrame() const noexcept
    { return (uint64_t) maxLag * (uint64_t) windowSize; }

private:
    void analyse() noexcept
    {
        const auto size = (int) history.size();
        for (int i = 0; i < size; ++i)
            frame[(size_t) i] = history[(size_t) ((writeIndex + i) % size)];

        // Bound the short window to >=one previous source cycle; never shorten
        // on a cold start or after successive rejected/noisy estimates. This
        // is a calculation-window change, NOT future-audio lookahead.
        activeWindowSize = windowSize;
        if (realtimeTransitionWindow && lastLag > 0 && framesSinceGood == 0)
        {
            const int minimum = std::max(12, (int) std::ceil(analysisRate * 0.006));
            activeWindowSize = std::clamp((int) std::ceil((double)lastLag * (psrShortWindow ? 1.05 : 1.15)),
                                          std::min(minimum, windowSize), windowSize);
        }
        const float* newest = frame.data() + size - activeWindowSize;

        float energy = 0.0f;
        for (int j = 0; j < activeWindowSize; ++j)
            energy += newest[j] * newest[j];

        if (std::sqrt (energy / (float) activeWindowSize) < silenceThreshold)
        {
            frequency = confidence = rawFrequency = alternateFrequency = 0.0f;
            twoPeriodAperiodicity = -1.0f;
            if (++framesSinceGood > maxContinuityFrames)
            {
                lastLag = 0;
                // Keep the last raw lag across short transition dropouts,
                // even after octave-continuity memory expires. This lets the
                // next onset be compared against the previous syllable,
                // before stale harmonics can mimic an octave-lower note.
                if (framesSinceGood > maxContinuityFrames +
                    (int)std::ceil(0.035f / getHopSeconds()))
                {
                    lastRawLag = 0;
                    subharmonicTransitionHold = 0.0f;
                }
            }
            return;
        }

        for (int tau = 1; tau <= maxLag; ++tau)
        {
            const float* lagged = newest - tau;
            float sum = 0.0f;
            // Causal recency emphasis: newly arrived pitch periods should not
            // be outvoted by the preceding syllable in the same short YIN
            // window. The older half remains for periodicity robustness; this
            // changes weighting, not buffer length or source latency.
            const int midpoint = activeWindowSize / 2;
            const float oldWeight = recentTransitionEmphasis && realtimeTransitionWindow ? 0.60f : 1.0f;
            for (int j = 0; j < activeWindowSize; ++j)
            {
                const float d = newest[j] - lagged[j];
                sum += (j < midpoint ? oldWeight : 1.0f) * d * d;
            }
            difference[(size_t) tau] = sum;
        }

        difference[0] = 1.0f;
        float running = 0.0f;
        for (int tau = 1; tau <= maxLag; ++tau)
        {
            running += difference[(size_t) tau];
            difference[(size_t) tau] = running > 0.0f ? difference[(size_t) tau] * (float) tau / running : 1.0f;
        }

        int tau = -1;
        for (int t = minLag; t < maxLag; ++t)
        {
            if (difference[(size_t) t] < threshold)
            {
                while (t + 1 < maxLag && difference[(size_t) t + 1] < difference[(size_t) t])
                    ++t;
                tau = t;
                break;
            }
        }

        if (tau < 0)
        {
            frequency = confidence = rawFrequency = alternateFrequency = 0.0f;
            twoPeriodAperiodicity = -1.0f;
            if (++framesSinceGood > maxContinuityFrames)
            {
                lastLag = 0;
                // Keep the last raw lag across short transition dropouts,
                // even after octave-continuity memory expires. This lets the
                // next onset be compared against the previous syllable,
                // before stale harmonics can mimic an octave-lower note.
                if (framesSinceGood > maxContinuityFrames +
                    (int)std::ceil(0.035f / getHopSeconds()))
                {
                    lastRawLag = 0;
                    subharmonicTransitionHold = 0.0f;
                }
            }
            return;
        }

        // Keep the first admissible valley and a second independent valley
        // visible for offline diagnosis of harmonic/octave ambiguity.
        rawFrequency = (float) (analysisRate / (double) tau);
        const int initial = tau;
        tau = resolveOctave (tau);
        int alternate = -1;
        for (int k = minLag + 1; k < maxLag; ++k)
        {
            if (std::abs (k - tau) < std::max (3, tau / 8)) continue;
            if (difference[(size_t) k] >= 0.42f
                || difference[(size_t) k] > difference[(size_t) k-1]
                || difference[(size_t) k] > difference[(size_t) k+1]) continue;
            if (alternate < 0 || difference[(size_t) k] < difference[(size_t) alternate]) alternate = k;
        }
        if (alternate < 0 && initial != tau) alternate = initial;
        alternateFrequency = alternate > 0 ? (float) (analysisRate / (double) alternate) : 0.0f;
        lastLag = tau;
        framesSinceGood = 0;

        // A single-lag YIN valley can be accidental in a breathy onset.
        // A genuinely periodic note normally also repeats at twice its lag.
        // Reuse the already computed CMNDF: no FFT, allocations, new buffers,
        // or extra detector/lookahead latency. Search a small window so natural
        // vibrato does not reject the correct second-cycle minimum.
        twoPeriodAperiodicity = -1.0f;
        const int secondPeriod = 2 * tau;
        if (secondPeriod + 2 < maxLag)
        {
            const int radius = std::max(2, (int) std::ceil(0.035f * (float) secondPeriod));
            float best = 1.0f;
            for (int k = std::max(minLag, secondPeriod - radius);
                 k <= std::min(maxLag - 1, secondPeriod + radius); ++k)
                best = std::min(best, difference[(size_t) k]);
            twoPeriodAperiodicity = std::clamp(best, 0.0f, 1.0f);
        }

        const float prev = difference[(size_t) tau - 1], curr = difference[(size_t) tau], next = difference[(size_t) tau + 1];
        const float den = prev - 2.0f * curr + next;
        const float refined = (float) tau + (std::abs (den) > 1.0e-9f ? 0.5f * (prev - next) / den : 0.0f);

        frequency  = (float) (analysisRate * decimation) / refineAtFullRate (refined * (float) decimation);
        confidence = std::clamp (1.0f - curr, 0.0f, 1.0f);
    }

    // Deepest dip of d' within +-rel of `centre` (walks downhill from the minimum so a slope
    // edge isn't mistaken for a dip). Returns -1 if the range is outside the search lags.
    int deepestNear (float centre, float rel) const noexcept
    {
        const int lo = std::max (minLag, (int) std::floor (centre * (1.0f - rel)));
        const int hi = std::min (maxLag - 1, (int) std::ceil (centre * (1.0f + rel)));
        if (lo >= hi) return -1;
        int best = lo;
        for (int t = lo; t <= hi; ++t)
            if (difference[(size_t) t] < difference[(size_t) best]) best = t;
        while (best + 1 < maxLag && difference[(size_t) best + 1] < difference[(size_t) best]) ++best;
        while (best - 1 > minLag && difference[(size_t) best - 1] < difference[(size_t) best]) --best;
        return best;
    }

    // Plain YIN takes the first dip under the threshold, which on real voices is sometimes the
    // half-lag of the true period (octave too high) or, on fast pitch movement, a double lag
    // (octave too low). Two guards, tuned on a real vocal recording:
    //  1. continuity: a sudden octave candidate initially favors the last good
    //     lag, but a candidate recurring for three hops can establish a real leap.
    //  2. on low notes only, a substantially deeper 2x-lag valley supports an
    //     absent/weak fundamental instead of the first strong second harmonic.
    int resolveOctave (int tau) noexcept
    {
        const float chosen = difference[(size_t) tau];
        // V1.8: changing notes leave old partials in the causal observation
        // window. They can temporarily imitate odd harmonics at half the new
        // note, producing a spurious octave-down YIN correction. Suppress the
        // *optional* missing-fundamental rescue briefly after a new raw lag;
        // keep the normal first-valley estimate and octave continuity guard.
        // Raw-lag history (rather than resolved lag) is essential for real
        // missing-fundamental voices that legitimately use the 2x candidate.
        if (realtimeTransitionWindow && lastRawLag > 0 &&
            std::abs(tau-lastRawLag) > std::max(2.0f, 0.12f*lastRawLag))
            subharmonicTransitionHold = 0.024f;
        else
            subharmonicTransitionHold = std::max(0.0f, subharmonicTransitionHold - getHopSeconds());
        lastRawLag = tau;
        // Stronger, integer-related candidates are proposed rather than
        // permanently vetoed by the previous pitch. A confirmed real octave
        // must be permitted through continuity hysteresis.
        if (tau != octaveProposal) { octaveProposal = tau; octaveProposalFrames = 0; }
        ++octaveProposalFrames;

        if (lastLag > 0)
        {
            const float ratio = (float) tau / (float) lastLag;
            const bool octaveAway = (std::abs (ratio - 2.0f) < 0.16f) || (std::abs (ratio - 3.0f) < 0.24f)
                                 || (std::abs (ratio - 0.5f) < 0.04f) || (std::abs (ratio - 0.3333f) < 0.03f);
            if (octaveAway)
            {
                const int t = deepestNear ((float) lastLag, 0.06f);
                if (t > 0 && difference[(size_t) t] < 0.35f && difference[(size_t) t] <= chosen + 0.2f)
                {
                    // Allow a well-supported new periodicity after three
                    // consecutive analysis frames; a one-hop octave glitch
                    // remains rejected. This is detector state, not an
                    // additional delayed-audio buffer.
                    if (chosen > 0.12f || octaveProposalFrames < 3)
                        return t;
                }
            }
        }

        if (kDoubleLagRule && subharmonicTransitionHold <= 0.0f && chosen > 0.008f && analysisRate / (double) tau < 350.0)
            for (const float multiple : { 2.0f })
            {
                const int t = deepestNear ((float) tau * multiple, 0.05f);
                if (t > 0 && difference[(size_t) t] < threshold && difference[(size_t) t] < 0.50f * chosen
                    && hasOddSubharmonicEvidence (tau))
                    return t;
            }

        return tau;
    }

    // A 2x YIN dip alone is NOT evidence for an octave-lower vocal.  It
    // also occurs when two consecutive cycles happen to correlate better than
    // adjacent cycles in a natural, moving voice.  A true missing fundamental
    // has energy at odd harmonics of the proposed lower frequency (e.g. 3*f0),
    // even if energy at f0 itself is weak.  Reject a subharmonic hypothesis
    // when those odd harmonics are absent.  Run this only for the unusual
    // double-period proposal, and use already buffered audio; no extra delay.
    bool hasOddSubharmonicEvidence (int shortLag) const noexcept
    {
        const float highHz = (float) analysisRate / (float) shortLag;
        const float lowHz = 0.5f * highHz;
        // Keep only the most recent four proposed low-pitch cycles.
        // A stale previous syllable in a fixed 90-ms context can make its
        // harmonics masquerade as the odd harmonics of a false octave down.
        // Four cycles preserve the evidence for true missing fundamentals.
        const double duration = std::clamp(4.0 / (double)lowHz, 0.032, 0.090);
        const int count = std::min ((int) frame.size(), (int) (analysisRate * duration));
        if (count < 32) return false;
        double re[5] {}, im[5] {};
        // Evaluate f0, 2*f0, 3*f0, 4*f0, 5*f0 using a Hann taper.
        // Oscillator recurrence avoids sin/cos in the inner sample loop.
        double cr[5], ci[5], zr[5], zi[5];
        constexpr double pi = 3.14159265358979323846;
        for (int h = 0; h < 5; ++h)
        {
            const double w = -2.0 * pi * (double) (h + 1) * lowHz / analysisRate;
            cr[h] = std::cos (w); ci[h] = std::sin (w);
            zr[h] = 1.0; zi[h] = 0.0;
        }
        const int first = (int) frame.size() - count;
        for (int i = 0; i < count; ++i)
        {
            const double x = (double) frame[(size_t) (first + i)]
                    * (0.5 - 0.5 * std::cos (2.0 * pi * i / (count - 1)));
            for (int h = 0; h < 5; ++h)
            {
                re[h] += x * zr[h]; im[h] += x * zi[h];
                const double r = zr[h] * cr[h] - zi[h] * ci[h];
                zi[h] = zr[h] * ci[h] + zi[h] * cr[h];
                zr[h] = r;
            }
        }
        const auto power = [&] (int h) { return re[h] * re[h] + im[h] * im[h]; };
        const double odd = power(0) + power(2) + power(4);
        const double even = power(1) + power(3);
        return odd > 0.0040 * (even + 1.0e-12);
    }

    // The search above runs on decimated audio, so its lag has only coarse resolution
    // (about 9 cents at 1 kHz when decimating by 2). Re-measure the winning lag on the
    // original-rate signal, a handful of lags around it, for sub-cent accuracy.
    float refineAtFullRate (float coarseLag) const noexcept
    {
        if (decimation <= 1)
            return coarseLag;
        constexpr int span = 3;
        const int centre = (int) std::lround (coarseLag);
        const int window = activeWindowSize * decimation;
        // Keep the full-rate refinement aligned with the decimated FIR's
        // linear-phase group delay during changing-pitch transitions.
        const int newest = (int) fullWrite - 1 - (useFirDecimation ? (tapCount - 1) / 2 : 0);
        const int size = (int) fullSize;
        const auto at = [&] (int back) { int i = (newest - back) % size; return fullHistory[(size_t) (i < 0 ? i + size : i)]; };

        float d[2 * span + 1];
        int bestK = -span;
        for (int k = -span; k <= span; ++k)
        {
            const int lag = centre + k;
            float sum = 0.0f;
            for (int j = 0; j < window; ++j)
            {
                const float delta = at (j) - at (j + lag);
                sum += delta * delta;
            }
            d[k + span] = sum;
            // Initialise the winner on the first sample. Comparing against d[0] on
            // that iteration compares the value with itself and leaves bestK at an
            // edge forever, silently disabling the full-rate lag refinement.
            if (k == -span || sum < d[bestK + span])
                bestK = k;
        }

        if (bestK == -span || bestK == span)
            // A boundary minimum can be a genuine pitch period that lies
            // outside the seven-point search because of 8:1 decimation.
            // Recheck only below 80 Hz; this leaves the normal fast path
            // untouched and avoids turning every analysis hop into 15 lags.
            return analysisRate / (double) coarseLag < 80.0
                ? refineAtFullRateWide (coarseLag)
                : coarseLag;

        const float a = d[bestK + span - 1], b = d[bestK + span], c = d[bestK + span + 1];
        const float den = a - 2.0f * b + c;
        return (float) (centre + bestK) + (std::abs (den) > 1.0e-12f ? 0.5f * (a - c) / den : 0.0f);
    }


    // Separate cold path intentionally keeps the original seven-lag function
    // optimizable by MSVC/GCC/Clang.  Replacing the compile-time span with a
    // runtime loop caused a large whole-detector CPU regression in testing.
    float refineAtFullRateWide (float coarseLag) const noexcept
    {
        if (decimation <= 1)
            return coarseLag;

        constexpr int span = 7;
        const int centre = (int) std::lround (coarseLag);
        const int window = activeWindowSize * decimation;
        // Keep the full-rate refinement aligned with the decimated FIR's
        // linear-phase group delay during changing-pitch transitions.
        const int newest = (int) fullWrite - 1 - (useFirDecimation ? (tapCount - 1) / 2 : 0);
        const int size = (int) fullSize;
        const auto at = [&] (int back) { int i = (newest - back) % size; return fullHistory[(size_t) (i < 0 ? i + size : i)]; };

        float d[2 * span + 1];
        int bestK = -span;
        for (int k = -span; k <= span; ++k)
        {
            const int lag = centre + k;
            float sum = 0.0f;
            for (int j = 0; j < window; ++j)
            {
                const float delta = at (j) - at (j + lag);
                sum += delta * delta;
            }
            d[k + span] = sum;
            // Initialise the winner on the first sample. Comparing against d[0] on
            // that iteration compares the value with itself and leaves bestK at an
            // edge forever, silently disabling the full-rate lag refinement.
            if (k == -span || sum < d[bestK + span])
                bestK = k;
        }

        if (bestK == -span || bestK == span)
            return coarseLag;                                   // minimum at the edge: don't trust it

        const float a = d[bestK + span - 1], b = d[bestK + span], c = d[bestK + span + 1];
        const float den = a - 2.0f * b + c;
        return (float) (centre + bestK) + (std::abs (den) > 1.0e-12f ? 0.5f * (a - c) / den : 0.0f);
    }

    static constexpr float silenceThreshold = 0.003f; // about -50 dBFS RMS

    int tapCount = 31;
    int decimation = 1, maxLag = 0, minLag = 2, windowSize = 0, activeWindowSize = 0, hopSize = 1;
    bool realtimeTransitionWindow = false, recentTransitionEmphasis = false, psrShortWindow = false;
    double analysisRate = 44100.0;
    float threshold = 0.15f;

    std::vector<float> history, frame, difference, fullHistory;
    size_t fullSize = 0, fullWrite = 0;
    int writeIndex = 0, decimationCount = 0, samplesSinceAnalysis = 0;
    std::array<float, 63> decimatorTaps {}, decimatorHistory {};
    int decimatorIndex = 0;
    bool useFirDecimation = false;
    float decimationSum = 0.0f, frequency = 0.0f, confidence = 0.0f;
    float rawFrequency = 0.0f, alternateFrequency = 0.0f;
    float twoPeriodAperiodicity = -1.0f;
    static constexpr bool kDoubleLagRule = true;
    int octaveProposal = 0, octaveProposalFrames = 0;
    int lastRawLag = 0;
    float subharmonicTransitionHold = 0.0f;
    int lastLag = 0, framesSinceGood = 0;                 // continuity memory (analysis-rate lag)
    static constexpr int maxContinuityFrames = 8;         // ~40 ms
};

//==============================================================================
// Constrained-delay live monitor shifter.
//
// A pitch-synchronous grain covering a 70-Hz cycle cannot be guaranteed to
// arrive within 4 ms. This alternative uses two causal read heads in a small
// delay line. Each head plays the input at the requested rate, wrapping only
// while its raised-cosine gain is zero. It trades formant fidelity and some
// low-register smoothness for a HARD upper bound on sample-history age.
//
// This is a time-domain, amplitude-crossfaded resampler. It is not TD-PSOLA,
// and the read-head delay is variable while correction is active: the host
// budget is an upper bound, not exact sample-aligned wet transport delay.
//
// All allocation occurs in prepare(). Never read a future sample, including
// the cubic interpolation's two positive neighbours.
class MicroLatencyShifter
{
public:
    // Low-register spectral-envelope compensation uses only causal first-order
    // filter states. It does not mix delayed original pitch back into the wet
    // output and does not request more input history or change host PDC.
    // An explicit A/B switch is available to the portable quality regressions.
    void setLowRegisterFormantGuardEnabled(bool enabled) noexcept { formantGuardEnabled = enabled; }
    void setLowRegisterThreeHeadEnabled(bool enabled) noexcept { threeHeadEnabled = enabled; }
    // V1.12: select causal, correlation-validated, whole-source-period folds
    // for Live notes whose entire cycle fits the *existing* 3.5ms delay line.
    // Low notes physically cannot meet that condition; harmonic synthesis
    // remains their bounded-delay fallback. No allocations on the audio thread.
    void setPitchSynchronousResamplingEnabled(bool enabled) noexcept
    {
        if (pitchSynchronousLive == enabled) return;
        pitchSynchronousLive = enabled;
        wrapBlend = 0.0f; wrapFadeLen = 0;
        stableSamples = 0; transitionHold = 0;
        wrapDelay = wrapOldDelay = 0.5 * (minDelay <= 4 ? 2 + maxDelay : minDelay + maxDelay);
        wrapSign = wrapOldSign = 1.0f;
        halfFold = false; psrQuarantine = 0;
        psrAccepted = psrRejected = 0; lastPsrCorrelation = 0.0f;
    }
    bool isPitchSynchronousResamplingEnabled() const noexcept { return pitchSynchronousLive; }
    int getAcceptedCycleJumps() const noexcept { return psrAccepted; }
    int getRejectedCycleJumps() const noexcept { return psrRejected; }
    float getLastCycleCorrelation() const noexcept { return lastPsrCorrelation; }
    // A/B switch: low-register, source-phase-locked additive carrier. The bank
    // never copies original-pitch dry audio into the shifted output. Its causal
    // envelope estimators retain a *history of amplitudes*, not a read head
    // further behind the declared transport tap.
    void setPhaseCoherentLowRegisterEnabled(bool enabled) noexcept
    {
        if (harmonicCarrierEnabled == enabled) return;
        harmonicCarrierEnabled = enabled;
        // Quality/Live switching must not restart an old vowel's phase bank.
        harmonicCoefficientsA.fill({}); harmonicCoefficientsB.fill({});
        harmonicSourcePhase = harmonicTargetPhase = 0.0;
        harmonicMix = harmonicLastPeriod = 0.0f;
        harmonicStableSamples = 0;
        harmonicRawEnergy = harmonicModelEnergy = 0.0f;
        harmonicNeedsClear = false;
    }
    bool isPhaseCoherentLowRegisterEnabled() const noexcept { return harmonicCarrierEnabled; }
    bool isLowRegisterFormantGuardEnabled() const noexcept { return formantGuardEnabled; }

    static int hostLatencySamples(double sr) noexcept
    {
        return (int) std::ceil(std::max(8000.0, sr) * (latency_limits::liveMs * 0.001)); // 3.5 ms
    }

    void prepare(double sampleRate, int minimumSourceAgeSamples = 4, double maximumLatencyMs = latency_limits::liveMs)
    {
        sampleRateHz = std::max(8000.0, sampleRate);
        minDelay = std::max(4, minimumSourceAgeSamples);
        // With min age > 4, all interpolated samples are older than the fixed
        // transport horizon. Preserve the original Live path byte-for-byte.
        const int budget = (int) std::ceil(std::max(8000.0, sampleRate)
                                           * std::clamp(maximumLatencyMs, 0.5, latency_limits::qualityMs) * 0.001);
        maxDelay = minimumSourceAgeSamples <= 4 ? budget : minDelay + budget;
        span = std::max(16, maxDelay - minDelay);
        size_t capacity = 1;
        while (capacity < (size_t) (maxDelay + 80)) capacity <<= 1;
        ring.assign(capacity, 0.0f);
        mask = (int64_t) capacity - 1;
        fadeCoeff = 1.0f - std::exp(-1.0f / (float) (std::max(8000.0, sampleRate) * 0.003));
        powerCoeff = 1.0f - std::exp(-1.0f / (float) (std::max(8000.0, sampleRate) * 0.030));
        meterCoeff = 1.0f - std::exp(-4.0f / (float) (std::max(8000.0, sampleRate) * 0.030));
        wrapFadeMax = std::max(8, (int) std::lround(sampleRate * 0.0025));
        wrapBlendCoeff = 1.0f - std::exp(-1.0f / (float) (std::max(8000.0, sampleRate) * 0.0015));
        stableNeeded = (int) std::lround(sampleRate * 0.005);
        // 3 causal, matched one-pole crossovers. They compensate broad vowel
        // envelope tilts, NOT individual partials. No FFT/window/lookahead.
        const float fs = (float)std::max(sampleRate,8000.0);
        for (int b = 0; b < 3; ++b)
        {
            const float hz = b == 0 ? 330.0f : (b == 1 ? 1150.0f : 3100.0f);
            formantCutoffs[(size_t)b] = 1.0f - std::exp(-6.28318530718f * hz / fs);
        }
        formantMeterCoeff = 1.0f - std::exp(-1.0f/(fs*0.045f));
        formantGainCoeff = 1.0f - std::exp(-1.0f/(fs*0.020f));
        formantMixCoeff = 1.0f - std::exp(-1.0f/(fs*0.006f));
        threeHeadCoeff = 1.0f - std::exp(-1.0f/(fs*0.008f));
        // Two cascaded, six-millisecond synchronous detectors attenuate
        // the mirrored 2*F0 carrier. Coefficients and
        // oscillator phases are causal and block-size invariant.
        harmonicCoefAlpha = 1.0f-std::exp(-1.0f/(fs*0.006f));
        harmonicAttackAlpha = 1.0f-std::exp(-1.0f/(fs*0.004f));
        harmonicReleaseAlpha = 1.0f-std::exp(-1.0f/(fs*0.0012f));
        harmonicEnergyAlpha = 1.0f-std::exp(-1.0f/(fs*0.004f));
        // V20: monitor abrupt note-target changes over 3 ms, then require a
        // 30 ms transition settling interval before enabling period wrapping.
        // This is measured in samples, independent of host block size.
        ratioTrackCoeff = 1.0f - std::exp(-1.0f / (float) (sampleRate * 0.003));
        transitionHoldSamples = std::max(1, (int) std::lround(sampleRate * 0.030));
        reset();
    }

    void reset() noexcept
    {
        std::fill(ring.begin(), ring.end(), 0.0f);
        now = 0;
        phase = 0.25;
        ratio = 1.0f;
        lastAlignedDry = 0.0f;
        pitchBlend = 0.0f;
        dryPower = wetPower = 1.0e-6f;
        wetGain = gainTarget = 1.0f;
        wrapDelay = 0.5 * (minDelay <= 4 ? 2 + maxDelay : minDelay + maxDelay);
        wrapOldDelay = wrapDelay;
        wrapPeriod = 0.0f;
        noteWasReleased = false;
        wrapFadePos = wrapFadeLen = 0;
        wrapBlend = 0.0f;
        previousPeriod = 0.0f; stableSamples = 0;
        trackedRatio = 1.0f; transitionHold = 0;
        psrQuarantine = 0; psrAccepted = psrRejected = 0;
        lastPsrCorrelation = 0.0f;
        wrapSign = wrapOldSign = 1.0f; halfFold = false; halfCorrelation = 1.0f;
        halfEvidence = 0; halfDot = halfEnergyA = halfEnergyB = 0.0;
        formantDryLP.fill(0.0f); formantWetLP.fill(0.0f);
        formantDryPower.fill(1.0e-7f); formantWetPower.fill(1.0e-7f);
        formantGains.fill(1.0f); formantTargets.fill(1.0f); formantMix = 0.0f;
        threeHeadMix = 0.0f;
        harmonicCoefficientsA.fill({});
        harmonicCoefficientsB.fill({});
        harmonicSourcePhase = harmonicTargetPhase = 0.0;
        harmonicMix = 0.0f;
        harmonicStableSamples = 0;
        harmonicRawEnergy = harmonicModelEnergy = 0.0f;
        harmonicLastPeriod = 0.0f;
        harmonicNeedsClear = false;
    }

    void setRatio(float newRatio) noexcept
    {
        ratio = std::clamp(std::isfinite(newRatio) ? newRatio : 1.0f, 0.5f, 2.0f);
    }

    // The source period is passed from the *same* robust detector estimate
    // used by the correction controller.  Do not infer it from the target F0.
    void setPeriod(float samples) noexcept
    {
        // Detector periods are bounded in ordinary operation.  An unusually
        // large but FINITE input is just as dangerous as NaN here: the live
        // wrap scheduler converts 0.40 * period into an integer fade length.
        // lround(FLT_MAX) raises FE_INVALID and a narrowing integer cast may
        // have undefined behaviour.  Treat physically implausible periods as
        // unvoiced instead of allowing them to corrupt persistent state.
        // 8 * 3.5ms corresponds to a conservative ~36-Hz lower limit, well
        // below the detector's nominal 70-Hz floor at every sample rate.
        const float nextPeriod = std::isfinite(samples) && samples >= 4.0f
            && samples <= 8.0f * (float) maxDelay ? samples : 0.0f;
        // Silence/noise following a tracked voiced note must not inherit the
        // previous pitch ratio. Do not change stand-alone explicit ratio-only
        // operation when no source period was ever supplied.
        if (wrapPeriod > 0.0f && nextPeriod <= 0.0f) noteWasReleased = true;
        else if (nextPeriod > 0.0f) noteWasReleased = false;
        wrapPeriod = nextPeriod;
    }

    int getMaximumDelaySamples() const noexcept { return maxDelay; }
    // Verified period accepted by the shifter, or zero for invalid/unvoiced.
    // Used by numerical-safety regressions and future detector diagnostics.
    float getAcceptedPeriodSamples() const noexcept { return wrapPeriod; }
    // Same stationary, maximum-delay tap used by the 3.5 ms dry/bypass path.
    // Recorded on each processSample(), so Amount automation cannot leak the
    // shifter's previous wet grains after Amount reaches zero.
    float getLastAlignedDry() const noexcept { return lastAlignedDry; }
    // Non-mutating diagnostics for deterministic synthesis regression tests.
    float getPeriodWrapBlend() const noexcept { return wrapBlend; }
    int getTransitionHoldSamples() const noexcept { return transitionHold; }
    // Non-mutating test diagnostics; not a declaration of pitch-tracking latency.
    float getHarmonicCarrierMix() const noexcept { return harmonicMix; }
    float getHarmonicModelFraction() const noexcept
    {
        return harmonicModelEnergy / std::max(harmonicRawEnergy, 1.0e-8f);
    }
    int getHarmonicStableSamples() const noexcept { return harmonicStableSamples; }
    bool isHalfCycleFolding() const noexcept { return halfFold && wrapBlend > 0.90f; }
    float getHalfCycleCorrelation() const noexcept { return halfCorrelation; }

    float processSample(float input) noexcept
    {
        input = boundedAudioSample(input);
        ring[(size_t) (now & mask)] = input;
        const float dry = read((double) now - (double) maxDelay);
        lastAlignedDry = dry;
        const float correction = std::abs(ratio - 1.0f);
        // A stable unity tap keeps consonants and naturally in-tune notes
        // transparent. Ramp during note acquisition and after voicing loss.
        // Once voicing is released, pitch correction must also release.
        // The controller can retain a smoothed correction ratio for a few ms,
        // but forcing it across an unvoiced consonant smears the onset.
        const float targetBlend = noteWasReleased ? 0.0f
            : std::clamp((correction - 0.0003f) / 0.002f, 0.0f, 1.0f);
        // Release the old vowel promptly when voicing vanishes. New note attacks
        // retain the smoother 3 ms acquisition envelope.
        const float blendCoeff = noteWasReleased ? harmonicReleaseAlpha : fadeCoeff;
        pitchBlend += blendCoeff * (targetBlend - pitchBlend);
        if (pitchBlend < 0.00001f) pitchBlend = 0.0f;
        // phase advances with delay slope (1-ratio). Positive pitch shifts
        // shrink the read delay, making the read head advance faster.
        phase += (1.0 - (double) ratio) / (double) span;
        phase -= std::floor(phase);
        float wet = dry;
        if (pitchBlend > 0.0f)
        {
            const double second = phase + 0.5 >= 1.0 ? phase - 0.5 : phase + 0.5;
            const double a = std::sin(3.14159265358979323846 * phase);
            const float wa = (float) (a * a);
            const float headA = read((double) now - (double) minDelay - phase * (double) span);
            const float headB = read((double) now - (double) minDelay - second * (double) span);
            wet = wa * headA + (1.0f - wa) * headB;
            // A third overlapping tap reduces low-F0 interference between
            // two far-apart fractional-delay heads. The read ages of all
            // three taps stay strictly within the same 3.5ms ring budget.
            const bool lowRegister = threeHeadEnabled && wrapPeriod > 0.0f
                && sampleRateHz / (double)wrapPeriod < 225.0;
            threeHeadMix += threeHeadCoeff*((lowRegister ? 1.0f : 0.0f) - threeHeadMix);
            if (threeHeadMix > 1.0e-5f)
            {
                float three = 0.0f;
                constexpr double third = 1.0 / 3.0;
                for (int j=0; j<3; ++j)
                {
                    double p = phase + (double)j*third;
                    if (p>=1.0) p-=1.0;
                    const float h = (float)std::sin(3.14159265358979323846 * p);
                    const float weight = (2.0f/3.0f)*h*h;
                    three += weight*read((double)now-(double)minDelay-p*(double)span);
                }
                wet += threeHeadMix*(three-wet);
            }
        }
        // The coherent carrier replaces the leakage-prone two-head crossover
        // below the pitch-cycle folding range. Output harmonics share a single
        // continuous target phase, and their magnitude/relative phase comes
        // only from causal, synchronously detected source harmonics.
        // We use the same delayed reference as the dry path: no new transport
        // buffer or future read is introduced. Envelope tracking has nonzero
        // settling time and is NOT an exact wet-waveform latency guarantee.
        wet = renderHarmonicCarrier(dry, wet);
        // Period-folding branch: unlike time-windowed dual heads, one tap
        // follows the requested playback speed between *whole-period* wraps.
        // The two taps sound the same pitched cycle at a wrap, minimizing
        // the original-frequency carrier and the crossfade modulation tones.
        // Full-cycle wraps are only possible when the measured source period
        // physically fits the 3.5 ms history budget.  Low notes use the
        // bounded two-head implementation rather than dishonest lookahead.
        const float delta = 1.0f - ratio;
        if (psrQuarantine > 0) --psrQuarantine;
        // V20: a source note can remain periodic even when the *target note*
        // changes abruptly. The old source-only stability gate kept period
        // wrapping live through those target jumps. That caused resampling
        // artifacts during note transitions. Track the correction trajectory
        // separately and fade temporarily to the bounded two-head shifter.
        trackedRatio += ratioTrackCoeff * (ratio - trackedRatio);
        if (std::abs(ratio - trackedRatio) > 0.0030f)
            transitionHold = pitchSynchronousLive
                ? std::max(1, (int)std::lround(sampleRateHz * 0.008))
                : transitionHoldSamples;
        else if (transitionHold > 0)
            --transitionHold;
        const float candidatePeriod = wrapPeriod;
        if (candidatePeriod > 0.0f && previousPeriod > 0.0f
            && std::abs(candidatePeriod - previousPeriod) <= candidatePeriod * (pitchSynchronousLive ? 0.004f : 0.006f))
            stableSamples = std::min(stableSamples + 1, stableNeeded + 1);
        else
            stableSamples = 0;
        previousPeriod = candidatePeriod;
        // Low-register extension: a near-anti-periodic waveform can be joined
        // at HALF a cycle if the new read tap is polarity-inverted. This uses
        // no future samples and never expands the existing 3.5ms delay ring.
        // Strong even harmonics break half-cycle anti-periodicity, so gate by
        // measured normalized correlation and fall back to V20 in that case.
        const float deltaAbs = std::abs(delta);
        const int fadeSamples = std::min(wrapFadeMax,
            std::max(8, (int) std::lround(candidatePeriod * 0.40f)));
        const double reserve = 0.6 + (double)deltaAbs * (double)(fadeSamples + 3);
        const int requiredStability = pitchSynchronousLive
            ? std::max(1, (int)std::lround(sampleRateHz * 0.003)) : stableNeeded;
        const bool stable = transitionHold == 0 && stableSamples >= requiredStability
            && psrQuarantine == 0 && candidatePeriod >= 14.0f && pitchBlend > 0.001f;
        const bool fullUsable = stable &&
            (double)candidatePeriod + 2.0 * reserve < (double)(maxDelay - (minDelay <= 4 ? 3 : minDelay+3));
        // The correlation check is deliberately not part of the general
        // per-sample DSP: no extra work for in-tune notes or high register.
        if (!stable || fullUsable)
        {
            // Reset correlation memory when the note / synthesis range changes.
            halfCorrelation = 1.0f;
            halfEvidence = 0;
            halfDot = halfEnergyA = halfEnergyB = 0.0;
        }
        else if ((now & 63) == 0)
        {
            double dot=0.0, energyA=0.0, energyB=0.0;
            if (measureHalfCycleStatistics(candidatePeriod * 0.5f, dot, energyA, energyB))
            {
                // This MUST average over many source phases. Testing a tiny
                // window in isolation can misclassify vowels with even harmonics
                // as anti-periodic near a local zero crossing.
                constexpr double hold = 0.80;
                halfDot = hold * halfDot + dot;
                halfEnergyA = hold * halfEnergyA + energyA;
                halfEnergyB = hold * halfEnergyB + energyB;
                halfEvidence = std::min(halfEvidence + 1, 100);
                halfCorrelation = halfEvidence >= 12 && halfEnergyA > 1.e-5 && halfEnergyB > 1.e-5
                    ? (float)(halfDot / std::sqrt(halfEnergyA * halfEnergyB)) : 1.0f;
            }
            else
            {
                halfCorrelation = 1.0f;
                halfEvidence = 0;
                halfDot = halfEnergyA = halfEnergyB = 0.0;
            }
        }
        const bool halfUsable = !pitchSynchronousLive && stable && !fullUsable && halfCorrelation < -0.985f
            && (double)candidatePeriod * 0.5 + 2.0 * reserve < (double)(maxDelay - (minDelay <= 4 ? 3 : minDelay+3));
        const bool wantHalf = halfUsable;
        // A fold mode change must first finish a fade to V20. Never change
        // polarity conventions in the middle of an audible grain.
        bool usable = fullUsable || halfUsable;
        if (usable && wantHalf != halfFold && wrapBlend >= 0.002f)
            usable = false;
        if (wrapBlend < 0.002f && usable && wantHalf != halfFold)
        {
            halfFold = wantHalf;
            wrapSign = wrapOldSign = 1.0f;
            wrapDelay = wrapOldDelay = 0.5 * (minDelay <= 4 ? 2 + maxDelay : minDelay + maxDelay);
            wrapFadeLen = 0;
        }
        const float desiredWrap = usable ? 1.0f : 0.0f;
        wrapBlend += wrapBlendCoeff * (desiredWrap - wrapBlend);
        if (wrapBlend < 0.00001f)
        {
            wrapBlend = 0.0f;
            if (!usable) { wrapDelay = 0.5 * (minDelay <= 4 ? 2 + maxDelay : minDelay + maxDelay); wrapFadeLen = 0; wrapSign = wrapOldSign = 1.0f; }
        }
        if (wrapBlend > 0.0f || usable)
        {
            wrapDelay += (double) delta;
            if (wrapFadeLen > 0) wrapOldDelay += (double) delta;
            if (wrapFadeLen == 0 && usable)
            {
                const double next = (double) candidatePeriod * (halfFold ? 0.5 : 1.0);
                if (delta < 0.0f && wrapDelay < (minDelay <= 4 ? 2.0 : (double)minDelay+3.0) + reserve)
                {
                    const double refined = refineWrapPeriod(wrapDelay, next, 1, halfFold);
                    if (!pitchSynchronousLive || validateCycleMatch(wrapDelay, wrapDelay + refined))
                    {
                        wrapOldDelay = wrapDelay;
                        wrapOldSign = wrapSign;
                        wrapDelay += refined;
                        if (halfFold) wrapSign = -wrapSign;
                        wrapFadeLen = fadeSamples;
                        wrapFadePos = 0;
                        if (pitchSynchronousLive) ++psrAccepted;
                    }
                    else
                    {
                        ++psrRejected;
                        psrQuarantine = std::max(1, (int)std::lround(sampleRateHz * 0.005));
                    }
                }
                else if (delta > 0.0f && wrapDelay > (double) (maxDelay - 1) - reserve)
                {
                    const double refined = refineWrapPeriod(wrapDelay, next, -1, halfFold);
                    if (!pitchSynchronousLive || validateCycleMatch(wrapDelay, wrapDelay - refined))
                    {
                        wrapOldDelay = wrapDelay;
                        wrapOldSign = wrapSign;
                        wrapDelay -= refined;
                        if (halfFold) wrapSign = -wrapSign;
                        wrapFadeLen = fadeSamples;
                        wrapFadePos = 0;
                        if (pitchSynchronousLive) ++psrAccepted;
                    }
                    else
                    {
                        ++psrRejected;
                        psrQuarantine = std::max(1, (int)std::lround(sampleRateHz * 0.005));
                    }
                }
            }
            // Keep both read taps causal even during detector dropout,
            // automation and extreme ±12-semitone corrections.
            wrapDelay = std::clamp(wrapDelay, minDelay <= 4 ? 2.0 : (double) minDelay+3.0, (double) maxDelay - 1.0);
            wrapOldDelay = std::clamp(wrapOldDelay, minDelay <= 4 ? 2.0 : (double) minDelay+3.0, (double) maxDelay - 1.0);
            float wrapped = wrapSign * read((double) now - wrapDelay);
            if (wrapFadeLen > 0)
            {
                const float k = (float)(wrapFadePos + 1) / (float)wrapFadeLen;
                const float x = 0.5f - 0.5f * std::cos(3.14159265358979323846f * k);
                wrapped = wrapOldSign * read((double)now - wrapOldDelay) * (1.0f - x) + wrapped * x;
                if (++wrapFadePos >= wrapFadeLen) wrapFadeLen = 0;
            }
            wet += wrapBlend * (wrapped - wet);
        }
        // Track true delayed-input and shifted-output energy; compensate only
        // for level loss from head interference, never an unconstrained boost.
        // Limit meter magnitudes independently of the audio stream. A finite
        // but pathological host sample must never poison this persistent
        // energy tracker via x*x overflow (or infinity/infinity gain).
        if ((now & 3) == 0)
        {
            const float safeDry = std::clamp(dry, -4.0f, 4.0f);
            const float safeWet = std::clamp(wet, -4.0f, 4.0f);
            dryPower += meterCoeff * (safeDry * safeDry - dryPower);
            wetPower += meterCoeff * (safeWet * safeWet - wetPower);
        }
        // A decimated, strictly bounded gain target removes the recurring
        // square-root from the audio-rate inner loop. The per-sample output
        // gain itself remains smoothed and continuous.
        if ((now & 31) == 0)
            gainTarget = dryPower < 1.0e-7f ? 1.0f
                : std::clamp(std::sqrt((dryPower + 1.0e-7f) / (wetPower + 1.0e-7f)), 0.85f, 1.30f);
        wetGain += powerCoeff * (gainTarget - wetGain);
        // The low F0 path cannot support full-period wrapping within 3.5 ms.
        // Match the broadband vowel envelope after pitch shifting, while
        // leaving corrected pitch and its oscillator phase intact.
        wet = restoreBroadFormantEnvelope(dry, wet,
            formantGuardEnabled && wrapPeriod > 0.0f &&
            ((float) sampleRateHz / wrapPeriod) < 230.0f && pitchBlend > 0.5f);
        ++now;
        const float output = dry + pitchBlend * (wetGain * wet - dry);
        return std::isfinite(output) ? output : 0.0f;
    }

private:
    struct HarmonicIQ { float cos=0.0f, sin=0.0f; };
    static constexpr int harmonicCount = 18;
    float renderHarmonicCarrier(float delayedDry, float legacyWet) noexcept
    {
        constexpr double twoPi = 6.28318530717958647692;
        const bool valid = wrapPeriod > 0.0f && std::isfinite(wrapPeriod);
        if (!harmonicCarrierEnabled) return legacyWet;
        // Synchronise output phase to measured source F0, NEVER to a new
        // target note, which would force audible phase resets at transitions.
        const double f0 = valid ? sampleRateHz/(double)wrapPeriod : 0.0;
        const bool inRange = valid && f0 >= 67.0 && f0 <= 225.0;
        const double incr = inRange ? twoPi / (double)wrapPeriod : 0.0;
        if (inRange)
        {
            harmonicSourcePhase += incr;
            harmonicTargetPhase += incr * (double)ratio;
            if (harmonicSourcePhase >= twoPi) harmonicSourcePhase -= twoPi;
            if (harmonicTargetPhase >= twoPi) harmonicTargetPhase -= twoPi;
        }
        // An F0 discontinuity should stop the previous vowel's harmonic
        // envelope, but cannot atomically reset a bank while it is audible.
        // We first fade it out, then clear it below -30 dB of carrier mix.
        const bool sourceJump = inRange && harmonicLastPeriod > 0.0f &&
            std::abs(wrapPeriod-harmonicLastPeriod) > wrapPeriod*0.025f;
        if (!inRange || sourceJump)
        {
            harmonicStableSamples=0;
            if (harmonicLastPeriod > 0.0f) harmonicNeedsClear=true;
        }
        else
            harmonicStableSamples = std::min(harmonicStableSamples+1, (int)sampleRateHz);
        harmonicLastPeriod = inRange ? wrapPeriod : 0.0f;

        // Always process state for coherent switching even when this mode is
        // disabled. This is the only extra DSP in the independent A/B mode.
        float harmonicWet = 0.0f, sourceModel = 0.0f;
        if (inRange)
        {
            const double phIn = harmonicSourcePhase - incr*(double)maxDelay;
            const double phOut = harmonicTargetPhase - incr*(double)maxDelay;
            const double sinIn = std::sin(phIn), cosIn = std::cos(phIn);
            const double sinOut = std::sin(phOut), cosOut = std::cos(phOut);
            double sc=cosIn, ss=sinIn, tc=cosOut, ts=sinOut;
            // Analyze no partial higher than the original 3.8 kHz source
            // ceiling, and never synthesize a partial above the *corrected*
            // output's Nyquist guard. The V1.5 source-only limit produced
            // aliases in 8/16-kHz sessions at upward retune ratios.
            // The soft 15% transition avoids a hard partial switch during
            // ratio automation; it costs only two scalar ops per partial.
            const double outputCeilingHz = std::min(5600.0, 0.43 * sampleRateHz);
            const double outputF0 = f0 * (double)ratio;
            const int sourceLimit = std::min(harmonicCount, (int)(3800.0/f0));
            const int outputLimit = std::min(harmonicCount,
                (int)std::ceil(outputCeilingHz / std::max(1.0, outputF0)));
            const int limit = std::min(sourceLimit, outputLimit);
            const double fadeStartHz = outputCeilingHz * 0.85;
            const double fadeWidthHz = outputCeilingHz - fadeStartHz;
            for(int k=0;k<limit;++k)
            {
                HarmonicIQ& first=harmonicCoefficientsA[(size_t)k];
                HarmonicIQ& second=harmonicCoefficientsB[(size_t)k];
                const float c=(float)(2.0*(double)delayedDry*sc);
                const float v=(float)(2.0*(double)delayedDry*ss);
                first.cos += harmonicCoefAlpha*(c-first.cos);
                first.sin += harmonicCoefAlpha*(v-first.sin);
                second.cos += harmonicCoefAlpha*(first.cos-second.cos);
                second.sin += harmonicCoefAlpha*(first.sin-second.sin);
                const double harmonicHz = (k+1)*outputF0;
                const double taper = std::clamp((outputCeilingHz-harmonicHz)/fadeWidthHz,0.0,1.0);
                harmonicWet += (float)(taper*((double)second.cos*tc + (double)second.sin*ts));
                sourceModel += (float)(taper*((double)second.cos*sc + (double)second.sin*ss));
                const double nextSC=sc*cosIn-ss*sinIn;
                ss=ss*cosIn+sc*sinIn; sc=nextSC;
                const double nextTC=tc*cosOut-ts*sinOut;
                ts=ts*cosOut+tc*sinOut; tc=nextTC;
            }
        }
        // Fast causal coherence proxy: how much of the delayed source's
        // energy is explained by its tracked harmonic series? It rejects
        // silence and many broadband consonants even if a stale detector
        // continues providing an F0. This is not a proof of voicedness.
        const float raw = std::clamp(delayedDry, -2.0f, 2.0f);
        const float modeled = std::clamp(sourceModel, -2.0f, 2.0f);
        harmonicRawEnergy += harmonicEnergyAlpha * (raw*raw - harmonicRawEnergy);
        harmonicModelEnergy += harmonicEnergyAlpha * (modeled*modeled - harmonicModelEnergy);
        if (harmonicRawEnergy < 1.0e-18f) harmonicRawEnergy=0.0f;
        if (harmonicModelEnergy < 1.0e-18f) harmonicModelEnergy=0.0f;
        // Need a little more than one F0 cycle at low pitches, plus enough
        // settling time for the two six-millisecond synchronous estimators.
        // Unlike the old fixed 30ms gate, readiness varies with F0.
        const double requiredSeconds = inRange
            ? std::clamp(1.25 / f0, 0.010, 0.019) : 0.019;
        const bool modelReady = harmonicRawEnergy > 1.0e-6f
            && harmonicModelEnergy > 0.18f * harmonicRawEnergy;
        const bool want = inRange && !harmonicNeedsClear && f0 < 205.0
            && harmonicStableSamples >= (int)std::ceil(sampleRateHz * requiredSeconds)
            && modelReady && pitchBlend > 0.3f && std::abs(ratio-1.0f)>0.007f;
        const float mixAlpha = want ? harmonicAttackAlpha : harmonicReleaseAlpha;
        harmonicMix += mixAlpha*((want?1.0f:0.0f)-harmonicMix);
        if (!want && harmonicMix < 0.03f && harmonicNeedsClear)
        {
            // Reset only after the obsolete carrier has become inaudible;
            // force a fresh acquisition on the next voiced segment.
            harmonicCoefficientsA.fill({}); harmonicCoefficientsB.fill({});
            harmonicSourcePhase=harmonicTargetPhase=0.0;
            harmonicRawEnergy=harmonicModelEnergy=0.0f;
            harmonicStableSamples=0;
            harmonicNeedsClear=false;
            harmonicMix=0.0f;
        }
        if (!want && harmonicMix < 1.0e-5f) harmonicMix=0.0f;
        return legacyWet + harmonicMix * (harmonicWet - legacyWet);
    }
    float restoreBroadFormantEnvelope(float reference, float shifted, bool apply) noexcept
    {
        // Complementary first-order bands reconstruct the wet sample exactly
        // before gain correction. Both references use the same transfer function.
        std::array<float,4> dryBands{},wetBands{};
        float dryPrev = reference, wetPrev = shifted;
        for (int b=2; b>=0; --b)
        {
            const float coefficient = formantCutoffs[(size_t)b];
            auto& dryLP = formantDryLP[(size_t)b];
            auto& wetLP = formantWetLP[(size_t)b];
            dryLP += coefficient*(reference - dryLP);
            wetLP += coefficient*(shifted - wetLP);
            // Hard-zero decayed IIR tails to avoid denormals in idle audio.
            if (std::abs(dryLP) < 1.0e-18f) dryLP = 0.0f;
            if (std::abs(wetLP) < 1.0e-18f) wetLP = 0.0f;
            dryBands[(size_t)b+1] = dryPrev - dryLP;
            wetBands[(size_t)b+1] = wetPrev - wetLP;
            dryPrev = dryLP; wetPrev = wetLP;
        }
        dryBands[0] = dryPrev; wetBands[0] = wetPrev;
        float corrected = 0.0f;
        for(int b=0;b<4;++b)
        {
            const float d=std::clamp(dryBands[(size_t)b],-4.0f,4.0f);
            const float w=std::clamp(wetBands[(size_t)b],-4.0f,4.0f);
            auto& dryEnergy=formantDryPower[(size_t)b];
            auto& wetEnergy=formantWetPower[(size_t)b];
            dryEnergy += formantMeterCoeff*(d*d - dryEnergy);
            wetEnergy += formantMeterCoeff*(w*w - wetEnergy);
            dryEnergy = std::max(dryEnergy, 1.0e-9f);
            wetEnergy = std::max(wetEnergy, 1.0e-9f);
            // Cap compensation to avoid filling harmonic notches with noise.
            if ((now & 31) == 0)
            {
                const float target = (dryEnergy < 1.0e-7f || wetEnergy < 1.0e-7f)
                    ? 1.0f : std::clamp(std::sqrt((dryEnergy + 1.0e-7f)/
                                             (wetEnergy + 1.0e-7f)) / std::max(0.75f, wetGain), 0.65f, 1.35f);
                formantTargets[(size_t)b] = target;
            }
            auto& gain = formantGains[(size_t)b];
            gain += formantGainCoeff*(formantTargets[(size_t)b] - gain);
            corrected += wetBands[(size_t)b]*gain;
        }
        const float requested = apply ? 1.0f : 0.0f;
        formantMix += formantMixCoeff*(requested - formantMix);
        if (!apply && formantMix < 1.0e-5f) formantMix = 0.0f;
        return shifted + formantMix*(corrected - shifted);
    }
    // The detector's fractional F0 is accurate for note decisions, but a
    // 1-2 sample period error can phase-cancel harmonics at a grain splice.
    // At infrequent cycle boundaries, find the most coherent causal period
    // within ±2 samples; no future lookahead or allocation is required.
    double refineWrapPeriod(double oldDelay, double suggested, int direction, bool inverted) const noexcept
    {
        const int samples = std::clamp((int)std::lround(suggested * 0.30), 12, 36);
        double best = suggested, bestError = 1e20;
        for (int j=-2;j<=2;j++)
        {
            const double pitchPeriod = suggested + (double)j;
            const double candidate = oldDelay + (double) direction * pitchPeriod;
            if(candidate < (minDelay <= 4 ? 2.0 : (double)minDelay+3.0) || candidate > (double)maxDelay - 1.0) continue;
            double numerator=0.0, power=1e-12;
            double dot=0.0, ea=0.0, eb=0.0, sa=0.0, sb=0.0;
            int count=0;
            for (int k=0;k<samples;k+=2)
            {
                const double a = read((double)now - oldDelay - (double)k);
                const double b = read((double)now - candidate - (double)k) * (inverted ? -1.0 : 1.0);
                const double d = a - b;
                numerator += d*d;
                power += a*a+b*b;
                if (pitchSynchronousLive)
                {
                    dot+=a*b;ea+=a*a;eb+=b*b;sa+=a;sb+=b;++count;
                }
            }
            // Legacy folding keeps its amplitude-normalized least-squares
            // selector. V1.12 uses *zero-mean cross-correlation* to choose
            // the strongest phase-consistent wrap candidate instead.
            double cost=numerator/power;
            if(pitchSynchronousLive && count>1)
            {
                dot-=sa*sb/count;ea-=sa*sa/count;eb-=sb*sb/count;
                cost=ea*eb>1.e-10 ? 1.0-dot/std::sqrt(ea*eb) : 2.0;
                // Prefer the detector's measured period when alignment scores
                // are indistinguishable, reducing ±1-sample period chatter.
                cost+=0.0015*std::abs(j);
            }
            if (cost<bestError) {bestError=cost;best=pitchPeriod;}
        }
        return best;
    }
    // Normalized, zero-mean cross-correlation on *past-only* cycles; verifies
    // that a proposed period jump really joins the same voiced waveform phase.
    // The sample window is bounded to 24 taps and evaluated only at a wrap.
    bool validateCycleMatch(double oldAge, double newAge) noexcept
    {
        double ab=0.0, aa=0.0, bb=0.0, sa=0.0, sb=0.0;
        constexpr int count=24;
        // In 3.5ms Live the minimum age is 4, so the interpolation at k=0
        // cannot reach a future sample; k>0 reads only further into history.
        for (int k=0;k<count;++k)
        {
            const double a=read((double)now-oldAge-(double)k);
            const double b=read((double)now-newAge-(double)k);
            sa+=a;sb+=b;aa+=a*a;bb+=b*b;ab+=a*b;
        }
        ab-=sa*sb/count; aa-=sa*sa/count; bb-=sb*sb/count;
        const double energy=aa*bb;
        lastPsrCorrelation=energy>1.0e-10
            ? (float)std::clamp(ab/std::sqrt(energy),-1.0,1.0) : 0.0f;
        return lastPsrCorrelation >= 0.72f;
    }
    bool measureHalfCycleStatistics(float halfPeriod, double& dot,
                                    double& energyA, double& energyB) const noexcept
    {
        if (halfPeriod < 7.0f || (double)halfPeriod + 30.0 > (double)maxDelay)
            return false;
        for (int k = 0; k < 24; ++k)
        {
            const double a = read((double)now - (minDelay <= 4 ? 4.0 : (double)minDelay+3.0) - (double)k);
            const double b = read((double)now - (minDelay <= 4 ? 4.0 : (double)minDelay+3.0) - (double)k - (double)halfPeriod);
            dot += a * b;
            energyA += a * a;
            energyB += b * b;
        }
        return energyA >= 1.0e-5 && energyB >= 1.0e-5;
    }
    float read(double position) const noexcept
    {
        const int64_t i = (int64_t) std::floor(position);
        const float f = (float) (position - (double) i);
        const float a = ring[(size_t) ((i - 1) & mask)];
        const float b = ring[(size_t) (i & mask)];
        const float c = ring[(size_t) ((i + 1) & mask)];
        const float d = ring[(size_t) ((i + 2) & mask)];
        const float k1 = 0.5f * (c - a);
        const float k2 = a - 2.5f * b + 2.0f * c - 0.5f * d;
        const float k3 = 0.5f * (d - a) + 1.5f * (b - c);
        return ((k3 * f + k2) * f + k1) * f + b;
    }
    bool harmonicCarrierEnabled = false;
    std::array<HarmonicIQ,harmonicCount> harmonicCoefficientsA{}, harmonicCoefficientsB{};
    double harmonicSourcePhase=0.0, harmonicTargetPhase=0.0;
    float harmonicCoefAlpha=0.0002f, harmonicAttackAlpha=0.001f,
          harmonicReleaseAlpha=0.005f, harmonicEnergyAlpha=0.005f, harmonicMix=0.0f;
    float harmonicRawEnergy=0.0f,harmonicModelEnergy=0.0f;
    float harmonicLastPeriod=0.0f;
    int harmonicStableSamples=0;
    bool harmonicNeedsClear=false;
    bool threeHeadEnabled = false; // research A/B only: leakage varies by source F0
    float threeHeadMix = 0.0f, threeHeadCoeff = 0.001f;
    bool formantGuardEnabled = false; // opt in only for strict V1.4 production
    double sampleRateHz = 48000.0;
    std::array<float,3> formantCutoffs{}, formantDryLP{}, formantWetLP{};
    std::array<float,4> formantDryPower{}, formantWetPower{}, formantGains{}, formantTargets{1,1,1,1};
    float formantMeterCoeff=0.0005f,formantGainCoeff=0.0005f,formantMixCoeff=0.001f,formantMix=0.0f;
    std::vector<float> ring;
    int64_t mask = 0, now = 0;
    int maxDelay = 168, minDelay = 4, span = 164;
    double phase = 0.25;
    float ratio = 1.0f, pitchBlend = 0.0f, fadeCoeff = 0.004f;
    float lastAlignedDry = 0.0f;
    float dryPower = 1.0e-6f, wetPower = 1.0e-6f;
    float wetGain = 1.0f, gainTarget = 1.0f, powerCoeff = 0.001f, meterCoeff = 0.004f;
    double wrapDelay = 80.0, wrapOldDelay = 80.0;
    float wrapPeriod = 0.0f, wrapBlend = 0.0f, wrapBlendCoeff = 0.005f;
    bool noteWasReleased = false;
    int wrapFadePos = 0, wrapFadeLen = 0, wrapFadeMax = 120;
    float previousPeriod = 0.0f;
    int stableSamples = 0, stableNeeded = 960;
    float trackedRatio = 1.0f, ratioTrackCoeff = 0.005f;
    int transitionHold = 0, transitionHoldSamples = 576;
    float wrapSign = 1.0f, wrapOldSign = 1.0f, halfCorrelation = 0.0f;
    bool halfFold = false;
    bool pitchSynchronousLive = false;
    int psrQuarantine = 0, psrAccepted = 0, psrRejected = 0;
    float lastPsrCorrelation = 0.0f;
    int halfEvidence = 0;
    double halfDot = 0.0, halfEnergyA = 0.0, halfEnergyB = 0.0;
};

//==============================================================================
// Base V1 guarded, causal low-register cycle reconstruction.
//
// IMPORTANT: this renderer IS selected in opt-in Precision Live mode. It
// consumes only past input, but its sustained corrected carrier may come from
// substantially older source material than the nominal 3.5-ms host delay.
// Host PDC describes the short path, NOT an exact wet-waveform delay contract.
// An opt-in research texture-envelope guard is available, but disabled in
// shipping paths because falling back can leak the uncorrected fundamental.
class HistoricalCycleExperiment
{
public:
    enum class EnvelopeMode { none, recentEnvelope };
    void prepare(double sampleRate, int transportDelaySamples = -1)
    {
        sr = std::max(8000.0, sampleRate);
        recentDelay = transportDelaySamples > 0 ? transportDelaySamples
                    : (int) std::ceil(sr * 0.0035);
        size_t n=1;
        while(n < (size_t)(recentDelay + std::ceil(sr * 0.065))) n <<= 1;
        ring.assign(n,0.0f);
        mask = (int64_t)n-1;
        envelopeCoeff = 1.0f-std::exp(-1.0f / (float)(sr*0.0015));
        levelCoeff = 1.0f-std::exp(-1.0f/(float)(sr*0.0020));
        mixCoeff = 1.0f-std::exp(-1.0f/(float)(sr*0.0030));
        ratioTrackingCoeff = 1.0f-std::exp(-1.0f/(float)(sr*0.0030));
        textureLowCoeff = 1.0f-std::exp(-2.0f*3.14159265358979323846f*270.0f/(float)sr);
        textureEnergyCoeff = 1.0f-std::exp(-1.0f/(float)(sr*0.0080));
        textureGateCoeff = 1.0f-std::exp(-1.0f/(float)(sr*0.0012));
        textureReferenceCoeff = 1.0f-std::exp(-1.0f/(float)(sr*0.045));
        reset();
    }
    void reset() noexcept
    {
        std::fill(ring.begin(),ring.end(),0.0f);
        now=0; ratio=1.0f; period=0.0f; delay=0; oldDelay=0;
        fadePos=fadeLength=0; mix=0.0f;
        previousPeriod=0.0f; smoothedRatio=1.0f; stableSamples=0; transitionHold=0;
        recentEnergy=oldEnergy=1.0e-6f;
        envelopeGain=1.0f;
        lastRecentDry=0.0f;
        recentLow=oldLow=recentHighEnergy=oldHighEnergy=0.0f;
        textureGate=1.0f; effectiveMix=0.0f;
        recentTextureReference=oldTextureReference=0.0f;
        cycleWrapCount=phaseAlignedWrapCount=0;
        wrapCorrelationTotal=0.0;
        lastWrapCorrelation=0.0f;
    }
    void setParameters(float inputPeriod,float pitchRatio) noexcept
    {
        const float nextPeriod = std::isfinite(inputPeriod) && inputPeriod>=4.0f && inputPeriod <=(float)(sr/55.0)
                 ? inputPeriod : 0.0f;
        const float nextRatio=std::clamp(std::isfinite(pitchRatio)?pitchRatio:1.0f,0.5f,2.0f);
        if (nextPeriod > 0.0f && previousPeriod > 0.0f &&
            std::abs(nextPeriod-previousPeriod) < 0.006f*nextPeriod)
            stableSamples=std::min(stableSamples+1,(int)(sr*0.030));
        else stableSamples=0;
        if (std::abs(nextRatio-smoothedRatio)>0.0030f ||
            (nextPeriod>0 && previousPeriod>0 && std::abs(nextPeriod-previousPeriod)>0.04f*nextPeriod))
            transitionHold=(int)(sr*0.030);
        else if(transitionHold>0) --transitionHold;
        smoothedRatio += ratioTrackingCoeff * (nextRatio-smoothedRatio);
        previousPeriod=nextPeriod;
        period=nextPeriod;ratio=nextRatio;
    }
    void setEnvelopeMode(EnvelopeMode m) noexcept { mode=m; }
    // A/B research only: avoids stale timbre but can leak the old source F0
    // through the short-history fallback. DO NOT enable in production unless
    // spectral-purity and multi-vowel listening gates are passed.
    void setTextureTransitionGuardEnabled(bool enabled) noexcept
    {
        if (enabled != textureGuardEnabled)
        {
            textureGuardEnabled=enabled;
            recentLow=oldLow=recentHighEnergy=oldHighEnergy=0.0f;
            recentTextureReference=oldTextureReference=0.0f;
            textureGate=1.0f;
        }
    }
    float getLastRecentDry() const noexcept {return lastRecentDry;}
    float getHistoricalMix() const noexcept {return mix;}
    float getEffectiveHistoricalMix() const noexcept {return effectiveMix;}
    float getTextureGate() const noexcept {return textureGate;}
    // Unlike host input-to-output onset delay, stable historical voiced
    // carrier age may be > 3.5 ms. This is intentionally exposed rather than
    // pretending the waveform has an exact stationary wet delay.
    float getCarrierSourceAgeSamples() const noexcept {return mix > 0.05f ? (float)delay : 0.0f;}
    int getRecentEnvelopeDelaySamples() const noexcept {return recentDelay;}
    // A finite, conservative upper bound on the oldest source samples
    // consulted by this carrier, including the cubic interpolator's neighbour.
    // It is not a fixed wet group delay and excludes downstream output padding.
    int getMaximumCarrierSourceAgeSamples() const noexcept
    {
        return (int)std::ceil((double)recentDelay + 3.3 * sr / 110.0) + 2;
    }
    uint64_t getCycleWrapCount() const noexcept {return cycleWrapCount;}
    uint64_t getPhaseAlignedWrapCount() const noexcept {return phaseAlignedWrapCount;}
    float getLastWrapCorrelation() const noexcept {return lastWrapCorrelation;}
    double getMeanWrapCorrelation() const noexcept
    {return cycleWrapCount ? wrapCorrelationTotal / (double)cycleWrapCount : 0.0;}

    float processSample(float input, float boundedShortPath = std::numeric_limits<float>::quiet_NaN()) noexcept
    {
        input=boundedAudioSample(input);
        ring[(size_t)(now&mask)]=input;
        const float recent=read((double)now-(double)recentDelay);
        const float fallback=std::isfinite(boundedShortPath) ? boundedAudioSample(boundedShortPath) : recent;
        lastRecentDry=recent;
        const double minAge=std::max((double)recentDelay+4.0,1.1*(double)period+(double)recentDelay);
        const double maxAge=minAge+2.2*(double)period;
        const bool possible=period>0.0f && (double)period>=sr/360.0 && (double)period<=sr/110.0
            && maxAge < (double)ring.size()-4.0 && std::abs(ratio-1.0f)>0.002f;
        // A newly voiced note has to accumulate a complete historical cycle.
        // Do not blend old vocal segments into a new consonant immediately.
        const bool ready=possible && now>(int64_t)std::ceil(maxAge+4.0)
            && stableSamples >= (int)(sr*0.008) && transitionHold==0;
        mix+=mixCoeff*((ready?1.0f:0.0f)-mix);
        if (!ready && mix<0.00001f) mix=0.0f;
        if (!ready && mix<=0.0f)
        {
            // Reset detector state during long inactivity rather than reuse
            // high-band averages from a previous syllable.
            recentLow=oldLow=recentHighEnergy=oldHighEnergy=0.0f;
            textureGate=1.0f;
            recentTextureReference=oldTextureReference=0.0f;
            effectiveMix=0.0f;
            ++now;
            return fallback;
        }
        if (delay<minAge || delay>maxAge || !std::isfinite(delay))
        {
            delay=0.5*(minAge+maxAge);
            oldDelay=delay;
            fadeLength=0;
        }
        const double slope=1.0-(double)ratio;
        delay+=slope;
        if (fadeLength>0) oldDelay+=slope;
        if (fadeLength==0 && ready)
        {
            // Move a whole source cycle in the read-head phase domain.
            // We preserve the old grain during a short causal crossfade.
            if (delay<minAge+0.2*(double)period && slope<0.0)
            {
                oldDelay=delay;
                delay+=(double)period;
                delay=alignWholeCycle(delay,oldDelay,minAge,maxAge);
                fadeLength=std::max(8,(int)std::lround(0.30*(double)period));
                fadePos=0;
            }
            else if (delay>maxAge-0.2*(double)period && slope>0.0)
            {
                oldDelay=delay;
                delay-=(double)period;
                delay=alignWholeCycle(delay,oldDelay,minAge,maxAge);
                fadeLength=std::max(8,(int)std::lround(0.30*(double)period));
                fadePos=0;
            }
        }
        delay=std::clamp(delay,minAge,maxAge);
        oldDelay=std::clamp(oldDelay,minAge,maxAge);
        float carrier=read((double)now-delay);
        if (fadeLength>0)
        {
            const float t=(float)(fadePos+1)/(float)fadeLength;
            const float k=0.5f-0.5f*std::cos(3.14159265f*t);
            carrier=(1.0f-k)*read((double)now-oldDelay)+k*carrier;
            if (++fadePos>=fadeLength) fadeLength=0;
        }
        // Transfer the recent vocal *amplitude* envelope onto the old cycle.
        // Do not use sample-wise ratios, which destroy zero crossings.
        const float r=std::clamp(recent,-4.0f,4.0f);
        const float c=std::clamp(carrier,-4.0f,4.0f);
        recentEnergy+=levelCoeff*(r*r-recentEnergy);
        oldEnergy+=levelCoeff*(c*c-oldEnergy);
        const float target=std::clamp(std::sqrt((recentEnergy+1.e-8f)/(oldEnergy+1.e-8f)),0.0f,1.8f);
        envelopeGain+=envelopeCoeff*(target-envelopeGain);
        float gated=mix;
        if (mode==EnvelopeMode::recentEnvelope)
        {
            if (textureGuardEnabled)
            {
            // Recent and historical voice can have similar loudness but
            // different formants. Track the *fraction* of power above 270 Hz
            // independently in each; mismatched timbre fades back toward the
            // existing short carrier until the older source catches up.
            // All state is per channel, finite, bounded and allocation-free.
            recentLow += textureLowCoeff*(r-recentLow);
            oldLow += textureLowCoeff*(c-oldLow);
            const float recentHigh=r-recentLow, oldHigh=c-oldLow;
            recentHighEnergy += textureEnergyCoeff*(recentHigh*recentHigh-recentHighEnergy);
            oldHighEnergy += textureEnergyCoeff*(oldHigh*oldHigh-oldHighEnergy);
            const float recentFraction=(recentHighEnergy+1.e-8f)/(recentEnergy+1.e-7f);
            const float oldFraction=(oldHighEnergy+1.e-8f)/(oldEnergy+1.e-7f);
            if (recentTextureReference<=0.0f)
            {
                recentTextureReference=recentFraction;
                oldTextureReference=oldFraction;
            }
            recentTextureReference+=textureReferenceCoeff*(recentFraction-recentTextureReference);
            oldTextureReference+=textureReferenceCoeff*(oldFraction-oldTextureReference);
            // Compare *changes* in spectral tilt, not absolute differences:
            // a pitch-shifted carrier can legitimately have different tilt
            // even on a perfectly stationary vowel.
            const float recentChange=recentFraction/(recentTextureReference+1.e-6f);
            const float oldChange=oldFraction/(oldTextureReference+1.e-6f);
            const float contrast=std::min(recentChange/oldChange,oldChange/recentChange);
            const float goal=std::clamp((contrast-0.40f)/0.25f,0.0f,1.0f);
            textureGate+=textureGateCoeff*(goal-textureGate);
            gated*=textureGate;
            }
            // Release quickly on silence, preventing an old voiced cycle from
            // ringing through the following unvoiced/consonant region.
            const float normalizedRecent=recentEnergy/(oldEnergy+1.e-7f);
            // An old cycle cannot represent a just-started consonant or
            // syllable, even if its envelope is brought up to date.
            // During a fast attack/release, use the short-delay carrier
            // instead. The hard activity check deliberately precedes
            // the slower gain-envelope smoothing.
            if (normalizedRecent<0.30f) gated *= std::clamp(normalizedRecent/0.30f,0.0f,1.0f);
            if (normalizedRecent>2.0f) gated *= std::clamp(4.0f/normalizedRecent-1.0f,0.0f,1.0f);
            carrier *= envelopeGain;
        }
        effectiveMix=gated;
        const float out=fallback+gated*(carrier-fallback);
        ++now;
        return boundedAudioSample(out);
    }
private:
    // A bounded, backward-looking phase fit at each *whole-cycle* wrap.
    // Frequency estimators can miss a pulse by a few samples, and simply
    // crossfading two correlated-but-opposite phases erases low harmonics.
    // Match the candidate to the previous read head over a broad fraction of
    // the source period, not a single sample or an arbitrary block boundary.
    // Never shift the schedule by more than 5% of one period; this prevents
    // a tempting high-harmonic match from becoming a half-cycle/octave jump.
    // The fit runs once per wrap, uses fixed stack scalars and past input only.
    double alignWholeCycle(double candidate, double previous,
                           double minimum, double maximum) noexcept
    {
        ++cycleWrapCount;
        const int radius=std::max(1,std::min(16,(int)std::floor(0.05*(double)period)));
        const double span=0.68*(double)period;
        double bestScore=-2.0, bestDelay=candidate;
        double bestEnergy=0.0;
        for (int shift=-radius; shift<=radius; shift+=2)
        {
            const double proposed=candidate+(double)shift;
            if (proposed<minimum || proposed>maximum) continue;
            double dot=0.0,ea=0.0,eb=0.0;
            for (int k=0;k<28;++k)
            {
                const double offset=4.0+span*(double)k/27.0;
                const double x=(double)read((double)now-previous-offset);
                const double y=(double)read((double)now-proposed-offset);
                dot+=x*y;
                ea+=x*x;eb+=y*y;
            }
            if (ea < 1.e-7 || eb < 1.e-7) continue;
            const double score=dot/std::sqrt(ea*eb);
            // Mild inertia prevents unbiased noise from walking source phase.
            const double adjusted=score-0.001*(double)(shift*shift)/(double)(radius*radius);
            if (adjusted>bestScore)
            {bestScore=adjusted; bestDelay=proposed; bestEnergy=std::min(ea,eb);}
        }
        if (bestScore>0.62 && bestEnergy>1.e-7)
        {
            // Refine the winning integer location to a half sample; the
            // historical carrier uses cubic interpolation, not hard indices.
            for (double sub : {-0.5,0.5})
            {
                const double proposed=bestDelay+sub;
                if (proposed<minimum || proposed>maximum)continue;
                double dot=0.0,ea=0.0,eb=0.0;
                for (int k=0;k<28;++k)
                {
                    const double offset=4.0+span*(double)k/27.0;
                    const double x=(double)read((double)now-previous-offset);
                    const double y=(double)read((double)now-proposed-offset);
                    dot+=x*y;ea+=x*x;eb+=y*y;
                }
                const double score=dot/std::sqrt(std::max(1.e-14,ea*eb));
                if (score>bestScore)
                {bestScore=score;bestDelay=proposed;}
            }
            ++phaseAlignedWrapCount;
        }
        else bestDelay=candidate;
        lastWrapCorrelation=(float)std::clamp(bestScore, -1.0, 1.0);
        wrapCorrelationTotal+=(double)lastWrapCorrelation;
        return bestDelay;
    }
    float read(double pos) const noexcept
    {
        // The newest +2 cubic neighbour MUST be <= now to remain causal.
        pos=std::min(pos,(double)now-2.0);
        const int64_t i=(int64_t)std::floor(pos);
        const float f=(float)(pos-(double)i);
        const float a=ring[(size_t)((i-1)&mask)];
        const float b=ring[(size_t)(i&mask)];
        const float c=ring[(size_t)((i+1)&mask)];
        const float d=ring[(size_t)((i+2)&mask)];
        return ((0.5f*(d-a)+1.5f*(b-c))*f+(a-2.5f*b+2.f*c-0.5f*d))*f*f
                 +0.5f*(c-a)*f+b;
    }
    std::vector<float> ring;
    int64_t mask=0,now=0;
    double sr=48000.0, delay=0, oldDelay=0;
    int recentDelay=168,fadePos=0,fadeLength=0;
    float period=0,ratio=1,mix=0,mixCoeff=0.001f;
    float previousPeriod=0.0f, smoothedRatio=1.0f;
    int stableSamples=0,transitionHold=0;
    float recentEnergy=1.e-6f,oldEnergy=1.e-6f,envelopeGain=1.0f;
    float levelCoeff=0.001f,envelopeCoeff=0.001f,lastRecentDry=0;
    float ratioTrackingCoeff=0.001f, textureLowCoeff=0.04f;
    float textureEnergyCoeff=0.01f, textureGateCoeff=0.02f,textureReferenceCoeff=0.0005f;
    float recentLow=0.0f,oldLow=0.0f,recentHighEnergy=0.0f,oldHighEnergy=0.0f;
    float textureGate=1.0f,effectiveMix=0.0f;
    float recentTextureReference=0.0f,oldTextureReference=0.0f;
    EnvelopeMode mode=EnvelopeMode::none;
    bool textureGuardEnabled=false;
    uint64_t cycleWrapCount=0,phaseAlignedWrapCount=0;
    float lastWrapCorrelation=0.0f;
    double wrapCorrelationTotal=0.0;
};

//==============================================================================
// Opt-in fixed-clock research renderer (V1.2). This has a precise *causal
// information horizon*: NO audio sample or pitch-control value newer than
// output time minus fixedDelay may affect the output. Dry/unvoiced waveforms
// have exactly fixedDelay samples of transport delay. A resynthesized carrier
// necessarily consults older pitch cycles and hence DOES NOT have a single
// exact input-to-output wet waveform/group delay. Report both facts honestly.
//
// All rings are allocated in prepare(); processSample is bounded and noexcept.
// Upstream pitch-control events are timestamped with their source audio sample;
// the event queue is delayed by the identical amount as the audio's anchor.
class FixedHorizonVoicedRenderer
{
public:
    static int hostLatencySamples(double sampleRate) noexcept
    { return (int) std::ceil(std::max(8000.0,sampleRate) * 0.080); }

    void prepare(double sampleRate)
    {
        delay = hostLatencySamples(sampleRate);
        size_t size=1;
        while(size < (size_t)(delay+8)) size <<= 1;
        periodHistory.assign(size,0.0f);
        ratioHistory.assign(size,1.0f);
        mask = (int64_t)size-1;
        // Guard +4 accounts for both cubic interpolation's +2 neighbour
        // and the youngest moving read head; no interpolated sample may
        // read newer audio than the published delay horizon.
        fast.prepare(sampleRate, delay+4);
        historical.prepare(sampleRate,delay);
        historical.setEnvelopeMode(HistoricalCycleExperiment::EnvelopeMode::recentEnvelope);
        reset();
    }

    void reset() noexcept
    {
        std::fill(periodHistory.begin(),periodHistory.end(),0.0f);
        std::fill(ratioHistory.begin(),ratioHistory.end(),1.0f);
        now=0;
        fast.reset(); historical.reset();
    }
    int getHostLatencySamples() const noexcept { return delay; }
    float getLastAlignedDry() const noexcept { return historical.getLastRecentDry(); }
    float getHistoricalMix() const noexcept { return historical.getEffectiveHistoricalMix(); }
    float getCarrierSourceAgeSamples() const noexcept { return historical.getCarrierSourceAgeSamples(); }
    int getMaxCarrierSourceAgeSamples() const noexcept { return historical.getMaximumCarrierSourceAgeSamples(); }

    float processSample(float input, float sourcePeriod, float pitchRatio) noexcept
    {
        input=boundedAudioSample(input);
        periodHistory[(size_t)(now&mask)] = std::isfinite(sourcePeriod) ? sourcePeriod : 0.0f;
        ratioHistory[(size_t)(now&mask)] = std::isfinite(pitchRatio) ? pitchRatio : 1.0f;
        const int64_t controlIndex=now-(int64_t)delay;
        const float delayedPeriod=controlIndex>=0 ? periodHistory[(size_t)(controlIndex&mask)] : 0.0f;
        const float delayedRatio=controlIndex>=0 ? ratioHistory[(size_t)(controlIndex&mask)] : 1.0f;
        fast.setPeriod(delayedPeriod);
        fast.setRatio(delayedPeriod>0.0f ? delayedRatio : 1.0f);
        const float highCarrier=fast.processSample(input);
        historical.setParameters(delayedPeriod,delayedPeriod>0.0f ? delayedRatio : 1.0f);
        const float correction=std::abs(delayedRatio-1.0f);
        const float historicalWet=historical.processSample(input,highCarrier);
        ++now;
        // The neutral timeline, including transitions into silence/consonants,
        // must remain exactly time stamped. Do NOT expose micro's older neutral
        // tap or a warm-started pitch cycle when no correction is requested.
        if (delayedPeriod<=0.0f || correction<=0.0003f)
            return getLastAlignedDry();
        return historicalWet;
    }
private:
    MicroLatencyShifter fast;
    HistoricalCycleExperiment historical;
    std::vector<float> periodHistory,ratioHistory;
    int delay=0;
    int64_t now=0,mask=0;
};

//==============================================================================
// Real-time TD-PSOLA: Hann grains cut at the input pitch period and re-laid at
// period / pitchRatio. Formants stay put, so the voice doesn't turn chipmunk.
class PsolaShifter
{
public:
    // The shifter is an independently clocked, pulse-synchronous overlap-add
    // synthesizer.  Input and output grain schedules are separate: changing the
    // synthesis-mark spacing changes F0 without uniformly shifting the vocal
    // envelope.  All scratch space is allocated in prepare().
    void prepare (double sampleRate)
    {
        sr = std::max (8000.0, sampleRate);
        size_t n = 1;
        while (n < (size_t) (sr * 0.5)) n <<= 1;
        history.assign (n, 0.0f);
        mask = (int64_t) n - 1;
        unvoicedPeriod = (float) (sr * 0.008);
        maxPeriod = (float) (sr * 0.030);
        voicedAlpha = 1.0f - std::exp (-1.0f / (float) (sr * 0.0015));
        unityAlpha = 1.0f - std::exp (-1.0f / (float) (sr * 0.006));
        unityExitAlpha = 1.0f - std::exp (-1.0f / (float) (sr * 0.0018));
        levelAlpha = 1.0f - std::exp (-1.0f / (float) (sr * 0.035));
        // Bounded, reusable NCC template. No allocation on the audio thread.
        matchTemplate.assign ((size_t) (0.5 * maxPeriod) + 32, 0.0f);
        // Fixed-size past wet waveform for phase-matching near-unison handoffs.
        // Allocated only at prepare(), never on the audio thread.
        wetHistory.assign (2048, 0.0f);
        for (size_t i = 0; i < window.size(); ++i)
            window[i] = 0.5f - 0.5f * std::cos (6.28318530718f * (float) i / (float) (window.size() - 1));
        reset();
    }

    void reset() noexcept
    {
        std::fill (history.begin(), history.end(), 0.0f);
        for (auto& g : grains) g.active = false;
        now = 0;
        nextSynthesisMark = 0.0;
        lastSourceMark = 0.0;
        sourceValid = false;
        lowCoherenceGrains = 0;
        phaseReacquisitions = 0;
        ratio = 1.0f;
        inputPeriod = unvoicedPeriod;
        pitched = false;
        voicedMix = 0.0f;
        // Hold the dry tap steady in unvoiced segments; it only changes when
        // the voiced timeline changes, crossfaded over several milliseconds.
        dryDelay = std::round (unvoicedPeriod * 0.65f);
        previousDryDelay = dryDelay;
        dryFade = 1.0f;
        latencyEstimate = dryDelay;
        inputPower = outputPower = 1.0e-7f;
        gain = desiredGainSmoothed = 1.0f;
        unityMix = 0.0f;
        unityDelay = dryDelay;
        recentlyShifted = false;
        unityRequested = false;
        std::fill (wetHistory.begin(), wetHistory.end(), 0.0f);
        levelCounter = 0;
        dropoutCount = 0;
        overlapWeightSum = lastPhaseCorrelation = lastSourceAgeSamples = 0.0f;
        emittedGrains = 0;
        saturatedGrains = 0;
    }

    void setLowLatency (bool enabled) noexcept { fast = enabled; }

    void setParameters (float newRatio, float period) noexcept
    {
        const bool wasPitched = pitched;
        pitched = std::isfinite (period) && period > 0.0f;
        // Consonants are broadband: fractional interpolated dry taps act as a
        // low-pass and can quietly erase several dB of their energy. Move to
        // an exact integer-sample passthrough tap once voicing is lost.
        if (wasPitched && ! pitched)
        {
            sourceValid = false;
            lastPhaseCorrelation = 0.0f;
            previousDryDelay = dryDelay;
            dryDelay = std::round (dryDelay);
            dryFade = 0.0f;
        }
        const float incomingPeriod = pitched ? std::clamp (period, 8.0f, maxPeriod) : unvoicedPeriod;
        // A genuine legato note jump invalidates the old glottal-template
        // phase: don't ask NCC to find the new vowel using the previous
        // note's pulse period. This is separate from the ratio glide.
        if (wasPitched && pitched && sourceValid &&
            std::abs (incomingPeriod - inputPeriod) > inputPeriod * 0.16f)
        {
            sourceValid = false;
            lowCoherenceGrains = 0;
            ++phaseReacquisitions;
        }
        inputPeriod = incomingPeriod;
        ratio = pitched && std::isfinite (newRatio) ? std::clamp (newRatio, 0.25f, 4.0f) : 1.0f;
        if (! pitched) recentlyShifted = false;
        else if (std::abs (ratio - 1.0f) > 0.002f) recentlyShifted = true;
    }

    float processSample (float input) noexcept
    {
        input = boundedAudioSample(input);
        history[(size_t) (now & mask)] = input;

        // Once fully unvoiced, use the exact integer-delay dry path instead
        // of reconstructing fictitious grains or scanning the grain bank.
        // Keep the ring history warm so the next real note can begin immediately.
        if (! pitched && voicedMix == 0.0f && dryFade >= 1.0f && unityMix == 0.0f)
        {
            nextSynthesisMark = (double) now;
            sourceValid = false;
            const float dry = read ((double) now - (double) dryDelay);
            // Do not carry an old pitched gain estimate across long breaths.
            gain = desiredGainSmoothed = 1.0f;
            inputPower = outputPower = 1.0e-7f;
            ++now;
            return dry;
        }

        // A real-time-safe bound on grain scheduling, including corrupted
        // automation / non-monotonic periods. Prevent callback stalls.
        int scheduled = 0;
        // Unvoiced audio must not drive a fake pitch-synchronous oscillator.
        // Suspend grain spawning on breaths/silence and restart the timeline
        // at the next voiced onset. Existing grains are allowed to decay.
        if (! pitched) nextSynthesisMark = (double) now;
        while (pitched && nextSynthesisMark - halfWidth() <= (double) now && scheduled < 8)
        {
            spawn();
            ++scheduled;
        }
        if (pitched && scheduled == 8 && nextSynthesisMark <= (double) now)
            nextSynthesisMark = (double) now + (double) inputPeriod / (double) ratio;

        float sum = 0.0f, weight = 0.0f;
        double delaySum = 0.0;
        for (auto& g : grains)
        {
            if (! g.active) continue;
            const double age = (double) now - g.start;
            if (age >= g.length) { g.active = false; continue; }
            if (age < 0.0) continue;
            const float w = windowAt ((float) (age / g.length));
            sum += w * read (g.sourceStart + age);
            weight += w;
            delaySum += (double) w * g.delay;
        }
        // The normalized overlap weights are the SAME windows that generate
        // audio. Never assume the sum equals one and never apply ratio-dependent
        // arbitrary gain. Smoothly substitute delayed raw audio when grains do
        // not cover a sample instead of dividing by an almost-zero weight.
        overlapWeightSum = weight;
        const float coverage = std::clamp (weight * 100000.0f, 0.0f, 1.0f);
        const float wet = weight > 1.0e-6f ? sum / weight : 0.0f;
        if (pitched && coverage < 0.05f) ++dropoutCount;
        if (weight > 0.15f)
            latencyEstimate += 0.005 * (std::clamp (delaySum / (double) weight, 0.0, (double) maxPeriod * 2.5) - latencyEstimate);

        const float goal = pitched ? 1.0f : 0.0f;
        voicedMix += (goal - voicedMix) * voicedAlpha;
        if (std::abs (voicedMix - goal) < 1.0e-5f) voicedMix = goal;

        // The unpitched sample follows the last synchronized source delay. On
        // transition from voice to breath, keep the delay fixed, avoiding the
        // rapidly changing time-varying dry tap used in the original shifter.
        const float dryB = read ((double) now - (double) dryDelay);
        const float dry = dryFade >= 1.0f ? dryB
            : read ((double) now - (double) previousDryDelay) * (1.0f - dryFade) + dryFade * dryB;
        dryFade = std::min (1.0f, dryFade + 1.0f / (float) (sr * 0.002));

        // Slow energy matching, derived from actual dry and reconstructed
        // signals rather than a fixed sqrt(1/ratio) multiplier. It cannot
        // compensate for holes: those are handled by coverage above.
        // Compare the resynthesis against the *actual incoming* vocal. The
        // moving dry-grain tap is not a reliable loudness reference: its
        // time variation can substantially reduce measured energy and make
        // the controller attenuate an already-quiet output.
        const float dryPower = input * input;
        inputPower += levelAlpha * (dryPower - inputPower);
        outputPower += levelAlpha * (wet * wet - outputPower);
        if ((++levelCounter & 63u) == 0)
        {
            // The fast narrow windows can be ~2.4 dB down during octave-up
            // correction. Do not boost ALL vocals: allow the measured RMS
            // servo extra headroom only for large upward shifts. The existing
            // power ratio decides the actual correction, with a firm ceiling.
            // Normalize measured energy over meaningful pitch shifts. Close
            // to unison this stays near the original 1.20 ceiling; stronger
            // shifts may need additional headroom when grain phases cancel.
            const float signedShift = 12.0f * std::log2 (std::max (0.25f, ratio));
            const float significantShift = std::clamp ((std::abs (signedShift) - 0.5f) / 3.0f, 0.0f, 1.0f);
            const float gainCeiling = 1.20f + 0.65f * significantShift;
            desiredGainSmoothed = pitched
                ? std::clamp (std::sqrt ((inputPower + 1.0e-6f) /
                                          (outputPower + 1.0e-6f)), 0.83f, gainCeiling) : 1.0f;
        }
        gain += levelAlpha * (desiredGainSmoothed - gain);

        const float coherentWet = coverage * (wet * gain) + (1.0f - coverage) * dry;
        // Zero-shift transparency: conventional PSOLA still phase-modulates
        // vocal harmonics when the correction is *exactly* zero. Crossfade to
        // a fixed-delay copy of the original signal for near-unison regions.
        // Capture the grain delay on entry so both paths begin close in phase;
        // hold it while in unity mode so the dry copy has no pitch wobble.
        // abs(ratio-1) ~= ln(2) * cents / 1200 at the unison threshold.
        // Avoid a logarithm on every audio sample.
        // Hysteresis avoids flipping the transparent path at every tiny
        // vibrato/automation fluctuation around its entry threshold.
        const bool priorRequest = unityRequested;
        const float unityError = std::abs (ratio - 1.0f);
        if (! pitched || (unityRequested && unityError > 0.0018f)) unityRequested = false;
        else if (! unityRequested && unityError < 0.000866f) unityRequested = true;
        const float unityGoal = pitched && unityRequested ? 1.0f : 0.0f;
        // Match the fixed-delay original to RECENT wet waveform before entering
        // unity mode; measuring only the wet delay estimate is insufficient to
        // establish harmonic phase. Once acquired, the delay stays stationary
        // through the fade, avoiding a continuously pitch-modulated dry tap.
        if (unityGoal > 0.5f && ! priorRequest)
        {
            // A note that began at exact unison should stay on the original
            // short, transparent tap. Phase searching is needed only when a
            // previously shifted note *returns* to unison.
            unityDelay = recentlyShifted ? findPhaseAlignedUnityDelay()
                : std::clamp ((float) latencyEstimate, 6.0f,
                              fast ? (float) (sr * 0.0015) : 2.4f * maxPeriod);
        }
        // The wet waveform begins changing phase immediately when retuning
        // resumes. Exit the transparent path faster than we enter it, so two
        // increasingly different waveforms are not mixed for several cycles.
        const float unityStep = unityGoal > unityMix ? unityAlpha : unityExitAlpha;
        unityMix += unityStep * (unityGoal - unityMix);
        if (std::abs (unityMix - unityGoal) < 1.0e-5f) unityMix = unityGoal;
        float voiced = coherentWet;
        if (unityMix > 1.0e-5f)
        {
            const float transparentDry = read ((double) now - (double) unityDelay);
            voiced += unityMix * (transparentDry - coherentWet);
        }
        if (unityMix > 0.995f) latencyEstimate += 0.005 * ((double) unityDelay - latencyEstimate);
        const float result = voicedMix * voiced + (1.0f - voicedMix) * dry;
        if (! wetHistory.empty())
            wetHistory[(size_t) (now & 2047)] = coherentWet;
        ++now;
        return std::isfinite (result) ? result : 0.0f;
    }

    double getLatencySamples() const noexcept { return latencyEstimate; }
    float getOverlapWeightSum() const noexcept { return overlapWeightSum; }
    float getLastPhaseCorrelation() const noexcept { return lastPhaseCorrelation; }
    float getLastSourceAgeSamples() const noexcept { return lastSourceAgeSamples; }
    float getAppliedGain() const noexcept { return gain; }
    int getActiveGrainCount() const noexcept
    {
        int count = 0;
        for (const auto& grain : grains) if (grain.active) ++count;
        return count;
    }
    uint64_t getDropoutSamples() const noexcept { return dropoutCount; }
    uint64_t getSpawnedGrains() const noexcept { return emittedGrains; }
    uint64_t getGrainSaturationCount() const noexcept { return saturatedGrains; }
    uint64_t getPhaseReacquisitions() const noexcept { return phaseReacquisitions; }

private:
    float findPhaseAlignedUnityDelay() const noexcept
    {
        // The analysis window is short enough to follow current pulse phase,
        // but covers several harmonics. Strictly bounded ~2k dot products per
        // handoff, triggered only on entering the <1.5-cent unity region.
        const float base = std::clamp ((float) latencyEstimate, 6.0f,
                                         (float) (sr * (fast ? 1.95 : 2.45) / 70.0));
        if (now < (int64_t) (sr * 0.045) || wetHistory.empty()) return base;
        const float radius = std::min (inputPeriod * 0.70f, (float) (sr * 0.008));
        const float low = std::max (6.0f, base - radius);
        const float high = std::min ((float) (sr * (fast ? 1.95 : 2.45) / 70.0), base + radius);
        const int taps = 48;
        const int stride = 3;
        float bestScore = -1.0f;
        float bestDelay = base;
        const int count = 33;
        for (int c = 0; c < count; ++c)
        {
            const float candidate = low + (high - low) * (float) c / (float) (count - 1);
            double cross = 0.0, wetEnergy = 1.0e-10, dryEnergy = 1.0e-10;
            for (int k = 1; k <= taps; ++k)
            {
                const int sampleOffset = k * stride;
                const float wet = wetHistory[(size_t) ((now - sampleOffset) & 2047)];
                const float dry = read ((double) now - (double) sampleOffset - candidate);
                cross += (double) wet * dry;
                wetEnergy += (double) wet * wet;
                dryEnergy += (double) dry * dry;
            }
            const float normalized = (float) (cross / std::sqrt (wetEnergy * dryEnergy));
            // Prefer proximity to the physically measured source delay when
            // multiple harmonically equivalent phase solutions are available.
            const float score = normalized - 0.035f * std::abs (candidate - base) / std::max (20.0f, radius);
            if (score > bestScore) { bestScore = score; bestDelay = candidate; }
        }
        // If noise/unvoiced audio has no meaningful coherence, prefer the
        // stable reported delay rather than making a random phase jump.
        return bestScore > 0.32f ? bestDelay : base;
    }

    struct Grain
    {
        bool active = false;
        double start = 0.0, length = 0.0, sourceStart = 0.0, delay = 0.0;
    };

    float halfWidth() const noexcept
    {
        // Long overlapping windows can cancel nearly completely during large
        // upward corrections (multiple simultaneous source periods). Narrow
        // them only when needed, keeping normal-mode quality near unison.
        if (! fast)
        {
            // Gradually shorten the wide normal-mode window as an upward
            // correction grows. A hard switch at 1.3x would itself create
            // timbral zippering under glide/retune automation.
            const float narrowWidth = std::max (0.52f, 0.90f / std::max (0.25f, ratio));
            const float transition = std::clamp ((ratio - 1.1f) * 2.0f, 0.0f, 1.0f);
            const float normalWidth = 1.05f + transition * (std::min (narrowWidth, 1.05f) - 1.05f);
            return std::min (maxPeriod, inputPeriod * normalWidth);
        }
        const float defaultWidth = std::max (0.515f, 0.55f / std::max (0.25f, ratio));
        // Larger upward changes need a little more overlap to avoid an
        // underpowered two-grain junction. Only alter ratios above 1.5x:
        // normal correction and its latency budget stay unchanged.
        const float largeUpward = std::clamp ((ratio - 1.5f) * 2.0f, 0.0f, 1.0f);
        const float width = defaultWidth + largeUpward * (std::max (defaultWidth, 0.62f) - defaultWidth);
        return std::min (maxPeriod, inputPeriod * width);
    }

    void spawn() noexcept
    {
        const float hw = halfWidth();
        const double centre = nextSynthesisMark;
        nextSynthesisMark += (double) inputPeriod / (double) ratio;
        if (nextSynthesisMark <= centre) nextSynthesisMark = centre + 2.0;

        // Always choose source grains from a region ALREADY available at the
        // time of scheduling, including the interpolation neighbourhood.
        const double templateRadius = std::max (4.0, (fast ? 0.06 : 0.17) * (double) inputPeriod);
        // Expand the search after a weak phase match, without growing the
        // number of NCC candidates or adding any audio lookahead.
        const double reach = (fast ? (lowCoherenceGrains > 0 ? 0.09 : 0.04) : 0.13) * (double) inputPeriod;
        const double desired = centre - ((double) hw + reach + templateRadius + 4.0
                                        + (fast ? 0.005 : 0.12) * (double) inputPeriod);
        const double latest = (double) now - templateRadius - 3.0;
        double sourceMark = std::min (desired, latest);

        if (! pitched)
        {
            sourceValid = false;
        }
        else if (sourceValid)
        {
            // Synthesis marks advance at target F0; source marks advance on
            // source-period boundaries. This preserves the spectral envelope.
            const double count = std::floor ((desired - lastSourceMark) /
                                              (double) inputPeriod + 0.20);
            const double expected = lastSourceMark + count * (double) inputPeriod;
            sourceMark = expected > latest ? expected - std::ceil ((expected - latest) / (double) inputPeriod) * (double) inputPeriod : expected;
            const int radius = (int) std::ceil (reach);
            const int h = std::min ((int) templateRadius, ((int) matchTemplate.size() - 1) / 2);
            if (lastSourceMark >= (double) h + 2.0 && h >= 4 && radius > 0)
            {
                // NCC must normalize using the SAME temporal samples in the
                // reference and candidate. Earlier versions summed reference
                // energy at full resolution but candidate energy on a decimated
                // subset, artificially depressing match scores for low notes.
                const int sampleStride = h > 100 ? 4 : (h > 40 ? 2 : 1);
                float refEnergy = 1.0e-10f;
                for (int j = -h; j <= h; j += sampleStride)
                {
                    const float x = read (lastSourceMark + (double) j);
                    matchTemplate[(size_t) (j + h)] = x;
                    refEnergy += x * x;
                }
                const int lo = (int) std::ceil (sourceMark - (double) radius);
                const int hi = (int) std::floor (std::min (sourceMark + (double) radius, latest));
                float best = -1.0f;
                double bestCentre = sourceMark;
                // Coarse-to-fine normalized correlation. Hard upper work bound
                // (at most 25 + 7 candidates) reduces worst-case callback CPU.
                const int stride = std::max (1, (hi - lo) / 12);
                for (int pass = 0; pass < 2; ++pass)
                {
                    const int from = pass == 0 ? lo : std::max (lo, (int) bestCentre - stride);
                    const int to = pass == 0 ? hi : std::min (hi, (int) bestCentre + stride);
                    const int step = pass == 0 ? stride : std::max (1, (stride + 3) / 4);
                    for (int k = from; k <= to; k += step)
                    {
                        float dot = 0.0f, e = 1.0e-10f;
                        for (int j = -h; j <= h; j += sampleStride)
                        {
                            const float x = read ((double) k + (double) j);
                            dot += x * matchTemplate[(size_t) (j + h)];
                            e += x * x;
                        }
                        const float score = dot / std::sqrt (e * refEnergy);
                        if (score > best)
                        {
                            best = score;
                            bestCentre = (double) k;
                        }
                    }
                }
                // Normal mode has enough grain/latency headroom for fractional
                // refinement. Live mode deliberately retains the existing
                // integer match to protect its tighter phase/latency bounds.
                // Coarse/fine NCC above works at integer input samples. A
                // persistent fractional source period (e.g. 48k / 213 Hz)
                // otherwise quantizes the pulse centre by up to half a sample
                // at every grain. Estimate the local NCC maximum with one
                // bounded, three-point fractional refinement. The synthesized
                // clock remains continuous; only the source pulse's alignment
                // is refined. Unlike per-buffer oscillator phase resets this
                // introduces no extra lookahead and never allocates.
                if (! fast && best > 0.55f && bestCentre >= (double) lo + 1.0 &&
                    bestCentre <= (double) hi - 1.0)
                {
                    const auto fractionalScore = [&] (double position) noexcept
                    {
                        float dot = 0.0f, energy = 1.0e-10f;
                        for (int j = -h; j <= h; j += sampleStride)
                        {
                            const float x = read (position + (double) j);
                            dot += x * matchTemplate[(size_t) (j + h)];
                            energy += x * x;
                        }
                        return dot / std::sqrt (energy * refEnergy);
                    };
                    const float left = fractionalScore (bestCentre - 0.5);
                    const float mid = best; // Already evaluated at the integer winner.
                    const float right = fractionalScore (bestCentre + 0.5);
                    const float curvature = left - 2.0f * mid + right;
                    if (curvature < -1.0e-5f)
                    {
                        const float delta = std::clamp (0.25f * (left - right) / curvature,
                                                        -0.5f, 0.5f);
                        const float refined = fractionalScore (bestCentre + (double) delta);
                        if (refined > best)
                        {
                            best = refined;
                            bestCentre += (double) delta;
                        }
                    }
                }
                lastPhaseCorrelation = best;
                if (best > 0.44f)
                {
                    sourceMark = bestCentre;
                    if (best > 0.70f) lowCoherenceGrains = 0;
                }
                else
                {
                    if (++lowCoherenceGrains >= 4)
                    {
                        // A stale source template can lock consecutive grains
                        // to a former syllable and generate phase-cancellation
                        // bursts. Reacquire once after repeated weak NCC.
                        sourceValid = false;
                        lowCoherenceGrains = 0;
                        ++phaseReacquisitions;
                    }
                }
            }
        }

        if (pitched)
        {
            lastSourceMark = sourceMark;
            lastSourceAgeSamples = (float) std::clamp ((double) now - sourceMark, 0.0, (double) (sr * 0.5));
            sourceValid = true;
            const float nextDelay = (float) std::clamp (centre - sourceMark, 6.0, 2.4 * (double) maxPeriod);
            // Update the passthrough tap with a crossfade only when entering
            // a new source-delay region (not for every input sample).
            if (std::abs (nextDelay - dryDelay) > 2.0f && dryFade >= 1.0f)
            {
                previousDryDelay = dryDelay;
                dryDelay = nextDelay;
                dryFade = 0.0f;
            }
        }

        // During attack/startup the source window may refer to history before
        // sample zero. The ring is initialized to silence, never stale memory.
        bool placed = false;
        for (auto& g : grains)
        {
            if (g.active && (double) now - g.start >= g.length) g.active = false;
            if (g.active) continue;
            g.active = true;
            g.start = centre - (double) hw;
            g.length = 2.0 * (double) hw;
            g.sourceStart = sourceMark - (double) hw;
            g.delay = centre - sourceMark;
            ++emittedGrains;
            placed = true;
            break;
        }
        if (! placed) ++saturatedGrains; // separately report events lost at the grain-bank limit
    }

    float windowAt (float position) const noexcept
    {
        const float index = std::clamp (position, 0.0f, 1.0f) * (float) (window.size() - 1);
        const size_t i = (size_t) index;
        const float a = index - (float) i;
        return i + 1 < window.size() ? window[i] + a * (window[i + 1] - window[i]) : window.back();
    }

    float read (double position) const noexcept
    {
        const int64_t ix = (int64_t) std::floor (position);
        const float frac = (float) (position - (double) ix);
        const float xm1 = history[(size_t) ((ix - 1) & mask)];
        const float x0 = history[(size_t) (ix & mask)];
        const float x1 = history[(size_t) ((ix + 1) & mask)];
        const float x2 = history[(size_t) ((ix + 2) & mask)];
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * frac + c2) * frac + c1) * frac + x0;
    }

    double sr = 48000.0;
    int64_t now = 0, mask = 0;
    std::vector<float> history, matchTemplate;
    std::array<Grain, 12> grains {};
    std::array<float, 1024> window {};
    double nextSynthesisMark = 0.0, lastSourceMark = 0.0, latencyEstimate = 200.0;
    float inputPeriod = 384.0f, ratio = 1.0f, unvoicedPeriod = 384.0f, maxPeriod = 1440.0f;
    float voicedAlpha = 0.005f, levelAlpha = 0.0006f, voicedMix = 0.0f;
    float dryDelay = 240.0f, previousDryDelay = 240.0f, dryFade = 1.0f;
    float unityMix = 0.0f, unityDelay = 240.0f, unityAlpha = 0.003f, unityExitAlpha = 0.01f;
    std::vector<float> wetHistory; // 2048-sample power-of-two ring
    float inputPower = 1.0e-7f, outputPower = 1.0e-7f, gain = 1.0f, desiredGainSmoothed = 1.0f;
    float overlapWeightSum = 0.0f, lastPhaseCorrelation = 0.0f, lastSourceAgeSamples = 0.0f;
    unsigned levelCounter = 0;
    bool sourceValid = false, fast = true, pitched = false, recentlyShifted = false, unityRequested = false;
    uint64_t dropoutCount = 0, emittedGrains = 0, saturatedGrains = 0;
    uint64_t phaseReacquisitions = 0;
    int lowCoherenceGrains = 0;
};

//==============================================================================
// Learns the key from sung pitches. Feed it detected MIDI notes (fractional), then ask
// for the best major/minor key. Relative major/minor share their notes, so for tuning
// purposes either answer snaps to the same pitches.
class KeyEstimator
{
public:
    struct Result { int key = 0; Scale scale = Scale::Major; float confidence = 0.0f; bool valid = false; };

    void reset() noexcept { histogram.fill (0.0f); total = 0.0f; }

    // weight: e.g. detector confidence times duration.
    void add (float midi, float weight = 1.0f) noexcept
    {
        if (! (midi > 0.0f) || weight <= 0.0f) return;
        const float f = midi - std::floor (midi);
        const int lo = ((int) std::floor (midi) % 12 + 12) % 12;
        histogram[(size_t) lo] += weight * (1.0f - f);
        histogram[(size_t) ((lo + 1) % 12)] += weight * f;     // soft assignment: out-of-tune notes still vote for both neighbours
        total += weight;
    }

    float getTotalWeight() const noexcept { return total; }

    Result estimate() const noexcept
    {
        Result r;
        if (total <= 0.0f) return r;

        // Krumhansl-Kessler key profiles.
        static constexpr float major[12] = { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
        static constexpr float minor[12] = { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };

        float best = -2.0f, second = -2.0f;
        for (int key = 0; key < 12; ++key)
            for (int mode = 0; mode < 2; ++mode)
            {
                const float c = correlation (mode == 0 ? major : minor, key);
                if (c > best)
                {
                    second = best; best = c;
                    r.key = key; r.scale = mode == 0 ? Scale::Major : Scale::Minor;
                }
                else if (c > second) second = c;
            }

        r.valid = true;
        r.confidence = std::clamp ((best - second) * 5.0f, 0.0f, 1.0f);
        return r;
    }

private:
    float correlation (const float* profile, int key) const noexcept
    {
        float mh = 0, mp = 0;
        for (int i = 0; i < 12; ++i) { mh += histogram[(size_t) i]; mp += profile[i]; }
        mh /= 12.0f; mp /= 12.0f;
        float num = 0, dh = 0, dp = 0;
        for (int i = 0; i < 12; ++i)
        {
            const float h = histogram[(size_t) ((i + key) % 12)] - mh, p = profile[i] - mp;
            num += h * p; dh += h * h; dp += p * p;
        }
        return dh > 0.0f && dp > 0.0f ? num / std::sqrt (dh * dp) : 0.0f;
    }

    std::array<float, 12> histogram {};
    float total = 0.0f;
};

// Offline helper: find the key of a whole mono recording.
inline KeyEstimator::Result estimateKey (const float* audio, size_t numSamples, double sampleRate, float referenceHz = 440.0f)
{
    YinDetector yin; yin.prepare (sampleRate);
    KeyEstimator est;
    for (size_t i = 0; i < numSamples; ++i)
        if (yin.push (audio[i]) && yin.getConfidence() > 0.85f && yin.getFrequency() > 0.0f)
            est.add (69.0f + 12.0f * std::log2 (yin.getFrequency() / referenceHz), yin.getConfidence());
    return est.estimate();
}

//==============================================================================
// V24: narrow guard for transient, unconfirmed large note jumps.
// The learned risk model remains diagnostic-only: controlled ablations found
// that raw YIN confidence is safer for this real-time decision on the available
// recording. This guard adds *decision persistence*, never audio lookahead.
struct LargeNoteJumpGuard final
{
    static float extraConfirmationSeconds (int currentMidi, int candidateMidi,
                                           float yinConfidence, bool strongOnset,
                                           bool precisionLive) noexcept
    {
        if (! precisionLive || currentMidi < 0 || candidateMidi < 0 || strongOnset
            || ! std::isfinite (yinConfidence))
            return 0.0f;
        return std::abs (candidateMidi - currentMidi) >= 7 && yinConfidence < 0.91f
            ? 0.030f : 0.0f;
    }
};

class RealtimeAutotune
{
public:
    struct Diagnostics
    {
        uint64_t analysisHop = 0;
        float detectedMidi = -1.0f, confidence = 0.0f, correctionSemitones = 0.0f;
        float targetRatio = 1.0f, smoothedRatio = 1.0f, estimatedPeriodSamples = 0.0f;
        // Scheduled grain clock, NOT a measurement of the rendered waveform's F0.
        float scheduledOutputPeriodSamples = 0.0f;
        float synthesisGain = 1.0f;
        float rawFrequencyHz = 0.0f, alternateFrequencyHz = 0.0f;
        float detectorFrequencyHz = 0.0f, twoPeriodAperiodicity = -1.0f;
        float learnedPitchReliability = 0.0f;
        float phaseCorrelation = 0.0f, overlapWeightSum = 0.0f, sourceAgeSamples = 0.0f;
        int targetMidi = -1, activeGrains = 0;
        uint64_t grainDropouts = 0, grainSaturations = 0, phaseReacquisitions = 0;
        bool voiced = false, pendingNote = false;
    };
    // minHz/maxHz bound the pitch search (default suits speech and singing; lower minHz for basses).
    void prepare (double newSampleRate, float minHz = 70.0f, float maxHz = 1000.0f)
    {
        sampleRate = newSampleRate;
        detector.prepare (sampleRate, minHz, maxHz);
        detector.setRealtimeTransitionWindowEnabled(settings.strictRealtime);
        detector.setPitchSynchronousWindowEnabled(settings.strictRealtime && settings.lowLatency
            && settings.pitchSynchronousLive);
        shifter.prepare (sampleRate);
        microShifter.prepare (sampleRate);
        qualityShifter.prepare (sampleRate, 4, latency_limits::qualityMs);
        historicalShifter.prepare (sampleRate);
        fixedHorizonShifter.prepare (sampleRate);
        for (auto& fixed : channelFixedHorizonShifters) fixed.prepare (sampleRate);
        historicalShifter.setEnvelopeMode (HistoricalCycleExperiment::EnvelopeMode::recentEnvelope);
        for (auto& historical : channelHistoricalShifters)
        {
            historical.prepare (sampleRate);
            historical.setEnvelopeMode (HistoricalCycleExperiment::EnvelopeMode::recentEnvelope);
        }
        for (auto& micro : channelMicroShifters) micro.prepare (sampleRate);
        for (auto& quality : channelQualityShifters) quality.prepare (sampleRate, 4, latency_limits::qualityMs);
        snapShifter.prepare (sampleRate);
        for (auto& snap : channelSnapShifters) snap.prepare (sampleRate);
        snapBlendCoeff = 1.0f - std::exp (-1.0f / (0.004f * (float) sampleRate));
        for (auto& channelShifter : channelShifters)
            channelShifter.prepare (sampleRate);
        hopSeconds = detector.getHopSeconds();
        ratioCoeff = 1.0f - std::exp (-1.0f / (0.002f * (float) sampleRate));
        fadeStep   = 1.0f / (0.010f * (float) sampleRate);   // 10 ms bypass crossfade
        amountFadeCoeff = 1.0f - std::exp (-1.0f / (0.003f * (float) sampleRate));
        holdLimit  = 0.02f; // bridge detector dropouts up to this long; longer ones (consonants) fall back to pass-through
        onsetFastCoeff = 1.0f - std::exp (-1.0f / (float) (sampleRate * 0.002));
        onsetSlowCoeff = 1.0f - std::exp (-1.0f / (float) (sampleRate * 0.025));
        reset();
    }

    void reset()
    {
        detector.reset();
        shifter.reset();
        microShifter.reset();
        qualityShifter.reset();
        historicalShifter.reset();
        for (auto& h : channelHistoricalShifters) h.reset();
        fixedHorizonShifter.reset();
        for (auto& fixed : channelFixedHorizonShifters) fixed.reset();
        for (auto& micro : channelMicroShifters) micro.reset();
        for (auto& quality : channelQualityShifters) quality.reset();
        snapShifter.reset();
        for (auto& snap : channelSnapShifters) snap.reset();
        snapBlend = 0.0f;
        for (auto& channelShifter : channelShifters)
        {
            channelShifter.reset();
            channelShifter.setParameters (1.0f, 0.0f);
        }
        resetTracking();
        ratio = 1.0f; period = 0.0f;
        outputPeriodValid = false; outputPeriod = 0.0f;
        analysisHops = 0;
        channelEnergy = { 0.0, 0.0 };
        stereoCrossEnergy = 0.0;
        polarityEnergy = {0.0, 0.0};
        analysisPolarity = 1.0f;
        pendingStereoSwitch = 0;
        onsetFastPower = onsetSlowPower = 0.0f;
        fastHopRemaining = 0; fastHopActive = false; fastHopSamples = 0;
        channelsPathActive = false;
        activeChannelCount = 1;
        wasHistoricalActive = settings.precisionHardTune && settings.lowLatency && settings.ultraLowLatency;
        wasFixedHorizonActive = settings.fixedHorizonVoiced;
        analysisChannel = 0;
        shifter.setParameters (1.0f, 0.0f);
        publishMeters (-1.0f, -1, 0.0f);
        wetGain = settings.bypass ? 0.0f : 1.0f;
        microAmountMix = settings.strength <= 1.0e-6f ? 0.0f : 1.0f;
        idle = settings.bypass;
    }

    // Apply settings immediately. Only call from the audio thread (or while audio is stopped).
    void setSettings (const Settings& s)
    {
        // Host automation and restored session state are external inputs. Reject
        // non-finite floats before they reach log2(), exp(), or MIDI-to-note casts.
        // Valid settings are left unchanged, preserving existing preset behavior.
        const bool wasAdaptive = settings.strictRealtime || (settings.precisionHardTune
            && settings.ultraLowLatency && settings.lowLatency);
        settings = s;
        // V1.4 production: spectral envelope guard is causal, latency neutral
        // for PDC and never dry-additive. Legacy direct shifter snapshots and
        // research paths keep their original numerical behavior.
        microShifter.setLowRegisterFormantGuardEnabled(settings.strictRealtime);
        qualityShifter.setLowRegisterFormantGuardEnabled(settings.strictRealtime);
        // V1.5 coherent additive carrier is confined to the 3.5ms Live path.
        // The 8ms Quality path retains V1.4 while independent comparison and
        // real-singer acceptance gates remain open.
        microShifter.setPhaseCoherentLowRegisterEnabled(settings.strictRealtime && settings.lowLatency);
        microShifter.setPitchSynchronousResamplingEnabled(settings.strictRealtime && settings.lowLatency && settings.pitchSynchronousLive);
        qualityShifter.setPhaseCoherentLowRegisterEnabled(false);
        qualityShifter.setPitchSynchronousResamplingEnabled(false);
        for (auto& sh : channelMicroShifters)
        {
            sh.setLowRegisterFormantGuardEnabled(settings.strictRealtime);
            sh.setPhaseCoherentLowRegisterEnabled(settings.strictRealtime && settings.lowLatency);
            sh.setPitchSynchronousResamplingEnabled(settings.strictRealtime && settings.lowLatency && settings.pitchSynchronousLive);
        }
        for (auto& sh : channelQualityShifters)
            sh.setLowRegisterFormantGuardEnabled(settings.strictRealtime);
        // Three-head crossover is deliberately A/B-only: the multi-note
        // sweep sometimes showed MORE original F0 energy than dual-head.
        // A restored V1.2 preset must not silently re-enable 80ms research DSP.
        if (settings.strictRealtime)
        {
            settings.fixedHorizonVoiced = false;
            settings.ultraLowLatency = true; // stereo renderer selects 3.5 / 8 ms below
        }
        if (settings.fixedHorizonVoiced != wasFixedHorizonActive)
        {
            fixedHorizonShifter.reset();
            for (auto& fixed : channelFixedHorizonShifters) fixed.reset();
            wasFixedHorizonActive = settings.fixedHorizonVoiced;
        }
        const auto sane = [] (float value, float fallback, float lo, float hi) noexcept
        {
            return std::clamp (std::isfinite (value) ? value : fallback, lo, hi);
        };
        settings.key = ((settings.key % 12) + 12) % 12;
        if ((int) settings.scale < (int) Scale::Chromatic || (int) settings.scale > (int) Scale::Custom)
            settings.scale = Scale::Chromatic;
        settings.referenceHz = sane (settings.referenceHz, 440.0f, 400.0f, 480.0f);
        settings.retuneMs = sane (settings.retuneMs, 20.0f, 0.0f, 2000.0f);
        settings.glideMs = sane (settings.glideMs, 0.0f, 0.0f, 500.0f);
        settings.maxCorrectionSemitones = sane (settings.maxCorrectionSemitones, 12.0f, 0.0f, 12.0f);
        settings.strength = sane (settings.strength, 1.0f, 0.0f, 1.0f);
        settings.humanize = sane (settings.humanize, 0.0f, 0.0f, 1.0f);
        settings.vibratoPreserve = sane (settings.vibratoPreserve, 0.7f, 0.0f, 1.0f);
        settings.follow = sane (settings.follow, 0.0f, 0.0f, 1.0f);
        settings.voicingThreshold = sane (settings.voicingThreshold, 0.85f, 0.0f, 1.0f);
        settings.noteConfirmMs = sane (settings.noteConfirmMs, 10.0f, 0.0f, 100.0f);
        settings.consonantReleaseMs = sane (settings.consonantReleaseMs, 8.0f, 0.0f, 30.0f);
        settings.noteStabilizerMs = sane (settings.noteStabilizerMs, 300.0f, 0.0f, 5000.0f);
        if (settings.precisionHardTune)
        {
            // Compared against a supplied A-major hard-tune vocal reference.
            // Deliberately an OPT-IN mode: it overrides natural-vibrato and
            // note-confirmation settings, which is not suitable for all vocals.
            settings.retuneMs = 0.0f;
            settings.follow = 1.0f;
            settings.noteStabilizerMs = 0.0f;
            settings.vibratoPreserve = 0.0f;
            settings.noteConfirmMs = 0.0f;
            settings.voicingThreshold = 0.83f;
        }
        mask = scaleMask (settings.scale, settings.customScale);
        detector.setRealtimeTransitionWindowEnabled(settings.strictRealtime);
        detector.setRecentTransitionEmphasisEnabled(settings.strictRealtime && !settings.precisionHardTune);
        detector.setThreshold (1.0f - settings.voicingThreshold);
        const bool isAdaptive = settings.strictRealtime || (settings.precisionHardTune
            && settings.ultraLowLatency && settings.lowLatency);
        // JUCE sends current parameters EVERY audio block. Keep the analysis
        // cadence and its short event-hold timer across identical settings.
        // Reset the cadence only on an actual processing-mode transition.
        if (isAdaptive != wasAdaptive)
        {
            fastHopActive = false;
            fastHopRemaining = 0;
        }
        // A historical cycle must never survive an unrelated processing mode
        // and leak stale voiced audio into the next Precision Live phrase.
        const bool historicalRequested = settings.precisionHardTune && settings.lowLatency
            && settings.ultraLowLatency;
        if (historicalRequested != wasHistoricalActive)
        {
            historicalShifter.reset();
            for (auto& h : channelHistoricalShifters) h.reset();
            wasHistoricalActive = historicalRequested;
        }
        detector.setAnalysisHopSeconds (isAdaptive && fastHopActive ? 0.001f
                                       : (settings.strictRealtime ? 0.0020f
                                          : (settings.lowLatency ? 0.0025f : 0.0050f)));
        shifter.setLowLatency (settings.lowLatency);
        for (auto& channelShifter : channelShifters)
        {
            channelShifter.setLowLatency (settings.lowLatency);
        }
        hopSeconds = detector.getHopSeconds();
    }
    const Settings& getSettings() const noexcept { return settings; }

    // Hand new settings over from ANY single other thread (UI, message thread). Lock-free
    // triple buffer: the audio thread picks them up at the start of its next call.
    void pushSettings (const Settings& s) noexcept
    {
        slots[(size_t) writeSlot] = s;
        writeSlot = mailbox.exchange (writeSlot | dirtyBit, std::memory_order_acq_rel) & 3;
    }

    float process (float input) noexcept
    {
        // Host/driver samples are untrusted. A single NaN can otherwise
        // contaminate onset filters and the YIN ring for many analysis hops;
        // at zero Strength it was also passed straight to the DAW output.
        input = boundedAudioSample(input);
        channelsPathActive = false;
        activeChannelCount = 1;
        if (! advanceControl (input))
            return input; // completely bypassed: exact, zero-CPU pass-through

        microShifter.setRatio (period > 0.0f ? ratio : 1.0f);
        microShifter.setPeriod (period);
        qualityShifter.setRatio (period > 0.0f ? ratio : 1.0f);
        qualityShifter.setPeriod (period);
        float microWet = settings.ultraLowLatency ? microShifter.processSample (input) : 0.0f;
        const float qualityWet = settings.strictRealtime ? qualityShifter.processSample(input) : 0.0f;
        float snapAlignedDry = 0.0f;
        if (settings.strictRealtime)
        {
            // Snap Live keeps its history warm while unselected; a short equal-power crossfade
            // hides the change of renderer (and of delay) when it is switched on or off.
            advanceSnapBlend();
            snapShifter.setRatio (period > 0.0f ? ratio : 1.0f);
            snapShifter.setPeriod (period);
            snapShifter.setRenderingEnabled (snapBlend > 0.0f);
            const float snapWet = snapShifter.processSample (input);
            snapAlignedDry = snapShifter.getLastAlignedDry();
            if (settings.lowLatency) microWet = blendSnap (microWet, snapWet);
        }
        float wet = settings.strictRealtime ? (settings.lowLatency ? microWet : qualityWet)
                  : (settings.ultraLowLatency ? microWet : shifter.processSample (input));
        if (!settings.strictRealtime && settings.ultraLowLatency && settings.lowLatency && settings.precisionHardTune)
        {
            historicalShifter.setParameters (period, period > 0.0f ? ratio : 1.0f);
            wet = historicalShifter.processSample (input, microWet);
        }
        if (settings.fixedHorizonVoiced)
            wet = fixedHorizonShifter.processSample(input,period,period>0.0f ? ratio : 1.0f);
        advanceWetGain();

        // Amount=0 means genuinely unprocessed audio. Keep advancing the shifter above
        // so its history stays warm if Amount is raised during playback, but never expose
        // the pitch-synchronous grain reconstruction when no correction is requested.
        const float strictAlignedDry = settings.strictRealtime
            ? (settings.lowLatency ? blendSnap (microShifter.getLastAlignedDry(), snapAlignedDry)
                                   : qualityShifter.getLastAlignedDry()) : input;
        if (settings.strength <= 1.0e-6f)
            return settings.fixedHorizonVoiced ? fixedHorizonShifter.getLastAlignedDry() : strictAlignedDry;

        const float aligned = settings.fixedHorizonVoiced ? fixedHorizonShifter.getLastAlignedDry() : strictAlignedDry;
        return wetGain >= 1.0f ? wet : aligned + wetGain * (wet - aligned);
    }

    void process (float* data, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
            data[i] = process (data[i]);
    }

    // Linked pitch control with per-channel PSOLA. This avoids destructive L/R
    // averaging (which cancels polarity-inverted vocals) and keeps bypass stereo-safe.
    // Pitch is tracked from the stronger channel with smoothed energy/hysteresis, while
    // each channel gets its own pulse alignment and output grain stream.
    // Optional per-sample estimate of intrinsic engine delay. When non-null, the caller
    // must provide at least `numSamples * min(numChannels, 2)` floats in channel-major
    // layout: channel 0 occupies [0, numSamples), channel 1 [numSamples, 2*numSamples).
    // The processor can use this telemetry to pad paths to a fixed host-reported latency.
    void processChannels (float* const* channels, int numChannels, int numSamples,
                          float* intrinsicLatencySamples = nullptr) noexcept
    {
        if (channels == nullptr || numChannels <= 0 || numSamples <= 0)
            return;

        channelsPathActive = true;
        activeChannelCount = std::clamp (numChannels, 1, (int) channelShifters.size());

        // Skip even the block-energy scan once fully bypassed.
        applyPendingSettings();
        if (wetGain == 0.0f && settings.bypass)
        {
            // Full bypass still must not leak NaNs/Infs back to the host.
            for (int c = 0; c < activeChannelCount; ++c)
                if (channels[c] != nullptr)
                    for (int i = 0; i < numSamples; ++i)
                        channels[c][i] = boundedAudioSample(channels[c][i]);
            idle = true;
            if (intrinsicLatencySamples != nullptr)
                std::fill (intrinsicLatencySamples,
                           intrinsicLatencySamples + (size_t) numSamples * (size_t) activeChannelCount, 0.0f);
            return;
        }

        const int trackedChannels = std::min (numChannels, (int) channelShifters.size());
        if (trackedChannels <= 0)
        {
            if (intrinsicLatencySamples != nullptr)
                std::fill (intrinsicLatencySamples, intrinsicLatencySamples + numSamples, 0.0f);
            return;
        }

        // Update smoothed channel energy sample-by-sample, not once per host
        // block. This makes channel tracking independent of DAW block size and
        // bounded processor slices. The 40 ms time constant and 1.45x hysteresis
        // prevent unstable switching between similarly loud stereo channels.
        const double energyAlpha = 1.0 - std::exp (-1.0 / (sampleRate * 0.040));
        if (trackedChannels == 1)
        {
            analysisChannel = 0;
            analysisPolarity = 1.0f;
            pendingStereoSwitch = 0;
        }
        // HelixTune's transient analysis and SnapTune's live detector both
        // reinforce this principle: avoid injecting an artificial waveform
        // discontinuity *before* pitch detection. Opposite-polarity stereo
        // should not flip the YIN history each time the louder side changes.
        const int maxSwitchWait = std::max(1, (int) std::lround(sampleRate * 0.012));
        const double polarityAlpha = 1.0 - std::exp(-1.0 / (sampleRate * 0.006));
        for (int i = 0; i < numSamples; ++i)
        {
            for (int c = 0; c < trackedChannels; ++c)
            {
                if (channels[c] != nullptr)
                    channels[c][i] = boundedAudioSample(channels[c][i]);
                const double x = channels[c] != nullptr ? (double) channels[c][i] : 0.0;
                channelEnergy[(size_t) c] += energyAlpha * (x * x - channelEnergy[(size_t) c]);
            }
            if (trackedChannels == 2)
            {
                const double x0 = channels[0] != nullptr ? (double) channels[0][i] : 0.0;
                const double x1 = channels[1] != nullptr ? (double) channels[1][i] : 0.0;
                stereoCrossEnergy += polarityAlpha * (x0 * x1 - stereoCrossEnergy);
                polarityEnergy[0] += polarityAlpha * (x0*x0 - polarityEnergy[0]);
                polarityEnergy[1] += polarityAlpha * (x1*x1 - polarityEnergy[1]);
                const int candidate = 1 - analysisChannel;
                if (channelEnergy[(size_t) candidate] > channelEnergy[(size_t) analysisChannel] * 1.45)
                {
                    pendingStereoSwitch = std::min(pendingStereoSwitch + 1, maxSwitchWait);
                    // Only invert the detector input when the channels have
                    // clearly negative correlation; never sum stereo audio.
                    const double geometric = std::sqrt(std::max(1.0e-18,
                        polarityEnergy[0] * polarityEnergy[1]));
                    const float nextPolarity = candidate == 1 && stereoCrossEnergy < -0.72 * geometric
                        ? -1.0f : 1.0f;
                    const double current = (analysisChannel == 0 ? x0 : x1) * analysisPolarity;
                    const double following = (candidate == 0 ? x0 : x1) * nextPolarity;
                    // Prefer a low-discontinuity source handoff. The bound
                    // prevents a muted source from trapping selection forever.
                    const double level = std::sqrt(std::max(1.0e-12,
                        channelEnergy[(size_t) analysisChannel]));
                    if (std::abs(current - following) <= 0.22 * level ||
                        pendingStereoSwitch >= maxSwitchWait)
                    {
                        analysisChannel = candidate;
                        analysisPolarity = nextPolarity;
                        pendingStereoSwitch = 0;
                    }
                }
                else
                    pendingStereoSwitch = 0;
            }
            const float analysis = (channels[analysisChannel] != nullptr
                ? channels[analysisChannel][i] : 0.0f) * analysisPolarity;
            if (! advanceControl (analysis))
            {
                if (intrinsicLatencySamples != nullptr)
                    for (int c = 0; c < trackedChannels; ++c)
                        intrinsicLatencySamples[(size_t) c * (size_t) numSamples + (size_t) i] = 0.0f;
                continue; // do not overwrite channels during full bypass
            }

            std::array<float, 2> dry { 0.0f, 0.0f };
            std::array<float, 2> wet { 0.0f, 0.0f };
            std::array<float, 2> snapWetOut { 0.0f, 0.0f }, snapDryOut { 0.0f, 0.0f };
            if (settings.strictRealtime)
            {
                // Snap Live, linked stereo: the pitch-analysis channel decides every splice and
                // the other channel replays it, so both always jump by the same length.
                advanceSnapBlend();
                const bool snapRender = snapBlend > 0.0f;
                const int leader = trackedChannels == 2 ? analysisChannel : 0;
                for (int k = 0; k < trackedChannels; ++k)
                {
                    const int c = k == 0 ? leader : 1 - leader;
                    if (channels[c] == nullptr) continue;
                    auto& snap = channelSnapShifters[(size_t)c];
                    snap.setFollower (k != 0);
                    snap.setRatio (period > 0.0f ? ratio : 1.0f);
                    snap.setPeriod (period);
                    snap.setRenderingEnabled (snapRender);
                    if (k != 0)
                        snap.applyEvent (channelSnapShifters[(size_t)leader].getLastEvent());
                    snapWetOut[(size_t)c] = snap.processSample (channels[c][i]);
                    snapDryOut[(size_t)c] = snap.getLastAlignedDry();
                }
            }
            for (int c = 0; c < trackedChannels; ++c)
            {
                if (channels[c] == nullptr) continue;
                dry[(size_t)c] = channels[c][i];
                // Both histories stay warm so live/normal automation does not
                // begin with an empty ring or a cold PSOLA grain timeline.
                const float normalWet = settings.strictRealtime ? 0.0f
                    : channelShifters[(size_t)c].processSample (dry[(size_t)c]);
                float microWet = channelMicroShifters[(size_t)c].processSample (dry[(size_t)c]);
                if (settings.strictRealtime && settings.lowLatency)
                    microWet = blendSnap (microWet, snapWetOut[(size_t)c]);
                const float qualityWet = settings.strictRealtime
                    ? channelQualityShifters[(size_t)c].processSample(dry[(size_t)c]) : 0.0f;
                wet[(size_t)c] = settings.strictRealtime
                    ? (settings.lowLatency ? microWet : qualityWet)
                    : (settings.ultraLowLatency ? microWet : normalWet);
                if (!settings.strictRealtime && settings.ultraLowLatency && settings.lowLatency && settings.precisionHardTune)
                {
                    auto& h = channelHistoricalShifters[(size_t)c];
                    h.setParameters (period, period > 0.0f ? ratio : 1.0f);
                    wet[(size_t)c] = h.processSample (dry[(size_t)c], microWet);
                }
                if (settings.fixedHorizonVoiced)
                    wet[(size_t)c] = channelFixedHorizonShifters[(size_t)c].processSample(
                        dry[(size_t)c],period,period>0.0f ? ratio : 1.0f);
            }
            advanceWetGain();
            for (int c = 0; c < trackedChannels; ++c)
            {
                if (channels[c] == nullptr) continue;
                // Normal-mode behavior is unchanged. In the 3.5 ms mode,
                // Amount=0 must be an aligned DRY tap, not a slowly decaying
                // corrected wet head. Smooth both transitions to prevent an
                // abrupt phase discontinuity under fast Amount automation.
                const float fixedDry = settings.fixedHorizonVoiced
                    ? channelFixedHorizonShifters[(size_t)c].getLastAlignedDry() : dry[(size_t)c];
                const float processed = settings.strength <= 1.0e-6f
                    ? wet[(size_t)c]
                    : (wetGain >= 1.0f ? wet[(size_t)c]
                       : fixedDry + wetGain * (wet[(size_t)c] - fixedDry));
                if (settings.fixedHorizonVoiced)
                    channels[c][i] = fixedDry + microAmountMix * (processed - fixedDry);
                else if (settings.strictRealtime || settings.ultraLowLatency)
                {
                    const float alignedDry = settings.strictRealtime && !settings.lowLatency
                        ? channelQualityShifters[(size_t)c].getLastAlignedDry()
                        : (settings.strictRealtime && settings.lowLatency
                            ? blendSnap (channelMicroShifters[(size_t)c].getLastAlignedDry(), snapDryOut[(size_t)c])
                            : channelMicroShifters[(size_t)c].getLastAlignedDry());
                    channels[c][i] = microAmountMix >= 1.0f ? processed
                        : alignedDry + microAmountMix * (processed - alignedDry);
                }
                else
                    channels[c][i] = settings.strength <= 1.0e-6f ? dry[(size_t)c] : processed;
            }

            if (intrinsicLatencySamples != nullptr)
            {
                const double wetFactor = settings.strength > 1.0e-6f ? (double) wetGain : 0.0;
                for (int c = 0; c < trackedChannels; ++c)
                {
                    const double intrinsic = settings.strictRealtime
                        ? (double)(settings.lowLatency
                            ? (snapBlend > 0.5f ? channelSnapShifters[(size_t)c].getMaximumDelaySamples()
                                                : channelMicroShifters[(size_t)c].getMaximumDelaySamples())
                            : channelQualityShifters[(size_t)c].getMaximumDelaySamples())
                        : settings.fixedHorizonVoiced ? (double) fixedHorizonShifter.getHostLatencySamples()
                        : settings.ultraLowLatency
                        ? (double) channelMicroShifters[(size_t)c].getMaximumDelaySamples()
                        : channelShifters[(size_t) c].getLatencySamples() * wetFactor;
                    intrinsicLatencySamples[(size_t) c * (size_t) numSamples + (size_t) i]
                        = (float) std::max (0.0, intrinsic);
                }
            }
        }
    }

    // Diagnostic only: the age of samples used by the historical carrier,
    // NOT the plugin's input-to-output wet latency or a replacement for PDC.
    // Zero means inactive or not yet crossfaded into the historical path.
    float getHistoricalCarrierSourceAgeSamples() const noexcept
    {
        if (settings.strictRealtime || settings.bypass || !settings.lowLatency || !settings.ultraLowLatency ||
            !settings.precisionHardTune) return 0.0f;
        return std::max({historicalShifter.getCarrierSourceAgeSamples(),
            channelHistoricalShifters[0].getCarrierSourceAgeSamples(),
            channelHistoricalShifters[1].getCarrierSourceAgeSamples()});
    }

    // Meters for a tuner display (fractional MIDI note; -1 = none).
    // Audio-thread diagnostics: the selected analysis source and its polarity.
    uint64_t getFastAnalysisSamples() const noexcept {return fastHopSamples;}
    int getAnalysisChannel() const noexcept { return analysisChannel; }
    float getAnalysisPolarity() const noexcept { return analysisPolarity; }
    float getDetectedMidi() const noexcept   { return detectedMidi; }
    int   getTargetMidi() const noexcept     { return targetMidi; }
    float getCorrectionSemitones() const noexcept { return shiftSemitones; }
    float getConfidence() const noexcept     { return detector.getConfidence(); }
    bool  isVoiced() const noexcept          { return period > 0.0f; }   // currently pitch-tracking
    // Current input-to-output delay in samples. It varies with pitch, shift ratio and the active mode;
    // for accurate host alignment the processor needs a fixed-delay output stage or a suitable fixed budget.
    // Host transport-delay budget is fixed until prepareToPlay; note-dependent
    // intrinsic grain delay is absorbed by OutputLatencyCompensator. This is not
    // the YIN history duration and should not change every analysis frame.
    // Select a smaller PDC budget ONLY when the DSP is permanently locked to
    // fast grain scheduling. A switchable normal/fast plugin must use 2.5
    // periods because normal-mode overlap can exceed the fast delay budget.
    // 2.0 periods for the dedicated fast-only plugin includes margin above
    // the steady 70-Hz intrinsic-delay measurements (~1.53 periods); dynamic
    // low-pitch speech still warrants host testing before a public release.
    static int ultraLowLatencyHostSamples(double sr) noexcept
    {
        return MicroLatencyShifter::hostLatencySamples(sr);
    }
    static int realtimeQualityHostSamples(double sr) noexcept
    {
        return (int) std::ceil(std::max(8000.0, sr) * (latency_limits::qualityMs * 0.001)); // 8.0ms
    }

    static int recommendedHostLatencySamples (double sr, float minHz = 70.0f,
                                               bool fastOnly = false) noexcept
    {
        return (int) std::ceil (std::max (8000.0, sr) * (fastOnly ? 2.0 : 2.5)
                                / (double) std::max (30.0f, minHz));
    }
    uint64_t getMaxDifferenceOperationsPerFrame() const noexcept
    { return detector.getMaxDifferenceOperationsPerFrame(); }
    // Read ONLY on the audio thread or in an offline renderer. GUI threads
    // must copy to their own atomics; do not read mutable DSP fields directly.
    Diagnostics getDiagnostics() const noexcept
    {
        const auto& synthesis = channelsPathActive ? channelShifters[0] : shifter;
        Diagnostics d;
        d.analysisHop = analysisHops;
        d.detectedMidi = detectedMidi;
        d.confidence = detector.getConfidence();
        d.rawFrequencyHz = detector.getRawFrequency();
        d.detectorFrequencyHz = detector.getFrequency();
        d.twoPeriodAperiodicity = detector.getTwoPeriodAperiodicity();
        d.learnedPitchReliability = PitchRiskModel::reliability (d.confidence, d.detectorFrequencyHz, d.rawFrequencyHz, detector.getAlternateFrequency(), d.twoPeriodAperiodicity);
        d.alternateFrequencyHz = detector.getAlternateFrequency();
        d.phaseCorrelation = synthesis.getLastPhaseCorrelation();
        d.overlapWeightSum = synthesis.getOverlapWeightSum();
        d.sourceAgeSamples = synthesis.getLastSourceAgeSamples();
        d.correctionSemitones = shiftSemitones;
        d.targetRatio = targetRatio;
        d.smoothedRatio = ratio;
        d.estimatedPeriodSamples = period;
        d.scheduledOutputPeriodSamples = period > 0.0f && ratio > 0.0f
            ? period / ratio : 0.0f;
        d.synthesisGain = synthesis.getAppliedGain();
        d.targetMidi = targetMidi;
        d.activeGrains = synthesis.getActiveGrainCount();
        d.grainDropouts = synthesis.getDropoutSamples();
        d.grainSaturations = synthesis.getGrainSaturationCount();
        d.phaseReacquisitions = synthesis.getPhaseReacquisitions();
        d.voiced = period > 0.0f;
        d.pendingNote = pendingNote >= 0;
        return d;
    }
    double getLatencySamples() const noexcept
    {
        if (! channelsPathActive)
            return shifter.getLatencySamples();

        double latency = channelShifters[0].getLatencySamples();
        for (int c = 1; c < activeChannelCount; ++c)
            latency = std::max (latency, channelShifters[(size_t)c].getLatencySamples());
        return latency;
    }

private:
    // Returns false only once fully bypassed, allowing the caller to leave samples untouched.
    bool advanceControl (float analysisSample) noexcept
    {
        // Guard this internal boundary too: callers may feed an analysis
        // channel even if the synthesizer sanitizes its own input.
        analysisSample = boundedAudioSample(analysisSample);
        // processChannels() applies pending settings once at the block start.
        // Do not repeat an atomic mailbox load for every channel/sample;
        // scalar mono processing retains its per-sample automation check.
        if (! channelsPathActive) applyPendingSettings();
        const float goal = settings.bypass ? 0.0f : 1.0f;
        if (wetGain == 0.0f && goal == 0.0f)
        {
            idle = true;
            return false;
        }

        if (idle)
        {
            const auto keep = wetGain;
            detector.reset();
            shifter.reset();
            for (auto& channelShifter : channelShifters) channelShifter.reset();
            microShifter.reset();
            qualityShifter.reset();
            for (auto& micro : channelMicroShifters) micro.reset();
            for (auto& quality : channelQualityShifters) quality.reset();
            snapShifter.reset();
            for (auto& snap : channelSnapShifters) snap.reset();
            // Full bypass skips processing and leaves historical input frozen.
            // Never reuse a pre-bypass voiced cycle on re-entry, even when
            // returning to the exact same Precision Live settings.
            historicalShifter.reset();
            for (auto& historical : channelHistoricalShifters) historical.reset();
            fixedHorizonShifter.reset();
            for (auto& fixed : channelFixedHorizonShifters) fixed.reset();
            resetTracking();
            ratio = 1.0f; period = 0.0f;
            shifter.setParameters (1.0f, 0.0f);
            for (auto& channelShifter : channelShifters) channelShifter.setParameters (1.0f, 0.0f);
            wetGain = keep;
            idle = false;
        }

        // Dual-timescale onset envelope: energy rise is evidence that a pitch
        // change is a new syllable rather than a stationary harmonic error.
        const float power = analysisSample * analysisSample;
        onsetFastPower += onsetFastCoeff * (power - onsetFastPower);
        onsetSlowPower += onsetSlowCoeff * (power - onsetSlowPower);
        // Higher detector cadence only for established transients and
        // pending note changes. Keep the robust original YIN window.
        // This changes detector *update frequency*, not audio transport delay.
        if (settings.strictRealtime || (settings.precisionHardTune && settings.ultraLowLatency
            && settings.lowLatency))
        {
            if ((onsetSlowPower > 1.0e-6f && onsetFastPower > onsetSlowPower * 1.30f)
                || pendingNote >= 0)
                fastHopRemaining = (int)(sampleRate * 0.060);
            else if (fastHopRemaining > 0)
                --fastHopRemaining;
            const bool wantFast = fastHopRemaining > 0;
            if (wantFast != fastHopActive)
            {
                fastHopActive = wantFast;
                detector.setAnalysisHopSeconds(wantFast ? 0.001f : (settings.strictRealtime ? 0.0020f : 0.0025f));
                hopSeconds = detector.getHopSeconds();
            }
            fastHopSamples += (uint64_t)wantFast;
        }
        if (detector.push (analysisSample))
        {
            ++analysisHops;
            updateControl();
        }

        if (settings.precisionHardTune && period > 0.0f && targetRatio > 0.0f)
        {
            // Precision mode: smooth the SYNTHESIS period, not the ratio.
            // The ratio is relative to the pitch detector and therefore changes
            // whenever source F0 changes. Smoothing that ratio independently of
            // period can modulate output pitch even on a stable target note.
            const float wantedPeriod = period / targetRatio;
            if (! outputPeriodValid)
            {
                outputPeriod = wantedPeriod;
                outputPeriodValid = true;
            }
            outputPeriod += (wantedPeriod - outputPeriod) * ratioCoeff;
            ratio = std::clamp (period / std::max (8.0f, outputPeriod), 0.25f, 4.0f);
        }
        else
        {
            outputPeriodValid = false;
            ratio += (targetRatio - ratio) * ratioCoeff;
        }
        // Avoid updating inactive resynthesis instances on every sample.
        // This matters in DAWs with dozens of plugin instances.
        if (! channelsPathActive)
            shifter.setParameters (ratio, period);
        else
            for (int c = 0; c < activeChannelCount; ++c)
            {
                channelShifters[(size_t)c].setParameters (ratio, period);
                channelMicroShifters[(size_t)c].setRatio (period > 0.0f ? ratio : 1.0f);
                channelMicroShifters[(size_t)c].setPeriod (period);
                channelQualityShifters[(size_t)c].setRatio(period > 0.0f ? ratio : 1.0f);
                channelQualityShifters[(size_t)c].setPeriod(period);
            }
        return true;
    }

    // Snap Live <-> Live renderer switch: equal-power crossfade over ~4 ms (time constant).
    void advanceSnapBlend() noexcept
    {
        const float goal = (settings.strictRealtime && settings.lowLatency && settings.snapLive) ? 1.0f : 0.0f;
        snapBlend += snapBlendCoeff * (goal - snapBlend);
        if (std::abs (goal - snapBlend) < 1.0e-4f) snapBlend = goal;
    }
    float blendSnap (float liveValue, float snapValue) const noexcept
    {
        if (snapBlend <= 0.0f) return liveValue;
        if (snapBlend >= 1.0f) return snapValue;
        constexpr float halfPi = 1.57079632679f;
        return std::cos (halfPi * snapBlend) * liveValue + std::sin (halfPi * snapBlend) * snapValue;
    }

    void advanceWetGain() noexcept
    {
        const float goal = settings.bypass ? 0.0f : 1.0f;
        if (wetGain < goal)      wetGain = std::min (goal, wetGain + fadeStep);
        else if (wetGain > goal) wetGain = std::max (goal, wetGain - fadeStep);
        const float amountGoal = settings.strength <= 1.0e-6f ? 0.0f : 1.0f;
        microAmountMix += amountFadeCoeff * (amountGoal - microAmountMix);
        if (amountGoal == 0.0f && microAmountMix < 1.0e-5f)
            microAmountMix = 0.0f;
        else if (amountGoal == 1.0f && microAmountMix > 1.0f - 1.0e-5f)
            microAmountMix = 1.0f;
    }

    void resetTracking()
    {
        medianCount = 0; hasCenter = false; center = 0.0f; correction = 0.0f;
        lastLeadPitchValid = false; lastLeadPitch = leadSlope = 0.0f;
        motionMean = 0.0f; motionHigh = motionLow = 0.0f; motionDirection = 0; motionEvidence = 0.0f;
        motionRollback = 0.0f; motionPriorCenter = 0.0f; motionPriorTarget = -1;
        recentAttackSeconds = 0.0f;
        reacquireGuard = false;
        targetNote = -1; targetRatio = 1.0f; holdTime = 0.0f; shiftSemitones = 0.0f;
        pendingNote = -1; pendingNoteSeconds = 0.0f;
        glideRemainingSeconds = 0.0f; glideShift = 0.0f;
        outputPeriodValid = false;
        outputPeriod = 0.0f;
    }

    void publishMeters (float detected, int target, float shift) noexcept
    {
        detectedMidi = detected; targetMidi = target; shiftSemitones = shift;
    }

    void applyPendingSettings() noexcept
    {
        if (! (mailbox.load (std::memory_order_relaxed) & dirtyBit))
            return;
        readSlot = mailbox.exchange (readSlot, std::memory_order_acq_rel) & 3;
        setSettings (slots[(size_t) readSlot]);
    }

    bool inScale (int note) const noexcept { return ((mask >> (((note - settings.key) % 12 + 12) % 12)) & 1) != 0; }

    int nearestNote (float note) const noexcept
    {
        int best = -1;
        float bestDist = 100.0f;
        const int base = (int) std::floor (note);
        // Resolve scale-note ties consistently even when the detector estimate
        // differs by a few cents between analysis modes or at a note transition.
        // A near-exact tie (within 5 cents) goes to the higher allowed note.
        constexpr float tieToleranceSemitones = 0.05f;
        for (int n = base - 6; n <= base + 7; ++n)
        {
            if (! inScale (n))
                continue;

            const float d = std::abs (note - (float) n);
            const bool clearlyCloser = d < bestDist - tieToleranceSemitones;
            const bool tiedAndHigher = best >= 0
                && std::abs (d - bestDist) <= tieToleranceSemitones
                && n > best;
            if (best < 0 || clearlyCloser || tiedAndHigher)
            {
                best = n;
                bestDist = d;
            }
        }
        // Hysteresis: stay on the current note until another is clearly closer,
        // except when the current note is the lower member of an actual tie and
        // the deterministic tie rule selects the higher member.
        if (targetNote >= 0 && inScale (targetNote))
        {
            const float currentDistance = std::abs (note - (float) targetNote);
            const bool lowerNoteLosesTie = best > targetNote
                && std::abs (currentDistance - bestDist) <= tieToleranceSemitones;
            if (! lowerNoteLosesTie && currentDistance < bestDist + 0.15f)
                return targetNote;
        }
        return best;
    }

    static float median3 (float a, float b, float c) noexcept
    {
        return std::max (std::min (a, b), std::min (std::max (a, b), c));
    }

    void updateControl() noexcept
    {
        const float f = detector.getFrequency();
        // Remember a real amplitude onset across several detector hops. The
        // causal detector may not reach a trustworthy pitch until after the
        // instantaneous envelope rise has already passed.
        recentAttackSeconds = std::max (0.0f, recentAttackSeconds - hopSeconds);
        if (onsetSlowPower > 1.0e-6f && onsetFastPower > onsetSlowPower * 1.65f)
            recentAttackSeconds = 0.026f;

        // Unvoiced/noisy frames should not continue forcing the last note
        // through a consonant. Release before clearing the note history, so a
        // one-hop YIN dropout does not create flutter or spuriously reacquire.
        if (f <= 0.0f || detector.getConfidence() < std::max (0.52f, settings.voicingThreshold - 0.03f))
        {
            lastLeadPitchValid = false; leadSlope = 0.0f;
            holdTime += hopSeconds;
            // Broadband/high-energy consonant onsets deserve a prompt release;
            // a quiet one-hop YIN dropout still uses the user's longer hold.
            // Only shorten after the pitch detector explicitly loses voicing.
            const bool consonantAttack = settings.strictRealtime && f <= 0.0f
                && onsetSlowPower > 1.0e-6f
                && onsetFastPower > onsetSlowPower * 1.80f;
            const float releaseSeconds = consonantAttack
                ? std::min(0.004f, settings.consonantReleaseMs * 0.001f)
                : settings.consonantReleaseMs * 0.001f;
            if (holdTime >= releaseSeconds)
            {
                targetRatio = 1.0f;
                period = 0.0f;
                pendingNote = -1;
                pendingNoteSeconds = 0.0f;
            }
            if (holdTime > holdLimit)
            {
                const bool wasVoiced = targetNote >= 0 || reacquireGuard;
                resetTracking();
                reacquireGuard = wasVoiced; // stricter first hop only AFTER a dropout
                period = 0.0f;                // unvoiced: pass audio through untouched
                publishMeters (-1.0f, -1, 0.0f);
            }
            return;
        }

        // NoteForge's two-cycle check prevents accidental harmonic/noise locks
        // at a *new* voiced onset. Keep established notes permissive: breathy
        // consonants can weaken the second period before the note releases.
        const float twoCycle = detector.getTwoPeriodAperiodicity();
        if (targetNote < 0 && twoCycle >= 0.0f
            && twoCycle > (reacquireGuard ? 0.25f : 0.65f))
        {
            holdTime += hopSeconds;
            return;
        }

        holdTime = 0.0f;
        reacquireGuard = false;
        const float midi = 69.0f + 12.0f * std::log2 (f / settings.referenceHz);

        // Median of the last three estimates rejects single-frame octave errors.
        history[(size_t) (medianCount % 3)] = midi;
        ++medianCount;
        const float med = medianCount >= 3 ? median3 (history[0], history[1], history[2]) : midi;
        const float m = std::abs (midi - med) > 0.5f ? med : midi;

        // V1.10: distinguish a fresh note attack from periodic wide vibrato.
        // Previously |m-center| > 0.9 semitone snapped the pitch centre on
        // *every* vibrato peak, flipping target notes even on stable sung vowels.
        // A short causal novelty filter looks for a fast pitch movement, then
        // requires independent persistent samples before moving the centre.
        // No audio lookahead, resampling, allocations, or additional history.
        const bool naturalLive = settings.strictRealtime && settings.follow < 0.45f
            && settings.noteStabilizerMs > 0.5f;
        if (! hasCenter)
        {
            center = m; motionMean = m; motionHigh = motionLow = m; motionDirection = 0;
            motionEvidence = 0.0f; hasCenter = true;
            motionRollback = 0.0f;
        }
        else if (! naturalLive)
        {
            // Follow/Precision and legacy paths keep their existing response.
            if (std::abs (m - center) > 0.9f || settings.noteStabilizerMs <= 0.5f)
                center = m;
            else
                center += (m - center) * (1.0f - std::exp (-hopSeconds / (settings.noteStabilizerMs * 0.001f)));
            motionMean = m; motionHigh = motionLow = m; motionDirection = 0; motionEvidence = 0.0f;
            motionRollback = 0.0f;
        }
        else
        {
            // A recent attack is provisional: if pitch returns to the old
            // centre within 36ms, undo the recenter and any false note lock.
            // This is a causal recovery mechanism, NOT future lookahead.
            if (motionRollback > 0.0f)
            {
                motionRollback = std::max (0.0f, motionRollback - hopSeconds);
                if (std::abs (m - motionPriorCenter) < 0.32f
                    && std::abs (m - center) > 0.52f)
                {
                    center = motionPriorCenter;
                    if (motionPriorTarget >= 0 && targetNote != motionPriorTarget)
                        targetNote = motionPriorTarget;
                    pendingNote = -1; pendingNoteSeconds = 0.0f;
                    motionRollback = 0.0f;
                    motionDirection = 0; motionEvidence = 0.0f;
                    motionMean = m; motionHigh = motionLow = m;
                }
            }
            // A 9ms high-pass novelty estimate: even 1.25-semitone/6.5Hz
            // vibrato moves slowly enough to remain below the attack gate.
            const float novelty = m - motionMean;
            motionMean += (m - motionMean) * (1.0f - std::exp (-hopSeconds / 0.009f));
            const bool reliable = detector.getConfidence() >= 0.87f
                && (twoCycle < 0.0f || twoCycle <= 0.32f);
            // The retained extrema represent the recent vibrato envelope.
            // A periodic return to its existing peak is not a new note attack.
            const bool beyondEnvelope = m > motionHigh + 0.32f
                || m < motionLow - 0.32f;
            motionHigh = std::max (m, motionHigh - 0.60f * hopSeconds);
            motionLow = std::min (m, motionLow + 0.60f * hopSeconds);
            if (reliable && beyondEnvelope && std::abs (novelty) > 0.58f
                && std::abs (m - center) > 0.55f)
            {
                const int direction = novelty > 0.0f ? 1 : -1;
                if (motionDirection != direction)
                {
                    motionDirection = direction;
                    motionEvidence = 0.0f;
                }
            }
            bool recentered = false;
            if (motionDirection != 0)
            {
                if (! reliable || (m - center) * (float) motionDirection < 0.46f)
                {
                    motionDirection = 0;
                    motionEvidence = 0.0f;
                }
                else
                {
                    motionEvidence += hopSeconds;
                    // Only onset-backed, high-periodicity moderate changes get
                    // the shorter motion proof. Pure frequency bursts (without
                    // envelope attack) retain the V1.10 10ms safeguard.
                    const bool strongFreshAttack = recentAttackSeconds > 0.0f
                        && detector.getConfidence() >= 0.96f
                        && twoCycle >= 0.0f && twoCycle < 0.14f
                        && std::abs (m - center) <= 7.0f;
                    if (motionEvidence >= (strongFreshAttack ? 0.006f : 0.010f))
                    {
                        motionPriorCenter = center;
                        motionPriorTarget = targetNote;
                        motionRollback = 0.036f;
                        center = m;
                        recentered = true;
                        motionDirection = 0;
                        motionEvidence = 0.0f;
                        motionMean = m;
                    }
                }
            }
            if (! recentered)
                center += (m - center) * (1.0f - std::exp (-hopSeconds / (settings.noteStabilizerMs * 0.001f)));
        }

        // Vibrato = how far the current estimate sits from the slow centre. The shifter's grain spacing comes
        // from the same estimate, so ratio and period always agree and no lead/extrapolation is wanted:
        // an earlier lead term was measured to add error equal to (pitch slope x lead) once grains were
        // aligned to the real pulses.
        // V1.12: predict a few milliseconds of a continuously voiced *local*
        // trajectory to offset the detector's causal observation-window age.
        // Never predict note *selection* or source-period placement; both
        // continue to use measured YIN pitch, preserving octave safeguards.
        // Reject sharp note changes, noisy frames and unstable derivatives.
        float leadSemitones = 0.0f;
        const bool predictiveLive = settings.pitchSynchronousLive && settings.strictRealtime && settings.lowLatency
            && !settings.precisionHardTune && settings.follow < 0.45f;
        const bool smoothFrame = predictiveLive && detector.getConfidence() >= 0.93f
            && targetNote >= 0 && pendingNote < 0 && medianCount >= 3
            && (twoCycle < 0.0f || twoCycle < 0.24f)
            && lastLeadPitchValid && std::abs(m - lastLeadPitch) < 0.18f
            && hopSeconds > 0.0005f;
        if (smoothFrame)
        {
            const float instantaneous = (m - lastLeadPitch) / hopSeconds;
            leadSlope += 0.28f * (instantaneous - leadSlope);
            // Max 6 cents: prevents aggressive upward vibrato predictions
            // from acting as false semitone correction at an attack.
            leadSemitones = std::clamp(leadSlope * 0.0030f, -0.06f, 0.06f);
        }
        else leadSlope = 0.0f;
        lastLeadPitch = m;
        lastLeadPitchValid = true;
        const float vibrato = m + leadSemitones - center;

        // Which note to aim for. By default judge by the slow centre so sung vibrato swinging across a
        // note boundary can't flip the target. `follow` moves the decision to the instantaneous pitch, which
        // tracks fast speech-like movement much better (against a reference hard-tune of a real rapped
        // vocal: 87% -> 93% note agreement).
        const float selectPitch = center + std::clamp (settings.follow, 0.0f, 1.0f) * (m - center);
        const int previousTarget = targetNote;
        const int proposed = nearestNote (selectPitch);
        // New targets must persist over consecutive YIN hops with adequate
        // periodicity. A transient pitch spike can therefore be observed but
        // cannot immediately make the shifter jump to a new scale degree.
        if (previousTarget >= 0 && proposed != previousTarget)
        {
            if (detector.getConfidence() < std::max (0.78f, settings.voicingThreshold)
                || ! std::isfinite (selectPitch))
            {
                pendingNote = -1;
                pendingNoteSeconds = 0.0f;
            }
            else
            {
                if (pendingNote != proposed)
                {
                    pendingNote = proposed;
                    pendingNoteSeconds = 0.0f;
                }
                pendingNoteSeconds += hopSeconds;
                const bool realOnset = onsetSlowPower > 1.0e-6f
                    && onsetFastPower > onsetSlowPower * 1.65f;
                // Strong energy onsets can switch sooner; a bare frequency
                // spike without an onset must persist longer. Octave-sized
                // leaps demand additional confidence to avoid harmonic flips.
                const float extraConfirmation = LargeNoteJumpGuard::extraConfirmationSeconds (
                    previousTarget, proposed, detector.getConfidence(), realOnset,
                    settings.precisionHardTune && settings.lowLatency && settings.ultraLowLatency);
                const bool reliableModerateMove = settings.strictRealtime
                    && std::abs(proposed - previousTarget) <= 6
                    && detector.getConfidence() >= 0.94f
                    && (twoCycle < 0.0f || twoCycle <= 0.20f);
                // A detected octave has *already* passed YIN's persistent-lag
                // continuity check. When the second-period comparison agrees,
                // let the control stage confirm sooner, rather than blindly
                // imposing another fixed 12ms veto. Still require at least
                // 8ms of independently sampled evidence on a new octave:
                // short-lived octave-harmonic glitches must not become notes.
                const bool octaveLeap = std::abs(proposed - previousTarget) >= 11;
                const bool independentlyVoiced = twoCycle >= 0.0f && twoCycle < 0.15f;
                const bool reliableOctave = settings.strictRealtime && !settings.precisionHardTune && octaveLeap
                    && proposed > previousTarget  // down-octaves need longer subharmonic proof
                    && detector.getConfidence() >= 0.95f && independentlyVoiced;
                const float octaveSafety = octaveLeap ? (reliableOctave ? 0.002f : 0.012f) : 0.0f;
                const float required = extraConfirmation + settings.noteConfirmMs * 0.001f
                    * (reliableModerateMove ? 0.60f
                       : (realOnset && detector.getConfidence() > 0.91f ? 0.55f : 1.0f))
                    + octaveSafety;
                // A zero Retune Speed must mean instantaneous synthesis once
                // the note is *confirmed*, not acceptance of a one-hop phase
                // glitch. Require a few independent analysis updates for
                // non-onset Precision Live note switches; source output delay
                // remains 3.5 ms and there is no new audio lookahead.
                // Larger leaps are already subject to the separate octave/
                // confidence guards. Preserve immediate well-evidenced wide
                // note switches; only adjacent-note jitter needs this floor.
                const float minimumEvidence = reliableOctave ? 0.008f
                    : (settings.strictRealtime && settings.precisionHardTune
                        && std::abs(proposed - previousTarget) <= 2
                        && !realOnset ? 0.006f : 0.0f);
                if (pendingNoteSeconds >= std::max(required, minimumEvidence))
                {
                    targetNote = proposed;
                    pendingNote = -1;
                    pendingNoteSeconds = 0.0f;
                }
            }
        }
        else
        {
            pendingNote = -1;
            pendingNoteSeconds = 0.0f;
            if (previousTarget < 0)
                targetNote = detector.getConfidence() >= std::max (0.75f, settings.voicingThreshold) ? proposed : -1;
        }
        if (targetNote < 0) { period = 0.0f; targetRatio = 1.0f; return; }
        // Glide applies only on real note changes, never on the initial acquisition.
        // Its separate smoothing prevents short retune times from producing an abrupt
        // correction jump at a note boundary. An off setting preserves legacy DSP.
        if (previousTarget >= 0 && targetNote != previousTarget && settings.glideMs > 0.0f)
        {
            glideRemainingSeconds = settings.glideMs * 0.001f;
            glideShift = shiftSemitones;
        }
        float error = (float) targetNote - center;

        const float deadband = std::clamp (settings.humanize, 0.0f, 1.0f) * 0.4f;
        error = std::copysign (std::max (0.0f, std::abs (error) - deadband), error);

        // While evaluating a possible new note, do NOT pull its audio toward
        // the previous target (which could be several semitones away). Coast
        // gently toward unity until the candidate is confirmed or rejected.
        // Keep a short, unconfirmed target candidate from yanking the
        // correction toward either note. Hold the last valid correction
        // until the pitch change is accepted, rather than adding a scoop.
        const float desired = pendingNote >= 0 ? correction
            : error * std::clamp (settings.strength, 0.0f, 1.0f);
        const float coeff = settings.retuneMs < 0.5f ? 1.0f : 1.0f - std::exp (-hopSeconds * 1000.0f / settings.retuneMs);
        correction += (desired - correction) * coeff;

        const float flatten = settings.strength * (1.0f - std::clamp (settings.vibratoPreserve, 0.0f, 1.0f));
        float shift = std::clamp (correction - flatten * vibrato,
                                  -settings.maxCorrectionSemitones, settings.maxCorrectionSemitones);
        if (glideRemainingSeconds > 0.0f && settings.glideMs > 0.0f)
        {
            // First-order slew over the selected transition duration. Outside a
            // transition it introduces no extra lag or permanent pitch error.
            const float alpha = 1.0f - std::exp (-hopSeconds / (settings.glideMs * 0.001f));
            glideShift += (shift - glideShift) * alpha;
            glideRemainingSeconds = std::max (0.0f, glideRemainingSeconds - hopSeconds);
            // Avoid a discontinuity at the nominal end of Glide: exponential
            // interpolation usually has residual error after one time constant.
            // Continue the tail until it is effectively at the new correction.
            if (glideRemainingSeconds <= 0.0f && std::abs (glideShift - shift) > 0.01f)
                glideRemainingSeconds = hopSeconds;
            shift = glideShift;
        }
        else
            glideShift = shift;

        targetRatio = std::exp2 (shift / 12.0f);
        // IMPORTANT: use the same robust MIDI estimate for grain period and
        // note decisions. Otherwise a rejected one-hop octave glitch can
        // still halve the grain period and generate an audible crunch.
        period = (float) (sampleRate / (double) settings.referenceHz
                          * std::exp2 ((69.0 - (double) m) / 12.0));
        publishMeters (m, targetNote, shift);
    }

    double sampleRate = 44100.0;
    uint64_t analysisHops = 0;
    Settings settings;
    int mask = 0xfff;

    static constexpr int dirtyBit = 4;
    std::array<Settings, 3> slots {};
    std::atomic<int> mailbox { 1 };   // the slot in transit, plus the dirty flag
    int writeSlot = 0, readSlot = 2;  // owned by the writer / audio thread respectively
    float glideRemainingSeconds = 0.0f, glideShift = 0.0f;
    float wetGain = 1.0f, fadeStep = 0.01f;
    float microAmountMix = 1.0f, amountFadeCoeff = 0.01f;
    bool idle = false;

    YinDetector detector;
    PsolaShifter shifter; // mono processing path
    MicroLatencyShifter microShifter;
    MicroLatencyShifter qualityShifter;
    SnapLiveShifter snapShifter;                       // V1.13 Snap Live, mono path
    std::array<SnapLiveShifter, 2> channelSnapShifters {};
    float snapBlend = 0.0f, snapBlendCoeff = 0.01f;
    HistoricalCycleExperiment historicalShifter;
    FixedHorizonVoicedRenderer fixedHorizonShifter;
    std::array<FixedHorizonVoicedRenderer, 2> channelFixedHorizonShifters {};
    bool wasFixedHorizonActive=false;
    std::array<HistoricalCycleExperiment, 2> channelHistoricalShifters {};
    bool wasHistoricalActive = false;
    std::array<MicroLatencyShifter, 2> channelMicroShifters {};
    std::array<MicroLatencyShifter, 2> channelQualityShifters {};
    std::array<PsolaShifter, 2> channelShifters {}; // stereo-linked, independently aligned output paths
    std::array<double, 2> channelEnergy { 0.0, 0.0 };
    double stereoCrossEnergy = 0.0;
    std::array<double, 2> polarityEnergy {0.0, 0.0};
    float analysisPolarity = 1.0f;
    int pendingStereoSwitch = 0;
    bool channelsPathActive = false;
    int activeChannelCount = 1;
    int analysisChannel = 0;

    float lastLeadPitch = 0.0f, leadSlope = 0.0f;
    bool lastLeadPitchValid = false;
    float hopSeconds = 0.005f, ratioCoeff = 0.01f, holdLimit = 0.02f, holdTime = 0.0f;
    float onsetFastCoeff = 0.005f, onsetSlowCoeff = 0.001f;
    float onsetFastPower = 0.0f, onsetSlowPower = 0.0f;
    int fastHopRemaining = 0;
    bool fastHopActive = false;
    uint64_t fastHopSamples = 0;
    std::array<float, 3> history { { 0, 0, 0 } };
    int medianCount = 0, targetNote = -1, pendingNote = -1;
    float pendingNoteSeconds = 0.0f;
    bool hasCenter = false;
    float center = 0.0f, correction = 0.0f;
    float motionMean = 0.0f, motionEvidence = 0.0f;
    float motionHigh = 0.0f, motionLow = 0.0f;
    float motionRollback = 0.0f, motionPriorCenter = 0.0f;
    float recentAttackSeconds = 0.0f;
    bool reacquireGuard = false;
    int motionPriorTarget = -1;
    int motionDirection = 0;
    float ratio = 1.0f, targetRatio = 1.0f, period = 0.0f;
    float outputPeriod = 0.0f;
    bool outputPeriodValid = false;

    float detectedMidi = -1.0f, shiftSemitones = 0.0f;
    int targetMidi = -1;
};

//==============================================================================
// Constant, integer-sample delay for sources generated INSIDE the plugin (e.g.
// the reference piano tone). Such sources have no intrinsic pitch-shifter delay
// and cannot use the pitch-dependent compensation tap from the vocal stream.
// prepare() allocates; processSample() is allocation-free and noexcept.
class FixedDirectDelay
{
public:
    // Reserve the largest selectable delay once, outside the audio callback.
    // The active tap can then change without allocating or clearing history.
    void prepare (int maxDelaySamples)
    {
        capacity = std::max (0, maxDelaySamples);
        ring.assign ((size_t) capacity + 1u, 0.0f);
        delay = previousDelay = capacity;
        fadeLength = fadePosition = 0;
        write = 0;
    }

    void reset() noexcept
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        write = 0;
        previousDelay = delay;
        fadePosition = fadeLength = 0;
    }

    // Crossfade only when the user changes latency modes. Holding the tap
    // perfectly still between toggles avoids resampling and detuning vocals.
    void setDelaySamples (int newDelay, int transitionSamples = 0) noexcept
    {
        const int requested = std::clamp (newDelay, 0, capacity);
        if (requested == delay) return;
        previousDelay = delay;
        delay = requested;
        fadeLength = std::max (0, transitionSamples);
        fadePosition = 0;
    }

    int getLatencySamples() const noexcept { return delay; }
    int getCapacitySamples() const noexcept { return capacity; }

    float processSample (float input) noexcept
    {
        if (! std::isfinite (input)) input = 0.0f;
        if (ring.empty()) return 0.0f;
        ring[write] = input;
        const auto readAt = [this] (int offset) noexcept -> float
        {
            const size_t read = (write + ring.size() - (size_t) offset) % ring.size();
            return ring[read];
        };
        const float next = readAt (delay);
        float output = next;
        if (fadePosition < fadeLength)
        {
            const float previous = readAt (previousDelay);
            const float alpha = (float) (fadePosition + 1) / (float) fadeLength;
            output = previous + alpha * (next - previous);
            ++fadePosition;
        }
        write = (write + 1u) % ring.size();
        return output;
    }

private:
    std::vector<float> ring;
    size_t write = 0;
    int capacity = 0, delay = 0, previousDelay = 0;
    int fadeLength = 0, fadePosition = 0;
};

//==============================================================================
// PDC-correct, allocation-free bypass for a DAW plugin that reports latency.
// The input and synthesized output are both heard at the *same host delay*.
// A conventional wet/dry crossfade using the undelayed input can null vocal
// harmonics even if the pitch shifter itself is behaving correctly. Prepare
// this helper alongside OutputLatencyCompensator and advance it ONCE per sample
// for all channels to keep stereo imaging stable. The shifter can stay warm
// while bypassed, avoiding a cold-start transient upon reactivation.
class LatencyAlignedBypass
{
public:
    void prepare (int hostLatencySamples, double sampleRate)
    {
        for (auto& delay : dryDelays)
            delay.prepare (hostLatencySamples);
        const double rate = std::max (8000.0, sampleRate);
        // Short but click-free transition, independent of host block size.
        alpha = (float) (1.0 - std::exp (-1.0 / (rate * 0.003)));
        bypassMix = target = 0.0f;
    }

    void reset() noexcept
    {
        for (auto& delay : dryDelays) delay.reset();
        bypassMix = target = 0.0f;
    }

    // Change both dry-channel PDC taps together; no audio-thread allocation.
    void setHostLatencySamples (int samples, int fadeSamples = 0) noexcept
    {
        for (auto& delay : dryDelays)
            delay.setDelaySamples (samples, fadeSamples);
    }
    int getHostLatencySamples() const noexcept { return dryDelays[0].getLatencySamples(); }

    void setBypassed (bool bypassed) noexcept { target = bypassed ? 1.0f : 0.0f; }
    float delayDry (int channel, float x) noexcept
    {
        return dryDelays[(size_t) std::clamp (channel, 0, 1)].processSample (x);
    }
    void advance() noexcept
    {
        bypassMix += (target - bypassMix) * alpha;
        if (std::abs (target - bypassMix) < 1.0e-5f) bypassMix = target;
    }
    float mix (float processed, float delayedDry) const noexcept
    { return processed + bypassMix * (delayedDry - processed); }
    float getBypassMix() const noexcept { return bypassMix; }

private:
    std::array<FixedDirectDelay, 2> dryDelays;
    float alpha = 0.0f, bypassMix = 0.0f, target = 0.0f;
};

//==============================================================================
// Pads an audio stream to a fixed host-declared latency while accounting for the
// pitch-dependent intrinsic delay of the PSOLA engine. Storage is allocated in
// prepare(); processChannels() is lock-free and allocation-free. Large delay-tap
// changes are crossfaded to avoid a hard discontinuity, while small changes track
// smoothly. This keeps the long-term output alignment fixed; note transitions can
// still contain a short crossfade while the compensation tap moves.
class OutputLatencyCompensator
{
public:
    void prepare (int fixedLatencySamples, double newSampleRate)
    {
        fixedLatency = maximumLatency = std::max (0, fixedLatencySamples);
        const double safeSampleRate = std::max (8000.0, newSampleRate);
        ringSize = std::max (16, fixedLatency + 16);
        for (auto& channel : delayLines)
            channel.assign ((size_t) ringSize, 0.0f);
        fadeLength = std::max (32, (int) std::lround (safeSampleRate * 0.004)); // 4 ms tap crossfade
        // Chosen once per sample rate, not derived from changing voiced pitch.
        // Approximate wet-vs-host alignment; never alter this while a sustained
        // note is playing (the previous per-sample movement detuned the voice).
        stableWetPadding = (int) std::lround ((double) fixedLatency * 0.60);
        reset();
    }

    void reset() noexcept
    {
        for (auto& channel : delayLines)
            std::fill (channel.begin(), channel.end(), 0.0f);
        writePosition = 0;
        overBudgetSamples = 0;
        maxObservedIntrinsic = 0.0f;
        for (int c = 0; c < (int) currentDelay.size(); ++c)
        {
            currentDelay[(size_t)c] = (float) fixedLatency;
            fadeFrom[(size_t)c] = currentDelay[(size_t)c];
            fadeTo[(size_t)c] = currentDelay[(size_t)c];
            fadePosition[(size_t)c] = fadeLength;
        }
    }

    // The legacy per-sample latency pad is a variable-rate resampler: when
    // intrinsic telemetry moves it bends the pitch AFTER correction. On a real
    // hard-tuned vocal that substantially undid the pitch lock. Use a fixed
    // fractional transport pad for tuned audio instead. The shifter's intrinsic
    // group delay is pitch-dependent, so the advertised latency remains an
    // approximate transport budget, NOT guaranteed exact sample alignment.
    // Null telemetry (generated tones/direct audio) remains an exact full pad.
    // Switch the host PDC budget without reallocating either delay line.
    // prepare() reserves the Normal-mode maximum; the Live-mode tap is shorter.
    // A one-off crossfade handles switching, but the tap remains fixed for
    // every sustained note to preserve V15's pitch-lock correction.
    void setHostLatencySamples (int samples) noexcept
    {
        const int newLatency = std::clamp (samples, 0, maximumLatency);
        if (newLatency == fixedLatency) return;
        fixedLatency = newLatency;
        stableWetPadding = (int) std::lround ((double) fixedLatency * 0.60);
    }
    int getPreparedMaximumLatencySamples() const noexcept { return maximumLatency; }

    void setPitchPreserving (bool enabled) noexcept { pitchPreserving = enabled; }
    bool isPitchPreserving() const noexcept { return pitchPreserving; }
    int getStableWetPaddingSamples() const noexcept { return stableWetPadding; }

    int getFixedLatencySamples() const noexcept { return fixedLatency; }
    uint64_t getOverBudgetSamples() const noexcept { return overBudgetSamples; }
    float getMaxObservedIntrinsicSamples() const noexcept { return maxObservedIntrinsic; }
    float getCurrentPaddingSamples (int channel = 0) const noexcept
    {
        return currentDelay[(size_t) std::clamp (channel, 0, (int) currentDelay.size() - 1)];
    }

    // `intrinsicLatencySamples` is optional. If supplied, it contains channel-major
    // telemetry: [channel 0: numSamples values][channel 1: numSamples values]. Null
    // means a direct (zero-intrinsic-latency) path that must receive the full pad.
    void processChannels (float* const* channels, int numChannels, int numSamples,
                          const float* intrinsicLatencySamples = nullptr) noexcept
    {
        if (channels == nullptr || numChannels <= 0 || numSamples <= 0 || ringSize <= 0)
            return;

        const int count = std::min (numChannels, (int) delayLines.size());
        for (int i = 0; i < numSamples; ++i)
        {
            for (int c = 0; c < count; ++c)
            {
                const float rawIntrinsic = intrinsicLatencySamples != nullptr
                    ? intrinsicLatencySamples[(size_t) c * (size_t) numSamples + (size_t) i] : 0.0f;
                // A clamped negative padding means host PDC cannot be accurate.
                // Expose this instead of silently hiding a budget underflow.
                if (std::isfinite (rawIntrinsic) && rawIntrinsic > maxObservedIntrinsic)
                    maxObservedIntrinsic = rawIntrinsic;
                if (! std::isfinite (rawIntrinsic) || rawIntrinsic > (float) fixedLatency + 0.5f)
                    ++overBudgetSamples;
                const float intrinsic = std::clamp (std::isfinite (rawIntrinsic) ? rawIntrinsic : 0.0f,
                                                   0.0f, (float) fixedLatency);
                // Don't allow source-mark / estimator jitter to become another
                // pitch shifter. Still support legacy behavior for comparative
                // tests or for a caller explicitly opting out.
                const float targetDelay = pitchPreserving && intrinsicLatencySamples != nullptr && rawIntrinsic > 0.0f
                    ? (float) stableWetPadding : (float) fixedLatency - intrinsic;
                const size_t ch = (size_t) c;

                if (fadePosition[ch] < fadeLength)
                {
                    // Let the destination follow larger note changes without restarting
                    // the fade on every detector/grain update.
                    if (std::abs (targetDelay - fadeTo[ch]) > 2.0f)
                        fadeTo[ch] += 0.25f * (targetDelay - fadeTo[ch]);
                }
                else
                {
                    const float delta = targetDelay - currentDelay[ch];
                    if (std::abs (delta) > 8.0f)
                    {
                        fadeFrom[ch] = currentDelay[ch];
                        fadeTo[ch] = targetDelay;
                        fadePosition[ch] = 0;
                    }
                    else
                    {
                        // Small telemetry fluctuations track smoothly instead of
                        // creating repeated discontinuities at the read head.
                        currentDelay[ch] += 0.10f * delta;
                    }
                }
            }

            for (int c = 0; c < count; ++c)
                if (channels[c] != nullptr)
                    delayLines[(size_t) c][(size_t) writePosition] =
                        std::isfinite (channels[c][i]) ? channels[c][i] : 0.0f;

            for (int c = 0; c < count; ++c)
            {
                if (channels[c] == nullptr) continue;
                const size_t ch = (size_t) c;
                const bool fading = fadePosition[ch] < fadeLength;
                const float alpha = fading ? (float) fadePosition[ch] / (float) fadeLength : 1.0f;
                const float oldTap = fading ? readDelay (c, fadeFrom[ch]) : 0.0f;
                const float newTap = readDelay (c, fading ? fadeTo[ch] : currentDelay[ch]);
                channels[c][i] = fading ? oldTap + alpha * (newTap - oldTap) : newTap;

                if (fading)
                {
                    ++fadePosition[ch];
                    if (fadePosition[ch] >= fadeLength)
                    {
                        currentDelay[ch] = fadeTo[ch];
                        fadeFrom[ch] = fadeTo[ch];
                    }
                }
            }
            writePosition = (writePosition + 1) % ringSize;
        }
    }

private:
    float readDelay (int channel, float delaySamples) const noexcept
    {
        const float delay = std::clamp (delaySamples, 0.0f, (float) fixedLatency);
        float readPosition = (float) writePosition - delay;
        if (readPosition < 0.0f)
            readPosition += (float) ringSize;
        const int i0 = std::clamp ((int) std::floor (readPosition), 0, ringSize - 1);
        const int i1 = (i0 + 1) % ringSize;
        const float frac = readPosition - (float) i0;
        const auto& line = delayLines[(size_t) channel];
        return line[(size_t) i0] + frac * (line[(size_t) i1] - line[(size_t) i0]);
    }

    std::array<std::vector<float>, 2> delayLines;
    int fixedLatency = 0, maximumLatency = 0, ringSize = 0, writePosition = 0;
    int stableWetPadding = 0;
    bool pitchPreserving = false;
    uint64_t overBudgetSamples = 0;
    float maxObservedIntrinsic = 0.0f;
    int fadeLength = 192;
    std::array<int, 2> fadePosition { 192, 192 };
    std::array<float, 2> currentDelay { 0.0f, 0.0f }, fadeFrom { 0.0f, 0.0f }, fadeTo { 0.0f, 0.0f };
};

} // namespace autotune
