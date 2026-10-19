// Broad pitch-shift level-matching gate: catches reference-energy regressions.
// All audio is synthetic; not a formant or subjective-quality measurement.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

int main()
{
    constexpr double pi = 3.14159265358979323846;
    double lowest = 10.0, highest = 0.0;
    int cases = 0;
    for (const double sr : {44100., 48000., 96000.})
        for (const bool fast : {false, true})
            for (const double f0 : {100., 220., 440.})
                for (const float shiftRatio : {0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f})
                {
                    autotune::PsolaShifter s;
                    s.prepare (sr);
                    s.setLowLatency (fast);
                    s.setParameters (shiftRatio, (float) (sr / f0));
                    double dryEnergy = 0.0, wetEnergy = 0.0, phase = 0.0;
                    const int length = (int) (sr * 0.27);
                    const int settled = (int) (sr * 0.13);
                    for (int i = 0; i < length; ++i)
                    {
                        const float in = (float) (0.18 * std::sin (phase) +
                            0.07 * std::sin (phase * 2.0) + 0.04 * std::sin (phase * 3.0));
                        const float out = s.processSample (in);
                        if (!std::isfinite (out)) return 1;
                        if (i > settled)
                        {
                            dryEnergy += (double) in * in;
                            wetEnergy += (double) out * out;
                        }
                        phase += 2.0 * pi * f0 / sr;
                    }
                    const double level = std::sqrt (wetEnergy / std::max (dryEnergy, 1.e-12));
                    lowest = std::min (lowest, level);
                    highest = std::max (highest, level);
                    // Large excursions in this controlled, sustained input
                    // indicate lost grain energy or an overactive gain servo.
                    if (level < 0.80 || level > 1.17 || s.getGrainSaturationCount() != 0)
                    {
                        std::fprintf (stderr,"failed rate %.0f low=%d f0=%.0f ratio=%.2f level=%.3f\n",
                            sr, (int) fast, f0, shiftRatio, level);
                        return 2;
                    }
                    if (sr == 48000. && fast && f0 == 100. && shiftRatio == 2.0f &&
                        (level < 0.95 || level > 1.08))
                    {
                        std::fprintf (stderr,"octave-up low latency RMS = %.3f\n", level);
                        return 3;
                    }
                    ++cases;
                }
    std::printf("PASS %d RMS/shift combinations; min %.4f max %.4f\n", cases, lowest, highest);
    return 0;
}
