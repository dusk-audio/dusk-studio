#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestTempDirectory.h"
#include "engine/PluginManager.h"
#include "engine/PluginSlot.h"
#include "engine/ScanProcess.h"
#include "engine/ipc/PluginScanProtocol.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined (__linux__)
 #include <csignal>
 #include <sys/types.h>
#endif

#if DUSKSTUDIO_HAS_OOP_PLUGINS && defined (DUSKSTUDIO_SPLIT_STATE_VST3_FIXTURE_PATH)
using namespace duskstudio;

namespace
{
// Parameter order and cue values of tests/fixtures/split_state_vst3.cpp.
constexpr int kGainIndex = 0;
constexpr int kHoldIndex = 1;
constexpr float kEditCue = 0.75f;
constexpr float kEditedGain = 0.5f;
constexpr int kBlockSize = 512;
constexpr const char* kHoldFileVariable = "DUSKSTUDIO_SPLIT_STATE_HOLD_FILE";

// The test binary does not host VST3 itself, so the descriptor the sandbox
// loads comes from the child's own scan of the fixture.
std::optional<PluginDescriptor> splitStateDescriptor()
{
    ScanProcess child;
    if (! child.start ({ DUSKSTUDIO_PLUGIN_HOST_PATH, "--scan", "VST3",
                         DUSKSTUDIO_SPLIT_STATE_VST3_FIXTURE_PATH }))
        return std::nullopt;
    std::string output;
    std::vector<char> chunk (4096);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto read = child.readOutput (chunk.data(), chunk.size(), 50);
        if (read.status == ScanProcess::ReadStatus::Data) output.append (chunk.data(), read.bytes);
        else if (read.status != ScanProcess::ReadStatus::Waiting) break;
    }
    const auto parsed = scanproto::parsePayload (scanproto::extractPayload (output));
    if (! parsed.has_value() || parsed->size() != 1) return std::nullopt;
    return parsed->front();
}

struct SplitState
{
    float component = -1.0f;
    float controller = -1.0f;
};

float gainFromChunk (const juce::XmlElement& state, const char* chunkName)
{
    const auto* chunk = state.getChildByName (chunkName);
    if (chunk == nullptr) return -1.0f;
    juce::MemoryBlock bytes;
    if (! bytes.fromBase64Encoding (chunk->getAllSubText().trim()) || bytes.getSize() != 4)
        return -1.0f;
    const auto* raw = static_cast<const std::uint8_t*> (bytes.getData());
    std::uint32_t bits = 0;
    for (int index = 0; index < 4; ++index)
        bits |= static_cast<std::uint32_t> (raw[index]) << (index * 8);
    float gain = 0.0f;
    std::memcpy (&gain, &bits, sizeof (gain));
    return gain;
}

std::optional<SplitState> decodeSplitState (const juce::String& stateBase64)
{
    juce::MemoryBlock blob;
    if (! blob.fromBase64Encoding (stateBase64)) return std::nullopt;
    const auto xml = juce::AudioProcessor::getXmlFromBinary (blob.getData(), (int) blob.getSize());
    if (xml == nullptr) return std::nullopt;
    return SplitState { gainFromChunk (*xml, "IComponent"), gainFromChunk (*xml, "IEditController") };
}

class ScopedEnvironment
{
public:
    ScopedEnvironment (const char* nameIn, const std::string& value) : name (nameIn)
    {
        if (const auto* existing = std::getenv (name)) previous = existing;
        set (value.c_str());
    }
    ~ScopedEnvironment() { set (previous ? previous->c_str() : nullptr); }

private:
    void set (const char* value) const
    {
       #if defined (_WIN32)
        ::_putenv_s (name, value != nullptr ? value : "");
       #else
        if (value != nullptr) ::setenv (name, value, 1);
        else                  ::unsetenv (name);
       #endif
    }

    const char* name;
    std::optional<std::string> previous;
};

struct SandboxedSplitSlot
{
    SandboxedSplitSlot()
    {
        manager.setOopEnabled (true);
        manager.setHostExecutableOverride (DUSKSTUDIO_PLUGIN_HOST_PATH, "--ipc-host");
        slot.setManager (manager);
        slot.prepareToPlay (48000.0, kBlockSize);
    }

    bool load (const PluginDescriptor& descriptor)
    {
        juce::String error;
        return slot.loadFromDescriptor (descriptor, error) && slot.isRemote();
    }

