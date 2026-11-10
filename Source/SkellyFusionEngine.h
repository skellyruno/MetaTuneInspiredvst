#pragma once

// MetaTune / LyraTune adapter.  The real correction and synthesis pipeline is
// LyraTune's detector -> target controller -> pitch-synchronous stereo shifter.
// MetaTune's UI, APVTS parameter IDs, state and host-facing meter API stay intact.
// The previous MetaTune renderers are not mixed or run in parallel.
//
// Original DSP: user-supplied LyraTune ZIP at commit 5f611543a21e3cf9b7ac4bc06ed9f854cde45235.
#include "RealtimeAutotune.h" // Existing settings, scales, host sample sanitiser.
#include "LyraDSP/TunerChain.h"
#include <algorithm>
#include <cmath>

namespace autotune {

class SkellyFusionEngine final {
public:
    using Diagnostics = RealtimeAutotune::Diagnostics;

    void prepare(double sampleRate, float minHz = 70.0f, float /*maxHz*/ = 1000.0f) {
        sr = std::max(8000.0, sampleRate);
        floorHz = std::clamp((double)minHz, 40.0, 500.0);
        engine.prepare(sr, floorHz);
        reset();
    }

    void reset() noexcept {
        engine.reset();
        detected = -1.0f;
        target = -1;
        confidence = 0.0f;
        correction = 0.0f;
    }

    void setSustain(float normalized) noexcept {
        sustain = std::isfinite(normalized) ? std::clamp(normalized, -1.0f, 1.0f) : 0.0f;
    }

    void setSettings(const Settings& s) noexcept {
        lyra::TunerChain::Settings translated;
        // The uploaded engine accepts -3..0 ms and implements its aggressive
        // note-switching effect (instant correction + progressively reduced
        // hysteresis). Never clamp this signed domain to zero again.
        translated.correction.speedMs = s.precisionHardTune
            ? -3.0 : (std::isfinite(s.retuneMs) ? std::clamp((double)s.retuneMs, -3.0, 1000.0) : 20.0);
        translated.correction.amount = std::isfinite(s.strength)
            ? std::clamp((double)s.strength, 0.0, 1.0) : 1.0;
        translated.correction.referenceHz = std::isfinite(s.referenceHz)
            ? std::clamp((double)s.referenceHz, 400.0, 480.0) : 440.0;
        const auto relativeMask = static_cast<lyra::PitchClassMask>(
            scaleMask(s.scale, s.customScale));
        translated.correction.mask = lyra::transposeMask(relativeMask, s.key);
        translated.correction.minConfidence = std::isfinite(s.voicingThreshold)
            ? std::clamp(s.voicingThreshold, 0.55f, 0.92f) : 0.70f;
        // Snap Live now uses the SAME Lyra shifter, but with a more responsive
        // scale boundary (a deliberate controller variant, not a fake DSP mode).
        translated.correction.hysteresisCents = s.snapLive ? 10.0 : 15.0;
        const double confirmation = std::isfinite(s.noteConfirmMs)
            ? std::clamp((double)s.noteConfirmMs, 0.0, 100.0) : 10.0;
        const double stabilizer = std::isfinite(s.noteStabilizerMs)
            ? std::clamp((double)s.noteStabilizerMs * 0.13, 0.0, 110.0) : 0.0;
        translated.correction.stabilizerMs =
            translated.correction.speedMs < 0.0 || s.precisionHardTune
                ? 0.0 : std::min(150.0, confirmation + stabilizer);
        translated.correction.sustain = sustain;

        // Fixed 70Hz prepared range: avoids allocations and fluctuating host
        // PDC. With Low Latency off, Lyra's *own* 2ms detector lookahead is on.
        // Snap Live shares the low-latency synthesis path.
        translated.minFreq = floorHz;
        translated.hq = !s.lowLatency;
        translated.bypass = s.bypass || translated.correction.amount <= 1.0e-6;
        // Preserve MetaTune's existing doubler as the ONLY doubler; avoid
        // running two independent double/chorus processors by accident.
        translated.doubler.enabled = false;
        translated.strike = false; // LyraStrike requires an explicit UI decision.
        engine.setSettings(translated);
    }

    // In-place, with JUCE's existing slice boundaries. Source audio is
    // sanitised both by the processor and at this integration boundary.
    void processChannels(float* const* channels, int numChannels, int numSamples,
                         float* intrinsicLatencySamples = nullptr) noexcept {
        if (channels == nullptr || numChannels < 1 || numSamples <= 0) return;
        const int used = std::min(numChannels, 2);
        for (int ch = 0; ch < used; ++ch) {
            if (channels[ch] == nullptr) return;
            for (int i = 0; i < numSamples; ++i)
                channels[ch][i] = boundedAudioSample(channels[ch][i]);
        }
        engine.process(channels, used, numSamples);
        const auto& c = engine.correction();
        const auto& p = engine.detector().latest();
        detected = c.inputMidi() > 0 ? (float)c.inputMidi() : -1.0f;
        target = c.targetNote();
        correction = (float)c.correctionSemitones();
        confidence = std::isfinite(p.confidence) ? std::clamp(p.confidence, 0.0f, 1.0f) : 0.0f;
        if (intrinsicLatencySamples != nullptr) {
            const auto count = (std::size_t)numSamples * (std::size_t)used;
            std::fill(intrinsicLatencySamples, intrinsicLatencySamples + count,
                      (float)engine.latencySamples());
        }
    }

    float getDetectedMidi() const noexcept { return detected; }
    int getTargetMidi() const noexcept { return target; }
    float getCorrectionSemitones() const noexcept { return correction; }
    float getConfidence() const noexcept { return confidence; }
    int latencySamples() const noexcept { return engine.latencySamples(); }

    Diagnostics getDiagnostics() const noexcept {
        Diagnostics d {};
        d.detectedMidi = detected;
        d.confidence = confidence;
        d.correctionSemitones = correction;
        d.targetMidi = target;
        d.voiced = detected >= 0;
        d.detectorFrequencyHz = engine.detector().latest().frequency;
        d.estimatedPeriodSamples = d.detectorFrequencyHz > 0
            ? (float)(sr / (double)d.detectorFrequencyHz) : 0.0f;
        return d;
    }

private:
    lyra::TunerChain engine;
    double sr = 48000.0, floorHz = 70.0;
    float sustain = 0.0f;
    float detected = -1.0f, confidence = 0.0f, correction = 0.0f;
    int target = -1;
};

} // namespace autotune
