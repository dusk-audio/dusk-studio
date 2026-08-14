#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/multisample/NativeMultisampleSlot.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;

TEST_CASE ("DuskMultisampleProcessor loads an SFZ file path with spaces", "[multisample][sfz]")
{
    const auto sfz = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getNonexistentChildFile ("Dusk SFZ load test", ".sfz", false);
    struct ScopedFileCleanup
    {
        juce::File file;
        ~ScopedFileCleanup() { file.deleteFile(); }
    } cleanup { sfz };

    REQUIRE (sfz.replaceWithText ("<region> key=60 sample=*sine\n"));

    duskstudio::DuskMultisampleProcessor processor;
    juce::String error;
    REQUIRE (processor.loadSfzFile (sfz, error));
    REQUIRE (error.isEmpty());
    REQUIRE (processor.hasLoadedFile());
    REQUIRE (processor.getLoadedFilePath() == sfz.getFullPathName());
    REQUIRE (processor.getNumRegions() == 1);
}

TEST_CASE ("DuskMultisampleProcessor rejects an SFZ with no resolvable samples",
           "[multisample][sfz]")
{
    const auto sfz = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getNonexistentChildFile ("Dusk SFZ missing sample", ".sfz", false);
    struct ScopedFileCleanup
    {
        juce::File file;
        ~ScopedFileCleanup() { file.deleteFile(); }
    } cleanup { sfz };

    // sfizz parses this, reports success, and then drops the region because the
    // sample is not on disk. Without the region check that reads as a soundfont
    // that loaded fine and plays nothing.
    REQUIRE (sfz.replaceWithText ("<region> key=60 sample=not-here.wav\n"));

    duskstudio::DuskMultisampleProcessor processor;
    juce::String error;
    REQUIRE_FALSE (processor.loadSfzFile (sfz, error));
    REQUIRE (error.contains ("No playable regions"));
}

TEST_CASE ("DuskMultisampleProcessor preserves its persisted path after a failed load",
           "[multisample][sfz]")
{
    const auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const auto good = tempDir.getNonexistentChildFile ("Dusk SFZ good", ".sfz", false);
    const auto missingSample = tempDir.getNonexistentChildFile ("Dusk SFZ missing", ".sfz", false);
    const auto regionless = tempDir.getNonexistentChildFile ("Dusk SFZ regionless", ".sfz", false);
    struct ScopedFileCleanup
    {
        juce::File a, b, c;
        ~ScopedFileCleanup() { a.deleteFile(); b.deleteFile(); c.deleteFile(); }
    } cleanup { good, missingSample, regionless };

    REQUIRE (good.replaceWithText ("<region> key=60 sample=*sine\n"));
    REQUIRE (missingSample.replaceWithText ("<region> key=60 sample=not-here.wav\n"));
    REQUIRE (regionless.replaceWithText (""));

    duskstudio::DuskMultisampleProcessor processor;
    juce::String error;
    REQUIRE (processor.loadSfzFile (good, error));
    REQUIRE (processor.hasLoadedFile());
    REQUIRE (processor.getLoadedPathSnapshot() == good.getFullPathName().toStdString());

    SECTION ("sfizz rejects an existing regionless file")
    {
        REQUIRE_FALSE (processor.loadSfzFile (regionless, error));
        REQUIRE (error.contains ("sfizz_load_file failed"));
        REQUIRE (processor.getNumRegions() == 0);
        REQUIRE (processor.hasLoadedFile());
        REQUIRE (processor.getLoadedFilePath() == good.getFullPathName());
        REQUIRE (processor.getLoadedPathSnapshot() == good.getFullPathName().toStdString());
        REQUIRE (processor.getLastLoadError() == error);

        processor.clearLoadedFile();
        REQUIRE_FALSE (processor.hasLoadedFile());
        REQUIRE (processor.getLoadedFilePath().isEmpty());
        REQUIRE (processor.getLoadedPathSnapshot().empty());
        REQUIRE (processor.getLastLoadError().isEmpty());
    }

    SECTION ("sfizz drops regions whose samples cannot be resolved")
    {
        REQUIRE_FALSE (processor.loadSfzFile (missingSample, error));
        REQUIRE (error.contains ("No playable regions"));
        REQUIRE (processor.getNumRegions() == 0);
        REQUIRE (processor.hasLoadedFile());
        REQUIRE (processor.getLoadedFilePath() == good.getFullPathName());
        REQUIRE (processor.getLoadedPathSnapshot() == good.getFullPathName().toStdString());
        REQUIRE (processor.getLastLoadError() == error);

        processor.clearLoadedFile();
        REQUIRE_FALSE (processor.hasLoadedFile());
        REQUIRE (processor.getLoadedFilePath().isEmpty());
        REQUIRE (processor.getLoadedPathSnapshot().empty());
        REQUIRE (processor.getLastLoadError().isEmpty());
    }

    SECTION ("a missing retained file keeps its reload error visible")
    {
        REQUIRE (good.deleteFile());
        REQUIRE_FALSE (processor.reloadCurrentFile (error));
        REQUIRE (error.contains ("File does not exist"));
        REQUIRE (processor.hasLoadedFile());
        REQUIRE (processor.getLoadedFilePath() == good.getFullPathName());
        REQUIRE (processor.getLoadedPathSnapshot() == good.getFullPathName().toStdString());
        REQUIRE (processor.getLastLoadError() == error);

        processor.clearLoadedFile();
        REQUIRE_FALSE (processor.hasLoadedFile());
        REQUIRE (processor.getLastLoadError().isEmpty());
    }
}

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

