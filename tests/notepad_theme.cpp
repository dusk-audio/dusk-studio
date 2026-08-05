#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

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

TEST_CASE ("Notepad palette stays legible on both surfaces", "[notepad][theme]")
{
    SECTION ("stage")
    {
        checkLegible (notepad::kStagePalette);
    }

    SECTION ("paper")
    {
        checkLegible (notepad::kPaperPalette);
    }
}

TEST_CASE ("Notepad palette keeps the surfaces distinguishable", "[notepad][theme]")
{
    for (const auto& p : { notepad::kStagePalette, notepad::kPaperPalette })
    {
        // Chrome and writing surface must separate without a border doing the
        // work, but not so far apart that the panel reads as two windows.
        const auto separation = notepad::contrastRatio (p.shell, p.stage);
        CHECK (separation > 1.05);
        CHECK (separation < 2.0);
        CHECK (notepad::contrastRatio (p.rule, p.stage) < 2.0);
    }
}

TEST_CASE ("Notepad reading steps scale monotonically around the writing size",
           "[notepad][theme]")
{
    CHECK (notepad::readingScale (0) == 1.0f);
    CHECK (notepad::readingScale (notepad::kMinReadingStep) < 1.0f);
    CHECK (notepad::readingScale (notepad::kMaxReadingStep) > 1.5f);

    for (int step = notepad::kMinReadingStep; step < notepad::kMaxReadingStep; ++step)
        CHECK (notepad::readingScale (step) < notepad::readingScale (step + 1));

    // Out-of-range steps clamp rather than running the type off the page.
    CHECK (notepad::readingScale (-9) == notepad::readingScale (notepad::kMinReadingStep));
    CHECK (notepad::readingScale (99) == notepad::readingScale (notepad::kMaxReadingStep));
}
