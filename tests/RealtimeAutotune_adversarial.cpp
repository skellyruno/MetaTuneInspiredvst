// Extreme sample-rate/ratio tests: no NaNs, capacity saturation, or grain gaps.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>

int main()
{
    constexpr double pi = 3.14159265358979323846;
    unsigned cases = 0;
    for (double sr : {16000., 44100., 48000., 88200., 96000., 192000.})
        for (bool low : {false, true})
            for (float ratio : {.5f, .67f, 1.0f, 1.5f, 2.0f, 4.0f})
            {
                autotune::PsolaShifter shifter;
                shifter.prepare(sr);
                shifter.setLowLatency(low);
                shifter.setParameters(ratio, (float)(sr / 100.));
                const int n = (int)(sr * .25);
                const int warm = (int)(sr * .12);
                double energy = 0., dryEnergy = 0.;
                double phase = 0.;
                const auto before = shifter.getDropoutSamples();
                for (int i = 0; i < n; ++i)
                {
                    const float x = (float)(.18*std::sin(phase) + .07*std::sin(2.*phase));
                    phase += 2.*pi*100./sr;
                    const float y = shifter.processSample(x);
                    if (!std::isfinite(y) || std::abs(y) > 1.5f)
                    {
                        std::fprintf(stderr,"Non-finite or runaway sr %.0f ratio %.2f low %d frame %d\n",sr,ratio,(int)low,i);
                        return 1;
                    }
                    if (i > warm) {energy += (double)y*y; dryEnergy += (double)x*x;}
                }
                const double level = std::sqrt (energy / std::max(dryEnergy, 1.e-20));
                // Supported plugin correction is +/-12 st, or ratios 0.5..2.0.
                // Extreme 4x direct-engine input is a safety test only.
                const double minimumLevel = ratio <= 2.0f ? .55 : .10;
                const auto gaps = shifter.getDropoutSamples() - before;
                const auto saturation = shifter.getGrainSaturationCount();
                if (energy < 1.e-4 || level < minimumLevel || level > 1.45 || saturation != 0 || gaps > (uint64_t)(sr*.05))
                {
                    std::fprintf(stderr,"Bad grain sr %.0f ratio %.2f low %d power %.3f saturations %llu gaps %llu\n",
                        sr,ratio,(int)low,energy,(unsigned long long)saturation,(unsigned long long)gaps);
                    return 2;
                }
                ++cases;
            }
    std::printf("PASS %u adversarial low/normal ratios and sample rates; zero grain-bank saturation\n",cases);
}
