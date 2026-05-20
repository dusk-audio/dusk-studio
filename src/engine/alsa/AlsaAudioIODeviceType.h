#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

namespace duskstudio
{
// Custom ALSA backend for Focal. Enumerates raw hw:CARD,DEV PCMs only - no
// plug:, default:, front:, dmix: aliases. Those route through alsa-lib's plug
// plugin, which on PipeWire systems gets intercepted by pipewire-alsa, with
// the result that "raw hardware" is anything but. The hw: PCMs talk directly
// to the kernel ALSA driver, the same path Ardour uses.
//
// The actual I/O loop lives in AlsaAudioIODevice (Phase 2), wrapping
// zita-alsa-pcmi (vendored from Ardour). This type is just the factory and
// enumeration glue.
class AlsaAudioIODeviceType final : public juce::AudioIODeviceType
{
public:
    AlsaAudioIODeviceType();

    void               scanForDevices() override;
    juce::StringArray  getDeviceNames (bool wantInputNames) const override;
    int                getDefaultDeviceIndex (bool forInput) const override;
    int                getIndexOfDevice (juce::AudioIODevice* device, bool asInput) const override;
    bool               hasSeparateInputsAndOutputs() const override { return true; }
    juce::AudioIODevice* createDevice (const juce::String& outputDeviceName,
                                        const juce::String& inputDeviceName) override;

    // Re-run device enumeration and notify any registered listeners (which
    // includes JUCE's AudioDeviceSelectorComponent, so the dropdown
    // repopulates). The UI's Rescan button calls this after a USB
    // plug/unplug.
    void rescan();

private:
    juce::StringArray inputNames, outputNames;
    juce::StringArray inputIds, outputIds;
    bool hasScanned = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AlsaAudioIODeviceType)
};
} // namespace duskstudio