std::vector<std::uint8_t> testSf2 (bool secondPresetPlayable = true,
                                   bool includeSecondPreset = true)
{
    Sf2Buf phdr;
    phdr.name ("pre0"); phdr.u16 (0); phdr.u16 (0); phdr.u16 (0); phdr.u32 (0); phdr.u32 (0); phdr.u32 (0);
    if (includeSecondPreset)
        { phdr.name ("pre1"); phdr.u16 (1); phdr.u16 (0); phdr.u16 (1); phdr.u32 (0); phdr.u32 (0); phdr.u32 (0); }
    phdr.name ("EOP"); phdr.u16 (0); phdr.u16 (0); phdr.u16 (includeSecondPreset ? 2 : 1); phdr.u32 (0); phdr.u32 (0); phdr.u32 (0);

    Sf2Buf pbag;
    pbag.u16 (0); pbag.u16 (0);
    if (includeSecondPreset) { pbag.u16 (1); pbag.u16 (0); }
    pbag.u16 (includeSecondPreset ? 2 : 1); pbag.u16 (0);
    Sf2Buf pgen;
    pgen.u16 (41); pgen.u16 (0);
    if (includeSecondPreset)
        { pgen.u16 (41); pgen.u16 (secondPresetPlayable ? 0 : 1); }
    pgen.u16 (0);  pgen.u16 (0);

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

std::filesystem::path createUniqueTempSoundfontDirectory()
{
    std::random_device random;
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        const auto nonce = (std::uint64_t (random()) << 32) ^ std::uint64_t (random());
        const auto candidate = std::filesystem::temp_directory_path()
                             / ("dusk-multisample-" + std::to_string (nonce));
        std::error_code error;
        if (std::filesystem::create_directory (candidate, error))
            return candidate;
    }
    return {};
}

struct ScopedPathCleanup
{
    std::filesystem::path path;
    ~ScopedPathCleanup()
    {
        std::error_code ignored;
        std::filesystem::remove_all (path, ignored);
    }
};

constexpr auto kPresetFallbackText = "using preset 0";
} // namespace

