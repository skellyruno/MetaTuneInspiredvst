#include "PluginEditor.h"

namespace {
const juce::Colour bg(0xff101012), panel(0xff242427), panel2(0xff18181b);
const juce::Colour purple(0xffa36bff), pink(0xffdf83c5), cyan(0xff66dfff);
const juce::Colour text(0xffe7e5e9), muted(0xffa7a5aa);
}

MetaTuneInspiredAudioProcessorEditor::MetaTuneInspiredAudioProcessorEditor(MetaTuneInspiredAudioProcessor& p)
 : AudioProcessorEditor(&p), processor(p)
{
    setSize(1366, 1080);
    setResizable(true, true);
    setResizeLimits(1000, 790, 1800, 1400);
    styleKnob(speed); styleKnob(amount); styleKnob(sustain);
    speed.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    amount.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    sustain.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    speedLabel.setText("Speed", juce::dontSendNotification);
    amountLabel.setText("Amount", juce::dontSendNotification);
    noteReadout.setJustificationType(juce::Justification::centred);
    noteReadout.setFont(juce::Font(juce::FontOptions(42.0f, juce::Font::bold)));
    status.setJustificationType(juce::Justification::centredLeft);
    presetLabel.setText("No preset selected", juce::dontSendNotification);
    presetLabel.setJustificationType(juce::Justification::centredLeft);

    juce::Component* components[] = { &speed, &amount, &sustain, &scale, &root, &stabilizer, &lowLatency,
                                      &speedLabel, &amountLabel, &noteReadout, &status, &presetLabel };
    for (auto* c : components) addAndMakeVisible(c);

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
    s.setColour(juce::Slider::rotarySliderFillColourId, purple);
    s.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff37373c));
    s.setColour(juce::Slider::thumbColourId, cyan);
    s.setColour(juce::Slider::textBoxTextColourId, text);
    s.setColour(juce::Slider::textBoxBackgroundColourId, panel2);
    s.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
}

void MetaTuneInspiredAudioProcessorEditor::drawKnob(juce::Graphics& g, juce::Rectangle<float> r,
                                                     float value, juce::String number, juce::String label,
                                                     juce::Colour accent)
{
    auto c = r.getCentre();
    float rad = juce::jmin(r.getWidth(), r.getHeight()) * 0.44f;
    g.setColour(juce::Colour(0xff080809)); g.fillEllipse(r.reduced(2));
    g.setColour(juce::Colour(0xff38383d)); g.drawEllipse(r.reduced(5), 2.0f);
    g.setColour(juce::Colour(0xff1b1b1e)); g.fillEllipse(r.reduced(13));
    const float start = juce::MathConstants<float>::pi * 0.75f;
    const float end = juce::MathConstants<float>::pi * 2.25f;
    juce::Path arc;
    arc.addCentredArc(c.x,c.y,rad,rad,0,start,start+(end-start)*juce::jlimit(0.0f,1.0f,value/100.0f),true);
    g.setColour(juce::Colour(0xff4b4b50)); g.strokePath(arc,juce::PathStrokeType(7.0f));
    juce::Path fill;
    fill.addCentredArc(c.x,c.y,rad,rad,0,start,start+(end-start)*juce::jlimit(0.0f,1.0f,value/100.0f),true);
    g.setColour(accent); g.strokePath(fill,juce::PathStrokeType(5.0f));
    g.setColour(text); g.setFont(juce::Font(juce::FontOptions(r.getWidth()*0.18f,juce::Font::bold)));
    g.drawText(number, r.withY(c.y-17).withHeight(34),juce::Justification::centred);
    g.setColour(muted); g.setFont(juce::Font(juce::FontOptions(15.0f,juce::Font::bold)));
    g.drawText(label,r.withY(c.y+18).withHeight(25),juce::Justification::centred);
}

void MetaTuneInspiredAudioProcessorEditor::drawPiano(juce::Graphics& g, juce::Rectangle<int> area)
{
    const char* names[]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    const bool black[]={false,true,false,true,false,false,true,false,true,false,true,false};
    const int w=area.getWidth()/12;
    for(int i=0;i<12;++i) {
        auto x=area.getX()+i*w;
        g.setColour(juce::Colour(0xff77777b)); g.fillRect(x,area.getY(),w-1,52);
        g.setColour(juce::Colour(0xff202024)); g.drawRect(x,area.getY(),w-1,52,1);
        g.setColour(text); g.setFont(juce::Font(juce::FontOptions(15.0f,juce::Font::bold)));
        g.drawText(names[i],x,area.getY()+3,w-1,45,juce::Justification::centred);
        g.setColour((i%3==0 || i==4 || i==9) ? juce::Colour(0xffa079ff) : juce::Colour(0xff6550a0));
        g.fillRect(x,area.getY()+53,w-1,area.getHeight()-53);
        g.setColour(juce::Colour(0xff29282f)); g.drawRect(x,area.getY()+53,w-1,area.getHeight()-53,1);
    }
    for(int i=0;i<12;++i) if(black[i]) {
        auto x=area.getX()+i*w-w/3;
        g.setColour(juce::Colour(0xff09090a)); g.fillRoundedRectangle((float)x,(float)area.getY()+52,(float)(w*0.64f),(float)(area.getHeight()*0.60f),10.0f);
        g.setColour(juce::Colour(0xff414147)); g.drawRoundedRectangle((float)x,(float)area.getY()+52,(float)(w*0.64f),(float)(area.getHeight()*0.60f),10.0f,1.5f);
    }
}

