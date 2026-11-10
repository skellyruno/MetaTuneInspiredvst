#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace lyra
{
// LyraStrike: a short formant accent on every note change, with the pitch left untouched.
//
// Formant shift without moving the pitch (LPC envelope swap, zero latency):
//   e = A(z) x          inverse-filter the tuned signal with its own envelope (FIR lattice, k)
//   y = g * e / A_s(z)  re-filter it with the envelope of the same window read s times faster
//                       (IIR lattice, k_s); the window's spectrum, and so its formants, scale by s.
// The harmonics live in e and are never resampled, so the pitch cannot change. A and A_s come from
// the last ~20 ms of the signal itself (causal: nothing is delayed). g = sqrt(E_s / E) keeps the
// level (E = prediction error power).
// With k_s == k and equal lattice states the cascade is an exact identity even while k varies, so the
// accent starts and ends seamlessly; between accents the signal passes untouched (no CPU).
//
// Accent shape (length L = 25..100 ms, default 80): u ramps to +-1 over 3 ms (raised cosine), back to 0
// over L - 3 ms (the attack stays 3 ms at any length, so the hit stays sharp; longer L = longer tail);
// s = 1 + depth * u, depth = 1..25 % (default 7.5 %). Note up -> u = -1 (darker, larger), note down -> u = +1 (brighter,
// smaller). A retrigger ramps from wherever u is, so rapid note changes never jump.
// Coefficients are recomputed every ~2 ms and interpolated per sample (reflection coefficients
// stay in (-1, 1), so the interpolated lattice stays stable).
// Order 16 per 48 kHz (max 32) with a 100 Hz lag window: on synthetic vowels (f0 110-260 Hz) the shift
// reaches ~100 % of a true 4 % formant move; higher orders fit single harmonics and overshoot (125-160 %)
// and wobble the pitch more. Residual: the changing filter phase moves the measured pitch by ~1.5 c for
// a few ms (2.1 c vs 0.5 c without the accent) - far below audibility for that duration.
//   shortcut: tuned on synthetic vowels; revisit by ear on very high or low voices.
//
// Threading: prepare() allocates; everything else is RT-safe.
class FormantAccent
{
public:
    static constexpr int kMaxOrder = 32, kMaxChannels = 2;
    static constexpr double kDefaultDepth = 0.075, kMinDepth = 0.01, kMaxDepth = 0.25;
    static constexpr double kAttackSeconds = 0.003;
    static constexpr double kDefaultLength = 0.080, kMinLength = 0.025, kMaxLength = 0.100;
    static constexpr double kWindowSeconds = 0.020, kUpdateSeconds = 0.002;

    void prepare (double sampleRate);
    void reset() noexcept;
    void setEnabled (bool on) noexcept { enabled = on; } // a running accent always finishes
    // Formant shift at the accent's peak, as a fraction (0.04 = +-4 %); clamped to [1 %, 25 %].
    // Read at every coefficient update, so a change during an accent glides with it.
    void setDepth (double d) noexcept { depth = std::isfinite (d) ? std::clamp (d, kMinDepth, kMaxDepth) : kDefaultDepth; }
    // Accent length in seconds, clamped to [25, 100] ms. Latched at each trigger: a running accent keeps its shape.
    void setLength (double s) noexcept { length = std::isfinite (s) ? std::clamp (s, kMinLength, kMaxLength) : kDefaultLength; }
    // Note change: direction > 0 = up to a higher note, < 0 = down.
    void trigger (int direction) noexcept;
    bool isActive() const noexcept { return active; }
    double accent() const noexcept { return u; } // current -1..1 (diagnostics/tests)

    // In place, after the pitch shifter.
    void process (float* const* channels, int numChannels, int numSamples) noexcept;

    // Autocorrelation LPC of `x` (n samples, already windowed) -> reflection coefficients k[0..order).
    // stride > 1 sums every stride-th product (an unbiased, cheaper estimate at high sample rates).
    // Returns the prediction error power (0 = silent: k all 0). Pure; exposed for tests.
    static double reflection (const double* x, int n, int order, double sampleRate, double* k, int stride = 1) noexcept;

private:
    void update() noexcept;                         // new coefficient targets from the history
    void windowed (double s) noexcept;              // fills `frame` with the window read at rate s
    double envelopeAt (double seconds) const noexcept; // accent u at time since trigger

    std::vector<float> hist;
    std::vector<double> frame, hann;
    int histMask = 0, histPos = 0, win = 0, order = 16, updateLen = 96, untilUpdate = 0, stride = 1;
    double sr = 48000, depth = kDefaultDepth, length = kDefaultLength, release = kDefaultLength - kAttackSeconds;
    bool enabled = false, active = false, finishing = false; // finishing: last ramp ends at k_s == k
    double u = 0, uFrom = 0, uTarget = 0, t = 0;             // envelope; t = seconds since trigger
    // Coefficients ramp linearly from *0 to *1 over one update (analysis ka, synthesis ks).
    double ka0[kMaxOrder] {}, ka1[kMaxOrder] {}, ks0[kMaxOrder] {}, ks1[kMaxOrder] {}, gain0 = 1, gain1 = 1;
    // Lattice states per channel: g = analysis backward errors, h = synthesis backward errors.
    double g[kMaxChannels][kMaxOrder] {}, h[kMaxChannels][kMaxOrder] {};
};
} // namespace lyra
