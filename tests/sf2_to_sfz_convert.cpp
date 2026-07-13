#include <catch2/catch_test_macros.hpp>

#include "engine/multisample/Sf2ToSfz.h"
#include "foundation/Text.h"

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
    REQUIRE_FALSE(conv.error.empty());
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
    REQUIRE(conv.error.empty());
    REQUIRE_FALSE(conv.presetName.empty());

    // SFZ body has the expected scaffolding + at least one region.
    REQUIRE(dusk::text::contains(conv.sfzText, "<region>"));
    REQUIRE(dusk::text::contains(conv.sfzText, "sample="));
    REQUIRE(dusk::text::contains(conv.sfzText, "pitch_keycenter="));
    REQUIRE(dusk::text::contains(conv.sfzText, "lokey="));

    // At least one WAV was extracted into the sample dir, and each
    // sample= name in the SFZ resolves to a real file there.
    auto wavs = dir.findChildFiles(juce::File::findFiles, false, "*.wav");
    REQUIRE(wavs.size() > 0);

    // Spot-check the first region's sample resolves on disk.
    const auto regionIdx = conv.sfzText.find("sample=");
    REQUIRE(regionIdx != std::string::npos);
    const auto afterEq = conv.sfzText.substr(regionIdx + 7);
    const auto name = dusk::text::trim(afterEq.substr(0, afterEq.find(' ')));
    REQUIRE(dir.getChildFile(name).existsAsFile());

    dir.deleteRecursively();
}
