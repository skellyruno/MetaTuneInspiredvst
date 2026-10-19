#include "PluginEditor.h"

namespace {
constexpr auto paper = juce::Colour(0xffd8e1e9);
constexpr auto dim = juce::Colour(0xff899da9);
constexpr auto accent = juce::Colour(0xff54d3c7);
constexpr auto panel = juce::Colour(0xff18232b);
}

MetaTuneInspiredAudioProcessorEditor::MetaTuneInspiredAudioProcessorEditor(MetaTuneInspiredAudioProcessor& p)
    : juce::AudioProcessorEditor(&p), processor(p)
{
    setOpaque(true);
    setSize(720, 565);
    setResizable(true, true);
    setResizeLimits(640, 540, 1200, 900);
    titleLabel.setText("SKELL  /  TUNE", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(27.0f, juce::Font::bold)));
    titleLabel.setColour(juce::Label::textColourId, paper);
    addAndMakeVisible(titleLabel);
    subtitleLabel.setText("VOCAL PITCH PROCESSOR  /  EXPERIMENTAL", juce::dontSendNotification);
    subtitleLabel.setColour(juce::Label::textColourId, dim);
    subtitleLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    addAndMakeVisible(subtitleLabel);

    for (const auto& item : {
             std::pair{"speed", "SPEED"}, {"amount", "AMOUNT"},
             {"sustain", "SUSTAIN"}, {"glide", "GLIDE"},
             {"correctionLimit", "MAX SHIFT"}, {"noteConfirm", "NOTE CONFIRM"},
             {"consonantRelease", "CONSONANT"}, {"pitchReference", "REFERENCE A"},
             {"doublerMix", "DOUBLER MIX"}, {"doublerWidth", "DOUBLER WIDTH"} })
        addKnob(item.first, item.second);

    for (int i = 0; i < 12; ++i)
        keyBox.addItem(juce::StringArray{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}[i], i + 1);
    scaleBox.addItemList({"Chromatic", "Major", "Minor", "Harmonic Minor",
                          "Major Pentatonic", "Minor Pentatonic", "Blues", "Dorian", "Custom"}, 1);
    customDegreesLabel.setText("CUSTOM SCALE DEGREES FROM ROOT", juce::dontSendNotification);
    customDegreesLabel.setColour(juce::Label::textColourId, dim);
    customDegreesLabel.setFont(juce::Font(juce::FontOptions(10.0f)));
    addAndMakeVisible(customDegreesLabel);
    static constexpr const char* degreeLabels[] = {"1", "b2", "2", "b3", "3", "4", "#4", "5", "b6", "6", "b7", "7"};
    for (int i = 0; i < 12; ++i)
    {
        auto& button = customDegreeButtons[(size_t)i];
        button.setButtonText(degreeLabels[i]);
        button.setColour(juce::ToggleButton::textColourId, paper);
        button.setColour(juce::ToggleButton::tickColourId, accent);
        addAndMakeVisible(button);
        customDegreeAttachments[(size_t)i] = std::make_unique<ButtonAttachment>(
            p.parameters, "customDegree" + juce::String(i), button);
    }
    stabilizerTimeBox.addItemList({"None", "Short", "Mid", "Long"}, 1);
    for (auto* cb : {&keyBox, &scaleBox, &stabilizerTimeBox})
    {
        cb->setColour(juce::ComboBox::backgroundColourId, panel);
        cb->setColour(juce::ComboBox::textColourId, paper);
        cb->setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff354651));
        addAndMakeVisible(*cb);
    }
    keyAttachment = std::make_unique<ComboAttachment>(p.parameters, "root", keyBox);
    scaleAttachment = std::make_unique<ComboAttachment>(p.parameters, "scale", scaleBox);
    stabilizerTimeAttachment = std::make_unique<ComboAttachment>(p.parameters, "stabilizerTime", stabilizerTimeBox);
    for (auto* label : {&keyLabel, &scaleLabel, &stabilizerLabel})
    {
        label->setColour(juce::Label::textColourId, dim);
        label->setFont(juce::Font(juce::FontOptions(11.0f)));
        addAndMakeVisible(*label);
    }
    keyLabel.setText("ROOT", juce::dontSendNotification);
    scaleLabel.setText("SCALE", juce::dontSendNotification);
    stabilizerLabel.setText("STABILIZER TIME", juce::dontSendNotification);

    for (auto* button : {&lowLatencyButton, &stabilizerButton, &bypassButton, &precisionButton})
    {
        button->setColour(juce::ToggleButton::textColourId, paper);
        button->setColour(juce::ToggleButton::tickColourId, accent);
        addAndMakeVisible(*button);
    }
