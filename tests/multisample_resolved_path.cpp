#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/multisample/NativeMultisampleSlot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace duskstudio;
using Catch::Matchers::WithinAbs;

namespace
{
struct SoundfontDirectory
{
    juce::File directory = juce::File::getSpecialLocation (juce::File::tempDirectory)
        .getChildFile ("dusk-resolved-soundfont-" + juce::Uuid().toString());
    SoundfontDirectory() { REQUIRE (directory.createDirectory().wasOk()); }
    ~SoundfontDirectory() { directory.deleteRecursively(); }
    std::filesystem::path path (const char* name) const
    { return std::filesystem::u8path (directory.getChildFile (name).getFullPathName().toStdString()); }
};

struct Sf2Bytes
{
    std::vector<std::uint8_t> bytes;
    void u8 (std::uint8_t value) { bytes.push_back (value); }
    void u16 (std::uint16_t value) { u8 ((std::uint8_t) value); u8 ((std::uint8_t) (value >> 8)); }
    void u32 (std::uint32_t value)
    { for (int byte = 0; byte < 4; ++byte) u8 ((std::uint8_t) (value >> (8 * byte))); }
    void id (const char* value) { for (int byte = 0; byte < 4; ++byte) u8 ((std::uint8_t) value[byte]); }
    void name (const char* value)
    {
        const auto start = bytes.size();
        bytes.resize (start + 20, 0);
        std::memcpy (bytes.data() + start, value, std::min<std::size_t> (20, std::strlen (value)));
    }
    void chunk (const char* type, const std::vector<std::uint8_t>& payload)
    {
        id (type); u32 ((std::uint32_t) payload.size());
        bytes.insert (bytes.end(), payload.begin(), payload.end());
        if (payload.size() & 1) u8 (0);
    }
};

// The same minimal two-preset layout used by multisample_state_roundtrip.
std::vector<std::uint8_t> twoPresetSoundfont()
{
    Sf2Bytes phdr;
    phdr.name ("pre0"); phdr.u16 (0); phdr.u16 (0); phdr.u16 (0);
    phdr.u32 (0); phdr.u32 (0); phdr.u32 (0);
    phdr.name ("pre1"); phdr.u16 (1); phdr.u16 (0); phdr.u16 (1);
    phdr.u32 (0); phdr.u32 (0); phdr.u32 (0);
    phdr.name ("EOP"); phdr.u16 (0); phdr.u16 (0); phdr.u16 (2);
    phdr.u32 (0); phdr.u32 (0); phdr.u32 (0);
    Sf2Bytes pbag;
    pbag.u16 (0); pbag.u16 (0); pbag.u16 (1); pbag.u16 (0); pbag.u16 (2); pbag.u16 (0);
    Sf2Bytes pgen;
    pgen.u16 (41); pgen.u16 (0); pgen.u16 (41); pgen.u16 (0); pgen.u16 (0); pgen.u16 (0);
    Sf2Bytes inst; inst.name ("inst0"); inst.u16 (0); inst.name ("EOI"); inst.u16 (1);
    Sf2Bytes ibag; ibag.u16 (0); ibag.u16 (0); ibag.u16 (2); ibag.u16 (0);
    Sf2Bytes igen; igen.u16 (43); igen.u16 (0x7f00); igen.u16 (53); igen.u16 (0);
    Sf2Bytes shdr;
    shdr.name ("samp0"); shdr.u32 (0); shdr.u32 (100); shdr.u32 (0); shdr.u32 (100);
    shdr.u32 (44100); shdr.u8 (60); shdr.u8 (0); shdr.u16 (0); shdr.u16 (1);
    shdr.name ("EOS"); shdr.u32 (0); shdr.u32 (0); shdr.u32 (0); shdr.u32 (0);
    shdr.u32 (0); shdr.u8 (0); shdr.u8 (0); shdr.u16 (0); shdr.u16 (0);
    Sf2Bytes pdta; pdta.id ("pdta");
    pdta.chunk ("phdr", phdr.bytes); pdta.chunk ("pbag", pbag.bytes); pdta.chunk ("pgen", pgen.bytes);
    pdta.chunk ("inst", inst.bytes); pdta.chunk ("ibag", ibag.bytes); pdta.chunk ("igen", igen.bytes);
    pdta.chunk ("shdr", shdr.bytes);
    Sf2Bytes sdta; sdta.id ("sdta"); sdta.chunk ("smpl", std::vector<std::uint8_t> (200, 0));
    Sf2Bytes content; content.id ("sfbk"); content.chunk ("LIST", sdta.bytes); content.chunk ("LIST", pdta.bytes);
    Sf2Bytes file; file.id ("RIFF"); file.u32 ((std::uint32_t) content.bytes.size());
    file.bytes.insert (file.bytes.end(), content.bytes.begin(), content.bytes.end());
    return file.bytes;
}

void checkSettings (const DuskMultisampleProcessor& processor)
{
    REQUIRE_THAT (processor.getOverrides().masterVolDb.load(), WithinAbs (-9.0f, 1.0e-6f));
    REQUIRE_THAT (processor.getOverrides().masterTuneCents.load(), WithinAbs (27.0f, 1.0e-6f));
    REQUIRE (processor.getOverrides().polyphony.load() == 32);
    REQUIRE_THAT (processor.getHDCC (7), WithinAbs (0.25f, 1.0e-6f));
    REQUIRE_THAT (processor.getHDCC (260), WithinAbs (0.75f, 1.0e-6f));
}

void setSettings (DuskMultisampleProcessor& processor)
{
    processor.getOverrides().masterVolDb.store (-9.0f);
    processor.getOverrides().masterTuneCents.store (27.0f);
    processor.setPolyphony (32);
    processor.setHDCC (7, 0.25f);
    processor.setHDCC (260, 0.75f);
}
} // namespace

