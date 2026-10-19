// Targeted corrected-pitch latency/accuracy regression checks.
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -ISource tests/RealtimeAutotune_latency_regression.cpp -o autotune_latency_regression
#include "RealtimeAutotune.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr double pi = 3.14159265358979323846;
constexpr double sampleRate = 48000.0;

struct ToneResult { double outputHz = 0.0, centsError = 0.0, medianMs = 0.0, p95Ms = 0.0, maxMs = 0.0, rmsRatio = 0.0; int targetMidi = -1; };

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return -1.0;
    const size_t ix = std::min(values.size() - 1, (size_t) std::floor(p * (values.size() - 1)));
    std::nth_element(values.begin(), values.begin() + (std::ptrdiff_t) ix, values.end());
    return values[ix];
}

double estimateHz(const std::vector<float>& data, int begin, int end) {
    std::vector<double> crossings;
    for (int i = std::max(1, begin + 1); i < end; ++i) {
        const float a = data[(size_t) i - 1], b = data[(size_t) i];
        if (a <= 0.0f && b > 0.0f && b != a)
            crossings.push_back((double) i - 1.0 + (double) (-a) / (double) (b - a));
    }
    if (crossings.size() < 2) return 0.0;
    return (double) (crossings.size() - 1) * sampleRate / (crossings.back() - crossings.front());
}

void configure(autotune::RealtimeAutotune& engine, bool lowLatency, float follow = 1.0f) {
    engine.prepare(sampleRate);
    autotune::Settings s;
    s.key = 0; s.scale = autotune::Scale::Major; s.strength = 1.0f;
    s.retuneMs = 0.0f; s.humanize = 0.0f; s.vibratoPreserve = 0.7f;
    s.follow = follow; s.voicingThreshold = 0.5f; s.noteStabilizerMs = follow >= 1.0f ? 0.0f : 300.0f;
    s.lowLatency = lowLatency;
    engine.setSettings(s); engine.reset();
}

ToneResult measureTone(float inputHz, bool lowLatency) {
    const int n = (int) sampleRate;
    autotune::RealtimeAutotune engine; configure(engine, lowLatency);
    std::vector<float> output((size_t)n);
    std::vector<double> delays; delays.reserve((size_t)(sampleRate * .75));
    double inEnergy = 0.0, outEnergy = 0.0;
    for (int i = 0; i < n; ++i) {
        const float x = .25f * (float) std::sin(2.0 * pi * inputHz * (double)i / sampleRate);
        const float y = engine.process(x); output[(size_t)i] = y;
        if (i >= (int)(sampleRate * .2) && engine.isVoiced()) delays.push_back(engine.getLatencySamples());
        if (i >= (int)(sampleRate * .5)) { inEnergy += (double)x*x; outEnergy += (double)y*y; }
    }
    ToneResult r;
    r.outputHz = estimateHz(output, (int)(sampleRate*.5), n);
    r.targetMidi = engine.getTargetMidi();
    const double targetHz = 440.0 * std::exp2(((double)r.targetMidi - 69.0) / 12.0);
    r.centsError = (r.targetMidi >= 0 && r.outputHz > 0.0) ? 1200.0 * std::log2(r.outputHz / targetHz) : 9999.0;
    r.medianMs = percentile(delays, .5) * 1000.0 / sampleRate;
    r.p95Ms = percentile(delays, .95) * 1000.0 / sampleRate;
    r.maxMs = percentile(delays, .999) * 1000.0 / sampleRate;
    r.rmsRatio = std::sqrt(outEnergy / std::max(1.0e-20, inEnergy));
    return r;
}

bool checkSteadyCorrection(bool lowLatency) {
    struct Case { float input; int target; };
    for (const Case c : { Case{233.08f,59}, Case{275.0f,60}, Case{300.0f,62} }) {
        const ToneResult r = measureTone(c.input, lowLatency);
        if (r.targetMidi != c.target || std::abs(r.centsError) > (c.input == 275.0f ? 5.0 : 25.0) || r.rmsRatio < .90 || r.rmsRatio > 1.10) {
            std::fprintf(stderr, "FAIL steady correction mode=%s in=%.2f target=%d expected=%d out=%.3f error=%.2fc rms=%.3f\n",
                lowLatency?"low":"normal", c.input, r.targetMidi, c.target, r.outputHz, r.centsError, r.rmsRatio);
            return false;
        }
        if (lowLatency) {
            const double medianLimit = c.input < 240.0f ? 4.80 : (c.input < 290.0f ? 4.35 : 3.95);
            const double p95Limit = c.input < 240.0f ? 7.00 : (c.input < 290.0f ? 5.90 : 5.40);
            if (r.medianMs >= medianLimit || r.p95Ms >= p95Limit) {
                std::fprintf(stderr, "FAIL latency regression in=%.2f median=%.2fms (limit %.2f) p95=%.2fms (limit %.2f)\n",
                    c.input, r.medianMs, medianLimit, r.p95Ms, p95Limit);
                return false;
            }
        }
        std::printf("PASS steady correction mode=%s in=%.2f out=%.3f target=%d err=%+.2fc delay median/p95/p99.9 %.2f/%.2f/%.2fms rms=%.3f\n",
            lowLatency?"low":"normal", c.input, r.outputHz, r.targetMidi, r.centsError, r.medianMs, r.p95Ms, r.maxMs, r.rmsRatio);
    }
    return true;
}

