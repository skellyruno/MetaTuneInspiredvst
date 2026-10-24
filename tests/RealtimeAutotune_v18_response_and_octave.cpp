// V1.8: full-path note response, causal YIN onset, temporary octave rejection,
// and unaltered 3.5ms Live buffer. Not proof of 3.5ms pitch recognition.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

using namespace autotune;
constexpr double pi = 3.14159265358979323846;
struct Timing { double detectorMs = -1, targetMs = -1; int finalTarget = -1; bool finite = true; };
static Timing transition(double sr, bool precision, double before, double after)
{
    RealtimeAutotune e; e.prepare(sr);
    Settings s; s.strictRealtime = s.lowLatency = s.ultraLowLatency = true;
    s.precisionHardTune=precision;s.follow=1;s.noteStabilizerMs=0;
    s.retuneMs=0;s.voicingThreshold=.80f;s.noteConfirmMs=10.0f;
    e.setSettings(s);
    const int start=(int)std::lround(sr*.25), total=(int)std::lround(sr*.48);
    const float targetMidi=(float)(69.0 + 12.0*std::log2(after/440.0));
    const int target=(int)std::lround(targetMidi);
    double phase=0.0;Timing t;uint64_t prevHop=0;
    for(int i=0;i<total;++i)
    {
        const double f=i<start?before:after;
        phase+=2*pi*f/sr;
        const float x=(float)(.21*std::sin(phase)+.07*std::sin(2*phase+.2)+.02*std::sin(3*phase-.4));
        const float y=e.process(x);
        if(!std::isfinite(y))t.finite=false;
        const auto d=e.getDiagnostics();
        if (i>=start)
        {
            if(t.detectorMs<0 && d.analysisHop>prevHop
               && d.confidence>.80f && std::abs(d.detectedMidi-targetMidi)<.5f)
                t.detectorMs=1000.*(i-start)/sr;
            if(t.targetMs<0 && d.targetMidi==target)
                t.targetMs=1000.*(i-start)/sr;
        }
        prevHop=d.analysisHop;
        if(i==total-1)t.finalTarget=d.targetMidi;
    }
    return t;
}
static bool glitchIsRejected(double sr, bool precision)
{
    RealtimeAutotune e;e.prepare(sr);
    Settings s;s.strictRealtime=s.lowLatency=s.ultraLowLatency=true;
    s.precisionHardTune=precision;s.follow=1;s.noteStabilizerMs=0;
    s.voicingThreshold=.80f;s.retuneMs=0;
    e.setSettings(s);
    const int glitchStart=(int)(sr*.22), glitchEnd=glitchStart+(int)(sr*.0015);
    double phase=0.;int correct=69+ (int)std::lround(12*std::log2(220./440.));
    int jumps=0;bool finite=true;
    for(int i=0;i<(int)(sr*.40);++i)
    {
        const double f=(i>=glitchStart && i<glitchEnd) ? 440.:220.;
        phase+=2*pi*f/sr;
        float y=e.process((float)(.23*std::sin(phase)+.05*std::sin(2*phase)));
        if (!std::isfinite(y))finite=false;
        if(i>glitchStart && i<glitchStart+(int)(sr*.07)
           && e.getTargetMidi()!=correct){ ++jumps;}
    }
    return finite && jumps==0;
}
static bool blockInvariant(double sr)
{
    struct AudioRun { std::vector<float> rendered; uint64_t hops=0, fastSamples=0; };
    const auto run = [sr](int block, bool repeat)->AudioRun
    {
        RealtimeAutotune e; e.prepare(sr);
        Settings s; s.strictRealtime=s.lowLatency=s.ultraLowLatency=true;
        s.precisionHardTune=false;s.follow=1;s.noteStabilizerMs=0;
        s.voicingThreshold=.80f;s.retuneMs=0;s.noteConfirmMs=10;
        e.setSettings(s);
        const int total=(int)(sr*.48);
        AudioRun result; result.rendered.resize((size_t)total);
        double phase=0;
        for(int i=0;i<total;++i)
        {
            const double f=i<(int)(sr*.2)?110.:146.832;
            phase+=2*pi*f/sr;
            const double attack=i<(int)(sr*.015)? 0.5 : 1.0;
            result.rendered[(size_t)i]=(float)(attack*(.21*std::sin(phase)+.07*std::sin(2*phase+.2)));
        }
        for(int first=0;first<total;first+=block)
        {
            if(repeat)e.setSettings(s);
            float* ch[]={result.rendered.data()+first};
            e.processChannels(ch,1,std::min(block,total-first));
        }
        result.hops=e.getDiagnostics().analysisHop;
        result.fastSamples=e.getFastAnalysisSamples();
        return result;
    };
    const auto a=run(64,false),b=run(257,false),c=run(257,true);
    const bool ok=a.rendered==b.rendered && a.rendered==c.rendered
        && a.hops==b.hops && a.hops==c.hops
        && a.fastSamples==b.fastSamples && a.fastSamples==c.fastSamples;
    if(!ok)std::printf("FAIL strict Live block cadence at %.0fHz hops %llu %llu %llu fast %llu %llu %llu\n",sr,
              (unsigned long long)a.hops,(unsigned long long)b.hops,(unsigned long long)c.hops,
              (unsigned long long)a.fastSamples,(unsigned long long)b.fastSamples,
              (unsigned long long)c.fastSamples);
    return ok;
}
int main()
{
    for(double sr:{48000.,96000.})
    {
        if(!blockInvariant(sr))return 5;
        if(RealtimeAutotune::ultraLowLatencyHostSamples(sr)!=(int)std::ceil(sr*.0035))return 1;
        for(bool precision:{false,true})
        {
            // The synthetic time bounds include detector observation and
            // target confirmation, not just harmonic-carrier engagement.
            for(auto p:{std::pair<double,double>{110,146.832}, {82.4069,110},
                        {220,293.665}, {180,360}, {195.998,261.626}})
            {
                const Timing t=transition(sr,precision,p.first,p.second);
                std::printf("%.0fk precision=%d %.0f->%.0f F0=%.1fms note=%.1fms\n",
                         sr/1000.,(int)precision,p.first,p.second,t.detectorMs,t.targetMs);
                const double bound = (p.second==360 ? 65.:50.);
                if (!t.finite || t.detectorMs<0 || t.targetMs<0
                    || t.detectorMs>45. || t.targetMs>bound
                    || t.finalTarget!=(int)std::lround(69+12*std::log2(p.second/440)))return 2;
            }
            if(!glitchIsRejected(sr,precision))return 3;
        }
    }
    // V1.8 short analysis window must remain bounded by the existing YIN
    // window and must never imply a new transport-delay / buffer requirement.
    YinDetector d;d.prepare(48000);d.setRealtimeTransitionWindowEnabled(true);
    const int original=d.getAnalysisWindowSamples();
    double ph=0;
    for(int i=0;i<24000;++i)
    {
        ph+=2*pi*220/48000;
        d.push((float)(.22*std::sin(ph)));
    }
    if(d.getLastAnalysisWindowSamples()>=original || d.getLastAnalysisWindowSamples()<12)return 4;
    std::puts("PASS V1.8 causal pitch detection, confirmation, and octave-transient guard");
    return 0;
}