void MetaTuneInspiredAudioProcessorEditor::paint(juce::Graphics& g)
{
    const float sx=getWidth()/1366.0f, sy=getHeight()/1080.0f;
    g.addTransform(juce::AffineTransform::scale(sx,sy));
    g.fillAll(bg);
    // Top chrome
    g.setColour(juce::Colour(0xff303034)); g.fillRect(0,0,1366,59);
    g.setColour(juce::Colour(0xff151517)); g.fillRect(0,58,1366,2);
    g.setColour(text); g.setFont(juce::Font(juce::FontOptions(24.0f)));
    g.drawText("S",20,7,35,44,juce::Justification::centred);
    g.setColour(juce::Colour(0xff202023)); g.fillRect(68,6,73,48);
    g.setColour(muted); g.drawText("☰",86,9,38,40,juce::Justification::centred);
    g.setColour(juce::Colour(0xff141416)); g.fillRect(143,7,245,47);
    g.setColour(muted); g.setFont(juce::Font(juce::FontOptions(16.0f,juce::Font::italic)));
    g.drawText("No preset selected",162,10,205,39,juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xff29292d)); g.fillRect(390,7,43,47);
    g.fillRect(435,7,72,47);
    g.setColour(muted); g.setFont(juce::Font(juce::FontOptions(19.0f)));
    g.drawText("▣",450,10,40,38,juce::Justification::centred);
    g.setColour(juce::Colour(0xff1b1a20)); g.fillRect(508,6,471,48);
    g.setColour(juce::Colour(0xffd9caff)); g.setFont(juce::Font(juce::FontOptions(27.0f,juce::Font::bold)));
    g.drawText("metaTune",508,6,471,48,juce::Justification::centred);
    g.setColour(muted); g.setFont(juce::Font(juce::FontOptions(23.0f)));
    g.drawText("↶      ↷      ⟳       A  ›  B       ▢",985,7,365,45,juce::Justification::centred);

    // Main surface and dramatic diagonal split
    g.setColour(juce::Colour(0xff09090b)); g.fillRect(0,60,1366,615);
    juce::Path right; right.startNewSubPath(755,60); right.lineTo(1366,60); right.lineTo(1366,675); right.lineTo(1120,675); right.closeSubPath();
    g.setColour(juce::Colour(0xff29292c)); g.fillPath(right);
    g.setColour(juce::Colour(0xff111113)); g.fillEllipse(238,138,465,460);
    g.setColour(juce::Colour(0xff66616e)); g.drawEllipse(241,141,459,454,3.0f);
    g.setColour(juce::Colour(0xff7652a9)); g.drawEllipse(319,215,303,302,8.0f);
    g.setColour(juce::Colour(0xffb9a2ff)); g.drawEllipse(370,270,201,190,5.0f);
    g.setColour(juce::Colour(0xffbb72e8)); g.setFont(juce::Font(juce::FontOptions(54.0f)));
    g.drawText("ϟ",430,322,90,80,juce::Justification::centred);

    // Left group selector rail
    g.setColour(juce::Colour(0xff18181b)); g.fillRoundedRectangle(-15,150,73,430,22.0f);
    g.setColour(purple); g.setFont(juce::Font(juce::FontOptions(15.0f,juce::Font::bold)));
    g.drawText("▦",12,170,34,40,juce::Justification::centred);
    g.setColour(muted); g.setFont(juce::Font(juce::FontOptions(19.0f,juce::Font::bold)));
    for(int i=0;i<4;++i) g.drawText(juce::String(i+1),13,232+i*91,38,42,juce::Justification::centred);
    g.setColour(purple); g.setFont(juce::Font(juce::FontOptions(14.0f,juce::Font::bold)));
    for(int i=0;i<4;++i) g.drawText("0",30,206+i*91,24,18,juce::Justification::centred);

    // Decorative outer orb and control labels
    g.setColour(juce::Colour(0xff5a5a5d)); g.fillEllipse(146,557,40,40);
    g.setColour(juce::Colour(0xff8b8b8e)); g.drawEllipse(146,557,40,40,1.5f);
    drawKnob(g,{930,105,280,275},speed.getValue(),juce::String(speed.getValue(),1),"Speed",pink);
    drawKnob(g,{1150,205,170,170},sustain.getValue(),juce::String(sustain.getValue(),0),"Sustain",purple);
    drawKnob(g,{1080,375,255,245},amount.getValue(),juce::String(amount.getValue(),0),"Amount",pink);

    // Lower selector strip
    g.setColour(juce::Colour(0xff27272b)); g.fillRect(0,676,1366,57);
    g.setColour(muted); g.setFont(juce::Font(juce::FontOptions(15.0f,juce::Font::bold)));
    g.drawText("Select",30,688,95,32,juce::Justification::centredLeft);
    g.setColour(purple); g.fillRoundedRectangle(167,686,114,35,17.0f);
    g.setColour(text); g.drawText("Note",167,686,114,35,juce::Justification::centred);
    g.setColour(juce::Colour(0xff18181b)); g.fillRoundedRectangle(298,686,310,35,17.0f);
    g.setColour(muted); g.drawText("Major     Hold       Minor     Hold",303,686,300,35,juce::Justification::centred);
    g.setColour(juce::Colour(0xff09090a)); g.fillRoundedRectangle(685,684,657,39,19.0f);
    g.setColour(muted); g.drawText("None       Short       Mid       Long       Note Stabilizer      ●  Low Latency",700,686,630,35,juce::Justification::centred);

    // Heatmaps
    g.setColour(juce::Colour(0xff1d1d20)); g.fillRect(0,734,1366,53);
    g.setColour(muted); g.setFont(juce::Font(juce::FontOptions(14.0f,juce::Font::bold)));
    g.drawText("Input",32,736,105,24,juce::Justification::centredLeft);
    g.drawText("Output",32,760,105,24,juce::Justification::centredLeft);
    for(int i=0;i<100;++i) {
        float x=165.0f+i*10.2f;
        auto col=(i%17<4) ? purple : ((i%13<3) ? cyan : juce::Colour(0xff39343f));
        g.setColour(col.withAlpha((i%5==0)?0.95f:0.45f)); g.fillRect(x,739.0f,7.0f,19.0f);
        g.setColour(col.withAlpha((i%7==0)?0.95f:0.30f)); g.fillRect(x+2.0f,762.0f,6.0f,18.0f);
    }

    // Keyboard and right status rail
    drawPiano(g,{165,788,1047,287});
    g.setColour(juce::Colour(0xff222226)); g.fillRect(1212,734,154,341);
    g.setColour(muted); g.setFont(juce::Font(juce::FontOptions(14.0f,juce::Font::bold)));
    g.drawText("◉  In/Out",1225,741,130,24,juce::Justification::centredLeft);
    g.drawText("HeatMaps",1225,761,130,24,juce::Justification::centredLeft);
    g.drawText("◉  Detected",1225,800,130,24,juce::Justification::centredLeft);
    g.drawText("Notes",1225,820,130,24,juce::Justification::centredLeft);
    g.drawText("⏻  Global",1225,880,130,24,juce::Justification::centredLeft);
    g.drawText("Bypass",1225,900,130,24,juce::Justification::centredLeft);
    g.drawText("Pitch Reference",1225,958,130,24,juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xff070708)); g.fillRoundedRectangle(1218,987,140,19,10.0f);
    g.setColour(text); g.drawText("Octs 440.0Hz",1220,1015,136,25,juce::Justification::centred);
    g.setColour(juce::Colour(0xff202024)); g.fillRect(0,1050,1366,30);
    g.setColour(purple); g.drawText("↗ Chromatic",24,1052,140,26,juce::Justification::centredLeft);
}

