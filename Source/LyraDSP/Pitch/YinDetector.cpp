#include "YinDetector.h"
#include <algorithm>
#include <cmath>

namespace lyra
{
void YinDetector::prepare (double sr, const Config& c)
{
    cfg = c;
    cfg.maxFreq = std::clamp (cfg.maxFreq, 100.0, 2000.0);
    cfg.minFreq = std::clamp (cfg.minFreq, 40.0, cfg.maxFreq * 0.5);

    decim = std::max (1, int (sr / 22050.0));
    detRate = sr / decim;
    floorFreq = cfg.minFreq;
    tauMin = std::max (2, int (detRate / cfg.maxFreq));
    hop = std::max (1, int (std::lround (cfg.hopSeconds * detRate)));
    octaveHoldHops = std::max (2, int (std::ceil (kOctaveHoldSeconds / (hop / detRate))));
    maxGapHops = std::max (1, int (std::ceil (0.010 / (hop / detRate))));
    configureLags();
    ringLen = frameLen; // capacity for the prepared floor; setMinFreq() only shrinks the frame
    silenceRms = std::pow (10.0f, cfg.silenceDb / 20.0f);

    hp.setHighpass (sr, 40.0, 0.7071);
    const double lpHz = std::min (4500.0, detRate * 0.4);
    lp1.setLowpass (sr, lpHz, 0.5412); // 4th-order Butterworth as two biquads
    lp2.setLowpass (sr, lpHz, 1.3066);

    ring.assign (size_t (ringLen), 0.0f);
    frame.assign (size_t (ringLen), 0.0f);
    cmndf.assign (size_t (tauMax + 2), 1.0f);
    reset();
}

void YinDetector::configureLags() noexcept
{
    tauMax = int (std::ceil (detRate / cfg.minFreq)) + 1;
    window = tauMax; // integration window = longest period: one full cycle of the lowest note
    frameLen = window + tauMax + 1;
}

void YinDetector::setMinFreq (double hz) noexcept
{
    hz = std::clamp (hz, floorFreq, cfg.maxFreq * 0.5);
    if (hz == cfg.minFreq)
        return;
    cfg.minFreq = hz;
    configureLags();
    std::fill (cmndf.begin(), cmndf.end(), 1.0f);
    prevTau = 0;
    octaveRun = 0;
}

void YinDetector::reset() noexcept
{
    std::fill (ring.begin(), ring.end(), 0.0f);
    hp.reset(); lp1.reset(); lp2.reset();
    decimPhase = writePos = filled = hopCounter = 0;
    last = {};
    prevTau = 0;
    octaveRun = 0;
    hopsSinceVoiced = 1000;
}

void YinDetector::analyse() noexcept
{
    // Unroll the newest frameLen samples into chronological order (cheap next to the O(W*tauMax) loop).
    for (int i = 0; i < frameLen; ++i)
        frame[size_t (i)] = ring[size_t ((writePos - frameLen + i + 2 * ringLen) % ringLen)];
    const float* x = frame.data();

    double energy = 0;
    for (int j = frameLen - window; j < frameLen; ++j)
        energy += double (x[j]) * x[j];
    const float rms = float (std::sqrt (energy / window));
    if (rms < silenceRms)
    {
        last = {};
        last.rms = rms;
        ++hopsSinceVoiced;
        return;
    }

    // Difference function + cumulative mean normalisation (YIN steps 2-3).
    // End-aligned: every lag compares the newest `window` samples with the ones tau earlier, so the
    // estimate describes the most recent audio (centre ~(window + tau)/2 back) whatever the lag.
    // A start-aligned frame would ignore the newest (tauMax - tau) samples and lag the shifter.
    const float* newest = x + (frameLen - window);
    cmndf[0] = 1.0f;
    double running = 0;
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        float d = 0;
        for (int j = 0; j < window; ++j)
        {
            const float diff = newest[j] - newest[j - tau];
            d += diff * diff;
        }
        running += d;
        cmndf[size_t (tau)] = running > 0 ? float (d * tau / running) : 1.0f;
    }

    // Absolute threshold: first dip below threshold, then slide to its local minimum (step 4).
    int best = -1;
    for (int tau = tauMin; tau < tauMax; ++tau)
    {
        if (cmndf[size_t (tau)] < cfg.threshold)
        {
            while (tau + 1 < tauMax && cmndf[size_t (tau + 1)] < cmndf[size_t (tau)])
                ++tau;
            best = tau;
            break;
        }
    }
    if (best < 0) // no dip below threshold: global minimum, voicing decided below
        best = int (std::min_element (cmndf.begin() + tauMin, cmndf.begin() + tauMax) - cmndf.begin());

    // Octave-up guard: a strong 2nd harmonic can make the half-period dip pass the threshold.
    // A genuine period leaves d(2*tau) close to d(tau); a half-period error leaves d(2*tau) far lower.
    if (const int t2 = best * 2; t2 + 1 < tauMax)
    {
        int m = t2;
        for (int t = t2 - 2; t <= t2 + 2; ++t)
            if (cmndf[size_t (t)] < cmndf[size_t (m)]) m = t;
        if (cmndf[size_t (best)] > 0.02f && cmndf[size_t (m)] < 0.6f * cmndf[size_t (best)])
            best = m;
    }

    // Continuity hold: an abrupt jump to a harmonic (period / 2..6, 2/3) or subharmonic (x2, x3) right
    // after voiced frames is treated as suspect. On real vocals these are the bulk of detector errors
    // (measured: 12/19/24/28-semitone excursions of 4-40 ms); each one used to flip the target note.
    bool held = false;
    if (prevTau > 0 && hopsSinceVoiced <= maxGapHops)
    {
        const double rel = best / prevTau;
        bool harmonic = std::abs (rel * 1.5 - 1.0) < 0.04 || std::abs (rel / 2.0 - 1.0) < 0.06 || std::abs (rel / 3.0 - 1.0) < 0.06;
        for (int k = 2; k <= 6; ++k)
            harmonic = harmonic || std::abs (rel * k - 1.0) < 0.04;
        if (harmonic)
        {
            if (++octaveRun < octaveHoldHops)
            {
                const int c = int (std::lround (prevTau)), r = std::max (2, c / 25); // +-4 %
                int m = -1;
                for (int t = std::max (tauMin, c - r); t <= std::min (tauMax - 1, c + r); ++t)
                    if (m < 0 || cmndf[size_t (t)] < cmndf[size_t (m)]) m = t;
                if (m >= 0 && cmndf[size_t (m)] < cfg.voicedThreshold)
                {
                    best = m;
                    held = true;
                }
            }
        }
    }
    if (! held)
        octaveRun = 0;

    // Parabolic interpolation around the dip (step 5) for sub-sample period precision.
    double tau = best;
    if (best > 1 && best + 1 <= tauMax)
    {
        const double a = cmndf[size_t (best - 1)], b = cmndf[size_t (best)], c = cmndf[size_t (best + 1)];
        const double denom = a - 2 * b + c;
        if (denom > 1e-12)
            tau += std::clamp (0.5 * (a - c) / denom, -0.5, 0.5);
    }

    const float dmin = cmndf[size_t (best)];
    last.rms = rms;
    last.confidence = std::clamp (1.0f - dmin, 0.0f, 1.0f);
    last.voiced = dmin < cfg.voicedThreshold;
    last.frequency = last.voiced ? float (detRate / tau) : 0.0f;
    if (last.voiced)
    {
        prevTau = tau;
        hopsSinceVoiced = 0;
    }
    else
    {
        ++hopsSinceVoiced;
    }
}
} // namespace lyra
