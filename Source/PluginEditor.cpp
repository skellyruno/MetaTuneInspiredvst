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
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff484052), orbCx - 54.0f, orbCy - 72.0f,
                                           juce::Colour(0xff0c0a12), orbCx + 228.0f, orbCy + 226.0f, true));
    g.fillEllipse(238.0f, 138.0f, 465.0f, 460.0f);

    // Brighter frosted glass volume. The reference has a clearly visible cloudy
    // silver shell, so build several broader and stronger translucent edge layers.
    // Wide low-opacity bloom is followed by progressively sharper white layers.
    // This gives the shell a luminous halo instead of the previous charcoal rim.
    g.setColour(juce::Colour(0xffeee9fa).withAlpha(0.24f));
    g.drawEllipse(220.0f, 120.0f, 501.0f, 496.0f, 34.0f);
    g.setColour(juce::Colour(0xfff4efff).withAlpha(0.30f));
    g.drawEllipse(228.0f, 128.0f, 485.0f, 480.0f, 27.0f);
    g.setColour(juce::Colour(0xfff5f1ff).withAlpha(0.38f));
    g.drawEllipse(234.0f, 134.0f, 473.0f, 468.0f, 20.0f);
    g.setColour(juce::Colour(0xfff8f4ff).withAlpha(0.47f));
    g.drawEllipse(239.0f, 139.0f, 463.0f, 458.0f, 13.0f);
    g.setColour(juce::Colour(0xfff0eaff).withAlpha(0.62f));
    g.drawEllipse(244.0f, 144.0f, 453.0f, 448.0f, 6.0f);

    // A cool-white top-left haze and a subtle violet lower rim lift the sphere
    // off the background rather than letting it disappear into black.
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xfff0eaff).withAlpha(0.30f),
                                           345.0f, 205.0f,
                                           juce::Colour(0xffa77cff).withAlpha(0.0f),
                                           555.0f, 415.0f, true));
    g.fillEllipse(230.0f, 130.0f, 480.0f, 475.0f);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff82dfff).withAlpha(0.15f),
                                           620.0f, 425.0f,
                                           juce::Colour(0xff82dfff).withAlpha(0.0f),
                                           465.0f, 368.0f, true));
    g.fillEllipse(238.0f, 138.0f, 465.0f, 460.0f);
    for (int i = 22; i >= 1; --i)
    {
        const float grow = (float)i * 1.45f;
        const float alpha = 0.095f + 0.0090f * (22 - i);
        g.setColour(juce::Colour(0xffeeeaf6).withAlpha(alpha));
        g.drawEllipse(241.0f - grow, 141.0f - grow, 459.0f + grow * 2.0f,
                      454.0f + grow * 2.0f, 2.0f + (float)(i % 3) * 0.35f);
    }

    // Static deterministic noise lets the frosted shell look organic without flickering.
    // Radial jitter creates dense white particles and a smaller number of violet/cyan sparks.
    for (int i = 0; i < 8600; ++i)
    {
        const unsigned int h1 = (unsigned int)i * 747796405u + 2891336453u;
        const unsigned int h2 = (unsigned int)i * 277803737u + 1013904223u;
        const float a = (float)i * 2.39996323f + (float)(h1 & 1023u) * 0.00019f;
        const float rj = (float)(h2 % 10000u) / 10000.0f;
        const float cluster = (float)((h1 >> 8u) % 1000u) / 1000.0f;
        const float radius = 226.0f + (rj - 0.5f) * 36.0f + (cluster - 0.5f) * 12.0f;
        const float px = orbCx + std::cos(a) * radius;
        const float py = orbCy + std::sin(a) * radius * 0.982f;
        const float dot = (i % 17 == 0) ? 3.8f : ((i % 5 == 0) ? 2.35f : 1.15f);
        juce::Colour particle = juce::Colour(0xfff3eff9);
        if (i % 11 == 0) particle = juce::Colour(0xffd9c9f7);
        if (i % 37 == 0) particle = juce::Colour(0xff95dfff);
        const float angleLight = juce::jlimit(0.58f, 1.42f, 0.92f + 0.48f * -std::sin(a) + 0.12f * -std::cos(a));
        const float brightness = ((i % 13 == 0) ? 1.0f : ((i % 4 == 0) ? 0.92f : 0.66f)) * angleLight;
        g.setColour(particle.withAlpha(juce::jmin(0.98f, brightness)));
        g.fillEllipse(px, py, dot, dot);
    }

    // Fine drifting flecks spread through the shell's thickness: this breaks the
    // perfect circular edge and creates the luminous grain seen in the original.
    for (int i = 0; i < 3700; ++i)
    {
        const unsigned int h1 = (unsigned int)i * 2246822519u + 3266489917u;
        const unsigned int h2 = (unsigned int)i * 3266489917u + 668265263u;
        const float a = (float)i * 2.39996323f + (float)(h1 % 4096u) * 0.00021f;
        const float r = 211.0f + (float)(h2 % 10000u) * 0.0033f;
        const float px = orbCx + std::cos(a) * r;
        const float py = orbCy + std::sin(a) * r * 0.982f;
        const float size = (i % 23 == 0) ? 4.0f : ((i % 7 == 0) ? 2.5f : 1.1f);
        const auto tint = (i % 19 == 0) ? juce::Colour(0xffc7afff)
                         : ((i % 31 == 0) ? juce::Colour(0xffa9e6ff) : juce::Colour(0xffece7f4));
        const float alpha = (i % 17 == 0) ? 0.94f : ((i % 4 == 0) ? 0.64f : 0.32f);
        g.setColour(tint.withAlpha(alpha));
        g.fillEllipse(px, py, size, size);
    }

    // Fine highlight edge and a dark inner lens create depth, rather than a flat ring.
    g.setColour(juce::Colour(0xfffaf7ff).withAlpha(0.82f));
    g.drawEllipse(243.0f, 143.0f, 454.0f, 450.0f, 2.2f);
    g.setColour(juce::Colour(0xffeee8f7).withAlpha(0.45f));
    g.drawEllipse(247.0f, 147.0f, 446.0f, 442.0f, 1.4f);
    g.setColour(juce::Colour(0xff9d91b5).withAlpha(0.38f));
    g.drawEllipse(250.0f, 150.0f, 440.0f, 436.0f, 1.0f);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff282032), 390.0f, 260.0f,
                                           juce::Colour(0xff0b0911), 470.0f, 550.0f, true));
    g.fillEllipse(280.0f, 178.0f, 380.0f, 380.0f);

    // Violet halo - numerous soft strokes underneath a crisp neon ring.
    for (int i = 12; i >= 1; --i)
    {
        const float inset = (float)i * 1.8f;
        g.setColour(juce::Colour(0xffad82ff).withAlpha(0.064f * (13 - i)));
        g.drawEllipse(319.0f - inset, 215.0f - inset,
                      303.0f + inset * 2.0f, 302.0f + inset * 2.0f, 3.0f + (float)(i % 2));
    }
    g.setColour(juce::Colour(0xffa97cff).withAlpha(0.62f));
    g.drawEllipse(308.0f, 204.0f, 325.0f, 324.0f, 22.0f);
    g.setColour(juce::Colour(0xff965eff).withAlpha(0.76f));
    g.drawEllipse(314.0f, 210.0f, 313.0f, 312.0f, 15.0f);
    g.setColour(juce::Colour(0xffa77aff).withAlpha(0.98f));
    g.drawEllipse(319.0f, 215.0f, 303.0f, 302.0f, 7.8f);
    g.setColour(juce::Colour(0xffe9d9ff).withAlpha(0.92f));
    g.drawEllipse(323.0f, 219.0f, 295.0f, 294.0f, 1.7f);
    for (int bloom = 9; bloom >= 1; --bloom)
    {
        const float grow = (float)bloom * 2.0f;
        g.setColour(juce::Colour(0xffa875ff).withAlpha(0.035f * (10 - bloom)));
        g.drawEllipse(319.0f - grow, 215.0f - grow,
                      303.0f + grow * 2.0f, 302.0f + grow * 2.0f, 5.0f + (float)(bloom % 3));
    }
    g.setColour(juce::Colour(0xffaa7aff).withAlpha(0.78f));
    g.drawEllipse(319.0f, 215.0f, 303.0f, 302.0f, 5.0f);

    // Inner 2-tone plasma globe. A cloudy gradient, fine particle edge and curved energy seam
    // mimic the cyan-left / magenta-right glass surface from the reference screenshot.
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff9366a4), 411.0f, 294.0f,
                                           juce::Colour(0xff281936), 518.0f, 443.0f, true));
    g.fillEllipse(370.0f, 270.0f, 201.0f, 190.0f);

    // Overlapping radial colour fields form cloudy energy rather than a hard vertical split.
    // Each field fades to transparent and is clipped to the spherical glass lens.
    juce::Path innerGlassClip;
    innerGlassClip.addEllipse(372.0f, 272.0f, 197.0f, 186.0f);
    g.saveState();
    g.reduceClipRegion(innerGlassClip);

    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff31e8ff).withAlpha(0.96f),
                                           414.0f, 355.0f,
                                           juce::Colour(0xff4acde7).withAlpha(0.0f),
                                           482.0f, 355.0f, true));
    g.fillEllipse(365.0f, 293.0f, 155.0f, 150.0f);

    g.setGradientFill(juce::ColourGradient(juce::Colour(0xffff70df).withAlpha(0.94f),
                                           519.0f, 349.0f,
                                           juce::Colour(0xffb65cc9).withAlpha(0.0f),
                                           584.0f, 349.0f, true));
    g.fillEllipse(427.0f, 281.0f, 155.0f, 166.0f);

    g.setGradientFill(juce::ColourGradient(juce::Colour(0xffad97ff).withAlpha(0.79f),
                                           462.0f, 407.0f,
                                           juce::Colour(0xff6651d0).withAlpha(0.0f),
                                           462.0f, 473.0f, true));
    g.fillEllipse(393.0f, 348.0f, 153.0f, 112.0f);

    // Irregular translucent lobes create a marbled cyan/magenta plasma surface.
    // These overlapping patches deliberately avoid a clean half-and-half split.
    juce::Path cyanLobe;
    cyanLobe.startNewSubPath(382.0f, 313.0f);
    cyanLobe.cubicTo(402.0f, 290.0f, 439.0f, 301.0f, 452.0f, 328.0f);
    cyanLobe.cubicTo(461.0f, 347.0f, 450.0f, 372.0f, 429.0f, 389.0f);
    cyanLobe.cubicTo(407.0f, 406.0f, 388.0f, 385.0f, 383.0f, 359.0f);
    cyanLobe.cubicTo(379.0f, 340.0f, 375.0f, 325.0f, 382.0f, 313.0f);
    cyanLobe.closeSubPath();
    g.setColour(juce::Colour(0xff31dffb).withAlpha(0.62f));
    g.fillPath(cyanLobe);

    juce::Path magentaLobe;
    magentaLobe.startNewSubPath(477.0f, 295.0f);
    magentaLobe.cubicTo(505.0f, 288.0f, 541.0f, 318.0f, 547.0f, 346.0f);
    magentaLobe.cubicTo(555.0f, 379.0f, 526.0f, 398.0f, 508.0f, 382.0f);
    magentaLobe.cubicTo(488.0f, 363.0f, 501.0f, 343.0f, 485.0f, 327.0f);
    magentaLobe.cubicTo(474.0f, 316.0f, 464.0f, 303.0f, 477.0f, 295.0f);
    magentaLobe.closeSubPath();
    g.setColour(juce::Colour(0xffff79e2).withAlpha(0.62f));
    g.fillPath(magentaLobe);

    // A few hazy highlights deepen the glass without obscuring the lightning.
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xfff4b9ff).withAlpha(0.46f),
                                           491.0f, 316.0f,
                                           juce::Colour(0xfff4b9ff).withAlpha(0.0f),
                                           520.0f, 371.0f, true));
    g.fillEllipse(454.0f, 283.0f, 103.0f, 111.0f);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff9beaff).withAlpha(0.40f),
                                           410.0f, 375.0f,
                                           juce::Colour(0xff9beaff).withAlpha(0.0f),
                                           442.0f, 419.0f, true));
    g.fillEllipse(376.0f, 343.0f, 112.0f, 103.0f);

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
    for (int i = 0; i < 4100; ++i)
    {
        const unsigned int h = (unsigned int)i * 2246822519u + 3266489917u;
        const float px = 379.0f + (float)(h % 18300u) / 100.0f;
        const float py = 277.0f + (float)((h >> 7u) % 16900u) / 100.0f;
        const float dx = (px - 470.5f) / 99.0f;
        const float dy = (py - 365.0f) / 94.0f;
        if (dx * dx + dy * dy > 1.0f) continue;
        const float seamX = 470.5f + 12.0f * std::sin((py - 277.0f) * 0.034f)
                           + 5.0f * std::sin((py - 277.0f) * 0.077f);
        const bool left = px < seamX;
        auto cloud = left ? juce::Colour(0xff35dff8) : juce::Colour(0xffff7bdc);
        if (i % 9 == 0) cloud = juce::Colour(0xffe4c7ff);
        g.setColour(cloud.withAlpha((i % 9 == 0) ? 0.86f : 0.48f));
        const float dot = (i % 13 == 0) ? 2.4f : ((i % 3 == 0) ? 1.65f : 1.0f);
        g.fillEllipse(px, py, dot, dot);
    }
    g.restoreState();

    // Grainy white inner boundary with tinted lower-left and upper-right arcs.
    for (int i = 0; i < 3300; ++i)
    {
        const unsigned int h = (unsigned int)i * 747796405u + 2891336453u;
        const float a = (float)i * 2.39996323f + (float)(h % 1000u) * 0.00012f;
        const float rr = 96.0f + ((float)((h >> 10u) % 10000u) / 10000.0f - 0.5f) * 12.0f;
        const float px = 470.0f + std::cos(a) * rr;
        const float py = 365.0f + std::sin(a) * rr * 0.91f;
        juce::Colour edge = juce::Colour(0xfffff6ff);
        if (std::cos(a) < -0.28f) edge = juce::Colour(0xff75eaff);
        else if (std::cos(a) > 0.25f) edge = juce::Colour(0xffff91e3);
        const float alpha = (i % 11 == 0) ? 0.99f : ((i % 4 == 0) ? 0.78f : 0.43f);
        const float dot = (i % 7 == 0) ? 2.6f : 1.25f;
        g.setColour(edge.withAlpha(alpha));
        g.fillEllipse(px, py, dot, dot);
    }
    juce::Path innerRim;
    innerRim.addEllipse(370.0f, 270.0f, 201.0f, 190.0f);
    g.setColour(juce::Colour(0xfffff7ff).withAlpha(1.0f));
    g.strokePath(innerRim, juce::PathStrokeType(2.7f));
    g.setColour(juce::Colour(0xffe0cfff).withAlpha(0.66f));
    g.strokePath(innerRim, juce::PathStrokeType(8.0f));

    juce::Path plasma;
    plasma.startNewSubPath(395.0f, 383.0f);
    plasma.cubicTo(420.0f, 376.0f, 446.0f, 350.0f, 468.0f, 330.0f);
    plasma.cubicTo(491.0f, 308.0f, 514.0f, 303.0f, 531.0f, 313.0f);
    g.setColour(juce::Colour(0xfff0c7ff).withAlpha(0.73f));
    g.strokePath(plasma, juce::PathStrokeType(1.7f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    juce::Path filamentOne;
    filamentOne.startNewSubPath(394.0f, 356.0f);
    filamentOne.cubicTo(414.0f, 346.0f, 430.0f, 333.0f, 448.0f, 317.0f);
    filamentOne.cubicTo(466.0f, 302.0f, 483.0f, 299.0f, 497.0f, 304.0f);
    g.setColour(juce::Colour(0xff92e7ff).withAlpha(0.40f));
    g.strokePath(filamentOne, juce::PathStrokeType(1.1f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
    juce::Path filamentTwo;
    filamentTwo.startNewSubPath(409.0f, 409.0f);
    filamentTwo.cubicTo(437.0f, 425.0f, 473.0f, 428.0f, 502.0f, 413.0f);
    g.setColour(juce::Colour(0xffffa8e9).withAlpha(0.30f));
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
    g.setColour(juce::Colour(0xffc66dff).withAlpha(0.56f));
    g.fillPath(bolt, juce::AffineTransform::translation(1.2f, 1.2f));
    g.setColour(juce::Colour(0xfff0adff).withAlpha(0.98f));
    g.fillPath(bolt);

    // Final frosted perimeter bloom, drawn last so later inner-lens fills cannot dim it.
    for (int i = 12; i >= 1; --i)
    {
        const float spread = (float)i * 1.15f;
        const float a = 0.018f + 0.014f * (13 - i);
        g.setColour(juce::Colour(0xfff7f3ff).withAlpha(a));
        g.drawEllipse(243.0f - spread, 143.0f - spread,
                      454.0f + spread * 2.0f, 450.0f + spread * 2.0f, 2.1f);
    }
    g.setColour(juce::Colour(0xfff7f4ff).withAlpha(0.74f));
    g.drawEllipse(243.0f, 143.0f, 454.0f, 450.0f, 2.5f);
    g.setColour(juce::Colour(0xfffdfaff).withAlpha(0.56f));
    g.drawEllipse(246.0f, 146.0f, 448.0f, 444.0f, 1.1f);

    // Final high-energy frosted shell. Wider overlapping strokes create a visible
    // silver-white volume even over near-black backgrounds; sparse points preserve
    // the organic rather than perfectly geometric edge.
    for (int layer = 17; layer >= 1; --layer)
    {
        const float spread = (float)layer * 1.35f;
        const float alpha = 0.065f + 0.014f * (18 - layer);
        const float thickness = 3.0f + (float)(layer % 4) * 2.2f;
        g.setColour(juce::Colour(0xfff4efff).withAlpha(alpha));
        g.drawEllipse(242.0f - spread, 142.0f - spread,
                      456.0f + spread * 2.0f, 452.0f + spread * 2.0f, thickness);
    }
    g.setColour(juce::Colour(0xffeee8fa).withAlpha(0.52f));
    g.drawEllipse(240.0f, 140.0f, 460.0f, 456.0f, 15.0f);
    g.setColour(juce::Colour(0xfff8f4ff).withAlpha(0.76f));
    g.drawEllipse(243.0f, 143.0f, 454.0f, 450.0f, 6.0f);
    g.setColour(juce::Colour(0xfffffcff).withAlpha(1.0f));
    g.drawEllipse(246.0f, 146.0f, 448.0f, 444.0f, 2.4f);

    // Directional specular arcs make the shell feel frosted and illuminated,
    // not like a uniformly machined grey ring. The top-left catches the light.
    juce::Path upperShellGlint;
    upperShellGlint.addArc(242.0f, 142.0f, 456.0f, 452.0f, 3.30f, 5.25f, true);
    g.setColour(juce::Colour(0xfffffcff).withAlpha(0.26f));
    g.strokePath(upperShellGlint, juce::PathStrokeType(15.0f, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
    g.setColour(juce::Colour(0xfffffdff).withAlpha(0.76f));
    g.strokePath(upperShellGlint, juce::PathStrokeType(3.2f, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
    juce::Path lowerShellGlint;
    lowerShellGlint.addArc(244.0f, 144.0f, 452.0f, 448.0f, 0.58f, 1.85f, true);
    g.setColour(juce::Colour(0xffd8c8f5).withAlpha(0.24f));
    g.strokePath(lowerShellGlint, juce::PathStrokeType(7.0f, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));

    // Make the inner energy globe a separate transparent layer. This keeps the
    // frosted shell/rim cached while allowing the inner plasma to rotate by Speed.
    constexpr float coreCx = 285.0f, coreCy = 280.0f;
    constexpr float coreRx = 93.0f, coreRy = 88.0f;
    for (int py = (int)(coreCy - coreRy - 1.0f); py <= (int)(coreCy + coreRy + 1.0f); ++py)
        for (int px = (int)(coreCx - coreRx - 1.0f); px <= (int)(coreCx + coreRx + 1.0f); ++px)
        {
            const float nx = (px - coreCx) / coreRx;
            const float ny = (py - coreCy) / coreRy;
            if (nx * nx + ny * ny <= 1.0f)
                image.setPixelAt(px, py, juce::Colours::transparentBlack);
        }

    return image;
}

juce::Image createInnerOrbArtwork()
{
    constexpr int imageWidth = 240;
    constexpr int imageHeight = 220;
    juce::Image image(juce::Image::ARGB, imageWidth, imageHeight, true);
    image.clear(image.getBounds(), juce::Colours::transparentBlack);
    juce::Graphics g(image);

    const juce::Rectangle<float> lens(28.0f, 23.0f, 184.0f, 174.0f);
    const juce::Point<float> centre = lens.getCentre();
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff4a2b60), 77.0f, 59.0f,
                                           juce::Colour(0xff090912), 166.0f, 173.0f, true));
    g.fillEllipse(lens);

    g.saveState();
    juce::Path clip;
    clip.addEllipse(lens);
    g.reduceClipRegion(clip);

    // Overlapping, non-symmetrical colour clouds recreate the original cyan-left,
    // magenta-right inner globe without the static half-and-half gradient look.
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff2de7fa).withAlpha(0.98f),
                                           77.0f, 108.0f,
                                           juce::Colour(0xff3ecfe9).withAlpha(0.0f),
                                           137.0f, 105.0f, true));
    g.fillEllipse(22.0f, 51.0f, 135.0f, 134.0f);

    g.setGradientFill(juce::ColourGradient(juce::Colour(0xffff70dc).withAlpha(0.94f),
                                           168.0f, 91.0f,
                                           juce::Colour(0xffb64bc6).withAlpha(0.0f),
                                           117.0f, 127.0f, true));
    g.fillEllipse(104.0f, 34.0f, 132.0f, 149.0f);

    g.setGradientFill(juce::ColourGradient(juce::Colour(0xffa67aff).withAlpha(0.77f),
                                           121.0f, 154.0f,
                                           juce::Colour(0xff5542a9).withAlpha(0.0f),
                                           119.0f, 204.0f, true));
    g.fillEllipse(53.0f, 100.0f, 145.0f, 112.0f);

    // Irregular plasma lobes, deliberately offset and asymmetrical.
    juce::Path cyanLobe;
    cyanLobe.startNewSubPath(38.0f, 65.0f);
    cyanLobe.cubicTo(54.0f, 42.0f, 92.0f, 50.0f, 106.0f, 76.0f);
    cyanLobe.cubicTo(118.0f, 98.0f, 99.0f, 125.0f, 79.0f, 138.0f);
    cyanLobe.cubicTo(57.0f, 153.0f, 38.0f, 129.0f, 35.0f, 103.0f);
    cyanLobe.cubicTo(32.0f, 86.0f, 31.0f, 75.0f, 38.0f, 65.0f);
    cyanLobe.closeSubPath();
    g.setColour(juce::Colour(0xff26dbf4).withAlpha(0.42f));
    g.fillPath(cyanLobe);

    juce::Path magentaLobe;
    magentaLobe.startNewSubPath(129.0f, 43.0f);
    magentaLobe.cubicTo(159.0f, 32.0f, 190.0f, 57.0f, 193.0f, 85.0f);
    magentaLobe.cubicTo(197.0f, 111.0f, 178.0f, 138.0f, 157.0f, 124.0f);
    magentaLobe.cubicTo(140.0f, 111.0f, 147.0f, 91.0f, 134.0f, 76.0f);
    magentaLobe.cubicTo(122.0f, 63.0f, 118.0f, 49.0f, 129.0f, 43.0f);
    magentaLobe.closeSubPath();
    g.setColour(juce::Colour(0xffff78df).withAlpha(0.40f));
    g.fillPath(magentaLobe);

    // Curving filaments rotate with the plasma and give the Speed control a
    // visible animation response. A slower setting moves gently; fast settings snap.
    juce::Path filamentA;
    filamentA.startNewSubPath(38.0f, 124.0f);
    filamentA.cubicTo(62.0f, 114.0f, 80.0f, 86.0f, 104.0f, 75.0f);
    filamentA.cubicTo(119.0f, 67.0f, 129.0f, 70.0f, 140.0f, 75.0f);
    g.setColour(juce::Colour(0xff4ceaff).withAlpha(0.34f));
    g.strokePath(filamentA, juce::PathStrokeType(5.0f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
    g.setColour(juce::Colour(0xffc2f5ff).withAlpha(0.82f));
    g.strokePath(filamentA, juce::PathStrokeType(1.15f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));

    juce::Path filamentB;
    filamentB.startNewSubPath(125.0f, 42.0f);
    filamentB.cubicTo(156.0f, 51.0f, 177.0f, 78.0f, 172.0f, 101.0f);
    filamentB.cubicTo(170.0f, 119.0f, 154.0f, 134.0f, 143.0f, 141.0f);
    g.setColour(juce::Colour(0xffff66d9).withAlpha(0.28f));
    g.strokePath(filamentB, juce::PathStrokeType(5.0f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
    g.setColour(juce::Colour(0xffffc0ed).withAlpha(0.86f));
    g.strokePath(filamentB, juce::PathStrokeType(1.2f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));

    // Dense deterministic grain produces the grainy texture rather than a smooth gradient.
    for (int i = 0; i < 3900; ++i)
    {
        const unsigned int h1 = (unsigned int)i * 747796405u + 2891336453u;
        const unsigned int h2 = (unsigned int)i * 2246822519u + 3266489917u;
        const float px = 28.0f + (float)(h1 % 18400u) / 100.0f;
        const float py = 23.0f + (float)(h2 % 17400u) / 100.0f;
        const float dx = (px - centre.x) / 92.0f;
        const float dy = (py - centre.y) / 87.0f;
        if (dx * dx + dy * dy > 1.0f) continue;
        const float seam = centre.x + 10.0f * std::sin((py - 23.0f) * 0.055f)
                                    + 4.0f * std::sin((py - 23.0f) * 0.13f);
        juce::Colour speck = px < seam ? juce::Colour(0xff78ecff)
                           : juce::Colour(0xffff9de5);
        if (i % 7 == 0) speck = juce::Colour(0xfff1d8ff);
        const float alpha = (i % 17 == 0) ? 0.88f : ((i % 4 == 0) ? 0.43f : 0.19f);
        const float size = (i % 29 == 0) ? 2.3f : ((i % 5 == 0) ? 1.35f : 0.75f);
        g.setColour(speck.withAlpha(alpha));
        g.fillEllipse(px, py, size, size);
    }

    // Extra turbulent micro-clouds: overlapping translucent, off-axis eddies avoid
    // the clean symmetric blobs that make the orb look like a flat gradient.
    for (int i = 0; i < 34; ++i)
    {
        const unsigned int h1 = (unsigned int)i * 747796405u + 2891336453u;
        const unsigned int h2 = (unsigned int)i * 277803737u + 1013904223u;
        const float cx = 39.0f + (float)(h1 % 15000u) / 100.0f;
        const float cy = 38.0f + (float)(h2 % 13000u) / 100.0f;
        const float w = 15.0f + (float)((h1 >> 7u) % 3200u) / 100.0f;
        const float h = 12.0f + (float)((h2 >> 9u) % 2600u) / 100.0f;
        const bool cool = ((h1 >> 15u) & 1u) == 0u;
        const juce::Colour colour = cool ? juce::Colour(0xff29dff3) : juce::Colour(0xffff66d7);
        const float alpha = 0.035f + (float)((h2 >> 18u) % 100u) / 1000.0f;
        g.setGradientFill(juce::ColourGradient(colour.withAlpha(alpha), cx, cy,
                                               colour.withAlpha(0.0f), cx + w * 0.8f, cy + h, true));
        g.fillEllipse(cx - w * 0.5f, cy - h * 0.5f, w, h);
    }

    // Soft cloudy turbulence across the lower half gives the core depth.
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff3fbef4).withAlpha(0.32f),
                                           80.0f, 144.0f,
                                           juce::Colour(0xff3fbef4).withAlpha(0.0f),
                                           130.0f, 179.0f, true));
    g.fillEllipse(28.0f, 103.0f, 134.0f, 96.0f);
    g.restoreState();
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

            const float zeroAngle = startAngle + (endAngle - startAngle) * 0.5f;
            const float currentAngle = startAngle + (endAngle - startAngle) * sliderPos;
            juce::Path sustainValue;
            sustainValue.addCentredArc(cx, cy, tickRadius + 1.5f, tickRadius + 1.5f, 0.0f,
                                       juce::jmin(zeroAngle, currentAngle),
                                       juce::jmax(zeroAngle, currentAngle), true);
            const auto sustainAccent = slider.getValue() < 0.0 ? cyan : pink;
            g.setColour(sustainAccent.withAlpha(0.90f));
            g.strokePath(sustainValue, juce::PathStrokeType(2.2f));

            // Neutral-zero tick distinguishes slower negative sustain from faster positive sustain.
            const float tickInner = tickRadius - 4.0f;
            const float tickOuter = tickRadius + 5.0f;
            g.setColour(juce::Colour(0xffd9d4e1).withAlpha(0.9f));
            g.drawLine(cx + std::cos(zeroAngle) * tickInner, cy + std::sin(zeroAngle) * tickInner,
                       cx + std::cos(zeroAngle) * tickOuter, cy + std::sin(zeroAngle) * tickOuter, 1.2f);
        }
        else
        {
            // The open C-shaped housing is an important visual signature of Speed.
            juce::Path casing;
            casing.addCentredArc(cx, cy, radius + 5.0f, radius + 5.0f, 0.0f,
                                 startAngle, endAngle, true);
            g.setColour(juce::Colour(0xff030304));
            g.strokePath(casing, juce::PathStrokeType(isSpeed ? 10.8f : 8.0f,
                                                      juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
            g.setColour(juce::Colour(0xff393940));
            g.strokePath(casing, juce::PathStrokeType(isSpeed ? 5.8f : 3.8f,
                                                      juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
            juce::Path rimHighlight;
            rimHighlight.addCentredArc(cx - 0.6f, cy - 0.6f, radius + 8.2f, radius + 8.2f,
                                       0.0f, startAngle + 0.08f, endAngle - 0.08f, true);
            g.setColour(juce::Colour(0xff9b95a7).withAlpha(0.38f));
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
            g.setColour(juce::Colour(0xff2b2a2f));
            g.fillPath(notch);
            g.setColour(juce::Colour(0xff19181d).withAlpha(0.88f));
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
                                       : juce::Colour(0xff696570).withAlpha(0.84f));
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

        const juce::String value = isSpeed
            ? juce::String(MetaTuneInspiredAudioProcessor::speedControlToMilliseconds((float)slider.getValue()), 1)
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
    // JUCE AudioProcessorEditor is not a SettableTooltipClient; individual controls provide tooltips.
    heatmapInputNotes.fill(-1);
    heatmapOutputNotes.fill(-1);
    heatmapConfidence.fill(0.0f);
    pitchReferenceHz = processor.getPitchReferenceHz();
    if (auto* toneVolumeRaw = processor.parameters.getRawParameterValue("toneVolume"))
        toneVolume = juce::jlimit(0.0f, 1.0f, toneVolumeRaw->load(std::memory_order_relaxed) / 100.0f);
    speed.setName("Speed"); amount.setName("Amount"); sustain.setName("Sustain");
    speed.setTooltip("Retune Speed: 200 ms (natural) down to -3 ms (hard tune)");
    amount.setTooltip("Correction Amount: 0% to 100%");
    sustain.setTooltip("Sustain response changes how retuning reacts on held notes; negative values slow it down and positive values speed it up.");
    styleKnob(speed); styleKnob(amount); styleKnob(sustain);
    sustain.setRange(-100.0, 100.0, 1.0);
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
    sustain.onDragStart = beginUndoableEdit;
    sustain.onDragEnd = finishUndoableEdit;
    sustain.onValueChange = [this]() { repaint(); };
    scaleAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(processor.parameters,"scale",scale);
    rootAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(processor.parameters,"root",root);
    stabAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.parameters,"stabilizer",stabilizer);
    latencyAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.parameters,"lowLatency",lowLatency);
    // Note Stabilizer and Low Latency are independently selectable in MetaTune.
    // Low Latency switches detector analysis windows; Stabilizer filters brief notes.
    stabilizer.setEnabled(true);
    lowLatency.onClick = [this]() { repaint(); };
    stabilizer.onClick = [this]() { repaint(); };
    selectedNoteIndex = juce::jlimit(0, 11, root.getSelectedItemIndex() < 0 ? 4 : root.getSelectedItemIndex());
    if (auto* rawWidth = processor.parameters.getRawParameterValue("doublerWidth"))
        doublerWidth = juce::jlimit(0.0f, 1.0f, rawWidth->load(std::memory_order_relaxed) / 100.0f);
    if (auto* rawMix = processor.parameters.getRawParameterValue("doublerMix"))
        doublerMix = juce::jlimit(0.0f, 1.0f, rawMix->load(std::memory_order_relaxed) / 100.0f);
    if (auto* rawBypass = processor.parameters.getRawParameterValue("globalBypass"))
        globalBypass = rawBypass->load(std::memory_order_relaxed) > 0.5f;
    if (auto* stabilizerTime = processor.parameters.getRawParameterValue("stabilizerTime"))
        selectedCorrectionTime = juce::jlimit(0, 3, (int) std::lround(stabilizerTime->load()));
    lastNonChromaticScaleMode = scale.getSelectedItemIndex() == 2 ? 2 : 1;
    slotA = processor.parameters.copyState();
    slotB = slotA.createCopy();
    scale.onChange = [this]() { repaint(); };
    root.onChange = [this]() { selectedNoteIndex = juce::jlimit(0, 11, root.getSelectedItemIndex()); repaint(); };

    // Startup spin-and-illuminate routine plays once for every newly opened editor.
    orbStartupStartedAtMs = juce::Time::getMillisecondCounter();
    orbLastTickMs = orbStartupStartedAtMs;
    orbStartupProgress = 0.0f;
    orbStartupGlow = 0.0f;
    orbStartupComplete = false;

    // Seed particles at staggered ages so the sphere feels alive immediately,
    // without a sudden burst when the first timer callback arrives.
    for (int i = 0; i < (int)orbParticles.size(); ++i)
    {
        auto& particle = orbParticles[(size_t)i];
        particle.lifetime = 1.20f + (float)(i % 7) * 0.115f;
        particle.age = ((float)((i * 29) % (int)orbParticles.size())
                        / (float)orbParticles.size()) * particle.lifetime;
        particle.angle = (float)i * 2.39996323f;
        particle.radialSpeed = 43.0f + (float)((i * 17) % 13) * 4.0f;
        particle.size = 0.7f + (float)((i * 7) % 5) * 0.34f;
        particle.twist = ((float)((i * 11) % 17) / 16.0f - 0.5f) * 0.50f;
        particle.seed = (float)i * 1.6180339f;
        particle.tint = i % 4;
    }
    // 60 Hz materially improves the startup light sweep, plasma flow and gyro motion.
    // The dense shell is cached; only the lightweight animated overlays are redrawn.
    startTimerHz(60);
}
MetaTuneInspiredAudioProcessorEditor::~MetaTuneInspiredAudioProcessorEditor()
{
    stopTimer();
    // A window can close mid-drag; always close any custom host automation gesture.
    if (activeGestureParamA != nullptr)
    {
        activeGestureParamA->endChangeGesture();
        activeGestureParamA = nullptr;
    }
    if (activeGestureParamB != nullptr)
    {
        activeGestureParamB->endChangeGesture();
        activeGestureParamB = nullptr;
    }
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
    static const int majorScale[] = { 0, 2, 4, 5, 7, 9, 11 };
    static const int minorScale[] = { 0, 2, 3, 5, 7, 8, 10 };

    const int labelH = juce::jmin(52, area.getHeight() / 4);
    const int keyTop = area.getY() + labelH;
    const int keyH = area.getBottom() - keyTop;
    const float laneW = (float)area.getWidth() / 12.0f;
    const int chosenRoot = juce::jlimit(0, 11, root.getSelectedItemIndex() < 0 ? selectedNoteIndex : root.getSelectedItemIndex());
    const int scaleMode = scale.getSelectedItemIndex(); // 0 chromatic, 1 major, 2 minor
    const float liveHz = processor.getDetectedHz();
    const float confidence = processor.getPitchConfidence();
    const bool hasLivePitch = liveHz > 0.0f && confidence >= 0.60f;
    const float pitchReference = juce::jmax(1.0f, processor.getPitchReferenceHz());
    const int liveInputPc = hasLivePitch
        ? ((int)std::lround(69.0f + 12.0f * std::log2(liveHz / pitchReference)) % 12 + 12) % 12 : -1;
    const int liveTargetPc = hasLivePitch
        ? ((int)std::lround(processor.getSmoothedTargetMidiNote()) % 12 + 12) % 12 : -1;

    const auto isAllowed = [scaleMode, chosenRoot](int noteIndex) noexcept
    {
        if (scaleMode == 0) return true;
        const int relative = (noteIndex - chosenRoot + 12) % 12;
        const int* intervals = scaleMode == 1 ? majorScale : minorScale;
        for (int degreeIndex = 0; degreeIndex < 7; ++degreeIndex)
            if (relative == intervals[degreeIndex]) return true;
        return false;
    };

    // MetaTune's keyboard is a 12-lane chromatic grid rather than a conventional
    // seven-key piano width: the header and illuminated lower lanes line up exactly.
    for (int note = 0; note < 12; ++note)
    {
        const float x = (float)area.getX() + (float)note * laneW;
        const bool allowed = isAllowed(note);
        const bool isRoot = note == chosenRoot;
        const bool isInput = note == liveInputPc;
        const bool isTarget = note == liveTargetPc;

        juce::Colour header = blackNote[note] ? juce::Colour(0xff35343a) : juce::Colour(0xff77777b);
        if (!allowed) header = header.interpolatedWith(juce::Colour(0xff242328), 0.42f);
        if (isRoot) header = header.interpolatedWith(juce::Colour(0xff8e68db), 0.28f);
        g.setColour(header);
        g.fillRect(x, (float)area.getY(), laneW - 1.0f, (float)labelH);
        g.setColour(juce::Colour(0xff202024));
        g.drawRect(x, (float)area.getY(), laneW - 1.0f, (float)labelH, 1.0f);
        if (isRoot)
        {
            g.setColour(juce::Colour(0xffd1afff).withAlpha(0.92f));
            g.fillRect(x + 1.0f, (float)area.getY() + 1.0f, laneW - 3.0f, 2.4f);
        }
        if (isInput)
        {
            g.setColour(cyan.withAlpha(0.95f));
            g.fillRect(x + 1.0f, (float)area.getBottom() - 4.0f, laneW - 3.0f, 3.0f);
        }
        g.setColour(text.withAlpha(allowed ? 1.0f : 0.56f));
        g.setFont(uiFont(juce::jlimit(9.0f, 15.0f, (float)labelH * 0.29f), juce::Font::bold));
        g.drawText(names[note], juce::Rectangle<float>(x, (float)area.getY() + 1.0f, laneW - 1.0f, (float)labelH - 2.0f),
                   juce::Justification::centred);

        const juce::Colour naturalTop = isRoot ? juce::Colour(0xffb08cff)
            : (allowed ? juce::Colour(0xff9878eb) : juce::Colour(0xff493e62));
        const juce::Colour naturalBottom = isRoot ? juce::Colour(0xffa78ae9)
            : (allowed ? juce::Colour(0xffa18be0) : juce::Colour(0xff393149));
        const juce::Colour blackTop = isRoot ? juce::Colour(0xff6b4b90)
            : (allowed ? juce::Colour(0xff594970) : juce::Colour(0xff29252f));
        const juce::Colour blackBottom = isRoot ? juce::Colour(0xff6b5791)
            : (allowed ? juce::Colour(0xff57466f) : juce::Colour(0xff211f27));
        const auto top = blackNote[note] ? blackTop : naturalTop;
        const auto bottom = blackNote[note] ? blackBottom : naturalBottom;
        juce::ColourGradient lane(top, x, (float)keyTop, bottom, x, (float)(keyTop + keyH), false);
        lane.addColour(0.50, blackNote[note]
            ? (allowed ? juce::Colour(0xff4d3f63) : juce::Colour(0xff28242e))
            : (allowed ? juce::Colour(0xff9a80e7) : juce::Colour(0xff423651)));
        g.setGradientFill(lane);
        g.fillRect(x + 0.5f, (float)keyTop, laneW - 1.0f, (float)keyH);
        g.setColour(juce::Colour(0xff25222c));
        g.drawRect(x, (float)keyTop, laneW - 1.0f, (float)keyH, 1.0f);
        g.setColour((isRoot ? juce::Colour(0xffe6d7ff) : juce::Colour(0xffcdb9ff))
                        .withAlpha(isRoot ? 0.68f : (allowed ? 0.20f : 0.05f)));
        g.drawLine(x + 1.4f, (float)keyTop + 1.2f, x + laneW - 2.0f, (float)keyTop + 1.2f,
                   isRoot ? 1.8f : 1.0f);

        if (isTarget)
        {
            g.setColour(cyan.withAlpha(0.13f + 0.18f * confidence));
            g.fillRect(x + 1.5f, (float)keyTop + 1.5f, laneW - 3.0f, (float)keyH - 3.0f);
            g.setColour(cyan.withAlpha(0.9f));
            g.drawRect(x + 1.0f, (float)keyTop + 1.0f, laneW - 2.0f, (float)keyH - 2.0f, 1.2f);
        }
    }

    // Black keys are distinct raised caps over the corresponding chromatic lanes.
    // They are narrower than their note lanes and have the original rounded lower corners.
    static const int blackNotes[] = { 1, 3, 6, 8, 10 };
    const float blackW = laneW * 0.92f;
    const float blackH = (float)keyH * 0.60f;
    const float corner = juce::jmin(10.5f, blackW * 0.17f);
    for (int note : blackNotes)
    {
        const float center = (float)area.getX() + ((float)note + 0.5f) * laneW;
        const float x = center - blackW * 0.5f;
        const float y = (float)keyTop;
        juce::Path shadow;
        shadow.addRoundedRectangle(x - 1.7f, y + 2.2f, blackW + 3.4f, blackH + 3.2f, corner + 1.5f);
        g.setColour(juce::Colour(0xff020203).withAlpha(0.82f));
        g.fillPath(shadow);

        juce::Path key;
        key.startNewSubPath(x, y);
        key.lineTo(x + blackW, y);
        key.lineTo(x + blackW, y + blackH - corner);
        key.quadraticTo(x + blackW, y + blackH, x + blackW - corner, y + blackH);
        key.lineTo(x + corner, y + blackH);
        key.quadraticTo(x, y + blackH, x, y + blackH - corner);
        key.closeSubPath();

        juce::ColourGradient black(juce::Colour(0xff302f35), x, y,
                                   juce::Colour(0xff09090b), x + blackW, y + blackH, false);
        black.addColour(0.50, juce::Colour(0xff1b1b20));
        g.setGradientFill(black);
        g.fillPath(key);
        const bool isRoot = note == chosenRoot;
        const bool isLiveInput = note == liveInputPc;
        const bool isLiveTarget = note == liveTargetPc;
        g.setColour(isLiveInput || isLiveTarget ? cyan.withAlpha(0.95f)
                     : (isRoot ? juce::Colour(0xffd9b6ff) : juce::Colour(0xff48464f)));
        g.strokePath(key, juce::PathStrokeType((isLiveInput || isLiveTarget || isRoot) ? 1.65f : 1.0f));

        juce::Path bevel;
        bevel.startNewSubPath(x + 2.0f, y + 2.0f);
        bevel.lineTo(x + 2.0f, y + blackH - corner);
        bevel.quadraticTo(x + 2.0f, y + blackH - 2.0f, x + corner, y + blackH - 2.0f);
        g.setColour(juce::Colour(0xff898390).withAlpha(0.30f));
        g.strokePath(bevel, juce::PathStrokeType(0.9f));
    }
}

float MetaTuneInspiredAudioProcessorEditor::nextOrbRandomUnit() noexcept
{
    orbParticleRandomState = orbParticleRandomState * 1664525u + 1013904223u;
    return (float)((orbParticleRandomState >> 8u) & 0x00ffffffu) / 16777215.0f;
}

void MetaTuneInspiredAudioProcessorEditor::updateOrbMotion(float deltaSeconds)
{
    const float dt = juce::jlimit(0.0f, 0.08f, deltaSeconds);
    if (dt <= 0.0f)
        return;

    orbElapsedSeconds += dt;

    // Let the orb breathe with its own slow cadence, but respond subtly to real
    // input energy and pitch lock. Smooth the control signal so consonants do not
    // make the entire globe flicker; silence still retains the idle animation.
    const float rmsDrive = juce::jlimit(0.0f, 1.0f, processor.getInputRms() * 10.0f);
    const float lockDrive = juce::jlimit(0.0f, 1.0f, processor.getPitchConfidence());
    const float driveTarget = 0.72f * rmsDrive + 0.28f * lockDrive;
    orbAudioDrive += (driveTarget - orbAudioDrive) * (1.0f - std::exp(-dt * 4.5f));
    orbBreathPhase += dt * juce::MathConstants<float>::twoPi
                    * (0.30f + 0.10f * orbAudioDrive);
    const float organicBreath = 0.5f + 0.5f * std::sin(orbBreathPhase);
    orbBreathAmount = juce::jlimit(0.0f, 1.0f,
        0.82f * organicBreath + 0.18f * orbAudioDrive);

    // Three rings use deliberately different, counter-rotating rates. Input
    // energy adds only a small velocity modulation, preserving the gyroscope feel.
    const float twoPi = juce::MathConstants<float>::twoPi;
    orbRingPhaseA = std::fmod(orbRingPhaseA + dt * (0.58f + 0.16f * orbAudioDrive), twoPi);
    orbRingPhaseB = std::fmod(orbRingPhaseB - dt * (0.40f + 0.09f * orbAudioDrive) + twoPi, twoPi);
    orbRingPhaseC = std::fmod(orbRingPhaseC + dt * (0.25f + 0.07f * orbAudioDrive), twoPi);

    for (auto& particle : orbParticles)
        if (particle.age < particle.lifetime)
            particle.age += dt;

    // Eject a small spark from the globe's equator about 18 times per second.
    // The fixed-size pool is bounded; recycling never allocates on the UI thread.
    orbParticleSpawnAccumulator += dt;
    const float spawnInterval = 0.064f - 0.020f * orbAudioDrive;
    while (orbParticleSpawnAccumulator >= spawnInterval)
    {
        orbParticleSpawnAccumulator -= spawnInterval;
        auto& particle = orbParticles[(size_t)orbParticleWriteIndex];
        particle.age = 0.0f;
        particle.lifetime = 1.05f + nextOrbRandomUnit() * 0.95f;
        particle.angle = nextOrbRandomUnit() * twoPi;
        particle.radialSpeed = 43.0f + nextOrbRandomUnit() * 62.0f + 18.0f * orbAudioDrive;
        particle.size = 0.65f + nextOrbRandomUnit() * 1.85f + 0.25f * orbAudioDrive;
        particle.twist = (nextOrbRandomUnit() - 0.5f) * 0.66f;
        particle.seed = nextOrbRandomUnit() * twoPi;
        particle.tint = (int)(nextOrbRandomUnit() * 4.0f);
        orbParticleWriteIndex = (orbParticleWriteIndex + 1) % (int)orbParticles.size();
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
    g.setFont(uiFont(26.0f, juce::Font::bold));
    // Fitted text prevents fallback font metrics from clipping the final letters.
    const juce::Rectangle<int> wordmarkBounds(508, 6, 471, 48);
    g.setColour(juce::Colour(0xffb9a0ff).withAlpha(0.22f));
    g.drawFittedText("metaTune", wordmarkBounds.translated(0, 1), juce::Justification::centred, 1, 0.90f);
    g.setColour(juce::Colour(0xffe7ddff));
    g.drawFittedText("metaTune", wordmarkBounds, juce::Justification::centred, 1, 0.90f);

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
    // MetaTune visualizer behavior from the design case study:
    // Amount changes outer-orb diameter, Speed rotates the inner plasma, and Doubler
    // shows two offset rings with blue haze controlled by the Doubler mix gesture.
    const float amountNorm = juce::jlimit(0.0f, 1.0f, (float)amount.getValue() / 100.0f);
    const float orbScale = (0.86f + 0.14f * amountNorm) * (1.0f + 0.006f * visualizerActivity);
    const float inputActivity = juce::jlimit(0.0f, 1.0f, visualizerActivity);
    const float pitchLock = juce::jlimit(0.0f, 1.0f, processor.getPitchConfidence());
    const float coreBreathScale = 0.978f + 0.044f * orbBreathAmount;
    const float flowTime = orbElapsedSeconds;
    constexpr float orbCentreX = 470.0f, orbCentreY = 365.0f;

    g.saveState();
    g.addTransform(juce::AffineTransform::scale(orbScale, orbScale,
                                               orbCentreX, orbCentreY));

    // Doubler visualizer: two translucent rings shift apart as width increases.
    // Mix controls the cyan-blue blur that the design uses to indicate blend amount.
    const float blurStrength = juce::jlimit(0.0f, 1.0f, doublerMix);
    const float doublerEngagement = juce::jlimit(0.0f, 1.0f, (blurStrength - 0.015f) / 0.20f);
    const float doublerOffset = (2.0f + 28.0f * juce::jlimit(0.0f, 1.0f, doublerWidth))
                              * doublerEngagement;
    const juce::Colour doublerBlue(0xff47cfff);
    if (doublerEngagement > 0.001f)
    {
        for (const float side : { -1.0f, 1.0f })
        {
            const float dx = side * doublerOffset;
            // The blend controls blur/brightness; width controls physical separation.
            g.setColour(doublerBlue.withAlpha((0.018f + 0.115f * blurStrength) * doublerEngagement));
            g.drawEllipse(238.0f + dx, 138.0f, 465.0f, 460.0f,
                          12.0f + blurStrength * 8.0f);
            g.setColour(doublerBlue.withAlpha((0.025f + 0.145f * blurStrength) * doublerEngagement));
            g.drawEllipse(241.0f + dx, 141.0f, 459.0f, 454.0f, 1.4f + blurStrength * 0.8f);
        }
    }

    // Heavy particle artwork is rasterized once, then reused during repaints.
    static const juce::Image orbArtwork = createOrbArtwork();
    g.drawImageAt(orbArtwork, 185, 85);

    // Startup light pass: a rotating icy highlight runs around the shell while
    // concentric silver/violet energy rings brighten and then settle smoothly.
    if (!orbStartupComplete && orbStartupGlow > 0.001f)
    {
        const float pulse = juce::jlimit(0.0f, 1.0f, orbStartupGlow);
        const float angle = -juce::MathConstants<float>::halfPi
                          + orbStartupProgress * juce::MathConstants<float>::twoPi * 2.65f;
        for (int layer = 0; layer < 7; ++layer)
        {
            const float spread = (float)layer * 3.4f + (1.0f - orbStartupProgress) * 14.0f;
            const float alpha = pulse * (0.045f + 0.018f * (6 - layer));
            g.setColour(juce::Colour(0xfff8f4ff).withAlpha(alpha));
            g.drawEllipse(238.0f - spread, 138.0f - spread,
                          465.0f + spread * 2.0f, 460.0f + spread * 2.0f,
                          2.0f + (float)(layer % 2));
        }

        juce::Path startupSweep;
        startupSweep.addArc(235.0f, 135.0f, 471.0f, 466.0f,
                            angle - 0.42f, angle + 0.35f, true);
        g.setColour(juce::Colour(0xffb4f1ff).withAlpha(0.18f + 0.30f * pulse));
        g.strokePath(startupSweep, juce::PathStrokeType(15.0f + 3.0f * pulse,
                                                        juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));
        g.setColour(juce::Colour(0xfffffdff).withAlpha(0.42f + 0.48f * pulse));
        g.strokePath(startupSweep, juce::PathStrokeType(2.0f + 1.6f * pulse,
                                                        juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));

        // A violet sweep crosses the inner halo in the opposite hue direction.
        juce::Path violetSweep;
        violetSweep.addArc(315.0f, 211.0f, 311.0f, 310.0f,
                           -angle * 0.73f - 0.48f, -angle * 0.73f + 0.24f, true);
        g.setColour(juce::Colour(0xffc59aff).withAlpha(0.20f + 0.42f * pulse));
        g.strokePath(violetSweep, juce::PathStrokeType(8.0f + 5.0f * pulse,
                                                       juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
    }

    // Retain the luminous silver shell at 50% host scaling.
    for (int shell = 16; shell >= 1; --shell)
    {
        const float spread = (float)shell * 1.25f;
        const float alpha = 0.045f + 0.012f * (17 - shell)
                          + 0.045f * inputActivity + 0.025f * pitchLock;
        const float thickness = 9.0f + (float)((shell + 1) % 4) * 4.0f
                              + 1.5f * inputActivity;
        g.setColour(juce::Colour(0xfff9f6ff).withAlpha(juce::jmin(0.70f, alpha)));
        g.drawEllipse(238.0f - spread, 138.0f - spread,
                      465.0f + spread * 2.0f, 460.0f + spread * 2.0f, thickness);
    }
    juce::Path shellSpecular;
    shellSpecular.addArc(241.0f, 141.0f, 459.0f, 454.0f, 3.28f, 5.18f, true);
    g.setColour(juce::Colour(0xfffffdff).withAlpha(0.22f + 0.07f * blurStrength + 0.12f * inputActivity));
    g.strokePath(shellSpecular, juce::PathStrokeType(12.0f + 3.0f * inputActivity, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
    g.setColour(juce::Colour(0xfffffdff).withAlpha(0.80f));
    g.strokePath(shellSpecular, juce::PathStrokeType(2.8f, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
    g.setColour(juce::Colour(0xfffdfaff).withAlpha(0.72f));
    g.drawEllipse(241.0f, 141.0f, 459.0f, 454.0f, 2.5f);

    // Violet bloom around the middle ring stays diffuse under the crisp neon rim.
    g.setColour(juce::Colour(0xffa875ff).withAlpha(0.13f));
    g.drawEllipse(306.0f, 202.0f, 329.0f, 328.0f, 31.0f);
    g.setColour(juce::Colour(0xffaa79ff).withAlpha(0.28f));
    g.drawEllipse(313.0f, 209.0f, 315.0f, 314.0f, 19.0f);
    g.setColour(juce::Colour(0xffb58aff).withAlpha(0.80f));
    g.drawEllipse(319.0f, 215.0f, 303.0f, 302.0f, 6.5f);
    g.setColour(juce::Colour(0xffe2d0ff).withAlpha(0.70f));
    g.drawEllipse(323.0f, 219.0f, 295.0f, 294.0f, 1.8f);

    // The inner texture rotates at a rate derived from Retune Speed. Faster/harder
    // tuning rotates faster and shifts the live tint from cyan-violet toward magenta.
    static const juce::Image innerOrbArtwork = createInnerOrbArtwork();
    const float speedNorm = juce::jlimit(0.0f, 1.0f, (float)speed.getValue() / 100.0f);
    const juce::Colour speedTint = juce::Colour(0xff4edcf5).interpolatedWith(
                                       juce::Colour(0xffff73d9), speedNorm);
    juce::Path coreClip;
    coreClip.addEllipse(370.0f, 270.0f, 201.0f, 190.0f);
    g.saveState();
    g.reduceClipRegion(coreClip);
    g.addTransform(juce::AffineTransform::scale(coreBreathScale, coreBreathScale,
                                                 orbCentreX, orbCentreY));
    g.addTransform(juce::AffineTransform::rotation(orbRotationRadians, orbCentreX, orbCentreY));
    g.drawImageAt(innerOrbArtwork, 350, 255);
    g.restoreState();

    // Slight live hue correlation to the Speed dial, applied gently so texture stays visible.
    g.saveState();
    g.reduceClipRegion(coreClip);
    g.setGradientFill(juce::ColourGradient(speedTint.withAlpha(0.035f + 0.095f * speedNorm),
                                           518.0f, 348.0f,
                                           speedTint.withAlpha(0.0f), 463.0f, 367.0f, true));
    g.fillEllipse(370.0f, 270.0f, 201.0f, 190.0f);
    const float plasmaPulse = 0.5f + 0.5f * std::sin(orbRotationRadians * 1.7f);
    const juce::Colour pulseColour = cyan.interpolatedWith(pink, 0.5f + 0.5f * std::sin(orbRotationRadians * 0.63f));
    g.setGradientFill(juce::ColourGradient(pulseColour.withAlpha(0.025f + 0.075f * plasmaPulse
                                                                    + 0.055f * inputActivity),
                                           468.0f + 9.0f * std::sin(orbRotationRadians),
                                           357.0f + 7.0f * std::cos(orbRotationRadians * 0.81f),
                                           pulseColour.withAlpha(0.0f), 470.0f, 365.0f, true));
    g.fillEllipse(370.0f, 270.0f, 201.0f, 190.0f);
    g.restoreState();

    // Organic plasma drift: broad colour clouds migrate upward through the globe,
    // while multiple sine-warped filaments bend at different rates. This is separate
    // from the inner texture rotation, so the material feels fluid rather than rigid.
    g.saveState();
    g.reduceClipRegion(coreClip);
    g.addTransform(juce::AffineTransform::scale(coreBreathScale, coreBreathScale,
                                                 orbCentreX, orbCentreY));
    const float cloudCycleA = std::fmod(flowTime * 16.0f, 56.0f);
    const float cloudCycleB = std::fmod(flowTime * 11.0f + 19.0f, 62.0f);
    const float cloudX = 8.0f * std::sin(flowTime * 0.73f);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff32eaff).withAlpha(0.18f + 0.10f * orbBreathAmount),
                                           410.0f + cloudX, 371.0f - cloudCycleA,
                                           juce::Colour(0xff32eaff).withAlpha(0.0f),
                                           461.0f + cloudX, 351.0f - cloudCycleA, true));
    g.fillEllipse(376.0f + cloudX, 305.0f - cloudCycleA, 104.0f, 112.0f);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xffff73de).withAlpha(0.15f + 0.09f * (1.0f - orbBreathAmount)),
                                           524.0f - 7.0f * std::sin(flowTime * 0.61f), 338.0f - cloudCycleB,
                                           juce::Colour(0xffff73de).withAlpha(0.0f),
                                           485.0f, 361.0f - cloudCycleB, true));
    g.fillEllipse(471.0f - 7.0f * std::sin(flowTime * 0.61f), 286.0f - cloudCycleB,
                  94.0f, 124.0f);

    for (int stream = 0; stream < 4; ++stream)
    {
        const float phase = flowTime * (0.72f + 0.13f * (float)stream)
                          + (float)stream * 1.73f + orbRotationRadians * 0.17f;
        const float startX = 390.0f + (float)stream * 12.0f + 8.0f * std::sin(phase);
        const float startY = 401.0f - std::fmod(flowTime * (8.0f + 1.9f * (float)stream)
                                                + (float)stream * 23.0f, 54.0f);
        juce::Path streamPath;
        streamPath.startNewSubPath(startX, startY);
        streamPath.cubicTo(405.0f + 20.0f * std::sin(phase + 0.8f),
                           382.0f - 13.0f * std::cos(phase * 1.2f),
                           454.0f + 15.0f * std::sin(phase + 2.0f),
                           317.0f + 11.0f * std::cos(phase + 1.1f),
                           528.0f + 8.0f * std::sin(phase + 2.8f),
                           304.0f + 16.0f * std::sin(phase * 0.81f));
        const juce::Colour streamColour = (stream % 2 == 0)
            ? juce::Colour(0xff74e9ff) : juce::Colour(0xffff8ee6);
        g.setColour(streamColour.withAlpha(0.035f + 0.035f * orbBreathAmount));
        g.strokePath(streamPath, juce::PathStrokeType(7.5f, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
        g.setColour(streamColour.withAlpha(0.12f + 0.09f * orbBreathAmount));
        g.strokePath(streamPath, juce::PathStrokeType(1.0f + 0.35f * (float)(stream % 2),
                                                       juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
    }
    g.restoreState();

    // Live pitch-reactive plasma. The cached artwork remains static and cheap to
    // draw; only these few filaments/particles respond to detected pitch confidence
    // and the direction/strength of the current correction.
    const float correctionCentsLive = processor.getCorrectionCents();
    const float correctionStrength = juce::jlimit(0.0f, 1.0f, std::abs(correctionCentsLive) / 55.0f);
    const float lockGlow = pitchLock * (0.10f + 0.28f * correctionStrength)
                         * (0.25f + 0.75f * inputActivity);
    const juce::Colour correctionTint = correctionCentsLive >= 0.0f
        ? juce::Colour(0xffff78dc) : juce::Colour(0xff62ddfa);

    if (pitchLock > 0.05f && inputActivity > 0.015f)
    {
        juce::Path liveFilament;
        const float filamentPhase = orbRotationRadians * 0.42f
                                  + correctionCentsLive * 0.0018f;
        liveFilament.startNewSubPath(397.0f, 388.0f);
        liveFilament.cubicTo(414.0f + 10.0f * std::sin(filamentPhase),
                             369.0f + 7.0f * std::cos(filamentPhase),
                             447.0f + 8.0f * std::sin(filamentPhase + 1.4f),
                             331.0f + 7.0f * std::cos(filamentPhase + 1.4f),
                             532.0f, 318.0f + 9.0f * std::sin(filamentPhase + 2.1f));
        juce::Path clippedLiveFilament;
        clippedLiveFilament.addPath(liveFilament);
        g.saveState();
        g.reduceClipRegion(coreClip);
        g.setColour(correctionTint.withAlpha(0.07f + 0.20f * lockGlow));
        g.strokePath(clippedLiveFilament, juce::PathStrokeType(9.0f + 5.0f * correctionStrength,
                                                               juce::PathStrokeType::curved,
                                                               juce::PathStrokeType::rounded));
        g.setColour(correctionTint.withAlpha(0.12f + 0.50f * lockGlow));
        g.strokePath(clippedLiveFilament, juce::PathStrokeType(1.0f + 1.3f * correctionStrength,
                                                               juce::PathStrokeType::curved,
                                                               juce::PathStrokeType::rounded));

        juce::Path secondaryFilament;
        secondaryFilament.startNewSubPath(405.0f, 325.0f + 5.0f * std::sin(filamentPhase + 0.7f));
        secondaryFilament.cubicTo(428.0f + 6.0f * std::sin(filamentPhase + 1.8f),
                                  306.0f + 8.0f * std::cos(filamentPhase),
                                  470.0f + 8.0f * std::sin(filamentPhase + 2.3f),
                                  415.0f + 6.0f * std::cos(filamentPhase + 1.0f),
                                  523.0f, 395.0f + 8.0f * std::sin(filamentPhase + 0.4f));
        g.setColour(pink.interpolatedWith(cyan, 0.5f + 0.5f * std::sin(filamentPhase))
                        .withAlpha(0.06f + 0.15f * lockGlow));
        g.strokePath(secondaryFilament, juce::PathStrokeType(5.5f, juce::PathStrokeType::curved,
                                                              juce::PathStrokeType::rounded));
        g.setColour(juce::Colour(0xffffe7fa).withAlpha(0.08f + 0.18f * lockGlow));
        g.strokePath(secondaryFilament, juce::PathStrokeType(1.0f, juce::PathStrokeType::curved,
                                                              juce::PathStrokeType::rounded));

        for (int i = 0; i < 42; ++i)
        {
            const float a = orbRotationRadians * 0.58f + (float)i * 2.39996323f;
            const float jitter = (float)((i * 29 + 7) % 17) / 16.0f;
            const float r = 48.0f + jitter * 34.0f;
            const float px = 470.0f + std::cos(a) * r;
            const float py = 365.0f + std::sin(a) * r * 0.91f;
            const float size = (i % 7 == 0) ? 2.0f : 1.0f;
            const auto particleColour = juce::Colour(0xff5ee7fa).interpolatedWith(
                juce::Colour(0xffff82df), (std::sin(a) + 1.0f) * 0.5f);
            g.setColour(particleColour.withAlpha(0.05f + 0.20f * pitchLock * (0.45f + 0.55f * jitter)));
            g.fillEllipse(px, py, size, size);
        }
        g.restoreState();
    }

    // Three ultra-thin gyroscope rings with separate tilted planes and opposing
    // angular velocities. Segment brightness follows the ring's pseudo-depth, giving
    // a holographic front/back turn without using expensive per-frame image filters.
    struct RingSpec { float rx, ry, tilt, phase; juce::Colour cool, warm; float width; };
    const RingSpec ringSpecs[] = {
        { 102.0f, 26.0f, 0.47f, orbRingPhaseA, juce::Colour(0xffa9e9ff), juce::Colour(0xffd3a4ff), 0.95f },
        { 114.0f, 37.0f, 0.62f, orbRingPhaseB, juce::Colour(0xff8de4f7), juce::Colour(0xfff3caff), 0.80f },
        { 94.0f, 51.0f, 0.34f, orbRingPhaseC, juce::Colour(0xffc6afff), juce::Colour(0xff72e2f8), 1.05f }
    };
    const auto ringPoint = [coreBreathScale](float t, const RingSpec& ring)
    {
        const float localX = ring.rx * coreBreathScale * std::cos(t);
        const float localY = ring.ry * coreBreathScale * std::sin(t);
        const float cs = std::cos(ring.phase);
        const float sn = std::sin(ring.phase);
        return juce::Point<float>(470.0f + localX * cs - localY * sn,
                                  365.0f + (localX * sn + localY * cs) * ring.tilt);
    };
    constexpr int ringSegments = 88;
    for (int ringIndex = 0; ringIndex < 3; ++ringIndex)
    {
        const auto& ring = ringSpecs[ringIndex];
        for (int segment = 0; segment < ringSegments; ++segment)
        {
            const float t0 = juce::MathConstants<float>::twoPi * (float)segment / (float)ringSegments;
            const float t1 = juce::MathConstants<float>::twoPi * (float)(segment + 1) / (float)ringSegments;
            const auto p0 = ringPoint(t0, ring);
            const auto p1 = ringPoint(t1, ring);
            const float depth = 0.5f + 0.5f * std::sin(t0 + (float)ringIndex * 1.3f);
            const float alpha = 0.09f + 0.36f * depth;
            const float hue = 0.5f + 0.5f * std::sin(t0 * 1.2f + (float)ringIndex * 1.9f);
            const auto ringColour = ring.cool.interpolatedWith(ring.warm, hue);
            if (depth > 0.72f && segment % 3 == 0)
            {
                g.setColour(ringColour.withAlpha(0.045f + 0.07f * depth));
                g.drawLine(p0.x, p0.y, p1.x, p1.y, 3.0f);
            }
            g.setColour(ringColour.withAlpha(alpha));
            g.drawLine(p0.x, p0.y, p1.x, p1.y, ring.width);
        }
    }

    // Equatorial micro-sparks launch from the glass core and drift outward into
    // the dark annulus. Each has its own lifetime, radial velocity, curved drift,
    // tint and size; opacity ramps in and decays to zero before the particle recycles.
    for (const auto& particle : orbParticles)
    {
        if (particle.age < 0.0f || particle.age >= particle.lifetime)
            continue;
        const float life = juce::jlimit(0.0f, 1.0f, particle.age / particle.lifetime);
        const float angle = particle.angle + particle.twist * particle.age
                          + 0.075f * std::sin(particle.age * 4.1f + particle.seed);
        const float radius = 88.0f + particle.radialSpeed * particle.age;
        const float x = orbCentreX + std::cos(angle) * radius;
        const float y = orbCentreY + std::sin(angle) * radius * 0.91f;
        const float fadeIn = juce::jlimit(0.0f, 1.0f, life * 8.0f);
        const float fadeOut = 1.0f - life;
        const float twinkle = 0.82f + 0.18f * std::sin(orbElapsedSeconds * 8.0f + particle.seed);
        const float alpha = std::pow(fadeIn * fadeOut, 0.72f) * twinkle;
        juce::Colour spark = particle.tint == 0 ? cyan
                           : (particle.tint == 1 ? pink
                           : (particle.tint == 2 ? juce::Colour(0xffc4a0ff)
                                                 : juce::Colour(0xfff6efff)));
        const float size = particle.size * (0.82f + 0.50f * life);
        g.setColour(spark.withAlpha(0.075f * alpha));
        g.fillEllipse(x - size * 2.8f, y - size * 2.8f, size * 5.6f, size * 5.6f);
        g.setColour(spark.withAlpha(juce::jlimit(0.0f, 0.90f, 0.90f * alpha)));
        g.fillEllipse(x - size * 0.5f, y - size * 0.5f, size, size);
        if (life < 0.75f)
        {
            const float tailX = x - std::cos(angle) * (2.0f + 5.0f * life);
            const float tailY = y - std::sin(angle) * (2.0f + 4.0f * life);
            g.setColour(spark.withAlpha(0.06f + 0.22f * alpha));
            g.drawLine(tailX, tailY, x, y, 0.7f + 0.4f * alpha);
        }
    }

    // Keep the lightning logo fixed while the plasma texture moves beneath it.
    juce::Path bolt;
    bolt.startNewSubPath(477.0f, 326.0f);
    bolt.lineTo(455.0f, 354.0f);
    bolt.lineTo(469.0f, 354.0f);
    bolt.lineTo(461.0f, 380.0f);
    bolt.lineTo(486.0f, 345.0f);
    bolt.lineTo(472.0f, 345.0f);
    bolt.closeSubPath();
    g.setColour(juce::Colour(0xffc66dff).withAlpha(0.18f + 0.10f * orbBreathAmount));
    g.fillPath(bolt, juce::AffineTransform::translation(1.0f, 1.0f));
    g.setColour(juce::Colour(0xffffc6ff).withAlpha(0.92f + 0.08f * orbBreathAmount));
    g.fillPath(bolt);

    // One clean luminous inner rim keeps the rotating texture inside the glass lens.
    g.setColour(juce::Colour(0xfff0e5ff).withAlpha(0.68f));
    g.drawEllipse(370.0f, 270.0f, 201.0f, 190.0f, 2.4f);
    g.setColour(juce::Colour(0xffd6bdff).withAlpha(0.20f));
    g.drawEllipse(367.0f, 267.0f, 207.0f, 196.0f, 7.0f);

    g.restoreState();

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
        const auto noteTint = [](int note, bool output) -> juce::Colour
        {
            if (note < 0) return juce::Colour(0xff27252d);
            const int pc = (note % 12 + 12) % 12;
            static const juce::Colour hues[] = {
                juce::Colour(0xff9470ff), juce::Colour(0xff65dff6),
                juce::Colour(0xff8b72ee), juce::Colour(0xff65dff6),
                juce::Colour(0xffca83ee), juce::Colour(0xff67d7f1),
                juce::Colour(0xff9375ed), juce::Colour(0xff68dff3),
                juce::Colour(0xffa27aff), juce::Colour(0xff65d7f1),
                juce::Colour(0xffcf7edc), juce::Colour(0xff9879f2)
            };
            const auto base = hues[pc];
            return output ? base.interpolatedWith(juce::Colour(0xffff86d8), 0.26f) : base;
        };

        for (int i = 0; i < heatmapColumnCount; ++i)
        {
            const int historyIndex = (heatmapWriteIndex + i) % heatmapColumnCount;
            const float x = 165.0f + (float)i * 10.2f;
            const float strength = juce::jlimit(0.0f, 1.0f, heatmapConfidence[(size_t)historyIndex]);
            const int inputNote = heatmapInputNotes[(size_t)historyIndex];
            const int outputNote = heatmapOutputNotes[(size_t)historyIndex];
            g.setColour(juce::Colour(0xff0a0a0d));
            g.fillRect(x, 739.0f, 7.0f, 19.0f);
            g.fillRect(x, 762.0f, 7.0f, 18.0f);
            if (inputNote >= 0)
            {
                const auto inputColour = noteTint(inputNote, false);
                g.setColour(inputColour.withAlpha(0.30f + 0.68f * strength));
                g.fillRect(x, 739.0f, 6.0f, 19.0f);
                g.setColour(inputColour.withAlpha(0.14f + 0.42f * strength));
                g.fillRect(x + 1.0f, 741.0f, 2.0f, 15.0f);
            }
            if (outputNote >= 0)
            {
                const auto outputColour = noteTint(outputNote, true);
                g.setColour(outputColour.withAlpha(0.26f + 0.70f * strength));
                g.fillRect(x, 762.0f, 6.0f, 18.0f);
                g.setColour(outputColour.withAlpha(0.12f + 0.44f * strength));
                g.fillRect(x + 2.0f, 764.0f, 2.0f, 14.0f);
            }
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
            safeThis->toneVolume = 0.65f;
            safeThis->stabilizer.setToggleState(true, juce::sendNotificationSync);
            safeThis->lowLatency.setToggleState(true, juce::sendNotificationSync);
            safeThis->selectedCorrectionTime = 1;
            if (auto* parameter = safeThis->processor.parameters.getParameter("stabilizerTime"))
                parameter->setValueNotifyingHost(parameter->convertTo0to1(1.0f));
            safeThis->globalBypass = false;
            if (auto* parameter = safeThis->processor.parameters.getParameter("pitchReference"))
                parameter->setValueNotifyingHost(parameter->convertTo0to1(440.0f));
            if (auto* parameter = safeThis->processor.parameters.getParameter("globalBypass"))
                parameter->setValueNotifyingHost(parameter->convertTo0to1(0.0f));
            safeThis->doublerWidth = 0.46f;
            safeThis->doublerMix = 0.49f;
            if (auto* parameter = safeThis->processor.parameters.getParameter("doublerWidth"))
                parameter->setValueNotifyingHost(parameter->convertTo0to1(46.0f));
            if (auto* parameter = safeThis->processor.parameters.getParameter("doublerMix"))
                parameter->setValueNotifyingHost(parameter->convertTo0to1(49.0f));
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
        // Horizontal movement is the primary Doubler Width axis; vertical movement
        // continues to control the mix/blur intensity shown by the two orb rings.
        doublerWidth = juce::jlimit(0.0f, 1.0f,
            doublerDragStartWidth + (designX - orbDragStartX) / 100.0f);
        doublerMix = juce::jlimit(0.0f, 1.0f,
            doublerDragStartMix + (orbDragStartY - designY) / 90.0f);
        if (auto* parameter = processor.parameters.getParameter("doublerWidth"))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(doublerWidth * 100.0f));
        if (auto* parameter = processor.parameters.getParameter("doublerMix"))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(doublerMix * 100.0f));
        repaint();
        return;
    }
    if (orbDragMode == 4)
    {
        toneVolume = juce::jlimit(0.0f, 1.0f, (designX - 10.0f) / 145.0f);
        if (auto* parameter = processor.parameters.getParameter("toneVolume"))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(toneVolume * 100.0f));
        repaint();
        return;
    }
    if (orbDragMode == 5)
    {
        const float t = juce::jlimit(0.0f, 1.0f, (designX - 1218.0f) / 140.0f);
        pitchReferenceHz = pitchReferenceMinHz + t * (pitchReferenceMaxHz - pitchReferenceMinHz);
        pitchReferenceHz = (float)(juce::roundToInt(pitchReferenceHz * 10.0f) / 10.0f);
        if (auto* parameter = processor.parameters.getParameter("pitchReference"))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(pitchReferenceHz));
        repaint();
        return;
    }

    const float delta = (orbDragStartY - designY) * 0.48f;
    if (orbDragMode == 1)
        speed.setValue(juce::jlimit<float>(0.0f, 100.0f,
                         static_cast<float>(orbDragStartValue + delta)), juce::sendNotificationSync);
    else if (orbDragMode == 2)
        amount.setValue(juce::jlimit<float>(0.0f, 100.0f,
                          static_cast<float>(orbDragStartValue + delta)), juce::sendNotificationSync);
}

void MetaTuneInspiredAudioProcessorEditor::mouseUp(const juce::MouseEvent&)
{
    processor.stopTonePreview();
    if (activeGestureParamA != nullptr)
    {
        activeGestureParamA->endChangeGesture();
        activeGestureParamA = nullptr;
    }
    if (activeGestureParamB != nullptr)
    {
        activeGestureParamB->endChangeGesture();
        activeGestureParamB = nullptr;
    }
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
        undoState = processor.parameters.copyState();
        hasUndoState = true;
        hasRedoState = false;
        orbDragMode = 3;
        orbDragStartX = x;
        orbDragStartY = y;
        doublerDragStartWidth = doublerWidth;
        doublerDragStartMix = doublerMix;
        activeGestureParamA = processor.parameters.getParameter("doublerWidth");
        activeGestureParamB = processor.parameters.getParameter("doublerMix");
        if (activeGestureParamA != nullptr) activeGestureParamA->beginChangeGesture();
        if (activeGestureParamB != nullptr) activeGestureParamB->beginChangeGesture();
        return;
    }

    // The guide documents orb gestures as alternate ways to adjust Speed and Amount.
    // Drag the small inner globe for Speed; drag the frosted outer shell for Amount.
    const float orbDx = x - 470.0f;
    const float orbDy = y - 365.0f;
    const float drawnOrbRadius = std::sqrt(orbDx * orbDx + orbDy * orbDy);
    const float amountNormForHitTest = juce::jlimit(0.0f, 1.0f, (float)amount.getValue() / 100.0f);
    const float orbHitScale = 0.86f + 0.14f * amountNormForHitTest;
    const float orbRadius = drawnOrbRadius / juce::jmax(0.01f, orbHitScale);
    if (orbRadius <= 101.0f)
    {
        orbDragMode = 1;
        orbDragStartY = y;
        orbDragStartValue = (float)speed.getValue();
        activeGestureParamA = processor.parameters.getParameter("speed");
        if (activeGestureParamA != nullptr) activeGestureParamA->beginChangeGesture();
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
        activeGestureParamA = processor.parameters.getParameter("amount");
        if (activeGestureParamA != nullptr) activeGestureParamA->beginChangeGesture();
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
        int note = juce::jlimit(0, 11, (int)(keyboardX / semitoneW));
        if (y >= 840.0f)
        {
            const int blackNotes[] = { 1, 3, 6, 8, 10 };
            const float blackW = semitoneW * 0.92f;
            const float blackBottom = 840.0f + (1075.0f - 840.0f) * 0.60f;
            if (y < blackBottom)
            {
                for (const int blackNote : blackNotes)
                {
                    const float centre = ((float)blackNote + 0.5f) * semitoneW;
                    if (keyboardX >= centre - blackW * 0.5f && keyboardX <= centre + blackW * 0.5f)
                    {
                        note = blackNote;
                        break;
                    }
                }
            }
        }
        setRootNote(juce::jlimit(0, 11, note));
        // Audition the clicked semitone while the mouse is held, using the Tone
        // Volume control and current Pitch Reference. Mouse-up releases smoothly.
        processor.startTonePreview(60 + juce::jlimit(0, 11, note));
        return;
    }

    // Heatmaps, detected-note display, bypass, and the pitch-reference slider are real hit targets.
    if (x >= 1212.0f && y >= 734.0f && y < 790.0f) { showHeatMaps = !showHeatMaps; repaint(); return; }
    if (x >= 1212.0f && y >= 790.0f && y < 865.0f) { showDetectedNotes = !showDetectedNotes; repaint(); return; }
    if (x >= 1212.0f && y >= 865.0f && y < 940.0f)
    {
        globalBypass = !globalBypass;
        if (auto* parameter = processor.parameters.getParameter("globalBypass"))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(globalBypass ? 1.0f : 0.0f));
        repaint();
        return;
    }
    if (x >= 1212.0f && y >= 940.0f && y < 1045.0f)
    {
        const float t = juce::jlimit(0.0f, 1.0f, (x - 1218.0f) / 140.0f);
        pitchReferenceHz = pitchReferenceMinHz + t * (pitchReferenceMaxHz - pitchReferenceMinHz);
        pitchReferenceHz = (float)(juce::roundToInt(pitchReferenceHz * 10.0f) / 10.0f);
        if (auto* parameter = processor.parameters.getParameter("pitchReference"))
        {
            activeGestureParamA = parameter;
            activeGestureParamA->beginChangeGesture();
            parameter->setValueNotifyingHost(parameter->convertTo0to1(pitchReferenceHz));
        }
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
            if (auto* parameter = processor.parameters.getParameter("stabilizerTime"))
                parameter->setValueNotifyingHost(parameter->convertTo0to1((float)selectedCorrectionTime));
            repaint(); return;
        }
    }

    // Tone-volume bar controls the real reference-tone audition level in the processor.
    if (x >= 0.0f && x < 165.0f && y >= 822.0f && y < 860.0f)
    {
        toneVolume = juce::jlimit(0.0f, 1.0f, (x - 10.0f) / 145.0f);
        if (auto* parameter = processor.parameters.getParameter("toneVolume"))
        {
            activeGestureParamA = parameter;
            activeGestureParamA->beginChangeGesture();
            parameter->setValueNotifyingHost(parameter->convertTo0to1(toneVolume * 100.0f));
        }
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
    // One-shot startup sequence: fast spinning plasma and an illumination sweep
    // over 1.8 seconds, then a clean hand-off to normal Speed-driven animation.
    constexpr float startupDurationMs = 1800.0f;
    const juce::uint32 elapsedMs = juce::Time::getMillisecondCounter() - orbStartupStartedAtMs;
    orbStartupProgress = juce::jlimit(0.0f, 1.0f, (float)elapsedMs / startupDurationMs);

    if (elapsedMs >= (juce::uint32)startupDurationMs)
    {
        orbStartupComplete = true;
        orbStartupGlow = 0.0f;
    }
    else
    {
        const float attack = juce::jlimit(0.0f, 1.0f, (float)elapsedMs / 260.0f);
        const float envelope = std::sin(juce::MathConstants<float>::pi * orbStartupProgress);
        orbStartupGlow = attack * juce::jlimit(0.0f, 1.0f, 0.16f + 1.10f * envelope);
    }

    // Advance all motion by measured timer delta (instead of assuming exactly
    // 30 Hz), preventing speed jumps when the message thread is briefly busy.
    const juce::uint32 nowMs = juce::Time::getMillisecondCounter();
    const float deltaSeconds = (float)(nowMs - orbLastTickMs) * 0.001f;
    orbLastTickMs = nowMs;
    updateOrbMotion(deltaSeconds);

    // The inner plasma spins quickly on launch, then settles to a rate tied to Speed.
    const float speedNorm = juce::jlimit(0.0f, 1.0f, (float)speed.getValue() / 100.0f);
    const float confidenceForMotion = juce::jlimit(0.0f, 1.0f, processor.getPitchConfidence());
    const float effectiveMs = juce::jlimit(0.0f, 200.0f, processor.getEffectiveRetuneTimeMs());
    const float effectiveSpeedNorm = 1.0f - effectiveMs / 200.0f;
    const float motionSpeed = speedNorm * (1.0f - 0.28f * confidenceForMotion)
                            + effectiveSpeedNorm * 0.28f * confidenceForMotion;
    const float normalTurnsPerSecond = 0.08f + 2.40f * std::pow(motionSpeed, 1.35f);
    const float startupBlend = orbStartupComplete ? 0.0f
        : std::pow(1.0f - orbStartupProgress, 1.25f);
    const float turnsPerSecond = normalTurnsPerSecond + 4.6f * startupBlend;
    orbRotationRadians += juce::MathConstants<float>::twoPi * turnsPerSecond * juce::jlimit(0.0f, 0.08f, deltaSeconds);
    if (orbRotationRadians >= juce::MathConstants<float>::twoPi)
        orbRotationRadians = std::fmod(orbRotationRadians, juce::MathConstants<float>::twoPi);

    // Mirror host automation/state restoration from the signed APVTS parameter.
    if (auto* rawSustain = processor.parameters.getRawParameterValue("sustain"))
    {
        const double signedValue = (double)rawSustain->load();
        if (std::abs(sustain.getValue() - signedValue) > 0.51)
            sustain.setValue(signedValue, juce::dontSendNotification);
    }

    pitchReferenceHz = processor.getPitchReferenceHz();
    if (auto* rawWidth = processor.parameters.getRawParameterValue("doublerWidth"))
        doublerWidth = juce::jlimit(0.0f, 1.0f, rawWidth->load(std::memory_order_relaxed) / 100.0f);
    if (auto* rawMix = processor.parameters.getRawParameterValue("doublerMix"))
        doublerMix = juce::jlimit(0.0f, 1.0f, rawMix->load(std::memory_order_relaxed) / 100.0f);
    if (auto* rawBypass = processor.parameters.getRawParameterValue("globalBypass"))
        globalBypass = rawBypass->load(std::memory_order_relaxed) > 0.5f;
    if (auto* stabilizerTime = processor.parameters.getRawParameterValue("stabilizerTime"))
        selectedCorrectionTime = juce::jlimit(0, 3, (int) std::lround(stabilizerTime->load()));
    const float hz = processor.getDetectedHz();
    const float confidence = processor.getPitchConfidence();

    // Feed a rolling 3.3-second note history to the two HeatMap lanes.
    heatmapInputNotes[(size_t)heatmapWriteIndex] = -1;
    heatmapOutputNotes[(size_t)heatmapWriteIndex] = -1;
    heatmapConfidence[(size_t)heatmapWriteIndex] = 0.0f;
    if (hz > 0.0f && confidence >= 0.60f)
    {
        const int inputMidi = (int) std::lround(69.0f + 12.0f * std::log2(hz / processor.getPitchReferenceHz()));
        const int outputMidi = (int) std::lround(processor.getSmoothedTargetMidiNote());
        heatmapInputNotes[(size_t)heatmapWriteIndex] = (inputMidi % 12 + 12) % 12;
        heatmapOutputNotes[(size_t)heatmapWriteIndex] = (outputMidi % 12 + 12) % 12;
        heatmapConfidence[(size_t)heatmapWriteIndex] = confidence;
    }
    heatmapWriteIndex = (heatmapWriteIndex + 1) % heatmapColumnCount;

    const float activityTarget = juce::jlimit(0.0f, 1.0f, processor.getInputRms() * 9.0f)
                               * (0.30f + 0.70f * confidence);
    visualizerActivity = juce::jlimit(0.0f, 1.0f,
        visualizerActivity * 0.78f + activityTarget * 0.22f);

    if (hz > 0.0f && confidence >= 0.60f) {
        const int midi=(int)std::round(processor.getDetectedMidiNote());
        static const char* names[]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
        noteReadout.setText(names[(midi%12+12)%12],juce::dontSendNotification);
        noteReadout.setVisible(showDetectedNotes);
        status.setText(juce::String(hz,1)+" Hz   -   "+juce::String(confidence * 100.0f, 0)+"% confidence   -   "+juce::String(processor.getCorrectionCents(),1)+" cents",juce::dontSendNotification);
    } else {
        noteReadout.setText("",juce::dontSendNotification);
        noteReadout.setVisible(false);
        status.setText("Waiting for pitched input...",juce::dontSendNotification);
    }
    repaint();
}