// DuskMultisampleProcessor persists its file path + override params
// (master volume / tune / polyphony) via saveState + loadState. A session
// save / load that loses any of these would silently reset the user's mix
// on reopen.
TEST_CASE ("DuskMultisampleProcessor round-trips overrides through state", "[multisample]")
{
    duskstudio::DuskMultisampleProcessor a;
    auto& aov = a.getOverrides();
    aov.masterVolDb    .store (-6.0f, std::memory_order_relaxed);
    aov.masterTuneCents.store ( 25.0f, std::memory_order_relaxed);
    a.setPolyphony (32);   // message-thread-only setter

    std::vector<std::uint8_t> blob;
    REQUIRE (a.saveState (blob));
    REQUIRE (! blob.empty());

    duskstudio::DuskMultisampleProcessor b;
    REQUIRE (b.loadState (blob));
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

    std::vector<std::uint8_t> blob;
    REQUIRE (a.saveState (blob));

    duskstudio::DuskMultisampleProcessor b;
    REQUIRE (b.loadState (blob));
    const auto& bov = b.getOverrides();

    REQUIRE (bov.masterVolDb    .load() >= -60.0f);
    REQUIRE (bov.masterVolDb    .load() <=  12.0f);
    REQUIRE (bov.masterTuneCents.load() >= -100.0f);
    REQUIRE (bov.masterTuneCents.load() <=  100.0f);
    REQUIRE (bov.polyphony      .load() >=  1);
    REQUIRE (bov.polyphony      .load() <=  256);
}

// Empty / junk state (fresh processor with no save data) must not crash.
TEST_CASE ("DuskMultisampleProcessor accepts empty state", "[multisample]")
{
    duskstudio::DuskMultisampleProcessor p;
    REQUIRE_FALSE (p.loadState ({}));
    REQUIRE_FALSE (p.loadState ({ 0x00, 0x01, 0x02 }));
    REQUIRE_FALSE (p.hasLoadedFile());
}

// The blob is a plain ValueTree binary, which is what sessions on disk carry.
// A blob built by hand (i.e. written by an older build) must still restore.
TEST_CASE ("DuskMultisampleProcessor loads a legacy state blob", "[multisample]")
{
    juce::ValueTree state ("DuskMultisample");
    state.setProperty ("masterVolDb",     -3.5f, nullptr);
    state.setProperty ("masterTuneCents", -12.0f, nullptr);
    state.setProperty ("polyphony",       48,    nullptr);
    juce::ValueTree ccTree ("cc");
    juce::ValueTree e ("c");
    e.setProperty ("n", 7,     nullptr);
    e.setProperty ("v", 0.25f, nullptr);
    ccTree.appendChild (e, nullptr);
    state.appendChild (ccTree, nullptr);

    juce::MemoryOutputStream os;
    state.writeToStream (os);
    const auto* bytes = static_cast<const std::uint8_t*> (os.getData());
    const std::vector<std::uint8_t> blob (bytes, bytes + os.getDataSize());

    duskstudio::DuskMultisampleProcessor p;
    REQUIRE (p.loadState (blob));
    const auto& ov = p.getOverrides();
    REQUIRE_THAT (ov.masterVolDb    .load (std::memory_order_relaxed), WithinAbs (-3.5f,  1e-4f));
    REQUIRE_THAT (ov.masterTuneCents.load (std::memory_order_relaxed), WithinAbs (-12.0f, 1e-4f));
    REQUIRE (ov.polyphony.load (std::memory_order_relaxed) == 48);
    REQUIRE_THAT (p.getHDCC (7), WithinAbs (0.25f, 1e-4f));

    // And a fresh save of that state reproduces the same reader-visible values.
    std::vector<std::uint8_t> resaved;
    REQUIRE (p.saveState (resaved));
    duskstudio::DuskMultisampleProcessor q;
    REQUIRE (q.loadState (resaved));
    REQUIRE (q.getOverrides().polyphony.load (std::memory_order_relaxed) == 48);
}