void MetaTuneInspiredAudioProcessorEditor::resized()
{
    const float sx=getWidth()/1366.0f, sy=getHeight()/1080.0f;
    speed.setBounds(960*sx,125*sy,220*sx,220*sy);
    sustain.setBounds(1168*sx,220*sy,135*sx,135*sy);
    amount.setBounds(1097*sx,400*sy,220*sx,210*sy);
    speedLabel.setBounds(960*sx,320*sy,220*sx,25*sy);
    amountLabel.setBounds(1097*sx,575*sy,220*sx,25*sy);
    scale.setBounds(165*sx,686*sy,125*sx,35*sy);
    root.setBounds(300*sx,686*sy,78*sx,35*sy);
    stabilizer.setBounds(1000*sx,686*sy,160*sx,35*sy);
    lowLatency.setBounds(1200*sx,686*sy,150*sx,35*sy);
    noteReadout.setBounds(360*sx,365*sy,230*sx,70*sy);
    status.setBounds(32*sx,610*sy,600*sx,25*sy);
    presetLabel.setBounds(160*sx,8*sy,220*sx,35*sy);
}
void MetaTuneInspiredAudioProcessorEditor::timerCallback()
{
    const float hz=processor.getDetectedHz();
    if(hz>0.0f) {
        const int midi=(int)std::round(processor.getDetectedMidiNote());
        static const char* names[]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
        noteReadout.setText(names[(midi%12+12)%12],juce::dontSendNotification);
        status.setText(juce::String(hz,1)+" Hz   •   correction "+juce::String(processor.getCorrectionCents(),1)+" cents",juce::dontSendNotification);
    } else {
        noteReadout.setText("--",juce::dontSendNotification);
        status.setText("Waiting for pitched input…",juce::dontSendNotification);
    }
}
