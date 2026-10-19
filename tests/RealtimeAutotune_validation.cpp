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
    std::vector<float> intrinsicLatency ((size_t) 2 * (size_t) n, -1.0f);
    float* channels[] = { left.data(), right.data() };
    engine.processChannels (channels, 2, n, intrinsicLatency.data());

    if (std::memcmp (left.data(), originalLeft.data(), n * sizeof (float)) != 0
        || std::memcmp (right.data(), originalRight.data(), n * sizeof (float)) != 0)
    {
        std::fprintf (stderr, "FAIL zero strength: Amount=0 changed samples despite no correction being requested\n");
        return false;
    }
    for (const float delay : intrinsicLatency)
        if (delay != 0.0f)
        {
            std::fprintf (stderr, "FAIL zero strength: direct path reported nonzero intrinsic delay %.3f\n", delay);
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


bool checkTieBreakingAcrossModes()
{
    constexpr double sampleRate = 48000.0;
    constexpr double transitionHz = 277.18263; // C#4, midway between C4 and D4
    constexpr double expectedHz = 293.66477;   // ties consistently resolve upward to D4
    constexpr int n = (int) (sampleRate * 0.75);
    constexpr int transitionSample = (int) (sampleRate * 0.25);

    for (const bool lowLatency : { false, true })
    {
        autotune::RealtimeAutotune engine;
        engine.prepare (sampleRate);
        autotune::Settings settings;
        settings.key = 0;
        settings.scale = autotune::Scale::Major;
        settings.strength = 1.0f;
        settings.retuneMs = 0.0f;
        settings.follow = 1.0f;
        settings.vibratoPreserve = 1.0f;
        settings.noteStabilizerMs = 0.0f;
        settings.voicingThreshold = 0.5f;
        settings.lowLatency = lowLatency;
        engine.setSettings (settings);
        engine.reset();

        std::vector<float> audio ((size_t) n);
        double phase = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double hz = i < transitionSample ? 205.0 : transitionHz;
            audio[(size_t) i] = 0.3f * (float) std::sin (phase);
            phase += 2.0 * pi * hz / sampleRate;
            if (phase >= 2.0 * pi) phase -= 2.0 * pi;
        }
        engine.process (audio.data(), n);

        // Use interpolated positive-going zero crossings after the transition.
        const int start = (int) (sampleRate * 0.43);
        const int end = (int) (sampleRate * 0.72);
        double first = -1.0, last = -1.0;
        int crossings = 0;
        for (int i = start + 1; i < end; ++i)
        {
            const float a = audio[(size_t) i - 1];
            const float b = audio[(size_t) i];
            if (a <= 0.0f && b > 0.0f)
            {
                const double crossing = (double) (i - 1) + (double) (-a) / (double) (b - a);
                if (first < 0.0) first = crossing;
                last = crossing;
                ++crossings;
            }
        }
        const double measuredHz = crossings > 3
            ? (double) (crossings - 1) * sampleRate / (last - first) : 0.0;
        const double cents = measuredHz > 0.0
            ? 1200.0 * std::log2 (measuredHz / expectedHz) : 1.0e6;
        if (engine.getTargetMidi() != 62 || ! std::isfinite (cents) || std::abs (cents) > 35.0)
        {
            std::fprintf (stderr, "FAIL tie transition: lowLatency=%d detectedMidi=%.3f target=%d measured=%.2f Hz error=%.1f cents\n",
                          lowLatency ? 1 : 0, engine.getDetectedMidi(), engine.getTargetMidi(), measuredHz, cents);
            return false;
        }
    }
    std::puts ("PASS scale tie-breaking: C#4 transition targets D4 consistently in Normal and Low Latency");
    return true;
}


