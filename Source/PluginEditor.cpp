#include "PluginEditor.h"
#include <memory>

namespace {
const juce::Colour bg(0xff101012), panel(0xff242427), panel2(0xff18181b);
const juce::Colour purple(0xffa36bff), pink(0xffdf83c5), cyan(0xff66dfff);
const juce::Colour text(0xffe7e5e9), muted(0xffa7a5aa);
constexpr float pitchReferenceMinHz = 415.3047f; // A440 - 1 semitone
constexpr float pitchReferenceMaxHz = 466.1638f; // A440 + 1 semitone

// Use named typefaces and JUCE 8 portable metrics consistently throughout the UI.
// This build uses a JUCE version that does not expose Direct2D hinting through
// FontOptions, so we avoid that newer API and rely on portable metrics/fallbacks.
juce::Font uiFont(float height, int style = juce::Font::plain)
{
    auto options = juce::FontOptions("Segoe UI", height, style)
        .withMetricsKind(juce::TypefaceMetricsKind::portable)
        .withFallbacks(std::vector<juce::String>{ "Arial", "Tahoma" });
    return juce::Font(options);
}

juce::Font digitalFont(float height, int style = juce::Font::bold)
{
    auto options = juce::FontOptions("Consolas", height, style)
        .withMetricsKind(juce::TypefaceMetricsKind::portable)
        .withFallbacks(std::vector<juce::String>{ "Segoe UI", "Arial" });
    return juce::Font(options);
}

juce::Image createOrbArtwork()
{
    constexpr int imageWidth = 560;
    constexpr int imageHeight = 560;
    constexpr float originX = 185.0f;
    constexpr float originY = 85.0f;
    juce::Image image(juce::Image::ARGB, imageWidth, imageHeight, true);
    image.clear(image.getBounds(), juce::Colours::transparentBlack);
    juce::Graphics g(image);
    g.addTransform(juce::AffineTransform::translation(-originX, -originY));
    // Center orb - layered dark glass, frosted particle shell, violet neon halo and split-color core.
    constexpr float orbCx = 470.0f, orbCy = 368.0f;
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff302a39), orbCx - 54.0f, orbCy - 72.0f,
                                           juce::Colour(0xff08070c), orbCx + 228.0f, orbCy + 226.0f, true));
    g.fillEllipse(238.0f, 138.0f, 465.0f, 460.0f);

    // Brighter frosted glass volume. The reference has a clearly visible cloudy
    // silver shell, so build several broader and stronger translucent edge layers.
    g.setColour(juce::Colour(0xffe8e3f1).withAlpha(0.075f));
    g.drawEllipse(226.0f, 126.0f, 489.0f, 484.0f, 32.0f);
    g.setColour(juce::Colour(0xffe8e3f1).withAlpha(0.085f));
    g.drawEllipse(233.0f, 133.0f, 475.0f, 470.0f, 25.0f);
    g.setColour(juce::Colour(0xfff1edf7).withAlpha(0.115f));
    g.drawEllipse(238.0f, 138.0f, 465.0f, 460.0f, 18.0f);
    g.setColour(juce::Colour(0xfff5f0ff).withAlpha(0.15f));
    g.drawEllipse(242.0f, 142.0f, 457.0f, 452.0f, 11.0f);
    g.setColour(juce::Colour(0xffe7e0f2).withAlpha(0.17f));
    g.drawEllipse(245.0f, 145.0f, 451.0f, 446.0f, 5.0f);

    // A cool-white top-left haze and a subtle violet lower rim lift the sphere
    // off the background rather than letting it disappear into black.
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xfff0eaff).withAlpha(0.18f),
                                           345.0f, 205.0f,
                                           juce::Colour(0xffa77cff).withAlpha(0.0f),
                                           555.0f, 415.0f, true));
    g.fillEllipse(230.0f, 130.0f, 480.0f, 475.0f);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff82dfff).withAlpha(0.085f),
                                           620.0f, 425.0f,
                                           juce::Colour(0xff82dfff).withAlpha(0.0f),
                                           465.0f, 368.0f, true));
    g.fillEllipse(238.0f, 138.0f, 465.0f, 460.0f);
    for (int i = 22; i >= 1; --i)
    {
        const float grow = (float)i * 1.45f;
        const float alpha = 0.045f + 0.0045f * (22 - i);
        g.setColour(juce::Colour(0xffeeeaf6).withAlpha(alpha));
        g.drawEllipse(241.0f - grow, 141.0f - grow, 459.0f + grow * 2.0f,
                      454.0f + grow * 2.0f, 2.0f + (float)(i % 3) * 0.35f);
    }

    // Static deterministic noise lets the frosted shell look organic without flickering.
    // Radial jitter creates dense white particles and a smaller number of violet/cyan sparks.
    for (int i = 0; i < 6200; ++i)
    {
        const unsigned int h1 = (unsigned int)i * 747796405u + 2891336453u;
        const unsigned int h2 = (unsigned int)i * 277803737u + 1013904223u;
        const float a = (float)i * 2.39996323f + (float)(h1 & 1023u) * 0.00019f;
        const float rj = (float)(h2 % 10000u) / 10000.0f;
        const float cluster = (float)((h1 >> 8u) % 1000u) / 1000.0f;
        const float radius = 219.0f + (rj - 0.5f) * 39.0f + (cluster - 0.5f) * 10.0f;
        const float px = orbCx + std::cos(a) * radius;
        const float py = orbCy + std::sin(a) * radius * 0.982f;
        const float dot = (i % 17 == 0) ? 3.0f : ((i % 5 == 0) ? 1.8f : 0.9f);
        juce::Colour particle = juce::Colour(0xfff3eff9);
        if (i % 11 == 0) particle = juce::Colour(0xffd9c9f7);
        if (i % 37 == 0) particle = juce::Colour(0xff95dfff);
        const float angleLight = juce::jlimit(0.45f, 1.35f, 0.82f + 0.42f * -std::sin(a));
        const float brightness = ((i % 13 == 0) ? 0.94f : ((i % 4 == 0) ? 0.64f : 0.29f)) * angleLight;
        g.setColour(particle.withAlpha(juce::jmin(0.98f, brightness)));
        g.fillEllipse(px, py, dot, dot);
    }

    // Fine drifting flecks spread through the shell's thickness: this breaks the
    // perfect circular edge and creates the luminous grain seen in the original.
    for (int i = 0; i < 2350; ++i)
    {
        const unsigned int h1 = (unsigned int)i * 2246822519u + 3266489917u;
        const unsigned int h2 = (unsigned int)i * 3266489917u + 668265263u;
        const float a = (float)i * 2.39996323f + (float)(h1 % 4096u) * 0.00021f;
        const float r = 202.0f + (float)(h2 % 10000u) * 0.0041f;
        const float px = orbCx + std::cos(a) * r;
        const float py = orbCy + std::sin(a) * r * 0.982f;
        const float size = (i % 23 == 0) ? 3.5f : ((i % 7 == 0) ? 2.0f : 0.9f);
        const auto tint = (i % 19 == 0) ? juce::Colour(0xffc7afff)
                         : ((i % 31 == 0) ? juce::Colour(0xffa9e6ff) : juce::Colour(0xffece7f4));
        const float alpha = (i % 17 == 0) ? 0.46f : ((i % 4 == 0) ? 0.25f : 0.095f);
        g.setColour(tint.withAlpha(alpha));
        g.fillEllipse(px, py, size, size);
    }

    // Fine highlight edge and a dark inner lens create depth, rather than a flat ring.
    g.setColour(juce::Colour(0xfff1edf6).withAlpha(0.45f));
    g.drawEllipse(243.0f, 143.0f, 454.0f, 450.0f, 1.5f);
    g.setColour(juce::Colour(0xff6d6879).withAlpha(0.62f));
    g.drawEllipse(250.0f, 150.0f, 440.0f, 436.0f, 1.0f);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff1a1720), 390.0f, 260.0f,
                                           juce::Colour(0xff08070b), 470.0f, 550.0f, true));
    g.fillEllipse(280.0f, 178.0f, 380.0f, 380.0f);

    // Violet halo - numerous soft strokes underneath a crisp neon ring.
    for (int i = 12; i >= 1; --i)
    {
        const float inset = (float)i * 1.8f;
        g.setColour(juce::Colour(0xffa875ff).withAlpha(0.048f * (13 - i)));
        g.drawEllipse(319.0f - inset, 215.0f - inset,
                      303.0f + inset * 2.0f, 302.0f + inset * 2.0f, 3.0f + (float)(i % 2));
    }
    g.setColour(juce::Colour(0xffa47aff).withAlpha(0.46f));
    g.drawEllipse(312.0f, 208.0f, 317.0f, 316.0f, 19.0f);
    g.setColour(juce::Colour(0xff8f5cff).withAlpha(0.52f));
    g.drawEllipse(316.0f, 212.0f, 309.0f, 308.0f, 13.0f);
    g.setColour(juce::Colour(0xffa77aff));
    g.drawEllipse(319.0f, 215.0f, 303.0f, 302.0f, 7.4f);
    g.setColour(juce::Colour(0xffe2ccff).withAlpha(0.76f));
    g.drawEllipse(323.0f, 219.0f, 295.0f, 294.0f, 1.5f);

    // Inner 2-tone plasma globe. A cloudy gradient, fine particle edge and curved energy seam
    // mimic the cyan-left / magenta-right glass surface from the reference screenshot.
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff493658), 411.0f, 294.0f,
                                           juce::Colour(0xff17131f), 518.0f, 443.0f, true));
    g.fillEllipse(370.0f, 270.0f, 201.0f, 190.0f);

    // Overlapping radial colour fields form cloudy energy rather than a hard vertical split.
    // Each field fades to transparent and is clipped to the spherical glass lens.
    juce::Path innerGlassClip;
    innerGlassClip.addEllipse(372.0f, 272.0f, 197.0f, 186.0f);
    g.saveState();
    g.reduceClipRegion(innerGlassClip);

    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff62dff5).withAlpha(0.68f),
                                           414.0f, 355.0f,
                                           juce::Colour(0xff4acde7).withAlpha(0.0f),
                                           482.0f, 355.0f, true));
    g.fillEllipse(365.0f, 293.0f, 155.0f, 150.0f);

    g.setGradientFill(juce::ColourGradient(juce::Colour(0xffe18ce4).withAlpha(0.62f),
                                           519.0f, 349.0f,
                                           juce::Colour(0xffb65cc9).withAlpha(0.0f),
                                           584.0f, 349.0f, true));
    g.fillEllipse(427.0f, 281.0f, 155.0f, 166.0f);

    g.setGradientFill(juce::ColourGradient(juce::Colour(0xffa08bff).withAlpha(0.46f),
                                           462.0f, 407.0f,
                                           juce::Colour(0xff6651d0).withAlpha(0.0f),
                                           462.0f, 473.0f, true));
    g.fillEllipse(393.0f, 348.0f, 153.0f, 112.0f);

    // A barely visible shadow along the lower-right creates internal spherical depth.
    juce::Path internalShade;
    internalShade.startNewSubPath(472.0f, 285.0f);
    internalShade.cubicTo(530.0f, 302.0f, 552.0f, 345.0f, 552.0f, 367.0f);
    internalShade.cubicTo(550.0f, 407.0f, 518.0f, 427.0f, 489.0f, 435.0f);
    internalShade.lineTo(471.0f, 364.0f);
    internalShade.closeSubPath();
    g.setColour(juce::Colour(0xff120d1b).withAlpha(0.045f));
    g.fillPath(internalShade);
    g.restoreState();

    // Cloudy micro-texture inside the glass, kept faint so the note icon remains clear.
    juce::Path innerClip;
    innerClip.addEllipse(372.0f, 272.0f, 197.0f, 186.0f);
    g.saveState();
    g.reduceClipRegion(innerClip);
    for (int i = 0; i < 900; ++i)
    {
        const unsigned int h = (unsigned int)i * 2246822519u + 3266489917u;
        const float px = 379.0f + (float)(h % 18300u) / 100.0f;
        const float py = 277.0f + (float)((h >> 7u) % 16900u) / 100.0f;
        const float dx = (px - 470.5f) / 99.0f;
        const float dy = (py - 365.0f) / 94.0f;
        if (dx * dx + dy * dy > 1.0f) continue;
        const bool left = px < 470.5f;
        auto cloud = left ? juce::Colour(0xff72dcf7) : juce::Colour(0xffe09be9);
        g.setColour(cloud.withAlpha((i % 9 == 0) ? 0.28f : 0.095f));
        const float dot = (i % 13 == 0) ? 1.4f : 0.7f;
        g.fillEllipse(px, py, dot, dot);
    }
    g.restoreState();

    // Grainy white inner boundary with tinted lower-left and upper-right arcs.
    for (int i = 0; i < 2100; ++i)
    {
        const unsigned int h = (unsigned int)i * 747796405u + 2891336453u;
        const float a = (float)i * 2.39996323f + (float)(h % 1000u) * 0.00012f;
        const float rr = 96.0f + ((float)((h >> 10u) % 10000u) / 10000.0f - 0.5f) * 12.0f;
        const float px = 470.0f + std::cos(a) * rr;
        const float py = 365.0f + std::sin(a) * rr * 0.91f;
        juce::Colour edge = juce::Colour(0xfff4e9ff);
        if (std::cos(a) < -0.28f) edge = juce::Colour(0xff8ce7ff);
        else if (std::cos(a) > 0.25f) edge = juce::Colour(0xffffb6e9);
        const float alpha = (i % 11 == 0) ? 0.66f : ((i % 4 == 0) ? 0.34f : 0.13f);
        const float dot = (i % 7 == 0) ? 2.0f : 0.9f;
        g.setColour(edge.withAlpha(alpha));
        g.fillEllipse(px, py, dot, dot);
    }
    juce::Path innerRim;
    innerRim.addEllipse(370.0f, 270.0f, 201.0f, 190.0f);
    g.setColour(juce::Colour(0xfff6edff).withAlpha(0.80f));
    g.strokePath(innerRim, juce::PathStrokeType(2.7f));
    g.setColour(juce::Colour(0xffb6a0dd).withAlpha(0.34f));
    g.strokePath(innerRim, juce::PathStrokeType(8.0f));

    juce::Path plasma;
    plasma.startNewSubPath(395.0f, 383.0f);
    plasma.cubicTo(420.0f, 376.0f, 446.0f, 350.0f, 468.0f, 330.0f);
    plasma.cubicTo(491.0f, 308.0f, 514.0f, 303.0f, 531.0f, 313.0f);
    g.setColour(juce::Colour(0xffd7b1ff).withAlpha(0.50f));
    g.strokePath(plasma, juce::PathStrokeType(1.7f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    juce::Path filamentOne;
    filamentOne.startNewSubPath(394.0f, 356.0f);
    filamentOne.cubicTo(414.0f, 346.0f, 430.0f, 333.0f, 448.0f, 317.0f);
    filamentOne.cubicTo(466.0f, 302.0f, 483.0f, 299.0f, 497.0f, 304.0f);
    g.setColour(juce::Colour(0xff92e7ff).withAlpha(0.24f));
    g.strokePath(filamentOne, juce::PathStrokeType(1.1f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
    juce::Path filamentTwo;
    filamentTwo.startNewSubPath(409.0f, 409.0f);
    filamentTwo.cubicTo(437.0f, 425.0f, 473.0f, 428.0f, 502.0f, 413.0f);
    g.setColour(juce::Colour(0xffe7aaff).withAlpha(0.18f));
    g.strokePath(filamentTwo, juce::PathStrokeType(1.0f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));

    // Draw the lightning mark as vector paths so the central icon is DPI independent.
    juce::Path bolt;
    bolt.startNewSubPath(477.0f, 326.0f);
    bolt.lineTo(455.0f, 354.0f);
    bolt.lineTo(469.0f, 354.0f);
    bolt.lineTo(461.0f, 380.0f);
    bolt.lineTo(486.0f, 345.0f);
    bolt.lineTo(472.0f, 345.0f);
    bolt.closeSubPath();
    g.setColour(juce::Colour(0xffc66dff).withAlpha(0.28f));
    g.fillPath(bolt, juce::AffineTransform::translation(1.2f, 1.2f));
    g.setColour(juce::Colour(0xffd077ff));
    g.fillPath(bolt);

    return image;
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
        const bool isSustain = slider.getName() == "Sustain";
        const float radius = side * (isSpeed ? 0.435f : (isAmount ? 0.435f : 0.405f));
        const float tickRadius = radius * (isSustain ? 0.78f : 0.82f);
        const juce::Colour accent = isAmount ? pink : (isSustain ? juce::Colour(0xff55545c) : purple);

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

        // Speed and Amount get the stylized open housing. Sustain is the small,
        // muted secondary dial and uses a continuous recessed ring instead.
        if (isSustain)
        {
            g.setColour(juce::Colour(0xff050506));
            g.drawEllipse(cx - radius - 4.0f, cy - radius - 4.0f,
                          (radius + 4.0f) * 2.0f, (radius + 4.0f) * 2.0f, 2.0f);
            g.setColour(juce::Colour(0xff303036));
            g.drawEllipse(cx - radius + 2.0f, cy - radius + 2.0f,
                          (radius - 2.0f) * 2.0f, (radius - 2.0f) * 2.0f, 1.0f);

            juce::Path sustainTrack;
            sustainTrack.addCentredArc(cx, cy, tickRadius + 1.5f, tickRadius + 1.5f,
                                       0.0f, startAngle, endAngle, true);
            g.setColour(juce::Colour(0xff45444b).withAlpha(0.72f));
            g.strokePath(sustainTrack, juce::PathStrokeType(2.2f));
            if (sliderPos > 0.001f)
            {
                juce::Path sustainValue;
                sustainValue.addCentredArc(cx, cy, tickRadius + 1.5f, tickRadius + 1.5f,
                                           0.0f, startAngle,
                                           startAngle + (endAngle - startAngle) * sliderPos, true);
                g.setColour(juce::Colour(0xff77717f).withAlpha(0.88f));
                g.strokePath(sustainValue, juce::PathStrokeType(2.2f));
            }
        }
        else
        {
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
        }

        // Remove a slice from the Speed housing to create MetaTune's broad C-shaped opening.
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
        const int ticks = isSustain ? 0 : (isSpeed ? 63 : 57);
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

        if (!isSustain)
        {
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
        }

        const juce::String value = isSpeed ? juce::String(slider.getValue(), 1)
                                           : juce::String(slider.getValue(), 0);
        g.setColour(juce::Colour(0xfff0edf4));
        const float valueFontSize = juce::jlimit(isSustain ? 11.0f : 13.0f,
                                                 isSustain ? 16.0f : 23.0f,
                                                 side * (isSustain ? 0.19f : 0.185f));
        g.setFont(digitalFont(valueFontSize, juce::Font::bold));
        const float valueY = cy - side * (isSustain ? 0.22f : 0.10f);
        const float valueH = side * (isSustain ? 0.30f : 0.27f);
        g.drawText(value, juce::Rectangle<float>(cx - radius * 0.72f, valueY,
                                                  radius * 1.44f, valueH),
                   juce::Justification::centred);

        g.setColour(isSustain ? juce::Colour(0xffbdbac4) : muted);
        const float labelSize = juce::jlimit(isSustain ? 8.0f : 9.0f,
                                             isSustain ? 10.0f : 14.0f,
                                             side * (isSustain ? 0.11f : 0.105f));
        g.setFont(uiFont(labelSize, juce::Font::bold));
        const float labelY = cy + side * (isSustain ? 0.08f : 0.08f);
        const float labelH = side * (isSustain ? 0.18f : 0.22f);
        g.drawText(slider.getName(), juce::Rectangle<float>(cx - radius * 0.78f, labelY,
                                                             radius * 1.56f, labelH),
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
    noteReadout.setVisible(false);
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
    auto beginUndoableEdit = [this]() { undoState = processor.parameters.copyState(); hasUndoState = true; hasRedoState = false; };
    auto finishUndoableEdit = [this]() { redoState = processor.parameters.copyState(); hasRedoState = hasUndoState; };
    speed.onDragStart = beginUndoableEdit; speed.onDragEnd = finishUndoableEdit;
    amount.onDragStart = beginUndoableEdit; amount.onDragEnd = finishUndoableEdit;
    sustain.onDragStart = beginUndoableEdit; sustain.onDragEnd = finishUndoableEdit;
    scaleAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(processor.parameters,"scale",scale);
    rootAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(processor.parameters,"root",root);
    stabAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.parameters,"stabilizer",stabilizer);
    latencyAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.parameters,"lowLatency",lowLatency);
    stabilizer.setEnabled(!lowLatency.getToggleState());
    lowLatency.onClick = [this]()
    {
        if (lowLatency.getToggleState())
            stabilizer.setToggleState(false, juce::sendNotificationSync);
        stabilizer.setEnabled(!lowLatency.getToggleState());
        repaint();
    };
    stabilizer.onClick = [this]()
    {
        if (stabilizer.getToggleState() && lowLatency.getToggleState())
            lowLatency.setToggleState(false, juce::sendNotificationSync);
        stabilizer.setEnabled(!lowLatency.getToggleState());
        repaint();
    };
    selectedNoteIndex = juce::jlimit(0, 11, root.getSelectedItemIndex() < 0 ? 4 : root.getSelectedItemIndex());
    lastNonChromaticScaleMode = scale.getSelectedItemIndex() == 2 ? 2 : 1;
    slotA = processor.parameters.copyState();
    slotB = slotA.createCopy();
    scale.onChange = [this]() { repaint(); };
    root.onChange = [this]() { selectedNoteIndex = juce::jlimit(0, 11, root.getSelectedItemIndex()); repaint(); };
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
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static const bool blackNote[] = { false, true, false, true, false, false, true, false, true, false, true, false };
    const int labelH = juce::jmin(52, area.getHeight() / 4);
    const int keyTop = area.getY() + labelH;
    const int keyH = area.getBottom() - keyTop;
    const float semitoneW = (float)area.getWidth() / 12.0f;
    const int chosenNote = juce::jlimit(0, 11, selectedNoteIndex);

    // Chromatic note names form a narrow header strip; the selected root has a violet edge.
    for (int i = 0; i < 12; ++i)
    {
        const float x = (float)area.getX() + i * semitoneW;
        g.setColour(blackNote[i] ? juce::Colour(0xff343439) : juce::Colour(0xff77777b));
        g.fillRect(x, (float)area.getY(), semitoneW - 1.0f, (float)labelH);
        g.setColour(juce::Colour(0xff202024));
        g.drawRect(x, (float)area.getY(), semitoneW - 1.0f, (float)labelH, 1.0f);
        if (i == chosenNote)
        {
            g.setColour(juce::Colour(0xffc9a7ff).withAlpha(0.35f));
            g.fillRect(x + 1.0f, (float)area.getY() + 1.0f, semitoneW - 3.0f, 3.0f);
        }
        g.setColour(text);
        g.setFont(uiFont(juce::jlimit(9.0f, 15.0f, (float)labelH * 0.29f), juce::Font::bold));
        g.drawText(names[i], juce::Rectangle<float>(x, (float)area.getY() + 1.0f,
                                                     semitoneW - 1.0f, (float)labelH - 2.0f),
                   juce::Justification::centred);
    }

    // Seven natural keys beneath the chromatic header. Their widths follow piano-key geometry;
    // black keys are separate overlays located by the 12-note header positions.
    const float naturalW = (float)area.getWidth() / 7.0f;
    const juce::Colour whiteTop(0xff85858a), whiteBottom(0xffa995e2);
    static const int naturalSemitones[] = { 0, 2, 4, 5, 7, 9, 11 };
    for (int i = 0; i < 7; ++i)
    {
        const float x = (float)area.getX() + naturalW * (float)i;
        juce::Rectangle<float> key(x, (float)keyTop, naturalW - 1.0f, (float)keyH);
        g.setColour(juce::Colour(0xff060607));
        g.fillRect(key.translated(0.0f, 2.0f));
        juce::ColourGradient natural(whiteTop, key.getX(), key.getY(),
                                     whiteBottom, key.getX(), key.getBottom(), false);
        natural.addColour(0.42, juce::Colour(0xff8d7bb9));
        g.setGradientFill(natural);
        g.fillRect(key);
        g.setColour(juce::Colour(0xff28262e));
        g.drawRect(key, 1.0f);

        const bool selected = chosenNote == naturalSemitones[i];
        g.setColour(selected ? juce::Colour(0xffd6b6ff).withAlpha(0.88f)
                             : juce::Colour(0xffe1d3ff).withAlpha(0.22f));
        g.drawLine(key.getX() + 2.0f, key.getY() + 1.2f,
                   key.getRight() - 2.0f, key.getY() + 1.2f, selected ? 2.1f : 1.1f);
        g.setColour(juce::Colour(0xffb48aff).withAlpha(selected ? 0.20f : (i % 2 == 0 ? 0.10f : 0.045f)));
        g.fillRect(key.reduced(2.0f, 0.0f));
    }

    // Authentic-looking black-key overlays are positioned using the chromatic lane centers,
    // not the white-key boundaries. This lines them up with C#, D#, F#, G# and A# labels.
    static const int blackSemitones[] = { 1, 3, 6, 8, 10 };
    const float blackW = semitoneW * 1.08f;
    const float blackH = (float)keyH * 0.59f;
    const float keyRadius = juce::jmin(10.0f, blackW * 0.12f);
    for (int i = 0; i < 5; ++i)
    {
        const int semitone = blackSemitones[i];
        const float center = (float)area.getX() + ((float)semitone + 0.5f) * semitoneW;
        const float x = center - blackW * 0.5f;
        const float y = (float)keyTop;

        juce::Path shadow;
        shadow.addRoundedRectangle(x - 2.0f, y + 3.0f, blackW + 4.0f, blackH + 4.0f, keyRadius + 2.0f);
        g.setColour(juce::Colour(0xff020203).withAlpha(0.85f));
        g.fillPath(shadow);

        juce::Path key;
        key.startNewSubPath(x, y);
        key.lineTo(x + blackW, y);
        key.lineTo(x + blackW, y + blackH - keyRadius);
        key.quadraticTo(x + blackW, y + blackH, x + blackW - keyRadius, y + blackH);
        key.lineTo(x + keyRadius, y + blackH);
        key.quadraticTo(x, y + blackH, x, y + blackH - keyRadius);
        key.closeSubPath();

        juce::ColourGradient dark(juce::Colour(0xff303035), x, y,
                                  juce::Colour(0xff080809), x + blackW, y + blackH, false);
        dark.addColour(0.45, juce::Colour(0xff1c1c20));
        g.setGradientFill(dark);
        g.fillPath(key);
        g.setColour(chosenNote == semitone ? juce::Colour(0xffd9b6ff) : juce::Colour(0xff48464f));
        g.strokePath(key, juce::PathStrokeType(chosenNote == semitone ? 1.8f : 1.0f));
        g.setColour(juce::Colour(0xff8a8592).withAlpha(0.30f));
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
    g.setColour(text); g.setFont(uiFont(23.0f, juce::Font::bold));
    g.drawText("S",20,7,35,44,juce::Justification::centred);
    g.setColour(juce::Colour(0xff202023)); g.fillRect(68,6,73,48);
    g.setColour(juce::Colour(0xff0f0f12));
    g.fillRect(86,20,25,2); g.fillRect(86,27,25,2); g.fillRect(86,34,25,2);
    g.setColour(juce::Colour(0xff3e3d43)); g.drawRect(68.0f,6.0f,73.0f,48.0f,1.0f);
    g.setColour(juce::Colour(0xff141416)); g.fillRect(143,7,245,47);
    g.setColour(muted); g.setFont(uiFont(16.0f,juce::Font::italic));
    g.drawText(selectedPresetName,162,10,205,39,juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xff29292d)); g.fillRect(390,7,43,47);
    g.fillRect(435,7,72,47);
    // Small up/down selectors and a save/floppy outline replace text stand-ins.
    juce::Path upArrow; upArrow.startNewSubPath(406.0f, 29.0f); upArrow.lineTo(412.0f, 21.0f); upArrow.lineTo(418.0f, 29.0f);
    g.setColour(muted); g.strokePath(upArrow, juce::PathStrokeType(1.5f));
    juce::Path downArrow; downArrow.startNewSubPath(406.0f, 35.0f); downArrow.lineTo(412.0f, 43.0f); downArrow.lineTo(418.0f, 35.0f);
    g.strokePath(downArrow, juce::PathStrokeType(1.5f));
    g.setColour(muted); g.drawRoundedRectangle(455.0f, 14.0f, 29.0f, 31.0f, 2.5f, 1.4f);
    g.setColour(muted.withAlpha(0.8f)); g.fillRect(461,15,17,8); g.fillRect(460,34,19,8);
    g.setColour(juce::Colour(0xff1b1a20)); g.fillRect(508,6,471,48);
    g.setColour(juce::Colour(0xffa36bff).withAlpha(0.22f));
    g.fillRect(508,6,471,2);
    const auto logoFont = uiFont(27.0f, juce::Font::bold);
    g.setFont(logoFont);
    // Use fixed integer bounds instead of measuring glyph widths: this avoids
    // deprecated text-width APIs and float-to-int overload ambiguity on MSVC.
    g.setColour(juce::Colour(0xffb9a0ff).withAlpha(0.23f));
    g.drawText("metaTune", 509, 7, 471, 47, juce::Justification::centred);
    g.setColour(juce::Colour(0xffe2d5ff));
    g.drawText("metaTune", 508, 6, 471, 48, juce::Justification::centred);

    // Icon-only toolbar with consistent vector strokes.
    auto drawUndoIcon = [&g](float x, bool redo)
    {
        juce::Path arrow;
        if (!redo)
        {
            arrow.startNewSubPath(x + 23.0f, 18.0f);
            arrow.cubicTo(x + 11.0f, 18.0f, x + 10.0f, 33.0f, x + 22.0f, 34.0f);
            arrow.lineTo(x + 29.0f, 34.0f);
            arrow.startNewSubPath(x + 15.0f, 13.0f); arrow.lineTo(x + 9.0f, 18.0f); arrow.lineTo(x + 15.0f, 23.0f);
        }
        else
        {
            arrow.startNewSubPath(x + 9.0f, 18.0f);
            arrow.cubicTo(x + 21.0f, 18.0f, x + 22.0f, 33.0f, x + 10.0f, 34.0f);
            arrow.lineTo(x + 3.0f, 34.0f);
            arrow.startNewSubPath(x + 17.0f, 13.0f); arrow.lineTo(x + 23.0f, 18.0f); arrow.lineTo(x + 17.0f, 23.0f);
        }
        g.strokePath(arrow, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    };
    g.setColour(muted); drawUndoIcon(986.0f, false); drawUndoIcon(1031.0f, true);
    juce::Path sync;
    sync.addCentredArc(1104.0f, 29.0f, 10.0f, 10.0f, 0.0f, 0.5f, 4.4f, true);
    g.strokePath(sync, juce::PathStrokeType(1.5f));
    juce::Path syncTip; syncTip.startNewSubPath(1092.0f,20.0f); syncTip.lineTo(1100.0f,19.0f); syncTip.lineTo(1098.0f,26.0f);
    g.strokePath(syncTip, juce::PathStrokeType(1.5f));
    g.setFont(uiFont(17.0f,juce::Font::bold));
    g.drawText(activeB ? "A  >  [B]" : "[A]  >  B",1130,7,115,45,juce::Justification::centred);
    g.setColour(muted); g.setFont(uiFont(15.0f));
    g.drawRoundedRectangle(1290.0f, 17.0f, 20.0f, 17.0f, 2.0f, 1.3f);
    g.drawLine(1295.0f, 34.0f, 1295.0f, 38.0f, 1.3f);
    g.drawText("?",1290,14,20,19,juce::Justification::centred);

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
    // Orb art is expensive to build (particle grain and plasma texture), so rasterize it
    // once at design resolution and composite it during subsequent repaints.
    static const juce::Image orbArtwork = createOrbArtwork();
    g.drawImageAt(orbArtwork, 185, 85);

    // Left group selector rail
    g.setColour(juce::Colour(0xff18181b)); g.fillRoundedRectangle(-15,150,73,430,22.0f);
    g.setColour(purple); g.setFont(uiFont(15.0f,juce::Font::bold));
    g.drawText("NOTES",5,170,48,40,juce::Justification::centred);
    g.setColour(muted); g.setFont(uiFont(19.0f,juce::Font::bold));
    for(int i=0;i<4;++i)
    {
        const float rowY = 224.0f + i * 91.0f;
        if (activeNoteGroup == i)
        {
            g.setColour(juce::Colour(0xff2b2535));
            g.fillRoundedRectangle(1.0f, rowY, 43.0f, 49.0f, 9.0f);
            g.setColour(purple);
            g.fillRoundedRectangle(1.0f, rowY + 7.0f, 3.0f, 35.0f, 1.5f);
        }
        g.setColour(activeNoteGroup == i ? text : muted);
        g.drawText(juce::String(i+1),13,(int)(rowY + 4.0f),38,42,juce::Justification::centred);
    }
    g.setColour(purple); g.setFont(uiFont(14.0f,juce::Font::bold));
    for(int i=0;i<4;++i) g.drawText(i == activeNoteGroup ? "1" : "0",30,206+i*91,24,18,juce::Justification::centred);

    // Doubler dot from the guide: drag left/right for width, up/down for wet/dry.
    const float doublerDotX = 120.0f + doublerWidth * 100.0f;
    const float doublerDotY = 620.0f - doublerMix * 90.0f;
    g.setColour(juce::Colour(0xff5a5a5d));
    g.fillEllipse(doublerDotX - 20.0f, doublerDotY - 20.0f, 40.0f, 40.0f);
    g.setColour(juce::Colour(0xff8b8b8e));
    g.drawEllipse(doublerDotX - 20.0f, doublerDotY - 20.0f, 40.0f, 40.0f, 1.5f);
    g.setColour(juce::Colour(0xffb9a2ff).withAlpha(0.22f));
    g.fillEllipse(doublerDotX - 12.0f, doublerDotY - 12.0f, 24.0f, 24.0f);
    g.setColour(juce::Colour(0xffb8b5bf));
    g.fillEllipse(doublerDotX - 8.0f, doublerDotY - 8.0f, 16.0f, 16.0f);
    g.setColour(juce::Colour(0xff3c3b42)); g.drawLine(0.0f, 673.0f, 1366.0f, 673.0f, 1.0f);

    // Lower selector strip
    g.setColour(juce::Colour(0xff27272b)); g.fillRect(0,676,1366,57);
    g.setColour(muted); g.setFont(uiFont(15.0f,juce::Font::bold));
    g.drawText("Select",30,688,95,32,juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xff18181b)); g.fillRoundedRectangle(298,686,310,35,17.0f);
    // The remaining part of the row exposes working Major/Hold/Minor/Hold selectors.
    const juce::Rectangle<float> majorArea(451.0f, 687.0f, 52.0f, 33.0f);
    const juce::Rectangle<float> majorHoldArea(503.0f, 687.0f, 26.0f, 33.0f);
    const juce::Rectangle<float> minorArea(529.0f, 687.0f, 52.0f, 33.0f);
    const juce::Rectangle<float> minorHoldArea(581.0f, 687.0f, 27.0f, 33.0f);
    if (scale.getSelectedItemIndex() == 1) { g.setColour(purple.withAlpha(0.34f)); g.fillRoundedRectangle(majorArea, 10.0f); }
    if (majorHold) { g.setColour(pink.withAlpha(0.24f)); g.fillRoundedRectangle(majorHoldArea, 8.0f); }
    if (scale.getSelectedItemIndex() == 2) { g.setColour(purple.withAlpha(0.34f)); g.fillRoundedRectangle(minorArea, 10.0f); }
    if (minorHold) { g.setColour(pink.withAlpha(0.24f)); g.fillRoundedRectangle(minorHoldArea, 8.0f); }
    g.setColour(muted); g.setFont(uiFont(12.0f, juce::Font::bold));
    g.drawText("Major", 451, 687, 52, 33, juce::Justification::centred);
    g.drawText("H", 503, 687, 26, 33, juce::Justification::centred);
    g.drawText("Minor", 529, 687, 52, 33, juce::Justification::centred);
    g.drawText("H", 581, 687, 27, 33, juce::Justification::centred);

    g.setColour(juce::Colour(0xff09090a)); g.fillRoundedRectangle(685,684,657,39,19.0f);
    g.setColour(juce::Colour(0xff35343b)); g.drawRoundedRectangle(685,684,657,39,19.0f,1.0f);
    const float modeCenters[] = { 713.0f, 780.0f, 850.0f, 921.0f };
    const float modeWidths[] = { 49.0f, 66.0f, 65.0f, 61.0f };
    const char* modeNames[] = { "None", "Short", "Mid", "Long" };
    for (int i = 0; i < 4; ++i)
    {
        if (selectedCorrectionTime == i)
        {
            g.setColour(juce::Colour(0xffd58acb).withAlpha(0.28f));
            g.fillRoundedRectangle(modeCenters[i] - modeWidths[i] * 0.5f, 687.0f,
                                   modeWidths[i], 32.0f, 15.0f);
        }
        g.setColour(i == selectedCorrectionTime ? juce::Colour(0xffffbfec) : muted);
        g.setFont(uiFont(12.0f, juce::Font::bold));
        g.drawText(modeNames[i], (int)(modeCenters[i] - modeWidths[i] * 0.5f), 687,
                   (int)modeWidths[i], 32, juce::Justification::centred);
    }

    // Heatmaps
    g.setColour(juce::Colour(0xff1d1d20)); g.fillRect(0,734,1366,53);
    g.setColour(juce::Colour(0xff0b0b0d)); g.fillRect(160,738,1052,45);
    g.setColour(juce::Colour(0xff303036)); g.drawLine(160.0f,760.0f,1212.0f,760.0f,1.0f);
    g.setColour(muted); g.setFont(uiFont(14.0f,juce::Font::bold));
    g.drawText("Input",32,736,105,24,juce::Justification::centredLeft);
    g.drawText("Output",32,760,105,24,juce::Justification::centredLeft);
    if (showHeatMaps)
    {
        for(int i=0;i<100;++i) {
            float x=165.0f+i*10.2f;
            auto col=(i%17<4) ? purple : ((i%13<3) ? cyan : juce::Colour(0xff39343f));
            g.setColour(col.withAlpha((i%5==0)?0.95f:0.45f)); g.fillRect(x,739.0f,7.0f,19.0f);
            g.setColour(col.withAlpha((i%7==0)?0.95f:0.30f)); g.fillRect(x+2.0f,762.0f,6.0f,18.0f);
        }
    }
    else
    {
        g.setColour(juce::Colour(0xff29282e));
        g.drawText("Heatmaps hidden", 390, 744, 260, 30, juce::Justification::centred);
    }

    // Tone-volume / scale control strip at the left of the keyboard.
    g.setColour(juce::Colour(0xff171719)); g.fillRect(0,788,165,287);
    g.setColour(juce::Colour(0xff38383d)); g.drawLine(0.0f,788.0f,1366.0f,788.0f,1.0f);
    g.setColour(muted); g.setFont(uiFont(15.0f,juce::Font::bold));
    g.drawText("Tone Volume",24,801,125,25,juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xff070708)); g.fillRoundedRectangle(10,833,145,19,9.0f);
    g.setColour(juce::Colour(0xff713a46)); g.fillRoundedRectangle(14.0f,837.0f,137.0f * toneVolume,11.0f,5.0f);
    g.setColour(juce::Colour(0xffd0a3b9)); g.fillRect(14.0f + 137.0f * toneVolume,834.0f,3.0f,17.0f);
    g.setColour(juce::Colour(0xff252329)); g.fillRoundedRectangle(12,875,141,169,15.0f);
    static const char* pitchNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    g.setColour(juce::Colour(0xffe7d9ef)); g.setFont(uiFont(50.0f,juce::Font::bold));
    g.drawText(pitchNames[juce::jlimit(0, 11, selectedNoteIndex)],12,891,141,58,juce::Justification::centred);
    g.setColour(muted); g.setFont(uiFont(16.0f,juce::Font::bold));
    const juce::String currentScaleName = scale.getSelectedItemIndex() == 1 ? "major" :
                                          (scale.getSelectedItemIndex() == 2 ? "minor" : "custom");
    g.drawText(currentScaleName,15,943,135,22,juce::Justification::centred);
    g.drawText("hover to",15,969,135,22,juce::Justification::centred);
    g.drawText("set scale",15,990,135,22,juce::Justification::centred);

    // Keyboard and right status rail
    drawPiano(g,{165,788,1047,287});
    g.setColour(juce::Colour(0xff222226)); g.fillRect(1212,734,154,341);
    g.setColour(showHeatMaps ? purple : muted); g.setFont(uiFont(14.0f,juce::Font::bold));
    g.drawText("In / Out",1225,741,130,24,juce::Justification::centredLeft);
    g.drawText(showHeatMaps ? "HeatMaps ON" : "HeatMaps OFF",1225,761,130,24,juce::Justification::centredLeft);
    g.setColour(showDetectedNotes ? purple : muted);
    g.drawText("Detected",1225,800,130,24,juce::Justification::centredLeft);
    g.drawText(showDetectedNotes ? "Notes ON" : "Notes OFF",1225,820,130,24,juce::Justification::centredLeft);
    g.setColour(globalBypass ? pink : muted);
    g.drawText("Global",1225,880,130,24,juce::Justification::centredLeft);
    g.drawText(globalBypass ? "Bypassed" : "Bypass",1225,900,130,24,juce::Justification::centredLeft);
    g.setColour(muted); g.drawText("Pitch Reference",1225,958,130,24,juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xff070708)); g.fillRoundedRectangle(1218,987,140,19,10.0f);
    const float referenceT = juce::jlimit(0.0f, 1.0f,
        (pitchReferenceHz - pitchReferenceMinHz) / (pitchReferenceMaxHz - pitchReferenceMinHz));
    g.setColour(juce::Colour(0xffbdb4ce));
    g.fillRoundedRectangle(1218.0f + 136.0f * referenceT, 987.0f, 4.0f, 19.0f, 2.0f);
    g.setColour(text); g.drawText(juce::String(pitchReferenceHz, 1) + " Hz",1220,1015,136,25,juce::Justification::centred);
    g.setColour(juce::Colour(0xff202024)); g.fillRect(0,1050,1366,30);
    const bool chromaticActive = scale.getSelectedItemIndex() == 0;
    if (chromaticActive)
    {
        g.setColour(purple.withAlpha(0.22f));
        g.fillRoundedRectangle(10.0f, 1051.0f, 126.0f, 25.0f, 6.0f);
    }
    g.setColour(chromaticActive ? juce::Colour(0xffd9c0ff) : purple);
    g.setFont(uiFont(15.0f, juce::Font::bold));
    g.drawText("Chromatic", 24, 1052, 112, 26, juce::Justification::centredLeft);
}


