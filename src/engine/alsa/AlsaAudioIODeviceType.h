#pragma once

#include "../device/IODeviceType.h"

#include <memory>
#include <string>
#include <vector>

namespace duskstudio
{
// Custom ALSA backend for Dusk Studio. Enumerates raw hw:CARD,DEV PCMs only - no
// plug:, default:, front:, dmix: aliases. Those route through alsa-lib's plug
// plugin, which on PipeWire systems gets intercepted by pipewire-alsa, with
// the result that "raw hardware" is anything but. The hw: PCMs talk directly
// to the kernel ALSA driver, the same path Ardour uses.
//
// The actual I/O loop lives in AlsaAudioIODevice. This type is just the factory
// and enumeration glue; createDevice wraps each device in a small owning handle
// whose destructor routes through AlsaAudioIODevice::destroyOrPark, so a device
// whose I/O thread wedged is leaked instead of destroyed under a live thread.
class AlsaAudioIODeviceType final : public device::IODeviceType
{
public:
    std::string getTypeName() const override { return "ALSA"; }

    void scanForDevices() override;
    std::vector<std::string> getDeviceNames (bool wantInputNames) const override;
    int  getDefaultDeviceIndex (bool forInput) const override;
    int  getIndexOfDevice (device::IODevice* dev, bool asInput) const override;
    std::unique_ptr<device::IODevice> createDevice (const std::string& outputDeviceName,
                                                    const std::string& inputDeviceName) override;

private:
    std::vector<std::string> inputNames, outputNames;
    std::vector<std::string> inputIds, outputIds;
    bool hasScanned = false;
};
} // namespace duskstudio
