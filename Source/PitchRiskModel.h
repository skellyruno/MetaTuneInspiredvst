// V23 experimental detector-reliability model. Synthetic data only.
// Trained weights from tools/ml_risk_model.py; NOT used for note decisions.
// This header is dependency-free, allocation-free, and safe to call on audio thread.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace autotune {
struct PitchRiskModel final
{
    static float reliability (float confidence, float f0, float rawF0,
                              float alternateF0, float twoCycle) noexcept
    {
        if (!(std::isfinite(f0) && f0 >= 40.0f && f0 <= 1200.0f)) return 0.0f;
        const auto safe = [](float x) noexcept { return std::isfinite(x) && x > 0.0f; };
        const std::array<double, 6> features {
            std::clamp(std::isfinite(confidence) ? (double)confidence : 0.0, 0.0, 1.0),
            std::log2((double)f0 / 220.0),
            safe(alternateF0) ? std::clamp(std::abs(std::log2((double)f0 / alternateF0)), 0.0, 3.0) : 0.0,
            safe(rawF0) ? std::clamp(std::abs(std::log2((double)f0 / rawF0)), 0.0, 2.0) : 0.0,
            std::isfinite(twoCycle) && twoCycle >= 0.0f ? std::clamp((double)twoCycle, 0.0, 1.0) : 0.0,
            std::isfinite(twoCycle) && twoCycle >= 0.0f ? 1.0 : 0.0
        };
        constexpr std::array<double, 6> mean {{0.93650534858255463, -0.15129887170818937, 0.85588456358114096, 0.039995580162628178, 0.060278668077625101, 0.69838179827229585}};
        constexpr std::array<double, 6> scale {{0.050010217122640588, 0.79216284588645358, 0.6064893647387678, 0.16515162201492956, 0.069037002842337311, 0.45896041453948905}};
        constexpr std::array<double, 6> coefficients {{-0.14361640190769284, 0.029655455971811781, 0.46632947936436275, -0.25954939506017743, -0.7700145468563584, 0.25584575855153213}};
        double logit = 0.12980201615630427;
        for (size_t i = 0; i < features.size(); ++i)
            logit += ((features[i] - mean[i]) / scale[i]) * coefficients[i];
        const double bounded = std::clamp(logit, -50.0, 50.0);
        return (float)(1.0 / (1.0 + std::exp(-bounded)));
    }
};
}  // namespace autotune