void MetaTuneInspiredAudioProcessorEditor::setRootNote(int noteIndex)
{
    const int next = juce::jlimit(0, 11, noteIndex);
    if (next != root.getSelectedItemIndex())
    {
        undoState = processor.parameters.copyState();
        hasUndoState = true;
        hasRedoState = false;
    }
    selectedNoteIndex = next;
    root.setSelectedItemIndex(selectedNoteIndex, juce::sendNotificationSync);
    repaint();
}

void MetaTuneInspiredAudioProcessorEditor::setScaleMode(int modeIndex)
{
    const int index = juce::jlimit(0, 2, modeIndex);
    if (index != 0)
        lastNonChromaticScaleMode = index;
    if (scale.getSelectedItemIndex() != index)
    {
        undoState = processor.parameters.copyState();
        hasUndoState = true;
        hasRedoState = false;
        scale.setSelectedItemIndex(index, juce::sendNotificationSync);
    }
    repaint();
}

void MetaTuneInspiredAudioProcessorEditor::openHeaderMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader("Scale");
    menu.addItem(1000, "Chromatic", true, scale.getSelectedItemIndex() == 0);
    menu.addItem(1001, "Major", true, scale.getSelectedItemIndex() == 1);
    menu.addItem(1002, "Minor", true, scale.getSelectedItemIndex() == 2);
    menu.addSeparator();
    menu.addSectionHeader("Root note");
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    for (int i = 0; i < 12; ++i)
        menu.addItem(2000 + i, names[i], true, i == selectedNoteIndex);
    menu.addSeparator();
    menu.addItem(3000, "Reset controls");

    juce::Component::SafePointer<MetaTuneInspiredAudioProcessorEditor> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options(), [safeThis](int result)
    {
        if (safeThis == nullptr || result == 0) return;
        if (result >= 1000 && result <= 1002)
            safeThis->setScaleMode(result - 1000);
        else if (result >= 2000 && result <= 2011)
            safeThis->setRootNote(result - 2000);
        else if (result == 3000)
        {
            safeThis->speed.setValue(30.0, juce::sendNotificationSync);
            safeThis->amount.setValue(100.0, juce::sendNotificationSync);
            safeThis->sustain.setValue(0.0, juce::sendNotificationSync);
            safeThis->setScaleMode(0);
            safeThis->setRootNote(4);
            safeThis->majorHold = false;
            safeThis->minorHold = false;
            safeThis->selectedCorrectionTime = 1;
            safeThis->globalBypass = false;
            safeThis->showHeatMaps = true;
            safeThis->showDetectedNotes = true;
            safeThis->pitchReferenceHz = 440.0f;
            safeThis->selectedPresetName = "No preset selected";
            safeThis->stabilizer.setEnabled(!safeThis->lowLatency.getToggleState());
            safeThis->repaint();
        }
    });
}

