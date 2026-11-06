#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace autotune {

// Lightweight, independent NSDF-style periodicity verifier. This deliberately
// does not replace the main YIN detector or change the synthesis clock.
// It only supplies conservative evidence to the note-transition controller.
// All storage is fixed-capacity and the audio-thread methods are noexcept.
class FusionPeriodGuard final {
public:
    struct Evidence {
        bool available = false;
        float periodScore = 0.0f;     // normalized correlation at estimated F0
        float twoPeriodScore = 0.0f;  // normalized correlation at twice the period
    };

    void prepare(double sampleRate) noexcept {
        sr = std::clamp(std::isfinite(sampleRate) ? sampleRate : 48000.0,
                        8000.0, 192000.0);
        reset();
    }

    void reset() noexcept {
        history.fill(0.0f);
        write = 0;
        energy = {0.0, 0.0};
        activeChannel = 0;
        candidateSamples = 0;
        riskSeconds = 0.0f;
        lastHop = std::numeric_limits<std::uint64_t>::max();
        evidence = {};
    }

    // Read ORIGINAL input before the main tuner overwrites it. For stereo, a
    // persistent stronger-channel choice avoids destructive L/R cancellation.
    // The guard never feeds this auxiliary waveform into the actual renderer.
    void push(const float* const* channels, int channelCount, int count) noexcept {
        if (channels == nullptr || channelCount <= 0 || count <= 0 || channels[0] == nullptr)
            return;
        const bool stereo = channelCount >= 2 && channels[1] != nullptr;
        const double alpha = 1.0 - std::exp(-1.0 / (sr * 0.040));
        const int hold = std::max(1, (int) std::lround(sr * 0.012));
        for (int i = 0; i < count; ++i) {
            const float l = sanitize(channels[0][i]);
            const float r = stereo ? sanitize(channels[1][i]) : l;
            energy[0] += alpha * ((double)l * l - energy[0]);
            energy[1] += alpha * ((double)r * r - energy[1]);
            if (!stereo) {
                activeChannel = 0;
                candidateSamples = 0;
            } else {
                const int alternate = 1 - activeChannel;
                if (energy[(std::size_t)alternate] > energy[(std::size_t)activeChannel] * 1.55) {
                    if (++candidateSamples >= hold) {
                        activeChannel = alternate;
                        candidateSamples = 0;
                    }
                } else {
                    candidateSamples = 0;
                }
            }
            history[(std::size_t)(write++ & mask)] = activeChannel == 0 ? l : r;
        }
    }

    Evidence inspect(float frequencyHz) const noexcept {
        Evidence out;
        if (!std::isfinite(frequencyHz) || frequencyHz < 50.0f || frequencyHz > 1400.0f)
            return out;
        const int period = (int) std::lround(sr / frequencyHz);
        const int window = std::clamp(period, 96, 1024);
        if (period < 3 || 2 * period + window + 8 >= (int) history.size()
            || write < (std::uint64_t)(2 * period + window + 8))
            return out;
        const int radius = std::clamp(period / 100, 1, 3);
        out.periodScore = bestScore(period, window, radius);
        out.twoPeriodScore = bestScore(2 * period, window, 2 * radius);
        out.available = true;
        return out;
    }

    // Called once after the main tuner processes a slice. Confidence/range
    // checks intentionally make the controller conservative: clean, strongly
    // tracked notes and zero-ms hard tune are not altered.
    void observe(std::uint64_t analysisHop, float detectorFrequencyHz,
                 float confidence, float twoCycleAperiodicity,
                 bool voiced, int processedSamples) noexcept {
        if (analysisHop == lastHop || processedSamples <= 0)
            return;
        lastHop = analysisHop;
        const float dt = (float) std::min(0.025,
            (double)processedSamples / std::max(8000.0, sr));
        evidence = voiced ? inspect(detectorFrequencyHz) : Evidence{};
        const bool weakPeriod = evidence.available
            && evidence.periodScore < 0.22f && evidence.twoPeriodScore < 0.30f;
        const bool inconsistentSecondCycle = twoCycleAperiodicity >= 0.0f
            && twoCycleAperiodicity > 0.34f;
        const bool suspect = voiced && confidence < 0.90f
            && (weakPeriod || inconsistentSecondCycle);
        riskSeconds = std::clamp(riskSeconds + (suspect ? dt : -2.0f * dt),
                                 0.0f, 0.030f);
    }

    // Only adjusts transition confirmation, never the audio delay budget,
    // tuning ratio, voice range or sample count. Maximum additional note
    // confirmation is 10ms during repeated weak/aperiodic evidence.
    float guardedNoteConfirmMs(float baseMs, bool strictRealtime,
                               bool precisionHardTune, float retuneMs) const noexcept {
        if (!strictRealtime || precisionHardTune || retuneMs <= 0.5f
            || baseMs <= 0.0f || !std::isfinite(baseMs))
            return baseMs;
        const float extra = std::clamp((riskSeconds - 0.004f) * 500.0f, 0.0f, 10.0f);
        return std::clamp(baseMs + extra, 0.0f, 100.0f);
    }

    Evidence lastEvidence() const noexcept { return evidence; }
    float ambiguitySeconds() const noexcept { return riskSeconds; }

private:
    static float sanitize(float x) noexcept {
        return std::isfinite(x) ? std::clamp(x, -32.0f, 32.0f) : 0.0f;
    }

    float bestScore(int lag, int window, int radius) const noexcept {
        float best = -1.0f;
        for (int offset = -radius; offset <= radius; ++offset) {
            const int d = lag + offset;
            if (d <= 0) continue;
            double xy = 0.0, xx = 0.0, yy = 0.0;
            for (int k = 0; k < window; ++k) {
                const float x = history[(std::size_t)((write - 1 - (std::uint64_t)k) & mask)];
                const float y = history[(std::size_t)((write - 1 - (std::uint64_t)(k + d)) & mask)];
                xy += (double)x * y;
                xx += (double)x * x;
                yy += (double)y * y;
            }
            const double norm = xx + yy;
            if (norm > 1.0e-12)
                best = std::max(best, (float)std::clamp(2.0 * xy / norm, -1.0, 1.0));
        }
        return best;
    }

    static constexpr std::uint64_t mask = (1u << 15) - 1u;
    std::array<float, (1u << 15)> history {};
    double sr = 48000.0;
    std::uint64_t write = 0;
    std::array<double, 2> energy {};
    int activeChannel = 0, candidateSamples = 0;
    float riskSeconds = 0.0f;
    std::uint64_t lastHop = std::numeric_limits<std::uint64_t>::max();
    Evidence evidence {};
};

} // namespace autotune
