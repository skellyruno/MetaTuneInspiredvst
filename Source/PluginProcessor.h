#pragma once
#include <JuceHeader.h>
#include <array>
#include <cstdint>
#include <vector>
#include "RealtimeAutotune.h"

class MetaTuneInspiredAudioProcessor final : public juce::AudioProcessor
{
public:
    MetaTuneInspiredAudioProcessor();
    ~MetaTuneInspiredAudioProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.1; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState parameters;
    float getDetectedMidiNote() const noexcept { return detectedMidiNote.load(std::memory_order_relaxed); }
    float getDetectedHz() const noexcept { return detectedHz.load(std::memory_order_relaxed); }
    float getCorrectionCents() const noexcept { return correctionCents.load(std::memory_order_relaxed); }
    float getPitchConfidence() const noexcept { return pitchConfidence.load(std::memory_order_relaxed); }
    float getInputRms() const noexcept { return inputRms.load(std::memory_order_relaxed); }
    float getTargetMidiNote() const noexcept { return targetMidiNote.load(std::memory_order_relaxed); }
    float getSmoothedTargetMidiNote() const noexcept { return smoothedTargetMidiNote.load(std::memory_order_relaxed); }
    float getEffectiveRetuneTimeMs() const noexcept { return effectiveRetuneTimeMs.load(std::memory_order_relaxed); }
    float getPitchReferenceHz() const noexcept
    {
        if (pitchReferenceParameter != nullptr)
            return pitchReferenceParameter->load(std::memory_order_relaxed);
        return 440.0f;
    }

    // Shared mapping for the UI readout and the active correction-time engine.
    static float speedControlToMilliseconds(float controlValue) noexcept;
    void startTonePreview(int midiNote) noexcept { tonePreviewMidiNote.store(juce::jlimit(0, 127, midiNote), std::memory_order_relaxed); }
    void stopTonePreview() noexcept { tonePreviewMidiNote.store(-1, std::memory_order_relaxed); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameters();
    void processDoubler(juce::AudioBuffer<float>& buffer, int startSample, int samples) noexcept;
    void processReferenceTone(juce::AudioBuffer<float>& buffer, int startSample, int samples) noexcept;

    static constexpr int doublerDelayLineSize = 8192; // fixed delay-line capacity used by the Doubler
    // Cache APVTS atomics once, avoiding repeated parameter lookup on the audio thread.
    // Pulse-aligned TD-PSOLA engine supplied in RealtimeAutotune.h.
    autotune::RealtimeAutotune realtimeAutotune;
    std::atomic<float>* speedParameter = nullptr;
    std::atomic<float>* amountParameter = nullptr;
    std::atomic<float>* sustainParameter = nullptr;
    std::atomic<float>* glideParameter = nullptr;
    std::atomic<float>* correctionLimitParameter = nullptr;
    std::atomic<float>* precisionHardTuneParameter = nullptr;
    std::atomic<float>* noteConfirmParameter = nullptr;
    std::atomic<float>* consonantReleaseParameter = nullptr;
    std::atomic<float>* scaleParameter = nullptr;
    std::array<std::atomic<float>*, 12> customDegreeParameters {};
    std::atomic<float>* rootParameter = nullptr;
    std::atomic<float>* lowLatencyParameter = nullptr;
    std::atomic<float>* stabilizerParameter = nullptr;
    std::atomic<float>* stabilizerTimeParameter = nullptr;
    std::atomic<float>* pitchReferenceParameter = nullptr;
    std::atomic<float>* doublerWidthParameter = nullptr;
    std::atomic<float>* doublerMixParameter = nullptr;
    std::atomic<float>* globalBypassParameter = nullptr;
    std::atomic<float>* toneVolumeParameter = nullptr;
    std::atomic<int> tonePreviewMidiNote { -1 };

    // All delay storage and per-sample latency telemetry are allocated before the
    // audio callback; processBlock performs no heap allocation.
    autotune::OutputLatencyCompensator outputLatencyCompensator;
    autotune::FixedDirectDelay referenceToneDelay; // fixed host-PDC path for plugin-generated piano notes
    autotune::LatencyAlignedBypass latencyAlignedBypass; // PDC-aligned dry/bypass branch
    std::vector<float> dryLatencyScratch; // channel-major; allocated in prepareToPlay
    std::vector<float> intrinsicLatencyScratch; // channel-major: two blocks of per-sample delay values
    int fixedLatencySamples = 0;
    std::array<std::array<float, doublerDelayLineSize>, 2> doublerDelayLine {};
    int doublerWritePosition = 0;
    float doublerLfoPhase = 0.0f;
    float smoothedDoublerWidth = 0.46f;
    float smoothedDoublerMix = 0.0f;
    float referenceTonePhase = 0.0f;
    float referenceToneAmplitude = 0.0f;
    float referenceToneFrequencyHz = 440.0f;
    std::atomic<float> detectedMidiNote { 64.0f }, detectedHz { 0.0f }, correctionCents { 0.0f };
    std::atomic<float> pitchConfidence { 0.0f }, inputRms { 0.0f }, targetMidiNote { 69.0f };
    std::atomic<float> smoothedTargetMidiNote { 69.0f }, effectiveRetuneTimeMs { 30.0f };
    double currentSampleRate = 44100.0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MetaTuneInspiredAudioProcessor)
};
