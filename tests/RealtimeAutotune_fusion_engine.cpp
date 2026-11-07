#include "SkellyFusionEngine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

static void expect(bool yes, const char* message) {
    if (!yes) {
        std::fprintf(stderr, "Fusion engine FAILURE: %s\n", message);
        std::exit(1);
    }
}

static void smoke(double sampleRate, int blockSize, bool lowLatency,
                  bool snapLive, float strength) {
    autotune::SkellyFusionEngine engine;
    engine.prepare(sampleRate, 70.0f, 1000.0f);
    autotune::Settings settings;
    settings.scale = autotune::Scale::Chromatic;
    settings.key = 0;
    settings.strictRealtime = true;
    settings.ultraLowLatency = true;
    settings.lowLatency = lowLatency;
    settings.snapLive = snapLive && lowLatency;
    settings.precisionHardTune = true;
    settings.retuneMs = 0.0f;
    settings.noteConfirmMs = 0.0f;
    settings.voicingThreshold = 0.68f;
    settings.strength = strength;
    engine.setSettings(settings);

    std::vector<float> left((std::size_t)blockSize, 0.0f);
    std::vector<float> right((std::size_t)blockSize, 0.0f);
    std::vector<float> latency((std::size_t)blockSize * 2u, 0.0f);
    const int total = (int)std::lround(sampleRate * 0.24);
    double leftPower = 0.0, rightPower = 0.0, sumPower = 0.0;
    int counted = 0;
    constexpr double pi = 3.14159265358979323846;

    for (int start = 0; start < total; start += blockSize) {
        const int count = std::min(blockSize, total-start);
        for (int i=0; i<count; ++i) {
            const int sample = start+i;
            const float x=(float)(0.30 * std::sin(2.0*pi*220.0*sample/sampleRate));
            left[(std::size_t)i] = x;
            right[(std::size_t)i] = -x;
        }
        // A host's invalid sample must not poison the pitch detector, stereo
        // renderer, output delays or secondary periodicity witness.
        if (start <= total / 4 && start + count > total / 4)
            left[(std::size_t)(total / 4 - start)] =
                std::numeric_limits<float>::quiet_NaN();

        float* channels[] = {left.data(), right.data()};
        engine.processChannels(channels, 2, count, latency.data());
        for (int i=0;i<count;++i) {
            const float l=left[(std::size_t)i], r=right[(std::size_t)i];
            expect(std::isfinite(l)&&std::isfinite(r), "all output audio must remain finite");
            expect(std::isfinite(latency[(std::size_t)i])
                   && std::isfinite(latency[(std::size_t)count+(std::size_t)i]),
                   "intrinsic delay telemetry must remain finite");
            if (start+i > total/2) {
                leftPower+=(double)l*l;
                rightPower+=(double)r*r;
                sumPower+=(double)(l+r)*(l+r);
                ++counted;
            }
        }
    }
    const float detected = engine.getDetectedMidi();
    const float confidence = engine.getConfidence();
    expect(std::isfinite(detected) && std::abs(detected-57.0f) < 0.50f,
           "the 220Hz A3 pitch must be tracked after a sustained phrase");
    expect(std::isfinite(confidence) && confidence > 0.5f,
           "sustained pitch must have detector confidence");
    expect(engine.getTargetMidi()==57, "chromatic A3 note lock");
    expect(std::isfinite(engine.getCorrectionSemitones()), "finite note correction");
    expect(counted > 0 && leftPower / counted > 0.0001
           && rightPower / counted > 0.0001, "non-silent stereo audio output");
    if (strength == 0.0f)
        expect(sumPower < 0.04 * (leftPower + rightPower),
               "zero-amount bypass must retain anti-phase stereo alignment");
}

int main() {
    for (double sampleRate: {44100.0, 48000.0, 96000.0}) {
        smoke(sampleRate, 64, true, false, 1.0f);  // 3.5ms Live
        smoke(sampleRate, 256, true, true, 1.0f); // 4ms Snap Live
        smoke(sampleRate, 1024, false, false, 1.0f); // 8ms Quality
        smoke(sampleRate, 128, true, false, 0.0f); // aligned unprocessed dry
    }
    std::puts("SkellyFusionEngine: live/snap/quality/dry stereo signal checks passed");
}
