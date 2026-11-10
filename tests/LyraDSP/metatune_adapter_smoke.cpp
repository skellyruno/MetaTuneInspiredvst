// Verify the actual MetaTune -> LyraTune settings adapter and audio path.
// This is a standalone C++17 build of the production SkellyFusionEngine.
// In particular, the host's Max Correction parameter must change the
// applied pitch ratio, not just a disconnected settings struct.
#include "SkellyFusionEngine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

static void require(bool value, const char* what) {
    if(!value) { std::fprintf(stderr, "Lyra adapter FAILURE: %s\n", what); std::exit(1); }
}

static void render(autotune::SkellyFusionEngine& e, double sr, double frequency,
                   double seconds, int blockSize, int& clock) {
    constexpr double pi = 3.14159265358979323846;
    float left[256] {};
    float right[256] {};
    const int count=(int)std::lround(sr*seconds);
    for(int start=0; start<count; start+=blockSize) {
        const int n=std::min(blockSize,count-start);
        for(int i=0;i<n;++i) {
            const double time=(double)clock++ / sr;
            left[i]=(float)(0.25*std::sin(2.0*pi*frequency*time));
            right[i]=-left[i];
        }
        float* channels[]={left,right};
        e.processChannels(channels,2,n);
        for(int i=0;i<n;++i)
            require(std::isfinite(left[i]) && std::isfinite(right[i]),
                    "finite anti-phase stereo output");
    }
}

static void testRate(double sr,int block) {
    autotune::SkellyFusionEngine e;
    e.prepare(sr,70.0f,1000.0f);
    autotune::Settings s;
    s.scale=autotune::Scale::Chromatic;
    s.referenceHz=440.0f;
    s.retuneMs=-3.0f;
    s.precisionHardTune=true;
    s.noteConfirmMs=0;
    s.noteStabilizerMs=0;
    s.lowLatency=true;
    s.strength=1;
    s.maxCorrectionSemitones=0.1f; // should cap 225Hz -> 220Hz correction to 10c
    e.setSettings(s);
    int clock=0;
    render(e,sr,225.0,0.65,block,clock);
    require(e.getConfidence()>0.60f && std::abs(e.getDetectedMidi()-57.39f)<0.2f,
            "A3 sharp is voiced and tracked");
    const double limited=e.getCorrectionSemitones();
    require(limited < -0.065 && limited > -0.12,
            "Max Correction 0.1st reduces real applied ratio to roughly -10 cents");

    s.maxCorrectionSemitones=0.0f;
    e.setSettings(s);
    render(e,sr,225.0,0.20,block,clock);
    require(std::abs(e.getCorrectionSemitones()) < 0.002,
            "0st max correction must yield natural pitch, no hidden offset");

    s.maxCorrectionSemitones=12.0f;
    e.setSettings(s);
    render(e,sr,225.0,0.30,block,clock);
    const double full=e.getCorrectionSemitones();
    require(full < -0.30 && full > -0.50,
            "restoring correction limit restores actual hard tune");

    // Directly exercise remaining automation setters and run through
    // invalid input/parameter values without breaking the state machine.
    s.glideMs=180.0f;
    s.consonantReleaseMs=30.0f;
    e.setSettings(s);
    render(e,sr,225.0,0.15,block,clock);
    require(std::isfinite(e.getCorrectionSemitones()),
            "glide/release controls remain finite through adapter");
    std::printf("MetaTune Lyra adapter @%.0fHz %d samples: limit=%.3f full=%.3f semitones\n",
                sr,block,limited,full);
}

static void testGlideThroughProductionAdapter() {
    auto transitionCorrection=[](float glideMs) {
        constexpr double sr=48000.0;
        autotune::SkellyFusionEngine e;
        e.prepare(sr,70.0f,1000.0f);
        autotune::Settings s;
        s.scale=autotune::Scale::Chromatic;
        s.retuneMs=-3.0f;
        s.precisionHardTune=true;
        s.noteConfirmMs=0.0f;
        s.noteStabilizerMs=0.0f;
        s.lowLatency=true;
        s.strength=1.0f;
        s.glideMs=glideMs;
        e.setSettings(s);
        int clock=0;
        render(e,sr,220.0,0.45,128,clock); // A3
        render(e,sr,246.94165,0.08,128,clock); // B3, fast note jump
        return (double)e.getCorrectionSemitones();
    };
    const double instant=transitionCorrection(0.0f);
    const double portamento=transitionCorrection(200.0f);
    std::printf("MetaTune Glide contract: instant %.3f st, Glide 200ms %.3f st\n",
                instant,portamento);
    require(std::abs(instant)<0.12, "0ms Glide must preserve instant hard tune");
    require(portamento < instant-0.35, "200ms Glide must audibly slow target-note transitions");
}

static void testSkellLiveProductionRouting()
{
    for (double sr: {44100.0,48000.0,96000.0})
    {
        autotune::SkellyFusionEngine e;
        e.prepare(sr,70.0f,1000.0f);
        autotune::Settings s;
        s.lowLatency=true;
        s.snapLive=false; // historical APVTS ID
        s.noteStabilizerMs=650.0f; // legacy control must be ignored
        e.setSettings(s);
        const int original=e.latencySamples();
        require(!e.isSkellLiveEnabled(), "standard mode really disables Skell controller");

        s.snapLive=true;
        e.setSettings(s);
        require(e.isSkellLiveEnabled(), "Skell Live legacy ID activates distinct DSP controller");
        require(e.latencySamples()==original,
                "Skell Live adds no audio delay and retains original host PDC");

        int clock=0;
        render(e,sr,220.0,0.20,128,clock);
        render(e,sr,261.62556,0.16,128,clock);
        require(std::isfinite(e.getCorrectionSemitones()),
                "Skell Live processes real fast-note audio without non-finite output");

        s.lowLatency=false;
        e.setSettings(s);
        require(!e.isSkellLiveEnabled(), "Skell Live cannot silently run in Quality");
        require(e.latencySamples()>original,"Quality retains full-range host PDC");

        s.lowLatency=true;
        s.snapLive=true;
        e.setSettings(s);
        require(e.isSkellLiveEnabled() && e.latencySamples()==original,
                "Skell Live reactivates after Quality/Live switch");

        std::printf("Skell Live controller @%.0f Hz: enabled, no extra PDC (%d samples)\n",
                    sr,original);
    }
}

static void testLegacyStabilizerIsActuallyInactive()
{
    auto measure=[](float legacy) {
        autotune::SkellyFusionEngine e;
        e.prepare(48000.0,70.0f,1000.0f);
        autotune::Settings s;
        s.lowLatency=true;
        s.snapLive=false;
        s.retuneMs=-3.0f;
        s.noteStabilizerMs=legacy;
        s.noteConfirmMs=0.0f;
        e.setSettings(s);
        int clock=0;
        render(e,48000.0,220.0,0.25,128,clock);
        render(e,48000.0,261.62556,0.25,128,clock);
        return e.getCorrectionSemitones();
    };
    const float oldOff=measure(0.0f);
    const float oldLong=measure(650.0f);
    require(std::isfinite(oldOff) && std::abs(oldOff-oldLong)<1.0e-6f,
            "deprecated Note Stabilizer controls must not change the active DSP");
}

int main() {
    testSkellLiveProductionRouting();
    testLegacyStabilizerIsActuallyInactive();
    testGlideThroughProductionAdapter();
    for(double rate: {44100.0,48000.0,96000.0})
        for(int block: {32,128,256})
            testRate(rate,block);
    std::puts("MetaTune Lyra automation adapter: DSP tests passed");
}
