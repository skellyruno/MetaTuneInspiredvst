#include "RealtimeAutotune.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

int main()
{
    autotune::RealtimeAutotune engine;
    engine.prepare(48000.0);
    auto settings = autotune::Settings{};
    const float bad = std::numeric_limits<float>::quiet_NaN();
    settings.referenceHz = bad;
    settings.retuneMs = bad;
    settings.strength = bad;
    settings.humanize = bad;
    settings.vibratoPreserve = bad;
    settings.follow = bad;
    settings.voicingThreshold = bad;
    settings.noteConfirmMs = bad;
    settings.consonantReleaseMs = bad;
    settings.noteStabilizerMs = bad;
    settings.key = -17;
    settings.scale = static_cast<autotune::Scale>(999);
    engine.setSettings(settings);
    auto safe = engine.getSettings();
    if (safe.referenceHz != 440.0f || safe.retuneMs != 20.0f ||
        safe.key != 7 || safe.scale != autotune::Scale::Chromatic ||
        safe.strength != 1.0f || safe.noteConfirmMs != 10.0f ||
        safe.consonantReleaseMs != 8.0f || !std::isfinite(safe.noteStabilizerMs)) return 1;
    std::vector<float> data(48000);
    for (size_t i=0;i<data.size();++i)
        data[i] = 0.3f * std::sin(6.283185307179586 * 233.0 * i / 48000.0);
    engine.process(data.data(), static_cast<int>(data.size()));
    for (float x : data) if (!std::isfinite(x)) return 2;
    // Ensure extreme but finite automation values are clamped and remain safe.
    settings.referenceHz = -1.0f;
    settings.retuneMs = -99.0f;
    settings.strength = 99.0f;
    engine.setSettings(settings);
    safe = engine.getSettings();
    if (safe.referenceHz != 400.0f || safe.retuneMs != 0.0f || safe.strength != 1.0f) return 3;
    std::puts("PASS invalid host parameter sanitization and finite DSP output");
    return 0;
}
