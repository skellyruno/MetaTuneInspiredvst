#include "FormantAccent.h"
#include <algorithm>
#include <cmath>

namespace lyra
{
static constexpr double kPi = 3.14159265358979323846;

static double raisedCos (double x) noexcept { return 0.5 - 0.5 * std::cos (kPi * std::clamp (x, 0.0, 1.0)); }

void FormantAccent::prepare (double sampleRate)
{
    sr = sampleRate;
    order = std::clamp (int (std::lround (16.0 * sr / 48000.0)), 16, kMaxOrder); // same envelope detail per Hz
    win = std::max (64, int (std::lround (kWindowSeconds * sr)));
    updateLen = std::max (16, int (std::lround (kUpdateSeconds * sr)));
    int size = 1;
    while (size < int (win * (1.0 + kMaxDepth)) + 8) size <<= 1;
    hist.assign (size_t (size), 0.0f);
    histMask = size - 1;
    frame.assign (size_t (win), 0.0);
    hann.resize (size_t (win));
    for (int j = 0; j < win; ++j) hann[size_t (j)] = 0.5 - 0.5 * std::cos (2.0 * kPi * (j + 0.5) / win);
    stride = std::max (1, int (std::lround (sr / 48000.0)));
    reset();
}

void FormantAccent::reset() noexcept
{
    std::fill (hist.begin(), hist.end(), 0.0f);
    histPos = 0;
    active = finishing = false;
    u = uFrom = uTarget = t = 0;
    untilUpdate = 0;
}

double FormantAccent::envelopeAt (double tt) const noexcept
{
    if (tt < kAttackSeconds)
        return uFrom + (uTarget - uFrom) * raisedCos (tt / kAttackSeconds);
    return uTarget * (1.0 - raisedCos ((tt - kAttackSeconds) / release));
}

void FormantAccent::windowed (double s) noexcept
{
    // Newest `win` samples read at rate s (s > 1 compresses time: spectrum and formants scale up by s),
    // 4-point Hermite interpolation, Hann window.
    if (s == 1.0) // integer positions: no interpolation
    {
        for (int j = 0; j < win; ++j)
            frame[size_t (j)] = hist[size_t ((histPos - win + j) & histMask)] * hann[size_t (j)];
        return;
    }
    const double newest = double (histPos - 1);
    for (int j = 0; j < win; ++j)
    {
        const double pos = newest - double (win - 1 - j) * s;
        const double fl = std::floor (pos), x = pos - fl;
        const int i = int (fl);
        const double xm1 = hist[size_t ((i - 1) & histMask)], x0 = hist[size_t (i & histMask)];
        const double x1 = hist[size_t ((i + 1) & histMask)], x2 = hist[size_t ((i + 2) & histMask)];
        const double c1 = 0.5 * (x1 - xm1), c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2, c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
        const double v = ((c3 * x + c2) * x + c1) * x + x0;
        frame[size_t (j)] = v * hann[size_t (j)];
    }
}

double FormantAccent::reflection (const double* x, int n, int p, double sampleRate, double* k, int stride) noexcept
{
    double r[kMaxOrder + 1] {}, a[kMaxOrder + 1] {}, tmp[kMaxOrder + 1] {};
    p = std::clamp (p, 1, kMaxOrder);
    for (int lag = 0; lag <= p; ++lag)
    {
        // Strided (same cost per update at any rate); four partial sums so the adds pipeline.
        double acc4[4] {};
        int j = n - 1, q = 0;
        for (; j >= lag; j -= stride, q = (q + 1) & 3) acc4[q] += x[j] * x[j - lag];
        const double acc = (acc4[0] + acc4[1]) + (acc4[2] + acc4[3]);
        // Gaussian lag window (~100 Hz): smooths the envelope so single harmonics are not modelled.
        const double w = 2.0 * kPi * 100.0 * lag / sampleRate;
        r[lag] = acc * std::exp (-0.5 * w * w);
    }
    std::fill (k, k + p, 0.0);
    if (! (r[0] > 1e-12) || ! std::isfinite (r[0]))
        return 0.0;
    r[0] *= 1.0001; // -40 dB white-noise floor: well-conditioned even on pure tones
    double e = r[0];
    for (int m = 1; m <= p; ++m)
    {
        double acc = r[m];
        for (int i = 1; i < m; ++i) acc += a[i] * r[m - i];
        const double km = std::clamp (-acc / e, -0.999, 0.999);
        for (int i = 1; i < m; ++i) tmp[i] = a[i] + km * a[m - i];
        for (int i = 1; i < m; ++i) a[i] = tmp[i];
        a[m] = km;
        k[m - 1] = km;
        e *= 1.0 - km * km;
    }
    return e;
}

void FormantAccent::update() noexcept
{
    // Ramp from the current coefficients to targets for the envelope value one update ahead.
    for (int m = 0; m < order; ++m) { ka0[m] = ka1[m]; ks0[m] = ks1[m]; }
    gain0 = gain1;
    const double uEnd = envelopeAt (t + double (updateLen) / sr);
    windowed (1.0);
    const double e = reflection (frame.data(), win, order, sr, ka1, stride);
    if (uEnd == 0.0)
    {
        std::copy (ka1, ka1 + order, ks1); // bit-identical: the cascade becomes an exact identity
        gain1 = 1.0;
        finishing = t >= kAttackSeconds;
    }
    else
    {
        windowed (1.0 + depth * uEnd);
        const double es = reflection (frame.data(), win, order, sr, ks1, stride);
        gain1 = e > 0 && es > 0 ? std::clamp (std::sqrt (es / e), 0.5, 2.0) : 1.0;
        finishing = false;
    }
    untilUpdate = updateLen;
}

void FormantAccent::trigger (int direction) noexcept
{
    if (! enabled || direction == 0)
        return;
    if (! active)
    {
        for (auto& ch : g) std::fill (std::begin (ch), std::end (ch), 0.0);
        for (auto& ch : h) std::fill (std::begin (ch), std::end (ch), 0.0);
        windowed (1.0);
        reflection (frame.data(), win, order, sr, ka1, stride);
        std::copy (ka1, ka1 + order, ks1);
        gain1 = 1.0;
        u = 0;
        active = true;
    }
    uFrom = u;
    uTarget = direction > 0 ? -1.0 : 1.0; // up -> formants down; down -> formants up
    release = length - kAttackSeconds;
    t = 0;
    untilUpdate = 0; // retarget now
    finishing = false;
}

void FormantAccent::process (float* const* ch, int numCh, int n) noexcept
{
    numCh = std::clamp (numCh, 0, kMaxChannels);
    double kA[kMaxOrder], kS[kMaxOrder];
    const double inv = 1.0 / double (std::max (1, numCh));
    for (int i = 0; i < n; ++i)
    {
        double mono = 0;
        for (int c = 0; c < numCh; ++c) mono += ch[c][i];
        hist[size_t (histPos)] = float (std::isfinite (mono) ? mono * inv : 0.0);
        histPos = (histPos + 1) & histMask;
        if (! active)
            continue;

        if (untilUpdate == 0)
        {
            if (finishing)
            {
                active = finishing = false; // coefficients now equal: output == input, pass through
                u = 0;
                continue;
            }
            update();
        }
        const double frac = 1.0 - double (--untilUpdate) / double (updateLen);
        for (int m = 0; m < order; ++m)
        {
            kA[m] = ka0[m] + (ka1[m] - ka0[m]) * frac;
            kS[m] = ks0[m] + (ks1[m] - ks0[m]) * frac;
        }
        const double gn = gain0 + (gain1 - gain0) * frac;
        t += 1.0 / sr;
        u = t < kAttackSeconds + release ? envelopeAt (t) : 0.0;

        for (int c = 0; c < numCh; ++c)
        {
            const double x = ch[c][i];
            if (! std::isfinite (x)) continue;
            // Analysis (FIR lattice): f_{m+1} = f_m + k_m g_m(n-1),  g_{m+1}(n) = g_m(n-1) + k_m f_m.
            double f = x, gin = x;
            double* gc = g[c];
            for (int m = 0; m < order; ++m)
            {
                const double old = gc[m];
                const double fNext = f + kA[m] * old;
                const double gNext = old + kA[m] * f;
                gc[m] = gin;
                gin = gNext;
                f = fNext;
            }
            // Synthesis (IIR lattice) with the shifted envelope: f_m = f_{m+1} - k_m h_m(n-1).
            double* hc = h[c];
            for (int m = order - 1; m >= 0; --m)
            {
                f -= kS[m] * hc[m];
                if (m + 1 < order) hc[m + 1] = hc[m] + kS[m] * f;
            }
            hc[0] = f;
            ch[c][i] = float (gn * f);
        }
    }
}
} // namespace lyra
