#include <catch2/catch_test_macros.hpp>

#include "engine/multisample/Sf2ToSfz.h"

#include <juce_core/juce_core.h>

namespace
{
const juce::File kFluidR3 { "/Users/marckorte/Downloads/FluidR3_GM_GS.sf2" };

juce::File freshTempDir()
{
    auto d = juce::File::getSpecialLocation(juce::File::tempDirectory)
                 .getChildFile("DuskStudioTest")
                 .getChildFile("sf2_" + juce::String::toHexString(
                     juce::Random::getSystemRandom().nextInt64()));
    d.createDirectory();
    return d;
}
}

TEST_CASE("Sf2ToSfz: missing file fails cleanly", "[sf2conv]")
{
    auto dir = freshTempDir();
    auto conv = duskstudio::convertSf2Preset(juce::File("/no/such.sf2"), 0, dir);
    REQUIRE_FALSE(conv.ok);
    REQUIRE(conv.error.isNotEmpty());
    dir.deleteRecursively();
}

TEST_CASE("Sf2ToSfz: converts FluidR3 preset 0 to SFZ + WAVs", "[sf2conv][.fixture]")
{
    if (! kFluidR3.existsAsFile())
    {
        SUCCEED("FluidR3 fixture not present - skipping");
        return;
    }

    auto dir = freshTempDir();
    auto conv = duskstudio::convertSf2Preset(kFluidR3, 0, dir);

    REQUIRE(conv.ok);
    REQUIRE(conv.error.isEmpty());
    REQUIRE(conv.presetName.isNotEmpty());

    // SFZ body has the expected scaffolding + at least one region.
    REQUIRE(conv.sfzText.contains("<region>"));
    REQUIRE(conv.sfzText.contains("sample="));
    REQUIRE(conv.sfzText.contains("pitch_keycenter="));
    REQUIRE(conv.sfzText.contains("lokey="));

    // At least one WAV was extracted into the sample dir, and each
    // sample= name in the SFZ resolves to a real file there.
    auto wavs = dir.findChildFiles(juce::File::findFiles, false, "*.wav");
    REQUIRE(wavs.size() > 0);

    // Spot-check the first region's sample resolves on disk.
    const int regionIdx = conv.sfzText.indexOf("sample=");
    REQUIRE(regionIdx >= 0);
    auto afterEq = conv.sfzText.substring(regionIdx + 7);
    const auto name = afterEq.upToFirstOccurrenceOf(" ", false, false).trim();
    REQUIRE(dir.getChildFile(name).existsAsFile());

    dir.deleteRecursively();
}