TEST_CASE ("Resolved SFZ state path overrides an existing stale inner file and preserves controls",
           "[multisample][sfz][resolved-path]")
{
    SoundfontDirectory files;
    const auto stale = files.directory.getChildFile ("stale.sfz");
    const auto resolved = files.directory.getChildFile ("resolved.sfz");
    REQUIRE (stale.replaceWithText ("<region> key=60 sample=*sine\n"));
    REQUIRE (resolved.replaceWithText ("<region> key=64 sample=*sine\n<region> key=67 sample=*sine\n"));
    DuskMultisampleProcessor source;
    juce::String error;
    REQUIRE (source.loadSfzFile (stale, error));
    setSettings (source);
    std::vector<std::uint8_t> state;
    REQUIRE (source.saveState (state));
    DuskMultisampleProcessor ordinary;
    REQUIRE (ordinary.loadState (state));
    REQUIRE (ordinary.getLoadedPathSnapshot() == stale.getFullPathName().toStdString());
    REQUIRE (ordinary.getNumRegions() == 1);
    DuskMultisampleProcessor portable;
    REQUIRE (portable.loadStateWithResolvedFile (state, files.path ("resolved.sfz")));
    REQUIRE (portable.getLoadedPathSnapshot() == resolved.getFullPathName().toStdString());
    REQUIRE (portable.getNumRegions() == 2);
    REQUIRE (portable.getLastLoadError().isEmpty());
    checkSettings (portable);
    std::string primeError;
    auto primed = NativeMultisampleSlot::prime (files.path ("resolved.sfz"), 48000.0, 256,
                                               primeError, &state, true);
    REQUIRE (primed);
    REQUIRE_FALSE (primed.stateRestoreFailed);
    REQUIRE (primed.path == resolved.getFullPathName().toStdString());
    REQUIRE (primed.instance->getNumRegions() == 2);
    checkSettings (*primed.instance);
}

TEST_CASE ("Resolved SF2 state restores a nondefault preset after its original file moved",
           "[multisample][sf2][resolved-path]")
{
    SoundfontDirectory files;
    const auto stale = files.directory.getChildFile ("old.sf2");
    const auto resolved = files.directory.getChildFile ("moved.sf2");
    const auto fixture = twoPresetSoundfont();
    REQUIRE (stale.replaceWithData (fixture.data(), fixture.size()));
    DuskMultisampleProcessor source;
    juce::String error;
    REQUIRE (source.loadSf2File (stale, error, 1));
    setSettings (source);
    std::vector<std::uint8_t> state;
    REQUIRE (source.saveState (state));
    REQUIRE (stale.moveFileTo (resolved));
    std::string primeError;
    auto primed = NativeMultisampleSlot::prime (files.path ("moved.sf2"), 48000.0, 256,
                                               primeError, &state, true);
    REQUIRE (primed);
    REQUIRE_FALSE (primed.stateRestoreFailed);
    REQUIRE (primed.instance->getSf2PresetIndex() == 1);
    REQUIRE (primed.instance->getLoadedPathSnapshot() == resolved.getFullPathName().toStdString());
    REQUIRE (primed.instance->getLastLoadError().isEmpty());
    checkSettings (*primed.instance);
}

