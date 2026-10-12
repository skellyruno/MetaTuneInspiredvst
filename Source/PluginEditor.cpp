#include "PluginEditor.h"

MetaTuneInspiredAudioProcessorEditor::MetaTuneInspiredAudioProcessorEditor(MetaTuneInspiredAudioProcessor& p)
 : AudioProcessorEditor(&p), processor(p)
{
    setSize(1000, 700);
    styleKnob(speed); styleKnob(amount); styleKnob(sustain);
    speed.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 22);
    amount.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 22);
    sustain.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 22);
    speedLabel.setText("Speed", juce::dontSendNotification);
    amountLabel.setText("Amount", juce::dontSendNotification);
    noteReadout.setJustificationType(juce::Justification::centred);
    noteReadout.setFont(juce::Font(juce::FontOptions(42.0f, juce::Font::bold)));
    status.setJustificationType(juce::Justification::centredLeft);
    juce::Component* components[] = { &speed, &amount, &sustain, &scale, &root, &stabilizer, &lowLatency,
                                      &speedLabel, &amountLabel, &noteReadout, &status };
    for (auto* c : components)
        addAndMakeVisible(c);

    scale.addItem("Chromatic", 1); scale.addItem("Major", 2); scale.addItem("Minor", 3);
    const char* notes[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    for (int i=0;i<12;++i) root.addItem(notes[i], i+1);
    speedAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(processor.parameters,"speed",speed);
    amountAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(processor.parameters,"amount",amount);
    sustainAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(processor.parameters,"sustain",sustain);
    scaleAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(processor.parameters,"scale",scale);
    rootAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(processor.parameters,"root",root);
    stabAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.parameters,"stabilizer",stabilizer);
    latencyAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.parameters,"lowLatency",lowLatency);
    startTimerHz(20);
}
MetaTuneInspiredAudioProcessorEditor::~MetaTuneInspiredAudioProcessorEditor() { stopTimer(); }

void MetaTuneInspiredAudioProcessorEditor::styleKnob(juce::Slider& s)
{
    s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    s.setRange(0.0,100.0,0.1);
    s.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xffc66bff));
    s.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff34313b));
    s.setColour(juce::Slider::thumbColourId, juce::Colour(0xff65d8ff));
    s.setColour(juce::Slider::textBoxTextColourId, juce::Colours::white);
    s.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(0xff17171b));
}
void MetaTuneInspiredAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff111114));
    g.setColour(juce::Colour(0xff1d1d22)); g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(8), 18.0f);
    g.setColour(juce::Colour(0xff29252f)); g.fillRoundedRectangle(juce::Rectangle<float>(18,18,964,48), 10.0f);
    g.setColour(juce::Colours::white); g.setFont(juce::Font(juce::FontOptions(22.0f,juce::Font::bold)));
    g.drawText("metaTune  /  INSPIRED", 35, 20, 400, 42, juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xff08080a)); g.fillEllipse(45,95,500,390);
    g.setColour(juce::Colour(0xff8b5ad9)); g.drawEllipse(110,145,370,290,4.0f);
    g.setColour(juce::Colour(0xff65d8ff)); g.drawEllipse(180,205,230,170,2.0f);
    g.setColour(juce::Colour(0xffd67aff)); g.setFont(juce::Font(juce::FontOptions(34.0f)));
    g.drawText("ϟ", 255,255,100,80,juce::Justification::centred);
    g.setColour(juce::Colour(0xff28282e)); g.fillRoundedRectangle(25,510,950,165,8.0f);
    g.setColour(juce::Colour(0xffb879ff)); g.fillRoundedRectangle(35,516,140,28,14.0f);
    g.setColour(juce::Colours::white); g.setFont(juce::Font(juce::FontOptions(14.0f,juce::Font::bold)));
    g.drawText("NOTE / SCALE",40,516,130,28,juce::Justification::centred);
    const char* keys[]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    for(int i=0;i<12;++i) {
        const int x=35+i*77;
        g.setColour((i%12==4 || i%12==9 || i%12==0) ? juce::Colour(0xff7651ad) : juce::Colour(0xff4b4b53));
        g.fillRect(x,552,75,112);
        g.setColour(juce::Colour(0xff17171b)); g.drawRect(x,552,75,112,1);
        g.setColour(juce::Colours::white); g.drawText(keys[i],x,555,75,25,juce::Justification::centred);
    }
    g.setColour(juce::Colour(0xff888890)); g.setFont(juce::Font(juce::FontOptions(12.0f)));
    g.drawText("PROTOTYPE • NOT AFFILIATED WITH OR ENDORSED BY THE ORIGINAL PLUGIN",25,680,950,14,juce::Justification::centred);
}
void MetaTuneInspiredAudioProcessorEditor::resized()
{
    speed.setBounds(700,110,130,150); amount.setBounds(825,300,145,155); sustain.setBounds(840,125,110,120);
    speedLabel.setBounds(700,250,130,22); amountLabel.setBounds(825,455,145,22);
    scale.setBounds(200,516,130,28); root.setBounds(340,516,75,28);
    stabilizer.setBounds(570,516,150,28); lowLatency.setBounds(720,516,130,28);
    noteReadout.setBounds(175,365,230,75); status.setBounds(35,480,600,22);
}
void MetaTuneInspiredAudioProcessorEditor::timerCallback()
{
    const float hz = processor.getDetectedHz();
    if (hz > 0.0f) {
        const int midi = (int)std::round(processor.getDetectedMidiNote());
        static const char* names[]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
        noteReadout.setText(names[(midi%12+12)%12], juce::dontSendNotification);
        status.setText(juce::String(hz,1)+" Hz   •   correction "+juce::String(processor.getCorrectionCents(),1)+" cents",juce::dontSendNotification);
    } else {
        noteReadout.setText("--",juce::dontSendNotification);
        status.setText("Waiting for pitched input…",juce::dontSendNotification);
    }
}
