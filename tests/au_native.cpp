#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/au/AuBundle.h"
#include "engine/au/AuHost.h"
#include "engine/au/AuScanner.h"
#include "engine/au/NativeAuSlot.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
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
    CHECK_FALSE (cycling); // loop bounds are not part of TransportPosition
    CHECK_THAT (sample, WithinAbs (96000.0, 1.0e-12));
    CHECK_THAT (cycleStart, WithinAbs (0.0, 1.0e-12));
    CHECK_THAT (cycleEnd, WithinAbs (0.0, 1.0e-12));
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
    slot.queueParamBinding (static_cast<std::uint32_t> (writable), 0.73f);
    slot.drainQueuedParamBindings();
    double changed = 0.0;
    REQUIRE (slot.getParamValue (parameter->id, changed));
    const double expected = parameter->minValue
        + 0.73 * (parameter->maxValue - parameter->minValue);
    CHECK_THAT (changed, WithinAbs (expected,
        std::max (1.0e-5, (parameter->maxValue - parameter->minValue) * 1.0e-5)));

    REQUIRE (slot.loadState (saved));
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
