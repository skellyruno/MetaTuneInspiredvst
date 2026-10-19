// Independent reconstruction stress case: harmonics, moving target ratio,
// abrupt unvoiced transitions and bounded synthesis coverage. Synthetic only.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>

static uint32_t prng(uint32_t& s) {s^=s<<13; s^=s>>17; s^=s<<5; return s;}

int main()
{
    for (double sr : { 44100.0, 48000.0, 96000.0 })
        for (bool low : { false, true })
        {
            autotune::PsolaShifter shifter;
            shifter.prepare(sr);
            shifter.setLowLatency(low);
            shifter.setParameters(1.12f, (float)(sr/210.0));
            const int n = (int)(sr * 1.15);
            double phase = 0.0;
            double dryPower = 0.0, wetPower = 0.0;
            double noiseInput = 0.0, noiseOutput = 0.0;
            double worstStep = 0.0;
            float old = 0.0f;
            uint32_t seed = 0x12345678;
            const int voicedEnd=(int)(sr * 0.85);
            const int levelBegin=(int)(sr * 0.25);
            const int noiseBegin=voicedEnd+(int)(sr * 0.04);
            uint64_t firstGaps=0;
            for (int i=0; i<n; ++i)
            {
                // The input is a phase-continuous multi-harmonic vowel-like
                // waveform, then an explicitly unpitched noise consonant.
                if (i == voicedEnd)
                {
                    firstGaps=shifter.getDropoutSamples();
                    shifter.setParameters(1.0f,0.0f);
                }
                const float x = i < voicedEnd
                    ? (float)(0.18 * std::sin(phase) + 0.07 * std::sin(2*phase + .4)
                               + 0.045 * std::sin(3*phase + .7))
                    : 0.20f*((prng(seed)&65535u)/32767.5f-1.0f);
                phase += 2.0 * 3.14159265358979323846 * 210.0 / sr;
                float y=shifter.processSample(x);
                if (!std::isfinite(y) || std::abs(y)>2.0f)
                    { std::fprintf(stderr,"NaN or runaway sr %.0f mode %d at %d\n",sr,(int)low,i);return 1; }
                if (i>levelBegin && i<voicedEnd)
                {
                    dryPower += (double)x*x;
                    wetPower += (double)y*y;
                    worstStep=std::max(worstStep, (double)std::abs(y-old));
                }
                if (i>noiseBegin)
                {
                    noiseInput+=(double)x*x;
                    noiseOutput+=(double)y*y;
                }
                old=y;
            }
            const double level=std::sqrt(wetPower/std::max(dryPower,1e-20));
            const double noise=std::sqrt(noiseOutput/std::max(noiseInput,1e-20));
            const double gap=(double)firstGaps/(double)voicedEnd;
            if (level<.82 || level>1.18 || noise<.84 || noise>1.16 || worstStep>.33 || gap>.01)
            {
                std::fprintf(stderr,"FAIL sr %.0f low %d voicedRMS %.3f noiseRMS %.3f maxStep %.3f gap %.4f\n",
                    sr,(int)low,level,noise,worstStep,gap);
                return 2;
            }
            std::printf("PASS sr %.0f low %d voicedRMS %.3f noiseRMS %.3f maxStep %.3f gap %.4f\n",
                sr,(int)low,level,noise,worstStep,gap);
        }
    return 0;
}