bool checkFixedLatencyCompensation()
{
    constexpr int fixedLatency = 512;
    constexpr int numSamples = 7000;
    constexpr int inputImpulse = 4000;
    constexpr double sampleRate = 48000.0;

    for (const int engineDelay : { 0, 75, 210 })
    {
        autotune::OutputLatencyCompensator compensator;
        compensator.prepare (fixedLatency, sampleRate);
        std::vector<float> engineOutput ((size_t) numSamples, 0.0f);
        std::vector<float> latency ((size_t) numSamples, (float) engineDelay);
        if (inputImpulse + engineDelay < numSamples)
            engineOutput[(size_t) (inputImpulse + engineDelay)] = 1.0f;
        float* channels[] = { engineOutput.data() };
        compensator.processChannels (channels, 1, numSamples, latency.data());

        int peakAt = -1;
        float peak = 0.0f;
        for (int i = inputImpulse; i < numSamples; ++i)
        {
            const float magnitude = std::abs (engineOutput[(size_t) i]);
            if (magnitude > peak) { peak = magnitude; peakAt = i; }
        }
        const int expected = inputImpulse + fixedLatency;
        if (peakAt != expected || peak < 0.99f)
        {
            std::fprintf (stderr, "FAIL fixed latency: intrinsic=%d peakAt=%d expected=%d peak=%.4f\n",
                          engineDelay, peakAt, expected, peak);
            return false;
        }
    }

    // Change intrinsic latency after the first event. Each event should still land
    // at the same host-compensated sample after the new delay tap has settled.
    {
        autotune::OutputLatencyCompensator compensator;
        compensator.prepare (fixedLatency, sampleRate);
        std::vector<float> audio ((size_t) numSamples, 0.0f);
        std::vector<float> latency ((size_t) numSamples, 75.0f);
        constexpr int firstInput = 1000, firstDelay = 75;
        constexpr int secondInput = 5000, secondDelay = 210;
        audio[(size_t) (firstInput + firstDelay)] = 1.0f;
        audio[(size_t) (secondInput + secondDelay)] = 0.8f;
        std::fill (latency.begin() + 3000, latency.end(), (float) secondDelay);
        float* channels[] = { audio.data() };
        compensator.processChannels (channels, 1, numSamples, latency.data());
        const int expectedFirst = firstInput + fixedLatency;
        const int expectedSecond = secondInput + fixedLatency;
        for (const auto expected : { expectedFirst, expectedSecond })
        {
            int peakAt = -1;
            float peak = 0.0f;
            for (int i = expected - 8; i <= expected + 8; ++i)
            {
                const float magnitude = std::abs (audio[(size_t) i]);
                if (magnitude > peak) { peak = magnitude; peakAt = i; }
            }
            const float expectedPeak = expected == expectedFirst ? 1.0f : 0.8f;
            if (peakAt != expected || std::abs (peak - expectedPeak) > 0.01f)
            {
                std::fprintf (stderr, "FAIL dynamic latency: peakAt=%d expected=%d peak=%.4f\n",
                              peakAt, expected, peak);
                return false;
            }
        }
    }
    std::puts ("PASS host latency compensation: fixed and changing intrinsic paths align at a common output sample");
    return true;
}


bool checkStereoLatencyCompensation()
{
    constexpr int fixedLatency = 512;
    constexpr int numSamples = 7000;
    constexpr int inputImpulse = 4000;
    constexpr int leftIntrinsic = 75;
    constexpr int rightIntrinsic = 210;
    autotune::OutputLatencyCompensator compensator;
    compensator.prepare (fixedLatency, 48000.0);

    std::vector<float> left ((size_t) numSamples, 0.0f);
    std::vector<float> right ((size_t) numSamples, 0.0f);
    std::vector<float> latency ((size_t) 2 * (size_t) numSamples, 0.0f);
    left[(size_t) (inputImpulse + leftIntrinsic)] = 1.0f;
    right[(size_t) (inputImpulse + rightIntrinsic)] = 1.0f;
    std::fill (latency.begin(), latency.begin() + numSamples, (float) leftIntrinsic);
    std::fill (latency.begin() + numSamples, latency.end(), (float) rightIntrinsic);
    float* channels[] = { left.data(), right.data() };
    compensator.processChannels (channels, 2, numSamples, latency.data());

    const int expected = inputImpulse + fixedLatency;
    const auto peakAt = [] (const std::vector<float>& data, int from, int to)
    {
        int index = -1;
        float amplitude = 0.0f;
        for (int i = from; i <= to; ++i)
            if (std::abs (data[(size_t)i]) > amplitude)
            {
                amplitude = std::abs (data[(size_t)i]);
                index = i;
            }
        return std::pair<int, float> { index, amplitude };
    };
    const auto l = peakAt (left, expected - 4, expected + 4);
    const auto r = peakAt (right, expected - 4, expected + 4);
    if (l.first != expected || r.first != expected || l.second < 0.99f || r.second < 0.99f)
    {
        std::fprintf (stderr, "FAIL stereo compensation: L=(%d, %.3f) R=(%d, %.3f) expected=%d\n",
                      l.first, l.second, r.first, r.second, expected);
        return false;
    }
    std::puts ("PASS stereo compensation: independent channel delays realign L/R at the same host output sample");
    return true;
}

