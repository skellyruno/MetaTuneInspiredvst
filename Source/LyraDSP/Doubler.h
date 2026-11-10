#pragma once
#include "Common/FixedDelay.h"
#include "Pitch/PitchShifter.h"
#include <cstdint>

namespace lyra
{
// Vocal doubler: two auxiliary voices derived from the corrected signal.
//
// Each voice = slowly drifting fractional delay (timing) -> PitchShifter at a slightly detuned,
// slowly wobbling ratio (pitch). Voices differ in base detune, base timing and drift seeds, so they
// behave like separate takes, not a copied waveform (which would only comb-filter / flange).
// Voice timing includes the shifter's own centre delay; doubles are intentionally late, so this
// extra delay is not reported to the host.
//
// Mix (b = blend 0..1): main * (1 - 0.3 b) + each voice * 0.75 b, voices constant-power panned to
// -/+ width. Enable, width and blend are smoothed per sample (20 ms) so changes never click.
//
// Threading: prepare() allocates; everything else is RT-safe.
class Doubler
{
public:
    static constexpr int kMaxBlock = 64;
    static constexpr int kVoices = 2;

    struct Params { bool enabled = false; float width = 0.7f, blend = 0.5f; };

    void prepare (double sampleRate);
    void reset() noexcept;
    void setParams (const Params&) noexcept;
    void setPeriod (double samples) noexcept; // detected input period (<= 0 = unvoiced)

    // In place on the corrected signal. numSamples <= kMaxBlock.
    void process (float* const* channels, int numChannels, int numSamples) noexcept;

    bool isActive() const noexcept { return gain > 1e-4f || params.enabled; }

private:
    struct Voice
    {
        PitchShifter shifter;
        float ring[4096] {}; // timing delay line (max ~85 ms at 48 kHz; capped below)
        int writePos = 0;
        double baseDelay = 0, driftDepth = 0, detuneCents = 0;
        float pan = 0; // -1 left .. +1 right (before width)
        // Smooth random drift: one-pole toward a new random target every ~0.3-0.6 s.
        float driftT = 0, driftP = 0, targetT = 0, targetP = 0;
        int samplesToNext = 0;
        std::uint32_t rng = 1;
        float buf[kMaxBlock] {}, ratio[kMaxBlock] {};
    };

    float nextRandom (Voice&) noexcept; // -1..1
    float readDelay (const Voice&, double delay) const noexcept;

    Voice voices[kVoices];
    Params params;
    double sr = 48000;
    float gain = 0, width = 0.7f, smooth = 0.001f, driftCoef = 0.0001f;
    float mono[kMaxBlock] {};
};
} // namespace lyra
