#include "RealtimeAutotune.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr double pi = 3.14159265358979323846;
constexpr double sampleRate = 48000.0;
constexpr int fixedLatency = 1371;
constexpr int samples = 46000;

std::vector<float> run(int hostBlock, bool toggle, int channel)
{
    autotune::LatencyAlignedBypass bypass;
    bypass.prepare(fixedLatency, sampleRate);
    std::vector<float> output(samples);
    autotune::FixedDirectDelay modeledWet;
    modeledWet.prepare(fixedLatency);
    const double tone = (9.5 * sampleRate) / fixedLatency; // old input/wet fade cancels
    for (int b = 0; b < samples; b += hostBlock)
    {
        const int n = std::min(hostBlock, samples - b);
        for (int j = 0; j < n; ++j)
        {
            const int i = b + j;
            if (toggle) bypass.setBypassed((i / 1500) % 2 != 0);
            else bypass.setBypassed(i >= 5000);
            const float x = 0.25f * (float) std::sin(2.0*pi*tone*(double)i/sampleRate);
            const float delayed = bypass.delayDry(channel, x);
            const float wet = modeledWet.processSample(x);
            bypass.advance();
            output[(size_t)i] = bypass.mix(wet, delayed);
        }
    }
    return output;
}
}

int main()
{
    const auto reference = run(1,true,0);
    for(int block : {7,31,64,257,1024})
    {
        auto candidate = run(block,true,0);
        float err = 0.0f;
        for (int i = 0; i < samples; ++i)
            err = std::max(err, std::abs(candidate[(size_t)i]-reference[(size_t)i]));
        if (err > 1.0e-6f) { std::fprintf(stderr,"FAIL block %d error %g\n",block,err);return 1; }
    }
    const auto left = run(17,true,0);
    const auto right = run(17,true,1);
    for (int i = 0; i < samples; ++i)
        if (std::abs(left[(size_t)i]-right[(size_t)i]) > 1.0e-6f)
            return 2;
    const auto steady = run(128,false,0);
    double energy = 0.0, wantEnergy = 0.0;
    const double tone = (9.5 * sampleRate) / fixedLatency;
    for(int i = 20000; i < samples; ++i)
    {
        const float expected = 0.25f*(float)std::sin(2.0*pi*tone*(double)(i-fixedLatency)/sampleRate);
        const float error = steady[(size_t)i]-expected;
        energy += (double)error*error;
        wantEnergy += (double)expected*expected;
    }
    const double relative = std::sqrt(energy / wantEnergy);
    if(relative > 1.0e-5) { std::fprintf(stderr,"FAIL fixed PDC or level %g\n",relative);return 3; }
    // An old-style 50/50 crossfade of an undelayed dry signal with a
    // fixed-latency wet path nearly cancels when the delay is a half-period.
    double oldEnergy = 0.0;
    for(int i = 20000; i < samples; ++i)
    {
        const double dry = .25*std::sin(2.0*pi*tone*(double)i/sampleRate);
        const double wet = .25*std::sin(2.0*pi*tone*(double)(i-fixedLatency)/sampleRate);
        const double old = 0.5*(dry+wet);
        oldEnergy += old*old;
    }
    const double oldRatio = std::sqrt(oldEnergy/wantEnergy);
    if(oldRatio > 0.02) { std::fprintf(stderr,"FAIL old-path comparator %g\n",oldRatio);return 4; }
    autotune::LatencyAlignedBypass bypass;
    bypass.prepare(fixedLatency,sampleRate);
    bypass.setBypassed(true);
    for (int i=0; i < 4000; ++i) bypass.advance();
    if (bypass.getBypassMix() < .999f) return 5;
    std::printf("PASS PDC-aligned bypass: stereo, block invariance, delayed-tone RMS error=%.8f, old 50/50 level=%.4f\n",relative,oldRatio);
}
