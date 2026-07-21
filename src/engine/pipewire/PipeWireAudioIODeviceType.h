#pragma once

#include "../device/IODeviceType.h"

#include <memory>
#include <string>
#include <vector>

namespace duskstudio
{
// Dusk Studio-owned native PipeWire backend. Enumerates the graph's audio
// Sink / Source nodes as individual devices (device-per-node, mirroring the
// ALSA backend's model so the existing AudioDeviceSelector UI works unchanged)
// and hands each one to a PipeWireAudioIODevice that speaks libpipewire
// directly via a single pw_filter node. Implements the dusk device::IODeviceType.
//
// Why native, not JUCE-JACK-over-pipewire-jack: the pipewire-jack shim adds a
// translation layer, mis-reports latency, and names our client generically.
// Talking to libpipewire directly gives correct graph latency, proper node
// naming ("Dusk Studio"), and one fewer moving part on the Linux #1 path.
//
// media.class "Audio/Sink"  -> output device (we send audio TO it)
// media.class "Audio/Source"-> input device  (we read audio FROM it)
// media.class "Audio/Duplex" contributes to BOTH lists.
//
// The node's PW_KEY_NODE_NAME is the stable identifier (used as the
// PW_KEY_TARGET_OBJECT link target at connect time); PW_KEY_NODE_DESCRIPTION
// (falling back to the name) is the human-readable dropdown label.
class PipeWireAudioIODeviceType final : public device::IODeviceType
{
public:
    PipeWireAudioIODeviceType();

    std::string        getTypeName() const override { return "PipeWire"; }
    void               scanForDevices() override;
    std::vector<std::string> getDeviceNames (bool wantInputNames) const override;
    int                getDefaultDeviceIndex (bool forInput) const override;
    int                getIndexOfDevice (device::IODevice* dev, bool asInput) const override;
    std::unique_ptr<device::IODevice> createDevice (const std::string& outputDeviceName,
                                                    const std::string& inputDeviceName) override;

private:
    std::vector<std::string> inputNames, outputNames;
    std::vector<std::string> inputIds, outputIds;   // PW_KEY_NODE_NAME, index-aligned with the *Names arrays
    std::vector<int>         inputChans, outputChans; // audio.channels, index-aligned with the *Names arrays
    bool hasScanned = false;

    PipeWireAudioIODeviceType (const PipeWireAudioIODeviceType&) = delete;
    PipeWireAudioIODeviceType& operator= (const PipeWireAudioIODeviceType&) = delete;
};
} // namespace duskstudio