// A legacy blob's "file" property names the soundfont to restore. A real path
// loads it; the state's overrides land on top.
TEST_CASE ("DuskMultisampleProcessor legacy blob restores its file", "[multisample]")
{
    const auto sfz = juce::File::createTempFile (".sfz");
    sfz.replaceWithText ("<region> sample=*sine");

    juce::ValueTree state ("DuskMultisample");
    state.setProperty ("file", sfz.getFullPathName(), nullptr);
    state.setProperty ("polyphony", 24, nullptr);
    juce::MemoryOutputStream os;
    state.writeToStream (os);
    const auto* b = static_cast<const std::uint8_t*> (os.getData());

    duskstudio::DuskMultisampleProcessor p;
    REQUIRE (p.loadState (std::vector<std::uint8_t> (b, b + os.getDataSize())));
    REQUIRE (p.hasLoadedFile());
    REQUIRE (p.getLoadedFilePath() == sfz.getFullPathName());
    REQUIRE (p.getLoadedPathSnapshot() == sfz.getFullPathName().toStdString());
    REQUIRE (p.getLastLoadError().isEmpty());
    REQUIRE (p.getOverrides().polyphony.load (std::memory_order_relaxed) == 24);

    sfz.deleteFile();
}

// A legacy blob naming a soundfont that is gone must fail SOFT: the error is
// surfaced for the editor, nothing is loaded, and the overrides still apply so
// the user can point the slot at a replacement without losing their settings.
TEST_CASE ("DuskMultisampleProcessor legacy blob with a missing file fails soft", "[multisample]")
{
    juce::ValueTree state ("DuskMultisample");
    state.setProperty ("file", juce::String ("/no/such/missing.sfz"), nullptr);
    state.setProperty ("masterVolDb", -9.0f, nullptr);
    juce::MemoryOutputStream os;
    state.writeToStream (os);
    const auto* b = static_cast<const std::uint8_t*> (os.getData());

    duskstudio::DuskMultisampleProcessor p;
    REQUIRE (p.loadState (std::vector<std::uint8_t> (b, b + os.getDataSize())));
    REQUIRE_FALSE (p.hasLoadedFile());
    REQUIRE (p.getLoadedPathSnapshot().empty());
    REQUIRE_FALSE (p.getLastLoadError().isEmpty());
    REQUIRE_THAT (p.getOverrides().masterVolDb.load (std::memory_order_relaxed),
                  WithinAbs (-9.0f, 1e-4f));
}

// Regression: an SF2 is already loaded, then a session restore references a
// DIFFERENT, missing SF2. The failed load must NOT let the saved preset index
// switch the still-loaded old file or clear the surfaced load error.
TEST_CASE ("DuskMultisampleProcessor: failed loadState leaves the old SF2's preset untouched", "[multisample]")
{
    duskstudio::DuskMultisampleProcessor proc;

    const auto aFile = juce::File::createTempFile (".sf2");
    const auto bytes = testSf2();
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
    const auto* stateBytes = static_cast<const std::uint8_t*> (os.getData());
    REQUIRE (proc.loadState (std::vector<std::uint8_t> (stateBytes,
                                                        stateBytes + os.getDataSize())));

    // The missing-B load failed: its error survives, and A keeps preset 0 -
    // the saved index must not have switched the still-loaded A to preset 1.
    REQUIRE_FALSE (proc.getLastLoadError().isEmpty());
    REQUIRE (proc.getSf2PresetIndex() == 0);
    REQUIRE (proc.getLoadedFilePath() == aPath);
    REQUIRE (proc.getLoadedPathSnapshot() == aPath.toStdString());

    aFile.deleteFile();
}

