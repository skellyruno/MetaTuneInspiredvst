#include "TestHarness.h"
#include "Pitch/PitchMath.h"
#include "Pitch/Scale.h"
#include "Pitch/TargetSelector.h"
#include "Pitch/YinDetector.h"
#include "Pitch/PitchShifter.h"
#include "Pitch/CorrectionEngine.h"
#include "Common/FixedDelay.h"
#include "TunerChain.h"
#include "Doubler.h"
#include <atomic>
#include <new>

#include <algorithm>
#include <chrono>
#include <limits>
#include <random>

using namespace lyra;

// Global allocation counter: lets tests assert the real-time path never allocates.
// (malloc/free pairs are correct here; GCC's mismatched-new-delete check cannot see that.)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
static std::atomic<long> gAllocs { 0 };
void* operator new (size_t n) { ++gAllocs; if (void* p = std::malloc (n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[] (size_t n) { ++gAllocs; if (void* p = std::malloc (n ? n : 1)) return p; throw std::bad_alloc(); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, size_t) noexcept { std::free (p); }
void operator delete[] (void* p, size_t) noexcept { std::free (p); }
constexpr double kPi = 3.14159265358979323846;

// ---------- pitch math ----------
TEST (freq_midi_roundtrip)
{
    CHECK_NEAR (freqToMidi (440.0), 69.0, 1e-12);
    CHECK_NEAR (freqToMidi (261.6255653), 60.0, 1e-6);
    CHECK_NEAR (freqToMidi (880.0), 81.0, 1e-12);
    CHECK_NEAR (midiToFreq (57.0), 220.0, 1e-9);
    for (double m = 20; m < 110; m += 0.37)
        CHECK_NEAR (freqToMidi (midiToFreq (m)), m, 1e-9);
}

TEST (reference_frequency)
{
    CHECK_NEAR (freqToMidi (432.0, 432.0), 69.0, 1e-12);
    CHECK_NEAR (freqToMidi (440.0, 432.0), 69.0 + 12.0 * std::log2 (440.0 / 432.0), 1e-12);
    CHECK_NEAR (midiToFreq (69.0, 442.0), 442.0, 1e-9);
}

TEST (cents_error)
{
    CHECK_NEAR (centsError (60.25, 60.0), -25.0, 1e-9);
    CHECK_NEAR (centsError (59.9, 60.0), 10.0, 1e-9);
    CHECK_NEAR (centsError (freqToMidi (445.0), 69.0), -1200.0 * std::log2 (445.0 / 440.0), 1e-9);
}

// ---------- scale masks ----------
TEST (scale_masks)
{
    CHECK (scaleMask (ScaleType::Chromatic, 5) == kAllNotes);
    const PitchClassMask cMaj = scaleMask (ScaleType::Major, 0);
    for (int pc : { 0, 2, 4, 5, 7, 9, 11 }) CHECK (isAllowed (cMaj, pc));
    for (int pc : { 1, 3, 6, 8, 10 })       CHECK (! isAllowed (cMaj, pc));
    // A minor == C major (relative keys); G major contains F#, not F.
    CHECK (scaleMask (ScaleType::Minor, 9) == cMaj);
    const PitchClassMask gMaj = scaleMask (ScaleType::Major, 7);
    CHECK (isAllowed (gMaj, 6) && ! isAllowed (gMaj, 5));
    CHECK (transposeMask (cMaj, 12) == cMaj && transposeMask (cMaj, -5) == transposeMask (cMaj, 7));
    CHECK (scaleMask (ScaleType::Custom, 3, 0xF000 | 0x0001) == 0x0001); // stray high bits dropped
}

// ---------- target selection ----------
TEST (nearest_allowed)
{
    const PitchClassMask cMaj = scaleMask (ScaleType::Major, 0);
    CHECK (TargetSelector::nearestAllowed (60.3, kAllNotes) == 60);
    CHECK (TargetSelector::nearestAllowed (60.7, kAllNotes) == 61);
    CHECK (TargetSelector::nearestAllowed (60.9, cMaj) == 60); // flat C# -> C
    CHECK (TargetSelector::nearestAllowed (61.2, cMaj) == 62); // sharp C# -> D
    CHECK (TargetSelector::nearestAllowed (61.0, cMaj) == 60); // exact tie -> lower
    CHECK (TargetSelector::nearestAllowed (66.0, PitchClassMask (1u << 0)) == 60); // only C: tritone tie -> lower
    CHECK (TargetSelector::nearestAllowed (66.4, PitchClassMask (1u << 0)) == 72);
    CHECK (TargetSelector::nearestAllowed (60.0, 0) == TargetSelector::kNoTarget);
    CHECK (TargetSelector::nearestAllowed (std::numeric_limits<double>::quiet_NaN(), kAllNotes) == TargetSelector::kNoTarget);
}

TEST (hysteresis)
{
    TargetSelector s;
    s.setHysteresisCents (15);
    CHECK (s.select (60.40) == 60);
    CHECK (s.select (60.55) == 60); // 5 c past the midpoint: inside the 15 c margin
    CHECK (s.select (60.64) == 60); // 14 c past
    CHECK (s.select (60.70) == 61); // 20 c past -> switch
    CHECK (s.select (60.40) == 61); // sticks to 61 on the way back
    CHECK (s.select (60.30) == 60);
    // Dithering around the boundary must not flip every estimate.
    int flips = 0, prev = s.select (60.5);
    for (int i = 0; i < 100; ++i) { const int t = s.select (60.5 + ((i & 1) ? 0.05 : -0.05)); flips += t != prev; prev = t; }
    CHECK (flips == 0);
}

TEST (mask_change_and_empty_mask)
{
    TargetSelector s;
    CHECK (s.select (60.9) == 61);
    s.setMask (scaleMask (ScaleType::Major, 0)); // C# no longer allowed: must leave it immediately
    CHECK (s.select (60.9) == 60);
    s.setMask (0);
    CHECK (s.select (60.9) == TargetSelector::kNoTarget);
    s.setMask (kAllNotes);
    CHECK (s.select (60.9) == 61);
}

// ---------- YIN ----------
struct Run { std::vector<PitchEstimate> est; };

template <typename Gen>
static Run runDetector (double sr, double seconds, Gen&& gen, int block = 128, YinDetector::Config cfg = {})
{
    YinDetector d;
    d.prepare (sr, cfg);
    std::vector<float> buf (static_cast<size_t> (block));
    Run r;
    const int total = int (sr * seconds);
    for (int n = 0; n < total; n += block)
    {
        const int len = std::min (block, total - n);
        for (int i = 0; i < len; ++i) buf[size_t (i)] = gen (n + i);
        d.process (buf.data(), len, [&] (int, const PitchEstimate& e) { r.est.push_back (e); });
    }
    return r;
}

// Cents error statistics over voiced estimates after a warm-up.
static void pitchStats (const Run& r, double trueHz, double& maxAbsCents, double& voicedFrac, size_t skip)
{
    maxAbsCents = 0;
    size_t voiced = 0, n = 0;
    for (size_t i = skip; i < r.est.size(); ++i, ++n)
        if (r.est[i].voiced)
        {
            ++voiced;
            maxAbsCents = std::max (maxAbsCents, std::abs (1200.0 * std::log2 (r.est[i].frequency / trueHz)));
        }
    voicedFrac = n ? double (voiced) / double (n) : 0;
}

TEST (yin_sine_accuracy_across_rates)
{
    for (double sr : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
        for (double f : { 80.0, 110.0, 196.0, 261.63, 440.0, 659.25, 880.0 })
        {
            auto r = runDetector (sr, 0.4, [&] (int n) { return float (0.5 * std::sin (2 * kPi * f * n / sr)); });
            YinDetector probe; probe.prepare (sr, {});
            double maxC, vf;
            pitchStats (r, f, maxC, vf, size_t (0.08 * sr / probe.hopSamples())); // skip 80 ms: frame fill + HP settling
            if (maxC > 3.0 || vf < 0.99)
                std::printf ("  sr=%g f=%g maxCents=%.3f voiced=%.2f\n", sr, f, maxC, vf);
            CHECK (maxC < 3.0);
            CHECK (vf > 0.99);
        }
}

// Vocal-like harmonic tone; optionally with the 2nd harmonic dominating (classic octave-up trap).
static float harmonicTone (int n, double sr, double f, bool strongSecond)
{
    double s = 0;
    for (int h = 1; h <= 12; ++h)
    {
        double a = 1.0 / h;
        if (strongSecond) a = h == 2 ? 1.0 : (h == 1 ? 0.25 : a * 0.5);
        s += a * std::sin (2 * kPi * f * h * n / sr + 0.3 * h);
    }
    return float (0.25 * s);
}

TEST (yin_harmonic_no_octave_errors)
{
    for (bool strong : { false, true })
        for (double f : { 98.0, 147.0, 220.0, 330.0, 523.25 })
        {
            auto r = runDetector (48000, 0.4, [&] (int n) { return harmonicTone (n, 48000, f, strong); });
            int octaveErrors = 0, voiced = 0;
            for (size_t i = 10; i < r.est.size(); ++i)
                if (r.est[i].voiced)
                {
                    ++voiced;
                    octaveErrors += std::abs (1200.0 * std::log2 (r.est[i].frequency / f)) > 600.0;
                }
            if (octaveErrors) std::printf ("  f=%g strong2nd=%d octaveErrors=%d/%d\n", f, strong, octaveErrors, voiced);
            CHECK (voiced > 0);
            CHECK (octaveErrors == 0);
        }
}

TEST (yin_silence_noise_and_nonfinite)
{
    auto silent = runDetector (48000, 0.3, [] (int) { return 0.0f; });
    CHECK (std::none_of (silent.est.begin(), silent.est.end(), [] (auto& e) { return e.voiced; }));

    std::mt19937 rng (1);
    std::normal_distribution<float> g (0.0f, 0.2f);
    auto noise = runDetector (48000, 0.5, [&] (int) { return g (rng); });
    const auto voicedNoise = std::count_if (noise.est.begin(), noise.est.end(), [] (auto& e) { return e.voiced; });
    if (voicedNoise) std::printf ("  noise voiced %ld/%zu\n", long (voicedNoise), noise.est.size());
    CHECK (voicedNoise <= long (noise.est.size() / 50)); // <= 2% false voicing on white noise

    // NaN/inf bursts must not poison state: pitch is recovered once the frame has refilled.
    const double f = 220;
    auto r = runDetector (48000, 0.6, [&] (int n) {
        if (n > 4800 && n < 4900) return (n & 1) ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
        return float (0.5 * std::sin (2 * kPi * f * n / 48000)); });
    CHECK (r.est.back().voiced);
    CHECK_NEAR (r.est.back().frequency, f, 0.5);
    for (auto& e : r.est) CHECK (std::isfinite (e.frequency) && std::isfinite (e.confidence));
}

TEST (yin_block_size_independent)
{
    auto gen = [] (int n) { return harmonicTone (n, 48000, 187.0, false); };
    auto ref = runDetector (48000, 0.3, gen, 1);
    for (int block : { 7, 32, 64, 333, 1024 })
    {
        auto r = runDetector (48000, 0.3, gen, block);
        CHECK (r.est.size() == ref.est.size());
        bool same = r.est.size() == ref.est.size();
        for (size_t i = 0; same && i < r.est.size(); ++i)
            same = r.est[i].frequency == ref.est[i].frequency && r.est[i].voiced == ref.est[i].voiced;
        CHECK (same);
    }
}

TEST (yin_octave_hold_and_genuine_leap)
{
    const double sr = 48000, f = 196.0;
    // Fundamental drops out for 20 ms (dominant 2nd harmonic): estimates must not jump an octave up.
    auto gen = [&] (int n) {
        const double t = n / sr;
        const double a1 = (t > 0.30 && t < 0.32) ? 0.02 : 0.6;
        double s = a1 * std::sin (2 * kPi * f * t);
        for (int h = 2; h <= 8; ++h) s += (h == 2 ? 0.8 : 0.3 / h) * std::sin (2 * kPi * f * h * t + h);
        return float (0.4 * s); };
    auto r = runDetector (sr, 0.5, gen);
    int up = 0;
    for (size_t i = 10; i < r.est.size(); ++i) up += r.est[i].voiced && r.est[i].frequency > f * 1.5;
    CHECK (up == 0);
    // A real octave leap (196 -> 392 Hz) must be followed after the hold expires.
    auto leap = runDetector (sr, 0.6, [&] (int n) { return harmonicTone (n, sr, n < sr * 0.3 ? f : 2 * f, false); });
    YinDetector probe; probe.prepare (sr, {});
    const size_t after = size_t ((0.3 + 0.03 + 0.04) * sr / probe.hopSamples()) + 1; // leap + hold + frame
    for (size_t i = after; i < leap.est.size(); ++i) CHECK_NEAR (leap.est[i].frequency, 2 * f, 1.0);
}

TEST (yin_tracks_note_step)
{
    // 220 Hz -> 247 Hz step at 0.25 s: detector should settle on the new note within one frame + hop.
    const double sr = 48000;
    auto r = runDetector (sr, 0.5, [&] (int n) { return harmonicTone (n, sr, n < sr / 4 ? 220.0 : 246.94, false); });
    YinDetector d; d.prepare (sr, {});
    const double settleSec = (d.frameLengthSamples() / d.detectorSampleRate()) + 2 * d.hopSamples() / sr;
    const size_t settledIdx = size_t ((0.25 + settleSec) * sr / d.hopSamples()) + 1;
    for (size_t i = settledIdx; i < r.est.size(); ++i)
        CHECK_NEAR (1200.0 * std::log2 (r.est[i].frequency / 246.94), 0.0, 5.0);
    std::printf ("  frame=%.1f ms hop=%.1f ms settle bound=%.1f ms\n",
                 1000.0 * d.frameLengthSamples() / d.detectorSampleRate(), 1000.0 * d.hopSamples() / sr, 1000 * settleSec);
}

TEST (yin_cpu_per_hop)
{
    // Not a pass/fail test: reports worst/avg analysis cost for the CPU budget (Release build only meaningful).
    for (double sr : { 48000.0, 192000.0 })
    {
        YinDetector d; d.prepare (sr, {});
        std::vector<float> blk (128);
        std::vector<double> perBlockUs;
        int n = 0;
        for (int b = 0; b < int (sr * 2 / 128); ++b)
        {
            for (auto& s : blk) s = harmonicTone (n++, sr, 150, false);
            const auto t0 = std::chrono::steady_clock::now();
            d.process (blk.data(), 128, [] (int, const PitchEstimate&) {});
            perBlockUs.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());
        }
        std::sort (perBlockUs.begin(), perBlockUs.end());
        double avg = 0; for (double v : perBlockUs) avg += v; avg /= double (perBlockUs.size());
        std::printf ("  sr=%g block=128 (%.0f us budget): avg %.1f us, p99 %.1f us, max %.1f us\n", sr, 128e6 / sr, avg,
                     perBlockUs[perBlockUs.size() * 99 / 100], perBlockUs.back());
    }
}

// ---------- pitch shifter ----------
// Runs `in` through the shifter at a constant ratio with the true period supplied.
static std::vector<float> shift (const std::vector<float>& in, double sr, float ratio, double periodSamples, int block = 64)
{
    PitchShifter ps;
    ps.prepare (sr, 1, 70.0);
    ps.setPeriod (periodSamples);
    std::vector<float> out = in, r (size_t (block), ratio);
    for (size_t n = 0; n < out.size(); n += size_t (block))
    {
        float* p = out.data() + n;
        ps.process (&p, 1, int (std::min (size_t (block), out.size() - n)), r.data());
    }
    return out;
}

static std::vector<float> tone (double sr, double f, double seconds, bool harmonic)
{
    std::vector<float> v (size_t (sr * seconds));
    for (size_t n = 0; n < v.size(); ++n)
        v[n] = harmonic ? harmonicTone (int (n), sr, f, false) : float (0.5 * std::sin (2 * kPi * f * double (n) / sr));
    return v;
}

static double medianPitch (const std::vector<float>& x, double sr, size_t skipEstimates)
{
    YinDetector d; d.prepare (sr, {});
    std::vector<float> f;
    d.process (x.data(), int (x.size()), [&] (int, const PitchEstimate& e) { if (e.voiced) f.push_back (e.frequency); });
    if (f.size() <= skipEstimates) return 0;
    f.erase (f.begin(), f.begin() + long (skipEstimates));
    std::nth_element (f.begin(), f.begin() + long (f.size() / 2), f.end());
    return f[f.size() / 2];
}

TEST (shifter_unity_is_pure_delay)
{
    const double sr = 48000;
    PitchShifter ps; ps.prepare (sr, 1, 70.0);
    auto in = tone (sr, 210.0, 0.3, true);
    auto out = shift (in, sr, 1.0f, sr / 210.0);
    const size_t L = size_t (ps.latencySamples());
    double maxErr = 0;
    for (size_t n = L; n < in.size(); ++n) maxErr = std::max (maxErr, double (std::abs (out[n] - in[n - L])));
    CHECK (maxErr < 1e-6);
    std::printf ("  latency %zu samples = %.2f ms @48k\n", L, 1000.0 * double (L) / sr);
}

TEST (shifter_mean_delay_equals_latency)
{
    // Across ratios and pitches the delay sawtooth must stay centred on the reported latency.
    const double sr = 48000;
    for (double minF : { 70.0, 100.0, 150.0 })
    {
        PitchShifter ps; ps.prepare (sr, 1, 70.0); ps.setMinFreq (minF); ps.reset();
        std::printf ("  range %g Hz: latency %d samples = %.2f ms\n", minF, ps.latencySamples(), 1000.0 * ps.latencySamples() / sr);
        for (double semis : { -2.0, -0.3, 0.3, 2.0 })
            for (double f : { minF * 1.01, 300.0 })
            {
                ps.reset(); ps.setPeriod (sr / f);
                std::vector<float> x (1, 0.0f), r (1, float (std::exp2 (semis / 12.0)));
                double sum = 0, lo = 1e9, hi = -1e9; const int N = 48000;
                for (int i = 0; i < N; ++i) { float* p = x.data(); ps.process (&p, 1, 1, r.data()); sum += ps.currentDelay(); lo = std::min (lo, ps.currentDelay()); hi = std::max (hi, ps.currentDelay()); }
                const double mean = sum / N, P = sr / f;
                CHECK_NEAR (mean, ps.latencySamples(), 0.06 * P + 2);
                CHECK (lo >= 2.0);
                CHECK (hi - lo <= P + 2 * 192 * 0.13 + 2); // sawtooth spans one period (+ fade drift)
            }
    }
}

TEST (shifter_pitch_accuracy)
{
    for (double sr : { 44100.0, 48000.0, 96000.0 })
        for (double semis : { -6.0, -1.0, -0.3, 0.3, 1.0, 6.0 })
            for (double f : { 110.0, 220.0, 440.0 })
            {
                const auto ratio = float (std::exp2 (semis / 12.0));
                auto out = shift (tone (sr, f, 0.6, true), sr, ratio, sr / f);
                const double got = medianPitch (out, sr, 20);
                const double errC = 1200.0 * std::log2 (got / (f * ratio));
                if (std::abs (errC) > 3.0) std::printf ("  sr=%g f=%g semis=%g err=%.2f c\n", sr, f, semis, errC);
                CHECK (std::abs (errC) < 3.0);
            }
}

TEST (shifter_splices_are_click_free)
{
    // Pure sine: the largest sample-to-sample step of the output may not exceed that of an ideal
    // sine at the shifted frequency by more than 10% (a misaligned splice produces a step jump).
    const double sr = 48000;
    for (double semis : { -2.0, -0.5, 0.5, 2.0 })
        for (double f : { 100.0, 233.0, 600.0 })
        {
            const auto ratio = float (std::exp2 (semis / 12.0));
            auto out = shift (tone (sr, f, 0.5, false), sr, ratio, sr / f);
            double maxStep = 0;
            for (size_t n = 2000; n < out.size(); ++n) maxStep = std::max (maxStep, double (std::abs (out[n] - out[n - 1])));
            const double ideal = 2 * kPi * f * ratio / sr * 0.5;
            if (maxStep > ideal * 1.10) std::printf ("  f=%g semis=%g step %.5f vs ideal %.5f\n", f, semis, maxStep, ideal);
            CHECK (maxStep <= ideal * 1.10);
        }
}

TEST (shifter_robust_inputs)
{
    const double sr = 48000;
    std::mt19937 rng (3);
    std::normal_distribution<float> g (0.0f, 0.3f);
    std::vector<float> noise (24000);
    for (auto& v : noise) v = g (rng);
    for (double period : { 0.0, -5.0, 1e9, 37.3 }) // unvoiced, invalid, huge, arbitrary
    {
        auto out = shift (noise, sr, 1.3f, period, 17);
        float peak = 0;
        for (float v : out) { CHECK (std::isfinite (v)); peak = std::max (peak, std::abs (v)); }
        CHECK (peak < 2.0f);
    }
    auto out = shift (tone (sr, 220, 0.2, false), sr, 100.0f, sr / 220, 5); // ratio clamped, not UB
    for (float v : out) CHECK (std::isfinite (v));
}

TEST (fixed_delay)
{
    FixedDelay fd; fd.prepare (1, 5);
    std::vector<float> x (20);
    for (size_t i = 0; i < x.size(); ++i) x[i] = float (i + 1);
    float* p = x.data();
    fd.process (&p, 1, 7); p += 7; fd.process (&p, 1, 13);
    for (size_t i = 0; i < x.size(); ++i) CHECK (x[i] == (i < 5 ? 0.0f : float (i - 4)));
}

TEST (fixed_delay_change_is_smooth)
{
    FixedDelay fd; fd.prepare (1, 300, 240);
    std::vector<float> x (4800);
    for (size_t i = 0; i < x.size(); ++i) x[i] = float (0.5 * std::sin (2 * kPi * 220 * double (i) / 48000));
    float* p = x.data(); fd.process (&p, 1, 2400);
    fd.setDelay (120);
    p = x.data() + 2400; fd.process (&p, 1, 2400);
    double maxStep = 0;
    for (size_t i = 400; i < x.size(); ++i) maxStep = std::max (maxStep, double (std::abs (x[i] - x[i - 1])));
    CHECK (maxStep < 0.5 * 2 * kPi * 220 / 48000 * 1.05);
}

// ---------- correction engine ----------
static PitchEstimate est (double hz) { return { float (hz), 0.95f, true }; }

TEST (correction_amount_and_speed_zero)
{
    CorrectionEngine e; e.prepare (48000);
    CorrectionEngine::Params p; p.speedMs = 0;
    const double sharp = midiToFreq (69.30); // A4 + 30 c -> target A4
    for (double amount : { 0.0, 0.5, 1.0 })
    {
        p.amount = amount; e.setParams (p); e.reset();
        e.onEstimate (est (sharp));
        const double cents = 1200.0 * std::log2 (double (e.nextRatio()));
        CHECK_NEAR (cents, -30.0 * amount, 1e-3);
    }
}

TEST (correction_speed_time_constant)
{
    const double sr = 48000;
    for (double ms : { 5.0, 50.0, 200.0 })
    {
        // A 1 s estimate interval puts the single estimate's note past the onset window (no 8 ms shortcut).
        CorrectionEngine e; e.prepare (sr, 1.0);
        CorrectionEngine::Params p; p.speedMs = ms; e.setParams (p);
        e.onEstimate (est (midiToFreq (69.40))); // 40 c sharp
        float r = 1, prev = 1;
        bool monotonic = true;
        for (int i = 0; i < int (ms / 1000 * sr); ++i) { r = e.nextRatio(); monotonic = monotonic && r <= prev; prev = r; }
        // Critically damped pair: 63.2 % of the step after one Speed time constant, like a one-pole.
        CHECK_NEAR (1200.0 * std::log2 (double (r)) / -40.0, 1.0 - std::exp (-1.0), 0.005);
        // ... approached monotonically, and never past the target.
        for (int i = 0; i < int (10 * ms / 1000 * sr); ++i) { r = e.nextRatio(); monotonic = monotonic && r <= prev; prev = r; }
        CHECK (monotonic);
        CHECK (1200.0 * std::log2 (double (r)) >= -40.0 - 0.01); // float ratio resolution
    }
}

TEST (correction_negative_speed_and_edges)
{
    CorrectionEngine e; e.prepare (48000);
    CorrectionEngine::Params p;
    for (double s : { -3.0, -1.5, -0.01, -100.0, std::numeric_limits<double>::quiet_NaN() })
    {
        p.speedMs = s; e.setParams (p); e.reset();
        e.onEstimate (est (midiToFreq (60.2)));
        const float r = e.nextRatio();
        CHECK (std::isfinite (r));
        if (s >= -3.0 && s < 0) CHECK_NEAR (1200.0 * std::log2 (double (r)), -20.0, 1e-3); // instant
    }
    // -3 ms: zero hysteresis -> switches as soon as the midpoint is crossed.
    p.speedMs = -3; e.setParams (p); e.reset();
    e.onEstimate (est (midiToFreq (60.45))); CHECK (e.targetNote() == 60);
    e.onEstimate (est (midiToFreq (60.52))); CHECK (e.targetNote() == 61);
    // Empty mask and unvoiced input release to ratio 1.
    p.speedMs = 0; p.mask = 0; e.setParams (p); e.reset();
    e.onEstimate (est (midiToFreq (60.3)));
    CHECK (e.nextRatio() == 1.0f);
    p.mask = kAllNotes; e.setParams (p);
    e.onEstimate (est (midiToFreq (60.3))); e.nextRatio();
    e.onEstimate ({ 0, 0, false });
    float r = 0; for (int i = 0; i < 48000; ++i) r = e.nextRatio();
    CHECK_NEAR (r, 1.0, 1e-4);
}


// ---------- note stabilizer ----------
// Feeds `seconds` of a constant pitch as 2 ms estimates; returns the last target.
static int feed (CorrectionEngine& e, double midi, double seconds, float* lastRatio = nullptr)
{
    const int hops = int (std::lround (seconds / 0.002));
    for (int i = 0; i < hops; ++i)
    {
        e.onEstimate (est (midiToFreq (midi)));
        for (int k = 0; k < 96; ++k) { const float r = e.nextRatio(); if (lastRatio) *lastRatio = r; } // 2 ms of samples
    }
    return e.targetNote();
}

TEST (stabilizer_ignores_short_excursion)
{
    for (double ms : { 40.0, 80.0, 200.0 })
    {
        CorrectionEngine e; e.prepare (48000, 0.002);
        CorrectionEngine::Params p; p.speedMs = 0; p.stabilizerMs = ms; e.setParams (p);
        CHECK (feed (e, 60.1, 0.2) == 60);                // onset: confirmed immediately
        // Excursion to D (62) shorter than the threshold: no retarget, correction released (pitch passes).
        float r = 0;
        CHECK (feed (e, 62.2, ms / 1000.0 * 0.5, &r) == TargetSelector::kNoTarget);
        CHECK (e.pendingNote() == 62);
        CHECK (feed (e, 60.1, 0.05) == 60);               // back on C: old target, no D ever confirmed
        CHECK (e.pendingNote() == TargetSelector::kNoTarget);
    }
}

TEST (stabilizer_confirms_after_threshold)
{
    for (double ms : { 40.0, 80.0, 200.0 })
    {
        CorrectionEngine e; e.prepare (48000, 0.002);
        CorrectionEngine::Params p; p.speedMs = 0; p.stabilizerMs = ms; e.setParams (p);
        feed (e, 60.1, 0.2);
        // Count hops until 62 is confirmed: must be the threshold (+- one hop).
        int hops = 0;
        while (feed (e, 62.2, 0.002) != 62 && hops < 1000) ++hops;
        CHECK_NEAR ((hops + 1) * 2.0, ms, 2.0);
        float r = 0; feed (e, 62.2, 0.002, &r);
        CHECK_NEAR (1200.0 * std::log2 (double (r)), -20.0, 1e-3); // fully corrected once confirmed
    }
    // Off: retargets on the first estimate.
    CorrectionEngine e; e.prepare (48000, 0.002);
    CorrectionEngine::Params p; p.speedMs = 0; e.setParams (p);
    feed (e, 60.1, 0.1);
    CHECK (feed (e, 62.2, 0.002) == 62);
}

TEST (stabilizer_onset_after_silence_is_immediate)
{
    CorrectionEngine e; e.prepare (48000, 0.002);
    CorrectionEngine::Params p; p.speedMs = 0; p.stabilizerMs = 200; e.setParams (p);
    feed (e, 60.1, 0.2);
    for (int i = 0; i < 40; ++i) e.onEstimate ({ 0, 0, false }); // 80 ms gap > 60 ms onset gap
    CHECK (feed (e, 64.2, 0.002) == 64);
    // A short unvoiced gap (consonant) inside a phrase keeps the stabilizer engaged.
    for (int i = 0; i < 10; ++i) e.onEstimate ({ 0, 0, false }); // 20 ms
    CHECK (feed (e, 67.1, 0.002) == TargetSelector::kNoTarget);
    CHECK (e.pendingNote() == 67);
}

// ---------- sustain ----------
// Time for the correction to reach 63.2 % of a step applied after the note has been held `hold` s.
static double timeConstantAfterHold (double speedMs, double sustain, double hold)
{
    const double sr = 48000;
    CorrectionEngine e; e.prepare (sr, 0.002);
    CorrectionEngine::Params p; p.speedMs = speedMs; p.sustain = sustain; e.setParams (p);
    feed (e, 60.0, hold); // hold the note exactly in tune (no correction needed)
    // Step: same note, now 40 c sharp. Count samples until 63.2 %.
    int n = 0; double cents = 0;
    for (int hop = 0; hop < 5000 && cents > -40.0 * (1.0 - std::exp (-1.0)); ++hop)
    {
        e.onEstimate (est (midiToFreq (60.4)));
        for (int k = 0; k < 96 && cents > -40.0 * (1.0 - std::exp (-1.0)); ++k, ++n)
            cents = 1200.0 * std::log2 (double (e.nextRatio()));
    }
    return 1000.0 * n / sr;
}

TEST (sustain_scales_speed_with_note_age)
{
    const double base = timeConstantAfterHold (80, 0.0, 1.0);
    const double fast = timeConstantAfterHold (80, 1.0, 1.0);
    const double slow = timeConstantAfterHold (80, -1.0, 1.0);
    std::printf ("  tau after 1 s hold: sustain 0 %.1f ms, +100 %.1f ms, -100 %.1f ms\n", base, fast, slow);
    CHECK_NEAR (base, 80.0, 3.0);
    CHECK_NEAR (fast, 10.0, 2.5);   // 80 / 8
    CHECK_NEAR (slow, 640.0, 25.0); // 80 * 8
    // Young note (just started): sustain has barely any effect yet.
    CHECK (timeConstantAfterHold (80, 1.0, 0.045) > 50.0); // just past the onset window: at most ~1.6x faster
    // Speed 0 stays instant whatever the sustain.
    CorrectionEngine e; e.prepare (48000, 0.002);
    CorrectionEngine::Params p; p.speedMs = 0; p.sustain = -1; e.setParams (p);
    feed (e, 60.0, 1.0);
    e.onEstimate (est (midiToFreq (60.3)));
    CHECK_NEAR (1200.0 * std::log2 (double (e.nextRatio())), -30.0, 1e-3);
    // Invalid values are clamped, never NaN.
    p.speedMs = 50; p.sustain = std::numeric_limits<double>::quiet_NaN(); p.stabilizerMs = -5; e.setParams (p);
    feed (e, 61.3, 0.1);
    CHECK (std::isfinite (e.nextRatio()));
}

// ---------- prediction, vibrato, onsets ----------
// Feeds a pitch trajectory as estimates every 2 ms that are `staleMs` old (like the real detector);
// returns the output pitch (input + correction) per sample from `from` seconds on.
template <typename Pitch>
static std::vector<double> runEngine (CorrectionEngine& e, Pitch&& pitch, double seconds, double from = 0, double staleMs = 1.8)
{
    const double sr = 48000;
    std::vector<double> y;
    for (int n = 0; n < int (seconds * sr); ++n)
    {
        const double t = n / sr;
        if (n % 96 == 0) { const double m = pitch (t - staleMs / 1000); e.onEstimate ({ float (m > 0 ? midiToFreq (m) : 0), 0.95f, m > 0 }); }
        const double c = 12.0 * std::log2 (double (e.nextRatio()));
        if (t >= from) y.push_back (pitch (t) + c);
    }
    return y;
}
static CorrectionEngine makeEngine (double speedMs, bool timing = true)
{
    CorrectionEngine e; e.prepare (48000, 0.002);
    CorrectionEngine::Params p; p.speedMs = speedMs; e.setParams (p);
    if (timing) e.setTiming (0.0067, 0.5 * (0.0067 + 1.0 / midiToFreq (60)) + 1e-4 - 0.0018); // lead = 1.8 ms at C4, like runEngine's stale estimates
    return e;
}

TEST (prediction_removes_detector_lag_and_is_bounded)
{
    // Slow swing (slope up to 2.8 st/s) with 1.8 ms stale estimates: a lag error of ~0.5 c without
    // prediction; prediction must cancel it.
    auto ramp = [] (double t) { return 60.0 + 0.15 * std::sin (2 * kPi * 3.0 * t); };
    for (bool timing : { false, true })
    {
        auto e = makeEngine (0, timing);
        auto y = runEngine (e, ramp, 1.0, 0.3);
        double worst = 0;
        for (double v : y) worst = std::max (worst, std::abs (100 * (v - 60.0)));
        std::printf ("  slow swing, Speed 0, prediction %s: max error %.2f c\n", timing ? "on " : "off", worst);
        CHECK (timing ? worst < 0.15 : worst > 0.4);
    }
    // A single bad estimate (+60 c for one hop) changes the predicted offset by at most kMaxPredict.
    auto a = makeEngine (0, true), b = makeEngine (0, false);
    double maxDiff = 0;
    for (int hop = 0; hop < 100; ++hop)
    {
        const double m = hop == 50 ? 60.70 : 60.10;
        a.onEstimate (est (midiToFreq (m))); b.onEstimate (est (midiToFreq (m)));
        for (int k = 0; k < 96; ++k)
            maxDiff = std::max (maxDiff, std::abs (12.0 * std::log2 (double (a.nextRatio()) / double (b.nextRatio()))));
    }
    CHECK (maxDiff <= CorrectionEngine::kMaxPredict + 1e-6);
}

TEST (vibrato_widens_hysteresis)
{
    // +-50 c, 5.5 Hz vibrato on a note sung 25 c sharp: peaks cross the C/C# midpoint by 25 c.
    // Fixed 15 c hysteresis flips the target every cycle; the vibrato-aware one must not.
    auto e = makeEngine (0);
    auto vib = [] (double t, double centre) { return centre + 0.5 * std::sin (2 * kPi * 5.5 * t); };
    int flips = 0, last = -1;
    for (int hop = 0; hop < 1000; ++hop) // 2 s
    {
        e.onEstimate (est (midiToFreq (vib (hop * 0.002, 60.25))));
        if (hop >= 150) { flips += last >= 0 && e.targetNote() != last; last = e.targetNote(); } // after ~1.5 cycles
        for (int k = 0; k < 96; ++k) e.nextRatio();
    }
    std::printf ("  vibrato depth seen %.0f c, target flips %d\n", e.vibratoDepthCents(), flips);
    CHECK (flips == 0);
    CHECK_NEAR (e.vibratoDepthCents(), 50.0, 5.0);
    // The singer really moves up a semitone (same vibrato): followed within 200 ms.
    int hops = 0;
    for (; hops < 500 && e.targetNote() != 61; ++hops)
    {
        e.onEstimate (est (midiToFreq (vib (2.0 + hops * 0.002, 61.25))));
        for (int k = 0; k < 96; ++k) e.nextRatio();
    }
    CHECK (hops * 2 < 200);
}

TEST (slow_speed_keeps_vibrato_and_tunes_its_centre)
{
    // +-40 c, 5.5 Hz vibrato, centre 30 c sharp. Output: centre on the note, vibrato kept in shape.
    auto measure = [] (double speed, double& centre, double& kept) {
        auto e = makeEngine (speed);
        auto y = runEngine (e, [] (double t) { return 60.3 + 0.4 * std::sin (2 * kPi * 5.5 * t); }, 3.0, 1.0);
        double mean = 0, inPhase = 0;
        for (double v : y) mean += v / double (y.size());
        for (size_t i = 0; i < y.size(); ++i) inPhase += 2.0 * (y[i] - mean) * std::sin (2 * kPi * 5.5 * (1.0 + double (i) / 48000)) / double (y.size());
        centre = 100 * (mean - 60.0);
        kept = inPhase / 0.4; // fraction of the input vibrato present, in phase, in the output
    };
    double centre, kept;
    for (double speed : { 20.0, 40.0 })
    {
        measure (speed, centre, kept);
        std::printf ("  Speed %.0f: centre %+.2f c, vibrato kept %.0f %%\n", speed, centre, 100 * kept);
        CHECK (std::abs (centre) < 1.0);
        CHECK (kept > (speed == 20.0 ? 0.8 : 0.95) && kept < 1.05);
    }
    measure (0, centre, kept);
    CHECK (std::abs (centre) < 1.0 && std::abs (kept) < 0.03); // Speed 0: flat
}

TEST (steady_onset_is_tuned_fast_scoop_stays_natural)
{
    // Speed 80 ms. Phrase starts 30 c sharp and steady: tuned within the onset window.
    auto steady = makeEngine (80);
    auto y = runEngine (steady, [] (double t) { return t < 0.1 ? 0.0 : 60.3; }, 0.3);
    const double at30 = 100 * (y[size_t (0.130 * 48000)] - 60.0);
    // Same Speed, phrase starts with a fast scoop (-60 c -> 0 in 40 ms, 15 st/s): left natural.
    auto scoop = makeEngine (80);
    auto natural = [] (double t) { return t < 0.1 ? 0.0 : 60.0 - 0.6 * std::max (0.0, 1.0 - (t - 0.1) / 0.04); };
    auto z = runEngine (scoop, natural, 0.3);
    const double at15 = 100 * (z[size_t (0.115 * 48000)] - natural (0.115));
    std::printf ("  steady onset error @30 ms %+.2f c; scoop deviation from natural @15 ms %+.2f c\n", at30, at15);
    CHECK (std::abs (at30) < 1.0);
    CHECK (std::abs (at15) < 6.0);
}

TEST (yin_harmonic_hold)
{
    // Fundamental and every harmonic except 3, 6, 9 vanish for 12 ms: the waveform's shortest period
    // becomes P/3 (19 semitones up). The estimate must hold the real pitch, not jump.
    const double sr = 48000, f = 150.0;
    auto gen = [&] (int n) {
        const double t = n / sr;
        const bool gap = t > 0.30 && t < 0.312;
        double s = 0;
        for (int h = 1; h <= 9; ++h) s += ((gap && h % 3 != 0) ? 0.0 : 1.0 / h) * std::sin (2 * kPi * f * h * t);
        return float (0.3 * s); };
    auto r = runDetector (sr, 0.5, gen);
    int jumped = 0;
    for (size_t i = 10; i < r.est.size(); ++i) jumped += r.est[i].voiced && std::abs (1200 * std::log2 (r.est[i].frequency / f)) > 50;
    CHECK (jumped == 0);
}

TEST (end_to_end_correction)
{
    // Detuned harmonic tone (A3 + 35 c) through detector -> engine -> shifter at speed 0: output ~A3.
    const double sr = 48000, f = midiToFreq (57.35);
    auto x = tone (sr, f, 1.0, true);
    YinDetector det; det.prepare (sr, {});
    CorrectionEngine eng; eng.prepare (sr);
    CorrectionEngine::Params p; p.speedMs = 0; eng.setParams (p);
    PitchShifter ps; ps.prepare (sr, 1, 70.0);
    std::vector<float> ratio (32);
    for (size_t n = 0; n < x.size(); n += 32)
    {
        float* blk = x.data() + n;
        det.process (blk, 32, [&] (int, const PitchEstimate& e) {
            eng.onEstimate (e);
            ps.setPeriod (e.voiced ? sr / e.frequency : 0); });
        for (auto& r : ratio) r = eng.nextRatio();
        ps.process (&blk, 1, 32, ratio.data());
    }
    const double errC = 1200.0 * std::log2 (medianPitch (x, sr, 40) / midiToFreq (57.0));
    std::printf ("  residual %.2f c (input was +35 c)\n", errC);
    CHECK (std::abs (errC) < 3.0);
}


// ---------- HQ mode ----------
// Runs a stereo-identical signal through a chain; returns the left output.
static std::vector<float> runChain (const std::vector<float>& in, TunerChain::Settings s, double sr = 48000, int block = 64)
{
    TunerChain chain; chain.prepare (sr, 70.0);
    chain.setSettings (s); chain.reset();
    std::vector<float> L = in, R = in;
    for (size_t n = 0; n < L.size(); n += size_t (block))
    {
        const int len = int (std::min (size_t (block), L.size() - n));
        float* io[2] { L.data() + n, R.data() + n };
        chain.setSettings (s);
        chain.process (io, 2, len);
    }
    return L;
}

TEST (hq_latency_and_alignment)
{
    const double sr = 48000;
    for (double minF : { 70.0, 150.0 })
    {
        TunerChain::Settings s; s.minFreq = minF; s.correction.amount = 0; // ratio 1: wet = pure delay
        TunerChain a; a.prepare (sr, 70.0);
        s.hq = true; a.setSettings (s); a.reset();
        CHECK (a.latencySamples() == TunerChain::latencyFor (sr, minF, true));
        CHECK (TunerChain::latencyFor (sr, minF, true) - TunerChain::latencyFor (sr, minF, false) == 96);
        // With HQ on, wet output must equal the input delayed by exactly the reported latency.
        auto x = tone (sr, 220.0, 0.3, true);
        auto y = runChain (x, s);
        const size_t L = size_t (a.latencySamples());
        double maxErr = 0;
        for (size_t n = L + 4800; n < x.size(); ++n) maxErr = std::max (maxErr, double (std::abs (y[n] - x[n - L])));
        CHECK (maxErr < 1e-5);
    }
}

TEST (hq_tracks_glides_better)
{
    // Off-key glide (vocal-like scoop between notes) at Speed 0. Slope prediction (CorrectionEngine)
    // closes the detector's lag causally, so HQ off must be as good as HQ on, and both tight.
    const double sr = 48000;
    std::vector<float> x (size_t (sr * 1.5));
    double ph = 0;
    for (size_t n = 0; n < x.size(); ++n)
    {
        const double t = double (n) / sr;
        const double midi = 57.3 + 2.0 * std::sin (2 * kPi * 0.8 * t); // slow 4-semitone swing
        ph += 2 * kPi * midiToFreq (midi) / sr;
        double v = 0;
        for (int h = 1; h <= 8; ++h) v += std::sin (h * ph) / h;
        x[n] = float (0.25 * v);
    }
    auto residual = [&] (bool hq) {
        TunerChain::Settings s; s.correction.speedMs = 0; s.minFreq = 150; s.hq = hq;
        auto y = runChain (x, s);
        YinDetector d; d.prepare (sr, {});
        std::vector<double> err;
        d.process (y.data() + int (sr * 0.2), int (y.size()) - int (sr * 0.2), [&] (int, const PitchEstimate& e) {
            if (e.voiced) { const double m = freqToMidi (e.frequency); err.push_back (std::abs (100.0 * (m - std::round (m)))); } });
        std::sort (err.begin(), err.end());
        return err[err.size() * 9 / 10];
    };
    const double off = residual (false), on = residual (true);
    std::printf ("  glide p90 residual: HQ off %.2f c, HQ on %.2f c\n", off, on);
    CHECK (off < 3.0 && on < 3.0);
    CHECK (off < on + 0.5);
}

// ---------- doubler ----------
TEST (doubler_off_is_bit_exact)
{
    auto x = tone (48000, 233.0, 0.4, true);
    TunerChain::Settings s; s.correction.speedMs = 10;
    auto a = runChain (x, s);
    s.doubler.width = 1.0f; s.doubler.blend = 1.0f; // parameters set but disabled
    auto b = runChain (x, s);
    CHECK (a == b);
}

TEST (doubler_voices_detuned_late_and_wide)
{
    const double sr = 48000;
    Doubler d; d.prepare (sr);
    Doubler::Params p; p.enabled = true; p.width = 1.0f; p.blend = 1.0f; d.setParams (p); d.reset();
    d.setPeriod (sr / 220.0);
    // Feed a click train + tone; isolate voices by subtracting the scaled main (0.7 x input).
    auto x = tone (sr, 220.0, 1.2, false);
    std::vector<float> L = x, R = x;
    for (size_t n = 0; n < x.size(); n += 64)
    {
        float* io[2] { L.data() + n, R.data() + n };
        d.process (io, 2, int (std::min<size_t> (64, x.size() - n)));
    }
    std::vector<float> vl (x.size()), vr (x.size());
    for (size_t n = 0; n < x.size(); ++n) { vl[n] = L[n] - 0.7f * x[n]; vr[n] = R[n] - 0.7f * x[n]; }
    // With width 1, left holds voice 1 (-8 c) and right voice 2 (+6 c), each +-3 c wobble.
    const double cl = 1200.0 * std::log2 (medianPitch (vl, sr, 100) / 220.0);
    const double cr = 1200.0 * std::log2 (medianPitch (vr, sr, 100) / 220.0);
    std::printf ("  voice detune: L %.1f c, R %.1f c\n", cl, cr);
    CHECK_NEAR (cl, -8.0, 3.5);
    CHECK_NEAR (cr, 6.0, 3.5);
    // L and R must be decorrelated (not the same waveform panned).
    double sxy = 0, sxx = 0, syy = 0;
    for (size_t n = size_t (sr * 0.3); n < x.size(); ++n) { sxy += double (L[n]) * R[n]; sxx += double (L[n]) * L[n]; syy += double (R[n]) * R[n]; }
    const double corr = sxy / std::sqrt (sxx * syy);
    std::printf ("  L/R correlation at width 100 %%: %.3f\n", corr);
    CHECK (corr < 0.95);
}

TEST (doubler_gain_stable_and_safe)
{
    const double sr = 48000;
    auto x = tone (sr, 196.0, 0.8, true);
    auto rmsDb = [&] (float blend, bool en) {
        TunerChain::Settings s; s.correction.amount = 0; s.doubler.enabled = en; s.doubler.blend = blend; s.doubler.width = 0.7f;
        auto y = runChain (x, s);
        double e = 0; size_t cnt = 0;
        for (size_t n = size_t (sr * 0.3); n < y.size(); ++n, ++cnt) e += double (y[n]) * y[n];
        return 10.0 * std::log10 (e / double (cnt));
    };
    const double ref = rmsDb (0.0f, false);
    for (float b : { 0.25f, 0.5f, 1.0f })
    {
        const double dB = rmsDb (b, true) - ref;
        std::printf ("  blend %.0f %%: %+.2f dB\n", b * 100, dB);
        CHECK (std::abs (dB) < 2.0);
    }
    // Non-finite parameters and huge input stay finite.
    Doubler d; d.prepare (sr);
    Doubler::Params p; p.enabled = true; p.width = std::numeric_limits<float>::quiet_NaN(); p.blend = 7.0f; d.setParams (p);
    d.setPeriod (-1);
    std::vector<float> big (64, 1e6f);
    float* io[1] { big.data() };
    d.process (io, 1, 64);
    for (float v : big) CHECK (std::isfinite (v));
}

// ---------- full chain ----------
// ---------- LyraStrike (formant accent) ----------
// Vowel-like test signal: harmonics of f0 shaped by resonances (formants) at F1, F2 and 2.9 kHz, all
// scaled by `formantScale` (1.04 = the same vowel with its formants really 4 % higher).
static std::vector<float> vowel (double sr, double f0, double F1, double seconds, double F2 = 1800.0, double formantScale = 1.0)
{
    const double F[3] { F1 * formantScale, F2 * formantScale, 2900.0 * formantScale }, B[3] { 160.0, 240.0, 360.0 }, A[3] { 1.0, 0.6, 0.3 };
    std::vector<float> v (size_t (sr * seconds));
    for (int h = 1; h * f0 < 0.45 * sr && h * f0 < 6000; ++h)
    {
        const double f = h * f0;
        double a = 0;
        for (int i = 0; i < 3; ++i) a += A[i] / std::sqrt (1.0 + std::pow ((f - F[i]) / (0.5 * B[i]), 2.0));
        for (size_t n = 0; n < v.size(); ++n) v[n] += float (0.05 * a * std::sin (2 * kPi * f * double (n) / sr + 0.7 * h));
    }
    return v;
}
// Runs a FormantAccent over x (32-sample blocks), triggering `dir` at each time in `at` (s).
static std::vector<float> runAccent (const std::vector<float>& x, double sr, std::vector<std::pair<double, int>> at, int* maxActiveSamples = nullptr,
                                     double depth = 0.04, double length = 0.025)
{
    // Explicit 4 % / 25 ms (not the defaults): the tests below are written around that shape.
    FormantAccent fa; fa.prepare (sr); fa.setEnabled (true); fa.setDepth (depth); fa.setLength (length);
    std::vector<float> y = x;
    int run = 0, longest = 0;
    for (size_t n = 0; n < y.size(); n += 32)
    {
        for (auto [tt, dir] : at) if (size_t (tt * sr) >= n && size_t (tt * sr) < n + 32) fa.trigger (dir);
        float* io[1] { y.data() + n };
        fa.process (io, 1, int (std::min<size_t> (32, y.size() - n)));
        run = fa.isActive() ? run + 32 : 0; longest = std::max (longest, run);
    }
    if (maxActiveSamples) *maxActiveSamples = longest;
    return y;
}
// Amplitude-weighted mean frequency of harmonics 1..30 of f0 over [t0, t0 + len) (Goertzel-style projection).
static double harmonicCentroid (const std::vector<float>& x, double sr, double f0, double t0, double len)
{
    double num = 0, den = 0;
    for (int h = 1; h <= 30; ++h)
    {
        double re = 0, im = 0;
        for (size_t n = size_t (t0 * sr); n < size_t ((t0 + len) * sr); ++n)
        { re += x[n] * std::cos (2 * kPi * h * f0 * double (n) / sr); im += x[n] * std::sin (2 * kPi * h * f0 * double (n) / sr); }
        const double a = std::sqrt (re * re + im * im);
        num += a * h * f0; den += a;
    }
    return num / den;
}

TEST (strike_moves_formants_not_pitch)
{
    // Spectral centroid of the harmonics during the strongest 10 ms of the accent, compared with the same
    // vowels whose formants really move 4 %. Summed over 4 vowels and 2 voices (one vowel alone is noisy:
    // with sparse harmonics a formant move barely changes the centroid).
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        double got[2] {}, want[2] {}, worst = 0, meanAbs = 0;
        for (double f0 : { 110.0, 220.0 })
            for (auto [F1, F2] : { std::pair { 700.0, 1200.0 }, { 400.0, 2000.0 }, { 550.0, 900.0 }, { 300.0, 2300.0 } })
            {
                auto x = vowel (sr, f0, F1, 0.6, F2);
                for (int d = 0; d < 2; ++d) // d 0: note up -> formants down; d 1: note down -> formants up
                {
                    auto y = runAccent (x, sr, { { 0.30, d == 0 ? +1 : -1 } });
                    auto truth = vowel (sr, f0, F1, 0.6, F2, d == 0 ? 0.96 : 1.04);
                    const double c0 = harmonicCentroid (x, sr, f0, 0.303, 0.010);
                    got[d] += harmonicCentroid (y, sr, f0, 0.303, 0.010) / c0 - 1;
                    want[d] += harmonicCentroid (truth, sr, f0, 0.303, 0.010) / c0 - 1;
                    // Pitch: harmonics are never moved, so the accent must average out; the detector sees
                    // a brief transient while the waveform shape changes inside its window.
                    YinDetector det; det.prepare (sr, {});
                    double sum = 0; int cnt = 0; size_t k = 0;
                    det.process (y.data(), int (y.size()), [&] (int, const PitchEstimate& e) {
                        if (++k > 50 && e.voiced) { const double c = 1200 * std::log2 (e.frequency / f0); worst = std::max (worst, std::abs (c)); sum += c; ++cnt; } });
                    meanAbs = std::max (meanAbs, std::abs (sum / std::max (1, cnt)));
                }
            }
        std::printf ("  sr %.0f: formant move vs a true 4 %% move: down %.0f %%, up %.0f %%; pitch: worst mean %.2f c, worst transient %.1f c\n",
                     sr, 100 * got[0] / want[0], 100 * got[1] / want[1], meanAbs, worst);
        CHECK (got[0] < 0 && got[1] > 0);                   // right directions
        CHECK (got[0] / want[0] > 0.6 && got[0] / want[0] < 1.6);
        CHECK (got[1] / want[1] > 0.6 && got[1] / want[1] < 1.6);
        CHECK (meanAbs < 0.3);
        CHECK (worst < 10.0);
    }
}

TEST (strike_amount_scales_the_shift)
{
    // 48 kHz, same vowels as above: the move must track the requested amount from 1 % to 25 %.
    const double sr = 48000;
    double prev = 0;
    for (double depth : { 0.01, 0.04, 0.10, 0.25 })
    {
        double got[2] {}, want[2] {}, worst = 0, pWorst = 0, pMean = 0;
        for (double f0 : { 110.0, 220.0 })
            for (auto [F1, F2] : { std::pair { 700.0, 1200.0 }, { 400.0, 2000.0 }, { 550.0, 900.0 }, { 300.0, 2300.0 } })
            {
                auto x = vowel (sr, f0, F1, 0.6, F2);
                const double c0 = harmonicCentroid (x, sr, f0, 0.303, 0.010);
                for (int d = 0; d < 2; ++d)
                {
                    auto y = runAccent (x, sr, { { 0.30, d == 0 ? +1 : -1 } }, nullptr, depth);
                    auto truth = vowel (sr, f0, F1, 0.6, F2, d == 0 ? 1.0 - depth : 1.0 + depth);
                    got[d] += harmonicCentroid (y, sr, f0, 0.303, 0.010) / c0 - 1;
                    want[d] += harmonicCentroid (truth, sr, f0, 0.303, 0.010) / c0 - 1;
                    for (float v : y) worst = std::max (worst, double (std::abs (v)));
                    YinDetector det; det.prepare (sr, {});
                    double sum = 0; int cnt = 0; size_t k = 0;
                    det.process (y.data(), int (y.size()), [&] (int, const PitchEstimate& e) {
                        if (++k > 50 && e.voiced) { const double c = 1200 * std::log2 (e.frequency / f0); pWorst = std::max (pWorst, std::abs (c)); sum += c; ++cnt; } });
                    pMean = std::max (pMean, std::abs (sum / std::max (1, cnt)));
                }
            }
        const double size = got[1] - got[0];
        std::printf ("  amount +-%.0f %%: move vs true: down %.0f %%, up %.0f %%; peak %.2f; pitch mean %.2f c, transient %.1f c\n",
                     100 * depth, 100 * got[0] / want[0], 100 * got[1] / want[1], worst, pMean, pWorst);
        CHECK (got[0] < 0 && got[1] > 0);
        CHECK (got[0] / want[0] > 0.5 && got[0] / want[0] < 1.6);
        CHECK (got[1] / want[1] > 0.5 && got[1] / want[1] < 1.6);
        CHECK (size > prev); // monotonic in the amount
        CHECK (worst < 1.0); // stays bounded at +-25 % (inputs peak ~0.3)
        CHECK (pMean < 0.5);
        prev = size;
    }
    FormantAccent fa;
    fa.setDepth (std::numeric_limits<double>::quiet_NaN()); fa.setDepth (0.9); fa.setDepth (-1); // clamped, never NaN
}

TEST (strike_length_sets_the_accent_duration)
{
    // One accent per length: active time = length + up to one 2 ms coefficient update; peak at ~3 ms
    // whatever the length; no clicks and pitch kept with the longest, strongest setting.
    const double sr = 48000;
    auto x = vowel (sr, 180.0, 700.0, 0.8);
    for (double len : { 0.025, 0.050, 0.100 })
    {
        FormantAccent fa; fa.prepare (sr); fa.setEnabled (true); fa.setDepth (0.25); fa.setLength (len);
        std::vector<float> y = x;
        int active = 0, peakAt = 0; double peak = 0;
        for (size_t n = 0; n < y.size(); ++n)
        {
            if (n == size_t (0.3 * sr)) fa.trigger (+1);
            if (n == size_t (0.3 * sr) + 48) fa.setLength (len == 0.1 ? 0.025 : 0.1); // ignored by the running accent
            float* io[1] { y.data() + n };
            fa.process (io, 1, 1);
            if (fa.isActive()) { ++active; if (std::abs (fa.accent()) > peak) { peak = std::abs (fa.accent()); peakAt = active; } }
        }
        double maxStepX = 0, maxStepY = 0;
        for (size_t n = 1; n < x.size(); ++n)
        {
            maxStepX = std::max (maxStepX, double (std::abs (x[n] - x[n - 1])));
            maxStepY = std::max (maxStepY, double (std::abs (y[n] - y[n - 1])));
        }
        const double ms = 1000.0 * active / sr;
        std::printf ("  length %3.0f ms: active %.1f ms, peak at %.1f ms; max step in %.3f out %.3f\n", 1000 * len, ms, 1000.0 * peakAt / sr, maxStepX, maxStepY);
        CHECK (ms >= 1000 * len && ms <= 1000 * len + 2.1);
        CHECK_NEAR (1000.0 * peakAt / sr, 3.0, 0.2);
        CHECK (maxStepY < 1.5 * maxStepX);
    }
    FormantAccent fa;
    fa.setLength (std::numeric_limits<double>::quiet_NaN()); fa.setLength (5.0); fa.setLength (-1); // clamped, never NaN
}

TEST (strike_is_seamless_and_bounded)
{
    const double sr = 48000;
    auto x = vowel (sr, 180.0, 700.0, 1.0);
    int longest = 0;
    // Rapid retriggers in alternating directions (worst case), then silence after.
    for (double depth : { 0.04, 0.25 })
    {
    auto y = runAccent (x, sr, { { 0.30, +1 }, { 0.31, -1 }, { 0.318, +1 }, { 0.6, -1 } }, &longest, depth);
    double maxStepX = 0, maxStepY = 0, eX = 0, eY = 0, maxDiffOutside = 0;
    for (size_t n = 1; n < x.size(); ++n)
    {
        CHECK (std::isfinite (y[n]));
        maxStepX = std::max (maxStepX, double (std::abs (x[n] - x[n - 1])));
        maxStepY = std::max (maxStepY, double (std::abs (y[n] - y[n - 1])));
        const double t = double (n) / sr;
        if ((t > 0.30 && t < 0.35) || (t > 0.6 && t < 0.63)) { eX += double (x[n]) * x[n]; eY += double (y[n]) * y[n]; }
        if (t < 0.30 || (t > 0.36 && t < 0.6) || t > 0.64) maxDiffOutside = std::max (maxDiffOutside, double (std::abs (y[n] - x[n])));
    }
    const double levelDb = 10 * std::log10 (eY / eX);
    std::printf ("  +-%.0f %%: max step in %.4f out %.4f; level during accents %+.2f dB; max diff outside accents %.2g; longest active %.1f ms\n",
                 100 * depth, maxStepX, maxStepY, levelDb, maxDiffOutside, 1000.0 * longest / sr);
    CHECK (maxStepY < 1.3 * maxStepX);        // no clicks
    CHECK (std::abs (levelDb) < 1.0);         // no level bump
    CHECK (maxDiffOutside == 0.0);            // bit-exact passthrough between accents
    CHECK (longest < int (0.050 * sr));       // accent ends (25 ms + coefficient ramp + retriggers)
    }
    // Disabled: triggers are ignored, output is the input.
    FormantAccent off; off.prepare (sr); off.setLength (0.025);
    std::vector<float> z = x;
    for (size_t n = 0; n < z.size(); n += 32) { if (n == 9600) off.trigger (1); float* io[1] { z.data() + n }; off.process (io, 1, 32); }
    CHECK (z == x);
}

TEST (strike_in_chain_triggers_on_note_changes_only)
{
    // Sung notes A3 -> C4 -> G3, a 100 ms gap, then E4: three note changes, the onset after the gap is not one.
    const double sr = 48000;
    auto noteAt = [] (double t) { return t < 0.4 ? 57.0 : t < 0.8 ? 60.0 : t < 1.2 ? 55.0 : t < 1.3 ? -1.0 : 64.0; };
    std::vector<float> L (size_t (sr * 1.7));
    double ph = 0;
    for (size_t n = 0; n < L.size(); ++n)
    {
        const double m = noteAt (double (n) / sr);
        if (m < 0) { L[n] = 0; continue; }
        ph += 2 * kPi * midiToFreq (m + 0.2) / sr;
        double v = 0; for (int h = 1; h <= 8; ++h) v += std::sin (h * ph) / h;
        L[n] = float (0.25 * v);
    }
    std::vector<float> R = L;
    TunerChain chain; chain.prepare (sr, 70.0);
    TunerChain::Settings s; s.correction.speedMs = 0; s.strike = true; s.minFreq = 100;
    CHECK (s.strikeDepth == 0.075 && s.strikeLength == 0.080); // shipped defaults (also in Parameters.h)
    chain.setSettings (s); chain.reset();
    int starts = 0; bool was = false; std::vector<double> dirs; dirs.reserve (16);
    const long before = gAllocs.load();
    for (size_t n = 0; n + 32 <= L.size(); n += 32)
    {
        float* io[2] { L.data() + n, R.data() + n };
        chain.setSettings (s);
        chain.process (io, 2, 32);
        const bool on = chain.formantAccent().isActive();
        if (on && ! was) ++starts;
        if (on && dirs.size() < size_t (starts)) dirs.push_back (0);
        if (on) dirs.back() += chain.formantAccent().accent();
        was = on;
    }
    CHECK (gAllocs.load() == before);
    std::printf ("  accents: %d, directions:", starts);
    for (double d : dirs) std::printf (" %s", d < 0 ? "formants-down" : "formants-up");
    std::printf ("\n");
    CHECK (starts == 2);                                   // A3->C4, C4->G3; the E4 onset after silence is not a change
    CHECK (dirs.size() == 2 && dirs[0] < 0 && dirs[1] > 0); // up -> darker, down -> brighter
}

TEST (chain_rt_no_alloc_and_switching)
{
    const double sr = 48000;
    TunerChain chain; chain.prepare (sr, 70.0);
    std::vector<float> L (sr), R (sr);
    for (size_t n = 0; n < L.size(); ++n) { L[n] = harmonicTone (int (n), sr, midiToFreq (57.4), false); R[n] = 0.8f * L[n]; }
    TunerChain::Settings s; s.correction.speedMs = 0;
    const long before = gAllocs.load();
    double maxStep = 0;
    float prevL = 0;
    int b = 0;
    for (size_t n = 0; n + 64 <= L.size(); n += 64, ++b)
    {
        s.bypass = (b / 40) % 2 == 1;                  // toggle bypass every ~53 ms
        s.minFreq = (b / 100) % 2 ? 150.0 : 100.0;     // switch range every ~133 ms
        if (b == 300) s.correction.speedMs = -3;
        if (b == 500) { s.correction.stabilizerMs = 80; s.correction.sustain = 0.7; s.correction.speedMs = 30; }
        if (b == 550) { s.doubler.enabled = true; s.doubler.blend = 0.6f; s.hq = true; }
        if (b == 650) s.hq = false;
        if (b == 600) s.strike = true;
        chain.setSettings (s);
        float* io[2] { L.data() + n, R.data() + n };
        chain.process (io, 2, 64);
        for (size_t i = n; i < n + 64; ++i)
        {
            CHECK (std::isfinite (L[i]));
            const double st = b > 8 ? std::abs (L[i] - prevL) : 0.0; // skip the abrupt test-signal onset
            maxStep = std::max (maxStep, st); prevL = L[i];
        }
    }
    CHECK (gAllocs.load() == before);
    // Input's own max step is ~0.09 (and the signal's onset is skipped); a click is several times larger.
    std::printf ("  max sample step with bypass/range switching: %.3f\n", maxStep);
    CHECK (maxStep < 0.15);
}

int main() { return th::runAll(); }
