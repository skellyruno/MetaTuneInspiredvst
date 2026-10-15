// Standalone smoke/quality checks for RealtimeAutotune.h (C++17).
// Build: c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic RealtimeAutotune_validation.cpp -o autotune_validation
#include "RealtimeAutotune.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
constexpr double pi = 3.14159265358979323846;

bool checkPitchDetection()
{
    for (const double sampleRate : { 44100.0, 48000.0, 96000.0 })
    {
        for (const float hz : { 82.41f, 110.0f, 220.0f, 233.08f, 440.0f, 880.0f })
        {
            autotune::RealtimeAutotune engine;
            engine.prepare (sampleRate);
            autotune::Settings settings;
            settings.scale = autotune::Scale::Chromatic;
            settings.strength = 0.0f;
            settings.voicingThreshold = 0.5f;
            engine.setSettings (settings);
            engine.reset();

            std::vector<float> audio ((size_t) (sampleRate * 0.5));
            for (size_t i = 0; i < audio.size(); ++i)
                audio[i] = 0.25f * std::sin (2.0 * pi * hz * (double) i / sampleRate);
            engine.process (audio.data(), (int) audio.size());

            const float expectedMidi = 69.0f + 12.0f * std::log2 (hz / 440.0f);
            const float errorCents = std::abs (engine.getDetectedMidi() - expectedMidi) * 100.0f;
            if (! std::isfinite (errorCents) || errorCents > 25.0f)
            {
                std::fprintf (stderr, "FAIL pitch: sr=%.0f Hz=%.2f midi=%.4f error=%.2f cents\n",
                              sampleRate, hz, engine.getDetectedMidi(), errorCents);
                return false;
            }
        }
    }
    std::puts ("PASS pitch detection: six sine pitches at 44.1/48/96 kHz within 25 cents");
    return true;
}

bool checkExactBypassAndStereo()
{
    constexpr int n = 4096;
    constexpr double sampleRate = 48000.0;
    autotune::RealtimeAutotune engine;
    engine.prepare (sampleRate);

    autotune::Settings settings;
    settings.bypass = true;
    engine.setSettings (settings);
    engine.reset();

    std::vector<float> left (n), right (n);
    for (int i = 0; i < n; ++i)
    {
        left[(size_t) i] = 0.7f * std::sin ((float) i * 0.071f);
        right[(size_t) i] = -0.4f * std::sin ((float) i * 0.071f)
                            + 0.03f * std::sin ((float) i * 0.013f);
    }
    const auto originalLeft = left;
    const auto originalRight = right;
    float* channels[] = { left.data(), right.data() };
    engine.processChannels (channels, 2, n);
    if (std::memcmp (left.data(), originalLeft.data(), n * sizeof (float)) != 0
        || std::memcmp (right.data(), originalRight.data(), n * sizeof (float)) != 0)
    {
        std::fprintf (stderr, "FAIL bypass: the bypassed samples changed\n");
        return false;
    }

    settings.bypass = false;
    settings.scale = autotune::Scale::Chromatic;
    settings.strength = 0.8f;
    engine.setSettings (settings);
    engine.reset();
    for (int i = 0; i < n; ++i)
    {
        const float v = 0.5f * std::sin (2.0 * pi * 233.0 * (double) i / sampleRate);
        left[(size_t) i] = v;
        right[(size_t) i] = -v;
    }
    engine.processChannels (channels, 2, n);

    double leftEnergy = 0.0, rightEnergy = 0.0, crossEnergy = 0.0;
    for (int i = 512; i < n; ++i)
    {
        const double l = left[(size_t) i], r = right[(size_t) i];
        if (! std::isfinite (l) || ! std::isfinite (r))
        {
            std::fprintf (stderr, "FAIL stereo: non-finite output\n");
            return false;
        }
        leftEnergy += l * l;
        rightEnergy += r * r;
        crossEnergy += l * r;
    }
    if (leftEnergy < 1.0 || rightEnergy < 1.0 || crossEnergy >= 0.0)
    {
        std::fprintf (stderr, "FAIL stereo: channels collapsed or silent\n");
        return false;
    }
    std::puts ("PASS stereo: exact bypass and anti-phase input stays stereo when tuning is active");
    return true;
}

