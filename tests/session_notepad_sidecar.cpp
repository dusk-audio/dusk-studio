#include <catch2/catch_test_macros.hpp>

#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

using namespace duskstudio;

namespace
{
juce::File makeTempDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-notepad-"
                                   + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    return dir;
}
} // namespace

TEST_CASE ("Notepad sidecar round-trips through the session directory",
           "[session][notepad]")
{
    const auto dir = makeTempDir();

    SECTION ("save then load returns the text")
    {
        const juce::String text ("# Verse 1\n\nsome **lyrics** here\n");
        REQUIRE (SessionSerializer::saveNotepad (dir, text));
        REQUIRE (dir.getChildFile ("notepad.md").existsAsFile());
        REQUIRE (SessionSerializer::loadNotepad (dir) == text);
    }

    SECTION ("load with no sidecar returns empty")
    {
        REQUIRE (SessionSerializer::loadNotepad (dir).isEmpty());
    }

    SECTION ("empty text with no existing file writes nothing")
    {
        REQUIRE (SessionSerializer::saveNotepad (dir, {}));
        REQUIRE (! dir.getChildFile ("notepad.md").exists());
    }

    SECTION ("empty text clears an existing sidecar's content")
    {
        REQUIRE (SessionSerializer::saveNotepad (dir, "old words"));
        REQUIRE (SessionSerializer::saveNotepad (dir, {}));
        REQUIRE (SessionSerializer::loadNotepad (dir).isEmpty());
    }

    SECTION ("save overwrites and leaves no temp file behind")
    {
        REQUIRE (SessionSerializer::saveNotepad (dir, "first"));
        REQUIRE (SessionSerializer::saveNotepad (dir, "second"));
        REQUIRE (SessionSerializer::loadNotepad (dir) == "second");
        REQUIRE (! dir.getChildFile ("notepad.md.tmp").exists());
    }

    SECTION ("invalid session directory fails without touching disk")
    {
        REQUIRE (! SessionSerializer::saveNotepad (juce::File(), "text"));
    }

    dir.deleteRecursively();
}
