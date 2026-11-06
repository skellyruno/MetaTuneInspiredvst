#pragma once

#include "RealtimeAutotune.h"
#include "FusionPeriodGuard.h"

namespace autotune {

// SkellyTune's unified processing boundary. The existing MetaTune YIN / note
// controller and its 3.5ms Live, 4ms Snap and 8ms Quality renderers remain the
// authoritative signal path. A separate NSDF-style periodicity witness adds
// bounded, conservative note-transition evidence; it never drives a second
// pitch shifter or shifts the host's latency timeline.
//
// All UI parameter IDs and the processor's state format remain unchanged.
class SkellyFusionEngine final {
public:
    using Diagnostics = RealtimeAutotune::Diagnostics;

    void prepare(double sampleRate, float minHz = 70.0f, float maxHz = 1000.0f) {
        core.prepare(sampleRate, minHz, maxHz);
        periodicity.prepare(sampleRate);
    }

    void reset() {
        core.reset();
        periodicity.reset();
    }

    // Called on the audio thread, preserving the host's existing settings.
    // The guard never overrides 0ms retune, disabled note confirmation,
    // precision hard-tune or the selected rendering/latency mode.
    void setSettings(const Settings& requested) {
        Settings effective = requested;
        effective.noteConfirmMs = periodicity.guardedNoteConfirmMs(
            requested.noteConfirmMs, requested.strictRealtime,
            requested.precisionHardTune, requested.retuneMs);
        core.setSettings(effective);
    }

    void processChannels(float* const* channels, int numChannels, int numSamples,
                         float* intrinsicLatencySamples = nullptr) noexcept {
        if (channels == nullptr || numChannels <= 0 || numSamples <= 0)
            return;
        // Capture original samples, *before* in-place correction. The detector's
        // independent evidence is never interpolated into the audio stream.
        const float* input[2] = { channels[0], numChannels > 1 ? channels[1] : nullptr };
        periodicity.push(input, numChannels > 1 ? 2 : 1, numSamples);
        core.processChannels(channels, numChannels, numSamples, intrinsicLatencySamples);
        const auto diagnostic = core.getDiagnostics();
        periodicity.observe(diagnostic.analysisHop, diagnostic.detectorFrequencyHz,
                            diagnostic.confidence, diagnostic.twoPeriodAperiodicity,
                            diagnostic.voiced, numSamples);
    }

    float getDetectedMidi() const noexcept { return core.getDetectedMidi(); }
    int getTargetMidi() const noexcept { return core.getTargetMidi(); }
    float getCorrectionSemitones() const noexcept { return core.getCorrectionSemitones(); }
    float getConfidence() const noexcept { return core.getConfidence(); }
    Diagnostics getDiagnostics() const noexcept { return core.getDiagnostics(); }
    FusionPeriodGuard::Evidence getFusionEvidence() const noexcept {
        return periodicity.lastEvidence();
    }
    float getAmbiguitySeconds() const noexcept { return periodicity.ambiguitySeconds(); }

private:
    RealtimeAutotune core;
    FusionPeriodGuard periodicity;
};

} // namespace autotune
