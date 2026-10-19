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
#include <vector>

namespace autotune
{
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
    bool  lowLatency = true;        // shorter detector hop and smaller grains; latency still depends on pitch/shift
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
        lastLag = framesSinceGood = 0;
        octaveProposal = octaveProposalFrames = 0;
    }

    // d' (normalised difference) below which a dip counts as pitched; lower = stricter.
    void setThreshold (float t) noexcept { threshold = std::clamp (t, 0.03f, 0.5f); }

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

        const float* newest = frame.data() + size - windowSize;

        float energy = 0.0f;
        for (int j = 0; j < windowSize; ++j)
            energy += newest[j] * newest[j];

        if (std::sqrt (energy / (float) windowSize) < silenceThreshold)
        {
            frequency = confidence = rawFrequency = alternateFrequency = 0.0f;
            if (++framesSinceGood > maxContinuityFrames) lastLag = 0;
            return;
        }

        for (int tau = 1; tau <= maxLag; ++tau)
        {
            const float* lagged = newest - tau;
            float sum = 0.0f;
            for (int j = 0; j < windowSize; ++j)
            {
                const float d = newest[j] - lagged[j];
                sum += d * d;
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
            if (++framesSinceGood > maxContinuityFrames) lastLag = 0;
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

        if (kDoubleLagRule && chosen > 0.008f && analysisRate / (double) tau < 350.0)
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
        // A 90-ms analysis span distinguishes 100/200 Hz much better than the
        // ordinary YIN difference window, without adding transport delay.
        const int count = std::min ((int) frame.size(), (int) (analysisRate * 0.090));
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
        const int window = windowSize * decimation;
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
        const int window = windowSize * decimation;
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
    int decimation = 1, maxLag = 0, minLag = 2, windowSize = 0, hopSize = 1;
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
    static constexpr bool kDoubleLagRule = true;
    int octaveProposal = 0, octaveProposalFrames = 0;
    int lastLag = 0, framesSinceGood = 0;                 // continuity memory (analysis-rate lag)
    static constexpr int maxContinuityFrames = 8;         // ~40 ms
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
        if (! std::isfinite (input)) input = 0.0f;
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
        shifter.prepare (sampleRate);
        for (auto& channelShifter : channelShifters)
            channelShifter.prepare (sampleRate);
        hopSeconds = detector.getHopSeconds();
        ratioCoeff = 1.0f - std::exp (-1.0f / (0.002f * (float) sampleRate));
        fadeStep   = 1.0f / (0.010f * (float) sampleRate);   // 10 ms bypass crossfade
        holdLimit  = 0.02f; // bridge detector dropouts up to this long; longer ones (consonants) fall back to pass-through
        onsetFastCoeff = 1.0f - std::exp (-1.0f / (float) (sampleRate * 0.002));
        onsetSlowCoeff = 1.0f - std::exp (-1.0f / (float) (sampleRate * 0.025));
        reset();
    }

    void reset()
    {
        detector.reset();
        shifter.reset();
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
        onsetFastPower = onsetSlowPower = 0.0f;
        channelsPathActive = false;
        activeChannelCount = 1;
        analysisChannel = 0;
        shifter.setParameters (1.0f, 0.0f);
        publishMeters (-1.0f, -1, 0.0f);
        wetGain = settings.bypass ? 0.0f : 1.0f;
        idle = settings.bypass;
    }

    // Apply settings immediately. Only call from the audio thread (or while audio is stopped).
    void setSettings (const Settings& s)
    {
        // Host automation and restored session state are external inputs. Reject
        // non-finite floats before they reach log2(), exp(), or MIDI-to-note casts.
        // Valid settings are left unchanged, preserving existing preset behavior.
        settings = s;
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
        detector.setThreshold (1.0f - settings.voicingThreshold);
        detector.setAnalysisHopSeconds (settings.lowLatency ? 0.0025f : 0.0050f);
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
        if (! std::isfinite (input)) input = 0.0f;
        channelsPathActive = false;
        activeChannelCount = 1;
        if (! advanceControl (input))
            return input; // completely bypassed: exact, zero-CPU pass-through

        const float wet = shifter.processSample (input);
        advanceWetGain();

        // Amount=0 means genuinely unprocessed audio. Keep advancing the shifter above
        // so its history stays warm if Amount is raised during playback, but never expose
        // the pitch-synchronous grain reconstruction when no correction is requested.
        if (settings.strength <= 1.0e-6f)
            return input;

        return wetGain >= 1.0f ? wet : input + wetGain * (wet - input);
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
                        if (! std::isfinite (channels[c][i])) channels[c][i] = 0.0f;
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
            analysisChannel = 0;

        for (int i = 0; i < numSamples; ++i)
        {
            for (int c = 0; c < trackedChannels; ++c)
            {
                // Clean the input before channel selection, detector history,
                // wet processing and the zero-strength passthrough.
                if (channels[c] != nullptr && ! std::isfinite (channels[c][i]))
                    channels[c][i] = 0.0f;
                const double x = channels[c] != nullptr ? (double) channels[c][i] : 0.0;
                channelEnergy[(size_t) c] += energyAlpha * (x * x - channelEnergy[(size_t) c]);
            }
            if (trackedChannels == 2)
            {
                const int other = 1 - analysisChannel;
                if (channelEnergy[(size_t) other] > channelEnergy[(size_t) analysisChannel] * 1.45)
                    analysisChannel = other;
            }

            const float analysis = channels[analysisChannel] != nullptr ? channels[analysisChannel][i] : 0.0f;
            if (! advanceControl (analysis))
            {
                if (intrinsicLatencySamples != nullptr)
                    for (int c = 0; c < trackedChannels; ++c)
                        intrinsicLatencySamples[(size_t) c * (size_t) numSamples + (size_t) i] = 0.0f;
                continue; // do not overwrite channels during full bypass
            }

            std::array<float, 2> dry { 0.0f, 0.0f };
            std::array<float, 2> wet { 0.0f, 0.0f };
            for (int c = 0; c < trackedChannels; ++c)
            {
                if (channels[c] == nullptr) continue;
                dry[(size_t)c] = channels[c][i];
                wet[(size_t)c] = channelShifters[(size_t)c].processSample (dry[(size_t)c]);
            }
            advanceWetGain();
            for (int c = 0; c < trackedChannels; ++c)
            {
                if (channels[c] == nullptr) continue;

                // Do not let an imperfect pitch estimate modulate an input whose Amount
                // is zero. Process the shifter above to keep its history warm for live
                // parameter changes, but leave the output samples untouched.
                if (settings.strength <= 1.0e-6f)
                    channels[c][i] = dry[(size_t)c];
                else
                    channels[c][i] = wetGain >= 1.0f ? wet[(size_t)c]
                        : dry[(size_t)c] + wetGain * (wet[(size_t)c] - dry[(size_t)c]);
            }

            if (intrinsicLatencySamples != nullptr)
            {
                const double wetFactor = settings.strength > 1.0e-6f ? (double) wetGain : 0.0;
                for (int c = 0; c < trackedChannels; ++c)
                {
                    const double intrinsic = channelShifters[(size_t) c].getLatencySamples() * wetFactor;
                    intrinsicLatencySamples[(size_t) c * (size_t) numSamples + (size_t) i]
                        = (float) std::max (0.0, intrinsic);
                }
            }
        }
    }

    // Meters for a tuner display (fractional MIDI note; -1 = none).
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
        if (! std::isfinite (analysisSample)) analysisSample = 0.0f;
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
                channelShifters[(size_t)c].setParameters (ratio, period);
        return true;
    }

    void advanceWetGain() noexcept
    {
        const float goal = settings.bypass ? 0.0f : 1.0f;
        if (wetGain < goal)      wetGain = std::min (goal, wetGain + fadeStep);
        else if (wetGain > goal) wetGain = std::max (goal, wetGain - fadeStep);
    }

    void resetTracking()
    {
        medianCount = 0; hasCenter = false; center = 0.0f; correction = 0.0f;
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

        // Unvoiced/noisy frames should not continue forcing the last note
        // through a consonant. Release before clearing the note history, so a
        // one-hop YIN dropout does not create flutter or spuriously reacquire.
        if (f <= 0.0f || detector.getConfidence() < std::max (0.52f, settings.voicingThreshold - 0.03f))
        {
            holdTime += hopSeconds;
            if (holdTime >= settings.consonantReleaseMs * 0.001f)
            {
                targetRatio = 1.0f;
                period = 0.0f;
                pendingNote = -1;
                pendingNoteSeconds = 0.0f;
            }
            if (holdTime > holdLimit)
            {
                resetTracking();
                period = 0.0f;                // unvoiced: pass audio through untouched
                publishMeters (-1.0f, -1, 0.0f);
            }
            return;
        }

        holdTime = 0.0f;
        const float midi = 69.0f + 12.0f * std::log2 (f / settings.referenceHz);

        // Median of the last three estimates rejects single-frame octave errors.
        history[(size_t) (medianCount % 3)] = midi;
        ++medianCount;
        const float med = medianCount >= 3 ? median3 (history[0], history[1], history[2]) : midi;
        const float m = std::abs (midi - med) > 0.5f ? med : midi;

        // Slow "centre" of the pitch track; what is left over is vibrato.
        // A jump of nearly a semitone means a new note, so the centre re-locks at once.
        if (! hasCenter || std::abs (m - center) > 0.9f)
        {
            center = m; hasCenter = true;
            }
        else if (settings.noteStabilizerMs <= 0.5f)
            center = m;
        else
            center += (m - center) * (1.0f - std::exp (-hopSeconds / (settings.noteStabilizerMs * 0.001f)));

        // Vibrato = how far the current estimate sits from the slow centre. The shifter's grain spacing comes
        // from the same estimate, so ratio and period always agree and no lead/extrapolation is wanted:
        // an earlier lead term was measured to add error equal to (pitch slope x lead) once grains were
        // aligned to the real pulses.
        const float vibrato = m - center;

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
                const float required = settings.noteConfirmMs * 0.001f
                    * (realOnset && detector.getConfidence() > 0.91f ? 0.55f : 1.0f)
                    + (std::abs (proposed - previousTarget) >= 11 ? 0.012f : 0.0f);
                if (pendingNoteSeconds >= required)
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
    bool idle = false;

    YinDetector detector;
    PsolaShifter shifter; // mono processing path
    std::array<PsolaShifter, 2> channelShifters {}; // stereo-linked, independently aligned output paths
    std::array<double, 2> channelEnergy { 0.0, 0.0 };
    bool channelsPathActive = false;
    int activeChannelCount = 1;
    int analysisChannel = 0;

    float hopSeconds = 0.005f, ratioCoeff = 0.01f, holdLimit = 0.02f, holdTime = 0.0f;
    float onsetFastCoeff = 0.005f, onsetSlowCoeff = 0.001f;
    float onsetFastPower = 0.0f, onsetSlowPower = 0.0f;
    std::array<float, 3> history { { 0, 0, 0 } };
    int medianCount = 0, targetNote = -1, pendingNote = -1;
    float pendingNoteSeconds = 0.0f;
    bool hasCenter = false;
    float center = 0.0f, correction = 0.0f;
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
    void prepare (int delaySamples)
    {
        delay = std::max (0, delaySamples);
        ring.assign ((size_t) delay + 1u, 0.0f);
        write = 0;
    }

    void reset() noexcept
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        write = 0;
    }

    int getLatencySamples() const noexcept { return delay; }

    float processSample (float input) noexcept
    {
        // Keep invalid host samples out of persistent delay history.
        if (! std::isfinite (input)) input = 0.0f;
        if (ring.empty()) return 0.0f;
        ring[write] = input;
        const size_t read = (write + 1u) % ring.size();
        const float output = ring[read];
        write = read;
        return output;
    }

private:
    std::vector<float> ring;
    size_t write = 0;
    int delay = 0;
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
        fixedLatency = std::max (0, fixedLatencySamples);
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
    int fixedLatency = 0, ringSize = 0, writePosition = 0;
    int stableWetPadding = 0;
    bool pitchPreserving = false;
    uint64_t overBudgetSamples = 0;
    float maxObservedIntrinsic = 0.0f;
    int fadeLength = 192;
    std::array<int, 2> fadePosition { 192, 192 };
    std::array<float, 2> currentDelay { 0.0f, 0.0f }, fadeFrom { 0.0f, 0.0f }, fadeTo { 0.0f, 0.0f };
};

} // namespace autotune