TEST_CASE ("DuskMultisampleProcessor: failed SFZ browse preserves the retained SF2 preset",
           "[multisample]")
{
    const auto tempDirectory = createUniqueTempSoundfontDirectory();
    REQUIRE_FALSE (tempDirectory.empty());
    const auto sf2Path = tempDirectory / "source.sf2";
    const auto brokenSfzPath = tempDirectory / "broken.sfz";
    ScopedPathCleanup cleanup { tempDirectory };

    const auto sf2Bytes = testSf2();
    {
        std::ofstream out (sf2Path, std::ios::binary);
        out.write (reinterpret_cast<const char*> (sf2Bytes.data()),
                   static_cast<std::streamsize> (sf2Bytes.size()));
        out.flush();
        REQUIRE (out.good());
    }

    duskstudio::MultisampleBundle sf2Bundle;
    std::string createError;
    REQUIRE (sf2Bundle.load (sf2Path.string(), createError));

    duskstudio::DuskMultisampleProcessor source;
    REQUIRE (source.create (sf2Bundle, {}, createError));
    auto error = source.getLastLoadError();
    REQUIRE (source.loadSf2Preset (1, error));
    REQUIRE (source.getSf2PresetIndex() == 1);

    {
        std::ofstream out (brokenSfzPath, std::ios::binary);
        out << "<region> key=60 sample=not-here.wav\n";
        out.flush();
        REQUIRE (out.good());
    }
    duskstudio::MultisampleBundle brokenSfzBundle;
    REQUIRE (brokenSfzBundle.load (brokenSfzPath.string(), createError));
    REQUIRE_FALSE (source.create (brokenSfzBundle, {}, createError));
    REQUIRE (source.getNumRegions() == 0);
    REQUIRE (source.getSf2Presets().empty());
    REQUIRE (source.getSf2PresetIndex() == -1);
    REQUIRE (source.getLoadedPathSnapshot() == sf2Path.string());

    SECTION ("state restore")
    {
        std::vector<std::uint8_t> savedState;
        REQUIRE (source.saveState (savedState));

        duskstudio::DuskMultisampleProcessor restored;
        REQUIRE (restored.loadState (savedState));
        REQUIRE (restored.getLoadedPathSnapshot() == sf2Path.string());
        REQUIRE (restored.getSf2PresetIndex() == 1);
        REQUIRE (restored.getLastLoadError().isEmpty());
    }

    SECTION ("same-processor state restore")
    {
        std::vector<std::uint8_t> savedState;
        REQUIRE (source.saveState (savedState));

        REQUIRE (source.loadState (savedState));
        REQUIRE (source.getLoadedPathSnapshot() == sf2Path.string());
        REQUIRE (source.getSf2PresetIndex() == 1);
        REQUIRE (source.getLastLoadError().isEmpty());
    }

    SECTION ("native slot prime restores the retained preset directly")
    {
        std::vector<std::uint8_t> savedState;
        REQUIRE (source.saveState (savedState));
        std::string primeError;
        auto primed = duskstudio::NativeMultisampleSlot::prime (
            sf2Path, 48000.0, 512, primeError, &savedState);

        INFO ("Prime error: " << primeError);
        REQUIRE (primed);
        REQUIRE (primed.instance->hasLoadedRuntime());
        REQUIRE (primed.instance->getLoadedPathSnapshot() == sf2Path.string());
        REQUIRE (primed.instance->getSf2PresetIndex() == 1);
        REQUIRE (primed.instance->getLastLoadError().isEmpty());
    }

    SECTION ("same-path state restore selects preset zero")
    {
        REQUIRE (source.reloadCurrentFile (error));
        REQUIRE (source.getSf2PresetIndex() == 1);

        duskstudio::DuskMultisampleProcessor presetZero;
        REQUIRE (presetZero.create (sf2Bundle, {}, createError));
        std::vector<std::uint8_t> savedState;
        REQUIRE (presetZero.saveState (savedState));

        REQUIRE (source.loadState (savedState));
        REQUIRE (source.getLoadedPathSnapshot() == sf2Path.string());
        REQUIRE (source.getSf2PresetIndex() == 0);
        REQUIRE (source.getLastLoadError().isEmpty());
    }

    SECTION ("editor reload")
    {
        REQUIRE (source.reloadCurrentFile (error));
        REQUIRE (source.getLoadedPathSnapshot() == sf2Path.string());
        REQUIRE (source.getSf2PresetIndex() == 1);
        REQUIRE (source.getLastLoadError().isEmpty());
    }
}

