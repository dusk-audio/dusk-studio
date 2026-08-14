#pragma once

#include "DeviceSetup.h"

#include <optional>
#include <string>

// The per-machine audio-device state blob: which backend, output/input device,
// rate, block size and channel masks the user last chose. The app persists it to
// disk and hands it back to DeviceManager::initialise on the next launch.
//
// Dusk writes the version-1 JSON form. It also reads the legacy JUCE <DEVICESETUP>
// XML form a machine may still carry from before the de-JUCE migration, converting
// to JSON the next time the engine persists. No JUCE dependency: the reader sniffs
// the first non-space character ('{' JSON, '<' legacy XML) and decodes each with a
// small bespoke scanner rather than JUCE's XmlDocument / BigInteger. Parse
// fidelity against those is pinned A/B in the tests, where JUCE is linkable.
namespace duskstudio::device
{
struct DeviceStateBlob
{
    std::string deviceType;   // backend name, e.g. "PipeWire" / "ALSA"
    DeviceSetup setup;

    // Serialise to the version-1 JSON form (the only form Dusk writes).
    std::string toJson() const;

    // Parse either supported form. nullopt on an empty or unparseable blob - the
    // caller then behaves as a fresh machine, matching JUCE's parse-fail contract.
    static std::optional<DeviceStateBlob> parse (const std::string& blob);
};
} // namespace duskstudio::device