void MetaTuneInspiredAudioProcessorEditor::savePresetToFile()
{
    if (saveChooser != nullptr) return;
    saveChooser = std::make_shared<juce::FileChooser>("Save MetaTune Inspired preset",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("MetaTuneInspired.mti"), "*.mti");
    auto chooser = saveChooser;
    juce::Component::SafePointer<MetaTuneInspiredAudioProcessorEditor> safeThis(this);
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis, chooser](const juce::FileChooser& fc)
        {
            if (safeThis == nullptr) return;
            const auto file = fc.getResult();
            if (file != juce::File{})
            {
                juce::MemoryBlock block;
                safeThis->processor.getStateInformation(block);
                if (!file.replaceWithData(block.getData(), block.getSize()))
                    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Preset save failed",
                                                           "The selected preset file could not be written.");
                else
                {
                    safeThis->selectedPresetName = file.getFileNameWithoutExtension();
                    safeThis->repaint();
                    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Preset saved",
                                                           "Preset state saved successfully.");
                }
            }
            safeThis->saveChooser.reset();
        });
}


void MetaTuneInspiredAudioProcessorEditor::loadPresetFromFile()
{
    if (loadChooser != nullptr) return;
    loadChooser = std::make_shared<juce::FileChooser>("Load MetaTune Inspired preset",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory), "*.mti");
    auto chooser = loadChooser;
    juce::Component::SafePointer<MetaTuneInspiredAudioProcessorEditor> safeThis(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis, chooser](const juce::FileChooser& fc)
        {
            if (safeThis == nullptr) return;
            const auto file = fc.getResult();
            if (file.existsAsFile())
            {
                juce::MemoryBlock block;
                if (file.loadFileAsData(block))
                {
                    safeThis->processor.setStateInformation(block.getData(), (int)block.getSize());
                    safeThis->selectedPresetName = file.getFileNameWithoutExtension();
                    safeThis->selectedNoteIndex = juce::jlimit(0, 11, safeThis->root.getSelectedItemIndex());
                    if (safeThis->scale.getSelectedItemIndex() != 0)
                        safeThis->lastNonChromaticScaleMode = safeThis->scale.getSelectedItemIndex();
                    if (safeThis->lowLatency.getToggleState() && safeThis->stabilizer.getToggleState())
                        safeThis->stabilizer.setToggleState(false, juce::sendNotificationSync);
                    safeThis->stabilizer.setEnabled(!safeThis->lowLatency.getToggleState());
                    safeThis->repaint();
                }
                else
                {
                    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                        "Preset load failed", "The selected file could not be read as a MetaTune Inspired preset.");
                }
            }
            safeThis->loadChooser.reset();
        });
}

