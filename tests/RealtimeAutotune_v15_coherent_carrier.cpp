// V1.5: finite-bounded, causal, sample-block-independent harmonic synthesis.
// This does NOT prove perfect 3.5ms wet timing or listening quality.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>
using autotune::MicroLatencyShifter;
static constexpr double pi=3.14159265358979323846;
struct Fourier {
    double re=0,im=0;
    void add(double x,double p) {re+=x*std::cos(p);im+=x*std::sin(p);}
    double magnitude() const {return std::hypot(re,im);}
};
static bool stationary(double f0,double ratio,int seed=0) {
    constexpr double sr=48000.0;
    MicroLatencyShifter before,after;
    before.prepare(sr);after.prepare(sr);
    before.setLowRegisterFormantGuardEnabled(true);after.setLowRegisterFormantGuardEnabled(true);
    after.setPhaseCoherentLowRegisterEnabled(true);
    before.setRatio((float)ratio);after.setRatio((float)ratio);
    before.setPeriod((float)(sr/f0));after.setPeriod((float)(sr/f0));
    if(before.getMaximumDelaySamples()!=168||after.getMaximumDelaySamples()!=168)return false;
    std::array<Fourier,9> oldBefore{},oldAfter{},targetBefore{},targetAfter{};
    double rms0=0,rms1=0,maxStep0=0,maxStep1=0,prev0=0,prev1=0;
    constexpr int count=48000;
    for(int i=0;i<count;++i){
        const double t=i/sr;
        double x=0;
        for(int k=1;k<=24;++k){
            const double hz=f0*k;
            const double env=0.11*std::exp(-0.5*std::pow((hz-500.0-80*seed)/195.0,2))
                           +0.15*std::exp(-0.5*std::pow((hz-1130.0-130*seed)/290.0,2))
                           +0.07*std::exp(-0.5*std::pow((hz-2500.0)/420.0,2))+.018;
            x+=env/std::sqrt((double)k)*std::sin(2*pi*hz*t+.2*k+.015*k*k);
        }
        x*=.34;
        const double a=before.processSample((float)x), b=after.processSample((float)x);
        if(!std::isfinite(a)||!std::isfinite(b)||std::abs(b)>2.0)return false;
        if(i>count/2){
            rms0+=a*a; rms1+=b*b;
            maxStep0=std::max(maxStep0,std::abs(a-prev0));
            maxStep1=std::max(maxStep1,std::abs(b-prev1));
            for(int k=2;k<=9;++k){
                oldBefore[(size_t)k-1].add(a,2*pi*f0*k*t);
                oldAfter[(size_t)k-1].add(b,2*pi*f0*k*t);
                targetBefore[(size_t)k-1].add(a,2*pi*f0*ratio*k*t);
                targetAfter[(size_t)k-1].add(b,2*pi*f0*ratio*k*t);
            }
        }
        prev0=a;prev1=b;
    }
    double ob=0,oa=0,tb=0,ta=0;
    for(int k=2;k<=9;++k){if(f0*k*(ratio-1)<16)continue;
        ob+=oldBefore[(size_t)k-1].magnitude();oa+=oldAfter[(size_t)k-1].magnitude();
        tb+=targetBefore[(size_t)k-1].magnitude();ta+=targetAfter[(size_t)k-1].magnitude();}
    const double leak0=20*std::log10((ob+1.e-5)/(tb+1.e-5));
    const double leak1=20*std::log10((oa+1.e-5)/(ta+1.e-5));
    const double levelDb=10*std::log10((rms1+1.e-9)/(rms0+1.e-9));
    if(leak1>leak0-9.0||std::abs(levelDb)>3.0||maxStep1>0.6){
        std::fprintf(stderr,"case failed f0=%.2f ratio=%.4f leak=%.2f->%.2f gain=%.2f jump=%.2f\n", f0,ratio,leak0,leak1,levelDb,maxStep1);
        return false;
    }
    return true;
}
static bool transition(double sr){
    MicroLatencyShifter a;a.prepare(sr);a.setPhaseCoherentLowRegisterEnabled(true);
    a.setLowRegisterFormantGuardEnabled(true);
    if(a.getMaximumDelaySamples()!=(int)std::ceil(sr*.0035))return false;
    double srcPhase=0,prev=0,maxStep=0;
    for(int i=0;i<(int)(sr*.9);++i){
        const double t=i/sr;
        // Vibrato followed by octave-like source change and a consonant gap.
        const bool gap=t>.41&&t<.50;
        const double f0=t<.55 ? 92.*std::pow(2.,(0.35*std::sin(2*pi*5*t))/12.) : 148.;
        srcPhase+=2*pi*f0/sr;
        const float x=gap?0.f:(float)(.17*std::sin(srcPhase)+.11*std::sin(2*srcPhase+.2)+.08*std::sin(3*srcPhase+.1));
        a.setPeriod(gap?0.0f:(float)(sr/f0));
        a.setRatio(gap?1.0f:(t<.55?1.122462f:1.059463f));
        const double y=a.processSample(x);
        if(!std::isfinite(y)||std::abs(y)>2.0){std::fprintf(stderr,"NaN at %d\n",i);return false;}
        if(i>(int)(sr*.05)&&!gap)maxStep=std::max(maxStep,std::abs(y-prev));
        prev=y;
    }
    if(maxStep>.55){std::fprintf(stderr,"Transition click max %.3f sr %.0f\n",maxStep,sr);return false;}
    return true;
}
int main(){
    for(double f0:{75.0,82.4,92.0,110.0,140.0,165.0,195.0})
        for(double ratio:{1.059463094,1.122462048,1.189207115})
            if(!stationary(f0,ratio))return 2;
    for(double sr:{44100.0,48000.0,96000.0,192000.0})
        if(!transition(sr))return 3;
    std::puts("V1.5: 21 stationary cases rejected source F0 by >=9 dB relative to dual-head; 4 time-varying multirate transitions finite and bounded; 3.5ms ring unchanged");
}