TEST_CASE ("Multisample commit and detach retain outgoing occupants for destruction after the gate",
           "[multisample][resolved-path][lifecycle]")
{
    SoundfontDirectory files;
    REQUIRE (files.directory.getChildFile ("first.sfz").replaceWithText ("<region> key=60 sample=*sine\n"));
    REQUIRE (files.directory.getChildFile ("second.sfz").replaceWithText ("<region> key=64 sample=*sine\n"));
    NativeMultisampleSlot slot;
    std::string error;
    auto first = NativeMultisampleSlot::prime (files.path ("first.sfz"), 48000.0, 256, error);
    REQUIRE (slot.commit (first, 256));
    auto* outgoing = slot.getInstance();
    const auto generation = slot.generation();
    int released = 0;
    auto releaseHook = std::make_shared<std::function<void()>> ([&] { ++released; });
    slot.setInstanceReleaseHook (releaseHook);
    auto second = NativeMultisampleSlot::prime (files.path ("second.sfz"), 48000.0, 256, error);
    REQUIRE_FALSE (slot.commit (second, 128));
    REQUIRE (slot.getInstance() == outgoing);
    REQUIRE (released == 0);
    REQUIRE (slot.commit (second, 256));
    REQUIRE (released == 1);
    REQUIRE (slot.generation() == generation + 1);
    REQUIRE (second.instance.get() == outgoing);
    REQUIRE (second.instance->hasLoadedRuntime());
    REQUIRE (second.instance->isActive());
    REQUIRE (second.path == files.path ("first.sfz").u8string());
    auto* newest = slot.getInstance();
    slot.setInstanceReleaseHook (releaseHook);
    auto detached = slot.detach();
    REQUIRE_FALSE (slot.isLoaded());
    REQUIRE (slot.getInstance() == nullptr);
    REQUIRE (detached.instance.get() == newest);
    REQUIRE (detached.instance->hasLoadedRuntime());
    REQUIRE (detached.path == files.path ("second.sfz").u8string());
    REQUIRE (released == 2);
    REQUIRE (slot.generation() == generation + 2);
}

TEST_CASE ("Multisample engine replacement hook keeps the live instrument playing on failure",
           "[multisample][resolved-path][replacement]")
{
    SoundfontDirectory files;
    const auto loaded = files.directory.getChildFile ("playing.sfz");
    REQUIRE (loaded.replaceWithText ("<region> key=60 sample=*sine\n"));
    DuskMultisampleProcessor processor;
    juce::String error;
    REQUIRE (processor.loadSfzFile (loaded, error));
    std::string activationError;
    REQUIRE (processor.activate (48000.0, 256, activationError));
    DuskMultisampleProcessor::ReplacementCompletion finish;
    processor.setReplacementLoader ([&] (std::filesystem::path path, int preset,
                                         DuskMultisampleProcessor::ReplacementCompletion completion)
    {
        REQUIRE (path == files.path ("missing.sfz"));
        REQUIRE (preset == -1);
        finish = std::move (completion);
    });
    int completions = 0;
    processor.loadFileAsync (files.directory.getChildFile ("missing.sfz"), [&] (bool ok, juce::String message)
    {
        REQUIRE_FALSE (ok);
        REQUIRE (message == "replacement failed");
        REQUIRE_FALSE (processor.isLoadPending());
        ++completions;
    });
    REQUIRE (processor.isLoadPending());
    REQUIRE (processor.hasLoadedRuntime());
    std::array<float, 256> left {}, right {};
    float* output[] { left.data(), right.data() };
    dusk::MidiBuffer midi;
    const std::uint8_t noteOn[] { 0x90, 60, 100 };
    midi.addEvent (noteOn, 3, 0);
    hosting::PortBuffers buffers;
    buffers.mainOut = output;
    buffers.mainOutChannels = 2;
    buffers.numFrames = 256;
    buffers.midiIn = &midi;
    processor.processBlock (buffers);
    float peak = 0.0f;
    for (float sample : left) peak = std::max (peak, std::abs (sample));
    REQUIRE (peak > 1.0e-6f);
    REQUIRE (finish);
    finish (false, "replacement failed");
    REQUIRE (completions == 1);
    REQUIRE (processor.getNumRegions() == 1);
    REQUIRE (processor.getLoadedPathSnapshot() == loaded.getFullPathName().toStdString());
}

