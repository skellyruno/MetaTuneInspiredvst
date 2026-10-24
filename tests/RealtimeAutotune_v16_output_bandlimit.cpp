// Regression: the harmonic bank must band-limit the CORRECTED F0, not
// the original F0. Otherwise high output partials fold below Nyquist.
// This unit test probes the standalone carrier, not an external DAW.
#include "RealtimeAutotune.h"
#include <cmath>
#include <cstdio>
#include <algorithm>
static constexpr double pi = 3.14159265358979323846;
static double foldedAmplitude(double sr, double ratio)
{
    autotune::MicroLatencyShifter shifter;
    shifter.prepare(sr);
    shifter.setPhaseCoherentLowRegisterEnabled(true);
    shifter.setPeriod((float)(sr/190.0));
    shifter.setRatio((float)ratio);
    if (shifter.getMaximumDelaySamples() != (int)std::ceil(sr*0.0035)) return 1.e3;
    const double inputFrequency = 190.0*16.0;
    double aliasedFrequency = std::fmod(inputFrequency*ratio, sr);
    if (aliasedFrequency > sr/2.0) aliasedFrequency = sr-aliasedFrequency;
    const int total = (int)(sr*2.0);
    double re=0, im=0;
    for(int n=0;n<total;++n)
    {
        const double t=(double)n/sr;
        const float x=(float)(0.20*std::sin(2*pi*inputFrequency*t+0.3)
                             +0.04*std::sin(2*pi*190.0*t));
        const float y=shifter.processSample(x);
        if (!std::isfinite(y) || std::abs(y)>2.0f) return 1.e3;
        if(n>total/2)
        {
            re += y*std::cos(2*pi*aliasedFrequency*t);
            im += y*std::sin(2*pi*aliasedFrequency*t);
        }
    }
    return 2.0*std::hypot(re,im)/(total/2);
}
static bool automationIsFiniteAndContinuous(double sr)
{
    autotune::MicroLatencyShifter shifter;
    shifter.prepare(sr);
    shifter.setPhaseCoherentLowRegisterEnabled(true);
    shifter.setPeriod((float)(sr/190.0));
    const int n=(int)(sr*2.0);
    double last=0, step=0, peak=0;
    for(int i=0;i<n;++i)
    {
        const double t=(double)i/sr;
        // Cross several output partial boundaries while retaining the
        // original fixed 3.5ms ring size throughout automation.
        const float ratio=(float)(1.12+0.77*(0.5-0.5*std::cos(2*pi*t*0.8)));
        shifter.setRatio(ratio);
        const float x=(float)(0.18*std::sin(2*pi*3040.0*t)
                               +0.08*std::sin(2*pi*190.0*t));
        const double y=shifter.processSample(x);
        if(!std::isfinite(y)) return false;
        step=std::max(step,std::abs(y-last));
        peak=std::max(peak,std::abs(y));
        last=y;
    }
    return shifter.getMaximumDelaySamples()==(int)std::ceil(sr*.0035)
       && step < 0.8 && peak < 2.0;
}
int main()
{
    const double a=foldedAmplitude(8000.,1.4); // 4256Hz folds to 3744Hz
    const double b=foldedAmplitude(8000.,1.8); // 5472Hz folds to 2528Hz
    const double c=foldedAmplitude(16000.,1.8); // soft 5.6kHz limit
    const double d=foldedAmplitude(48000.,1.4); // in-band partial retained
    std::printf("8k_1.4=%.7f,8k_1.8=%.7f,16k_1.8=%.7f,48k_1.4=%.7f\n",a,b,c,d);
    if (!(a<0.01 && b<0.01 && c<0.10 && d>0.12)) return 2;
    if (!automationIsFiniteAndContinuous(8000.) || !automationIsFiniteAndContinuous(48000.)) return 3;
    return 0;
}
