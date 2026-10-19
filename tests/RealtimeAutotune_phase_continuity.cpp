// V10: harmonic-phase continuity and bounded source-mark refinement.
// Inspired by the independent real-time autotune project's demonstration
// that per-buffer harmonic-phase resets create discontinuities. This test
// protects our *different* TD-PSOLA architecture against equivalent seams.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

int main()
{
    constexpr double pi = 3.14159265358979323846;
    int cases = 0;
    for (const bool fast : {false, true})
        for (const double fs : {44100.0, 48000.0, 96000.0})
            for (const double hz : {91.3, 234.2})
                for (const double pitchRatio : {0.82, 1.57})
                {
                    autotune::PsolaShifter shifter;
                    shifter.prepare (fs);
                    shifter.setLowLatency (fast);
                    double inputPhase = 0.0, correlationTotal = 0.0;
                    double seamLargest = 0.0, stepLargest = 0.0;
                    int grainCount = 0, seams = 0;
                    float oldSample = 0.0f;
                    uint64_t previousGrains = 0;
                    const int n = (int) (fs * 1.25);
                    for (int i = 0; i < n; ++i)
                    {
                        const double time = i / fs;
                        // Fractional, varying fundamental ensures phase-centre
                        // matching isn't accidentally helped by integer periods.
                        const double freq = hz * std::exp2 (15.0 * std::sin (2.0 * pi * 4.0 * time) / 1200.0);
                        inputPhase += 2.0 * pi * freq / fs;
                        const float input = 0.15f * (float) (
                            std::sin (inputPhase) + 0.4 * std::sin (2.0 * inputPhase + 0.3)
                                                  + 0.25 * std::sin (3.0 * inputPhase + 0.9));
                        shifter.setParameters ((float) pitchRatio, (float) (fs / freq));
                        const float output = shifter.processSample (input);
                        if (!std::isfinite (output)) return 1;
                        if (i > (int) (fs * 0.30))
                        {
                            const double step = std::abs (output - oldSample);
                            stepLargest = std::max (stepLargest, step);
                            if (i % 512 == 0) { seamLargest = std::max (seamLargest, step); ++seams; }
                            if (shifter.getSpawnedGrains() != previousGrains)
                            {
                                correlationTotal += shifter.getLastPhaseCorrelation();
                                ++grainCount;
                            }
                        }
                        oldSample = output;
                        previousGrains = shifter.getSpawnedGrains();
                    }
                    const double meanPhaseCorrelation = correlationTotal / std::max (1, grainCount);
                    if (grainCount < 15 || shifter.getGrainSaturationCount() != 0 ||
                        shifter.getDropoutSamples() > (uint64_t) (n / 200) ||
                        meanPhaseCorrelation < (fast ? 0.9997 : 0.99990) ||
                        stepLargest > 0.10 || seamLargest > 0.10 || seams < 20)
                    {
                        std::fprintf (stderr, "FAIL phase case fast=%d fs=%.0f hz=%.1f ratio=%.2f corr=%.6f seam=%.4f step=%.4f missing=%llu saturations=%llu\n",
                            fast,fs,hz,pitchRatio,meanPhaseCorrelation,seamLargest,stepLargest,
                            (unsigned long long)shifter.getDropoutSamples(),
                            (unsigned long long)shifter.getGrainSaturationCount());
                        return 2;
                    }
                    ++cases;
                }
    std::printf ("PASS %d phase/512-sample boundary cases, normal/live, 44.1/48/96 kHz\n", cases);
    return 0;
}
