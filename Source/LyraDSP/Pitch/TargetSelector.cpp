#include "TargetSelector.h"
#include "PitchMath.h"
#include <cmath>

namespace lyra
{
int TargetSelector::nearestAllowed (double midi, PitchClassMask m) noexcept
{
    if ((m & kAllNotes) == 0 || ! std::isfinite (midi))
        return kNoTarget;

    // Allowed notes repeat every octave, so +-11 semitones around the rounded pitch contains the nearest.
    const int centre = int (std::lround (midi));
    int best = kNoTarget;
    double bestDist = 1e9;
    for (int n = centre - 11; n <= centre + 11; ++n)
    {
        if (! isAllowed (m, pitchClassOf (n)))
            continue;
        const double d = std::abs (midi - n);
        if (d < bestDist - 1e-9) { bestDist = d; best = n; } // strict: lower note wins ties
    }
    return best;
}

int TargetSelector::select (double midi) noexcept
{
    const int candidate = nearestAllowed (midi, mask);
    if (candidate == kNoTarget)
        return current = kNoTarget;

    if (current != kNoTarget && current != candidate && isAllowed (mask, pitchClassOf (current))
        && std::abs (midi - current) <= std::abs (midi - candidate) + 2.0 * hysteresis)
        return current;

    return current = candidate;
}
} // namespace lyra
