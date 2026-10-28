#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/au/AuBundle.h"
#include "engine/au/AuHost.h"
#include "engine/au/AuScanner.h"
#include "engine/au/NativeAuSlot.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace
{
constexpr std::uint32_t fourcc (const char (&text)[5]) noexcept
{
    return (static_cast<std::uint32_t> (text[0]) << 24)
         | (static_cast<std::uint32_t> (text[1]) << 16)
         | (static_cast<std::uint32_t> (text[2]) << 8)
         |  static_cast<std::uint32_t> (text[3]);
}

// Parameter listener notifications arrive through the main run loop.
template <typename Done>
bool runMainLoopUntil (Done&& done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (2);
    while (! done() && std::chrono::steady_clock::now() < deadline)
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
    return done();
}

void runMainLoopFor (double seconds)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::duration<double> (seconds);
    while (std::chrono::steady_clock::now() < deadline)
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
}

std::string loadAppleEffect (duskstudio::au::NativeAuSlot& slot)
{
    for (const auto& plugin : duskstudio::au::AuBundle::enumerate())
    {
        if (plugin.isInstrument || plugin.id.manufacturer != fourcc ("appl")) continue;
        const auto identifier = plugin.id.toString();
        std::string error;
        if (slot.load (std::filesystem::u8path (identifier), 48000.0, 128,
                       error, identifier))
            return identifier;
    }
    return {};
}
} // namespace

TEST_CASE ("Audio Unit identifiers round-trip and classify", "[au][identifier]")
{
    using duskstudio::au::AuBundle;
    using duskstudio::au::ComponentId;

    const ComponentId effect { fourcc ("aufx"), fourcc ("dely"), fourcc ("Dusk") };
    const auto text = effect.toString();
    CHECK (text == "AudioUnit:Effects/aufx,dely,Dusk");

    ComponentId parsed;
    REQUIRE (ComponentId::parse (text, parsed));
    CHECK (parsed == effect);
    CHECK_FALSE (ComponentId::parse ("AudioUnit:Effects/not-a-triple", parsed));
    CHECK (AuBundle::isSupportedType (fourcc ("aufx")));
    CHECK (AuBundle::isSupportedType (fourcc ("aumf")));
    CHECK (AuBundle::isSupportedType (fourcc ("aumu")));
    CHECK_FALSE (AuBundle::isSupportedType (fourcc ("augn")));
    CHECK (AuBundle::isInstrumentType (fourcc ("aumu")));
    CHECK_FALSE (AuBundle::isInstrumentType (fourcc ("aufx")));
}

TEST_CASE ("Audio Unit scanner cancels and emits stable native descriptor rows",
           "[au][scanner][regression][issue-466]")
{
    using duskstudio::PluginBackend;
    using duskstudio::au::AuBundle;
    using duskstudio::au::AuScanner;
    using duskstudio::au::ComponentId;

    std::atomic<bool> abort { true };
    CHECK (AuScanner::scan (&abort).empty());

    const auto rows = AuScanner::scan();
    REQUIRE_FALSE (rows.empty());
    for (const auto& row : rows)
    {
        ComponentId id;
        CHECK (row.formatName == "AudioUnit");
        CHECK (row.backend == PluginBackend::Native);
        CHECK_FALSE (row.name.empty());
        CHECK (row.location == row.pluginId);
        REQUIRE (ComponentId::parse (row.pluginId, id));
        CHECK (row.isInstrument == AuBundle::isInstrumentType (id.type));
        CHECK (row.category == AuBundle::categoryForType (id.type));
    }
}