bool checkAbruptTransitions(bool lowLatency) {
    constexpr int segment = (int)(sampleRate * .6);
    constexpr int n = segment * 3;
    const float freqs[] = {233.08f, 275.0f, 300.0f};
    const int targets[] = {59,60,62};
    autotune::RealtimeAutotune engine; configure(engine, lowLatency);
    std::vector<float> out((size_t)n);
    double phase = 0.0;
    for (int i=0;i<n;++i) {
        const int s = std::min(2, i / segment);
        phase += 2.0*pi*(double)freqs[s]/sampleRate;
        out[(size_t)i] = engine.process(.25f*(float)std::sin(phase));
        if (!std::isfinite(out[(size_t)i])) { std::fprintf(stderr,"FAIL transition non-finite output\n"); return false; }
    }
    for (int s=0;s<3;++s) {
        const int begin = s*segment + (int)(sampleRate*.18);
        const int end = (s+1)*segment - 1;
        const double f = estimateHz(out,begin,end);
        const double target = 440.0*std::exp2(((double)targets[s]-69.0)/12.0);
        const double cents = f>0 ? 1200.0*std::log2(f/target) : 9999.0;
        if (std::abs(cents)>25.0) { std::fprintf(stderr,"FAIL transition mode=%s segment=%d out=%.2f err=%+.2fc\n",lowLatency?"low":"normal",s,f,cents); return false; }
        std::printf("PASS transition mode=%s segment=%d output=%.3f target=%d err=%+.2fc\n",lowLatency?"low":"normal",s,f,targets[s],cents);
    }
    return true;
}

bool checkVibratoStability(bool lowLatency) {
    const int n=(int)(sampleRate*1.4);
    autotune::RealtimeAutotune engine; configure(engine,lowLatency,0.0f);
    int wrongTarget=0, checked=0; double phase=0.0;
    std::vector<float> out((size_t)n);
    for(int i=0;i<n;++i) {
        const double t=(double)i/sampleRate;
        const double hz=233.08*std::exp2((20.0*std::sin(2.0*pi*5.3*t))/1200.0);
        phase += 2.0*pi*hz/sampleRate;
        out[(size_t)i]=engine.process(.25f*(float)std::sin(phase));
        if(i>(int)(sampleRate*.3)) { ++checked; if(engine.getTargetMidi()!=59) ++wrongTarget; }
        if(!std::isfinite(out[(size_t)i])) { std::fprintf(stderr,"FAIL vibrato non-finite output\n"); return false; }
    }
    const double wrongFraction=(double)wrongTarget/std::max(1,checked);
    const double measured=estimateHz(out,(int)(sampleRate*.5),n);
    const double target=440.0*std::exp2((59.0-69.0)/12.0);
    const double cents=1200.0*std::log2(measured/target);
    if(wrongFraction>.01 || std::abs(cents)>25.0) {
        std::fprintf(stderr,"FAIL vibrato mode=%s target flips=%.2f%% out=%.3f err=%+.2fc\n",lowLatency?"low":"normal",100*wrongFraction,measured,cents); return false;
    }
    std::printf("PASS vibrato mode=%s target flips=%.2f%% mean out=%.3f err=%+.2fc\n",lowLatency?"low":"normal",100*wrongFraction,measured,cents);
    return true;
}
}

int main() {
    for(bool lowLatency : {false,true}) {
        if(!checkSteadyCorrection(lowLatency)) return 1;
        if(!checkAbruptTransitions(lowLatency)) return 2;
        if(!checkVibratoStability(lowLatency)) return 3;
    }
    std::puts("All targeted corrected-pitch latency/accuracy checks passed.");
    return 0;
}
