// V15 regression: an independently varying latency estimate must not
// resample/detune corrected pitched audio after synthesis.
// The old moving-tap compensation fails this intentionally adversarial test.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
constexpr double sr = 48000.0;
constexpr int budget = 1372;
constexpr int length = 3 * (int)sr;
constexpr double pi = 3.1415926535897932384626433832795;

std::vector<float> run (bool stable, int block, bool direct = false)
{
    autotune::OutputLatencyCompensator delay;
    delay.prepare (budget, sr);
    delay.setPitchPreserving (stable);
    std::vector<float> output ((size_t) length, 0.0f);
    std::vector<float> telemetry ((size_t) block, 0.0f);
    for (int base = 0; base < length; base += block)
    {
        const int n = std::min (block, length - base);
        for (int i = 0; i < n; ++i)
        {
            const int pos = base + i;
            const double seconds = (double) pos / sr;
            output[(size_t) pos] = (float)(0.25 * std::sin (2.0 * pi * 440.0 * seconds));
            telemetry[(size_t)i] = direct ? 0.0f : (float)(470.0 + 110.0 * std::sin (2.0 * pi * 3.5 * seconds));
        }
        float* ptr[] = { output.data() + base };
        delay.processChannels (ptr, 1, n, direct ? nullptr : telemetry.data());
    }
    if (stable && !direct && delay.getStableWetPaddingSamples() < 1) return {};
    return output;
}

// Local pitch in a window: positive-going zero crossings on a clean harmonic.
// Moving playback delay gives a varying instantaneous frequency, even when
// synthesis produces a perfectly constant 440-Hz note.
double estimatedCents (const std::vector<float>& signal, int begin, int end)
{
    int crossings = 0;
    double first = 0.0, last = 0.0;
    for (int i = begin + 1; i < end; ++i)
    {
        const float a = signal[(size_t)i-1], b = signal[(size_t)i];
        if (a <= 0.0f && b > 0.0f && b != a)
        {
            const double where = (double)(i-1) - (double)a / ((double)b - (double)a);
            if (crossings++ == 0) first = where;
            last = where;
        }
    }
    if (crossings < 6 || last <= first) return std::numeric_limits<double>::infinity();
    const double frequency = (double)(crossings - 1) * sr / (last - first);
    return 1200.0 * std::log2 (frequency / 440.0);
}

double percentile95 (const std::vector<float>& v)
{
    std::vector<double> errors;
    for (int n = (int)(sr * 0.5); n + 2048 < length; n += 512)
        errors.push_back (std::abs (estimatedCents (v, n, n + 2048)));
    if (errors.empty()) return std::numeric_limits<double>::infinity();
    std::sort (errors.begin(), errors.end());
    return errors[(size_t)(errors.size() * .95)];
}
}

int main()
{
    const auto stable = run (true, 256);
    const auto legacy = run (false, 256);
    if (stable.empty() || legacy.empty()) return 1;
    const double p95Stable = percentile95 (stable);
    const double p95Legacy = percentile95 (legacy);
    if (!(p95Stable < 0.20 && p95Legacy > 15.0))
    {
        std::fprintf (stderr, "FAIL stable pitch %.3fc / variable-tap pitch %.3fc\n",p95Stable,p95Legacy);
        return 2;
    }
    // No dependence on DAW callback boundaries.
    for (int block : {1, 17, 63, 128, 1024})
    {
        const auto candidate = run (true,block);
        if (candidate.size() != stable.size()) return 3;
        double maxDifference = 0.0;
        for (size_t i=0;i<candidate.size();++i)
            maxDifference=std::max(maxDifference,std::abs((double)candidate[i]-stable[i]));
        if (maxDifference > 1e-6) { std::fprintf(stderr,"FAIL block %d diff %.8g\n",block,maxDifference);return 4; }
    }
    // Generated/reference-tone branch must remain EXACT full-budget PDC.
    autotune::OutputLatencyCompensator comp;
    comp.prepare(budget,sr);
    comp.setPitchPreserving(true);
    std::vector<float> impulse((size_t)(budget+16),0.0f);
    impulse[0]=1.0f;
    float* ch[]={impulse.data()};
    comp.processChannels(ch,1,(int)impulse.size(),nullptr);
    for(size_t i=0;i<impulse.size();++i)
        if(std::abs(impulse[i]-(i==(size_t)budget?1.f:0.f))>1e-6f) return 5;
    std::printf("PASS pitch-preserving PDC: stable p95 %.4fc, moving-tap p95 %.2fc; block invariance and direct impulse\n",p95Stable,p95Legacy);
}
