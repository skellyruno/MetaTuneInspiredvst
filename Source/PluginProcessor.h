#pragma once
#include <JuceHeader.h>
#include <array>
#include <cstdint>

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

    // Shared mapping for the UI readout and future correction-time engine.
    static float speedControlToMilliseconds(float controlValue) noexcept;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameters();
    void updatePitchEstimate(const juce::AudioBuffer<float>& buffer);
    void estimatePitchFromHistory() noexcept;

    static constexpr int pitchHistorySize = 8192;
    int shortAnalysisDecimation = 1;
    int shortAnalysisWindowSamples = 1024;
    int longAnalysisDecimation = 2;
    int longAnalysisWindowSamples = 2048;
    int pitchAnalysisHopSamples = 256;
    // Cache APVTS atomics once, avoiding repeated parameter lookup on the audio thread.
    std::atomic<float>* speedParameter = nullptr;
    std::atomic<float>* amountParameter = nullptr;
    std::atomic<float>* sustainParameter = nullptr;
    std::atomic<float>* scaleParameter = nullptr;
    std::atomic<float>* rootParameter = nullptr;
    std::atomic<float>* lowLatencyParameter = nullptr;
    std::atomic<float>* stabilizerParameter = nullptr;
    std::atomic<float>* stabilizerTimeParameter = nullptr;
    std::atomic<float>* pitchReferenceParameter = nullptr;

    std::array<float, pitchHistorySize> pitchHistory {};
    int pitchHistoryWritePosition = 0;
    int pitchHistorySamples = 0;
    int samplesSincePitchAnalysis = 0;
    int consecutiveUnvoicedAnalyses = 0;

    std::atomic<float> detectedMidiNote { 64.0f }, detectedHz { 0.0f }, correctionCents { 0.0f };
    std::atomic<float> pitchConfidence { 0.0f }, inputRms { 0.0f }, targetMidiNote { 69.0f };
    std::atomic<float> smoothedTargetMidiNote { 69.0f }, effectiveRetuneTimeMs { 30.0f };
    float heldPitchAnchorMidi = 69.0f;
    std::int64_t heldPitchSamples = 0;
    float audioSmoothedTargetMidi = 69.0f;
    bool hasHeldPitchAnchor = false;
    bool hasSmoothedTarget = false;
    int committedScaleNote = 69;
    int pendingScaleNote = 69;
    int pendingScaleSamples = 0;
    bool hasCommittedScaleNote = false;
    bool hasPendingScaleNote = false;
    double currentSampleRate = 44100.0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MetaTuneInspiredAudioProcessor)
};
