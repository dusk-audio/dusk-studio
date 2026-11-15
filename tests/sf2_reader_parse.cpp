#include <catch2/catch_test_macros.hpp>

#include "TestTempDirectory.h"
#include "engine/multisample/Sf2Reader.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
std::filesystem::path toPath (const juce::File& f)
{
    return std::filesystem::u8path (f.getFullPathName().toStdString());
}

// Assembles little-endian SF2 chunks so the positive path needs no bank on disk.
struct Buf
{
    std::vector<std::uint8_t> b;
    void u8  (std::uint8_t v)  { b.push_back (v); }
    void u16 (std::uint16_t v) { u8 ((std::uint8_t) (v & 0xFF)); u8 ((std::uint8_t) (v >> 8)); }
    void u32 (std::uint32_t v) { for (int i = 0; i < 4; ++i) u8 ((std::uint8_t) (v >> (8 * i))); }
    void id  (const char* s)   { for (int i = 0; i < 4; ++i) u8 ((std::uint8_t) s[i]); }
    void name (const char* s)  { char n[20] {}; std::strncpy (n, s, 20); for (int i = 0; i < 20; ++i) u8 ((std::uint8_t) n[i]); }
    void raw (const std::vector<std::uint8_t>& x) { b.insert (b.end(), x.begin(), x.end()); }
};

// id + u32 length + body, padded to even length like every RIFF chunk.
void chunk (Buf& out, const char* id, const std::vector<std::uint8_t>& body)
{
    out.id (id);
    out.u32 ((std::uint32_t) body.size());
    out.raw (body);
    if (body.size() & 1) out.u8 (0);
}

std::vector<std::uint8_t> minimalSf2()
{
    // One preset -> one instrument zone (keyRange 0..127 + sampleID 0) ->
    // one 44.1 kHz sample. Terminal EOP/EOI/EOS sentinels as the spec requires.
    Buf phdr;
    phdr.name ("pre0"); phdr.u16 (0); phdr.u16 (0); phdr.u16 (0); phdr.u32 (0); phdr.u32 (0); phdr.u32 (0);
    phdr.name ("EOP");  phdr.u16 (0); phdr.u16 (0); phdr.u16 (1); phdr.u32 (0); phdr.u32 (0); phdr.u32 (0);

    Buf pbag; pbag.u16 (0); pbag.u16 (0); pbag.u16 (1); pbag.u16 (0);
    Buf pgen; pgen.u16 (41); pgen.u16 (0);   // 41 = instrument

    Buf inst;
    inst.name ("inst0"); inst.u16 (0);
    inst.name ("EOI");   inst.u16 (1);

    Buf ibag; ibag.u16 (0); ibag.u16 (0); ibag.u16 (2); ibag.u16 (0);
    Buf igen;
    igen.u16 (43); igen.u16 (0x7F00);   // 43 = keyRange, lo=0 hi=127
    igen.u16 (53); igen.u16 (0);         // 53 = sampleID -> sample 0

    Buf shdr;
    shdr.name ("samp0"); shdr.u32 (0); shdr.u32 (100); shdr.u32 (0); shdr.u32 (100);
    shdr.u32 (44100); shdr.u8 (60); shdr.u8 (0); shdr.u16 (0); shdr.u16 (1);
    shdr.name ("EOS");  shdr.u32 (0); shdr.u32 (0);   shdr.u32 (0); shdr.u32 (0);
    shdr.u32 (0);     shdr.u8 (0);  shdr.u8 (0); shdr.u16 (0); shdr.u16 (0);

    Buf pdta; pdta.id ("pdta");
    chunk (pdta, "phdr", phdr.b); chunk (pdta, "pbag", pbag.b); chunk (pdta, "pgen", pgen.b);
    chunk (pdta, "inst", inst.b); chunk (pdta, "ibag", ibag.b); chunk (pdta, "igen", igen.b);
    chunk (pdta, "shdr", shdr.b);

    Buf sdta; sdta.id ("sdta");
    chunk (sdta, "smpl", std::vector<std::uint8_t> (200, 0));   // 100 16-bit samples

    Buf content; content.id ("sfbk");
    chunk (content, "LIST", sdta.b);
    chunk (content, "LIST", pdta.b);

    Buf file; file.id ("RIFF"); file.u32 ((std::uint32_t) content.b.size()); file.raw (content.b);
    return file.b;
}
}

TEST_CASE("Sf2Reader: rejects a non-SF2 file", "[sf2]")
{
    auto tmp = juce::File::createTempFile(".sf2");
    tmp.replaceWithText("this is not a soundfont");
    auto sf = duskstudio::readSf2(toPath(tmp));
    REQUIRE_FALSE(sf.ok);
    REQUIRE_FALSE(sf.error.empty());
    tmp.deleteFile();
}

TEST_CASE("Sf2Reader: missing file errors cleanly", "[sf2]")
{
    auto sf = duskstudio::readSf2("/no/such/file.sf2");
    REQUIRE_FALSE(sf.ok);
    REQUIRE_FALSE(sf.error.empty());
}

TEST_CASE("Sf2Reader: a file it cannot open is named in UTF-8", "[sf2]")
{
    const duskstudio::test::TempDirectory root("dusk-sf2-unicode-");
    const std::string name = u8"\u0416\u65E5\u672C.sf2";
    duskstudio::Sf2File sf;
    REQUIRE_NOTHROW(sf = duskstudio::readSf2(root.path() / std::filesystem::u8path(name)));
    REQUIRE_FALSE(sf.ok);
    CHECK(sf.error == "Could not open " + name);
}

TEST_CASE("Sf2Reader: parses a hand-built minimal SF2", "[sf2]")
{
    const auto bytes = minimalSf2();
    auto tmp = juce::File::createTempFile(".sf2");
    {
        std::ofstream os (toPath(tmp), std::ios::binary);
        os.write (reinterpret_cast<const char*>(bytes.data()), (std::streamsize) bytes.size());
    }

    auto sf = duskstudio::readSf2(toPath(tmp));
    tmp.deleteFile();

    REQUIRE(sf.ok);
    REQUIRE(sf.error.empty());
    REQUIRE(sf.presets.size()     == 1);
    REQUIRE(sf.instruments.size() == 1);
    REQUIRE(sf.samples.size()     == 1);

    // sdta smpl located via the streamed file position, PCM skipped not read.
    // Offset is exact and file-absolute: RIFF header (12) + LIST/sdta header
    // (12) + smpl id/size (8) = 32.
    REQUIRE(sf.smplOffset == 32);
    REQUIRE(sf.smplSize   == 200);

    REQUIRE(sf.presets[0].name == "pre0");
    REQUIRE_FALSE(sf.presets[0].zones.empty());
    REQUIRE(sf.samples[0].name       == "samp0");
    REQUIRE(sf.samples[0].sampleRate == 44100);

    bool sawSampleId = false, sawKeyRange = false;
    for (const auto& z : sf.instruments[0].zones)
    {
        if (z.find(duskstudio::kGenSampleID) != nullptr) sawSampleId = true;
        if (const auto* g = z.find(duskstudio::kGenKeyRange))
        {
            REQUIRE(g->lo() <= g->hi());
            sawKeyRange = true;
        }
    }
    REQUIRE(sawSampleId);
    REQUIRE(sawKeyRange);
}
