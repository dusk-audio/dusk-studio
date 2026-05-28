#include <catch2/catch_test_macros.hpp>

#include "engine/multisample/Sf2Reader.h"

#include <juce_core/juce_core.h>

namespace
{
// FluidR3 GM/GS - the standard free General MIDI bank. Fixture-gated like
// the ARIA Swirly test: runs only where the file is present, skipped
// silently elsewhere so the suite stays portable.
const juce::File kFluidR3 { "/Users/marckorte/Downloads/FluidR3_GM_GS.sf2" };
}

TEST_CASE("Sf2Reader: rejects a non-SF2 file", "[sf2]")
{
    auto tmp = juce::File::createTempFile(".sf2");
    tmp.replaceWithText("this is not a soundfont");
    auto sf = duskstudio::readSf2(tmp);
    REQUIRE_FALSE(sf.ok);
    REQUIRE(sf.error.isNotEmpty());
    tmp.deleteFile();
}

TEST_CASE("Sf2Reader: missing file errors cleanly", "[sf2]")
{
    auto sf = duskstudio::readSf2(juce::File("/no/such/file.sf2"));
    REQUIRE_FALSE(sf.ok);
    REQUIRE(sf.error.isNotEmpty());
}

TEST_CASE("Sf2Reader: parses FluidR3 GM structure", "[sf2][.fixture]")
{
    if (! kFluidR3.existsAsFile())
    {
        SUCCEED("FluidR3 fixture not present - skipping");
        return;
    }

    auto sf = duskstudio::readSf2(kFluidR3);
    REQUIRE(sf.ok);
    REQUIRE(sf.error.isEmpty());

    // GM bank: 128 melodic + percussion + GS extras. Exact count varies
    // by FluidR3 revision; assert a sane lower bound rather than ==.
    REQUIRE(sf.presets.size()     > 100);
    REQUIRE(sf.instruments.size() > 100);
    REQUIRE(sf.samples.size()     > 100);

    // sdta smpl chunk located + non-empty (16-bit PCM).
    REQUIRE(sf.smplOffset > 0);
    REQUIRE(sf.smplSize   > 0);

    // Every preset has a name and at least one zone.
    for (const auto& p : sf.presets)
    {
        REQUIRE(p.name.isNotEmpty());
        REQUIRE_FALSE(p.zones.empty());
    }

    // At least one instrument zone references a sample via the sampleID
    // generator (oper 53) - the link the SFZ converter walks.
    bool sawSampleId = false;
    for (const auto& ins : sf.instruments)
    {
        for (const auto& z : ins.zones)
            if (z.find(duskstudio::kGenSampleID) != nullptr)
            {
                sawSampleId = true;
                break;
            }
        if (sawSampleId) break;
    }
    REQUIRE(sawSampleId);

    // At least one instrument zone carries a keyRange (oper 43) with a
    // sane lo<=hi byte packing.
    bool sawKeyRange = false;
    for (const auto& ins : sf.instruments)
    {
        for (const auto& z : ins.zones)
            if (const auto* g = z.find(duskstudio::kGenKeyRange))
            {
                REQUIRE(g->lo() <= g->hi());
                sawKeyRange = true;
                break;
            }
        if (sawKeyRange) break;
    }
    REQUIRE(sawKeyRange);

    // Samples have plausible rates + the EOS sentinel was dropped.
    bool sawRealRate = false;
    for (const auto& s : sf.samples)
    {
        REQUIRE(s.name != "EOS");
        if (s.sampleRate >= 8000 && s.sampleRate <= 192000)
            sawRealRate = true;
    }
    REQUIRE(sawRealRate);
}
