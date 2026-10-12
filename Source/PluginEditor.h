#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <memory>

class MetaTuneInspiredAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                                    private juce::Timer
{
public:
    explicit MetaTuneInspiredAudioProcessorEditor(MetaTuneInspiredAudioProcessor&);
    ~MetaTuneInspiredAudioProcessorEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    void styleKnob(juce::Slider&);
    void drawKnob(juce::Graphics&, juce::Rectangle<float>, float, juce::String, juce::String, juce::Colour);
    void drawPiano(juce::Graphics&, juce::Rectangle<int>);
    void setRootNote(int noteIndex);
    void setScaleMode(int modeIndex);
    void openHeaderMenu();
    void savePresetToFile();
    void loadPresetFromFile();

    MetaTuneInspiredAudioProcessor& processor;
    juce::Slider speed, amount, sustain;
    juce::ComboBox scale, root;
    juce::ToggleButton stabilizer { "Note Stabilizer" }, lowLatency { "Low Latency" };
    juce::Label speedLabel, amountLabel, noteReadout, status, presetLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> speedAttach, amountAttach, sustainAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> scaleAttach, rootAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> stabAttach, latencyAttach;

    int selectedNoteIndex = 4;
    int activeNoteGroup = 0;
    int selectedCorrectionTime = 1; // None, Short, Mid, Long
    bool majorHold = false;
    bool minorHold = false;
    bool globalBypass = false;
    bool showHeatMaps = true;
    bool showDetectedNotes = true;
    bool activeB = false;
    float toneVolume = 0.65f;
    float pitchReferenceHz = 440.0f;

    juce::ValueTree slotA, slotB, undoState, redoState;
    juce::String selectedPresetName { "No preset selected" };
    bool hasUndoState = false;
    bool hasRedoState = false;
    std::shared_ptr<juce::FileChooser> saveChooser, loadChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MetaTuneInspiredAudioProcessorEditor)
};