void MetaTuneInspiredAudioProcessorEditor::mouseDrag(const juce::MouseEvent& event)
{
    if (orbDragMode == 0)
        return;

    const float sx = getWidth() / 1366.0f;
    const float sy = getHeight() / 1080.0f;
    const float designX = event.position.x / juce::jmax(0.0001f, sx);
    const float designY = event.position.y / juce::jmax(0.0001f, sy);

    if (orbDragMode == 3)
    {
        doublerWidth = juce::jlimit(0.0f, 1.0f, doublerDragStartWidth + (designX - orbDragStartX) / 100.0f);
        doublerMix = juce::jlimit(0.0f, 1.0f, doublerDragStartMix + (orbDragStartY - designY) / 90.0f);
        repaint();
        return;
    }
    if (orbDragMode == 4)
    {
        toneVolume = juce::jlimit(0.0f, 1.0f, (designX - 10.0f) / 145.0f);
        repaint();
        return;
    }
    if (orbDragMode == 5)
    {
        const float t = juce::jlimit(0.0f, 1.0f, (designX - 1218.0f) / 140.0f);
        pitchReferenceHz = pitchReferenceMinHz + t * (pitchReferenceMaxHz - pitchReferenceMinHz);
        pitchReferenceHz = (float)(juce::roundToInt(pitchReferenceHz * 10.0f) / 10.0f);
        repaint();
        return;
    }

    const float delta = (orbDragStartY - designY) * 0.42f;
    if (orbDragMode == 1)
        speed.setValue(juce::jlimit(0.0f, 100.0f, orbDragStartValue + delta), juce::sendNotificationSync);
    else if (orbDragMode == 2)
        amount.setValue(juce::jlimit(0.0f, 100.0f, orbDragStartValue + delta), juce::sendNotificationSync);
}

