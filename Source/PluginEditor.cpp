#include "PluginEditor.h"

namespace {
const juce::Colour bg(0xff101012), panel(0xff242427), panel2(0xff18181b);
const juce::Colour purple(0xffa36bff), pink(0xffdf83c5), cyan(0xff66dfff);
const juce::Colour text(0xffe7e5e9), muted(0xffa7a5aa);

class MetaTuneKnobLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float startAngle, float endAngle,
                          juce::Slider& slider) override
    {
        const float side = (float) juce::jmin(width, height);
        const float cx = x + width * 0.5f;
        const float cy = y + height * 0.5f;
        const float radius = side * 0.43f;
        const float ringRadius = radius * 0.88f;
        const float start = startAngle;
        const float end = endAngle;
        const auto accent = slider.getName() == "Amount" ? pink : purple;

        g.setColour(juce::Colour(0xff08080a));
        g.fillEllipse(cx - radius - 7.0f, cy - radius - 7.0f,
                      (radius + 7.0f) * 2.0f, (radius + 7.0f) * 2.0f);
        g.setColour(juce::Colour(0xff3a393f));
        g.drawEllipse(cx - radius - 5.0f, cy - radius - 5.0f,
                      (radius + 5.0f) * 2.0f, (radius + 5.0f) * 2.0f, 2.0f);
        g.setColour(juce::Colour(0xff19191d));
        g.fillEllipse(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

        // Fine segmented scale, like the hardware-style MetaTune dials.
        const int ticks = 71;
        for (int i = 0; i < ticks; ++i)
        {
            const float t = (float) i / (float) (ticks - 1);
            const float a = start + (end - start) * t;
            const float inner = ringRadius - (i % 5 == 0 ? 7.0f : 3.5f);
            const float outer = ringRadius + 1.0f;
            juce::Line<float> tick(cx + std::cos(a) * inner, cy + std::sin(a) * inner,
                                   cx + std::cos(a) * outer, cy + std::sin(a) * outer);
            g.setColour(t <= sliderPos ? accent.withAlpha(0.96f) : juce::Colour(0xff4a494f));
            g.drawLine(tick, i % 5 == 0 ? 1.7f : 1.0f);
        }

        juce::Path progress;
        progress.addCentredArc(cx, cy, ringRadius - 4.0f, ringRadius - 4.0f, 0.0f,
                               start, start + (end - start) * sliderPos, true);
        g.setColour(accent.withAlpha(0.95f));
        g.strokePath(progress, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
        const float thumbAngle = start + (end - start) * sliderPos;
        g.setColour(cyan);
        g.fillEllipse(cx + std::cos(thumbAngle) * (ringRadius - 4.0f) - 5.0f,
                      cy + std::sin(thumbAngle) * (ringRadius - 4.0f) - 5.0f, 10.0f, 10.0f);

        juce::String value;
        if (slider.getName() == "Speed") value = juce::String(slider.getValue(), 1);
        else value = juce::String(slider.getValue(), 0);
        g.setColour(text);
        g.setFont(juce::Font(juce::FontOptions(side * 0.16f, juce::Font::bold)));
        g.drawText(value, juce::Rectangle<float>(cx - radius * 0.72f, cy - 14.0f,
                                                  radius * 1.44f, 31.0f), juce::Justification::centred);
        g.setColour(muted);
        g.setFont(juce::Font(juce::FontOptions(juce::jlimit(10.0f, 14.0f, side * 0.055f),
                                               juce::Font::bold)));
        g.drawText(slider.getName(), juce::Rectangle<float>(cx - radius * 0.72f, cy + 14.0f,
                                                             radius * 1.44f, 23.0f), juce::Justification::centred);
    }
};

MetaTuneKnobLookAndFeel metaTuneKnobLookAndFeel;
}

