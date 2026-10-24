// V1.11: temporal note tracking / reacquisition; no audio lookahead added.
#include "RealtimeAutotune.h"
#include <cmath>
#include <cstdio>
using namespace autotune;
namespace {
constexpr double pi=3.14159265358979323846;
Settings live(){Settings s;s.strictRealtime=s.lowLatency=s.ultraLowLatency=true;s.follow=0;s.noteStabilizerMs=300;s.noteConfirmMs=10;s.retuneMs=0;s.voicingThreshold=.8f;return s;}
struct Counts {int falseNote=0, unvoiced=0, firstTarget=-1;};
Counts run(double sr,double destination,double durationMs,double attackGain) {
    RealtimeAutotune e;e.prepare(sr);e.setSettings(live());
    int start=(int)std::lround(sr*.25), stop=(int)std::lround(sr*.45), transitionEnd=start+(int)std::lround(sr*durationMs*.001);
    int final=(int)std::lround(sr*.35), wanted=std::lround(69+12*std::log2(destination/440));
    double phase=0;Counts c;
    for(int i=0;i<stop;i++){
        double f=i>=start&&i<transitionEnd?destination:220.0;
        phase+=2*pi*f/sr;
        double gain=(i>=start&&i<transitionEnd)?attackGain:1.0;
        float x=(float)(gain*(.21*std::sin(phase)+.07*std::sin(2*phase+.2)+.02*std::sin(3*phase-.4)));
        if(!std::isfinite(e.process(x)))return {1000000,1000000,-1};
        if(i>=start&&i<final){
            int note=e.getTargetMidi();
            if(note>=0 && note!=57 && (durationMs <= 17.0 || note!=wanted))++c.falseNote;
            if(note==-1)++c.unvoiced;
            if(note==wanted&&c.firstTarget<0)c.firstTarget=i-start;
        }
    }
    return c;
}
}
int main() {
    for(double sr:{44100.,48000.,96000.}){
        if(RealtimeAutotune::ultraLowLatencyHostSamples(sr)!=(int)std::ceil(sr*.0035))return 1;
        // A 10ms false octave with NO independent onset may trigger temporary
        // detector dropout; once stable pitch returns it MUST NOT invent a new
        // musical note for a sustained tail. Count wrong notes separately
        // from honest unvoiced status.
        const Counts octave=run(sr,440.0,10.0,1.0);
        if(octave.falseNote!=0 || octave.unvoiced>(int)(sr*.03)){
          std::printf("FAIL %.0fHz octave burst: wrong-note=%d unvoiced=%d\n",sr,octave.falseNote,octave.unvoiced);return 2;
        }
    }
    for(const auto p:{std::pair<double,double>{246.9417,20.0},{261.6256,23.0},{293.6648,25.0}}){
        // A reliable rise in amplitude and F0 can recenter sooner, but should
        // not turn noise-only or no-onset glitches into new target notes.
        const Counts onset=run(48000.0,p.first,190.0,1.7);
        const double ms=onset.firstTarget*1000.0/48000.0;
        if(onset.firstTarget<0||ms>p.second||onset.falseNote){
            std::printf("FAIL onset %.1fHz: %.3fms spurious=%d\n",p.first,ms,onset.falseNote);return 3;
        }
    }
    std::puts("PASS V1.11 reliable-onset acceleration and post-dropout note reentry");
}