bool checkZeroStrengthIsTransparent()
{
    constexpr double sampleRate = 48000.0;
    constexpr int n = 16384;
    autotune::RealtimeAutotune engine;
    engine.prepare (sampleRate);
    autotune::Settings settings;
    settings.scale = autotune::Scale::Major;
    settings.key = 0;
    settings.strength = 0.0f;
    settings.bypass = false;
    engine.setSettings (settings);
    engine.reset();

    std::vector<float> left ((size_t)n), right ((size_t)n);
    for (int i = 0; i < n; ++i)
    {
        const double t = (double)i / sampleRate;
        left[(size_t)i] = 0.31f * std::sin (2.0 * pi * 233.0 * t)
                        + 0.09f * std::sin (2.0 * pi * 468.7 * t);
        right[(size_t)i] = -0.27f * std::sin (2.0 * pi * 233.0 * t)
                         + 0.04f * std::sin (2.0 * pi * 701.0 * t);
    }
    const auto originalLeft = left;
    const auto originalRight = right;
    float* channels[] = { left.data(), right.data() };
    engine.processChannels (channels, 2, n);

    if (std::memcmp (left.data(), originalLeft.data(), n * sizeof (float)) != 0
        || std::memcmp (right.data(), originalRight.data(), n * sizeof (float)) != 0)
    {
        std::fprintf (stderr, "FAIL zero strength: Amount=0 changed samples despite no correction being requested\n");
        return false;
    }
    std::puts ("PASS Amount=0: stereo samples remain bit-identical while tracking/shifter history stays warm");
    return true;
}

bool checkZeroStrengthMonoTransparent()
{
    constexpr double sampleRate = 44100.0;
    constexpr int n = 8192;
    autotune::RealtimeAutotune engine;
    engine.prepare (sampleRate);
    autotune::Settings settings;
    settings.scale = autotune::Scale::Chromatic;
    settings.strength = 0.0f;
    engine.setSettings (settings);
    engine.reset();

    std::vector<float> audio ((size_t)n);
    for (int i = 0; i < n; ++i)
    {
        const double t = (double)i / sampleRate;
        audio[(size_t)i] = 0.28f * std::sin (2.0 * pi * 173.0 * t)
                         + 0.12f * std::sin (2.0 * pi * 347.0 * t)
                         + 0.03f * std::sin (2.0 * pi * 719.0 * t);
    }
    const auto original = audio;
    engine.process (audio.data(), n);
    if (std::memcmp (audio.data(), original.data(), n * sizeof (float)) != 0)
    {
        std::fprintf (stderr, "FAIL mono zero strength: Amount=0 changed samples\n");
        return false;
    }
    std::puts ("PASS Amount=0: mono process path is bit-identical");
    return true;
}

bool checkLowLatencyModeReducesDelay()
{
    constexpr double sampleRate = 48000.0;
    constexpr float testHz = 440.0f;
    constexpr int n = 48000;
    const auto medianDelay = [&] (bool lowLatency)
    {
        autotune::RealtimeAutotune engine;
        engine.prepare (sampleRate);
        autotune::Settings settings;
        settings.scale = autotune::Scale::Chromatic;
        settings.strength = 1.0f;
        settings.retuneMs = 0.0f;
        settings.voicingThreshold = 0.65f;
        settings.lowLatency = lowLatency;
        engine.setSettings (settings);
        engine.reset();

        std::vector<double> delays;
        delays.reserve (n / 4);
        for (int i = 0; i < n; ++i)
        {
            const float sample = 0.4f * std::sin (2.0 * pi * testHz * (double)i / sampleRate);
            engine.process (sample);
            if (i > (int)(sampleRate * 0.1) && engine.isVoiced())
                delays.push_back (engine.getLatencySamples());
        }
        if (delays.empty()) return -1.0;
        const auto middle = delays.begin() + (std::ptrdiff_t)(delays.size() / 2);
        std::nth_element (delays.begin(), middle, delays.end());
        return *middle;
    };

    const double normal = medianDelay (false);
    const double low = medianDelay (true);
    if (normal <= 0.0 || low <= 0.0 || low >= normal * 0.75 || low >= sampleRate * 0.002)
    {
        std::fprintf (stderr, "FAIL low latency: normal %.2f samples, low %.2f samples\n", normal, low);
        return false;
    }
    std::printf ("PASS Low Latency geometry: 440 Hz median delay %.1f -> %.1f samples (%.2f ms)\n",
                 normal, low, low * 1000.0 / sampleRate);
    return true;
}