TEST_CASE ("DuskMultisampleProcessor: an unplayable saved SF2 preset falls back to preset zero",
           "[multisample]")
{
    const auto tempDirectory = createUniqueTempSoundfontDirectory();
    REQUIRE_FALSE (tempDirectory.empty());
    const auto sf2Path = tempDirectory / "fallback.sf2";
    ScopedPathCleanup cleanup { tempDirectory };

    const auto writeSoundfont = [&sf2Path] (const std::vector<std::uint8_t>& bytes)
    {
        std::ofstream out (sf2Path, std::ios::binary | std::ios::trunc);
        out.write (reinterpret_cast<const char*> (bytes.data()),
                   static_cast<std::streamsize> (bytes.size()));
        out.flush();
        return out.good();
    };

    const auto playableBytes = testSf2();
    REQUIRE (writeSoundfont (playableBytes));

    duskstudio::MultisampleBundle bundle;
    std::string createError;
    REQUIRE (bundle.load (sf2Path.string(), createError));

    duskstudio::DuskMultisampleProcessor source;
    REQUIRE (source.create (bundle, {}, createError));
    auto error = source.getLastLoadError();
    REQUIRE (source.loadSf2Preset (1, error));
    std::vector<std::uint8_t> savedState;
    REQUIRE (source.saveState (savedState));

    SECTION ("unplayable saved preset")
    {
        REQUIRE (writeSoundfont (testSf2 (false)));
        duskstudio::DuskMultisampleProcessor restored;
        REQUIRE (restored.loadState (savedState));
        REQUIRE (restored.getSf2PresetIndex() == 0);
        REQUIRE (restored.getNumRegions() > 0);
        REQUIRE (restored.getLastLoadError().contains (kPresetFallbackText));
    }

    SECTION ("explicit preset selection")
    {
        REQUIRE (writeSoundfont (testSf2 (false)));
        duskstudio::DuskMultisampleProcessor restored;
        REQUIRE (restored.loadState (savedState));

        error.clear();
        REQUIRE_FALSE (restored.loadSf2Preset (1, error));
        REQUIRE (restored.getSf2PresetIndex() == 0);
        REQUIRE (restored.getSf2Presets().size() == 2);
        REQUIRE (restored.getNumRegions() > 0);
        REQUIRE (restored.getLastLoadError() == error);
        REQUIRE_FALSE (error.contains (kPresetFallbackText));
    }

    SECTION ("same-path restore")
    {
        REQUIRE (writeSoundfont (testSf2 (false)));
        duskstudio::DuskMultisampleProcessor restored;
        REQUIRE (restored.loadState (savedState));

        REQUIRE (restored.loadState (savedState));
        REQUIRE (restored.getSf2PresetIndex() == 0);
        REQUIRE (restored.getNumRegions() > 0);
        REQUIRE_FALSE (restored.getLastLoadError().contains (kPresetFallbackText));
    }

    SECTION ("recovered preset persistence")
    {
        REQUIRE (writeSoundfont (testSf2 (false)));
        duskstudio::DuskMultisampleProcessor restored;
        REQUIRE (restored.loadState (savedState));

        // The failed selection must not become authoritative in the next save.
        error.clear();
        REQUIRE_FALSE (restored.loadSf2Preset (1, error));
        std::vector<std::uint8_t> fallbackState;
        REQUIRE (restored.saveState (fallbackState));
        REQUIRE (writeSoundfont (playableBytes));

        duskstudio::DuskMultisampleProcessor reopened;
        REQUIRE (reopened.loadState (fallbackState));
        REQUIRE (reopened.getSf2PresetIndex() == 0);
        REQUIRE (reopened.getLastLoadError().isEmpty());
    }

    SECTION ("out-of-range saved preset")
    {
        REQUIRE (writeSoundfont (testSf2 (true, false)));
        duskstudio::DuskMultisampleProcessor outOfRange;
        REQUIRE (outOfRange.loadState (savedState));
        REQUIRE (outOfRange.getSf2PresetIndex() == 0);
        REQUIRE (outOfRange.getNumRegions() > 0);
        REQUIRE (outOfRange.getLastLoadError().contains (kPresetFallbackText));

        duskstudio::DuskMultisampleProcessor samePathOutOfRange;
        REQUIRE (samePathOutOfRange.create (bundle, {}, createError));
        REQUIRE (samePathOutOfRange.loadState (savedState));
        REQUIRE (samePathOutOfRange.getSf2PresetIndex() == 0);
        REQUIRE (samePathOutOfRange.getNumRegions() > 0);
        REQUIRE (samePathOutOfRange.getLastLoadError().contains (kPresetFallbackText));
    }
}

