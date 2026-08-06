#include <catch2/catch_test_macros.hpp>

#include "ui/NotepadTheme.h"

namespace notepad = duskstudio::notepad;

namespace
{
void checkLegible (const notepad::Palette& p)
{
    // Lyrics carry the reading load, so they clear AAA body text. Chords have
    // to read at arm's length from a chair, which is why they sit well above
    // the 4.5:1 floor rather than at it.
    CHECK (notepad::contrastRatio (p.lyric, p.stage) >= 12.0);
    CHECK (notepad::contrastRatio (p.chord, p.stage) >= 7.0);
    CHECK (notepad::contrastRatio (p.muted, p.stage) >= 4.5);
    CHECK (notepad::contrastRatio (p.muted, p.shell) >= 4.5);

    // The chord lane is the one bold thing in the panel: it must be plainly
    // distinct from the lyric it sits above, not just legible on its own.
    CHECK (notepad::contrastRatio (p.chord, p.lyric) >= 1.7);
}
} // namespace

TEST_CASE ("Notepad palette stays legible", "[notepad][theme]")
{
    checkLegible (notepad::kStagePalette);
}

TEST_CASE ("Notepad palette keeps the chrome and writing surface distinguishable",
           "[notepad][theme]")
{
    const auto& p = notepad::kStagePalette;
    // Chrome and writing surface must separate without a border doing the
    // work, but not so far apart that the panel reads as two windows.
    const auto separation = notepad::contrastRatio (p.shell, p.stage);
    CHECK (separation > 1.05);
    CHECK (separation < 2.0);
    CHECK (notepad::contrastRatio (p.rule, p.stage) < 2.0);
}
