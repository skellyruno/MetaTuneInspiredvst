// Standalone real-time callback benchmark; build against either baseline or current header.
// Not a deterministic regression gate: timings depend on the executing machine.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <numeric>

int main(int argc, char** argv)
{
    const char* label = argc > 1 ? argv[1] : "candidate";
    constexpr int frames = 256;
    constexpr double pi = 3.14159265358979323846;
    for (double rate : {44100., 48000., 96000.})
    {
        autotune::RealtimeAutotune engine;
        engine.prepare(rate);
        autotune::Settings config;
        config.voicingThreshold = 0.75f;
        config.follow = 1.0f;
        config.noteStabilizerMs = 0.0f;
        config.lowLatency = true;
        engine.setSettings(config);
        const int blockCount = (int)std::ceil(8.0 * rate / frames);
        std::vector<double> nanos;
        nanos.reserve((size_t)blockCount);
        std::vector<float> samples((size_t)frames);
        uint32_t seed = 0x929B2CADu;
        double phase = 0.0;
        for (int b=0; b<blockCount; ++b)
        {
            const int section = (b * frames / (int)(rate*2.0)) % 4;
            const double freq = section == 0 ? 75.0 : (section == 1 ? 233.08 : 420.0);
            for (int i=0; i<frames; ++i)
            {
                phase += 2.0 * pi * freq / rate;
                seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                samples[(size_t)i] = section == 3
                    ? .24f * ((float)(seed & 65535u) / 32767.5f - 1.0f)
                    : .28f * (float)std::sin(phase);
            }
            float* ch[] = {samples.data()};
            const auto start = std::chrono::steady_clock::now();
            engine.processChannels(ch, 1, frames);
            const auto end = std::chrono::steady_clock::now();
            nanos.push_back((double)std::chrono::duration_cast<std::chrono::nanoseconds>(end-start).count());
        }
        auto at = [&] (double q) {
            auto tmp=nanos;
            auto idx=(size_t)((tmp.size()-1)*q);
            std::nth_element(tmp.begin(),tmp.begin()+(std::ptrdiff_t)idx,tmp.end());
            return tmp[idx]*1.0e-6;
        };
        const double sum = std::accumulate(nanos.begin(),nanos.end(),0.0);
        std::printf("%s,%.0f,%d,%.4f,%.4f,%.4f,%.4f\n",label,rate,frames,
            (sum/(double)nanos.size())*1.e-6,at(.95),at(.99),at(1.0));
    }
}
