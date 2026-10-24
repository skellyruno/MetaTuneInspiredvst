// V29: polarity-aware stereo pitch analysis keeps the detector's timeline
// continuous when louder-channel selection flips. No audio output summation.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr double pi=3.14159265358979323846;
struct Result { std::vector<float> notes; int switches=0, inverted=0; double peakError=0.; };
Result run(double sr, double hz, int blockSize, bool opposite)
{
    autotune::RealtimeAutotune engine;
    engine.prepare(sr);
    autotune::Settings p;
    p.lowLatency=true; p.ultraLowLatency=true; p.precisionHardTune=true;
    p.scale=autotune::Scale::Chromatic;
    engine.setSettings(p);
    Result out;
    const int count=(int)(sr*1.86); // three full 620-ms amplitude cycles
    int previous=0;
    for (int start=0;start<count;start+=blockSize)
    {
        const int n=std::min(blockSize,count-start);
        std::vector<float> left((size_t)n),right((size_t)n);
        for(int j=0;j<n;++j)
        {
            const int sample=start+j;
            const double cycle=std::fmod((double)sample/sr,0.62);
            const double a=cycle<0.31 ? 1. : 0.24;
            const double b=cycle<0.31 ? 0.24 : 1.;
            const double ph=2*pi*hz*sample/sr;
            const double v=0.24*(std::sin(ph)+0.20*std::sin(2*ph+0.2));
            left[(size_t)j]=(float)(a*v);
            right[(size_t)j]=(float)((opposite?-1.:1.)*b*v);
        }
        float* channels[]={left.data(),right.data()};
        engine.processChannels(channels,2,n);
        for (int j=0;j<n;++j)
            if(!std::isfinite(left[(size_t)j])||!std::isfinite(right[(size_t)j]))
                return {};
        if (engine.getAnalysisChannel()!=previous) {++out.switches;previous=engine.getAnalysisChannel();}
        if(engine.getAnalysisChannel()==1 && engine.getAnalysisPolarity()<0) ++out.inverted;
        if(start>0.25*sr)
        {
            const float midi=engine.getDetectedMidi();
            if(!std::isfinite(midi)||midi<0) {out.peakError=1e6;continue;}
            const double pitch=440.0*std::exp2(((double)midi-69.0)/12.0);
            const double err=std::abs(1200.0*std::log2(pitch/hz));
            out.peakError=std::max(out.peakError,err);
            out.notes.push_back(midi);
        }
    }
    return out;
}
}
int main()
{
    for(double sr:{44100.,48000.,96000.})
    for(double hz:{180.,440.})
    for(bool opposite:{false,true})
    {
        auto a=run(sr,hz,64,opposite);
        auto b=run(sr,hz,257,opposite);
        if(a.notes.empty()||b.notes.empty()||a.switches<2||b.switches<2||
           a.peakError>65.0||b.peakError>65.0||
           (opposite&&(a.inverted==0||b.inverted==0))||
           (!opposite&&(a.inverted!=0||b.inverted!=0)))
        {
            std::fprintf(stderr,"handoff failed sr=%.0f hz=%.0f opposite=%d peak=%.2f %.2f switch=%d %d inverse=%d %d\n",
                         sr,hz,(int)opposite,a.peakError,b.peakError,
                         a.switches,b.switches,a.inverted,b.inverted);
            return 1;
        }
        std::printf("sr %.0f hz %.0f opposite %d peak %.2f / %.2f cents; switches %d / %d\n",
                    sr,hz,(int)opposite,a.peakError,b.peakError,a.switches,b.switches);
    }
    return 0;
}
