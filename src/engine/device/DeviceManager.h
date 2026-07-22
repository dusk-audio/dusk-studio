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
// is the seam - the public API is pure dusk (IODevice / DeviceSetup / ChannelSet).
// On Linux (DeviceManager.cpp) it is a native orchestrator: it owns the
// IODeviceType list, drives IODevice open/start/stop/close itself, and fans the
// callback out directly to the native PipeWire/ALSA backends. Off Linux
// (DeviceManagerJuce.cpp) it delegates to a wrapped juce::AudioDeviceManager,
// adapting JUCE's device/callback types behind this same API.
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

    // The output device name recorded in a saved state blob (falling back to the
    // input device name), or empty. Reads the user's INTENDED device, which may
    // differ from the one initialise() actually opened on failure.
    std::string outputDeviceNameFromState (const std::string& savedState) const;

    // A view of the live device, repointed on each call - use it, don't cache it
    // (a later call reflects a different device through the same object).
    IODevice*     getCurrentDevice();
    IODeviceType* getCurrentDeviceType();
    void          setCurrentDeviceType (const std::string& typeName, bool treatAsChosen);

    DeviceSetup getSetup() const;
    // Apply a setup. Returns an empty string on success, else an error message.
    std::string setSetup (const DeviceSetup& setup, bool treatAsChosen);

    // True between a deliberate device change - a type switch, a setup change,
    // or an explicit closeDevice - and the next device coming up. All of those
    // close the current device, so a null device during that window is expected
    // rather than a disconnection, and switching backend leaves it null
    // indefinitely, until the user picks an interface from the new backend's
    // list. Whoever watches for hot-unplug must consult this before deciding a
    // null device means the hardware went away. Self clearing: dropped as soon
    // as a device starts, or as soon as a change broadcast reports a live one.
    bool isDeviceChangePending() const noexcept;

    void addCallback (IODeviceCallback* callback);
    void removeCallback (IODeviceCallback* callback);
    void closeDevice();

    // Hot-plug / device-change notification, fired on the message thread. Several
    // independent subscribers observe device changes (the engine's fallback
    // handler plus the settings UI), so listeners are keyed by an opaque owner
    // pointer - each subscriber passes `this` and removes by the same identity.
    void addChangeListener (void* owner, std::function<void()> onChange);
    void removeChangeListener (void* owner);
    // Force-fire every listener (the dusk replacement for a manual JUCE
    // sendChangeMessage()) - used after a rescan where the backend's own diff
    // check may swallow the notification.
    void notifyChange();

   #if ! defined(__linux__)
    // Off Linux the MIDI seam falls back to JUCE's MIDI device API, whose input
    // enable/callback lifecycle runs against this same manager. Linux drives the
    // native ALSA-sequencer backend and compiles this hatch out; it disappears
    // entirely when the wrapper does.
    juce::AudioDeviceManager& juceManager();
   #endif

    // Test seam (native Linux orchestrator): install a caller-supplied backend
    // set in place of the platform PipeWire/ALSA registration, so the mock suite
    // can drive open/start/stop/close ordering and fan-out with no real device.
    // Call before initialise(). Off Linux it is an unused no-op (the wrapper owns
    // its own juce backends and the native mock suite is Linux-only).
    void setDeviceTypesForTest (std::vector<std::unique_ptr<IODeviceType>> types);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    DeviceManager (const DeviceManager&) = delete;
    DeviceManager& operator= (const DeviceManager&) = delete;
};
} // namespace duskstudio::device
