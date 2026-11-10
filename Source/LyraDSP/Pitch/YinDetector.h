#pragma once
#include "../Common/Biquad.h"
#include <vector>

namespace lyra
{
struct PitchEstimate
{
    float frequency = 0;  // Hz; 0 when unvoiced
    float confidence = 0; // 1 - CMNDF minimum, clamped to [0, 1]
    bool voiced = false;
    float rms = 0;        // band-limited frame RMS (linear)
};

// Streaming YIN (de Cheveigne & Kawahara 2002) pitch detector.
//
// Signal path: band-limit (HP 40 Hz, LP 4th-order Butterworth) -> decimate to ~22-24 kHz ->
// ring buffer -> every hop, CMNDF over the last (window + maxLag) samples.
// Decimation keeps CPU nearly independent of host sample rate (192 kHz costs about the same as 48).
//
// Response lag: the difference function is end-aligned, so an estimate for period tau describes
// audio centred ~(window + tau)/2 detector samples back (window = longest period), plus hop/2
// average staleness and negligible filter group delay. Lower minFreq means a longer window: low
// notes need more observation time; this cannot be avoided, only traded off.
//
// Octave errors: a guard prefers 2*tau when the half-period dip is much shallower than the full one,
// and a continuity hold keeps the previous octave for up to kOctaveHoldSeconds when the estimate jumps
// by ~an octave but the old period is still a valid dip. Holding the lower octave is safe for the
// shifter (a multiple of the true period is still a phase-aligned splice); half a period is not.
//
// Threading: prepare() allocates for the lowest minFreq it is given; process()/reset()/setMinFreq()
// are RT-safe.
class YinDetector
{
public:
    struct Config
    {
        double minFreq = 70.0, maxFreq = 1000.0;
        double hopSeconds = 0.002; // estimate rate; staleness averages hop/2
        float threshold = 0.15f;      // YIN absolute threshold for the first-dip search
        float voicedThreshold = 0.30f; // CMNDF minimum above this -> unvoiced
        float silenceDb = -55.0f;     // frame RMS below this -> unvoiced, no analysis
    };

    void prepare (double hostSampleRate, const Config& cfg);
    void reset() noexcept;
    // Raise the lowest detectable pitch (clamped to the prepared floor). Shorter frame = faster response.
    void setMinFreq (double hz) noexcept;
    static constexpr double kOctaveHoldSeconds = 0.030;

    // Feeds mono samples. onEstimate(int sampleIndexInBlock, const PitchEstimate&) runs once per hop.
    template <typename Callback>
    void process (const float* in, int numSamples, Callback&& onEstimate) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            float x = in[i];
            if (! (x == x) || x > 1e6f || x < -1e6f) // NaN/inf guard: never poison filter state
                x = 0.0f;
            x = lp2.process (lp1.process (hp.process (x)));
            if (++decimPhase < decim)
                continue;
            decimPhase = 0;
            ring[size_t (writePos)] = x;
            writePos = (writePos + 1) % ringLen;
            if (filled < ringLen) ++filled;
            if (++hopCounter >= hop)
            {
                hopCounter = 0;
                if (filled >= frameLen)
                    analyse();
                onEstimate (i, last);
            }
        }
    }

    const PitchEstimate& latest() const noexcept { return last; }
    double detectorSampleRate() const noexcept { return detRate; }
    int frameLengthSamples() const noexcept { return frameLen; } // at detector rate
    int hopSamples() const noexcept { return hop * decim; }      // at host rate
    double windowSeconds() const noexcept { return window / detRate; } // integration window (see Response lag)

private:
    void analyse() noexcept;
    void configureLags() noexcept;

    Config cfg;
    double detRate = 48000;
    int decim = 1, decimPhase = 0;
    double floorFreq = 70.0;
    int window = 0, tauMin = 2, tauMax = 0, frameLen = 0, hop = 1;
    double prevTau = 0; // last accepted voiced period (detector samples), 0 = none
    int octaveRun = 0, hopsSinceVoiced = 1000, octaveHoldHops = 15, maxGapHops = 5;
    int ringLen = 0, writePos = 0, filled = 0, hopCounter = 0;
    float silenceRms = 0;
    Biquad hp, lp1, lp2;
    std::vector<float> ring, frame, cmndf;
    PitchEstimate last;
};
} // namespace lyra
