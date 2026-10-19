// Low latency recovery: unvoiced CPU path, voiced reacquisition, alias FIR reprepare.
#include "RealtimeAutotune.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>

static uint32_t noise(uint32_t& s) {s^=s<<13;s^=s>>17;s^=s<<5;return s;}
int main()
{
    const double pi=std::acos(-1.0);
    // With recycled detector objects, the 63-tap filter must not leak into the
    // new 31-tap mode: the *actual* coefficient sum must stay unity.
    for(double first:{44100.,48000.,96000.})
        for(double second:{44100.,48000.,96000.})
        {
            autotune::YinDetector d;
            d.prepare(first);
            d.prepare(second);
            const float dc=d.getDecimatorCoefficientSum();
            if(std::abs(dc-1.0f)>.0002f)
            {
                std::fprintf(stderr,"FAIL FIR filter stale tap DC %.4f rate %.0f -> %.0f\n",dc,first,second);
                return 1;
            }
        }

    for(double sr:{44100.,48000.,96000.})
        for(bool low:{false,true})
        {
            autotune::PsolaShifter sh;
            sh.prepare(sr);
            sh.setLowLatency(low);
            const float period=(float)(sr/154.0);
            sh.setParameters(1.35f,period);
            double phase=0;
            for(int i=0;i<(int)(sr*.22);++i)
            {
                phase+=2*pi*154.0/sr;
                if(!std::isfinite(sh.processSample(.21f*(float)std::sin(phase))))return 2;
            }
            if(sh.getSpawnedGrains()==0) return 3;
            const auto before=sh.getSpawnedGrains();
            sh.setParameters(1.0f,0.0f);
            uint32_t seed=12345;
            double inEnergy=0,outEnergy=0;
            const int n=(int)(sr*.33);
            for(int i=0;i<n;++i)
            {
                const float x=.22f*((noise(seed)&65535u)/32767.5f-1.0f);
                const float y=sh.processSample(x);
                if(!std::isfinite(y))return 4;
                if(i>n/2){inEnergy+=(double)x*x;outEnergy+=(double)y*y;}
            }
            if(sh.getSpawnedGrains()!=before)
            {
                std::fprintf(stderr,"FAIL unvoiced fake grains rate=%.0f low=%d count=%llu\n",sr,(int)low,(unsigned long long)(sh.getSpawnedGrains()-before));
                return 5;
            }
            const double relative=std::sqrt(outEnergy/std::max(1.e-20,inEnergy));
            if(relative<.94 || relative>1.06)
            {
                std::fprintf(stderr,"FAIL consonant level %.3f rate=%.0f low=%d\n",relative,sr,(int)low);
                return 6;
            }
            // The first voiced note following a breath must restart the grain
            // clock and produce valid resynthesis, not stick to a dry pass-through.
            sh.setParameters(1.18f,period);
            for(int i=0;i<(int)(sr*.16);++i)
            {
                phase+=2*pi*154.0/sr;
                if(!std::isfinite(sh.processSample(.21f*(float)std::sin(phase))))return 7;
            }
            if(sh.getSpawnedGrains()==before || sh.getGrainSaturationCount()>0) return 8;
            std::printf("PASS %.0fHz mode=%s unvoiced spawn=0 RMS %.4f and clean note return\n",sr,low?"low":"normal",relative);
        }
}