bool checkLatencyBudgetAtLowestPitch()
{
    for (const double sampleRate : { 44100.0, 48000.0, 96000.0 })
    {
        for (const bool lowLatency : { false, true })
        {
            autotune::RealtimeAutotune engine;
            engine.prepare (sampleRate, 70.0f, 1000.0f);
            autotune::Settings settings;
            settings.key = 0;
            settings.scale = autotune::Scale::Major;
            settings.strength = 1.0f;
            settings.retuneMs = 0.0f;
            settings.follow = 1.0f;
            settings.noteStabilizerMs = 0.0f;
            settings.vibratoPreserve = 1.0f;
            settings.voicingThreshold = 0.5f;
            settings.lowLatency = lowLatency;
            engine.setSettings (settings);
            engine.reset();

            const int n = (int) (sampleRate * 0.35);
            std::vector<float> left ((size_t) n), right ((size_t) n);
            std::vector<float> delays ((size_t) 2 * (size_t) n, 0.0f);
            for (int i = 0; i < n; ++i)
            {
                const double phase = 2.0 * pi * 70.0 * (double) i / sampleRate;
                left[(size_t) i] = 0.3f * (float) std::sin (phase);
                right[(size_t) i] = 0.23f * (float) std::sin (phase + 1.17);
            }
            float* channels[] = { left.data(), right.data() };
            engine.processChannels (channels, 2, n, delays.data());

            float maxDelay = 0.0f;
            for (int i = (int) (sampleRate * 0.05); i < n; ++i)
            {
                maxDelay = std::max (maxDelay, delays[(size_t) i]);
                maxDelay = std::max (maxDelay, delays[(size_t) n + (size_t) i]);
            }
            const int budget = (int) std::lround (sampleRate * 2.5 / 70.0);
            if (! std::isfinite (maxDelay) || maxDelay >= (float) budget)
            {
                std::fprintf (stderr, "FAIL latency budget: sr=%.0f lowLatency=%d max=%.1f budget=%d\n",
                              sampleRate, lowLatency ? 1 : 0, maxDelay, budget);
                return false;
            }
        }
    }
    std::puts ("PASS host latency budget: stereo 70 Hz delays fit 2.5-period budget at 44.1/48/96 kHz");
    return true;
}

bool checkLatencyGridBudget()
{
    constexpr double sampleRate = 48000.0;
    constexpr float frequencies[] = { 70.0f, 82.41f, 98.0f, 110.0f, 146.83f,
                                      220.0f, 329.63f, 440.0f, 659.25f, 987.77f };
    const int n = (int) (sampleRate * 0.35);
    const float budget = (float) std::lround (sampleRate * 2.5 / 70.0);
    float worstDelay = 0.0f;
    float worstFrequency = 0.0f;
    bool worstLowLatency = false;

    for (const bool lowLatency : { false, true })
        for (const float frequency : frequencies)
        {
            autotune::RealtimeAutotune engine;
            engine.prepare (sampleRate, 70.0f, 1000.0f);
            autotune::Settings settings;
            settings.key = 0;
            settings.scale = autotune::Scale::Major;
            settings.strength = 1.0f;
            settings.retuneMs = 0.0f;
            settings.follow = 1.0f;
            settings.noteStabilizerMs = 0.0f;
            settings.vibratoPreserve = 1.0f;
            settings.voicingThreshold = 0.5f;
            settings.lowLatency = lowLatency;
            engine.setSettings (settings);
            engine.reset();

            std::vector<float> left ((size_t)n), right ((size_t)n);
            std::vector<float> latency ((size_t)2 * (size_t)n, 0.0f);
            for (int i = 0; i < n; ++i)
            {
                const double phase = 2.0 * pi * (double)frequency * (double)i / sampleRate;
                left[(size_t)i] = 0.3f * (float)std::sin (phase);
                right[(size_t)i] = 0.27f * (float)std::sin (phase + 1.11);
            }
            float* channels[] = { left.data(), right.data() };
            engine.processChannels (channels, 2, n, latency.data());

            float measured = 0.0f;
            const int start = (int)(sampleRate * 0.05);
            for (int i = start; i < n; ++i)
            {
                measured = std::max (measured, latency[(size_t)i]);
                measured = std::max (measured, latency[(size_t)n + (size_t)i]);
            }
            if (!std::isfinite (measured) || measured >= budget)
            {
                std::fprintf (stderr,
                    "FAIL latency grid: f=%.2f lowLatency=%d max=%.1f samples budget=%.0f\n",
                    frequency, lowLatency ? 1 : 0, measured, budget);
                return false;
            }
            if (measured > worstDelay)
            {
                worstDelay = measured;
                worstFrequency = frequency;
                worstLowLatency = lowLatency;
            }
        }

    std::printf ("PASS latency grid: 10 stereo fundamentals in both modes stay under %.0f samples; worst %.1f at %.2f Hz (%s)\n",
                 budget, worstDelay, worstFrequency, worstLowLatency ? "Low Latency" : "Normal");
    return true;
}

