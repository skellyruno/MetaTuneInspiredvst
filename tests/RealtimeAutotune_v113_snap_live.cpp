// V1.13 Snap Live: SnapTune's delay-line + whole-cycle-splice renderer inside Skelly.
// Synthetic-signal regressions only; these are not real-singer quality scores.
#include "RealtimeAutotune.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>

// Allocation trap: nothing in the audio callback may touch the heap.
static std::atomic<bool> inCallback { false };
static std::atomic<unsigned> allocations { 0 };
void* operator new (std::size_t n)
{
    if (inCallback.load (std::memory_order_relaxed)) allocations.fetch_add (1, std::memory_order_relaxed);
    if (auto* p = std::malloc (n)) return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t n) { return ::operator new (n); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

using namespace autotune;
static constexpr double pi = 3.14159265358979323846;

static float voice (double ph)
{
    return (float) (.22 * std::sin (ph) + .11 * std::sin (2 * ph + .3) + .07 * std::sin (3 * ph - .7)
                    + .04 * std::sin (4 * ph + 1.1) + .02 * std::sin (5 * ph));
}

// Fraction of energy explained by the first `harmonics` partials of fOut, in dB.
static double harmonicSNR (const std::vector<float>& y, double sr, double fOut, int from, int to, int harmonics = 10)
{
    const int n = to - from;
    std::vector<double> a ((size_t) harmonics), b ((size_t) harmonics);
    for (int h = 1; h <= harmonics; ++h)
    {
        double c = 0, s = 0;
        for (int i = from; i < to; ++i)
        {
            const double ph = 2 * pi * fOut * h * i / sr;
            c += y[(size_t) i] * std::cos (ph);
            s += y[(size_t) i] * std::sin (ph);
        }
        a[(size_t) h - 1] = 2 * c / n;
        b[(size_t) h - 1] = 2 * s / n;
    }
    double explained = 0, residual = 0;
    for (int i = from; i < to; ++i)
    {
        double m = 0;
        for (int h = 1; h <= harmonics; ++h)
        {
            const double ph = 2 * pi * fOut * h * i / sr;
            m += a[(size_t) h - 1] * std::cos (ph) + b[(size_t) h - 1] * std::sin (ph);
        }
        explained += m * m;
        residual += (y[(size_t) i] - m) * (y[(size_t) i] - m);
    }
    return 10 * std::log10 (explained / (residual + 1e-20));
}

static bool fail (const char* what, double a = 0, double b = 0)
{
    std::printf ("FAIL: %s (%.4f, %.4f)\n", what, a, b);
    return false;
}

// 1. The shifter alone: pitch purity, click-free splices, bounded delay window.
static bool shifterAlone (double sr)
{
    bool ok = true;
    for (double f0 : { 110.0, 140.0, 220.0, 330.0, 440.0 })
        for (double semis : { -2.0, -0.5, 0.3, 1.0 })
        {
            const double r = std::exp2 (semis / 12.0);
            const float P = (float) (sr / f0);
            SnapLiveShifter s;
            s.prepare (sr);
            s.setRenderingEnabled (true);
            const int N = (int) (sr * 1.2);
            std::vector<float> y ((size_t) N);
            double ph = 0, maxInStep = 0, maxOutStep = 0, prevX = 0;
            for (int i = 0; i < N; ++i)
            {
                ph += 2 * pi * f0 / sr;
                const float x = voice (ph);
                s.setRatio ((float) r);
                s.setPeriod (P);
                y[(size_t) i] = s.processSample (x);
                if (i > sr * 0.3)
                {
                    maxInStep = std::max (maxInStep, std::abs ((double) x - prevX));
                    maxOutStep = std::max (maxOutStep, (double) std::abs (y[(size_t) i] - y[(size_t) i - 1]));
                }
                prevX = x;
            }
            const double snr = harmonicSNR (y, sr, f0 * r, (int) (sr * 0.4), N);
            if (snr < 35.0) ok = fail ("harmonic purity below 35 dB", f0, snr);
            // A splice with a bad length or a hard cut would show up as a step well above the signal's own.
            if (maxOutStep > 1.6 * std::max (1.0, r) * maxInStep) ok = fail ("click in output", maxOutStep, maxInStep);

            // Delay window: never closer than the interpolation guard, never beyond the splice window.
            const double minD = s.getMinObservedDelay(), maxD = s.getMaxObservedDelay();
            const double lo = std::max (10.0 + 2.0, (double) s.getNominalDelaySamples() - 0.625 * P);
            const double hi = lo + 1.25 * P + 0.006 * sr + 4.0;
            if (minD < 8.0) ok = fail ("delay fell inside interpolation guard", minD);
            if (maxD > hi) ok = fail ("delay left splice window", maxD, hi);
            if (s.getEqualPowerSpliceCount() != 0) ok = fail ("clean periodic input fell back to equal-power splice", s.getEqualPowerSpliceCount());
        }
    return ok;
}

// 2. Unvoiced audio returns to the nominal tap, and the aligned dry tap sits exactly there.
static bool recentring (double sr)
{
    SnapLiveShifter s;
    s.prepare (sr);
    s.setRenderingEnabled (true);
    double ph = 0;
    const double f0 = 140.0;
    const float P = (float) (sr / f0);
    // Pitch up for a while: the tap runs away from nominal.
    for (int i = 0; i < (int) (sr * 0.5); ++i)
    {
        ph += 2 * pi * f0 / sr;
        s.setRatio (1.12f);
        s.setPeriod (P);
        s.processSample (voice (ph));
    }
    const double during = s.getCurrentDelaySamples();
    // Brief detector dropout (5 ms) must NOT trigger an uncorrelated splice.
    const int splicesBefore = s.getSpliceCount();
    for (int i = 0; i < (int) (sr * 0.005); ++i)
    {
        ph += 2 * pi * f0 / sr;
        s.setRatio (1.0f);
        s.setPeriod (0.0f);
        s.processSample (voice (ph));
    }
    if (s.getEqualPowerSpliceCount() != 0) return fail ("5 ms dropout caused an equal-power splice");
    (void) splicesBefore;
    // Sustained noise-like unvoiced input: drift back to nominal.
    unsigned seed = 7;
    for (int i = 0; i < (int) (sr * 0.2); ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        const float n = ((float) (seed >> 8) / 8388608.0f - 1.0f) * 0.05f;
        s.setRatio (1.0f);
        s.setPeriod (0.0f);
        s.processSample (n);
    }
    const double settled = s.getCurrentDelaySamples();
    if (std::abs (settled - s.getNominalDelaySamples()) > 0.3 * sr / 200.0)
        return fail ("did not return to nominal delay", settled, s.getNominalDelaySamples());
    (void) during;
    return true;
}

// 3. Engine level: a flat/sharp held note ends up on the target note, the host latency is the
//    advertised Snap Live figure, and switching renderers while singing is bounded and finite.
static Settings liveSettings (bool snap)
{
    Settings s;
    s.strictRealtime = s.lowLatency = s.ultraLowLatency = true;
    s.snapLive = snap;
    s.retuneMs = 0.0f;
    s.follow = 0.85f;
    s.noteStabilizerMs = 0.0f;
    s.noteConfirmMs = 10.0f;
    s.voicingThreshold = 0.8f;
    return s;
}

static bool engineTarget (double sr)
{
    bool ok = true;
    for (double f0 : { 110.0 * std::exp2 (-0.30 / 12), 146.8 * std::exp2 (0.35 / 12), 220.0 * std::exp2 (-0.25 / 12), 329.6 * std::exp2 (0.30 / 12) })
    {
        RealtimeAutotune e;
        e.prepare (sr);
        e.setSettings (liveSettings (true));
        const int N = (int) (sr * 1.5);
        std::vector<float> y ((size_t) N);
        double ph = 0;
        for (int i = 0; i < N; ++i)
        {
            ph += 2 * pi * f0 / sr;
            y[(size_t) i] = e.process (voice (ph));
            if (! std::isfinite (y[(size_t) i])) return fail ("non-finite output");
        }
        const double target = std::round (69 + 12 * std::log2 (f0 / 440.0));
        const double fo = 440.0 * std::exp2 ((target - 69) / 12);
        const double onTarget = harmonicSNR (y, sr, fo, (int) (sr * 0.8), N);
        const double onInput = harmonicSNR (y, sr, f0, (int) (sr * 0.8), N);
        if (onTarget < 40.0) ok = fail ("engine output not locked on target note", f0, onTarget);
        if (onInput > onTarget - 25.0) ok = fail ("original pitch leaks into Snap Live output", onInput, onTarget);
    }
    return ok;
}

static bool hostLatencyFigure (double sr)
{
    const int snap = SnapLiveShifter::hostLatencySamples (sr);
    const int live = RealtimeAutotune::ultraLowLatencyHostSamples (sr);
    const int quality = RealtimeAutotune::realtimeQualityHostSamples (sr);
    if (! (live < snap && snap < quality)) return fail ("Snap Live latency not between Live and Quality", snap, live);
    if (std::abs ((double) snap / sr * 1000.0 - latency_limits::snapLiveMs) > 0.05) return fail ("Snap Live latency figure", snap);
    return true;
}

static bool modeSwitching (double sr)
{
    RealtimeAutotune e;
    e.prepare (sr);
    e.setSettings (liveSettings (false));
    const int N = (int) (sr * 2.4);
    double ph = 0, maxIn = 0, maxOut = 0, prevX = 0, prevY = 0;
    const double f0 = 196.0 * std::exp2 (0.3 / 12);
    for (int i = 0; i < N; ++i)
    {
        if (i % (int) (sr * 0.4) == 0)
            e.setSettings (liveSettings ((i / (int) (sr * 0.4)) % 2 == 0 ? false : true));
        ph += 2 * pi * f0 / sr;
        const float x = voice (ph);
        const float y = e.process (x);
        if (! std::isfinite (y) || std::abs (y) > 2.0f) return fail ("mode switch produced bad sample", y);
        if (i > sr * 0.3) { maxIn = std::max (maxIn, std::abs ((double) x - prevX)); maxOut = std::max (maxOut, std::abs ((double) y - prevY)); }
        prevX = x; prevY = y;
    }
    // Equal-power blend of two differently delayed copies can exceed the dry slope, but not wildly.
    if (maxOut > 3.0 * maxIn) return fail ("click on renderer switch", maxOut, maxIn);
    return true;
}

// 4. Linked stereo: identical content scaled differently must splice identically.
static bool linkedStereo (double sr)
{
    RealtimeAutotune e;
    e.prepare (sr);
    e.setSettings (liveSettings (true));
    constexpr int block = 64;
    std::array<float, block> l {}, r {};
    float* ch[] = { l.data(), r.data() };
    std::array<float, 2 * block> intrinsic {};
    double ph = 0, worst = 0;
    const double f0 = 123.47 * std::exp2 (-0.35 / 12.0); // 35 cents flat of B2: constant ~2% correction, frequent splices
    const int blocks = (int) (sr * 1.5 / block);
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < block; ++i)
        {
            ph += 2 * pi * f0 / sr;
            l[(size_t) i] = voice (ph);
            r[(size_t) i] = 0.5f * l[(size_t) i];
        }
        e.processChannels (ch, 2, block, intrinsic.data());
        if (b * block > sr * 0.5)
            for (int i = 0; i < block; ++i)
                worst = std::max (worst, (double) std::abs (r[(size_t) i] - 0.5f * l[(size_t) i]));
    }
    if (worst > 1.0e-4) return fail ("stereo channels spliced differently", worst);
    return true;
}