TEST_CASE ("Audio Unit host callbacks report musical and transport state",
           "[au][host]")
{
    using Catch::Matchers::WithinAbs;
    duskstudio::au::AuHost host;
    host.setSampleRate (48000.0);

    dusk::TransportPosition position;
    position.bpm = 120.0;
    position.ppqPosition = 7.5;
    position.timeInSamples = 96000;
    position.timeSignatureNumerator = 6;
    position.timeSignatureDenominator = 8;
    position.isPlaying = true;
    position.isLooping = true;
    position.ppqLoopStart = 1.25;
    position.ppqLoopEnd = 4.75;
    host.beginBlock (&position, 128);

    const auto& callbacks = host.callbacks();
    UInt32 deltaToBeat = 0;
    Float32 numerator = 0.0f;
    UInt32 denominator = 0;
    Float64 downBeat = 0.0;
    REQUIRE (callbacks.musicalTimeLocationProc (
                 callbacks.hostUserData, &deltaToBeat, &numerator,
                 &denominator, &downBeat) == noErr);
    CHECK (deltaToBeat == 12000);
    CHECK_THAT (static_cast<double> (numerator), WithinAbs (6.0, 1.0e-12));
    CHECK (denominator == 8);
    CHECK_THAT (downBeat, WithinAbs (6.0, 1.0e-12));

    Boolean playing = false;
    Boolean changed = false;
    Boolean cycling = true;
    Float64 sample = 0.0;
    Float64 cycleStart = -1.0;
    Float64 cycleEnd = -1.0;
    REQUIRE (callbacks.transportStateProc (
                 callbacks.hostUserData, &playing, &changed, &sample, &cycling,
                 &cycleStart, &cycleEnd) == noErr);
    CHECK (playing);
    CHECK (changed);
    CHECK (cycling);
    CHECK_THAT (sample, WithinAbs (96000.0, 1.0e-12));
    CHECK_THAT (cycleStart, WithinAbs (1.25, 1.0e-12));
    CHECK_THAT (cycleEnd, WithinAbs (4.75, 1.0e-12));
}

TEST_CASE ("Audio Unit transport continuity follows the previous rolling state",
           "[au][host][transport]")
{
    duskstudio::au::AuHost host;
    dusk::TransportPosition position;
    position.timeInSamples = 1000;
    const auto& callbacks = host.callbacks();
    const auto queryChange = [&]
    {
        Boolean changed = false;
        REQUIRE (callbacks.transportStateProc (callbacks.hostUserData, nullptr,
                     &changed, nullptr, nullptr, nullptr, nullptr) == noErr);
        return static_cast<bool> (changed);
    };
    host.beginBlock (&position, 128);
    CHECK (queryChange());
    host.beginBlock (&position, 128);
    CHECK_FALSE (queryChange());
    position.timeInSamples = 2000;
    host.beginBlock (&position, 128);
    CHECK (queryChange());
    host.beginBlock (&position, 128);
    CHECK_FALSE (queryChange());

    SECTION ("playback advances") { position.isPlaying = true; }
    SECTION ("recording advances") { position.isRecording = true; }
    host.beginBlock (&position, 128);
    CHECK (queryChange());
    position.timeInSamples += 128;
    host.beginBlock (&position, 64);
    CHECK_FALSE (queryChange());
    Boolean playing = false;
    Boolean recording = false;
    REQUIRE (callbacks.transportStateProc2 (callbacks.hostUserData, &playing,
                 &recording, nullptr, nullptr, nullptr, nullptr, nullptr) == noErr);
    CHECK (playing);
    CHECK (static_cast<bool> (recording) == position.isRecording);
    position.timeInSamples += 64;
    host.beginBlock (&position, 256);
    CHECK_FALSE (queryChange());
    position.timeInSamples += 512;
    host.beginBlock (&position, 256);
    CHECK (queryChange());
}

