// SnapLiveShifter.h - header-only, dependency-free (C++17).
//
// "Snap Live": SnapTune's Live renderer ported into Skelly MetaTune.
//
// Method (the classic Auto-Tune live path): the input is written into a ring and read through a
// fractional-delay tap that advances at the pitch-correction ratio. Reading faster than the input
// arrives (pitch up) shrinks the delay, reading slower (pitch down) grows it. Whenever the delay
// reaches the edge of its window the tap jumps one source period back (repeating a cycle) or
// forward (skipping one). A jump of exactly one period of a periodic signal is seamless; the jump
// length is located by normalised cross-correlation and the two read points are crossfaded.
//
// Differences from the SnapTune original (all causal, allocation-free in the audio callback):
//  1. Centred delay window. SnapTune lets the delay run from ~0 to 1.25 periods. Here the window
//     is centred on the reported latency, so the delay wanders about +-0.6 period around nominal
//     instead of sitting at one extreme, and the worst-case age of a wet sample is lower.
//  2. Correlation-gated crossfades. SnapTune always used an amplitude-complementary fade, which
//     dips when the two read points do not actually match. The match score now chooses between a
//     coherent (raised-cosine) and an equal-power fade, and a poor match is never trusted with a
//     long fade at the wrong length.
//  3. Coarse-to-fine splice search on a band-limited analysis copy: ~4x fewer multiply-adds than a
//     full-rate scan, so a splice no longer costs a CPU spike inside a single audio sample.
//  4. Linked stereo splices. One channel (the pitch-analysis channel) decides every splice; the
//     other applies the same event, so two channels never jump by different lengths.
//  5. Unvoiced re-centring waits for sustained unvoiced audio (default 15 ms) so a brief detector
//     dropout inside a vowel cannot trigger an uncorrelated splice.
//  6. The dry/aligned tap used for Amount = 0 sits at the nominal tap, which is exactly the delay
//     the wet path has when the signal is unvoiced or the ratio is unity and recentred.
//
// Latency contract: hostLatencySamples() is the figure to report to the host and is also this
// class's nominal tap delay (unvoiced audio, in-tune notes after a re-centre, and the aligned dry
// tap all sit exactly there). The instantaneous delay of *corrected voiced* audio moves inside the
// splice window - nominal +- ~0.6 source period, never below ~0.2 ms - and is NOT a fixed number.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include "RealtimeLatencyLimits.h"

namespace autotune
{
class SnapLiveShifter
{
public:
    // What a splice looks like, published by the leader channel so a follower can replay it.
    struct SpliceEvent
    {
        bool valid = false;
        double offset = 0.0;     // tap displacement in samples (negative = back in time)
        int fadeLength = 0;
        bool equalPower = false; // uncorrelated or poorly matched material
    };

    static int hostLatencySamples (double sr) noexcept
    {
        return (int) std::ceil (std::max (8000.0, sr) * (latency_limits::snapLiveMs * 0.001));
    }

    void prepare (double sampleRate)
    {
        sr = std::max (8000.0, sampleRate);
        latencyHost = hostLatencySamples (sr);
        nominalDelay = std::max (kMinDelay + 4, latencyHost);
        minDelay = (double) std::max (kMinDelay, (int) std::lround (sr * 0.0002));
        minPeriod = (float) (sr / 1500.0);
        maxPeriod = (float) (sr / 55.0);
        unvoicedPeriod = sr / 200.0;
        unvoicedHold = (int) std::lround (sr * 0.015);
        searchDecimation = sr > 64000.0 ? 4 : 2;
        maxWindow = std::max (64, (int) std::lround (sr * 0.0107));

        // History needed: tap delay (<= ~1.5 periods of the lowest pitch) + correlation window
        // + the longest splice length + interpolation margin. 70 ms covers 55 Hz at any rate.
        size_t capacity = 1;
        while (capacity < (size_t) std::ceil (sr * 0.070)) capacity <<= 1;
        ring.assign (capacity, 0.0f);
        analysis.assign (capacity, 0.0f);
        mask = (int64_t) capacity - 1;

        const double twoPi = 6.283185307179586;
        hpCoeff = (float) std::exp (-twoPi * 50.0 / sr);
        lpCoeff = (float) std::exp (-twoPi * 2000.0 / sr);
        switchFadeLength = std::max (1, (int) std::lround (sr * 0.008));
        reset();
    }

