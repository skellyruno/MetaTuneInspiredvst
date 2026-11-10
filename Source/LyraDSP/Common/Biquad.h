#pragma once
#include <cmath>

namespace lyra
{
// Transposed direct form II biquad (RBJ cookbook coefficients). Audio-thread owned, no allocation.
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    void setLowpass (double sampleRate, double freq, double q) noexcept { set (sampleRate, freq, q, false); }
    void setHighpass (double sampleRate, double freq, double q) noexcept { set (sampleRate, freq, q, true); }
    void reset() noexcept { z1 = z2 = 0; }

    float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

private:
    void set (double sr, double f, double q, bool highpass) noexcept
    {
        const double w = 2.0 * 3.14159265358979323846 * f / sr;
        const double alpha = std::sin (w) / (2.0 * q), c = std::cos (w), a0 = 1.0 + alpha;
        const double k = highpass ? (1.0 + c) / 2.0 : (1.0 - c) / 2.0;
        b0 = float (k / a0);
        b1 = float ((highpass ? -2.0 * k : 2.0 * k) / a0);
        b2 = b0;
        a1 = float (-2.0 * c / a0);
        a2 = float ((1.0 - alpha) / a0);
    }
};
} // namespace lyra
