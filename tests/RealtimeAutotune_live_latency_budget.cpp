// The fast-only VST3's fixed host budget must cover observed intrinsic delay.
// Synthetic sine and harmonic sources are NOT a bound on arbitrary real voices.
#include "RealtimeAutotune.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>
int main()
{
    constexpr double pi=3.1415926535897932384626;
    for (double sr : {44100.,48000.,96000.})
    {
        const int liveBudget=autotune::RealtimeAutotune::recommendedHostLatencySamples(sr,70.0f,true);
        const int normalBudget=autotune::RealtimeAutotune::recommendedHostLatencySamples(sr,70.0f,false);
        if(liveBudget>=normalBudget || liveBudget<sr/70.0) return 1;
        // Deliberately stress low pitches, large shifts, transient ratio changes
        // and voiced/unvoiced returns, measuring every single sample.
        autotune::PsolaShifter fast;
        fast.prepare(sr);fast.setLowLatency(true);
        const std::array<float,8> pitches{70.f,82.f,100.f,140.f,200.f,440.f,75.f,110.f};
        const std::array<float,8> shifts{0.5f,0.75f,1.2f,1.5f,2.0f,1.0f,2.0f,0.67f};
        double phase=0.0,observed=0.0;
        const int section=(int)(sr*.23);
        for(int i=0;i<section*(int)pitches.size();++i)
        {
            const int segment=i/section;
            const float freq=pitches[(size_t)segment];
            const bool consonant=(i%section > (int)(sr*.15) && i%section < (int)(sr*.17));
            fast.setParameters(shifts[(size_t)segment],consonant?0.0f:(float)(sr/freq));
            phase+=2.0*pi*(double)freq/sr;
            const float x=.19f*(float)std::sin(phase)+.06f*(float)std::sin(2.0*phase+.5);
            if(!std::isfinite(fast.processSample(x)))return 2;
            observed=std::max(observed,fast.getLatencySamples());
            if(fast.getLatencySamples() > (double)liveBudget + 0.5)
            {
                std::fprintf(stderr,"FAIL live underbudget sample-rate %.0f delay %.2fms budget %.2fms\n",
                    sr,1000.0*observed/sr,1000.0*liveBudget/sr);
                return 3;
            }
        }
        std::printf("PASS Live PDC %.0fHz: %.2fms vs switchable %.2fms; max measured %.2fms\n",
            sr,1000.*liveBudget/sr,1000.*normalBudget/sr,1000.*observed/sr);
    }
}