void MetaTuneInspiredAudioProcessorEditor::mouseUp(const juce::MouseEvent&)
{
    orbDragMode = 0;
}

void MetaTuneInspiredAudioProcessorEditor::mouseDown(const juce::MouseEvent& event)
{
    const float sx = getWidth() / 1366.0f;
    const float sy = getHeight() / 1080.0f;
    const float x = event.position.x / juce::jmax(0.0001f, sx);
    const float y = event.position.y / juce::jmax(0.0001f, sy);

    // Toolbar hit areas: menu, save, undo, redo, sync-to-detected-note, A/B and help.
    if (y >= 0.0f && y < 59.0f)
    {
        if (x >= 68.0f && x < 142.0f) { openHeaderMenu(); return; }
        if (x >= 143.0f && x < 389.0f) { loadPresetFromFile(); return; }
        if (x >= 390.0f && x < 434.0f)
        {
            const int step = y < 30.0f ? 1 : -1;
            setRootNote((selectedNoteIndex + step + 12) % 12);
            return;
        }
        if (x >= 435.0f && x < 508.0f) { savePresetToFile(); return; }
        if (x >= 985.0f && x < 1030.0f)
        {
            if (hasUndoState)
            {
                redoState = processor.parameters.copyState();
                hasRedoState = true;
                processor.parameters.replaceState(undoState.createCopy());
                hasUndoState = false;
            }
            else juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Undo", "Change a knob or select a scale first.");
            repaint(); return;
        }
        if (x >= 1030.0f && x < 1078.0f)
        {
            if (hasRedoState)
            {
                undoState = processor.parameters.copyState();
                hasUndoState = true;
                processor.parameters.replaceState(redoState.createCopy());
                hasRedoState = false;
            }
            else juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Redo", "There is no action to redo yet.");
            repaint(); return;
        }
        if (x >= 1078.0f && x < 1128.0f)
        {
            const float hz = processor.getDetectedHz();
            if (hz > 0.0f)
            {
                const int midi = (int)std::round(69.0f + 12.0f * std::log2(hz / pitchReferenceHz));
                setRootNote((midi % 12 + 12) % 12);
            }
            else juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Sync", "Play a pitched signal first, then Sync to detected pitch.");
            return;
        }
        if (x >= 1128.0f && x < 1248.0f)
        {
            const auto current = processor.parameters.copyState();
            if (x < 1188.0f)
            {
                if (activeB) { slotB = current.createCopy(); if (slotA.isValid()) processor.parameters.replaceState(slotA.createCopy()); }
                else slotA = current.createCopy();
                activeB = false;
            }
            else
            {
                if (!activeB) slotA = current.createCopy();
                if (slotB.isValid()) processor.parameters.replaceState(slotB.createCopy());
                activeB = true;
            }
            repaint(); return;
        }
        if (x >= 1250.0f) { juce::URL("https://docs.slatedigital.com/MetaTune/MetaTune.html").launchInDefaultBrowser(); return; }
        return;
    }

    // Doubler dot: the documented horizontal/vertical gesture changes width and wet/dry.
    const float dotX = 120.0f + doublerWidth * 100.0f;
    const float dotY = 620.0f - doublerMix * 90.0f;
    if (std::abs(x - dotX) <= 23.0f && std::abs(y - dotY) <= 23.0f)
    {
        orbDragMode = 3;
        orbDragStartX = x;
        orbDragStartY = y;
        doublerDragStartWidth = doublerWidth;
        doublerDragStartMix = doublerMix;
        return;
    }

    // The guide documents orb gestures as alternate ways to adjust Speed and Amount.
    // Drag the small inner globe for Speed; drag the frosted outer shell for Amount.
    const float orbDx = x - 470.0f;
    const float orbDy = y - 365.0f;
    const float orbRadius = std::sqrt(orbDx * orbDx + orbDy * orbDy);
    if (orbRadius <= 101.0f)
    {
        orbDragMode = 1;
        orbDragStartY = y;
        orbDragStartValue = (float)speed.getValue();
        undoState = processor.parameters.copyState();
        hasUndoState = true;
        hasRedoState = false;
        return;
    }
    if (orbRadius >= 184.0f && orbRadius <= 235.0f)
    {
        orbDragMode = 2;
        orbDragStartY = y;
        orbDragStartValue = (float)amount.getValue();
        undoState = processor.parameters.copyState();
        hasUndoState = true;
        hasRedoState = false;
        return;
    }

    // The left note-bank rail is selectable and highlights the active bank.
    if (x >= 0.0f && x < 58.0f && y >= 220.0f && y < 590.0f)
    {
        activeNoteGroup = juce::jlimit(0, 3, (int)((y - 224.0f) / 91.0f));
        repaint();
        return;
    }

    // Clicking the note labels selects their chromatic note. Clicking a black or white
    // key selects the actual key under the pointer, rather than simply using the header column.
    if (x >= 165.0f && x < 1212.0f && y >= 788.0f && y < 1050.0f)
    {
        const float keyboardX = x - 165.0f;
        const float keyboardW = 1212.0f - 165.0f;
        const float semitoneW = keyboardW / 12.0f;
        int note = 0;
        if (y < 840.0f)
        {
            note = (int)(keyboardX / semitoneW);
        }
        else
        {
            const int blackNotes[] = { 1, 3, 6, 8, 10 };
            const float blackW = semitoneW * 1.08f;
            const float keyTop = 840.0f;
            const float blackBottom = keyTop + (1075.0f - keyTop) * 0.59f;
            bool hitBlack = false;
            if (y < blackBottom)
            {
                for (const int blackNote : blackNotes)
                {
                    const float centre = ((float)blackNote + 0.5f) * semitoneW;
                    if (keyboardX >= centre - blackW * 0.5f && keyboardX <= centre + blackW * 0.5f)
                    {
                        note = blackNote;
                        hitBlack = true;
                        break;
                    }
                }
            }
            if (! hitBlack)
            {
                const float naturalWidth = keyboardW / 7.0f;
                const int naturalKey = juce::jlimit(0, 6, (int)(keyboardX / naturalWidth));
                const int naturalNotes[] = { 0, 2, 4, 5, 7, 9, 11 };
                note = naturalNotes[naturalKey];
            }
        }
        setRootNote(juce::jlimit(0, 11, note));
        return;
    }

    // Heatmaps, detected-note display, bypass, and the pitch-reference slider are real hit targets.
    if (x >= 1212.0f && y >= 734.0f && y < 790.0f) { showHeatMaps = !showHeatMaps; repaint(); return; }
    if (x >= 1212.0f && y >= 790.0f && y < 865.0f) { showDetectedNotes = !showDetectedNotes; repaint(); return; }
    if (x >= 1212.0f && y >= 865.0f && y < 940.0f) { globalBypass = !globalBypass; repaint(); return; }
    if (x >= 1212.0f && y >= 940.0f && y < 1045.0f)
    {
        const float t = juce::jlimit(0.0f, 1.0f, (x - 1218.0f) / 140.0f);
        pitchReferenceHz = pitchReferenceMinHz + t * (pitchReferenceMaxHz - pitchReferenceMinHz);
        pitchReferenceHz = (float)(juce::roundToInt(pitchReferenceHz * 10.0f) / 10.0f);
        orbDragMode = 5;
        repaint(); return;
    }

    // Selector row actions not already covered by the scale/root dropdown components.
    if (y >= 676.0f && y < 733.0f)
    {
        if (x >= 451.0f && x < 503.0f) { setScaleMode(1); return; }
        if (x >= 503.0f && x < 529.0f) { majorHold = !majorHold; repaint(); return; }
        if (x >= 529.0f && x < 581.0f) { setScaleMode(2); return; }
        if (x >= 581.0f && x < 608.0f) { minorHold = !minorHold; repaint(); return; }
        if (x >= 685.0f && x < 970.0f)
        {
            if (x < 755.0f) selectedCorrectionTime = 0;
            else if (x < 815.0f) selectedCorrectionTime = 1;
            else if (x < 885.0f) selectedCorrectionTime = 2;
            else selectedCorrectionTime = 3;
            repaint(); return;
        }
    }

    // Tone-volume bar is clickable. This remains a UI control until a tone-volume
    // parameter is added to the processor; it is not falsely mapped to another parameter.
    if (x >= 0.0f && x < 165.0f && y >= 822.0f && y < 860.0f)
    {
        toneVolume = juce::jlimit(0.0f, 1.0f, (x - 10.0f) / 145.0f);
        orbDragMode = 4;
        repaint(); return;
    }

    // Clicking the left scale card opens the same functional root/scale chooser.
    if (x < 165.0f && y >= 875.0f && y < 1045.0f) { openHeaderMenu(); return; }
    if (x < 165.0f && y >= 1045.0f && y < 1080.0f)
    {
        setScaleMode(scale.getSelectedItemIndex() == 0 ? lastNonChromaticScaleMode : 0);
        return;
    }
}

