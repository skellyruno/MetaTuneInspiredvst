// V1.3: The production strict-realtime renderer must never route corrected
// audio through historical (up to >80ms) or PSOLA (>30ms) synthesis.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr double pi = 3.14159265358979323846264;

bool impulseTest(double sr, bool live, bool precision)
{
    autotune::RealtimeAutotune e;
    e.prepare(sr, 70.0f, 1000.0f);
    autotune::Settings s;
    s.strictRealtime = true;
    s.lowLatency = live;
    s.ultraLowLatency = false;   // state should sanitize this internally
    s.fixedHorizonVoiced = true; // simulate restoring a deprecated 80ms preset
    s.precisionHardTune = precision;
    s.strength = 0.0f;
    e.setSettings(s);
    if (e.getSettings().fixedHorizonVoiced || !e.getSettings().ultraLowLatency)
        return false;
    const int bound = live ? autotune::RealtimeAutotune::ultraLowLatencyHostSamples(sr)
                           : autotune::RealtimeAutotune::realtimeQualityHostSamples(sr);
    if ((double)bound / sr * 1000.0 > 8.05) return false;
    constexpr int offset = 2800;
    const int total = offset + bound + 410;
    // Unequal channel impulses and awkward blocks check transport and stereo.
    int n = 0;
    while (n < total)
    {
        const int block = std::min(((n / 37) % 2 == 0) ? 64 : 257, total - n);
        std::vector<float> l((size_t)block), r((size_t)block), intrinsic((size_t)2 * block);
        for (int i=0;i<block;++i)
        {
            l[(size_t)i] = n+i == offset ? 0.4f : 0.0f;
            r[(size_t)i] = n+i == offset + 17 ? -0.3f : 0.0f;
        }
        float* ch[2] = {l.data(), r.data()};
        e.processChannels(ch,2,block,intrinsic.data());
        for (int i=0;i<block;++i)
        {
            if (!std::isfinite(l[(size_t)i]) || !std::isfinite(r[(size_t)i])) return false;
            const float expectedL=n+i==offset+bound ? 0.4f : 0.0f;
            const float expectedR=n+i==offset+17+bound ? -0.3f : 0.0f;
            if (std::abs(l[(size_t)i]-expectedL)>1e-6f ||
                std::abs(r[(size_t)i]-expectedR)>1e-6f ||
                std::abs(intrinsic[(size_t)i] - (float)bound)>0.01f ||
                std::abs(intrinsic[(size_t)block+i] - (float)bound)>0.01f)
            {
                std::fprintf(stderr,"impulse mismatch %.0f live=%d precision=%d sample=%d L=%f R=%f\n",sr,live,precision,n+i,l[(size_t)i],r[(size_t)i]);
                return false;
            }
        }
        n+=block;
    }
    if (e.getHistoricalCarrierSourceAgeSamples() != 0.0f) return false;
    // Standalone mono process() must use the same aligned dry age.
    autotune::RealtimeAutotune mono;
    mono.prepare(sr);
    mono.setSettings(s);
    for(int i=0;i<offset+bound+10;++i)
    {
        const float y=mono.process(i==offset ? .2f : 0.f);
        const float expected=i==offset+bound ? .2f : 0.f;
        if(std::abs(y-expected)>1.e-6f) return false;
    }
    return true;
}

// Strong synthetic vowel harmonics, first note below full-period-wrap support.
// The property checked here is causality and numerical stability, not a
// claim of transparent formants or bounded pitch-tracker acquisition time.
bool voiceTest(double sr, bool live, float f0, float ratio)
{
    const int bound = live ? autotune::RealtimeAutotune::ultraLowLatencyHostSamples(sr)
                           : autotune::RealtimeAutotune::realtimeQualityHostSamples(sr);
    autotune::MicroLatencyShifter sh;
    sh.prepare(sr,4,live?3.5:8.0);
    sh.setRatio(ratio);
    sh.setPeriod((float)(sr/f0));
    if(sh.getMaximumDelaySamples()!=bound) return false;
    const int n=(int)std::ceil(sr*0.3);
    float maxStep=0,prev=0;
    for (int i=0;i<n;++i)
    {
        const double t=(double)i/sr;
        const float x=(float)(0.12*std::sin(2*pi*f0*t)
            +0.075*std::sin(2*pi*2*f0*t+0.2)
            +0.04*std::sin(2*pi*3*f0*t+0.5));
        const float y=sh.processSample(x);
        if (!std::isfinite(y) || std::abs(y)>1.0f) return false;
        if (i>0 && i>bound*3) maxStep=std::max(maxStep,std::abs(y-prev));
        prev=y;
    }
    std::printf("voiced %.0f Hz mode=%s sr=%.0f max-step=%g max-source-age=%d\n",
        (double)f0,live?"Live":"Quality",sr,maxStep,bound);
    // An extremely large sample jump is a regression; tonal quality requires
    // separate listener and formant evaluation, not this continuity gate.
    return maxStep < 0.5f;
}

