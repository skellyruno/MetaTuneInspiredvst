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
    float strength = 1.0f;          // 0..1 how much of the correction is applied
    float humanize = 0.0f;          // 0..1 tolerance: errors inside humanize*40 cents are left alone
    float vibratoPreserve = 0.7f;   // 0..1 how much natural vibrato survives (1 = all of it)
    float follow = 0.0f;            // 0..1 how closely the target note follows the instantaneous pitch.
                                    // 0 = judge by the slow centre (safe for sung vibrato); 1 = follow every
                                    // movement (best for rapped/spoken vocals; wide vibrato may flip notes)
    float voicingThreshold = 0.85f; // 0..1 detector confidence needed to treat input as pitched
    float noteStabilizerMs = 300.0f; // 0 disables slow pitch-centre stabilisation
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
        decimation   = sampleRate > 60000.0 ? 4 : (sampleRate > 30000.0 ? 2 : 1);
        analysisRate = sampleRate / decimation;
        maxLag       = (int) std::ceil (analysisRate / minHz);
        minLag       = std::max (2, (int) std::floor (analysisRate / maxHz));
        windowSize   = maxLag;
        hopSize      = std::max (1, (int) (analysisRate * 0.005));

        fullSize = ((size_t) (windowSize + maxLag + 4) * (size_t) decimation) + 8;
        fullHistory.assign (decimation > 1 ? fullSize : 0, 0.0f);
        history.assign ((size_t) (windowSize + maxLag), 0.0f);
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
        frequency = confidence = 0.0f;
        lastLag = framesSinceGood = 0;
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

        decimationSum += x;
        if (++decimationCount < decimation)
            return false;

        history[(size_t) writeIndex] = decimationSum / (float) decimation;
        writeIndex = (writeIndex + 1) % (int) history.size();
        decimationSum = 0.0f;
        decimationCount = 0;

        if (++samplesSinceAnalysis < hopSize)
            return false;

        samplesSinceAnalysis = 0;
        analyse();
        return true;
    }

    float getFrequency() const noexcept  { return frequency; }   // Hz, 0 = unpitched
    float getConfidence() const noexcept { return confidence; }  // 0..1
    float getPeriodInSamples() const noexcept
    {
        return frequency > 0.0f ? (float) (analysisRate * decimation) / frequency : 0.0f;
    }
    float getHopSeconds() const noexcept { return (float) (hopSize * decimation / (analysisRate * decimation)); }

