// V9: phase templates from previous voiced notes must not survive large
// source-period jumps. Recovery must remain within the fast-only PDC budget.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
int main()
{
    constexpr double pi=3.14159265358979323846;
    int checked=0;
    for (double sr : {44100.,48000.,96000.})
        for (bool fast : {true,false})
        {
            autotune::PsolaShifter sh;
            sh.prepare(sr);sh.setLowLatency(fast);
            double phase=0.,outputEnergy=0.,maxPeak=0.,maxLatency=0.;
            const int segment=(int)(sr*.32);
            for(int i=0;i<segment*5;++i)
            {
                const int n=i/segment;
                const double hz=n==0?112.:(n==1?220.:(n==2?151.:(n==3?265.:132.)));
                const float shift=n%2==0?1.15f:.83f;
                phase+=2*pi*hz/sr;
                const float x=(float)(.18*std::sin(phase)+.07*std::sin(2*phase+.4)
                               +.04*std::sin(3*phase-.3));
                sh.setParameters(shift,(float)(sr/hz));
                const float y=sh.processSample(x);
                if(!std::isfinite(y))return 1;
                maxPeak=std::max(maxPeak,(double)std::abs(y));
                maxLatency=std::max(maxLatency,sh.getLatencySamples());
                if(i>segment/2)outputEnergy+=(double)y*y;
            }
            if(sh.getPhaseReacquisitions()<3 || sh.getGrainSaturationCount()!=0
                || maxPeak>1.6 || outputEnergy<10.0)
            {
                std::printf("FAIL reacq rate %.0f fast %d events %llu saturation %llu peak %.2f energy %.2f\n",
                    sr,(int)fast,(unsigned long long)sh.getPhaseReacquisitions(),
                    (unsigned long long)sh.getGrainSaturationCount(),maxPeak,outputEnergy);
                return 2;
            }
            const int fixedBudget=autotune::RealtimeAutotune::recommendedHostLatencySamples(sr,70.f,fast);
            if(maxLatency>fixedBudget+.5)
            {
                std::printf("FAIL latency %.1f > %d\n",maxLatency,fixedBudget);return 3;
            }
            std::printf("PASS %.0fHz fast=%d phase reacquisitions=%llu peak=%.3f\n",
                sr,(int)fast,(unsigned long long)sh.getPhaseReacquisitions(),maxPeak);
            ++checked;
        }
    return checked==6?0:4;
}