#if defined(SKELLTUNE_LIVE_FIXED_LOW_LATENCY) && SKELLTUNE_LIVE_FIXED_LOW_LATENCY
    lowLatencyButton.setButtonText("LIVE - FIXED LOW");
    lowLatencyButton.setEnabled(false); // deliberate: smaller host-PDC budget
#else
    lowLatencyButton.setButtonText("LOW LATENCY");
#endif
    stabilizerButton.setButtonText("NOTE STABILIZER");
    bypassButton.setButtonText("BYPASS");
    precisionButton.setButtonText("PRECISION HARD TUNE");
    lowLatencyAttachment = std::make_unique<ButtonAttachment>(p.parameters, "lowLatency", lowLatencyButton);
    stabilizerAttachment = std::make_unique<ButtonAttachment>(p.parameters, "stabilizer", stabilizerButton);
    bypassAttachment = std::make_unique<ButtonAttachment>(p.parameters, "globalBypass", bypassButton);
    precisionAttachment = std::make_unique<ButtonAttachment>(p.parameters, "precisionHardTune", precisionButton);

    for (auto* meter : {&pitchReadout, &correctionReadout, &confidenceReadout})
    {
        meter->setColour(juce::Label::textColourId, accent);
        meter->setFont(juce::Font(juce::FontOptions(17.0f, juce::Font::bold)));
        meter->setJustificationType(juce::Justification::centred);
        addAndMakeVisible(*meter);
    }
    timerCallback();
    startTimerHz(20);
}

MetaTuneInspiredAudioProcessorEditor::~MetaTuneInspiredAudioProcessorEditor() { stopTimer(); }

void MetaTuneInspiredAudioProcessorEditor::addKnob(const juce::String& id, const juce::String& caption)
{
    auto knob = std::make_unique<Knob>();
    knob->slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    knob->slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 94, 20);
    knob->slider.setColour(juce::Slider::rotarySliderFillColourId, accent);
    knob->slider.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff384a55));
    knob->slider.setColour(juce::Slider::thumbColourId, paper);
    knob->slider.setColour(juce::Slider::textBoxTextColourId, paper);
    knob->slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    knob->slider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    knob->label.setText(caption, juce::dontSendNotification);
    knob->label.setJustificationType(juce::Justification::centred);
    knob->label.setColour(juce::Label::textColourId, dim);
    knob->label.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    addAndMakeVisible(knob->slider);
    addAndMakeVisible(knob->label);
    knob->attachment = std::make_unique<SliderAttachment>(processor.parameters, id, knob->slider);
    knobs.push_back(std::move(knob));
}

void MetaTuneInspiredAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff10191f));
    auto bounds = getLocalBounds().toFloat();
    g.setColour(panel);
    g.fillRoundedRectangle(juce::Rectangle<float>(16.0f, 72.0f, bounds.getWidth() - 32.0f, 110.0f), 11.0f);
    g.fillRoundedRectangle(juce::Rectangle<float>(16.0f, 192.0f, bounds.getWidth() - 32.0f, bounds.getHeight() - 208.0f), 11.0f);
    g.setColour(accent);
    g.fillRoundedRectangle(18.0f, 65.0f, 68.0f, 3.0f, 1.5f);
    g.setColour(juce::Colour(0xff30404b));
    g.drawHorizontalLine(184, 16.0f, bounds.getWidth() - 16.0f);
    g.setColour(dim);
    g.setFont(11.0f);
    g.drawText("DETECTED NOTE", 30, 85, 180, 18, juce::Justification::centred);
    g.drawText("CORRECTION", getWidth() / 2 - 90, 85, 180, 18, juce::Justification::centred);
    g.drawText("CONFIDENCE", getWidth() - 210, 85, 180, 18, juce::Justification::centred);
}

