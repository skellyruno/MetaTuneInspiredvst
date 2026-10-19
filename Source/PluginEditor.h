#pragma once

#include <JuceHeader.h>
#include <memory>
#include <vector>
#include "PluginProcessor.h"

// Self-contained, resizable JUCE editor. No binary images or missing resource files.
class MetaTuneInspiredAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                                     private juce::Timer
{
public:
    explicit MetaTuneInspiredAudioProcessorEditor(MetaTuneInspiredAudioProcessor&);
    ~MetaTuneInspiredAudioProcessorEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<SliderAttachment> attachment;
    };
    MetaTuneInspiredAudioProcessor& processor;
    std::vector<std::unique_ptr<Knob>> knobs;
    juce::ComboBox keyBox, scaleBox, stabilizerTimeBox;
    std::unique_ptr<ComboAttachment> keyAttachment, scaleAttachment, stabilizerTimeAttachment;
    juce::ToggleButton lowLatencyButton, stabilizerButton, bypassButton, precisionButton;
    std::array<juce::ToggleButton, 12> customDegreeButtons;
    std::array<std::unique_ptr<ButtonAttachment>, 12> customDegreeAttachments;
    juce::Label customDegreesLabel;
    std::unique_ptr<ButtonAttachment> lowLatencyAttachment, stabilizerAttachment, bypassAttachment, precisionAttachment;
    juce::Label pitchReadout, correctionReadout, confidenceReadout, titleLabel, subtitleLabel;
    juce::Label keyLabel, scaleLabel, stabilizerLabel;
    void addKnob(const juce::String& id, const juce::String& caption);
    void timerCallback() override;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MetaTuneInspiredAudioProcessorEditor)
};
