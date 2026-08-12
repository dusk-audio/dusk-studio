#include <catch2/catch_test_macros.hpp>

#include "engine/multisample/AriaBank.h"

namespace
{
struct ScopedTempTree
{
    juce::File root;
    ~ScopedTempTree() { root.deleteRecursively(); }
};
} // namespace

TEST_CASE ("AriaBank preselects the program that owns the loaded SFZ",
           "[multisample][aria]")
{
    // Own directory: findBankFileNear walks upward and takes the first
    // *.bank.xml it sees, so a manifest from another test must not be in reach.
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getNonexistentChildFile ("DuskAriaBank", "", false);
    const ScopedTempTree cleanup { root };
    const auto programs = root.getChildFile ("Programs");
    REQUIRE (programs.createDirectory());

    const auto sfz = programs.getChildFile ("Grand.sfz");
    REQUIRE (sfz.replaceWithText ("<region> key=60 sample=*sine\n"));
    REQUIRE (root.getChildFile ("Test.bank.xml").replaceWithText (
        "<?xml version=\"1.0\"?>\n"
        "<AriaBank name=\"Test\" vendor=\"Dusk\">\n"
        "  <AriaProgram name=\"Grand\" gui=\"GUI/Grand.xml\">\n"
        "    <AriaElement path=\"Programs/Grand.sfz\"/>\n"
        "  </AriaProgram>\n"
        "</AriaBank>\n"));

    const auto bank = duskstudio::AriaBank::tryLoadFromSfz (sfz);
    REQUIRE (bank.has_value());
    REQUIRE (bank->programs.size() == 1);
    REQUIRE (bank->selectedIndex == 0);

    // Where the filesystem ignores case (Windows, default macOS) the manifest's
    // spelling and the path the user picked need not agree character for
    // character. File comparison follows the platform; the string compare this
    // replaced did not, and a miss silently costs the bank's custom skin.
    if (! juce::File::areFileNamesCaseSensitive())
    {
        const juce::File shouted (sfz.getFullPathName().toUpperCase());
        const auto reopened = duskstudio::AriaBank::tryLoadFromSfz (shouted);
        REQUIRE (reopened.has_value());
        REQUIRE (reopened->selectedIndex == 0);
    }
}