    float process (float input)
    {
        std::vector<float> left ((size_t) kBlockSize, input);
        std::vector<float> right ((size_t) kBlockSize, input);
        juce::MidiBuffer midi;
        (void) slot.processStereoBlock (left.data(), right.data(), kBlockSize, midi);
        return left.front();
    }

    PluginManager manager;
    PluginSlot slot;
};
} // namespace

// The child applies a parent's parameter write on its message thread, and a
// state capture parks the child's audio worker before that thread reads the
// state. A write queued ahead of the capture must still land in it: the child's
// message thread is held here until the worker is seen parked, so the write is
// applied only once the audio path has let go of the instance.
TEST_CASE ("A sandbox child applies queued parameter writes before it captures state", "[plugin][ipc][state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const auto descriptor = splitStateDescriptor();
    REQUIRE (descriptor.has_value());

    duskstudio::test::TempDirectory temp ("dusk-split-state-hold-");
    const auto releaseFile = temp.path() / "release";
    const ScopedEnvironment hold (kHoldFileVariable, releaseFile.u8string());

    SandboxedSplitSlot sandbox;
    REQUIRE (sandbox.load (*descriptor));

    std::atomic<bool> stop { false };
    std::atomic<bool> processing { false };
    std::atomic<bool> released { false };
    std::thread audio ([&]
    {
        while (! stop.load (std::memory_order_acquire))
        {
            if (sandbox.process (0.25f) > 0.0f)
                processing.store (true, std::memory_order_release);
            else if (processing.load (std::memory_order_acquire)
                     && ! released.load (std::memory_order_acquire))
            {
                // Silence from a live child means its worker is parked.
                std::ofstream (releaseFile) << "parked";
                released.store (true, std::memory_order_release);
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (10);
    while (! processing.load (std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    // Without a live audio path nothing would release the hold below.
    const bool started = processing.load (std::memory_order_acquire);
    if (! started)
    {
        stop.store (true, std::memory_order_release);
        audio.join();
    }
    REQUIRE (started);

    sandbox.slot.setParamNormalised (kHoldIndex, 1.0f);
    sandbox.slot.setParamNormalised (kGainIndex, 0.25f);
    std::string captureError;
    const auto state = sandbox.slot.getStateBase64ForSave (0, &captureError);
    stop.store (true, std::memory_order_release);
    audio.join();

    CHECK (released.load());
    CHECK (captureError.empty());
    const auto decoded = decodeSplitState (state);
    REQUIRE (decoded.has_value());
    CHECK_THAT (decoded->controller, Catch::Matchers::WithinAbs (0.25f, 1.0e-6f));
    CHECK_THAT (decoded->component, Catch::Matchers::WithinAbs (0.25f, 1.0e-6f));
}

// A JUCE-hosted VST3 hands a parameter change to its processor only inside the
// next process call, and nothing processes while the child captures. The
// capture has to deliver the change itself or the component chunk keeps the
// old value.
TEST_CASE ("A sandbox child flushes queued parameter changes into the processor before it captures state", "[plugin][ipc][state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const auto descriptor = splitStateDescriptor();
    REQUIRE (descriptor.has_value());
    SandboxedSplitSlot sandbox;
    REQUIRE (sandbox.load (*descriptor));

    sandbox.slot.setParamNormalised (kGainIndex, 0.375f);
    std::string captureError;
    const auto decoded = decodeSplitState (sandbox.slot.getStateBase64ForSave (0, &captureError));
    CHECK (captureError.empty());
    REQUIRE (decoded.has_value());
    CHECK_THAT (decoded->controller, Catch::Matchers::WithinAbs (0.375f, 1.0e-6f));
    CHECK_THAT (decoded->component, Catch::Matchers::WithinAbs (0.375f, 1.0e-6f));

    // Delivered once: the next block runs at the captured gain.
    CHECK_THAT (sandbox.process (0.5f), Catch::Matchers::WithinAbs (0.5f * 0.375f, 1.0e-6f));
}

// The parent cannot see a change the plug-in makes inside the child - an edit
// in its own editor, or here a processor that moves its own gain - unless the
// child reports it. Processing alone is not a change, and the combined revision
// never steps back when the slot drops a child or starts another.
TEST_CASE ("A sandboxed slot's state revision follows changes made inside the child", "[plugin][ipc][state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const auto descriptor = splitStateDescriptor();
    REQUIRE (descriptor.has_value());
    SandboxedSplitSlot sandbox;
    auto& slot = sandbox.slot;
    REQUIRE (sandbox.load (*descriptor));

    const auto generation = slot.generation();
    auto revision = slot.stateRevision();
    CHECK_THAT (sandbox.process (0.25f), Catch::Matchers::WithinAbs (0.25f, 1.0e-6f));
    CHECK (slot.stateRevision() == revision);
    CHECK_THAT (sandbox.process (kEditCue), Catch::Matchers::WithinAbs (kEditCue * kEditedGain, 1.0e-6f));
    CHECK (slot.stateRevision() > revision);
    CHECK (slot.generation() == generation);
    revision = slot.stateRevision();

    std::string captureError;
    const auto state = slot.getStateBase64ForSave (0, &captureError);
    CHECK (captureError.empty());
    const auto decoded = decodeSplitState (state);
    REQUIRE (decoded.has_value());
    CHECK_THAT (decoded->component, Catch::Matchers::WithinAbs (kEditedGain, 1.0e-6f));
    CHECK (slot.stateRevision() >= revision);
    revision = slot.stateRevision();

    slot.unload();
    CHECK (slot.stateRevision() >= revision);
    REQUIRE (sandbox.load (*descriptor));
    CHECK (slot.generation() != generation);
    CHECK (slot.stateRevision() >= revision);
    CHECK_THAT (sandbox.process (kEditCue), Catch::Matchers::WithinAbs (kEditCue * kEditedGain, 1.0e-6f));
    CHECK (slot.stateRevision() > revision);
    revision = slot.stateRevision();

    juce::String error;
    REQUIRE (slot.restoreFromSavedState (*descriptor, {}, state, error));
    REQUIRE (slot.isRemote());
    CHECK (slot.stateRevision() > revision);
    revision = slot.stateRevision();

    sandbox.manager.setOopEnabled (false);
    (void) slot.loadFromDescriptor (*descriptor, error);
    CHECK_FALSE (slot.isRemote());
    CHECK (slot.stateRevision() >= revision);
}

// A save marks the slot clean against the revision its state reflects. The child
// applies the write the capture sends, counts it, and reports the count it read
// the state at, so that write leaves nothing unsaved and a later change does.
TEST_CASE ("A sandboxed capture reports the revision the child read its state at", "[plugin][ipc][state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const auto descriptor = splitStateDescriptor();
    REQUIRE (descriptor.has_value());
    SandboxedSplitSlot sandbox;
    auto& slot = sandbox.slot;
    REQUIRE (sandbox.load (*descriptor));

    slot.setParamNormalised (kGainIndex, 0.625f);
    const auto queued = slot.stateRevision();
    std::string captureError;
    std::uint64_t captured = 0;
    const auto decoded = decodeSplitState (slot.getStateBase64ForSave (0, &captureError, &captured));
    CHECK (captureError.empty());
    REQUIRE (decoded.has_value());
    CHECK_THAT (decoded->component, Catch::Matchers::WithinAbs (0.625f, 1.0e-6f));
    CHECK (captured > queued);
    CHECK (slot.stateRevision() == captured);
    CHECK_THAT (sandbox.process (kEditCue), Catch::Matchers::WithinAbs (kEditCue * kEditedGain, 1.0e-6f));
    CHECK (slot.stateRevision() > captured);
}

// Release reads the child's state once more before the child drops its plug-in,
// and later saves serve that copy. It vouches for the revision it was read at,
// unless a write was still queued for a child that will never receive it.
TEST_CASE ("A released sandbox keeps the revision of the state it cached", "[plugin][ipc][state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const auto descriptor = splitStateDescriptor();
    REQUIRE (descriptor.has_value());
    SandboxedSplitSlot sandbox;
    auto& slot = sandbox.slot;
    REQUIRE (sandbox.load (*descriptor));
    CHECK_THAT (sandbox.process (kEditCue), Catch::Matchers::WithinAbs (kEditCue * kEditedGain, 1.0e-6f));

    bool writeQueued = false;
    SECTION ("nothing queued") {}
    SECTION ("a write still queued")
    {
        slot.setParamNormalised (kGainIndex, 0.25f);
        writeQueued = true;
    }
    slot.releaseResources();

    std::string captureError;
    std::uint64_t captured = 0;
    const auto decoded = decodeSplitState (slot.getStateBase64ForSave (0, &captureError, &captured));
    CHECK (captureError.empty());
    REQUIRE (decoded.has_value());
    CHECK_THAT (decoded->component, Catch::Matchers::WithinAbs (kEditedGain, 1.0e-6f));
    if (writeQueued) CHECK (slot.stateRevision() > captured);
    else             CHECK (slot.stateRevision() == captured);
}

// A plug-in writes its meters from inside every block. JUCE's VST3 host passes
// each one to the parameter listeners, so a session with audio running would
// never stay saved if a meter counted as an edit.
TEST_CASE ("A sandbox child does not count the meters its plug-in reports", "[plugin][ipc][state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const auto descriptor = splitStateDescriptor();
    REQUIRE (descriptor.has_value());
    SandboxedSplitSlot sandbox;
    auto& slot = sandbox.slot;
    REQUIRE (sandbox.load (*descriptor));

    const auto revision = slot.stateRevision();
    for (const float level : { 0.25f, 0.5f, 0.125f, 0.0625f })
        CHECK_THAT (sandbox.process (level), Catch::Matchers::WithinAbs (level, 1.0e-6f));
    CHECK (slot.stateRevision() == revision);

    std::string captureError;
    std::uint64_t captured = 0;
    (void) slot.getStateBase64ForSave (0, &captureError, &captured);
    CHECK (captureError.empty());
    CHECK (captured == revision);
    CHECK (sandbox.process (0.5f) > 0.0f);
    CHECK (slot.stateRevision() == captured);
}

// JUCE's VST3 host reports every restartComponent as a program change, whatever
// the flags. The fixture re-announces its latency on each activation, which a
// re-prepare for a new block size triggers: no state moved.
TEST_CASE ("A sandbox child does not count a latency restart as a state change", "[plugin][ipc][state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const auto descriptor = splitStateDescriptor();
    REQUIRE (descriptor.has_value());
    SandboxedSplitSlot sandbox;
    auto& slot = sandbox.slot;
    REQUIRE (sandbox.load (*descriptor));

    const auto revision = slot.stateRevision();
    slot.prepareToPlay (48000.0, kBlockSize / 2);
    REQUIRE (slot.isRemote());
    CHECK (slot.stateRevision() == revision);
    slot.prepareToPlay (44100.0, kBlockSize);
    REQUIRE (slot.isRemote());
    CHECK (slot.stateRevision() == revision);
}

#if defined (__linux__)
namespace
{
// The child's descriptors - its end of the control channel among them - close
// when its last thread has exited, which is when only the zombie leader is left.
bool waitUntilDefunct (int pid)
{
    const auto proc = std::filesystem::path ("/proc") / std::to_string (pid);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::ifstream stat (proc / "stat");
        std::string line;
        if (! std::getline (stat, line)) return true;
        const auto close = line.rfind (')');
        const bool zombie = close != std::string::npos && close + 2 < line.size()
                         && line[close + 2] == 'Z';
        std::error_code error;
        std::size_t threads = 0;
        for (auto it = std::filesystem::directory_iterator (proc / "task", error);
             ! error && it != std::filesystem::directory_iterator(); it.increment (error))
            ++threads;
        if (zombie && threads <= 1) return true;
        std::this_thread::yield();
    }
    return false;
}
} // namespace

// A write that never reached the child must fail the next save that reports
// errors. A capture that asks for no report - the region clone's snapshot - must
// not swallow it on the way.
TEST_CASE ("A sandboxed write the child never received survives a capture without an error report", "[plugin][ipc][state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const auto descriptor = splitStateDescriptor();
    REQUIRE (descriptor.has_value());
    SandboxedSplitSlot sandbox;
    auto& slot = sandbox.slot;
    REQUIRE (sandbox.load (*descriptor));

    const int pid = slot.getRemoteChildPid();
    REQUIRE (pid > 0);
    REQUIRE (::kill ((pid_t) pid, SIGKILL) == 0);
    REQUIRE (waitUntilDefunct (pid));

    slot.setParamNormalised (kGainIndex, 0.25f);
    (void) slot.getStateBase64ForSave (0);
    std::string captureError;
    (void) slot.getStateBase64ForSave (0, &captureError);
    CHECK (captureError == "a parameter change could not be sent to the sandboxed plug-in");
}
#endif
#endif