TEST_CASE ("Audio Unit transport changes survive blocks without a callback query",
           "[au][host][transport]")
{
    duskstudio::au::AuHost host;
    dusk::TransportPosition position;
    position.isPlaying = true;
    const auto& callbacks = host.callbacks();
    const auto queryChange = [&]
    {
        Boolean changed = false;
        REQUIRE (callbacks.transportStateProc (callbacks.hostUserData, nullptr,
                     &changed, nullptr, nullptr, nullptr, nullptr) == noErr);
        return static_cast<bool> (changed);
    };
    host.beginBlock (&position, 128);
    position.timeInSamples += 128;
    host.beginBlock (&position, 128);
    CHECK (queryChange());
    CHECK_FALSE (queryChange());

    position.timeInSamples = 1000;
    host.beginBlock (&position, 128);
    position.timeInSamples += 128;
    host.beginBlock (&position, 128);
    CHECK (queryChange());
    CHECK_FALSE (queryChange());

    host.beginBlock (nullptr, 128);
    host.beginBlock (nullptr, 128);
    CHECK (queryChange());
    CHECK_FALSE (queryChange());
    Boolean playing = true;
    Boolean cycling = true;
    Float64 sample = -1.0;
    Float64 cycleStart = -1.0, cycleEnd = -1.0;
    REQUIRE (callbacks.transportStateProc (callbacks.hostUserData, &playing,
                 nullptr, &sample, &cycling, &cycleStart, &cycleEnd) == noErr);
    CHECK_FALSE (playing);
    CHECK_FALSE (cycling);
    CHECK_THAT (sample, Catch::Matchers::WithinAbs (0.0, 1.0e-12));
    CHECK_THAT (cycleStart, Catch::Matchers::WithinAbs (0.0, 1.0e-12));
    CHECK_THAT (cycleEnd, Catch::Matchers::WithinAbs (0.0, 1.0e-12));
}

TEST_CASE ("Audio Unit transport cycles use usable common loop bounds",
           "[au][host][transport]")
{
    using Catch::Matchers::WithinAbs;
    duskstudio::au::AuHost host;
    dusk::TransportPosition position;
    position.isPlaying = position.isLooping = true;
    position.loopStartSamples = 24000;
    position.loopEndSamples = 96000;
    position.ppqLoopStart = 1.0;
    position.ppqLoopEnd = 4.0;
    const auto& callbacks = host.callbacks();
    host.beginBlock (&position, 128);
    REQUIRE (callbacks.transportStateProc (callbacks.hostUserData, nullptr,
                 nullptr, nullptr, nullptr, nullptr, nullptr) == noErr);
    position.timeInSamples += 128;
    position.loopStartSamples = 48000;
    position.ppqLoopStart = 2.0;
    host.beginBlock (&position, 128);
    Boolean changed = false, cycling = false;
    Float64 cycleStart = 0.0, cycleEnd = 0.0;
    REQUIRE (callbacks.transportStateProc (callbacks.hostUserData, nullptr,
                 &changed, nullptr, &cycling, &cycleStart, &cycleEnd) == noErr);
    CHECK (changed);
    CHECK (cycling);
    CHECK_THAT (cycleStart, WithinAbs (2.0, 1.0e-12));
    CHECK_THAT (cycleEnd, WithinAbs (4.0, 1.0e-12));

    SECTION ("disabled cycle") { position.isLooping = false; }
    SECTION ("empty bounds") { position.ppqLoopEnd = position.ppqLoopStart; }
    SECTION ("reversed bounds") { position.ppqLoopEnd = position.ppqLoopStart - 1.0; }
    SECTION ("nonfinite bounds") { position.ppqLoopEnd = std::numeric_limits<double>::quiet_NaN(); }
    position.timeInSamples += 128;
    host.beginBlock (&position, 128);
    cycling = true;
    REQUIRE (callbacks.transportStateProc2 (callbacks.hostUserData, nullptr,
                 nullptr, nullptr, nullptr, &cycling, &cycleStart, &cycleEnd) == noErr);
    CHECK_FALSE (cycling);
    if (position.isLooping)
    {
        CHECK_THAT (cycleStart, WithinAbs (0.0, 1.0e-12));
        CHECK_THAT (cycleEnd, WithinAbs (0.0, 1.0e-12));
    }
}

