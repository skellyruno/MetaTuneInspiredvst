#include "PitchShifter.h"
#include <algorithm>
#include <cmath>

namespace lyra
{
static constexpr double kFadeSeconds = 0.004;
static constexpr double kTaps = 2.0;           // Hermite reads 1 sample ahead and 2 behind the fractional position
static constexpr double kSemitone = 1.0594630943592953;

static int fadeSamples (double sr) noexcept { return std::max (1, int (std::lround (kFadeSeconds * sr))); }

int PitchShifter::latencyFor (double sr, double minFreq) noexcept
{
    const double margin = kTaps + fadeSamples (sr) * (kSemitone - 1.0);
    return int (std::ceil (margin + 0.5 * sr / minFreq));
}

void PitchShifter::prepare (double sampleRate, int numChannels, double minFreq)
{
    sr = sampleRate;
    floorFreq = minFreq;
    fadeLen = fadeSamples (sr);
    // Worst-case delay: centre + P/2, plus old-head drift during a fade when shifting down.
    const double maxDelay = latencyFor (sr, minFreq) + 0.5 * sr / minFreq + fadeLen * (1.0 - 1.0 / kMaxRatio);
    int size = 1;
    while (size < int (maxDelay) + 16) size <<= 1;
    mask = size - 1;
    rings.assign (size_t (std::max (1, numChannels)), std::vector<float> (size_t (size), 0.0f));
    maxPeriod = sr / minFreq;
    centre = latencyFor (sr, minFreq);
    reset();
}

void PitchShifter::setMinFreq (double hz) noexcept
{
    hz = std::max (hz, floorFreq);
    maxPeriod = sr / hz;
    centre = latencyFor (sr, hz);
    unvoicedPeriod = std::min (maxPeriod, 0.010 * sr);
    period = std::min (period, maxPeriod);
}

void PitchShifter::reset() noexcept
{
    for (auto& r : rings) std::fill (r.begin(), r.end(), 0.0f);
    writePos = 0;
    unvoicedPeriod = std::min (maxPeriod, 0.010 * sr);
    d = dOld = centre;
    period = unvoicedPeriod;
    fading = false;
    fadePos = 0;
}

void PitchShifter::setPeriod (double samples) noexcept
{
    period = (samples > 1.0 && std::isfinite (samples)) ? std::min (samples, maxPeriod) : unvoicedPeriod;
}

float PitchShifter::read (int ch, double delay) const noexcept
{
    // 4-point Hermite interpolation at (writePos - delay); delay >= 2 keeps every tap already written.
    const double pos = double (writePos) - delay;
    const double fl = std::floor (pos);
    const float t = float (pos - fl);
    const int i = int (fl);
    const auto& r = rings[size_t (ch)];
    const float xm1 = r[size_t ((i - 1) & mask)], x0 = r[size_t (i & mask)];
    const float x1 = r[size_t ((i + 1) & mask)], x2 = r[size_t ((i + 2) & mask)];
    const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

void PitchShifter::process (float* const* ch, int numCh, int n, const float* ratio) noexcept
{
    numCh = std::min (numCh, int (rings.size()));
    const double dMax = double (mask) - 3.0;
    for (int i = 0; i < n; ++i)
    {
        for (int c = 0; c < numCh; ++c)
            rings[size_t (c)][size_t (writePos)] = ch[c][i];

        const float g = fading ? float (fadePos) / float (curFade) : 1.0f; // linear: spliced heads are in phase
        for (int c = 0; c < numCh; ++c)
        {
            float y = read (c, d);
            if (fading)
                y = g * y + (1.0f - g) * read (c, dOld);
            ch[c][i] = y;
        }

        const double r = std::clamp (double (ratio[i]), 1.0 / kMaxRatio, kMaxRatio);
        d = std::clamp (d + 1.0 - r, kTaps, dMax);
        if (fading)
        {
            dOld = std::clamp (dOld + 1.0 - r, kTaps, dMax);
            fading = ++fadePos < curFade;
        }
        else if (std::abs (d - centre) > 0.5 * period)
        {
            // Whole-period jump back toward the centre; lands within +-P/2 of it.
            double k = std::round ((centre - d) / period);
            if (k == 0) k = d < centre ? 1 : -1;
            dOld = d;
            d = std::clamp (d + k * period, kTaps, dMax);
            // Shifting up, the old head keeps shrinking toward the write head: cap the fade so it stays valid.
            curFade = r > 1.0 ? std::clamp (int ((dOld - kTaps) / (r - 1.0)), 1, fadeLen) : fadeLen;
            fading = true;
            fadePos = 0;
        }
        writePos = (writePos + 1) & mask;
    }
}
} // namespace lyra