    void reset() noexcept
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        std::fill (analysis.begin(), analysis.end(), 0.0f);
        write = 0;
        hpIn = hpOut = lpState = 0.0f;
        ratio = 1.0f;
        period = 0.0f;
        unvoicedRun = 0;
        liveTap = (double) (write - nominalDelay);
        spliceTap = liveTap;
        fadeLength = fadePosition = 0;
        fadeEqualPower = false;
        pendingEvent = {};
        lastAlignedDry = 0.0f;
        splicesStarted = splicesCoherent = splicesEqualPower = 0;
        lastScore = 0.0f;
        maxDelayObserved = 0.0;
        minDelayObserved = 1.0e30;
        rendering = false;
        renderFade = 0;
    }

    void setRatio (float newRatio) noexcept
    {
        ratio = std::clamp (std::isfinite (newRatio) ? newRatio : 1.0f, 0.5f, 2.0f);
    }

    // The same detector period used by the correction controller; 0 = unvoiced.
    void setPeriod (float samples) noexcept
    {
        period = std::isfinite (samples) && samples >= minPeriod && samples <= maxPeriod ? samples : 0.0f;
    }

    // While not rendering, the shifter only keeps its history warm (ring + analysis filters) and
    // pins the tap at the nominal delay, so switching it on is click-free and costs nothing before.
    void setRenderingEnabled (bool enabled) noexcept
    {
        if (enabled == rendering) return;
        rendering = enabled;
        if (! enabled)
        {
            liveTap = (double) (write - 1) - (double) nominalDelay;
            fadeLength = fadePosition = 0;
            pendingEvent = {};
        }
    }
    bool isRenderingEnabled() const noexcept { return rendering; }

    // Leader/follower stereo linking.
    void setFollower (bool isFollower) noexcept { follower = isFollower; }
    const SpliceEvent& getLastEvent() const noexcept { return pendingEvent; }
    void applyEvent (const SpliceEvent& event) noexcept
    {
        if (! event.valid || ! rendering) return;
        startSplice (event.offset, event.fadeLength, event.equalPower);
    }

    int getMaximumDelaySamples() const noexcept { return latencyHost; }
    int getNominalDelaySamples() const noexcept { return nominalDelay; }
    float getLastAlignedDry() const noexcept { return lastAlignedDry; }
    double getCurrentDelaySamples() const noexcept { return (double) (write - 1) - liveTap; }
    bool isSplicing() const noexcept { return fadeLength > 0; }
    int getSpliceCount() const noexcept { return splicesStarted; }
    int getCoherentSpliceCount() const noexcept { return splicesCoherent; }
    int getEqualPowerSpliceCount() const noexcept { return splicesEqualPower; }
    float getLastSpliceScore() const noexcept { return lastScore; }
    double getMaxObservedDelay() const noexcept { return maxDelayObserved; }
    double getMinObservedDelay() const noexcept { return minDelayObserved; }

    float processSample (float input) noexcept
    {
        input = std::clamp (std::isfinite (input) ? input : 0.0f, -32.0f, 32.0f);
        const size_t slot = (size_t) (write & mask);
        ring[slot] = input;

        // Analysis copy: 50 Hz high-pass (DC/rumble) then 2 kHz low-pass (periodicity lives in the
        // low partials; sibilance and noise only blur the splice search).
        const float hp = hpCoeff * (hpOut + input - hpIn);
        hpIn = input;
        hpOut = hp;
        lpState = hp + lpCoeff * (lpState - hp);
        analysis[slot] = lpState;
        ++write;

        const int64_t newest = write - 1;
        lastAlignedDry = readRing (ring, (double) (newest - nominalDelay));

        if (! rendering)
        {
            liveTap = (double) newest - (double) nominalDelay;
            return lastAlignedDry;
        }

        float wet = renderSample (newest);

        // Fade in from the aligned dry tap when rendering was just enabled.
        if (renderFade < switchFadeLength)
        {
            const float g = (float) ++renderFade / (float) switchFadeLength;
            wet = lastAlignedDry + g * (wet - lastAlignedDry);
        }

        const double delay = (double) newest - liveTap;
        maxDelayObserved = std::max (maxDelayObserved, delay);
        minDelayObserved = std::min (minDelayObserved, delay);
        return wet;
    }

private:
    static constexpr int kMinDelay = 8;

    // 4-point Catmull-Rom read; all taps are at least minDelay samples old.
    static float readRing (const std::vector<float>& r, double position) noexcept
    {
        const auto mask = (int64_t) r.size() - 1;
        const auto i = (int64_t) std::floor (position);
        const float t = (float) (position - (double) i);
        const float y0 = r[(size_t) ((i - 1) & mask)], y1 = r[(size_t) (i & mask)];
        const float y2 = r[(size_t) ((i + 1) & mask)], y3 = r[(size_t) ((i + 2) & mask)];
        const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
        const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c = -0.5f * y0 + 0.5f * y2;
        return ((a * t + b) * t + c) * t + y1;
    }

