#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <memory>
#include <array>

class MetaTuneInspiredAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                                    private juce::Timer
{
public:
    explicit MetaTuneInspiredAudioProcessorEditor(MetaTuneInspiredAudioProcessor&);
    ~MetaTuneInspiredAudioProcessorEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

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
    int lastNonChromaticScaleMode = 1;
    int orbDragMode = 0; // 0 none, 1 Speed from inner orb, 2 Amount from outer shell
    float orbDragStartY = 0.0f;
    float orbDragStartX = 0.0f;
    float orbDragStartValue = 0.0f;
    float doublerWidth = 0.46f;
    float doublerMix = 0.49f;
    float doublerDragStartWidth = 0.46f;
    float doublerDragStartMix = 0.49f;
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
    float orbRotationRadians = 0.0f;

    // Bracket custom-painted macro drags as proper host automation gestures.
    // Slider-child drags are handled by JUCE's SliderAttachment callbacks.
    juce::AudioProcessorParameter* activeGestureParamA = nullptr;
    juce::AudioProcessorParameter* activeGestureParamB = nullptr;

    // Orb motion model: organic core breathing, three gyro rings and a bounded
    // equatorial particle emitter. All state is advanced on the editor timer.
    struct OrbParticle
    {
        float age = 0.0f;
        float lifetime = 1.4f;
        float angle = 0.0f;
        float radialSpeed = 54.0f;
        float size = 1.0f;
        float twist = 0.0f;
        float seed = 0.0f;
        int tint = 0;
    };
    std::array<OrbParticle, 72> orbParticles {};
    int orbParticleWriteIndex = 0;
    juce::uint32 orbLastTickMs = 0;
    juce::uint32 orbParticleRandomState = 0x9e3779b9u;
    float orbElapsedSeconds = 0.0f;
    float orbBreathPhase = 0.0f;
    float orbBreathAmount = 0.5f;
    float orbAudioDrive = 0.0f;
    float orbRingPhaseA = 0.0f;
    float orbRingPhaseB = 2.1f;
    float orbRingPhaseC = 4.2f;
    float orbParticleSpawnAccumulator = 0.0f;

    void updateOrbMotion(float deltaSeconds);
    float nextOrbRandomUnit() noexcept;

    // UI-thread history for the rolling input/output note HeatMaps.
    static constexpr int heatmapColumnCount = 100;
    std::array<int, heatmapColumnCount> heatmapInputNotes {};
    std::array<int, heatmapColumnCount> heatmapOutputNotes {};
    std::array<float, heatmapColumnCount> heatmapConfidence {};
    int heatmapWriteIndex = 0;
    float visualizerActivity = 0.0f;

    // One-shot Orb boot animation: fast spin, rising light, then settle into
    // continuous Speed-driven motion. Timestamp uses JUCE's wrap-safe ms counter.
    juce::uint32 orbStartupStartedAtMs = 0;
    float orbStartupProgress = 0.0f;
    float orbStartupGlow = 0.0f;
    bool orbStartupComplete = false;

    juce::ValueTree slotA, slotB, undoState, redoState;
    juce::String selectedPresetName { "No preset selected" };
    bool hasUndoState = false;
    bool hasRedoState = false;
    std::shared_ptr<juce::FileChooser> saveChooser, loadChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MetaTuneInspiredAudioProcessorEditor)
};
