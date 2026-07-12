#include "AlsaAudioIODeviceType.h"
#include "AlsaAudioIODevice.h"

#include <alsa/asoundlib.h>

namespace duskstudio
{
AlsaAudioIODeviceType::AlsaAudioIODeviceType()
    : juce::AudioIODeviceType ("ALSA")
{
}

void AlsaAudioIODeviceType::scanForDevices()
{
    inputNames.clear();
    outputNames.clear();
    inputIds.clear();
    outputIds.clear();

    snd_ctl_card_info_t* cardInfo = nullptr;
    snd_ctl_card_info_alloca (&cardInfo);

    int cardNum = -1;

    while (true)
    {
        if (snd_card_next (&cardNum) < 0 || cardNum < 0)
            break;

        const auto cardCtlId = juce::String ("hw:") + juce::String (cardNum);

        snd_ctl_t* ctl = nullptr;
        if (snd_ctl_open (&ctl, cardCtlId.toRawUTF8(), SND_CTL_NONBLOCK) < 0)
            continue;

        if (snd_ctl_card_info (ctl, cardInfo) < 0)
        {
            snd_ctl_close (ctl);
            continue;
        }

        // Card "id" is the symbolic short name (e.g. "UMC1820") used for the
        // hw: PCM identifier; "name" is the human-readable form (e.g.
        // "UMC1820, USB Audio") shown in the UI dropdown.
        juce::String cardSym (snd_ctl_card_info_get_id   (cardInfo));
        juce::String cardName (snd_ctl_card_info_get_name (cardInfo));
        if (cardSym.isEmpty())  cardSym  = juce::String (cardNum);
        if (cardName.isEmpty()) cardName = cardSym;

        snd_pcm_info_t* pcmInfo = nullptr;
        snd_pcm_info_alloca (&pcmInfo);

        int device = -1;
        while (true)
        {
            if (snd_ctl_pcm_next_device (ctl, &device) < 0 || device < 0)
                break;

            snd_pcm_info_set_device   (pcmInfo, (unsigned int) device);
            snd_pcm_info_set_subdevice (pcmInfo, 0);

            // Capture name from the FIRST successful query - pcmInfo is
            // mutated by snd_ctl_pcm_info and may be left in an undefined
            // state if a subsequent stream-direction query fails.
            juce::String pcmName;

            snd_pcm_info_set_stream (pcmInfo, SND_PCM_STREAM_CAPTURE);
            const bool isInput = (snd_ctl_pcm_info (ctl, pcmInfo) >= 0);
            if (isInput)
                pcmName = juce::String (snd_pcm_info_get_name (pcmInfo));

            snd_pcm_info_set_stream (pcmInfo, SND_PCM_STREAM_PLAYBACK);
            const bool isOutput = (snd_ctl_pcm_info (ctl, pcmInfo) >= 0);
            if (isOutput && pcmName.isEmpty())
                pcmName = juce::String (snd_pcm_info_get_name (pcmInfo));

            if (! (isInput || isOutput))
                continue;

            const juce::String id   = juce::String ("hw:") + cardSym + "," + juce::String (device);
            const juce::String name = pcmName.isEmpty() ? cardName
                                                         : (cardName + ", " + pcmName);

            if (isInput)
            {
                inputNames.add (name);
                inputIds.add   (id);
            }
            if (isOutput)
            {
                outputNames.add (name);
                outputIds.add   (id);
            }
        }

        snd_ctl_close (ctl);
    }

    inputNames.appendNumbersToDuplicates  (false, true);
    outputNames.appendNumbersToDuplicates (false, true);

    hasScanned = true;
}

juce::StringArray AlsaAudioIODeviceType::getDeviceNames (bool wantInputNames) const
{
    jassert (hasScanned);
    return wantInputNames ? inputNames : outputNames;
}

int AlsaAudioIODeviceType::getDefaultDeviceIndex (bool forInput) const
{
    jassert (hasScanned);

    // Heuristic: prefer the first device that doesn't look like a built-in
    // motherboard codec, an HDMI output, or a webcam. Most users running
    // Linux for audio plug in a USB or PCIe interface; first-run defaulting
    // to the laptop's onboard HDA + 5.1 surround mapping forces them to
    // dig into the dropdown to find their actual interface, which is the
    // exact "doesn't work out of the box" experience we want to avoid.
    //
    // This only matters on first launch and after a saved device name no
    // longer resolves; once the user's selection is persisted via JUCE's
    // AudioDeviceManager state, that takes precedence.
    const auto& names = forInput ? inputNames : outputNames;
    for (int i = 0; i < names.size(); ++i)
    {
        const auto& n = names[i];
        if (n.startsWithIgnoreCase ("HDA "))             continue;
        if (n.containsIgnoreCase ("HDMI"))               continue;
        if (forInput && n.containsIgnoreCase ("Webcam")) continue;
        return i;
    }

    // Only built-in / unwanted devices are present - fall back to the
    // first entry rather than returning -1, so the dialog still opens
    // something rather than refusing to enumerate.
    return names.isEmpty() ? -1 : 0;
}

int AlsaAudioIODeviceType::getIndexOfDevice (juce::AudioIODevice* device, bool asInput) const
{
    jassert (hasScanned);
    if (auto* alsa = dynamic_cast<AlsaAudioIODevice*> (device))
        return (asInput ? inputIds : outputIds).indexOf (asInput ? alsa->inputId : alsa->outputId);
    return -1;
}

juce::AudioIODevice* AlsaAudioIODeviceType::createDevice (const juce::String& outputDeviceName,
                                                            const juce::String& inputDeviceName)
{
    jassert (hasScanned);
    const int outIdx = outputNames.indexOf (outputDeviceName);
    const int inIdx  = inputNames .indexOf (inputDeviceName);

    if (outIdx < 0 && inIdx < 0)
        return nullptr;

    const juce::String outId = outIdx >= 0 ? outputIds[outIdx] : juce::String();
    const juce::String inId  = inIdx  >= 0 ? inputIds [inIdx]  : juce::String();
    const juce::String name  = outIdx >= 0 ? outputDeviceName : inputDeviceName;

    return new AlsaAudioIODevice (name, inId, outId);
}

void AlsaAudioIODeviceType::rescan()
{
    // scanForDevices() was tweaked earlier to drop its hasScanned early-
    // return so a second call does pick up freshly-plugged hw: devices.
    // After repopulating, fire the inherited listener notification so
    // JUCE's AudioDeviceSelectorComponent re-queries getDeviceNames()
    // and rebuilds its Output/Input combos. Without the listener call,
    // the new device list sits in our arrays but the dropdown stays
    // stale until the user closes and reopens the dialog.
    scanForDevices();
    callDeviceChangeListeners();
}
} // namespace duskstudio
