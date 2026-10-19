// Deterministic regression: missing fundamentals and genuine octave changes.
// A first-threshold YIN valley alone cannot discriminate every harmonic stack.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr double pi = 3.14159265358979323846;
struct Score {int valid = 0, accurate = 0, octaveErrors = 0;double maxBadRunMs = 0.;};
Score run(double sr, int style)
{
    autotune::YinDetector pitch;
    pitch.prepare(sr, 70.f, 1000.f);
    Score s;
    const int duration = (int)(sr * .75);
    const int split = (int)(sr * .32);
    double ph = 0.;
    int badRun=0;
    for (int i = 0; i < duration; ++i)
    {
        const bool second = i >= split;
        const double f0 = style==2 && second ? 220. : (style==3 && second ? 110. : (style==3 ? 220. : 110.));
        ph += 2.0*pi*f0/sr;
        float x=0.f;
        if(style == 1)
            x=(float)(.20*std::sin(2*ph)+.02*std::sin(3*ph)+.02*std::sin(4*ph));
        else
            x=(float)(.20*std::sin(ph)+.08*std::sin(2*ph)+.05*std::sin(3*ph));
        if(!pitch.push(x))continue;
        // Exclude detector warm-up and recognition time after the step.
        const bool test = i > (int)(sr*.15) && (style < 2 || i < split-(int)(sr*.02) || i > split+(int)(sr*.08));
        if(!test)continue;
        const double measured=pitch.getFrequency();
        if(measured==0)continue;
        ++s.valid;
        const double cents=1200.*std::log2(measured/f0);
        if(std::abs(cents) < 20.){++s.accurate;badRun=0;}
        else {++badRun; if(std::abs(cents)>700.) ++s.octaveErrors;}
        s.maxBadRunMs=std::max(s.maxBadRunMs,badRun*pitch.getHopSeconds()*1000.);
    }
    return s;
}
}
int main()
{
    for (double sr : {44100.,48000.,96000.})
        for (int style=0;style<4;++style)
        {
            const Score r=run(sr,style);
            const double rate=(double)r.accurate/std::max(1,r.valid);
            std::printf("%.0fHz %s accurate %.1f%% octaveErrors=%d maxBadRun=%.1fms\n",
                sr,style==0?"harmonic":style==1?"weak-fundamental":style==2?"octave-up":"octave-down",
                100.*rate,r.octaveErrors,r.maxBadRunMs);
            if(r.valid<40 || rate<.97 || r.octaveErrors>2)return 1;
        }
    return 0;
}
