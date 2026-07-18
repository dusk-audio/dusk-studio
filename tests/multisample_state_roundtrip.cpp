#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/multisample/DuskMultisampleProcessor.h"

#include <cstdint>
#include <cstring>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{
// Hand-build a valid two-preset SF2 (both presets share one instrument zone +
// one 44.1 kHz sample) so a saved preset index of 1 is in range. Mirrors the
// builder in sf2_reader_parse.cpp; kept local so the two test TUs stay
// independent.
struct Sf2Buf
{
    std::vector<std::uint8_t> b;
    void u8   (std::uint8_t v)  { b.push_back (v); }
    void u16  (std::uint16_t v) { u8 ((std::uint8_t) (v & 0xFF)); u8 ((std::uint8_t) (v >> 8)); }
    void u32  (std::uint32_t v) { for (int i = 0; i < 4; ++i) u8 ((std::uint8_t) (v >> (8 * i))); }
    void id   (const char* s)   { for (int i = 0; i < 4; ++i) u8 ((std::uint8_t) s[i]); }
    void name (const char* s)   { char n[20] {}; std::strncpy (n, s, 20); for (int i = 0; i < 20; ++i) u8 ((std::uint8_t) n[i]); }
    void raw  (const std::vector<std::uint8_t>& x) { b.insert (b.end(), x.begin(), x.end()); }
};

void sf2Chunk (Sf2Buf& out, const char* cid, const std::vector<std::uint8_t>& body)
{
    out.id (cid);
    out.u32 ((std::uint32_t) body.size());
    out.raw (body);
    if (body.size() & 1) out.u8 (0);
}

std::vector<std::uint8_t> twoPresetSf2()
{
    Sf2Buf phdr;
    phdr.name ("pre0"); phdr.u16 (0); phdr.u16 (0); phdr.u16 (0); phdr.u32 (0); phdr.u32 (0); phdr.u32 (0);
    phdr.name ("pre1"); phdr.u16 (1); phdr.u16 (0); phdr.u16 (1); phdr.u32 (0); phdr.u32 (0); phdr.u32 (0);
    phdr.name ("EOP");  phdr.u16 (0); phdr.u16 (0); phdr.u16 (2); phdr.u32 (0); phdr.u32 (0); phdr.u32 (0);

    Sf2Buf pbag; pbag.u16 (0); pbag.u16 (0); pbag.u16 (1); pbag.u16 (0); pbag.u16 (2); pbag.u16 (0);
    Sf2Buf pgen; pgen.u16 (41); pgen.u16 (0); pgen.u16 (41); pgen.u16 (0); pgen.u16 (0); pgen.u16 (0);

    Sf2Buf inst;
    inst.name ("inst0"); inst.u16 (0);
    inst.name ("EOI");   inst.u16 (1);

    Sf2Buf ibag; ibag.u16 (0); ibag.u16 (0); ibag.u16 (2); ibag.u16 (0);
    Sf2Buf igen;
    igen.u16 (43); igen.u16 (0x7F00);   // keyRange 0..127
    igen.u16 (53); igen.u16 (0);         // sampleID 0

    Sf2Buf shdr;
    shdr.name ("samp0"); shdr.u32 (0); shdr.u32 (100); shdr.u32 (0); shdr.u32 (100);
    shdr.u32 (44100); shdr.u8 (60); shdr.u8 (0); shdr.u16 (0); shdr.u16 (1);
    shdr.name ("EOS");  shdr.u32 (0); shdr.u32 (0);   shdr.u32 (0); shdr.u32 (0);
    shdr.u32 (0);     shdr.u8 (0);  shdr.u8 (0); shdr.u16 (0); shdr.u16 (0);

    Sf2Buf pdta; pdta.id ("pdta");
    sf2Chunk (pdta, "phdr", phdr.b); sf2Chunk (pdta, "pbag", pbag.b); sf2Chunk (pdta, "pgen", pgen.b);
    sf2Chunk (pdta, "inst", inst.b); sf2Chunk (pdta, "ibag", ibag.b); sf2Chunk (pdta, "igen", igen.b);
    sf2Chunk (pdta, "shdr", shdr.b);

    Sf2Buf sdta; sdta.id ("sdta");
    sf2Chunk (sdta, "smpl", std::vector<std::uint8_t> (200, 0));

    Sf2Buf content; content.id ("sfbk");
    sf2Chunk (content, "LIST", sdta.b);
    sf2Chunk (content, "LIST", pdta.b);

    Sf2Buf file; file.id ("RIFF"); file.u32 ((std::uint32_t) content.b.size()); file.raw (content.b);
    return file.b;
}
} // namespace