    float renderSample (int64_t newest) noexcept
    {
        const double r = (double) ratio;
        pendingEvent.valid = false;

        if (fadeLength == 0 && ! follower)
            decideSplice ((double) newest, r);

        float wet;

        if (fadeLength > 0)
        {
            const double t = ((double) fadePosition + 0.5) / (double) fadeLength;
            double fadeIn, fadeOut;

            if (fadeEqualPower)
            {
                fadeIn = std::sin (1.5707963267948966 * t);
                fadeOut = std::cos (1.5707963267948966 * t);
            }
            else
            {
                fadeIn = 0.5 - 0.5 * std::cos (3.141592653589793 * t);
                fadeOut = 1.0 - fadeIn;
            }

            wet = (float) (fadeOut * (double) readRing (ring, liveTap) + fadeIn * (double) readRing (ring, spliceTap));
            spliceTap += r;

            if (++fadePosition >= fadeLength)
            {
                liveTap = spliceTap;
                fadeLength = fadePosition = 0;
            }
            else
            {
                liveTap += r;
            }
        }
        else
        {
            wet = readRing (ring, liveTap);
            liveTap += r;
        }

        // Safety net: the tap may never run into the newest samples or fall off the ring.
        const double newestD = (double) newest;
        if (liveTap > newestD - 3.0 || spliceTap > newestD - 3.0)
        {
            liveTap = std::min (liveTap, newestD - minDelay);
            spliceTap = std::min (spliceTap, newestD - minDelay);
        }
        const double oldest = newestD - (double) (mask - 8);
        if (liveTap < oldest) liveTap = oldest;
        if (spliceTap < oldest) spliceTap = oldest;

        return wet;
    }

    void decideSplice (double newest, double r) noexcept
    {
        const double delay = newest - liveTap;

        if (period > 0.0f)
        {
            unvoicedRun = 0;
            const double P = (double) period;

            // Window of allowed delays, centred on the nominal tap. Its width (1.25 P) leaves room
            // for a one-period jump plus the drift that accumulates during the crossfade.
            const double width = 1.25 * P;
            double lo = std::max (minDelay + 2.0, (double) nominalDelay - 0.5 * width);
            double hi = lo + width;
            const double fade = fadeFor (P);

            if (r > 1.0 && delay < lo + (r - 1.0) * fade + 1.0)
                spliceBack (P, fade, delay);
            else if (r < 1.0 && delay > hi - (1.0 - r) * fade - 1.0)
                spliceForward (P, fade, delay, lo);
            return;
        }

        // Unvoiced: drift back to the nominal tap so host alignment holds, but only once the
        // audio has really been unvoiced for a while (a vowel with a detector dropout is not it).
        if (++unvoicedRun < unvoicedHold) return;
        unvoicedRun = unvoicedHold;

        if (std::abs (delay - (double) nominalDelay) > 0.25 * unvoicedPeriod)
            startSplice ((delay - (double) nominalDelay), (int) std::lround (sr * 0.005), true);
    }

    double fadeFor (double P) const noexcept
    {
        return std::clamp (0.5 * P, 16.0, 0.006 * sr);
    }

    void spliceBack (double P, double fade, double delay) noexcept
    {
        // Jump back: repeat one cycle. Needs history: delay + length + correlation window.
        const double maxLen = std::min (1.25 * P, (double) mask - delay - (double) maxWindow - 16.0);
        if (maxLen < 0.8 * P) return;
        double score = 0.0;
        const double length = findSpliceLength (liveTap, -1, 0.8 * P, maxLen, P, score);
        commitSplice (-length, fade, score);
    }

    void spliceForward (double P, double fade, double delay, double lo) noexcept
    {
        // Jump forward: skip one cycle. The new tap must stay inside the window and in the past.
        const double maxLen = std::min (1.25 * P, delay - lo - 2.0);
        if (maxLen < 0.8 * P)
        {
            // Not enough room for a full cycle (period larger than the window allows): fall back
            // to the longest jump that keeps the tap legal, uncorrelated.
            if (delay - minDelay - 2.0 > 0.25 * P)
                startSplice (std::min (delay - minDelay - 2.0, P), (int) std::lround (fade), true);
            return;
        }
        double score = 0.0;
        const double length = findSpliceLength (liveTap, 1, 0.8 * P, maxLen, P, score);
        commitSplice (length, fade, score);
    }