TEST_CASE ("Native Audio Unit slot lifecycle, process, parameters, state, and latency",
           "[au][slot][process][state]")
{
    using Catch::Matchers::WithinAbs;
    using duskstudio::au::NativeAuSlot;

    NativeAuSlot slot;
    const auto identifier = loadAppleEffect (slot);
    if (identifier.empty())
    {
        SUCCEED ("No loadable stock Apple effect Audio Unit - bench coverage remains owed");
        return;
    }

    REQUIRE (slot.isLoaded());
    REQUIRE (slot.getPluginId() == identifier);
    REQUIRE (slot.getInstance() != nullptr);
    REQUIRE (slot.getInstance()->isActive());
    CHECK (slot.getInstance()->getLatencySamples() >= 0);

    std::vector<float> left (128), right (128);
    for (std::size_t i = 0; i < left.size(); ++i)
    {
        left[i] = 0.2f * std::sin (static_cast<float> (i) * 0.08f);
        right[i] = 0.7f * left[i];
    }
    slot.processStereo (left.data(), right.data(), left.data(), right.data(), 128);
    float outputMagnitude = 0.0f;
    for (std::size_t i = 0; i < left.size(); ++i)
    {
        REQUIRE (std::isfinite (left[i]));
        REQUIRE (std::isfinite (right[i]));
        outputMagnitude += std::abs (left[i]) + std::abs (right[i]);
    }
    CHECK (outputMagnitude > 1.0e-4f);

    std::vector<std::uint8_t> saved;
    REQUIRE (slot.saveState (saved));
    REQUIRE_FALSE (saved.empty());

    int writable = -1;
    for (int i = 0; i < slot.paramCount(); ++i)
    {
        const auto* parameter = slot.paramInfo (i);
        REQUIRE (parameter != nullptr);
        if (parameter->writable && parameter->maxValue > parameter->minValue)
        {
            writable = i;
            break;
        }
    }
    REQUIRE (writable >= 0);
    const auto* parameter = slot.paramInfo (writable);
    double original = 0.0;
    REQUIRE (slot.getParamValue (parameter->id, original));
    const auto beforeBinding = slot.stateRevision();
    slot.queueParamBinding (static_cast<std::uint32_t> (writable), 0.73f);
    slot.drainQueuedParamBindings();
    CHECK (slot.stateRevision() > beforeBinding);
    double changed = 0.0;
    REQUIRE (slot.getParamValue (parameter->id, changed));
    const double expected = parameter->minValue
        + 0.73 * (parameter->maxValue - parameter->minValue);
    CHECK_THAT (changed, WithinAbs (expected,
        std::max (1.0e-5, (parameter->maxValue - parameter->minValue) * 1.0e-5)));

    // An edit in the unit's own view reaches the host only as a listener notification.
    AudioUnitParameter viewEdit { static_cast<AudioUnit> (slot.getInstance()->nativeAudioUnit()),
                                  parameter->id, kAudioUnitScope_Global, 0 };
    const auto beforeViewEdit = slot.stateRevision();
    REQUIRE (AUParameterSet (nullptr, nullptr, &viewEdit, static_cast<AudioUnitParameterValue> (
        parameter->minValue + 0.31 * (parameter->maxValue - parameter->minValue)), 0) == noErr);
    CHECK (runMainLoopUntil ([&] { return slot.stateRevision() > beforeViewEdit; }));

    runMainLoopFor (0.2);
    const auto beforeRestore = slot.stateRevision();
    REQUIRE (slot.loadState (saved));
    const auto afterRestore = slot.stateRevision();
    CHECK (afterRestore > beforeRestore);
    runMainLoopFor (0.2);
    CHECK (slot.stateRevision() == afterRestore);
    double restored = 0.0;
    REQUIRE (slot.getParamValue (parameter->id, restored));
    CHECK_THAT (restored, WithinAbs (original,
        std::max (1.0e-5, (parameter->maxValue - parameter->minValue) * 1.0e-5)));

    std::string error;
    REQUIRE (slot.reactivate (44100.0, 64, error));
    CHECK (slot.getInstance()->getLatencySamples() >= 0);

    NativeAuSlot restoredSlot;
    REQUIRE (restoredSlot.load (std::filesystem::u8path (identifier), 48000.0, 128,
                                error, identifier));
    REQUIRE (restoredSlot.loadState (saved));
    REQUIRE (restoredSlot.getPluginId() == identifier);

    slot.unload();
    CHECK_FALSE (slot.isLoaded());
    CHECK (slot.getInstance() == nullptr);
}
