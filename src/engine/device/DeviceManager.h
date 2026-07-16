#pragma once

#include "DeviceSetup.h"
#include "IODevice.h"
#include "IODeviceType.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace juce { class AudioDeviceManager; }

// The engine's audio-device orchestrator, mirroring the slice of JUCE's
// AudioDeviceManager the app drives: register backends, restore/persist the
// chosen setup, open a device, and fan the RT callback out to the engine. This
// is the seam - the public API is pure dusk (IODevice / DeviceSetup / ChannelSet),
// while the implementation currently delegates to a wrapped juce::AudioDeviceManager
// and adapts JUCE's device/callback types behind it. A later phase swaps the
// backing to the native PipeWire/ALSA types with this API unchanged.
//
// Message-thread only, same contract as juce::AudioDeviceManager: construct,
// initialise, add/remove callbacks and query the device off the message thread.
// The audio thread receives blocks through the registered IODeviceCallback, never
// by calling into this class.
namespace duskstudio::device
{
class IODeviceCallback;

class DeviceManager
{
public:
    DeviceManager();
    ~DeviceManager();

    std::vector<IODeviceType*> getAvailableDeviceTypes();
    void scanAllDeviceTypes();

    // Open the persisted (or default) device. savedState is the opaque blob from a
    // previous getStateBlob() ("" = fresh machine -> default device). Returns an
    // empty string on success, else a human-readable error.
    std::string initialise (int numInputChannels, int numOutputChannels,
                            const std::string& savedState, bool selectDefaultOnFailure);

    // Serialised device configuration for per-machine persistence (opaque; the
    // caller writes it to disk and hands it back to initialise()).
    std::string getStateBlob() const;

    // A view of the live device, repointed on each call - use it, don't cache it
    // (a later call reflects a different device through the same object).
    IODevice*     getCurrentDevice();
    IODeviceType* getCurrentDeviceType();
    void          setCurrentDeviceType (const std::string& typeName, bool treatAsChosen);

    DeviceSetup getSetup() const;
    // Apply a setup. Returns an empty string on success, else an error message.
    std::string setSetup (const DeviceSetup& setup, bool treatAsChosen);

    void addCallback (IODeviceCallback* callback);
    void removeCallback (IODeviceCallback* callback);
    void closeDevice();

    // Hot-plug / device-change notification, fired on the message thread.
    void setChangeCallback (std::function<void()> onChange);

    // Escape hatch: the MIDI input layer still drives MIDI device enable/disable
    // through the same juce::AudioDeviceManager. Removed once the native MIDI
    // backend lands and juce_audio_devices unlinks.
    juce::AudioDeviceManager& juceManager();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    DeviceManager (const DeviceManager&) = delete;
    DeviceManager& operator= (const DeviceManager&) = delete;
};
} // namespace duskstudio::device