void MetaTuneInspiredAudioProcessorEditor::resized()
{
    const float sx=getWidth()/1366.0f, sy=getHeight()/1080.0f;
    const auto px = [sx](float value) { return juce::roundToInt(value * sx); };
    const auto py = [sy](float value) { return juce::roundToInt(value * sy); };
    speed.setBounds(px(955), py(120), px(225), py(225));
    sustain.setBounds(px(1180), py(205), px(140), py(140));
    amount.setBounds(px(1090), py(395), px(225), py(225));
    speedLabel.setBounds(px(960), py(320), px(220), py(25));
    amountLabel.setBounds(px(1097), py(575), px(220), py(25));
    scale.setBounds(px(165), py(686), px(176), py(35));
    root.setBounds(px(350), py(686), px(100), py(35));
    stabilizer.setBounds(px(990), py(686), px(185), py(35));
    lowLatency.setBounds(px(1177), py(686), px(175), py(35));
    noteReadout.setBounds(px(360), py(365), px(230), py(70));
    status.setBounds(px(32), py(610), px(600), py(25));
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
        noteReadout.setVisible(showDetectedNotes);
        status.setText(juce::String(hz,1)+" Hz   -   correction "+juce::String(processor.getCorrectionCents(),1)+" cents",juce::dontSendNotification);
    } else {
        noteReadout.setText("",juce::dontSendNotification);
        noteReadout.setVisible(false);
        status.setText("Waiting for pitched input...",juce::dontSendNotification);
    }
}
