#include "FusionPeriodGuard.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

using autotune::FusionPeriodGuard;
constexpr double pi = 3.14159265358979323846;

static void expect(bool success, const char* message) {
    // Do not use assert(): Release builds disable it via NDEBUG.
    if (!success) {
        std::fprintf(stderr, "Fusion guard FAILURE: %s\n", message);
        std::exit(1);
    }
}

static void checkRate(double sr, int block) {
    FusionPeriodGuard guard;
    guard.prepare(sr);
    const int samples = (int)(sr * 0.25);
    std::vector<float> left((std::size_t)samples), right((std::size_t)samples);
    for (int i = 0; i < samples; ++i) {
        left[(std::size_t)i] = (float)(0.36 * std::sin(2.0 * pi * 220.0 * i / sr));
        right[(std::size_t)i] = -left[(std::size_t)i]; // anti-phase stereo must not cancel
    }
    for (int offset = 0; offset < samples; offset += block) {
        const float* channels[] = {left.data() + offset, right.data() + offset};
        guard.push(channels, 2, std::min(block, samples-offset));
    }
    const auto good = guard.inspect(220.0f);
    expect(good.available && good.periodScore > 0.93f
        && good.twoPeriodScore > 0.93f, "correct fundamental periodicity");
    const auto octave = guard.inspect(440.0f);
    expect(octave.available && octave.periodScore < -0.80f
        && octave.twoPeriodScore > 0.90f, "detect half-period octave alias");

    guard.observe(1, 220.0f, 0.99f, 0.01f, true, 256);
    expect(guard.correlationEvaluations() == 0,
           "clean voiced audio should bypass auxiliary correlations");
    expect(guard.guardedNoteConfirmMs(10.0f, true, false, 20.0f) == 10.0f,
           "clean voiced audio must not slow note confirmation");

    // YIN falsely suggests 440Hz on a clean 220Hz fundamental. Correlation at
    // 1/440 sec is anti-phase, at 2/440 sec returns to the same phase.
    for (int n = 2; n < 12; ++n)
        guard.observe((unsigned)n, 440.0f, 0.59f, 0.01f, true,
                      (int)(sr * 0.004));
    expect(guard.guardedNoteConfirmMs(10.0f, true, false, 20.0f) > 10.0f,
           "octave-up ambiguity must extend only note confirmation");
    expect(guard.correlationEvaluations() <= 7,
           "correlation scans must be limited in weak frames");
    expect(guard.guardedNoteConfirmMs(10.0f, true, false, 20.0f) <= 20.0f,
           "maximum additional confirmation must remain 10 ms");

    expect(guard.guardedNoteConfirmMs(0.0f, true, false, 20.0f) == 0.0f,
           "instant confirmation must not change");
    expect(guard.guardedNoteConfirmMs(10.0f, true, true, 20.0f) == 10.0f,
           "Precision Hard Tune must not change");
    expect(guard.guardedNoteConfirmMs(10.0f, true, false, 0.0f) == 10.0f,
           "0ms retune must not change");
    expect(guard.guardedNoteConfirmMs(10.0f, false, false, 20.0f) == 10.0f,
           "non-strict legacy mode must not change");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    expect(!guard.inspect(nan).available, "NaN frequency rejected");
    const float* invalid[] = {&nan};
    guard.push(invalid, 1, 1);
    guard.observe(12, nan, nan, nan, true, 192);
    expect(std::isfinite(guard.ambiguitySeconds()), "NaN cannot contaminate risk");

    guard.reset();
    expect(!guard.inspect(220.0f).available, "reset clears history");
    expect(guard.correlationEvaluations() == 0, "reset clears CPU telemetry");
}

static void checkCallbackInvariance(double sr) {
    FusionPeriodGuard tiny, large;
    tiny.prepare(sr);
    large.prepare(sr);
    // The first sample initializes the hop counters in both instances.
    tiny.observe(0, 220.0f, 0.55f, 0.90f, true, 1);
    large.observe(0, 220.0f, 0.55f, 0.90f, true, 1);
    for (int hop = 1; hop <= 15; ++hop) {
        // Six 32-sample blocks for each new analysis hop vs one 192 block.
        for (int j = 0; j < 5; ++j)
            tiny.observe((unsigned)(hop - 1), 220.0f, 0.55f, 0.90f, true, 32);
        tiny.observe((unsigned)hop, 220.0f, 0.55f, 0.90f, true, 32);
        large.observe((unsigned)hop, 220.0f, 0.55f, 0.90f, true, 192);
    }
    expect(std::abs(tiny.ambiguitySeconds() - large.ambiguitySeconds()) < 0.00001f,
           "risk integrator must use elapsed samples, not callback count");
    expect(tiny.correlationEvaluations() == 0
        && large.correlationEvaluations() == 0,
        "known two-cycle failure must skip redundant correlations");
    // Confidence recovery decays risk, rather than locking the engine in
    // a permanently cautious state.
    for (int hop = 16; hop < 50; ++hop)
        tiny.observe((unsigned)hop, 220.0f, 0.99f, 0.01f, true, 192);
    expect(tiny.ambiguitySeconds() == 0.0f, "risk should recover after clean audio");
}

int main() {
    for (double rate: {44100.0, 48000.0, 96000.0}) {
        for (int block: {32, 64, 128, 256, 1024})
            checkRate(rate, block);
        checkCallbackInvariance(rate);
    }
    std::puts("FusionPeriodGuard v2: periodicity, octave, CPU and callback tests passed");
}
