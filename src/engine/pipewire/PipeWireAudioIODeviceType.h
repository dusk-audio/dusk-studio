#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

namespace duskstudio
{
// Dusk Studio-owned native PipeWire backend. Enumerates the graph's audio
// Sink / Source nodes as individual devices (device-per-node, mirroring the
// ALSA backend's model so the existing AudioDeviceSelector UI works unchanged)
// and hands each one to a PipeWireAudioIODevice that speaks libpipewire
// directly via a single pw_filter node.
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
class PipeWireAudioIODeviceType final : public juce::AudioIODeviceType
{
public:
    PipeWireAudioIODeviceType();

    void               scanForDevices() override;
    juce::StringArray  getDeviceNames (bool wantInputNames) const override;
    int                getDefaultDeviceIndex (bool forInput) const override;
    int                getIndexOfDevice (juce::AudioIODevice* device, bool asInput) const override;
    bool               hasSeparateInputsAndOutputs() const override { return true; }
    juce::AudioIODevice* createDevice (const juce::String& outputDeviceName,
                                        const juce::String& inputDeviceName) override;

    // Re-run enumeration and notify listeners (repopulates the dropdown after a
    // node appears / disappears). Mirrors AlsaAudioIODeviceType::rescan().
    void rescan();

private:
    juce::StringArray inputNames, outputNames;
    juce::StringArray inputIds, outputIds;      // PW_KEY_NODE_NAME, index-aligned with the *Names arrays
    juce::Array<int>  inputChans, outputChans;  // audio.channels, index-aligned with the *Names arrays
    bool hasScanned = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PipeWireAudioIODeviceType)
};
} // namespace duskstudio