TEST_CASE ("Accepted multisample replacement notifies its host after draining and releasing the old editor",
           "[multisample][resolved-path][replacement][lifecycle]")
{
    SoundfontDirectory files;
    REQUIRE (files.directory.getChildFile ("first.sfz").replaceWithText ("<region> key=60 sample=*sine\n"));
    REQUIRE (files.directory.getChildFile ("second.sfz").replaceWithText ("<region> key=64 sample=*sine\n"));
    NativeMultisampleSlot slot;
    std::string error;
    auto first = NativeMultisampleSlot::prime (files.path ("first.sfz"), 48000.0, 256, error);
    REQUIRE (slot.commit (first, 256));
    auto* outgoing = slot.getInstance();
    REQUIRE (outgoing != nullptr);
    DuskMultisampleProcessor::ReplacementCompletion finish;
    outgoing->setReplacementLoader ([&] (std::filesystem::path path, int preset,
                                         DuskMultisampleProcessor::ReplacementCompletion completion)
    {
        REQUIRE (path == files.path ("second.sfz"));
        REQUIRE (preset == -1);
        finish = std::move (completion);
    });
    bool editorReleased = false;
    auto release = std::make_shared<std::function<void()>> ([&] { editorReleased = true; });
    slot.setInstanceReleaseHook (release);
    int hostCompletions = 0;
    outgoing->loadFileAsync (files.directory.getChildFile ("second.sfz"), [&] (bool success, juce::String message)
    {
        REQUIRE (success);
        REQUIRE (message.isEmpty());
        REQUIRE (editorReleased);
        REQUIRE (slot.getInstance() != outgoing);
        REQUIRE_FALSE (outgoing->isLoadPending());
        ++hostCompletions;
    });
    REQUIRE (outgoing->isLoadPending());
    REQUIRE (finish);
    auto replacement = NativeMultisampleSlot::prime (files.path ("second.sfz"), 48000.0, 256, error);
    REQUIRE (replacement);
    slot.prepareReplacement();
    REQUIRE (editorReleased);
    REQUIRE (slot.commit (replacement, 256));
    REQUIRE (replacement.instance.get() == outgoing);
    finish (true, {});
    REQUIRE (hostCompletions == 1);
    REQUIRE (slot.getLoadedSoundfontPath() == files.path ("second.sfz").u8string());
}

TEST_CASE ("Multisample SF2 preset hooks preserve the retained file and ignore cancelled completions",
           "[multisample][sf2][replacement]")
{
    SoundfontDirectory files;
    const auto soundfont = files.directory.getChildFile ("presets.sf2");
    const auto fixture = twoPresetSoundfont();
    REQUIRE (soundfont.replaceWithData (fixture.data(), fixture.size()));
    DuskMultisampleProcessor processor;
    juce::String error;
    REQUIRE (processor.loadSf2File (soundfont, error));
    std::vector<DuskMultisampleProcessor::ReplacementCompletion> requests;
    processor.setReplacementLoader ([&] (std::filesystem::path path, int preset,
                                         DuskMultisampleProcessor::ReplacementCompletion completion)
    {
        REQUIRE (path == files.path ("presets.sf2"));
        REQUIRE (preset == 1);
        requests.push_back (std::move (completion));
    });
    int completions = 0;
    processor.loadSf2PresetAsync (1, [&] (bool, juce::String) { ++completions; });
    processor.cancelPendingLoads();
    processor.loadSf2PresetAsync (1, [&] (bool, juce::String) { ++completions; });
    REQUIRE (requests.size() == 2);
    requests[0] (true, {});
    REQUIRE (completions == 0);
    REQUIRE (processor.isLoadPending());
    requests[0] (false, "cancelled");
    REQUIRE (completions == 0);
    REQUIRE (processor.isLoadPending());
    requests[1] (false, "failed");
    REQUIRE (completions == 1);
    REQUIRE_FALSE (processor.isLoadPending());
    REQUIRE (processor.getSf2PresetIndex() == 0);
    REQUIRE (processor.getLoadedPathSnapshot() == soundfont.getFullPathName().toStdString());
}
