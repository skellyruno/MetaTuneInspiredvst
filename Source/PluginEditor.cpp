#include "PluginEditor.h"

namespace {
const juce::Colour bg(0xff101012), panel(0xff242427), panel2(0xff18181b);
const juce::Colour purple(0xffa36bff), pink(0xffdf83c5), cyan(0xff66dfff);
const juce::Colour text(0xffe7e5e9), muted(0xffa7a5aa);

// Use named typefaces and JUCE 8 portable metrics consistently throughout the UI.
// Disabling Direct2D bitmap hinting keeps glyphs closer to JUCE's outline renderer.
juce::Font uiFont(float height, int style = juce::Font::plain)
{
    auto options = juce::FontOptions("Segoe UI", height, style)
        .withMetricsKind(juce::TypefaceMetricsKind::portable)
        .withDirect2DHinting(false)
        .withFallbacks(std::vector<juce::String>{ "Arial", "Tahoma" });
    return juce::Font(options);
}

juce::Font digitalFont(float height, int style = juce::Font::bold)
{
    auto options = juce::FontOptions("Consolas", height, style)
        .withMetricsKind(juce::TypefaceMetricsKind::portable)
        .withDirect2DHinting(false)
        .withFallbacks(std::vector<juce::String>{ "Segoe UI", "Arial" });
    return juce::Font(options);
}

class MetaTuneKnobLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float startAngle, float endAngle,
                          juce::Slider& slider) override
    {
        const float side = (float)juce::jmin(width, height);
        const float cx = x + width * 0.5f;
        const float cy = y + height * 0.5f;
        const bool isSpeed = slider.getName() == "Speed";
        const bool isAmount = slider.getName() == "Amount";
        const float radius = side * (isSpeed ? 0.435f : (isAmount ? 0.435f : 0.425f));
        const float tickRadius = radius * 0.82f;
        const juce::Colour accent = isAmount ? pink : purple;

        // Deep, machined outer housing with two edge bevels.
        g.setColour(juce::Colour(0xff050506).withAlpha(0.82f));
        g.fillEllipse(cx - radius - 10.0f, cy - radius - 9.0f,
                      (radius + 10.0f) * 2.0f, (radius + 9.0f) * 2.0f);
        g.setColour(juce::Colour(0xff424148));
        g.drawEllipse(cx - radius - 7.0f, cy - radius - 7.0f,
                      (radius + 7.0f) * 2.0f, (radius + 7.0f) * 2.0f, 1.4f);
        g.setColour(juce::Colour(0xff09090b));
        g.fillEllipse(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
        g.setColour(juce::Colour(0xff202025));
        g.drawEllipse(cx - radius + 3.0f, cy - radius + 3.0f,
                      (radius - 3.0f) * 2.0f, (radius - 3.0f) * 2.0f, 1.0f);

        // The open C-shaped housing is an important visual signature of Speed.
        juce::Path casing;
        casing.addCentredArc(cx, cy, radius + 5.0f, radius + 5.0f, 0.0f,
                             startAngle, endAngle, true);
        g.setColour(juce::Colour(0xff030304));
        g.strokePath(casing, juce::PathStrokeType(isSpeed ? 15.0f : 9.0f,
                                                  juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
        g.setColour(juce::Colour(0xff393940));
        g.strokePath(casing, juce::PathStrokeType(isSpeed ? 8.0f : 4.2f,
                                                  juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
        juce::Path rimHighlight;
        rimHighlight.addCentredArc(cx - 0.6f, cy - 0.6f, radius + 8.2f, radius + 8.2f,
                                   0.0f, startAngle + 0.08f, endAngle - 0.08f, true);
        g.setColour(juce::Colour(0xff85818d).withAlpha(0.30f));
        g.strokePath(rimHighlight, juce::PathStrokeType(0.9f));

        // Cut a true notch into the face-side of the Speed housing so its opening
        // is visible even at the compact 683 x 540 default size.
        if (isSpeed)
        {
            const float gapStart = endAngle;
            const float gapEnd = startAngle + juce::MathConstants<float>::twoPi;
            const float outerR = radius + 12.0f;
            const float innerR = radius * 0.75f;
            juce::Path notch;
            for (int i = 0; i <= 20; ++i)
            {
                const float t = (float)i / 20.0f;
                const float a = gapStart + (gapEnd - gapStart) * t;
                const float px = cx + std::cos(a) * outerR;
                const float py = cy + std::sin(a) * outerR;
                if (i == 0) notch.startNewSubPath(px, py);
                else notch.lineTo(px, py);
            }
            for (int i = 20; i >= 0; --i)
            {
                const float t = (float)i / 20.0f;
                const float a = gapStart + (gapEnd - gapStart) * t;
                notch.lineTo(cx + std::cos(a) * innerR, cy + std::sin(a) * innerR);
            }
            notch.closeSubPath();
            g.setColour(juce::Colour(0xff29292c));
            g.fillPath(notch);
            g.setColour(juce::Colour(0xff17171a).withAlpha(0.8f));
            g.drawLine(cx + std::cos(gapStart) * innerR, cy + std::sin(gapStart) * innerR,
                       cx + std::cos(gapStart) * outerR, cy + std::sin(gapStart) * outerR, 1.0f);
            g.drawLine(cx + std::cos(gapEnd) * innerR, cy + std::sin(gapEnd) * innerR,
                       cx + std::cos(gapEnd) * outerR, cy + std::sin(gapEnd) * outerR, 1.0f);
        }

        // Small segmented LEDs and major marks at fifth-tick intervals.
        const int ticks = isSpeed ? 63 : (isAmount ? 57 : 37);
        for (int i = 0; i < ticks; ++i)
        {
            const float t = (float)i / (float)(ticks - 1);
            const float angle = startAngle + (endAngle - startAngle) * t;
            const float innerR = tickRadius - ((i % 5 == 0) ? 5.0f : 2.8f);
            const float outerR = tickRadius + 1.3f;
            const juce::Line<float> tick(cx + std::cos(angle) * innerR,
                                         cy + std::sin(angle) * innerR,
                                         cx + std::cos(angle) * outerR,
                                         cy + std::sin(angle) * outerR);
            juce::Colour led = accent;
            if (isSpeed && t > 0.72f)
                led = pink.interpolatedWith(cyan, (t - 0.72f) / 0.28f);
            else if (isSpeed)
                led = purple.interpolatedWith(pink, t * 0.82f);
            if (t <= sliderPos)
            {
                g.setColour(led.withAlpha(isSpeed ? 0.16f : 0.11f));
                g.drawLine(tick, isSpeed ? 4.5f : 3.4f);
            }
            g.setColour(t <= sliderPos ? led.withAlpha(0.98f)
                                       : juce::Colour(0xff514f58).withAlpha(0.78f));
            g.drawLine(tick, i % 5 == 0 ? 1.5f : 0.9f);
        }

        juce::Path progress;
        progress.addCentredArc(cx, cy, tickRadius - 4.0f, tickRadius - 4.0f, 0.0f,
                               startAngle, startAngle + (endAngle - startAngle) * sliderPos, true);
        g.setColour(accent.withAlpha(0.13f));
        g.strokePath(progress, juce::PathStrokeType(7.0f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
        g.setColour(accent.withAlpha(0.92f));
        g.strokePath(progress, juce::PathStrokeType(isSpeed ? 2.8f : 2.3f,
                                                    juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));

        const float thumbAngle = startAngle + (endAngle - startAngle) * sliderPos;
        const float thumbR = tickRadius - 4.0f;
        g.setColour(juce::Colour(0xff070708));
        g.fillEllipse(cx + std::cos(thumbAngle) * thumbR - 5.4f,
                      cy + std::sin(thumbAngle) * thumbR - 5.4f, 10.8f, 10.8f);
        g.setColour(cyan);
        g.fillEllipse(cx + std::cos(thumbAngle) * thumbR - 4.2f,
                      cy + std::sin(thumbAngle) * thumbR - 4.2f, 8.4f, 8.4f);

        const juce::String value = isSpeed ? juce::String(slider.getValue(), 1)
                                           : juce::String(slider.getValue(), 0);
        g.setColour(juce::Colour(0xfff0edf4));
        g.setFont(digitalFont(juce::jlimit(13.0f, 23.0f, side * 0.185f),
                                                   juce::Font::bold));
        g.drawText(value, juce::Rectangle<float>(cx - radius * 0.72f, cy - side * 0.10f,
                                                  radius * 1.44f, side * 0.27f),
                   juce::Justification::centred);
        g.setColour(muted);
        g.setFont(uiFont(juce::jlimit(9.0f, 14.0f, side * 0.105f),
                                               juce::Font::bold));
        g.drawText(slider.getName(), juce::Rectangle<float>(cx - radius * 0.72f, cy + side * 0.08f,
                                                             radius * 1.44f, side * 0.22f),
                   juce::Justification::centred);
    }

    juce::Font getComboBoxFont(juce::ComboBox& box) override
    {
        const float h = juce::jlimit(8.0f, 15.0f, (float)box.getHeight() * 0.48f);
        return uiFont(h, juce::Font::plain);
    }

    juce::Font getPopupMenuFont() override
    {
        return uiFont(12.0f, juce::Font::plain);
    }

    juce::Font getLabelFont(juce::Label& label) override
    {
        const float h = juce::jlimit(8.0f, 20.0f, (float)label.getHeight() * 0.45f);
        return uiFont(h, juce::Font::plain);
    }

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                          bool highlighted, bool down) override
    {
        juce::ignoreUnused(highlighted, down);
        const auto bounds = button.getLocalBounds().toFloat();
        const float h = bounds.getHeight();
        const float sw = juce::jlimit(19.0f, 32.0f, h * 1.25f);
        const float sh = juce::jlimit(9.0f, 15.0f, h * 0.58f);
        const float x = bounds.getX() + 1.0f;
        const float y = bounds.getCentreY() - sh * 0.5f;
        const bool on = button.getToggleState();

        g.setColour(on ? juce::Colour(0xff7251a1) : juce::Colour(0xff111114));
        g.fillRoundedRectangle(x, y, sw, sh, sh * 0.5f);
        g.setColour(on ? juce::Colour(0xffd5aaff) : juce::Colour(0xff46444c));
        g.drawRoundedRectangle(x, y, sw, sh, sh * 0.5f, 1.0f);

        const float knob = sh - 3.0f;
        const float kx = on ? x + sw - knob - 1.5f : x + 1.5f;
        g.setColour(on ? juce::Colour(0xfff0d9ff) : juce::Colour(0xff929098));
        g.fillEllipse(kx, y + 1.5f, knob, knob);

        g.setColour(button.findColour(juce::ToggleButton::textColourId));
        g.setFont(uiFont(juce::jlimit(8.0f, 14.0f, h * 0.46f), juce::Font::bold));
        g.drawText(button.getButtonText(),
                   juce::Rectangle<float>(x + sw + 5.0f, bounds.getY(),
                                          juce::jmax(0.0f, bounds.getRight() - x - sw - 5.0f),
                                          bounds.getHeight()),
                   juce::Justification::centredLeft, true);
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
    speed.setName("Speed"); amount.setName("Amount"); sustain.setName("Sustain");
    styleKnob(speed); styleKnob(amount); styleKnob(sustain);
    speed.setLookAndFeel(&metaTuneKnobLookAndFeel);
    amount.setLookAndFeel(&metaTuneKnobLookAndFeel);
    sustain.setLookAndFeel(&metaTuneKnobLookAndFeel);
    speed.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    amount.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    sustain.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    speedLabel.setText("Speed", juce::dontSendNotification);
    amountLabel.setText("Amount", juce::dontSendNotification);
    noteReadout.setJustificationType(juce::Justification::centred);
    noteReadout.setFont(uiFont(42.0f, juce::Font::bold));
    noteReadout.setColour(juce::Label::textColourId, juce::Colour(0xfff2edf8));
    status.setColour(juce::Label::textColourId, juce::Colour(0xffdbd8e0));
    status.setJustificationType(juce::Justification::centredLeft);
    presetLabel.setText("No preset selected", juce::dontSendNotification);
    presetLabel.setJustificationType(juce::Justification::centredLeft);

    juce::Component* components[] = { &speed, &amount, &sustain, &scale, &root, &stabilizer, &lowLatency,
                                      &noteReadout, &status };
    for (auto* c : components) addAndMakeVisible(c);

    scale.addItem("Chromatic", 1); scale.addItem("Major", 2); scale.addItem("Minor", 3);
    for (auto* combo : { &scale, &root })
    {
        combo->setLookAndFeel(&metaTuneKnobLookAndFeel);
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
    scale.setLookAndFeel(nullptr);
    root.setLookAndFeel(nullptr);
}

void MetaTuneInspiredAudioProcessorEditor::styleKnob(juce::Slider& s)
{
    s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    s.setRange(0.0,100.0,0.1);
    if (s.getName() == "Speed")
        s.setRotaryParameters(0.56f, 5.55f, true);
    else if (s.getName() == "Amount")
        s.setRotaryParameters(0.72f, 5.62f, true);
    else
        s.setRotaryParameters(0.84f, 5.44f, true);
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
    g.setColour(text); g.setFont(uiFont(r.getWidth()*0.18f,juce::Font::bold));
    g.drawText(number, r.withY(c.y-17).withHeight(34),juce::Justification::centred);
    g.setColour(muted); g.setFont(uiFont(15.0f,juce::Font::bold));
    g.drawText(label,r.withY(c.y+18).withHeight(25),juce::Justification::centred);
}

void MetaTuneInspiredAudioProcessorEditor::drawPiano(juce::Graphics& g, juce::Rectangle<int> area)
{
    const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const bool blackNote[] = { false, true, false, true, false, false, true, false, true, false, true, false };
    const int labelH = juce::jmin(52, area.getHeight() / 4);
    const int keyTop = area.getY() + labelH;
    const int keyH = area.getBottom() - keyTop;
    const int labelW = area.getWidth() / 12;

    // Upper chromatic note-name row.
    for (int i = 0; i < 12; ++i)
    {
        const int x = area.getX() + i * labelW;
        g.setColour(blackNote[i] ? juce::Colour(0xff343439) : juce::Colour(0xff737378));
        g.fillRect(x, area.getY(), labelW - 1, labelH);
        g.setColour(juce::Colour(0xff1c1c20));
        g.drawRect(x, area.getY(), labelW - 1, labelH, 1);
        g.setColour(text);
        g.setFont(uiFont(juce::jlimit(9.0f, 15.0f, labelH * 0.29f),
                                               juce::Font::bold));
        g.drawText(names[i], x, area.getY() + 1, labelW - 1, labelH - 2,
                   juce::Justification::centred);
    }

    // The lower keyboard has seven natural keys, each with its own subtle
    // vertical tone gradient and a thin illuminated edge.
    const float naturalW = (float)area.getWidth() / 7.0f;
    const juce::Colour naturalTop(0xff77777b);
    const juce::Colour naturalBottom(0xffb1a0e2);
    for (int i = 0; i < 7; ++i)
    {
        const float x = (float)area.getX() + naturalW * (float)i;
        juce::Rectangle<float> key(x, (float)keyTop, naturalW - 1.0f, (float)keyH);
        g.setColour(juce::Colour(0xff080809));
        g.fillRect(key.translated(0.0f, 2.0f));
        g.setGradientFill(juce::ColourGradient(naturalTop, key.getX(), key.getY(),
                                               naturalBottom, key.getX(), key.getBottom(), false));
        g.fillRect(key);
        g.setColour(juce::Colour(0xff27262c));
        g.drawRect(key, 1.0f);
        g.setColour(juce::Colour(0xffe9ddff).withAlpha(0.34f));
        g.drawLine(key.getX() + 2.0f, key.getY() + 1.0f,
                   key.getRight() - 2.0f, key.getY() + 1.0f, 1.4f);
        g.setColour(juce::Colour(0xffb791ff).withAlpha(i % 2 == 0 ? 0.12f : 0.06f));
        g.fillRect(key.reduced(3.0f, 0.0f));
    }

    // Black keys are centered over the gaps after C, D, F, G and A.
    const int blackAfter[] = { 0, 1, 3, 4, 5 };
    const float blackW = naturalW * 0.48f;
    const float blackH = (float)keyH * 0.59f;
    for (int i = 0; i < 5; ++i)
    {
        const float boundary = (float)area.getX() + (float)(blackAfter[i] + 1) * naturalW;
        const float x = boundary - blackW * 0.5f;
        const float y = (float)keyTop;
        juce::Path shadow;
        shadow.addRoundedRectangle(x - 2.0f, y + 3.0f, blackW + 4.0f, blackH + 3.0f, 8.5f);
        g.setColour(juce::Colour(0xff020203).withAlpha(0.85f));
        g.fillPath(shadow);

        juce::Path blackKey;
        blackKey.startNewSubPath(x, y);
        blackKey.lineTo(x + blackW, y);
        blackKey.lineTo(x + blackW, y + blackH - 10.0f);
        blackKey.quadraticTo(x + blackW, y + blackH, x + blackW - 9.0f, y + blackH);
        blackKey.lineTo(x + 9.0f, y + blackH);
        blackKey.quadraticTo(x, y + blackH, x, y + blackH - 10.0f);
        blackKey.closeSubPath();
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff29282e), x, y,
                                               juce::Colour(0xff050506), x + blackW, y + blackH, false));
        g.fillPath(blackKey);
        g.setColour(juce::Colour(0xff424148));
        g.strokePath(blackKey, juce::PathStrokeType(1.1f));

        juce::Path bevel;
        bevel.startNewSubPath(x + 2.0f, y + 3.0f);
        bevel.lineTo(x + 2.0f, y + blackH - 11.0f);
        bevel.quadraticTo(x + 2.0f, y + blackH - 3.0f, x + 10.0f, y + blackH - 3.0f);
        g.setColour(juce::Colour(0xff797681).withAlpha(0.27f));
        g.strokePath(bevel, juce::PathStrokeType(1.0f));
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
    g.setColour(text); g.setFont(uiFont(24.0f));
    g.drawText("S",20,7,35,44,juce::Justification::centred);
    g.setColour(juce::Colour(0xff202023)); g.fillRect(68,6,73,48);
    g.setColour(muted); g.setFont(uiFont(12.0f,juce::Font::bold)); g.drawText("MENU",76,9,57,40,juce::Justification::centred);
    g.setColour(juce::Colour(0xff141416)); g.fillRect(143,7,245,47);
    g.setColour(muted); g.setFont(uiFont(16.0f,juce::Font::italic));
    g.drawText("No preset selected",162,10,205,39,juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xff29292d)); g.fillRect(390,7,43,47);
    g.fillRect(435,7,72,47);
    g.setColour(muted); g.setFont(uiFont(19.0f));
    g.drawText("SAVE",450,10,40,38,juce::Justification::centred);
    g.setColour(juce::Colour(0xff1b1a20)); g.fillRect(508,6,471,48);
    g.setColour(juce::Colour(0xffa36bff).withAlpha(0.22f));
    g.fillRect(508,6,471,2);
    g.setColour(juce::Colour(0xffd9caff)); g.setFont(uiFont(27.0f,juce::Font::bold));
    g.drawText("metaTune",508,6,471,48,juce::Justification::centred);
    g.setColour(juce::Colour(0xffa36bff).withAlpha(0.10f));
    g.drawLine(535.0f, 49.0f, 952.0f, 49.0f, 1.0f);
    g.setColour(muted); g.setFont(uiFont(17.0f));
    g.drawText("UNDO",986,7,45,45,juce::Justification::centred);
    g.drawText("REDO",1035,7,45,45,juce::Justification::centred);
    g.drawText("SYNC",1081,7,45,45,juce::Justification::centred);
    g.setFont(uiFont(19.0f,juce::Font::bold));
    g.drawText("A  >  B",1130,7,115,45,juce::Justification::centred);
    g.setFont(uiFont(15.0f));
    g.drawText("HELP",1260,7,86,45,juce::Justification::centred);

    // Main surface and dramatic diagonal split
    g.setColour(juce::Colour(0xff09090b)); g.fillRect(0,60,1366,615);
    juce::Path right; right.startNewSubPath(755,60); right.lineTo(1366,60); right.lineTo(1366,675); right.lineTo(1120,675); right.closeSubPath();
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff202023), 760.0f, 60.0f,
                                           juce::Colour(0xff343438), 1366.0f, 675.0f, false));
    g.fillPath(right);
    juce::Path splitHighlight;
    splitHighlight.startNewSubPath(755.0f, 60.0f);
    splitHighlight.lineTo(1120.0f, 675.0f);
    g.setColour(juce::Colour(0xff5a575f).withAlpha(0.18f));
    g.strokePath(splitHighlight, juce::PathStrokeType(2.0f));
    // Large visualizer orb: frosted particle shell, wide violet aura and split-energy centre.
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff1b1920), 385.0f, 240.0f,
                                           juce::Colour(0xff050506), 505.0f, 520.0f, true));
    g.fillEllipse(238,138,465,460);

    // Layered glow around the outer glass shell.
    for (int i = 15; i >= 1; --i)
    {
        const float inset = (float)i * 1.8f;
        g.setColour(juce::Colour(0xffd8cbf3).withAlpha(0.010f * (16 - i)));
        g.drawEllipse(241.0f - inset, 141.0f - inset,
                      459.0f + inset * 2.0f, 454.0f + inset * 2.0f, 2.3f);
    }

    // Uneven points form a frosted, almost noisy ring rather than a perfect outline.
    for (int i = 0; i < 2800; ++i)
    {
        const float angle = (float)i * 2.39996323f;
        const float j1 = (float)((i * 37) % 101) / 100.0f;
        const float j2 = (float)((i * 19 + 23) % 97) / 96.0f;
        const float radius = 225.0f + (j1 - 0.5f) * 25.0f + (j2 - 0.5f) * 11.0f;
        const float px = 470.0f + std::cos(angle) * radius;
        const float py = 368.0f + std::sin(angle) * radius * 0.985f;
        const float dot = (i % 13 == 0) ? 2.4f : ((i % 4 == 0) ? 1.35f : 0.75f);
        juce::Colour particle = juce::Colour(0xffded9e8);
        if (i % 8 == 0) particle = juce::Colour(0xffc8a5ff);
        if (i % 17 == 0) particle = juce::Colour(0xff8ce5ff);
        const float alpha = (i % 11 == 0) ? 0.36f : ((i % 3 == 0) ? 0.15f : 0.065f);
        g.setColour(particle.withAlpha(alpha));
        g.fillEllipse(px, py, dot, dot);
    }
    for (int glow = 5; glow >= 1; --glow)
    {
        g.setColour(juce::Colour(0xffe8e0ff).withAlpha(0.018f * (6 - glow)));
        g.drawEllipse(242.0f - glow * 0.8f, 142.0f - glow * 0.8f,
                      456.0f + glow * 1.6f, 452.0f + glow * 1.6f, 3.0f);
    }
    g.setColour(juce::Colour(0xffefebf8).withAlpha(0.18f));
    g.drawEllipse(242.0f, 142.0f, 456.0f, 452.0f, 2.6f);
    g.setColour(juce::Colour(0xff625d6e).withAlpha(0.58f));
    g.drawEllipse(247.0f, 147.0f, 446.0f, 442.0f, 1.0f);

    // Violet corona with a soft multi-layer edge.
    g.setColour(juce::Colour(0xff08080b).withAlpha(0.78f));
    g.fillEllipse(284.0f, 181.0f, 372.0f, 374.0f);
    for (int i = 13; i >= 1; --i)
    {
        const float grow = (float)i * 1.45f;
        g.setColour(juce::Colour(0xffa77aff).withAlpha(0.026f * (14 - i)));
        g.drawEllipse(319.0f - grow, 215.0f - grow,
                      303.0f + 2.0f * grow, 302.0f + 2.0f * grow, 2.3f);
    }
    g.setColour(juce::Colour(0xff7753ac));
    g.drawEllipse(319.0f, 215.0f, 303.0f, 302.0f, 7.0f);
    g.setColour(juce::Colour(0xffc1a5ff).withAlpha(0.58f));
    g.drawEllipse(323.0f, 219.0f, 295.0f, 294.0f, 1.3f);

    // Inner orb uses translucent cyan and magenta fields with a textured highlight.
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff3a234d), 423.0f, 300.0f,
                                             juce::Colour(0xff101117), 483.0f, 458.0f, true));
    g.fillEllipse(366.0f, 266.0f, 209.0f, 198.0f);
    g.setColour(juce::Colour(0xff6b3c94).withAlpha(0.26f));
    g.fillEllipse(378.0f, 278.0f, 185.0f, 175.0f);

    juce::Path cyanField;
    cyanField.startNewSubPath(388.0f, 302.0f);
    cyanField.cubicTo(372.0f, 327.0f, 382.0f, 373.0f, 411.0f, 401.0f);
    cyanField.cubicTo(430.0f, 417.0f, 450.0f, 422.0f, 470.0f, 423.0f);
    cyanField.lineTo(470.0f, 365.0f);
    cyanField.closeSubPath();
    g.setColour(juce::Colour(0xff38c9ed).withAlpha(0.14f));
    g.fillPath(cyanField);

    juce::Path magentaField;
    magentaField.startNewSubPath(470.0f, 291.0f);
    magentaField.cubicTo(504.0f, 299.0f, 541.0f, 325.0f, 552.0f, 355.0f);
    magentaField.cubicTo(563.0f, 392.0f, 534.0f, 421.0f, 492.0f, 427.0f);
    magentaField.lineTo(470.0f, 365.0f);
    magentaField.closeSubPath();
    g.setColour(juce::Colour(0xffd65be0).withAlpha(0.16f));
    g.fillPath(magentaField);

    // Finer inner rim and a short curved plasma streak.
    juce::Path innerRim;
    innerRim.addEllipse(370.0f, 270.0f, 201.0f, 190.0f);
    g.setColour(juce::Colour(0xfff0e2ff).withAlpha(0.72f));
    g.strokePath(innerRim, juce::PathStrokeType(3.5f));
    g.setColour(juce::Colour(0xffe8c9ff).withAlpha(0.15f));
    g.strokePath(innerRim, juce::PathStrokeType(9.0f));

    juce::Path plasma;
    plasma.startNewSubPath(397.0f, 383.0f);
    plasma.cubicTo(419.0f, 376.0f, 446.0f, 353.0f, 466.0f, 330.0f);
    plasma.cubicTo(489.0f, 305.0f, 516.0f, 301.0f, 532.0f, 312.0f);
    g.setColour(juce::Colour(0xffd4a0ff).withAlpha(0.42f));
    g.strokePath(plasma, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    // Lightning is vector-drawn, so no missing glyphs at small DPI scales.
    juce::Path bolt;
    bolt.startNewSubPath(477.0f, 326.0f);
    bolt.lineTo(455.0f, 354.0f);
    bolt.lineTo(469.0f, 354.0f);
    bolt.lineTo(461.0f, 380.0f);
    bolt.lineTo(486.0f, 345.0f);
    bolt.lineTo(472.0f, 345.0f);
    bolt.closeSubPath();
    g.setColour(juce::Colour(0xffc66dff).withAlpha(0.15f));
    g.fillPath(bolt, juce::AffineTransform::translation(1.0f, 1.0f));
    g.setColour(juce::Colour(0xffc66dff));
    g.fillPath(bolt);

    // Left group selector rail
    g.setColour(juce::Colour(0xff18181b)); g.fillRoundedRectangle(-15,150,73,430,22.0f);
    g.setColour(purple); g.setFont(uiFont(15.0f,juce::Font::bold));
    g.drawText("NOTES",5,170,48,40,juce::Justification::centred);
    g.setColour(muted); g.setFont(uiFont(19.0f,juce::Font::bold));
    for(int i=0;i<4;++i) g.drawText(juce::String(i+1),13,232+i*91,38,42,juce::Justification::centred);
    g.setColour(purple); g.setFont(uiFont(14.0f,juce::Font::bold));
    for(int i=0;i<4;++i) g.drawText("0",30,206+i*91,24,18,juce::Justification::centred);

    // Decorative outer orb and signal-status lamp.
    g.setColour(juce::Colour(0xff5a5a5d)); g.fillEllipse(146,557,40,40);
    g.setColour(juce::Colour(0xff8b8b8e)); g.drawEllipse(146,557,40,40,1.5f);
    g.setColour(juce::Colour(0xffb9a2ff).withAlpha(0.22f)); g.fillEllipse(154,565,24,24);
    g.setColour(juce::Colour(0xffb8b5bf)); g.fillEllipse(158,569,16,16);
    g.setColour(juce::Colour(0xff3c3b42)); g.drawLine(0.0f, 673.0f, 1366.0f, 673.0f, 1.0f);

    // Lower selector strip
    g.setColour(juce::Colour(0xff27272b)); g.fillRect(0,676,1366,57);
    g.setColour(muted); g.setFont(uiFont(15.0f,juce::Font::bold));
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
    g.setColour(juce::Colour(0xff0b0b0d)); g.fillRect(160,738,1052,45);
    g.setColour(juce::Colour(0xff303036)); g.drawLine(160.0f,760.0f,1212.0f,760.0f,1.0f);
    g.setColour(muted); g.setFont(uiFont(14.0f,juce::Font::bold));
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
    g.setColour(muted); g.setFont(uiFont(15.0f,juce::Font::bold));
    g.drawText("Tone Volume",24,801,125,25,juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xff070708)); g.fillRoundedRectangle(10,833,145,19,9.0f);
    g.setColour(juce::Colour(0xff713a46)); g.fillRoundedRectangle(14,837,92,11,5.0f);
    g.setColour(juce::Colour(0xffd0a3b9)); g.fillRect(103,834,3,17);
    g.setColour(juce::Colour(0xff252329)); g.fillRoundedRectangle(12,875,141,169,15.0f);
    g.setColour(juce::Colour(0xffe7d9ef)); g.setFont(uiFont(50.0f,juce::Font::bold));
    g.drawText("E",12,891,141,58,juce::Justification::centred);
    g.setColour(muted); g.setFont(uiFont(16.0f,juce::Font::bold));
    g.drawText("custom",15,943,135,22,juce::Justification::centred);
    g.drawText("hover to",15,969,135,22,juce::Justification::centred);
    g.drawText("set scale",15,990,135,22,juce::Justification::centred);
    g.setColour(purple); g.setFont(uiFont(15.0f,juce::Font::bold));
    g.drawText("SCALE",16,1048,140,25,juce::Justification::centredLeft);

    // Keyboard and right status rail
    drawPiano(g,{165,788,1047,287});
    g.setColour(juce::Colour(0xff222226)); g.fillRect(1212,734,154,341);
    g.setColour(muted); g.setFont(uiFont(14.0f,juce::Font::bold));
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
    scale.setBounds(165*sx,686*sy,176*sx,35*sy);
    root.setBounds(350*sx,686*sy,100*sx,35*sy);
    stabilizer.setBounds(990*sx,686*sy,185*sx,35*sy);
    lowLatency.setBounds(1177*sx,686*sy,175*sx,35*sy);
    noteReadout.setBounds(360*sx,365*sy,230*sx,70*sy);
    status.setBounds(32*sx,610*sy,600*sx,25*sy);
    const float uiScale = juce::jmin(sx, sy);
    noteReadout.setFont(uiFont(juce::jlimit(16.0f, 42.0f, 42.0f * uiScale),
                              juce::Font::bold));
    status.setFont(uiFont(juce::jlimit(8.0f, 15.0f, 15.0f * uiScale),
                          juce::Font::bold));
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