bool correctedStereoBlockInvariance(double sr, bool live)
{
    const int count=(int)(sr*0.45);
    std::array<std::vector<float>,2> source;
    for(auto& c:source)c.resize((size_t)count);
    for(int i=0;i<count;++i)
    {
        const double t=i/sr;
        const double f0=i<count/2?82.4:205.6;
        const float x=(float)(0.16*std::sin(2*pi*f0*t) +
            0.11*std::sin(4*pi*f0*t +0.31));
        source[0][(size_t)i]=x;
        source[1][(size_t)i]=x*.71f;
    }
    auto render=[&](int bs)
    {
        auto out=source;
        autotune::RealtimeAutotune engine;
        engine.prepare(sr);
        autotune::Settings cfg;
        cfg.strictRealtime=true;
        cfg.lowLatency=live;
        cfg.precisionHardTune=true;
        cfg.strength=1.0f;
        cfg.fixedHorizonVoiced=true;
        engine.setSettings(cfg);
        for(int at=0;at<count;at+=bs)
        {
            const int n=std::min(bs,count-at);
            float* ch[2]={out[0].data()+at,out[1].data()+at};
            engine.processChannels(ch,2,n);
        }
        return out;
    };
    const auto a=render(64),b=render(257);
    float peak=0;
    for(int c=0;c<2;++c)
        for(int i=0;i<count;++i)
            peak=std::max(peak,std::abs(a[(size_t)c][(size_t)i]-b[(size_t)c][(size_t)i]));
    if(peak>0.00001f)
        std::fprintf(stderr,"strict stereo block mismatch sr=%.0f live=%d peak=%g\n",sr,(int)live,peak);
    return peak<0.00001f;
}

bool correctedImpulseBound(double sr, bool live, float ratio)
{
    autotune::MicroLatencyShifter sh;
    sh.prepare(sr,4,live?3.5:8.0);
    sh.setRatio(ratio);
    const int bound=sh.getMaximumDelaySamples(), at=1000;
    bool heard=false;
    for (int i=0;i<at+bound+100;++i)
    {
        const float y=sh.processSample(i==at?0.25f:0.0f);
        if (!std::isfinite(y)) return false;
        if (std::abs(y)>1e-6f)
        {
            heard=true;
            if (i<at || i>at+bound) return false;
        }
    }
    return heard;
}
}

int main()
{
    for (double sr : {44100.,48000.,96000.,192000.})
        for (bool live : {true,false})
        {
            // Delayed doubler voice: conservative cubic-interpolator margin.
            const int transport=live ? autotune::RealtimeAutotune::ultraLowLatencyHostSamples(sr)
                                     : autotune::RealtimeAutotune::realtimeQualityHostSamples(sr);
            const int doublerExtra=(int)std::ceil(sr * 0.001 *
                (autotune::latency_limits::doublerCentreMs +
                 autotune::latency_limits::doublerDepthBaseMs +
                 autotune::latency_limits::doublerDepthWidthMs)) + 2;
            if((double)(transport+doublerExtra)/sr>0.010)
            {
                std::fprintf(stderr,"10ms doubler budget violation sr=%.0f mode=%d\n",sr,(int)live);
                return 4;
            }
            for(bool precision : {false,true})
                if (!impulseTest(sr,live,precision)) return 1;
            for(float ratio : {0.82f,0.965f,1.035f,1.22f})
                if (!correctedImpulseBound(sr,live,ratio)) return 2;
            for(float hz : {75.f,110.f,195.f,420.f})
                if (!voiceTest(sr,live,hz,1.035f)) return 3;
            if (!correctedStereoBlockInvariance(sr,live)) return 5;
        }
    std::puts("PASS V1.3 strict realtime 3.5/8ms budget, restored state and stereo");
}
