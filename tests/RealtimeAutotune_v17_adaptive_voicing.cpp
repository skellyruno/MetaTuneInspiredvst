// V1.7 regression: shorter adaptive carrier acquisition must not mistake
// consonant noise for a voiced vowel or revive stale pre-gap oscillator state.
// These are synthesis-only tests with supplied periods, not detector latency.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
using autotune::MicroLatencyShifter;
constexpr double pi = 3.14159265358979323846;
static bool steadyAcquisition(double sr,double f0)
{
    MicroLatencyShifter s; s.prepare(sr);
    s.setPhaseCoherentLowRegisterEnabled(true);
    s.setRatio(1.122462f); s.setPeriod((float)(sr/f0));
    if(s.getMaximumDelaySamples() != (int)std::ceil(sr*.0035)) return false;
    double phase=0, last=0,peak=0;
    int half=-1, ninety=-1;
    for(int i=0;i<(int)(sr*.085);++i)
    {
        phase += 2*pi*f0/sr;
        const float x=(float)(.19*std::sin(phase) + .11*std::sin(2*phase+.17)
                     + .06*std::sin(3*phase-.23) + .025*std::sin(4*phase+.4));
        const double y=s.processSample(x);
        if(!std::isfinite(y))return false;
        peak=std::max(peak,std::abs(y-last));last=y;
        if(half<0 && s.getHarmonicCarrierMix()>=.5f)half=i;
        if(ninety<0 && s.getHarmonicCarrierMix()>=.9f)ninety=i;
    }
    std::printf("steady %.0fHz at %.0fk 50%% %.2fms, 90%% %.2fms, step %.4f\n",
                f0,sr/1000.,1000.0*half/sr,1000.0*ninety/sr,peak);
    return half>=0 && (double)half/sr < .0295 && (double)ninety/sr < .037
        && (double)half/sr > .010 && peak < .45;
}
static bool noiseRefusal(double sr)
{
    MicroLatencyShifter s;s.prepare(sr);s.setPhaseCoherentLowRegisterEnabled(true);
    s.setRatio(1.122462f);s.setPeriod((float)(sr/95.0));
    uint32_t seed=1876543u;
    float maxMix=0;
    for(int i=0;i<(int)(sr*.10);++i)
    {
        seed=1664525u*seed+1013904223u;
        const float x=.16f*((float)((seed>>16)&65535u)/32767.5f-1.0f);
        const float y=s.processSample(x);
        if(!std::isfinite(y)) return false;
        maxMix=std::max(maxMix,s.getHarmonicCarrierMix());
    }
    std::printf("stale F0 noise at %.0fk: maximum carrier mix %.5f\n",sr/1000,maxMix);
    return maxMix < 0.06f;
}
static bool breathyVowel(double sr)
{
    MicroLatencyShifter s;s.prepare(sr);s.setPhaseCoherentLowRegisterEnabled(true);
    s.setRatio(1.122462f);s.setPeriod((float)(sr/82.));
    uint32_t seed=92731u;
    double ph=0.0;int half=-1;
    for(int i=0;i<(int)(sr*.080);++i)
    {
        ph+=2*pi*82./sr;
        seed=1664525u*seed+1013904223u;
        const float breath=.045f*((float)((seed>>16)&65535u)/32767.5f-1.0f);
        const float voiced=(float)(.19*std::sin(ph)+.11*std::sin(2*ph+.20)
                             +.06*std::sin(3*ph-.13));
        const float y=s.processSample(voiced+breath);
        if(!std::isfinite(y)) return false;
        if(half<0 && s.getHarmonicCarrierMix() >= .5f) half=i;
    }
    std::printf("breathy 82Hz at %.0fk 50%% carrier at %.2fms, model %.3f\n",
                sr/1000.,1000.0*half/sr,s.getHarmonicModelFraction());
    return half>=0 && half/sr<.033;
}
static bool phraseBoundary(double sr)
{
    MicroLatencyShifter s;s.prepare(sr);s.setPhaseCoherentLowRegisterEnabled(true);
    s.setLowRegisterFormantGuardEnabled(true);s.setRatio(1.122462f);
    double phase=0,last=0,maxStep=0,noiseError=0,noiseEnergy=0;
    uint32_t seed=3526731u;
    const int n=(int)(sr*.25), a=(int)(sr*.095), b=(int)(sr*.125);
    int release=-1,reacquire=-1;
    for(int i=0;i<n;++i)
    {
        const bool noise=i>=a&&i<b;
        const double f0=i<b?95.:156.;
        if(i==0)s.setPeriod((float)(sr/95.));
        if(i==a)s.setPeriod(0.0f);
        if(i==b){s.setPeriod((float)(sr/156.));s.setRatio(1.189207f);}
        phase+=2*pi*f0/sr;
        seed=1664525u*seed+1013904223u;
        const float x=noise?.17f*((float)((seed>>16)&65535u)/32767.5f-1.0f)
            :(float)(.20*std::sin(phase)+.11*std::sin(2*phase+.2)
                           + .05*std::sin(3*phase-.3));
        const double y=s.processSample(x),dry=s.getLastAlignedDry();
        if(!std::isfinite(y))return false;
        if(i>200) maxStep=std::max(maxStep,std::abs(y-last));
        if(noise && i>a+(int)(sr*.007)){
            noiseError += std::abs(y-dry);noiseEnergy+=std::abs(dry);
        }
        if(i>=a&&release<0&&s.getHarmonicCarrierMix()<.10f) release=i-a;
        if(i>=b&&reacquire<0&&s.getHarmonicCarrierMix()>.50f) reacquire=i-b;
        last=y;
    }
    const double relativeNoise=noiseError/std::max(1e-12,noiseEnergy);
    std::printf("phrase %.0fk release %.2fms reacquire %.2fms noise delta %.4f step %.4f\n",
         sr/1000.,1000.0*release/sr,1000.0*reacquire/sr,relativeNoise,maxStep);
    return release>=0 && release/sr<.0045 && reacquire>=0 && reacquire/sr<.034
        && relativeNoise<.18 && maxStep<.45;
}
int main()
{
    for(double sr:{44100.,48000.,96000.,192000.})
    {
        for(double f0:{75.,82.,110.,145.,195.})
            if(!steadyAcquisition(sr,f0))return 1;
        if(!noiseRefusal(sr))return 2;
        if(!breathyVowel(sr))return 4;
        if(!phraseBoundary(sr))return 3;
    }
    std::puts("PASS adaptive carrier onset, stale-F0 noise, and consonant-to-vowel handoff");
}