MetaTuneInspiredAudioProcessorEditor::MetaTuneInspiredAudioProcessorEditor(MetaTuneInspiredAudioProcessor& p)
 : AudioProcessorEditor(&p), processor(p)
{
    // Default to the requested compact size while keeping the editor resizable.
    // The drawing canvas remains 1366x1080 and is scaled in paint()/resized().
    setSize(683, 540);
    setResizable(true, true);
    setResizeLimits(683, 540, 1708, 1350);
    styleKnob(speed); styleKnob(amount); styleKnob(sustain);
    speed.setName("Speed"); amount.setName("Amount"); sustain.setName("Sustain");
    speed.setLookAndFeel(&metaTuneKnobLookAndFeel);
    amount.setLookAndFeel(&metaTuneKnobLookAndFeel);
    sustain.setLookAndFeel(&metaTuneKnobLookAndFeel);
    speed.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    amount.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    sustain.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    speedLabel.setText("Speed", juce::dontSendNotification);
    amountLabel.setText("Amount", juce::dontSendNotification);
    noteReadout.setJustificationType(juce::Justification::centred);
    noteReadout.setFont(juce::Font(juce::FontOptions(38.0f, juce::Font::bold)));
    status.setJustificationType(juce::Justification::centredLeft);
    presetLabel.setText("No preset selected", juce::dontSendNotification);
    presetLabel.setJustificationType(juce::Justification::centredLeft);

    juce::Component* components[] = { &speed, &amount, &sustain, &scale, &root, &stabilizer, &lowLatency,
                                      &noteReadout, &status, &presetLabel };
    for (auto* c : components) addAndMakeVisible(c);

    scale.addItem("Chromatic", 1); scale.addItem("Major", 2); scale.addItem("Minor", 3);
    for (auto* combo : { &scale, &root })
    {
        combo->setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff242429));
        combo->setColour(juce::ComboBox::textColourId, text);
        combo->setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff77777e));
        combo->setColour(juce::ComboBox::arrowColourId, text);
    }
    for (auto* toggle : { &stabilizer, &lowLatency })
    {
        toggle->setColour(juce::ToggleButton::textColourId, text);
        toggle->setColour(juce::ToggleButton::tickColourId, cyan);
        toggle->setColour(juce::ToggleButton::tickDisabledColourId, muted);
        toggle->setLookAndFeel(&metaTuneKnobLookAndFeel);
    }
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
MetaTuneInspiredAudioProcessorEditor::~MetaTuneInspiredAudioProcessorEditor()
{
    stopTimer();
    speed.setLookAndFeel(nullptr);
    amount.setLookAndFeel(nullptr);
    sustain.setLookAndFeel(nullptr);
    stabilizer.setLookAndFeel(nullptr);
    lowLatency.setLookAndFeel(nullptr);
}

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
    // Chromatic note strip above the actual seven-white-key octave. Keeping the
    // label strip chromatic mirrors MetaTune while the keyboard uses real piano geometry.
    const char* names[] = { "C", "C #", "D", "D #", "E", "F", "F #", "G", "G #", "A", "A #", "B" };
    const int labelH = 52;
    const int labelW = area.getWidth() / 12;
    for (int i = 0; i < 12; ++i)
    {
        const int x = area.getX() + i * labelW;
        g.setColour((i % 2 == 0) ? juce::Colour(0xff77777b) : juce::Colour(0xff68686d));
        g.fillRect(x, area.getY(), labelW - 1, labelH);
        g.setColour(juce::Colour(0xff202024));
        g.drawRect(x, area.getY(), labelW - 1, labelH, 1);
        g.setColour(text);
        g.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
        g.drawText(names[i], x, area.getY() + 2, labelW - 1, labelH - 4, juce::Justification::centred);
    }

    const int keyY = area.getY() + labelH;
    const int keyH = area.getHeight() - labelH;
    const int whiteCount = 7;
    const float whiteW = (float) area.getWidth() / (float) whiteCount;
    const juce::Colour topWhite(0xffa078ff), bottomWhite(0xff8d70d4);
    // Seven full-width white keys with subtle vertical lighting and clear gaps.
    for (int i = 0; i < whiteCount; ++i)
    {
        const float x = area.getX() + i * whiteW;
        juce::ColourGradient gradient(topWhite, x, (float)keyY,
                                      bottomWhite, x, (float)(keyY + keyH), false);
        gradient.addColour(0.48, juce::Colour(0xff9b7bf0));
        g.setGradientFill(gradient);
        g.fillRect(x + 0.5f, (float)keyY, whiteW - 1.0f, (float)keyH);
        g.setColour(juce::Colour(0xff29252f));
        g.drawRect(x, (float)keyY, whiteW, (float)keyH, 1.25f);
        g.setColour(juce::Colour(0xffd7c7ff).withAlpha(0.20f));
        g.drawLine(x + 2.0f, (float)keyY + 2.0f, x + whiteW - 2.0f, (float)keyY + 2.0f, 1.0f);
    }

    // Black keys sit between the natural notes, with rounded lower corners and
    // a dark top-to-bottom gradient. There is intentionally no key after E or B.
    const int blackAfterWhite[] = { 0, 1, 3, 4, 5 };
    const float blackW = whiteW * 0.54f;
    const float blackH = keyH * 0.60f;
    for (int i = 0; i < 5; ++i)
    {
        const float x = area.getX() + (blackAfterWhite[i] + 1) * whiteW - blackW * 0.5f;
        const float y = (float)keyY;
        juce::Path key;
        key.startNewSubPath(x, y);
        key.lineTo(x + blackW, y);
        key.lineTo(x + blackW, y + blackH - 12.0f);
        key.quadraticTo(x + blackW, y + blackH, x + blackW - 12.0f, y + blackH);
        key.lineTo(x + 12.0f, y + blackH);
        key.quadraticTo(x, y + blackH, x, y + blackH - 12.0f);
        key.closeSubPath();
        juce::ColourGradient blackGradient(juce::Colour(0xff303034), x, y,
                                           juce::Colour(0xff111113), x, y + blackH, false);
        blackGradient.addColour(0.55, juce::Colour(0xff202023));
        g.setGradientFill(blackGradient);
        g.fillPath(key);
        g.setColour(juce::Colour(0xff050506));
        g.strokePath(key, juce::PathStrokeType(2.0f));
        g.setColour(juce::Colour(0xff77717f).withAlpha(0.30f));
        g.drawLine(x + 2.0f, y + 2.0f, x + blackW - 2.0f, y + 2.0f, 1.0f);
    }
}

