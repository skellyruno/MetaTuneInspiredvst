#include "RealtimeAutotune.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

// A real pitch-correction control must keep the OUTPUT period fixed when the
// detected SOURCE period moves under vibrato. V13 smoothed the ratio alone,
// producing a false 2-8 cent synthesis-clock wobble around a fixed note.
int main()
{
    constexpr double pi = 3.14159265358979323846;
    for (double sr : { 44100.0, 48000.0, 96000.0 })
        for (bool live : { false, true })
        {
            autotune::RealtimeAutotune tuner;
            tuner.prepare (sr);
            autotune::Settings settings;
            settings.scale = autotune::Scale::Chromatic;
            settings.precisionHardTune = true;
            settings.lowLatency = live;
            settings.voicingThreshold = 0.79f;
            tuner.setSettings (settings);
            std::vector<float> errors;
            double phase = 0.0;
            int qualified = 0;
            for (int i = 0; i < (int) (2.0 * sr); ++i)
            {
                const double t = (double) i / sr;
                const double vib = 0.36 * std::sin (2.0 * pi * 8.5 * t)
                                 + 0.07 * std::sin (2.0 * pi * 6.1 * t);
                const double inputHz = 440.0 * std::exp2 (vib / 12.0);
                phase += 2.0 * pi * inputHz / sr;
                const float x = (float) (0.22 * std::sin (phase)
                               + 0.09 * std::sin (2.0 * phase + 0.2)
                               + 0.04 * std::sin (3.0 * phase + 0.1));
                tuner.process (x);
                if (i % 128 != 0 || t < 0.5) continue;
                const auto diag = tuner.getDiagnostics();
                if (! diag.voiced || diag.targetMidi != 69 || diag.smoothedRatio <= 0.2f)
                    continue;
                ++qualified;
                const double synthPeriod = (double) diag.estimatedPeriodSamples
                                           / (double) diag.smoothedRatio;
                const double synthCents = 1200.0 * std::log2 ((sr / 440.0) / synthPeriod);
                if (! std::isfinite (synthCents)) return 3;
                errors.push_back ((float) std::abs (synthCents));
            }
            if (qualified < 300) return 4;
            std::sort (errors.begin(), errors.end());
            const float p95 = errors[(size_t) (0.95 * (double) errors.size())];
            std::printf ("sample_rate=%g live=%d qualified=%d synth_clock_p95=%.6f cents\n",
                         sr, (int) live, qualified, p95);
            if (p95 > 0.015f) return 5;
        }
    return 0;
}
