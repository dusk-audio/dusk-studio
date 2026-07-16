#pragma once

#include <memory>
#include <string>
#include <vector>

// A family of audio devices sharing a backend (PipeWire, ALSA, and on Windows /
// macOS the wrapped JUCE natives), mirroring JUCE's AudioIODeviceType. Enumerates
// device names and constructs an IODevice for a chosen output/input pair.
namespace duskstudio::device
{
class IODevice;

class IODeviceType
{
public:
    virtual ~IODeviceType() = default;

    virtual std::string getTypeName() const = 0;

    // (Re)scan the backend for available devices; must run before getDeviceNames.
    virtual void scanForDevices() = 0;

    virtual std::vector<std::string> getDeviceNames (bool wantInputNames) const = 0;
    virtual int getDefaultDeviceIndex (bool forInput) const = 0;
    virtual int getIndexOfDevice (IODevice* device, bool asInput) const = 0;

    // Construct a device for the named output/input pair. Either name may be
    // empty (output-only / input-only). Returns null if neither resolves.
    virtual std::unique_ptr<IODevice> createDevice (const std::string& outputDeviceName,
                                                    const std::string& inputDeviceName) = 0;
};
} // namespace duskstudio::device