    void commitSplice (double offset, double fade, double score) noexcept
    {
        lastScore = (float) score;
        // A good match gets a coherent fade; a middling one an equal-power fade so the sum does
        // not dip; a bad one a longer equal-power fade (no point trusting the guessed length).
        if (score >= 0.80)
            startSplice (offset, (int) std::lround (fade), false);
        else if (score >= 0.45)
            startSplice (offset, (int) std::lround (std::max (fade, 0.003 * sr)), true);
        else
            startSplice (offset, (int) std::lround (std::max (fade, 0.005 * sr)), true);
    }

    void startSplice (double offset, int fadeSamples, bool equalPower) noexcept
    {
        spliceTap = liveTap + offset;
        fadeLength = std::max (1, fadeSamples);
        fadePosition = 0;
        fadeEqualPower = equalPower;
        ++splicesStarted;
        if (equalPower) ++splicesEqualPower; else ++splicesCoherent;

        pendingEvent.valid = true;
        pendingEvent.offset = offset;
        pendingEvent.fadeLength = fadeLength;
        pendingEvent.equalPower = equalPower;
    }

    // Normalised cross-correlation between the window ending at `tap` and the same window shifted
    // by `length` in `direction`. Coarse pass on every Nth lag/sample, then a full-rate refine.
    double findSpliceLength (double tap, int direction, double minLength, double maxLength,
                             double P, double& bestScoreOut) const noexcept
    {
        const int window = std::clamp ((int) std::lround (P), 32, maxWindow);
        const auto base = (int64_t) std::floor (tap);
        const int lo = (int) std::ceil (minLength);
        const int hi = (int) std::floor (maxLength);
        bestScoreOut = 0.0;

        if (hi <= lo)
            return 0.5 * (minLength + maxLength);

        const int step = searchDecimation;

        auto similarity = [&] (int length, int stride) noexcept
        {
            double cross = 0.0, energyA = 0.0, energyB = 0.0;
            const int64_t other = base + (int64_t) direction * length;

            for (int i = 0; i < window; i += stride)
            {
                const double a = (double) analysis[(size_t) ((base - i) & mask)];
                const double b = (double) analysis[(size_t) ((other - i) & mask)];
                cross += a * b;
                energyA += a * a;
                energyB += b * b;
            }
            return cross / std::sqrt (energyA * energyB + 1.0e-20);
        };

        int best = lo;
        double bestScore = -2.0;

        for (int length = lo; length <= hi; length += step)
        {
            const double s = similarity (length, step);
            if (s > bestScore) { bestScore = s; best = length; }
        }

        if (step > 1)
        {
            // Full-rate refinement around the coarse winner.
            const int from = std::max (lo, best - step), to = std::min (hi, best + step);
            bestScore = -2.0;
            for (int length = from; length <= to; ++length)
            {
                const double s = similarity (length, 1);
                if (s > bestScore) { bestScore = s; best = length; }
            }
        }
        else
        {
            bestScore = similarity (best, 1);
        }

        bestScoreOut = std::clamp (bestScore, -1.0, 1.0);

        // Sub-sample refinement.
        if (best > lo && best < hi)
        {
            const double a = similarity (best - 1, 1), b = similarity (best, 1), c = similarity (best + 1, 1);
            const double curvature = a - 2.0 * b + c;
            if (curvature < 0.0)
                return (double) best + std::clamp (0.5 * (a - c) / curvature, -0.5, 0.5);
        }
        return (double) best;
    }

    std::vector<float> ring, analysis;
    int64_t mask = 0, write = 0;
    double sr = 48000.0;
    int latencyHost = 0, nominalDelay = 0, searchDecimation = 2, maxWindow = 512;
    int unvoicedHold = 720, switchFadeLength = 384, renderFade = 0;
    double minDelay = 10.0, unvoicedPeriod = 240.0;
    float minPeriod = 32.0f, maxPeriod = 872.0f;

    float hpCoeff = 0.0f, lpCoeff = 0.0f, hpIn = 0.0f, hpOut = 0.0f, lpState = 0.0f;
    float ratio = 1.0f, period = 0.0f, lastAlignedDry = 0.0f;
    int unvoicedRun = 0;
    bool rendering = false, follower = false;

    double liveTap = 0.0, spliceTap = 0.0;
    int fadeLength = 0, fadePosition = 0;
    bool fadeEqualPower = false;
    SpliceEvent pendingEvent;

    int splicesStarted = 0, splicesCoherent = 0, splicesEqualPower = 0;
    float lastScore = 0.0f;
    double maxDelayObserved = 0.0, minDelayObserved = 1.0e30;
};
} // namespace autotune
