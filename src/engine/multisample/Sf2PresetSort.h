#pragma once

#include <algorithm>
#include <vector>

namespace duskstudio
{
// Display order for an SF2's presets, program-first: an instrument and its
// bank "variants" stay together (bank is a MIDI variant selector, not a
// category), so program 34's Picked Bass variants list as a block rather than
// scattering one-per-bank. Order is:
//   1. melodic before percussion  (bank 128 is a drum kit, NOT a variant of
//      the same-numbered melodic program, so it must not interleave)
//   2. program ascending
//   3. bank ascending             (the variants within a program)
//   4. sourceIndex ascending      (deterministic tiebreak)
// sourceIndex is the stable ID used for loading and session persistence, so it
// must never be reordered by the sort - only the visual ordering changes.
// Templated on the preset struct so the ordering can be unit-tested against a
// plain POD with no engine or sfizz dependencies (see tests/sf2_preset_sort.cpp).
template <typename Preset>
inline void sortSf2PresetsForDisplay (std::vector<Preset>& presets)
{
    std::sort (presets.begin(), presets.end(),
        [] (const Preset& a, const Preset& b)
        {
            const bool aPerc = a.bank == 128, bPerc = b.bank == 128;
            if (aPerc != bPerc)         return ! aPerc;      // melodic first
            if (a.program != b.program) return a.program < b.program;
            if (a.bank != b.bank)       return a.bank < b.bank;
            return a.sourceIndex < b.sourceIndex;
        });
}
} // namespace duskstudio