bool checkStereoLatencyPathReportsActiveShifters()
{
    constexpr double sampleRate = 48000.0;
    constexpr int n = 48000;
    constexpr int blockSize = 64;
    autotune::RealtimeAutotune engine;
    engine.prepare (sampleRate);
    autotune::Settings settings;
    settings.scale = autotune::Scale::Chromatic;
    settings.strength = 1.0f;
    settings.retuneMs = 0.0f;
    settings.lowLatency = true;
    settings.voicingThreshold = 0.65f;
    engine.setSettings (settings);
    engine.reset();

    std::vector<float> left ((size_t)n), right ((size_t)n);
    for (int i = 0; i < n; ++i)
        left[(size_t)i] = right[(size_t)i] = 0.4f * std::sin (2.0 * pi * 440.0 * (double)i / sampleRate);

    std::vector<double> delays;
    for (int offset = 0; offset < n; offset += blockSize)
    {
        float* channels[] = { left.data() + offset, right.data() + offset };
        engine.processChannels (channels, 2, std::min (blockSize, n - offset));
        if (offset > (int)(sampleRate * 0.1) && engine.isVoiced())
            delays.push_back (engine.getLatencySamples());
    }
    if (delays.empty())
    {
        std::fprintf (stderr, "FAIL stereo latency path: no voiced latency samples\n");
        return false;
    }
    const auto middle = delays.begin() + (std::ptrdiff_t)(delays.size() / 2);
    std::nth_element (delays.begin(), middle, delays.end());
    const double median = *middle;
    if (median <= 0.0 || median >= sampleRate * 0.002)
    {
        std::fprintf (stderr, "FAIL stereo latency path: median %.2f samples\n", median);
        return false;
    }
    std::printf ("PASS stereo latency reporting: median %.1f samples (%.2f ms), using active channel shifters\n",
                 median, median * 1000.0 / sampleRate);
    return true;
}

bool checkLowLatencyLevelStability()
{
    constexpr double sampleRate = 48000.0;
    constexpr int n = 48000;
    autotune::RealtimeAutotune engine;
    engine.prepare (sampleRate);
    autotune::Settings settings;
    settings.scale = autotune::Scale::Chromatic;
    settings.strength = 1.0f;
    settings.retuneMs = 0.0f;
    settings.lowLatency = true;
    settings.voicingThreshold = 0.65f;
    engine.setSettings (settings);
    engine.reset();

    double inputEnergy = 0.0, outputEnergy = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const float input = 0.4f * std::sin (2.0 * pi * 440.0 * (double)i / sampleRate);
        const float output = engine.process (input);
        if (i > (int)(sampleRate * 0.1))
        {
            inputEnergy += (double)input * input;
            outputEnergy += (double)output * output;
        }
    }
    const double rmsRatio = std::sqrt (outputEnergy / std::max (inputEnergy, 1.0e-20));
    if (! std::isfinite (rmsRatio) || rmsRatio < 0.93 || rmsRatio > 1.07)
    {
        std::fprintf (stderr, "FAIL low latency level: output/input RMS ratio %.4f\n", rmsRatio);
        return false;
    }
    std::printf ("PASS Low Latency unity-gain reconstruction: output/input RMS %.4f\n", rmsRatio);
    return true;
}

bool checkScaleCorrection()
{
    constexpr double sampleRate = 48000.0;
    constexpr float inputHz = 236.0f; // slightly sharp of A#3; C major should select B3
    const int n = (int) sampleRate;
    autotune::RealtimeAutotune engine;
    engine.prepare (sampleRate);
    autotune::Settings settings;
    settings.key = 0;
    settings.scale = autotune::Scale::Major;
    settings.strength = 1.0f;
    settings.retuneMs = 0.0f;
    settings.humanize = 0.0f;
    settings.vibratoPreserve = 1.0f;
    settings.follow = 1.0f;
    settings.voicingThreshold = 0.75f;
    settings.lowLatency = true;
    settings.noteStabilizerMs = 300.0f;
    engine.setSettings (settings);
    engine.reset();

    std::vector<float> audio ((size_t) n);
    for (int i = 0; i < n; ++i)
        audio[(size_t) i] = 0.25f * std::sin (2.0 * pi * inputHz * (double) i / sampleRate);
    engine.process (audio.data(), n);

    int crossings = 0;
    const int start = (int) (sampleRate * 0.5);
    for (int i = start + 1; i < n; ++i)
        if (audio[(size_t) i - 1] <= 0.0f && audio[(size_t) i] > 0.0f)
            ++crossings;
    const double measuredHz = (double) crossings / 0.5;
    if (engine.getTargetMidi() != 59 || measuredHz < 242.0 || measuredHz > 252.0)
    {
        std::fprintf (stderr, "FAIL correction: target=%d output=%.2f Hz\n", engine.getTargetMidi(), measuredHz);
        return false;
    }
    std::printf ("PASS scale correction: C-major target B3, measured output %.2f Hz\n", measuredHz);
    return true;
}
}

int main()
{
    if (! checkPitchDetection()) return 1;
    if (! checkExactBypassAndStereo()) return 2;
    if (! checkZeroStrengthIsTransparent()) return 3;
    if (! checkZeroStrengthMonoTransparent()) return 4;
    if (! checkLowLatencyModeReducesDelay()) return 5;
    if (! checkStereoLatencyPathReportsActiveShifters()) return 6;
    if (! checkLowLatencyLevelStability()) return 7;
    if (! checkScaleCorrection()) return 8;
    std::puts ("All standalone RealtimeAutotune checks passed.");
    return 0;
}
