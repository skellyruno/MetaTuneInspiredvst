// Regression for pitch-shift -> unison -> pitch-shift handoffs.
// V6's phase-incoherent unity tap could produce a 0.36x RMS dip on a
// 440-Hz harmonic voice, despite passing all steady-state quality tests.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr double pi = 3.14159265358979323846;

int main()
{
    int checks = 0;
    for (double sampleRate : { 44100.0, 48000.0, 96000.0 })
        for (bool lowLatency : { true, false })
            for (double frequency : { 70.0, 85.0, 110.0, 185.0, 440.0 })
            {
                autotune::PsolaShifter shifter;
                shifter.prepare(sampleRate);
                shifter.setLowLatency(lowLatency);
                const int length = (int) (sampleRate * 1.15);
                std::vector<float> original((size_t) length), corrected((size_t) length);
                double phase = 0.0;
                for (int i = 0; i < length; ++i)
                {
                    const double t = i / sampleRate;
                    // 35c up -> unity across 20 ms -> held -> 35c up again.
                    // The first transition tests entry; the second tests exit.
                    const double delta = t < 0.35 ? 0.035
                        : t < 0.37 ? 0.035 * (0.37 - t) / 0.02
                        : t < 0.60 ? 0.0
                        : t < 0.62 ? 0.035 * (t - 0.60) / 0.02 : 0.035;
                    shifter.setParameters((float) (1.0 + delta), (float) (sampleRate / frequency));
                    phase += 2.0 * pi * frequency / sampleRate;
                    original[(size_t) i] = 0.23f * (float)
                        (std::sin(phase) + 0.38 * std::sin(2.0 * phase) +
                         0.18 * std::sin(3.0 * phase));
                    corrected[(size_t) i] = shifter.processSample(original[(size_t) i]);
                    if (!std::isfinite(corrected[(size_t) i])) return 1;
                }
                const int window = (int) (sampleRate / frequency * 1.5);
                // Decimated scan only for test speed; use the complete RMS window.
                double minimumRmsRatio = 10.0;
                for (int i = (int) (sampleRate * 0.22); i + window < length; i += 8)
                {
                    double inputPower = 0.0, outputPower = 0.0;
                    for (int k = 0; k < window; ++k)
                    {
                        inputPower += original[(size_t) (i+k)] * original[(size_t) (i+k)];
                        outputPower += corrected[(size_t) (i+k)] * corrected[(size_t) (i+k)];
                    }
                    minimumRmsRatio = std::min(minimumRmsRatio,
                        std::sqrt(outputPower / std::max(1.0e-10, inputPower)));
                }
                // Account for the ordinary PSOLA level ripple at 110 Hz and
                // 44.1-kHz exit transitions. The old severe ~0.35x nulls
                // must not return at any pitch, rate or mode.
                if (minimumRmsRatio < 0.64)
                {
                    std::fprintf(stderr, "FAIL unity handoff %.0f Hz @ %.0f mode=%s min RMS %.3f\n",
                        frequency, sampleRate, lowLatency ? "live" : "normal", minimumRmsRatio);
                    return 2;
                }
                if (shifter.getGrainSaturationCount() != 0 || shifter.getDropoutSamples() > (uint64_t)(sampleRate*.1))
                {
                    std::fprintf(stderr, "FAIL unity handoff grain coverage %.0f @ %.0f\n", frequency, sampleRate);
                    return 3;
                }
                std::printf("PASS unity handoff %.0f Hz @ %.0f mode=%s worst RMS %.3f\n",
                            frequency, sampleRate, lowLatency ? "live" : "normal", minimumRmsRatio);
                ++checks;
            }
    return checks == 30 ? 0 : 4;
}
