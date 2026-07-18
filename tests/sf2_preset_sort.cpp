#include <catch2/catch_test_macros.hpp>

#include "engine/multisample/Sf2PresetSort.h"

#include <vector>

// Mirrors the shape of DuskMultisampleProcessor::Sf2PresetInfo without
// pulling in the processor's JUCE deps - sortSf2PresetsForDisplay is a
// template, so this POD instantiates the exact ordering the SF2 program
// switcher uses.
namespace
{
struct Preset
{
    int sourceIndex;
    int bank;
    int program;
};

std::vector<int> sourceOrder (const std::vector<Preset>& v)
{
    std::vector<int> out;
    out.reserve (v.size());
    for (const auto& p : v)
        out.push_back (p.sourceIndex);
    return out;
}
} // namespace

using duskstudio::sortSf2PresetsForDisplay;

TEST_CASE ("SF2 presets sort by program, then bank (variants grouped)", "[sf2][preset]")
{
    // PHDR (source) order is deliberately unsorted. Program 5 has two bank
    // variants that must end up adjacent, not split across the list.
    std::vector<Preset> presets {
        { /*src*/ 0, /*bank*/ 1, /*prog*/ 5 },   // Picked Bass 2
        { /*src*/ 1, /*bank*/ 0, /*prog*/ 8 },   // some prog-8 sound
        { /*src*/ 2, /*bank*/ 0, /*prog*/ 5 },   // Picked Bass 1
        { /*src*/ 3, /*bank*/ 0, /*prog*/ 2 },   // a prog-2 sound
    };

    sortSf2PresetsForDisplay (presets);

    // Expected: prog2, then prog5[bank0], prog5[bank1], then prog8.
    REQUIRE (sourceOrder (presets) == std::vector<int> { 3, 2, 0, 1 });
}

TEST_CASE ("SF2 sort keeps percussion (bank 128) after melodic", "[sf2][preset]")
{
    // Program 0 exists in both bank 0 (Grand Piano) and bank 128 (a drum kit).
    // The kit must NOT interleave as a "variant" of the piano - percussion
    // sorts to the end regardless of its program number.
    std::vector<Preset> presets {
        { /*src*/ 0, /*bank*/ 128, /*prog*/ 0 },  // Standard Kit
        { /*src*/ 1, /*bank*/ 0,   /*prog*/ 40 }, // a high melodic program
        { /*src*/ 2, /*bank*/ 0,   /*prog*/ 0 },  // Grand Piano
    };

    sortSf2PresetsForDisplay (presets);

    // Melodic first (prog0 piano, prog40), percussion kit last.
    REQUIRE (sourceOrder (presets) == std::vector<int> { 2, 1, 0 });
}

TEST_CASE ("SF2 preset sort breaks ties by sourceIndex", "[sf2][preset]")
{
    // Duplicate program+bank: the stable persisted ID (sourceIndex) decides,
    // ascending, so the display order is deterministic across loads.
    std::vector<Preset> presets {
        { /*src*/ 7, /*bank*/ 0, /*prog*/ 3 },
        { /*src*/ 2, /*bank*/ 0, /*prog*/ 3 },
        { /*src*/ 5, /*bank*/ 0, /*prog*/ 3 },
    };

    sortSf2PresetsForDisplay (presets);

    REQUIRE (sourceOrder (presets) == std::vector<int> { 2, 5, 7 });
}

TEST_CASE ("SF2 preset sort preserves source indices (IDs, not positions)",
           "[sf2][preset]")
{
    // The sort must reorder rows for display but never renumber sourceIndex:
    // that value is the stable ID used for loading + session persistence.
    std::vector<Preset> presets {
        { 3, 2, 1 }, { 0, 0, 0 }, { 1, 1, 4 },
    };

    sortSf2PresetsForDisplay (presets);

    // Every original sourceIndex still present exactly once.
    auto ids = sourceOrder (presets);
    std::sort (ids.begin(), ids.end());
    REQUIRE (ids == std::vector<int> { 0, 1, 3 });
}
