#pragma once
#include <vector>

namespace lyra
{
// Low-latency time-domain pitch shifter: variable-speed delay-line read head with
// period-synchronous splicing.
//
// The read head advances `ratio` samples per output sample, so the delay d drifts by (1 - ratio)
// per sample. When d leaves [D - P/2, D + P/2] (D = fixed centre delay, P = detected period) it
// jumps by whole periods back toward D and crossfades old/new heads. A period-aligned jump keeps
// the waveforms in phase, so the splice is inaudible on voiced input. Because the window is centred
// on D for every pitch, the mean delay equals D: that is the latency reported to the host. The
// instantaneous delay deviates by up to +-P/2 (inherent to any time-domain shifter).
//
// D must satisfy D - Pmax/2 >= margin, so D is set by the lowest supported pitch:
//   D = margin + Pmax/2,  margin = 2 taps + fade * (2^(1/12) - 1)  (old head drift during a 1-st fade)
// e.g. 48 kHz: 70 Hz -> 7.5 ms, 100 Hz -> 5.3 ms, 150 Hz -> 3.6 ms.
// Ratios beyond +1 semitone shorten the crossfade to stay within the margin.
// Resampling shifts formants with the pitch: inaudible for small corrections, noticeable past ~3 st.
// Channels share one head (stereo-linked: no image drift).
//
// Threading: prepare() allocates for its minFreq; reset()/process()/setMinFreq() are RT-safe.
class PitchShifter
{
public:
    static constexpr double kMaxRatio = 1.5; // +-7 semitones

    void prepare (double sampleRate, int numChannels, double minFreq);
    void reset() noexcept;
    void setMinFreq (double hz) noexcept; // >= prepared minFreq; moves the centre delay
    int latencySamples() const noexcept { return centre; }
    double currentDelay() const noexcept { return d; } // diagnostics/tests
    static int latencyFor (double sampleRate, double minFreq) noexcept; // pure; usable from any thread

    // Detected input period in samples; <= 0 means unvoiced (a fixed <= 10 ms splice period is used).
    void setPeriod (double samples) noexcept;

    // In-place. ratio[i] is the pitch ratio for sample i (clamped to [1/kMaxRatio, kMaxRatio]).
    void process (float* const* channels, int numChannels, int numSamples, const float* ratio) noexcept;

private:
    float read (int ch, double delay) const noexcept;

    std::vector<std::vector<float>> rings;
    int mask = 0, writePos = 0;
    double sr = 48000, floorFreq = 70, d = 0, dOld = 0, period = 0, unvoicedPeriod = 0, maxPeriod = 0;
    int centre = 0, fadeLen = 1, curFade = 1, fadePos = 0;
    bool fading = false;
};
} // namespace lyra