bool checkDynamicBlockStress()
{
    constexpr int blockSizes[] = { 1, 7, 31, 64, 127, 255, 512, 1024, 2048 };
    for (const double sampleRate : { 44100.0, 48000.0, 96000.0 })
    {
        autotune::RealtimeAutotune engine;
        engine.prepare (sampleRate, 70.0f, 1000.0f);
        const int fixedLatency = (int) std::lround (sampleRate * 2.5 / 70.0);
        autotune::OutputLatencyCompensator compensator;
        compensator.prepare (fixedLatency, sampleRate);
        int64_t sampleCursor = 0;

        for (int block = 0; block < 90; ++block)
        {
            const int n = blockSizes[block % (int)(sizeof(blockSizes) / sizeof(blockSizes[0]))];
            autotune::Settings settings;
            settings.key = block % 12;
            settings.scale = block % 3 == 0 ? autotune::Scale::Chromatic
                          : block % 3 == 1 ? autotune::Scale::Major : autotune::Scale::Minor;
            settings.referenceHz = 440.0f;
            settings.strength = block % 13 < 4 ? 0.0f : block % 13 < 8 ? 0.5f : 1.0f;
            settings.retuneMs = block % 4 == 0 ? 0.0f : block % 4 == 1 ? 15.0f : 120.0f;
            settings.follow = (block % 5) * 0.25f;
            settings.noteStabilizerMs = block % 3 == 0 ? 0.0f : block % 3 == 1 ? 120.0f : 450.0f;
            settings.vibratoPreserve = 0.7f;
            settings.voicingThreshold = 0.5f;
            settings.lowLatency = (block % 2) != 0;
            settings.bypass = (block % 19 == 18);
            engine.setSettings (settings);

            std::vector<float> left ((size_t)n), right ((size_t)n);
            std::vector<float> latency ((size_t)2 * (size_t)n, -1.0f);
            for (int i = 0; i < n; ++i)
            {
                const double time = (double)(sampleCursor + i) / sampleRate;
                const double sweep = 70.0 + 930.0 * (0.5 + 0.5 * std::sin (2.0 * pi * time * 0.17));
                const double phase = 2.0 * pi * sweep * time;
                left[(size_t)i] = 0.25f * (float) std::sin (phase);
                right[(size_t)i] = 0.21f * (float) std::sin (phase + 0.83);
            }
            float* channels[] = { left.data(), right.data() };
            engine.processChannels (channels, 2, n, latency.data());
            compensator.processChannels (channels, 2, n, latency.data());

            for (int i = 0; i < n; ++i)
            {
                if (!std::isfinite (left[(size_t)i]) || !std::isfinite (right[(size_t)i]))
                {
                    std::fprintf (stderr, "FAIL dynamic block stress: non-finite output sr=%.0f block=%d size=%d\n",
                                  sampleRate, block, n);
                    return false;
                }
                for (int c = 0; c < 2; ++c)
                {
                    const float value = latency[(size_t)c * (size_t)n + (size_t)i];
                    if (!std::isfinite (value) || value < 0.0f || value > (float)fixedLatency)
                    {
                        std::fprintf (stderr, "FAIL dynamic block stress: bad telemetry sr=%.0f block=%d ch=%d value=%.3f\n",
                                      sampleRate, block, c, value);
                        return false;
                    }
                }
            }
            sampleCursor += n;
        }
    }
    std::puts ("PASS dynamic block stress: 3 sample rates, tiny/large blocks, parameter changes, bypass and stereo remain finite");
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
    if (! checkTieBreakingAcrossModes()) return 8;
    if (! checkFixedLatencyCompensation()) return 9;
    if (! checkStereoLatencyCompensation()) return 10;
    if (! checkLatencyBudgetAtLowestPitch()) return 11;
    if (! checkLatencyGridBudget()) return 12;
    if (! checkDynamicBlockStress()) return 13;
    if (! checkScaleCorrection()) return 14;
    std::puts ("All standalone RealtimeAutotune checks passed.");
    return 0;
}
