#pragma once
#include <cmath>

namespace lyra
{
// Fractional MIDI note: m = 69 + 12 * log2(f / ref). A4 = 69 at the reference frequency.
inline double freqToMidi (double freq, double refHz = 440.0) noexcept { return 69.0 + 12.0 * std::log2 (freq / refHz); }
inline double midiToFreq (double midi, double refHz = 440.0) noexcept { return refHz * std::exp2 ((midi - 69.0) / 12.0); }
// Signed cents needed to move `fromMidi` onto `toMidi`.
inline double centsError (double fromMidi, double toMidi) noexcept { return (toMidi - fromMidi) * 100.0; }
inline int pitchClassOf (int note) noexcept { return ((note % 12) + 12) % 12; }
} // namespace lyra
