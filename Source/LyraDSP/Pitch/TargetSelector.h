#pragma once
#include "Scale.h"

namespace lyra
{
// Picks the target note for a fractional-MIDI pitch, with hysteresis so a pitch hovering near the
// midpoint between two allowed notes does not flip between them. Pure logic, no audio state.
class TargetSelector
{
public:
    static constexpr int kNoTarget = -1;

    void setMask (PitchClassMask m) noexcept { mask = PitchClassMask (m & kAllNotes); }
    PitchClassMask getMask() const noexcept { return mask; }
    // How far (cents) the pitch must go past the midpoint between notes before the target switches.
    void setHysteresisCents (double c) noexcept { hysteresis = c > 0 ? c / 100.0 : 0.0; }
    void reset() noexcept { current = kNoTarget; }
    int currentTarget() const noexcept { return current; }

    // Returns the target MIDI note, or kNoTarget when the mask allows no notes.
    int select (double midi) noexcept;

    // Nearest allowed note to `midi`; ties resolve to the lower note. kNoTarget if mask is empty.
    static int nearestAllowed (double midi, PitchClassMask mask) noexcept;

private:
    PitchClassMask mask = kAllNotes;
    double hysteresis = 0.15; // semitones past the midpoint
    int current = kNoTarget;
};
} // namespace lyra