// 5. Garbage in, finite out; and no heap use in the callback while toggling renderers.
static bool robustAndRealtime (double sr)
{
    RealtimeAutotune e;
    e.prepare (sr);
    constexpr int block = 128;
    std::array<float, block> a {}, b {};
    float* ch[] = { a.data(), b.data() };
    std::array<float, 2 * block> intrinsic {};
    const unsigned before = allocations.load();
    for (int blk = 0; blk < 400; ++blk)
    {
        auto s = liveSettings ((blk / 25) % 2 == 0);
        s.retuneMs = (float) (blk % 60);
        s.bypass = (blk % 97) == 50;
        e.setSettings (s);
        for (int i = 0; i < block; ++i)
        {
            const double t = (blk * block + i) / sr;
            float x = (float) (.2 * std::sin (2 * pi * (140.0 + 90.0 * std::sin (2 * pi * 0.7 * t)) * t));
            if (blk == 120 && i == 5) x = std::numeric_limits<float>::quiet_NaN();
            if (blk == 121 && i == 9) x = std::numeric_limits<float>::infinity();
            if (blk == 122 && i == 3) x = 1.0e30f;
            a[(size_t) i] = x;
            b[(size_t) i] = -0.7f * x;
        }
        inCallback.store (true, std::memory_order_relaxed);
        e.processChannels (ch, 2, block, intrinsic.data());
        inCallback.store (false, std::memory_order_relaxed);
        for (int i = 0; i < block; ++i)
            if (! std::isfinite (a[(size_t) i]) || ! std::isfinite (b[(size_t) i]) || std::abs (a[(size_t) i]) > 64.0f)
                return fail ("non-finite or runaway output", a[(size_t) i], blk);
    }
    if (allocations.load() != before) return fail ("heap allocation inside audio callback", allocations.load() - before);
    return true;
}

int main()
{
    bool ok = true;
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        std::printf ("sample rate %.0f\n", sr);
        ok = shifterAlone (sr) && ok;
        ok = recentring (sr) && ok;
        ok = hostLatencyFigure (sr) && ok;
    }
    for (double sr : { 44100.0, 48000.0 })
    {
        ok = engineTarget (sr) && ok;
        ok = modeSwitching (sr) && ok;
        ok = linkedStereo (sr) && ok;
        ok = robustAndRealtime (sr) && ok;
    }
    std::printf (ok ? "PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