private:
    void analyse() noexcept
    {
        const auto size = (int) history.size();
        for (int i = 0; i < size; ++i)
            frame[(size_t) i] = history[(size_t) ((writeIndex + i) % size)];

        const float* newest = frame.data() + maxLag;

        float energy = 0.0f;
        for (int j = 0; j < windowSize; ++j)
            energy += newest[j] * newest[j];

        if (std::sqrt (energy / (float) windowSize) < silenceThreshold)
        {
            frequency = confidence = 0.0f;
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
            frequency = confidence = 0.0f;
            if (++framesSinceGood > maxContinuityFrames) lastLag = 0;
            return;
        }

        tau = resolveOctave (tau);
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
    //  1. continuity: an estimate an octave (or twelfth) away from the pitch tracked a few
    //     frames ago is almost always an error, so prefer a dip near the old lag if one exists.
    //     A genuine leap loses that dip and is accepted immediately.
    //  2. a clearly deeper dip at 2x the lag means the first dip was the half-period.
    int resolveOctave (int tau) const noexcept
    {
        const float chosen = difference[(size_t) tau];

        if (lastLag > 0)
        {
            const float ratio = (float) tau / (float) lastLag;
            const bool octaveAway = (std::abs (ratio - 2.0f) < 0.16f) || (std::abs (ratio - 3.0f) < 0.24f)
                                 || (std::abs (ratio - 0.5f) < 0.04f) || (std::abs (ratio - 0.3333f) < 0.03f);
            if (octaveAway)
            {
                const int t = deepestNear ((float) lastLag, 0.06f);
                if (t > 0 && difference[(size_t) t] < 0.35f && difference[(size_t) t] <= chosen + 0.2f)
                    return t;
            }
        }

        if (kDoubleLagRule && chosen > 0.04f)
            for (const float multiple : { 2.0f })
            {
                const int t = deepestNear ((float) tau * multiple, 0.05f);
                if (t > 0 && difference[(size_t) t] < threshold && difference[(size_t) t] < 0.6f * chosen)
                    return t;
            }

        return tau;
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
        const int newest = (int) fullWrite - 1;
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

    int decimation = 1, maxLag = 0, minLag = 2, windowSize = 0, hopSize = 1;
    double analysisRate = 44100.0;
    float threshold = 0.15f;

    std::vector<float> history, frame, difference, fullHistory;
    size_t fullSize = 0, fullWrite = 0;
    int writeIndex = 0, decimationCount = 0, samplesSinceAnalysis = 0;
    float decimationSum = 0.0f, frequency = 0.0f, confidence = 0.0f;
    static constexpr bool kDoubleLagRule = false;
    int lastLag = 0, framesSinceGood = 0;                 // continuity memory (analysis-rate lag)
    static constexpr int maxContinuityFrames = 8;         // ~40 ms
};

//==============================================================================
// Real-time TD-PSOLA: Hann grains cut at the input pitch period and re-laid at
// period / pitchRatio. Formants stay put, so the voice doesn't turn chipmunk.
class PsolaShifter
{
public:
    void prepare (double sampleRate)
    {
        size_t n = 1;
        while (n < (size_t) (sampleRate * 0.5)) n <<= 1;
        buffer.assign (n, 0.0f);
        mask = (int64_t) n - 1;
        unvoicedPeriod = (float) (sampleRate * 0.008);
        maxHalfWidth   = (float) (sampleRate * 0.03);
        scores.assign ((size_t) (0.3 * maxHalfWidth) + 8, 0.0f);          // search window: 2 x 0.15 periods
        reference.assign ((size_t) (0.4 * maxHalfWidth) + 16, 0.0f);      // template: 2 x 0.2 periods

        for (size_t i = 0; i < window.size(); ++i)
            window[i] = 0.5f - 0.5f * std::cos (6.2831853f * (float) i / (float) (window.size() - 1));
        reset();
    }

    void reset()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        for (auto& g : grains) g.active = false;
        now = 0; nextMark = 0.0; hasLastSource = false; lastDelay = 0.5 * (double) unvoicedPeriod + 4.0; unvoicedDelay = std::round (lastDelay);
    }

    // Current input-to-output delay estimate in samples. Normal geometry is nominally
    // about 1.45 periods, but source-period repetition can add nearly one period on an
    // upward shift. The processor's output pad covers that bounded excursion.
    double getLatencySamples() const noexcept { return hasLastSource ? lastDelay : unvoicedDelay; }

    // Low-latency mode trades some pulse-context for shorter grains and a shorter
    // source look-back. It is still pulse-aligned; only the grain/search geometry changes.
    void setLowLatency (bool enabled) noexcept { lowLatencyMode = enabled; }

    // pitchRatio = out/in frequency; period <= 0 means unvoiced (pitch left alone).
    void setParameters (float newPitchRatio, float newPeriod) noexcept
    {
        const bool voiced = newPeriod > 0.0f;
        period     = voiced ? std::clamp (newPeriod, 8.0f, maxHalfWidth) : unvoicedPeriod;
        pitchRatio = voiced ? std::clamp (newPitchRatio, 0.25f, 4.0f) : 1.0f;
        isVoiced   = voiced;
    }

    float processSample (float input) noexcept
    {
        buffer[(size_t) (now & mask)] = input;

        while (nextMark - halfWidth() <= (double) now)
            spawnGrain();

        float sum = 0.0f, weight = 0.0f;
        for (auto& g : grains)
        {
            if (! g.active) continue;
            const double age = (double) now - g.start;
            if (age >= g.length) { g.active = false; continue; }
            if (age < 0.0) continue;

            const float w = windowAt ((float) (age / g.length));
            sum += w * read (g.sourceStart + age);
            weight += w;
        }
        ++now;

        const float gap = pitchRatio < 1.0f ? std::sqrt (1.0f / pitchRatio) : 1.0f;
        // Normalise by the actual overlap weight. Clamping the denominator to 1.0
        // attenuates short grains because their overlapping Hann windows sum to <1.
        // The tiny floor handles only the no-coverage/numeric-edge case.
        return (weight > 1.0e-5f ? sum / weight : 0.0f) * gap;
    }

private:
    struct Grain { bool active = false; double start = 0.0, length = 1.0, sourceStart = 0.0; };

    float halfWidth() const noexcept
    {
        if (! lowLatencyMode)
            return std::min (maxHalfWidth, period);

        // Keep a little overlap without carrying the previous 1.20-period support
        // into every ratio. At unity, 0.515 periods gives 1.03 periods of grain
        // support. Downward shifts get 1.10 output periods via 0.55 / pitchRatio;
        // this wider margin is important for preserving the tested downward tuning.
        // More aggressive source-search/template reductions were tested separately
        // and rejected because they regressed corrected-note accuracy.
        const float overlapSafeWidth = std::max (0.515f, 0.55f / std::max (0.25f, pitchRatio));
        return std::min (maxHalfWidth, period * overlapSafeWidth);
    }

    void spawnGrain() noexcept
    {
        const float hw = halfWidth();
        const double centre = nextMark;
        nextMark += period / pitchRatio;

        double sourceCentre;

        if (! isVoiced)
        {
            // Unvoiced: contiguous pass-through at a constant, whole-sample delay (bit-exact reconstruction).
            sourceCentre = centre - unvoicedDelay;
            hasLastSource = false;
        }
        else
        {
            // Voiced: each source grain is placed one estimated period after the previous one, then slid a
            // little to wherever the waveform best matches the previous grain. That locks successive source
            // grains to the real glottal pulses, so the output pitch is set by the synthesis spacing alone
            // and does not inherit errors or staleness in the pitch estimate.
            const double reach = (lowLatencyMode ? 0.05 : 0.15) * (double) period;
            const double half = std::max (4.0, (lowLatencyMode ? 0.1 : 0.2) * (double) period);
            const double safetyMargin = (lowLatencyMode ? 0.005 : 0.1) * (double) period;
            const double desired = centre - ((double) hw + reach + half + 3.0 + safetyMargin);

            if (! hasLastSource)
                sourceCentre = desired;
            else
            {
                // Keep source grains on whole pitch periods. This floor-based phase
                // progression preserves the tested pitch-shifting behavior; it can
                // repeat a pulse temporarily on upward shifts, so the host latency
                // budget must cover the resulting bounded delay excursion.
                const double predicted = lastSourceCentre
                                       + std::floor ((desired - lastSourceCentre) / (double) period + 0.1) * (double) period;
                const double newest = (double) now - 2.0 - half;            // latest centre whose neighbourhood exists
                const int lo = (int) std::floor (predicted - reach), hi = (int) std::floor (std::min (predicted + reach, newest));
                sourceCentre = std::min (predicted, newest);

                const int h = (int) half, count = std::min (hi - lo + 1, (int) scores.size());
                if (count > 2 && 2 * h + 1 <= (int) reference.size())
                {
                    // The previous grain's neighbourhood is the template (read once, fractional position).
                    float refEnergy = 1.0e-9f;
                    for (int j = -h; j <= h; ++j) { const float r = read (lastSourceCentre + (double) j); reference[(size_t) (j + h)] = r; refEnergy += r * r; }

                    float best = -2.0f; int bestAt = 0;
                    for (int i = 0; i < count; ++i)
                    {
                        const int64_t centreIndex = (int64_t) lo + i;
                        float num = 0.0f, energy = 1.0e-9f;
                        for (int j = -h; j <= h; ++j)
                        {
                            const float a = buffer[(size_t) ((centreIndex + j) & mask)];
                            num += a * reference[(size_t) (j + h)]; energy += a * a;
                        }
                        const float score = num / std::sqrt (energy * refEnergy);
                        scores[(size_t) i] = score;
                        if (score > best) { best = score; bestAt = i; }
                    }

                    if (best > 0.3f)
                    {
                        double offset = 0.0;
                        if (bestAt > 0 && bestAt + 1 < count)
                        {
                            const float l = scores[(size_t) bestAt - 1], c = scores[(size_t) bestAt], r = scores[(size_t) bestAt + 1];
                            const float den = l - 2.0f * c + r;
                            if (std::abs (den) > 1.0e-9f) offset = std::clamp (0.5 * (double) (l - r) / (double) den, -0.5, 0.5);
                        }
                        sourceCentre = (double) (lo + bestAt) + offset;
                    }
                }
            }
            hasLastSource = true;
            lastSourceCentre = sourceCentre;
            lastDelay = centre - sourceCentre;
            unvoicedDelay = std::max (8.0, std::round (lastDelay));
        }

        for (auto& g : grains)
        {
            if (g.active) continue;
            g.active = true;
            g.start = centre - hw;
            g.length = 2.0 * hw;
            g.sourceStart = sourceCentre - hw;
            return;
        }
    }

    float windowAt (float position) const noexcept
    {
        const float index = position * (float) (window.size() - 1);
        const size_t i = (size_t) index;
        const float frac = index - (float) i;
        return i + 1 < window.size() ? window[i] + frac * (window[i + 1] - window[i]) : window.back();
    }

    float read (double position) const noexcept
    {
        const int64_t i = (int64_t) std::floor (position);
        const float f = (float) (position - (double) i);
        const float xm1 = buffer[(size_t) ((i - 1) & mask)], x0 = buffer[(size_t) (i & mask)],
                    x1  = buffer[(size_t) ((i + 1) & mask)], x2 = buffer[(size_t) ((i + 2) & mask)];
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }

    std::vector<float> buffer, scores, reference;
    int64_t mask = 0, now = 0;
    bool lowLatencyMode = false;
    std::array<Grain, 24> grains {};
    std::array<float, 1024> window {};
    double nextMark = 0.0, lastSourceCentre = 0.0, lastDelay = 196.0, unvoicedDelay = 196.0;
    bool hasLastSource = false, isVoiced = false;
    float period = 350.0f, unvoicedPeriod = 350.0f, maxHalfWidth = 1300.0f, pitchRatio = 1.0f;
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
        channelEnergy = { 0.0, 0.0 };
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
        settings = s;
        mask = scaleMask (s.scale, s.customScale);
        detector.setThreshold (1.0f - std::clamp (s.voicingThreshold, 0.0f, 1.0f));
        detector.setAnalysisHopSeconds (s.lowLatency ? 0.0025f : 0.0050f);
        shifter.setLowLatency (s.lowLatency);
        for (auto& channelShifter : channelShifters)
            channelShifter.setLowLatency (s.lowLatency);
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
        applyPendingSettings();
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

        if (detector.push (analysisSample))
            updateControl();

        ratio += (targetRatio - ratio) * ratioCoeff;
        shifter.setParameters (ratio, period);
        for (auto& channelShifter : channelShifters)
            channelShifter.setParameters (ratio, period);
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

        if (f <= 0.0f)
        {
            holdTime += hopSeconds;
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
        targetNote = nearestNote (selectPitch);
        float error = (float) targetNote - center;

        const float deadband = std::clamp (settings.humanize, 0.0f, 1.0f) * 0.4f;
        error = std::copysign (std::max (0.0f, std::abs (error) - deadband), error);

        const float desired = error * std::clamp (settings.strength, 0.0f, 1.0f);
        const float coeff = settings.retuneMs < 0.5f ? 1.0f : 1.0f - std::exp (-hopSeconds * 1000.0f / settings.retuneMs);
        correction += (desired - correction) * coeff;

        const float flatten = settings.strength * (1.0f - std::clamp (settings.vibratoPreserve, 0.0f, 1.0f));
        const float shift = std::clamp (correction - flatten * vibrato, -12.0f, 12.0f);

        targetRatio = std::exp2 (shift / 12.0f);
        period = detector.getPeriodInSamples();
        publishMeters (m, targetNote, shift);
    }

    double sampleRate = 44100.0;
    Settings settings;
    int mask = 0xfff;

    static constexpr int dirtyBit = 4;
    std::array<Settings, 3> slots {};
    std::atomic<int> mailbox { 1 };   // the slot in transit, plus the dirty flag
    int writeSlot = 0, readSlot = 2;  // owned by the writer / audio thread respectively
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
    std::array<float, 3> history { { 0, 0, 0 } };
    int medianCount = 0, targetNote = -1;
    bool hasCenter = false;
    float center = 0.0f, correction = 0.0f;
    float ratio = 1.0f, targetRatio = 1.0f, period = 0.0f;

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
        reset();
    }

    void reset() noexcept
    {
        for (auto& channel : delayLines)
            std::fill (channel.begin(), channel.end(), 0.0f);
        writePosition = 0;
        for (int c = 0; c < (int) currentDelay.size(); ++c)
        {
            currentDelay[(size_t)c] = (float) fixedLatency;
            fadeFrom[(size_t)c] = currentDelay[(size_t)c];
            fadeTo[(size_t)c] = currentDelay[(size_t)c];
            fadePosition[(size_t)c] = fadeLength;
        }
    }

    int getFixedLatencySamples() const noexcept { return fixedLatency; }
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
                const float intrinsic = intrinsicLatencySamples != nullptr
                    ? std::clamp (intrinsicLatencySamples[(size_t) c * (size_t) numSamples + (size_t) i],
                                  0.0f, (float) fixedLatency)
                    : 0.0f;
                const float targetDelay = (float) fixedLatency - intrinsic;
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
                    delayLines[(size_t) c][(size_t) writePosition] = channels[c][i];

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
    int fadeLength = 192;
    std::array<int, 2> fadePosition { 192, 192 };
    std::array<float, 2> currentDelay { 0.0f, 0.0f }, fadeFrom { 0.0f, 0.0f }, fadeTo { 0.0f, 0.0f };
};

} // namespace autotune
