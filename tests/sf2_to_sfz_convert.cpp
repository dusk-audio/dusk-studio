#include <catch2/catch_test_macros.hpp>

#include "engine/multisample/Sf2ToSfz.h"
#include "foundation/Text.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

// Little-endian SF2 assembly, same shape as the builder in
// sf2_reader_parse.cpp, parameterised on the tuning / loop generators the
// converter has to translate. Kept local so the two test TUs stay independent.
struct Sf2Buf
{
    std::vector<std::uint8_t> b;
    void u8   (std::uint8_t v)  { b.push_back(v); }
    void u16  (std::uint16_t v) { u8((std::uint8_t) (v & 0xFF)); u8((std::uint8_t) (v >> 8)); }
    void s16  (int v)           { u16((std::uint16_t) (std::int16_t) v); }
    void u32  (std::uint32_t v) { for (int i = 0; i < 4; ++i) u8((std::uint8_t) (v >> (8 * i))); }
    void id   (const char* s)   { for (int i = 0; i < 4; ++i) u8((std::uint8_t) s[i]); }
    void name (const char* s)   { char n[20] {}; std::strncpy(n, s, 20); for (int i = 0; i < 20; ++i) u8((std::uint8_t) n[i]); }
    void raw  (const std::vector<std::uint8_t>& x) { b.insert(b.end(), x.begin(), x.end()); }
};

void sf2Chunk (Sf2Buf& out, const char* cid, const std::vector<std::uint8_t>& body)
{
    out.id(cid);
    out.u32((std::uint32_t) body.size());
    out.raw(body);
    if (body.size() & 1) out.u8(0);
}

struct TuningOpts
{
    int coarseTune      { 0 };   // igen 51, semitones
    int fineTune        { 0 };   // igen 52, cents
    int sampleModes     { 0 };   // igen 54
    int pitchCorrection { 0 };   // shdr, cents
};

// One preset -> one instrument zone -> one 100-frame sample looping 10..90.
std::vector<std::uint8_t> tunedSf2 (const TuningOpts& o)
{
    Sf2Buf phdr;
    phdr.name("pre0"); phdr.u16(0); phdr.u16(0); phdr.u16(0); phdr.u32(0); phdr.u32(0); phdr.u32(0);
    phdr.name("EOP");  phdr.u16(0); phdr.u16(0); phdr.u16(1); phdr.u32(0); phdr.u32(0); phdr.u32(0);

    Sf2Buf pbag; pbag.u16(0); pbag.u16(0); pbag.u16(1); pbag.u16(0);
    Sf2Buf pgen; pgen.u16(41); pgen.u16(0);   // 41 = instrument

    Sf2Buf inst;
    inst.name("inst0"); inst.u16(0);
    inst.name("EOI");   inst.u16(1);

    Sf2Buf ibag; ibag.u16(0); ibag.u16(0); ibag.u16(5); ibag.u16(0);
    Sf2Buf igen;
    igen.u16(43); igen.u16(0x7F00);        // keyRange 0..127
    igen.u16(51); igen.s16(o.coarseTune);
    igen.u16(52); igen.s16(o.fineTune);
    igen.u16(54); igen.s16(o.sampleModes);
    igen.u16(53); igen.u16(0);              // sampleID -> sample 0 (terminal)

    Sf2Buf shdr;
    shdr.name("samp0"); shdr.u32(0); shdr.u32(100); shdr.u32(10); shdr.u32(90);
    shdr.u32(44100); shdr.u8(60); shdr.u8((std::uint8_t) (std::int8_t) o.pitchCorrection);
    shdr.u16(0); shdr.u16(1);
    shdr.name("EOS");  shdr.u32(0); shdr.u32(0); shdr.u32(0); shdr.u32(0);
    shdr.u32(0);     shdr.u8(0);  shdr.u8(0); shdr.u16(0); shdr.u16(0);

    Sf2Buf pdta; pdta.id("pdta");
    sf2Chunk(pdta, "phdr", phdr.b); sf2Chunk(pdta, "pbag", pbag.b); sf2Chunk(pdta, "pgen", pgen.b);
    sf2Chunk(pdta, "inst", inst.b); sf2Chunk(pdta, "ibag", ibag.b); sf2Chunk(pdta, "igen", igen.b);
    sf2Chunk(pdta, "shdr", shdr.b);

    Sf2Buf sdta; sdta.id("sdta");
    sf2Chunk(sdta, "smpl", std::vector<std::uint8_t> (200, 0));   // 100 16-bit frames

    Sf2Buf content; content.id("sfbk");
    sf2Chunk(content, "LIST", sdta.b);
    sf2Chunk(content, "LIST", pdta.b);

    Sf2Buf file; file.id("RIFF"); file.u32((std::uint32_t) content.b.size()); file.raw(content.b);
    return file.b;
}

