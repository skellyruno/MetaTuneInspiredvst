#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

class MetaTuneInspiredAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                                    private juce::Timer
{
public:
    explicit MetaTuneInspiredAudioProcessorEditor(MetaTuneInspiredAudioProcessor&);
    ~MetaTuneInspiredAudioProcessorEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    void timerCallback() override;
    void styleKnob(juce::Slider&);
    void drawKnob(juce::Graphics&, juce::Rectangle<float>, float, juce::String, juce::String, juce::Colour);
    void drawPiano(juce::Graphics&, juce::Rectangle<int>);
    MetaTuneInspiredAudioProcessor& processor;
    juce::Slider speed, amount, sustain;
    juce::ComboBox scale, root;
    juce::ToggleButton stabilizer{"Note Stabilizer"}, lowLatency{"Low Latency"};
    juce::Label speedLabel, amountLabel, noteReadout, status, presetLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> speedAttach, amountAttach, sustainAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> scaleAttach, rootAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> stabAttach, latencyAttach;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MetaTuneInspiredAudioProcessorEditor)
};