TEST_CASE ("NativeMultisampleSlot prime keeps a missing-state diagnostic and loads its bundle",
           "[multisample]")
{
    const auto tempDirectory = createUniqueTempSoundfontDirectory();
    REQUIRE_FALSE (tempDirectory.empty());
    const auto statePath = tempDirectory / "missing-after-save.sfz";
    const auto bundlePath = tempDirectory / "fallback.sfz";
    ScopedPathCleanup cleanup { tempDirectory };

    for (const auto& path : { statePath, bundlePath })
    {
        std::ofstream out (path, std::ios::binary);
        out << "<region> key=60 sample=*sine\n";
        out.flush();
        REQUIRE (out.good());
    }

    duskstudio::MultisampleBundle stateBundle;
    std::string error;
    REQUIRE (stateBundle.load (statePath.string(), error));
    duskstudio::DuskMultisampleProcessor source;
    REQUIRE (source.create (stateBundle, {}, error));
    std::vector<std::uint8_t> state;
    REQUIRE (source.saveState (state));

    // The retained state path is authoritative when it names another valid
    // file, and prime must publish the same path sfizz actually loaded.
    auto mismatched = duskstudio::NativeMultisampleSlot::prime (
        bundlePath, 48000.0, 512, error, &state);
    INFO ("Prime error: " << error);
    REQUIRE (mismatched);
    REQUIRE (mismatched.instance->hasLoadedRuntime());
    REQUIRE (mismatched.path == statePath.string());
    REQUIRE (mismatched.instance->getLoadedPathSnapshot() == statePath.string());
    REQUIRE (mismatched.instance->getNumRegions() == 1);
    REQUIRE (mismatched.instance->getLastLoadError().isEmpty());

    duskstudio::NativeMultisampleSlot deferred;
    REQUIRE (deferred.load (bundlePath, 48000.0, 512, error));
    REQUIRE (deferred.loadState (state));
    REQUIRE (deferred.getPath() == statePath.string());
    REQUIRE (deferred.getLoadedSoundfontPath() == statePath.string());

    REQUIRE (std::filesystem::remove (statePath));

    auto primed = duskstudio::NativeMultisampleSlot::prime (
        bundlePath, 48000.0, 512, error, &state);
    INFO ("Prime error: " << error);
    REQUIRE (primed);
    REQUIRE (primed.instance->hasLoadedRuntime());
    REQUIRE (primed.instance->getLoadedPathSnapshot() == bundlePath.string());
    REQUIRE (primed.instance->getNumRegions() == 1);
    REQUIRE (primed.instance->getLastLoadError().contains ("File does not exist"));
}