// Convert a synthesised SF2 and hand back just the SFZ text, cleaning up the
// temp file + extracted WAVs.
std::string convertTuned (const TuningOpts& o)
{
    const auto bytes = tunedSf2(o);
    auto sf2 = juce::File::createTempFile(".sf2");
    {
        std::ofstream os (std::filesystem::u8path(sf2.getFullPathName().toStdString()),
                          std::ios::binary);
        os.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize) bytes.size());
    }

    auto dir  = freshTempDir();
    auto conv = duskstudio::convertSf2Preset(sf2, 0, dir);
    sf2.deleteFile();
    dir.deleteRecursively();

    REQUIRE(conv.ok);
    REQUIRE(dusk::text::contains(conv.sfzText, "<region>"));
    return conv.sfzText;
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

// SFZ tune is +/-100 cents, but sample pitchCorrection + instrument fine tune +
// preset fine tune sum to as much as +/-325, so the whole semitones have to
// reach the region through transpose or the excess is silently lost.
TEST_CASE("Sf2ToSfz: folds whole semitones out of the combined fine tune", "[sf2conv]")
{
    SECTION("positive correction past a semitone")
    {
        // 1 semitone coarse + (190 + 60) cents = transpose 3, tune 50.
        const auto sfz = convertTuned({ /*coarse*/ 1, /*fine*/ 190, /*modes*/ 0,
                                        /*pitchCorrection*/ 60 });
        REQUIRE(dusk::text::contains(sfz, "transpose=3 tune=50"));
    }

    SECTION("negative correction past a semitone")
    {
        const auto sfz = convertTuned({ 0, -190, 0, -60 });
        REQUIRE(dusk::text::contains(sfz, "transpose=-2 tune=-50"));
    }

    SECTION("sub-semitone correction stays in tune alone")
    {
        const auto sfz = convertTuned({ 0, 30, 0, 12 });
        REQUIRE(dusk::text::contains(sfz, "tune=42"));
        REQUIRE_FALSE(dusk::text::contains(sfz, "transpose="));
    }

    SECTION("exact semitones leave no tune opcode")
    {
        const auto sfz = convertTuned({ 0, 200, 0, 0 });
        REQUIRE(dusk::text::contains(sfz, "transpose=2"));
        REQUIRE_FALSE(dusk::text::contains(sfz, "tune="));
    }
}

// sampleModes 3 is "loop until release, then play the tail"; loop_continuous
// would leave those release tails looping forever.
TEST_CASE("Sf2ToSfz: maps the SF2 loop modes onto their SFZ equivalents", "[sf2conv]")
{
    SECTION("sampleModes 1 loops forever")
    {
        const auto sfz = convertTuned({ 0, 0, 1, 0 });
        REQUIRE(dusk::text::contains(sfz, "loop_mode=loop_continuous"));
        REQUIRE(dusk::text::contains(sfz, "loop_start=10"));
        REQUIRE(dusk::text::contains(sfz, "loop_end=89"));
    }

    SECTION("sampleModes 3 loops until release")
    {
        const auto sfz = convertTuned({ 0, 0, 3, 0 });
        REQUIRE(dusk::text::contains(sfz, "loop_mode=loop_sustain"));
        REQUIRE_FALSE(dusk::text::contains(sfz, "loop_continuous"));
    }

    SECTION("sampleModes 0 does not loop")
    {
        const auto sfz = convertTuned({ 0, 0, 0, 0 });
        REQUIRE_FALSE(dusk::text::contains(sfz, "loop_mode="));
    }
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
