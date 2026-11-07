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
        energyAlpha = 1.0 - std::exp(-1.0 / (sr * 0.040));
        switchHoldSamples = std::max(1, (int)std::lround(sr * 0.012));
        correlationIntervalSamples = std::max(1, (int)std::lround(sr * 0.006));
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
        pendingHopSamples = 0;
        correlationAgeSamples = 0;
        evaluationCount = 0;
        lastEvidenceFrequencyHz = 0.0f;
        evidence = {};
    }

    // Read ORIGINAL input before the main tuner overwrites it. For stereo, a
    // persistent stronger-channel choice avoids destructive L/R cancellation.
    // The guard never feeds this auxiliary waveform into the actual renderer.
    void push(const float* const* channels, int channelCount, int count) noexcept {
        if (channels == nullptr || channelCount <= 0 || count <= 0 || channels[0] == nullptr)
            return;
        const bool stereo = channelCount >= 2 && channels[1] != nullptr;
        // Alpha, hysteresis and analysis cadence are precomputed at prepare().
        for (int i = 0; i < count; ++i) {
            const float l = sanitize(channels[0][i]);
            const float r = stereo ? sanitize(channels[1][i]) : l;
            energy[0] += energyAlpha * ((double)l * l - energy[0]);
            energy[1] += energyAlpha * ((double)r * r - energy[1]);
            if (!stereo) {
                activeChannel = 0;
                candidateSamples = 0;
            } else {
                const int alternate = 1 - activeChannel;
                if (energy[(std::size_t)alternate] > energy[(std::size_t)activeChannel] * 1.55) {
                    if (++candidateSamples >= switchHoldSamples) {
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
        // A bounded, decimated comparison avoids paying full YIN's cost again.
        // At 96k/70Hz we still compare several complete waveform cycles
        // across the one- and two-period candidate lags.
        const int window = std::clamp(period, 64, 384);
        if (period < 3 || 2 * period + window + 8 >= (int) history.size()
            || write < (std::uint64_t)(2 * period + window + 8))
            return out;
        const int radius = std::clamp(period / 100, 1, 2);
        out.periodScore = bestScore(period, window, radius);
        out.twoPeriodScore = bestScore(2 * period, window, 2 * radius);
        out.available = true;
        return out;
    }

    // Called once after the core processes a slice. Accumulate samples
    // even when YIN did not complete a new hop; otherwise 32-sample host
    // callbacks incorrectly run this time-based stabilizer much more slowly
    // than 1024-sample callbacks.
    //
    // The expensive correlation is only needed on a weak, otherwise
    // unexplained pitch candidate, at most once per six milliseconds in a
    // steady note. An already-known two-cycle failure needs no redundant
    // periodicity scan. Clean confident singing costs *zero* such scans.
    void observe(std::uint64_t analysisHop, float detectorFrequencyHz,
                 float confidence, float twoCycleAperiodicity,
                 bool voiced, int processedSamples) noexcept {
        if (processedSamples <= 0) return;
        const int maxCarry = std::max(1, (int)std::lround(sr * 0.1));
        pendingHopSamples = std::min(maxCarry,
                                    pendingHopSamples + std::min(processedSamples, maxCarry));
        if (analysisHop == lastHop) return;
        lastHop = analysisHop;
        const int elapsed = pendingHopSamples;
        pendingHopSamples = 0;
        const float dt = (float)((double)elapsed / sr);

        const bool validFrequency = std::isfinite(detectorFrequencyHz)
            && detectorFrequencyHz >= 50.0f && detectorFrequencyHz <= 1400.0f;
        const bool weak = voiced && validFrequency
            && std::isfinite(confidence) && confidence < 0.90f;
        const bool inconsistentSecondCycle = std::isfinite(twoCycleAperiodicity)
            && twoCycleAperiodicity >= 0.0f && twoCycleAperiodicity > 0.34f;

        if (weak && !inconsistentSecondCycle) {
            correlationAgeSamples = std::min(maxCarry,
                correlationAgeSamples + elapsed);
            const bool changedPitch = lastEvidenceFrequencyHz <= 0.0f
                || std::abs(std::log2(detectorFrequencyHz / lastEvidenceFrequencyHz)) > 0.06f;
            if (correlationAgeSamples >= correlationIntervalSamples
                || !evidence.available || changedPitch) {
                evidence = inspect(detectorFrequencyHz);
                ++evaluationCount;
                lastEvidenceFrequencyHz = detectorFrequencyHz;
                correlationAgeSamples = 0;
            }
        } else {
            // Never reuse stale periodicity for a new note, a newly voiced
            // phrase or a confident clean frame.
            evidence = {};
            correlationAgeSamples = 0;
            lastEvidenceFrequencyHz = 0.0f;
        }

        const bool weakPeriod = evidence.available
            && evidence.periodScore < 0.22f && evidence.twoPeriodScore < 0.30f;
        // A half-period candidate on an underlying fundamental produces
        // negative one-period correlation but strong two-period correlation.
        // Rejecting this as ambiguous helps prevent false octave-up locking.
        const bool octaveAlias = evidence.available
            && evidence.periodScore < 0.25f && evidence.twoPeriodScore > 0.78f;
        const bool suspect = weak
            && (weakPeriod || octaveAlias || inconsistentSecondCycle);
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
    std::uint64_t correlationEvaluations() const noexcept { return evaluationCount; }

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
            for (int k = 0; k < window; k += 2) {
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
    double energyAlpha = 0.0005;
    int switchHoldSamples = 576, correlationIntervalSamples = 288;
    int pendingHopSamples = 0, correlationAgeSamples = 0;
    std::uint64_t evaluationCount = 0;
    float lastEvidenceFrequencyHz = 0.0f;
    std::uint64_t write = 0;
    std::array<double, 2> energy {};
    int activeChannel = 0, candidateSamples = 0;
    float riskSeconds = 0.0f;
    std::uint64_t lastHop = std::numeric_limits<std::uint64_t>::max();
    Evidence evidence {};
};

} // namespace autotune
