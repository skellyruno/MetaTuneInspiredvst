// A bounded causal correction predictor, not an early note-confirmation gate.
// The source period and note choice must still follow unpredicted YIN frames.
#include "RealtimeAutotune.h"
#include <cmath>
#include <cstdio>
#include <algorithm>
static constexpr double pi=3.141592653589793238462643383;
static bool check(int rateHz)
{
    using namespace autotune;
    RealtimeAutotune reference, predicted;
    reference.prepare(48000.0); predicted.prepare(48000.0);
    Settings s;
    s.strictRealtime=true; s.ultraLowLatency=true; s.lowLatency=true;
    s.retuneMs=0.0f; s.vibratoPreserve=0.0f; s.follow=0.0f;
    s.voicingThreshold=0.78f; s.scale=Scale::Chromatic;
    s.pitchSynchronousLive=false; reference.setSettings(s);
    s.pitchSynchronousLive=true; predicted.setSettings(s);
    double phase=0.0,referenceError=0.0,predictedError=0.0;
    int count=0,wrong=0;
    for(int i=0;i<57600;++i)
    {
        const double t=i/48000.0;
        const double bend=0.4*std::sin(2*pi*rateHz*t);
        const double pitchHz=220.0*std::pow(2.0,bend/12.0);
        phase+=2*pi*pitchHz/48000.0;
        const float input=(float)(0.16*std::sin(phase)+0.05*std::sin(2*phase+0.4));
        const auto a=reference.process(input),b=predicted.process(input);
        if(!std::isfinite(a)||!std::isfinite(b))return false;
        if(t>0.4)
        {
            const auto da=reference.getDiagnostics(),db=predicted.getDiagnostics();
            if(da.targetMidi!=57||db.targetMidi!=57)++wrong;
            referenceError+=std::abs(da.correctionSemitones+(float)bend);
            predictedError+=std::abs(db.correctionSemitones+(float)bend);
            ++count;
        }
    }
    std::printf("vibrato %dHz: correction error %.5f -> %.5f semitones; wrong target frames %d\n",
        rateHz,referenceError/count,predictedError/count,wrong);
    return wrong==0 && predictedError < referenceError*0.90;
}
int main(){if(!check(4)||!check(6))return 1;}