// DuskMultisampleProcessor persists its file path + override params
// (master volume / tune / polyphony) via getStateInformation +
// setStateInformation. A session save / load that loses any of
// these would silently reset the user's mix on reopen.
TEST_CASE ("DuskMultisampleProcessor round-trips overrides through state", "[multisample]")
{
    duskstudio::DuskMultisampleProcessor a;
    auto& aov = a.getOverrides();
    aov.masterVolDb    .store (-6.0f, std::memory_order_relaxed);
    aov.masterTuneCents.store ( 25.0f, std::memory_order_relaxed);
    a.setPolyphony (32);   // message-thread-only setter

    juce::MemoryBlock block;
    a.getStateInformation (block);
    REQUIRE (block.getSize() > 0);

    duskstudio::DuskMultisampleProcessor b;
    b.setStateInformation (block.getData(), (int) block.getSize());
    const auto& bov = b.getOverrides();

    REQUIRE_THAT (bov.masterVolDb    .load (std::memory_order_relaxed),
                  WithinAbs (-6.0f, 1e-4f));
    REQUIRE_THAT (bov.masterTuneCents.load (std::memory_order_relaxed),
                  WithinAbs (25.0f, 1e-4f));
    REQUIRE (bov.polyphony.load (std::memory_order_relaxed) == 32);
}

// Out-of-range values in the serialised state (hand-edited
// session.json, future-version sessions) must be clamped on load
// so the audio thread can't trip on NaN / negative polyphony.
TEST_CASE ("DuskMultisampleProcessor clamps out-of-range state", "[multisample]")
{
    duskstudio::DuskMultisampleProcessor a;
    auto& aov = a.getOverrides();
    aov.masterVolDb    .store (1000.0f, std::memory_order_relaxed);   // ridiculous
    aov.masterTuneCents.store (-9999.0f, std::memory_order_relaxed);  // ditto
    a.setPolyphony (0);   // clamps inside setPolyphony

    juce::MemoryBlock block;
    a.getStateInformation (block);

    duskstudio::DuskMultisampleProcessor b;
    b.setStateInformation (block.getData(), (int) block.getSize());
    const auto& bov = b.getOverrides();

    REQUIRE (bov.masterVolDb    .load() >= -60.0f);
    REQUIRE (bov.masterVolDb    .load() <=  12.0f);
    REQUIRE (bov.masterTuneCents.load() >= -100.0f);
    REQUIRE (bov.masterTuneCents.load() <=  100.0f);
    REQUIRE (bov.polyphony      .load() >=  1);
    REQUIRE (bov.polyphony      .load() <=  256);
}

// Empty state (fresh processor with no save data) must not crash.
TEST_CASE ("DuskMultisampleProcessor accepts empty state", "[multisample]")
{
    duskstudio::DuskMultisampleProcessor p;
    p.setStateInformation (nullptr, 0);
    p.setStateInformation ("", 0);
    REQUIRE_FALSE (p.hasLoadedFile());
}

// Regression: an SF2 is already loaded, then a session restore references a
// DIFFERENT, missing SF2. The failed load must NOT let the saved preset index
// switch the still-loaded old file or clear the surfaced load error.
TEST_CASE ("DuskMultisampleProcessor: failed setState load leaves the old SF2's preset untouched", "[multisample]")
{
    duskstudio::DuskMultisampleProcessor proc;

    const auto aFile = juce::File::createTempFile (".sf2");
    const auto bytes = twoPresetSf2();
    aFile.replaceWithData (bytes.data(), bytes.size());

    juce::String loadErr;
    const bool loaded = proc.loadSf2File (aFile, loadErr);
    INFO ("SF2 load error: " << loadErr);
    REQUIRE (loaded);
    REQUIRE (proc.getSf2Presets().size() >= 2);

    // Baseline: A loaded on preset 0, no error.
    REQUIRE (proc.getSf2PresetIndex() == 0);
    REQUIRE (proc.getLastLoadError().isEmpty());
    const auto aPath = proc.getLoadedFilePath();

    // Restore a session pointing at a missing SF2 and asking for preset 1.
    juce::ValueTree state ("DuskMultisample");
    state.setProperty ("file", juce::String ("/no/such/missing-b.sf2"), nullptr);
    state.setProperty ("sf2Preset", 1, nullptr);
    juce::MemoryOutputStream os;
    state.writeToStream (os);
    proc.setStateInformation (os.getData(), (int) os.getDataSize());

    // The missing-B load failed: its error survives, and A keeps preset 0 -
    // the saved index must not have switched the still-loaded A to preset 1.
    REQUIRE_FALSE (proc.getLastLoadError().isEmpty());
    REQUIRE (proc.getSf2PresetIndex() == 0);
    REQUIRE (proc.getLoadedFilePath() == aPath);

    aFile.deleteFile();
}
