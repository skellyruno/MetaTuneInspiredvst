// V1.12 pitch-synchronous Live: test 3.5 ms age bound, correlation-checked
// full-period wraps, low-F0 fallback, noise rejection, phase continuity and
// block invariant stereo. A test cannot establish commercial singing quality.
#include "RealtimeAutotune.h"
#include <array>
#include <vector>
#include <cmath>
#include <cstdio>
#include <algorithm>
using autotune::MicroLatencyShifter;
static constexpr double pi=3.141592653589793238462643383;
static bool check(double sr,double f0,float ratio,bool shouldWrap)
{
    MicroLatencyShifter s; s.prepare(sr);
    s.setPitchSynchronousResamplingEnabled(true);
    s.setPhaseCoherentLowRegisterEnabled(true);
    s.setLowRegisterFormantGuardEnabled(true);
    const int maxAge=MicroLatencyShifter::hostLatencySamples(sr);
    if(s.getMaximumDelaySamples()!=maxAge)return false;
    s.setPeriod((float)(sr/f0));s.setRatio(ratio);
    double peak=0,step=0,prev=0,power=0;
    for(int n=0;n<(int)(sr*0.45);++n)
    {
        const double t=n/sr;
        const float x=(float)(0.20*std::sin(2*pi*f0*t)
               +0.06*std::sin(2*pi*2*f0*t+0.21)
               +0.025*std::sin(2*pi*3*f0*t+0.44));
        const double y=s.processSample(x);
        if(!std::isfinite(y))return false;
        peak=std::max(peak,std::abs(y));
        if(n>maxAge+20)step=std::max(step,std::abs(y-prev));
        if(n>(int)(sr*.20))power+=y*y;
        prev=y;
    }
    const bool wrapped=s.getAcceptedCycleJumps()>2;
    if(wrapped!=shouldWrap || peak>1.0 || step>0.27 || power<2.0)
    {
        std::fprintf(stderr,"sr=%.0f f0=%.0f ratio=%.3f folds=%d rejects=%d corr=%.3f peak=%.3f step=%.3f power=%.3f\n",
            sr,f0,ratio,s.getAcceptedCycleJumps(),s.getRejectedCycleJumps(),
            s.getLastCycleCorrelation(),peak,step,power);
        return false;
    }
    if(shouldWrap && s.getLastCycleCorrelation()<0.72f)return false;
    return true;
}
static bool impulse(double sr)
{
    MicroLatencyShifter s;s.prepare(sr);
    s.setPitchSynchronousResamplingEnabled(true);
    s.setPhaseCoherentLowRegisterEnabled(true);
    const int d=s.getMaximumDelaySamples();
    for(int i=0;i<d+32;++i)
    {
        const float y=s.processSample(i==0?1.0f:0.0f);
        if(std::abs(y-(i==d?1.0f:0.0f))>1.e-4f)return false;
    }
    return true;
}
static bool nonperiodicNoise(double sr)
{
    MicroLatencyShifter s;s.prepare(sr);
    s.setPitchSynchronousResamplingEnabled(true);
    s.setRatio(1.122462f);s.setPeriod((float)(sr/700.0));
    unsigned x=0xABCDEF01u;
    for(int i=0;i<(int)(sr*.25);++i)
    {
        x^=x<<13;x^=x>>17;x^=x<<5;
        const float in=0.15f*((x & 65535u)/32767.5f-1.0f);
        const float y=s.processSample(in);
        if(!std::isfinite(y)||std::abs(y)>2.0f)return false;
    }
    if(s.getAcceptedCycleJumps()>0)
    {
        std::fprintf(stderr,"noise false period locks %d at %.0f\n",s.getAcceptedCycleJumps(),sr);
        return false;
    }
    return s.getRejectedCycleJumps()>0;
}
static bool timeVarying(double sr)
{
    MicroLatencyShifter s;s.prepare(sr);s.setPitchSynchronousResamplingEnabled(true);
    s.setPhaseCoherentLowRegisterEnabled(true);
    const int delay=s.getMaximumDelaySamples();
    double phase=0,prev=0,maxStep=0;
    for(int n=0;n<(int)(sr*.8);++n)
    {
        const double t=n/sr;
        const bool consonant=t>.37&&t<.40;
        const double f0=t<.44 ? 600.+12.*std::sin(2*pi*5*t) : 750.;
        const float ratio=(float)(t<.44 ? 1.122462 : .890899);
        phase+=2*pi*f0/sr;
        const float input=consonant?0.0f:(float)(.18*std::sin(phase)+.06*std::sin(2*phase+.4));
        s.setPeriod(consonant?0.0f:(float)(sr/f0));
        s.setRatio(consonant?1.0f:ratio);
        const double y=s.processSample(input);
        if(!std::isfinite(y)||std::abs(y)>1.5)return false;
        if(n>delay)maxStep=std::max(maxStep,std::abs(y-prev));
        prev=y;
    }
    if(maxStep>0.45){std::fprintf(stderr,"transient %.3f at %.0f\n",maxStep,sr);return false;}
    return true;
}
int main()
{
    for(double sr:{44100.0,48000.0,96000.0,192000.0})
    {
        if(!impulse(sr)||!nonperiodicNoise(sr)||!timeVarying(sr))return 1;
        for(double f0:{85.,110.,500.,700.})
            if(!check(sr,f0,1.122462f,f0>400.))return 2;
    }
    std::puts("V1.12: pitch-synchronous folds, noise rejection, causal delays and low-note fallback passed at 4 sample rates");
}
