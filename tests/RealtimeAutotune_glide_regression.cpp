#include "RealtimeAutotune.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

struct Result { float peakStep = 0.0f; bool switched = false; bool finite = true; float maxShift = 0.0f; };
static Result run(float glideMs, float limit)
{
    autotune::RealtimeAutotune dsp;
    dsp.prepare(48000.0);
    autotune::Settings s;
    s.key = 0;
    s.scale = autotune::Scale::Major;
    s.retuneMs = 0.0f;
    s.noteStabilizerMs = 0.0f;
    s.vibratoPreserve = 1.0f;
    s.follow = 1.0f;
    s.glideMs = glideMs;
    s.maxCorrectionSemitones = limit;
    dsp.setSettings(s);
    Result result;
    int prevNote = -1;
    float previous = 0.0f;
    int samplesSinceSwitch = 100000;
    double phase = 0.0;
    for (int i = 0; i < 48000; ++i)
    {
        const double f = i < 18000 ? 277.18 : 311.13; // C#4 -> D#4, C major target change
        phase += 6.283185307179586 * f / 48000.0;
        const float x = dsp.process(0.45f * std::sin(phase));
        if (!std::isfinite(x)) result.finite = false;
        if ((i % 240) != 0) continue;
        const int note = dsp.getTargetMidi();
        if (prevNote >= 0 && note >= 0 && note != prevNote)
        {
            result.switched = true;
            samplesSinceSwitch = 0;
        }
        if (note >= 0) prevNote = note;
        const float shift = dsp.getCorrectionSemitones();
        if (!std::isfinite(shift)) result.finite = false;
        result.maxShift = std::max(result.maxShift, std::abs(shift));
        if (samplesSinceSwitch < 20)
            result.peakStep = std::max(result.peakStep, std::abs(shift - previous));
        previous = shift;
        ++samplesSinceSwitch;
    }
    return result;
}
int main()
{
    const auto fast = run(0.0f, 12.0f);
    const auto slow = run(100.0f, 12.0f);
    const auto limited = run(0.0f, 0.25f);
    if (!fast.finite || !slow.finite || !limited.finite) return 1;
    if (!fast.switched || !slow.switched) return 2;
    if (!(slow.peakStep < fast.peakStep)) return 3;
    if (limited.maxShift > 0.251f) return 4;
    autotune::RealtimeAutotune engine;
    engine.prepare(48000.0);
    autotune::Settings invalid;
    invalid.glideMs = std::numeric_limits<float>::quiet_NaN();
    invalid.maxCorrectionSemitones = std::numeric_limits<float>::infinity();
    engine.setSettings(invalid);
    if (engine.getSettings().glideMs != 0.0f || engine.getSettings().maxCorrectionSemitones != 12.0f) return 5;
    std::printf("PASS note-change glide: peak step %.3f -> %.3f semitones, limit %.3f\n", fast.peakStep, slow.peakStep, limited.maxShift);
    return 0;
}
