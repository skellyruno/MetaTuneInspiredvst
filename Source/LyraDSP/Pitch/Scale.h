#pragma once
#include <cstdint>

namespace lyra
{
// Bit i set = pitch class i allowed (0 = C ... 11 = B).
using PitchClassMask = std::uint16_t;

enum class ScaleType { Chromatic, Major, Minor, Custom };

constexpr PitchClassMask kAllNotes = 0x0FFF;
constexpr PitchClassMask kMajorFromC = 0x0AB5; // C D E F G A B
constexpr PitchClassMask kMinorFromC = 0x05AD; // C D Eb F G Ab Bb (natural minor)

// Rotate a mask up by `semitones` (any sign): used for root selection and transposition.
constexpr PitchClassMask transposeMask (PitchClassMask m, int semitones) noexcept
{
    const int s = ((semitones % 12) + 12) % 12;
    m &= kAllNotes;
    return PitchClassMask (((m << s) | (m >> (12 - s))) & kAllNotes);
}

// `custom` is used only for ScaleType::Custom and is interpreted relative to C.
constexpr PitchClassMask scaleMask (ScaleType type, int root, PitchClassMask custom = 0) noexcept
{
    switch (type)
    {
        case ScaleType::Chromatic: return kAllNotes;
        case ScaleType::Major:     return transposeMask (kMajorFromC, root);
        case ScaleType::Minor:     return transposeMask (kMinorFromC, root);
        case ScaleType::Custom:    return PitchClassMask (custom & kAllNotes);
    }
    return kAllNotes;
}

constexpr bool isAllowed (PitchClassMask m, int pitchClass) noexcept { return (m >> pitchClass) & 1u; }
} // namespace lyra