void MetaTuneInspiredAudioProcessorEditor::resized()
{
    const int width = getWidth();
    titleLabel.setBounds(24, 17, 300, 31);
    subtitleLabel.setBounds(25, 47, width - 260, 15);
    precisionButton.setBounds(width - 217, 43, 200, 25);
    pitchReadout.setBounds(27, 111, 186, 44);
    correctionReadout.setBounds(width / 2 - 95, 111, 190, 44);
    confidenceReadout.setBounds(width - 213, 111, 186, 44);
    const int contentLeft = 23, contentRight = 23;
    const int usable = width - contentLeft - contentRight;
    const int colWidth = usable / 5;
    const int knobRowsTop = 208;
    const int knobZone = juce::jmax(100, getHeight() - 350);
    for (int i = 0; i < (int)knobs.size(); ++i)
    {
        const int col = i % 5, row = i / 5;
        const int x = contentLeft + col * colWidth;
        const int y = knobRowsTop + row * (knobZone / 2);
        knobs[(size_t)i]->label.setBounds(x, y, colWidth, 18);
        knobs[(size_t)i]->slider.setBounds(x + 2, y + 15, colWidth - 4, juce::jmax(77, knobZone / 2 - 17));
    }
    const int bottom = getHeight() - 100;
    customDegreesLabel.setBounds(27, bottom + 29, 280, 16);
    const int buttonWidth = juce::jmin(48, (getWidth() - 48) / 12);
    const int buttonLeft = (getWidth() - 12 * buttonWidth) / 2;
    for (int i = 0; i < 12; ++i)
        customDegreeButtons[(size_t)i].setBounds(buttonLeft + i * buttonWidth, bottom + 50, buttonWidth, 25);
    keyLabel.setBounds(27, bottom - 26, 75, 18);
    keyBox.setBounds(28, bottom, 75, 25);
    scaleLabel.setBounds(118, bottom - 26, 169, 18);
    scaleBox.setBounds(118, bottom, 169, 25);
    stabilizerLabel.setBounds(296, bottom - 26, 116, 18);
    stabilizerTimeBox.setBounds(296, bottom, 116, 25);
    const int toggleLeft = juce::jmax(420, width - 300);
    lowLatencyButton.setBounds(toggleLeft, bottom - 28, 160, 24);
    stabilizerButton.setBounds(toggleLeft, bottom + 3, 160, 24);
    bypassButton.setBounds(width - 130, bottom - 5, 107, 29);
}

void MetaTuneInspiredAudioProcessorEditor::timerCallback()
{
    const bool customSelected = scaleBox.getSelectedId() == 9;
    customDegreesLabel.setEnabled(customSelected);
    for (auto& button : customDegreeButtons)
        button.setEnabled(customSelected);
    const float midi = processor.getDetectedMidiNote();
    const float hz = processor.getDetectedHz();
    static constexpr const char* noteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    juce::String pitch = "--";
    if (hz > 0.0f && std::isfinite(midi))
    {
        const int note = juce::jlimit(0, 127, (int)std::lround(midi));
        pitch = juce::String(noteNames[note % 12]) + juce::String(note / 12 - 1)
                + "  /  " + juce::String(hz, 1) + " Hz";
    }
    pitchReadout.setText(pitch, juce::dontSendNotification);
    correctionReadout.setText(juce::String(processor.getCorrectionCents(), 1) + " cents", juce::dontSendNotification);
    confidenceReadout.setText(juce::String(processor.getPitchConfidence() * 100.0f, 0) + "%", juce::dontSendNotification);
}
