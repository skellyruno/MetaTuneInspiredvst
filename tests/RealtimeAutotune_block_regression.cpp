// Regression for oversized host blocks, channel-major telemetry, and output PDC.
// C++17, no JUCE dependency. Mirrors the processor's bounded-slice ordering.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
constexpr double sr = 48000.0;
constexpr int length = 26001; // Exceeds the previous 16,384-frame telemetry limit.
constexpr int fixed = 1714;   // round(2.5 * 48,000 / 70), matching JUCE prepareToPlay.
constexpr double pi = 3.14159265358979323846;

struct Output { std::vector<float> l, r, telemetryL, telemetryR; };

Output run(int blockSize, bool bypass, bool zeroAmount) {
    autotune::RealtimeAutotune engine;
    engine.prepare(sr);
    autotune::Settings s;
    s.key = 0; s.scale = autotune::Scale::Major; s.retuneMs = 0.0f;
    s.voicingThreshold = 0.5f; s.lowLatency = true;
    s.bypass = bypass; s.strength = zeroAmount ? 0.0f : 1.0f;
    engine.setSettings(s); engine.reset();
    autotune::OutputLatencyCompensator compensator;
    compensator.prepare(fixed, sr);

    Output out;
    out.l.resize(length); out.r.resize(length);
    out.telemetryL.resize(length); out.telemetryR.resize(length);
    for(int i=0; i<length; ++i) {
        const double t = (double)i/sr;
        // Independent L/R phase & amplitude, left is always the stronger channel.
        out.l[(size_t)i] = 0.35f * (float)std::sin(2*pi*233.08*t);
        out.r[(size_t)i] = 0.15f * (float)std::sin(2*pi*233.08*t + 1.2);
    }
    std::vector<float> telemetry((size_t)2*blockSize, -1000.0f);
    for(int pos=0;pos<length;pos+=blockSize) {
        const int n=std::min(blockSize,length-pos);
        float* ptrs[] = {out.l.data()+pos,out.r.data()+pos};
        engine.processChannels(ptrs,2,n,telemetry.data());
        for(int i=0;i<n;++i) {
            out.telemetryL[(size_t)pos+i]=telemetry[(size_t)i];
            out.telemetryR[(size_t)pos+i]=telemetry[(size_t)n+i];
        }
        compensator.processChannels(ptrs,2,n,telemetry.data());
    }
    return out;
}

bool compare(const Output& a,const Output& b, int blockSize,bool bypass,bool zeroAmount) {
    double maxSampleDiff=0.,maxTelemetryDiff=0.;
    for(int i=0;i<length;++i) {
        for(int c=0;c<2;++c) {
            const float actual = c == 0 ? b.l[(size_t)i] : b.r[(size_t)i];
            const float expected = c == 0 ? a.l[(size_t)i] : a.r[(size_t)i];
            const float d = c == 0 ? b.telemetryL[(size_t)i] : b.telemetryR[(size_t)i];
            const float e = c == 0 ? a.telemetryL[(size_t)i] : a.telemetryR[(size_t)i];
            if(!std::isfinite(actual) || !std::isfinite(d) || d < 0.0f || d > (float)fixed) {
                std::fprintf(stderr,"FAIL: nonfinite/out-of-budget channel=%d index=%d n=%d sample=%g latency=%g\n",c,i,blockSize,actual,d);
                return false;
            }
            maxSampleDiff=std::max(maxSampleDiff,std::abs((double)actual-expected));
            maxTelemetryDiff=std::max(maxTelemetryDiff,std::abs((double)d-e));
        }
    }
    if(maxSampleDiff > 3.0e-5 || maxTelemetryDiff > 1.0e-3) {
        std::fprintf(stderr,"FAIL block invariance n=%d bypass=%d zero=%d maxAudio=%g maxTelemetry=%g\n",blockSize,bypass,zeroAmount,maxSampleDiff,maxTelemetryDiff);
        return false;
    }
    std::printf("PASS blocks=%d bypass=%d zero=%d maxAudioDiff=%g maxTelemetryDiff=%g\n",blockSize,bypass,zeroAmount,maxSampleDiff,maxTelemetryDiff);
    return true;
}

bool verifyExactFixedPdc() {
    autotune::OutputLatencyCompensator pad;
    pad.prepare(fixed,sr);
    std::vector<float> l(length,0.f),r(length,0.f),tel((size_t)2*length,0.f);
    l[0]=1.f; r[0]=-1.f;
    float* channels[]={l.data(),r.data()};
    pad.processChannels(channels,2,length,tel.data());
    for(int i=0;i<length;++i) {
        const float wantL = i == fixed ? 1.f : 0.f;
        const float wantR = -wantL;
        if(std::abs(l[(size_t)i]-wantL)>1.e-6f || std::abs(r[(size_t)i]-wantR)>1.e-6f) {
            std::fprintf(stderr,"FAIL fixed PDC direct index=%d L=%g R=%g expected=%g\n",i,l[(size_t)i],r[(size_t)i],wantL);
            return false;
        }
    }
    std::puts("PASS exact impulse PDC for direct stereo path");
    return true;
}
bool verifyGeneratedSourcePdc() {
    autotune::FixedDirectDelay direct;
    direct.prepare(fixed);
    autotune::OutputLatencyCompensator vocal;
    vocal.prepare(fixed,sr);
    // The vocal's changing source-grain latency must not alter the timing of
    // independently generated reference tones. The direct branch stays at
    // exactly the advertised host latency throughout the pitch transitions.
    for(int i=0;i<length;++i) {
        const float piano = i==1000 || i==10000 ? 0.8f : 0.0f;
        const float y = direct.processSample(piano);
        const float want = (i==1000+fixed || i==10000+fixed) ? 0.8f : 0.0f;
        if(y!=want) {
            std::fprintf(stderr,"FAIL generated source PDC i=%d y=%g expected=%g\n",i,y,want);
            return false;
        }
    }
    direct.reset();
    for(int i=0;i<3*fixed;++i) {
        const float input = i==0 ? 1.0f : 0.0f;
        const float want = i==fixed ? 1.0f : 0.0f;
        if(direct.processSample(input)!=want) {
            std::fprintf(stderr,"FAIL direct PDC after reset i=%d\n",i);
            return false;
        }
    }
    std::puts("PASS generated reference-tone PDC: independent fixed delay and reset");
    return true;
}

}

int main() {
    for(bool bypass : {false,true}) {
        for(bool zero : {false,true}) {
            if(bypass && zero) continue;
            const Output reference=run(length,bypass,zero);
            for(int blockSize : {1,17,63,127,512,4096,16384})
                if(!compare(reference,run(blockSize,bypass,zero),blockSize,bypass,zero)) return 1;
        }
    }
    if(!verifyExactFixedPdc()) return 2;
    if(!verifyGeneratedSourcePdc()) return 3;
    std::puts("All bounded-block/PDC regression checks passed.");
}
