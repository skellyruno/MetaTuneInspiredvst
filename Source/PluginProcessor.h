#pragma once
#include <JuceHeader.h>

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
    float getDetectedMidiNote() const noexcept { return detectedMidiNote.load(); }
    float getDetectedHz() const noexcept { return detectedHz.load(); }
    float getCorrectionCents() const noexcept { return correctionCents.load(); }

    // Shared mapping for the UI readout and the future correction-time engine.
    static float speedControlToMilliseconds(float controlValue) noexcept;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameters();
    void updatePitchEstimate(const juce::AudioBuffer<float>& buffer);
    std::atomic<float> detectedMidiNote { 64.0f }, detectedHz { 0.0f }, correctionCents { 0.0f };
    juce::AudioBuffer<float> monoScratch;
    double currentSampleRate = 44100.0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MetaTuneInspiredAudioProcessor)
};