void MetaTuneInspiredAudioProcessorEditor::paint(juce::Graphics& g)
{
    const float sx=getWidth()/1366.0f, sy=getHeight()/1080.0f;
    g.addTransform(juce::AffineTransform::scale(sx,sy));
    g.fillAll(bg);

    // Very subtle material grain across the two large surfaces. Deterministic
    // points avoid flicker while giving the flat panels a more finished look.
    for (int i = 0; i < 1050; ++i)
    {
        const int px = (i * 97 + (i % 11) * 31) % 1366;
        const int py = 60 + ((i * 53 + (i % 17) * 19) % 615);
        const bool onDark = px < 755 || py > 460;
        g.setColour((onDark ? juce::Colour(0xff77727f) : juce::Colour(0xffa19ba8))
                        .withAlpha((i % 7 == 0) ? 0.035f : 0.014f));
        g.fillRect(px, py, (i % 3 == 0) ? 2 : 1, 1);
    }

    // Top chrome
    g.setColour(juce::Colour(0xff303034)); g.fillRect(0,0,1366,59);
    g.setColour(juce::Colour(0xff151517)); g.fillRect(0,58,1366,2);
    g.setColour(text); g.setFont(juce::Font(juce::FontOptions(24.0f)));
    g.drawText("S",20,7,35,44,juce::Justification::centred);
    g.setColour(juce::Colour(0xff202023)); g.fillRect(68,6,73,48);
    g.setColour(muted); g.setFont(juce::Font(juce::FontOptions(12.0f,juce::Font::bold))); g.drawText("MENU",76,9,57,40,juce::Justification::centred);
    g.setColour(juce::Colour(0xff141416)); g.fillRect(143,7,245,47);
    g.setColour(muted); g.setFont(juce::Font(juce::FontOptions(16.0f,juce::Font::italic)));
    g.drawText("No preset selected",162,10,205,39,juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xff29292d)); g.fillRect(390,7,43,47);
    g.fillRect(435,7,72,47);
    g.setColour(muted); g.setFont(juce::Font(juce::FontOptions(19.0f)));
    g.drawText("SAVE",450,10,40,38,juce::Justification::centred);
    g.setColour(juce::Colour(0xff1b1a20)); g.fillRect(508,6,471,48);
    g.setColour(juce::Colour(0xffa36bff).withAlpha(0.22f));
    g.fillRect(508,6,471,2);
    g.setColour(juce::Colour(0xffd9caff)); g.setFont(juce::Font(juce::FontOptions(27.0f,juce::Font::bold)));
    g.drawText("metaTune",508,6,471,48,juce::Justification::centred);
    g.setColour(juce::Colour(0xffa36bff).withAlpha(0.10f));
    g.drawLine(535.0f, 49.0f, 952.0f, 49.0f, 1.0f);
    g.setColour(muted); g.setFont(juce::Font(juce::FontOptions(17.0f)));
    g.drawText("UNDO",986,7,45,45,juce::Justification::centred);
    g.drawText("REDO",1035,7,45,45,juce::Justification::centred);
    g.drawText("SYNC",1081,7,45,45,juce::Justification::centred);
    g.setFont(juce::Font(juce::FontOptions(19.0f,juce::Font::bold)));
    g.drawText("A  >  B",1130,7,115,45,juce::Justification::centred);
    g.setFont(juce::Font(juce::FontOptions(15.0f)));
    g.drawText("HELP",1260,7,86,45,juce::Justification::centred);

    // Main surface and dramatic diagonal split
    g.setColour(juce::Colour(0xff09090b)); g.fillRect(0,60,1366,615);
    juce::Path right; right.startNewSubPath(755,60); right.lineTo(1366,60); right.lineTo(1366,675); right.lineTo(1120,675); right.closeSubPath();
    g.setColour(juce::Colour(0xff29292c)); g.fillPath(right);
    g.setColour(juce::Colour(0xff111113)); g.fillEllipse(238,138,465,460);
    // Multi-layer ambient halo, kept subtle so it scales cleanly at small sizes.
    for (int i = 12; i >= 1; --i)
    {
        const float inset = (float)i * 2.8f;
        g.setColour(juce::Colour(0xffa56bff).withAlpha(0.012f * (13 - i)));
        g.drawEllipse(319.0f - inset, 215.0f - inset,
                      303.0f + inset * 2.0f, 302.0f + inset * 2.0f, 2.2f);
    }
    // Faint inner haze and split cyan/magenta energy around the note core.
    g.setColour(juce::Colour(0xff33223f).withAlpha(0.42f));
    g.fillEllipse(392.0f, 291.0f, 157.0f, 151.0f);
    g.setColour(juce::Colour(0xff513c76).withAlpha(0.32f));
    g.fillEllipse(408.0f, 307.0f, 125.0f, 120.0f);
    g.setColour(juce::Colour(0xff66616e)); g.drawEllipse(241,141,459,454,3.0f);

    // Speckled energy rim inspired by the original's luminous particle orb.
    // It is static (not random per frame), so it remains stable while resizing.
    for (int i = 0; i < 1900; ++i)
    {
        const float a = (float)i * 2.39996323f;
        const float jitter = (float)((i * 37) % 101) / 100.0f;
        const float rr = 226.0f + (jitter - 0.5f) * 17.0f;
        const float px = 470.0f + std::cos(a) * rr;
        const float py = 368.0f + std::sin(a) * rr * 0.98f;
        const float dot = (i % 9 == 0) ? 2.0f : 1.0f;
        const auto glow = (i % 5 == 0) ? juce::Colour(0xffe3d8ff)
                         : ((i % 3 == 0) ? juce::Colour(0xffb78cff)
                                         : juce::Colour(0xffaaa5b7));
        g.setColour(glow.withAlpha((i % 6 == 0) ? 0.20f : 0.065f));
        g.fillEllipse(px, py, dot, dot);
    }
    g.setColour(juce::Colour(0xff7652a9)); g.drawEllipse(319,215,303,302,8.0f);
    g.setColour(juce::Colour(0xffb9a2ff)); g.drawEllipse(370,270,201,190,5.0f);
    // Fine highlight strokes create a more dimensional glass-ring appearance.
    juce::Path glassHighlight;
    glassHighlight.addArc(373.0f, 273.0f, 195.0f, 184.0f, 3.55f, 5.15f, true);
    g.setColour(juce::Colour(0xffd8c7ff).withAlpha(0.45f));
    g.strokePath(glassHighlight, juce::PathStrokeType(1.5f));

    juce::Path cyanHighlight;
    cyanHighlight.addArc(377.0f, 277.0f, 188.0f, 177.0f, 1.55f, 2.45f, true);
    g.setColour(juce::Colour(0xff68dfff).withAlpha(0.65f));
    g.strokePath(cyanHighlight, juce::PathStrokeType(2.0f));
    g.setColour(juce::Colour(0xffbb72e8)); g.setFont(juce::Font(juce::FontOptions(54.0f)));
    // Draw a small lightning mark with vector geometry instead of a font glyph.
    juce::Path bolt;
    bolt.startNewSubPath(469.0f, 326.0f);
    bolt.lineTo(451.0f, 353.0f);
    bolt.lineTo(466.0f, 353.0f);
    bolt.lineTo(458.0f, 378.0f);
    bolt.lineTo(483.0f, 345.0f);
    bolt.lineTo(468.0f, 345.0f);
    bolt.closeSubPath();
    g.setColour(juce::Colour(0xffbb72e8));
    g.fillPath(bolt);

    // Left group selector rail
    g.setColour(juce::Colour(0xff18181b)); g.fillRoundedRectangle(-15,150,73,430,22.0f);
    g.setColour(purple); g.setFont(juce::Font(juce::FontOptions(15.0f,juce::Font::bold)));
    g.drawText("NOTES",5,170,48,40,juce::Justification::centred);
    g.setColour(muted); g.setFont(juce::Font(juce::FontOptions(19.0f,juce::Font::bold)));
    for(int i=0;i<4;++i) g.drawText(juce::String(i+1),13,232+i*91,38,42,juce::Justification::centred);
    g.setColour(purple); g.setFont(juce::Font(juce::FontOptions(14.0f,juce::Font::bold)));
    for(int i=0;i<4;++i) g.drawText("0",30,206+i*91,24,18,juce::Justification::centred);

    // Decorative outer orb and signal-status lamp.
    g.setColour(juce::Colour(0xff5a5a5d)); g.fillEllipse(146,557,40,40);
    g.setColour(juce::Colour(0xff8b8b8e)); g.drawEllipse(146,557,40,40,1.5f);
    g.setColour(juce::Colour(0xffb9a2ff).withAlpha(0.22f)); g.fillEllipse(154,565,24,24);
    g.setColour(juce::Colour(0xffb8b5bf)); g.fillEllipse(158,569,16,16);
    g.setColour(juce::Colour(0xff3c3b42)); g.drawLine(0.0f, 673.0f, 1366.0f, 673.0f, 1.0f);

    // Lower selector strip
    g.setColour(juce::Colour(0xff27272b)); g.fillRect(0,676,1366,57);
    g.setColour(muted); g.setFont(juce::Font(juce::FontOptions(15.0f,juce::Font::bold)));
    g.drawText("Select",30,688,95,32,juce::Justification::centredLeft);
    g.setColour(purple); g.fillRoundedRectangle(167,686,114,35,17.0f);
    g.setColour(text); 
    g.setColour(juce::Colour(0xff18181b)); g.fillRoundedRectangle(298,686,310,35,17.0f);
    g.setColour(juce::Colour(0xff09090a)); g.fillRoundedRectangle(685,684,657,39,19.0f);
    g.setColour(juce::Colour(0xff35343b)); g.drawRoundedRectangle(685,684,657,39,19.0f,1.0f);
    g.setColour(juce::Colour(0xffd58acb).withAlpha(0.13f)); g.fillRoundedRectangle(778,687,82,32,15.0f);
    g.setColour(muted); g.drawText("None       Short       Mid       Long",700,686,285,35,juce::Justification::centredLeft);

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

    // Tone-volume / scale control strip at the left of the keyboard.
    g.setColour(juce::Colour(0xff171719)); g.fillRect(0,788,165,287);
    g.setColour(juce::Colour(0xff38383d)); g.drawLine(0.0f,788.0f,1366.0f,788.0f,1.0f);
    g.setColour(muted); g.setFont(juce::Font(juce::FontOptions(15.0f,juce::Font::bold)));
    g.drawText("Tone Volume",24,801,125,25,juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xff070708)); g.fillRoundedRectangle(10,833,145,19,9.0f);
    g.setColour(juce::Colour(0xff713a46)); g.fillRoundedRectangle(14,837,92,11,5.0f);
    g.setColour(juce::Colour(0xffd0a3b9)); g.fillRect(103,834,3,17);
    g.setColour(juce::Colour(0xff252329)); g.fillRoundedRectangle(12,875,141,169,15.0f);
    g.setColour(juce::Colour(0xffd7c8df)); g.setFont(juce::Font(juce::FontOptions(44.0f,juce::Font::bold)));
    g.drawText("--",12,902,141,58,juce::Justification::centred);
    g.setColour(muted); g.setFont(juce::Font(juce::FontOptions(16.0f,juce::Font::bold)));
    g.drawText("hover to",15,960,135,24,juce::Justification::centred);
    g.drawText("set scale",15,983,135,24,juce::Justification::centred);
    g.setColour(purple); g.setFont(juce::Font(juce::FontOptions(15.0f,juce::Font::bold)));
    g.drawText("Chromatic",16,1048,140,25,juce::Justification::centredLeft);

    // Keyboard and right status rail
    drawPiano(g,{165,788,1047,287});
    g.setColour(juce::Colour(0xff222226)); g.fillRect(1212,734,154,341);
    g.setColour(muted); g.setFont(juce::Font(juce::FontOptions(14.0f,juce::Font::bold)));
    g.drawText("In / Out",1225,741,130,24,juce::Justification::centredLeft);
    g.drawText("HeatMaps",1225,761,130,24,juce::Justification::centredLeft);
    g.drawText("Detected",1225,800,130,24,juce::Justification::centredLeft);
    g.drawText("Notes",1225,820,130,24,juce::Justification::centredLeft);
    g.drawText("Global",1225,880,130,24,juce::Justification::centredLeft);
    g.drawText("Bypass",1225,900,130,24,juce::Justification::centredLeft);
    g.drawText("Pitch Reference",1225,958,130,24,juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xff070708)); g.fillRoundedRectangle(1218,987,140,19,10.0f);
    g.setColour(text); g.drawText("440.0 Hz",1220,1015,136,25,juce::Justification::centred);
    g.setColour(juce::Colour(0xff202024)); g.fillRect(0,1050,1366,30);
    g.setColour(purple); g.drawText("Chromatic",24,1052,140,26,juce::Justification::centredLeft);
}

void MetaTuneInspiredAudioProcessorEditor::resized()
{
    const float sx=getWidth()/1366.0f, sy=getHeight()/1080.0f;
    speed.setBounds(955*sx,120*sy,225*sx,225*sy);
    sustain.setBounds(1155*sx,215*sy,150*sx,150*sy);
    amount.setBounds(1090*sx,395*sy,225*sx,225*sy);
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
        status.setText(juce::String(hz,1)+" Hz   -   correction "+juce::String(processor.getCorrectionCents(),1)+" cents",juce::dontSendNotification);
    } else {
        noteReadout.setText("",juce::dontSendNotification);
        status.setText("Waiting for pitched input...",juce::dontSendNotification);
    }
}
