// Fixed host-PDC budget, impulses, control-path operation budget and telemetry.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

int main()
{
    using namespace autotune;
    for (double rate : {44100.0, 48000.0, 96000.0})
    {
        const int budget = RealtimeAutotune::recommendedHostLatencySamples(rate, 70.0f);
        if (budget < (int)std::ceil(rate * 2.5 / 70.0) || budget < 1) return 1;
        OutputLatencyCompensator comp;
        comp.prepare(budget, rate);
        const int n = budget + 16;
        std::vector<float> samples((size_t)n, 0.0f);
        samples[0] = 1.0f;
        float* channel[] = {samples.data()};
        comp.processChannels(channel, 1, n);
        for (int i = 0; i < n; ++i)
        {
            const float expected = i == budget ? 1.0f : 0.0f;
            if (std::abs(samples[(size_t)i] - expected) > 0.00001f)
            {
                std::fprintf(stderr,"PDC impulse misplaced rate=%.0f index=%d expected=%g got=%g\n",rate,i,expected,samples[(size_t)i]);
                return 2;
            }
        }

        RealtimeAutotune engine;
        engine.prepare(rate);
        Settings cfg; cfg.lowLatency = true; cfg.voicingThreshold = 0.75f;
        cfg.follow = 1.0f; cfg.noteStabilizerMs = 0.0f;
        engine.setSettings(cfg);
        if (rate == 48000.0 && engine.getMaxDifferenceOperationsPerFrame() >= 35000u)
        { std::fprintf(stderr,"YIN worst-case pair count not reduced\n"); return 3; }
        // Exercise both low and near-lowest reference pitches and alternating noise;
        // every sample's intrinsic delay passes through the host fixed-budget stage.
        std::vector<float> data(256), latency(256);
        const double pi = 3.14159265358979323846;
        double phase = 0;
        for (int block = 0; block < 380; ++block)
        {
            const double f = (block / 80) % 2 ? 70.5 : 245.0;
            for (int i = 0; i < 256; ++i)
            {
                phase += 2.0 * pi * f / rate;
                data[(size_t)i] = block > 270 ? (float)((i * 37 % 128) / 128.0 - 0.5) : .28f * (float)std::sin(phase);
            }
            float* ch[] = { data.data() };
            engine.processChannels(ch, 1, 256, latency.data());
            comp.processChannels(ch, 1, 256, latency.data());
            for (float x : data) if (!std::isfinite(x)) return 4;
        }
        if (comp.getOverBudgetSamples() != 0)
        {
            std::fprintf(stderr,"host budget exceeded at %.0f Hz sample rate (count=%llu, max=%.1f, budget=%d)\n",
                rate, (unsigned long long)comp.getOverBudgetSamples(), comp.getMaxObservedIntrinsicSamples(), budget);
            return 5;
        }
        std::printf("PASS PDC rate %.0f: host budget=%d samples (%.2fms), max observed intrinsic=%.1f, YIN max pairs/frame=%llu\n",
            rate,budget,1000.0*(double)budget/rate,comp.getMaxObservedIntrinsicSamples(),
            (unsigned long long)engine.getMaxDifferenceOperationsPerFrame());
    }
    return 0;
}
