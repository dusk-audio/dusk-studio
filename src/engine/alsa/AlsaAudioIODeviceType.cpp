#include "AlsaAudioIODeviceType.h"
#include "AlsaAudioIODevice.h"

#include "../../foundation/Text.h"

#include <alsa/asoundlib.h>

#include <cassert>

namespace duskstudio
{
namespace
{
int indexOfString (const std::vector<std::string>& v, const std::string& s)
{
    for (int i = 0; i < (int) v.size(); ++i)
        if (v[(size_t) i] == s) return i;
    return -1;
}

// Disambiguate duplicate device labels: exact (case-sensitive) duplicates get
// " (1)", " (2)", ... suffixes, the first instance numbered too, so the dropdown
// never shows two identical names.
void appendNumbersToDuplicates (std::vector<std::string>& names)
{
    auto findFrom = [&names] (const std::string& target, int startIndex) -> int
    {
        for (int j = startIndex; j < (int) names.size(); ++j)
            if (names[(size_t) j] == target) return j;
        return -1;
    };
    for (int i = 0; i + 1 < (int) names.size(); ++i)
    {
        int nextIndex = findFrom (names[(size_t) i], i + 1);
        if (nextIndex < 0) continue;
        const std::string original = names[(size_t) i];
        int number = 0;
        names[(size_t) i] = original + " (" + std::to_string (++number) + ")";
        while (nextIndex >= 0)
        {
            names[(size_t) nextIndex] = names[(size_t) nextIndex] + " (" + std::to_string (++number) + ")";
            nextIndex = findFrom (original, nextIndex + 1);
        }
    }
}

// Owning handle createDevice hands to the manager. Destruction routes through
// destroyOrPark: a device whose I/O thread was abandoned (wedged past the timed
// join in stop()) is leaked into a process-lifetime holder instead of destroyed
// - the detached thread still dereferences the whole device.
class AlsaDeviceHandle final : public device::IODevice
{
public:
    explicit AlsaDeviceHandle (std::unique_ptr<AlsaAudioIODevice> d) noexcept : dev (std::move (d)) {}
    ~AlsaDeviceHandle() override
    {
        dev->close();
        AlsaAudioIODevice::destroyOrPark (std::move (dev));
    }

    AlsaAudioIODevice* inner() const noexcept { return dev.get(); }

    std::string getName() const override                        { return dev->getName(); }
    std::vector<std::string> getOutputChannelNames() override   { return dev->getOutputChannelNames(); }
    std::vector<std::string> getInputChannelNames() override    { return dev->getInputChannelNames(); }
    std::vector<double> getAvailableSampleRates() override      { return dev->getAvailableSampleRates(); }
    std::vector<int> getAvailableBufferSizes() override         { return dev->getAvailableBufferSizes(); }
    int getDefaultBufferSize() override                         { return dev->getDefaultBufferSize(); }

    std::string open (const device::ChannelSet& in, const device::ChannelSet& out,
                      double sr, int bs) override               { return dev->open (in, out, sr, bs); }
    void close() override                                       { dev->close(); }
    bool isOpen() override                                      { return dev->isOpen(); }

    void start (device::IODeviceCallback* cb) override          { dev->start (cb); }
    void stop() override                                        { dev->stop(); }
    bool isPlaying() override                                   { return dev->isPlaying(); }

    std::string getLastError() override                         { return dev->getLastError(); }
    int    getCurrentBufferSizeSamples() override               { return dev->getCurrentBufferSizeSamples(); }
    double getCurrentSampleRate() override                      { return dev->getCurrentSampleRate(); }
    int    getCurrentBitDepth() override                        { return dev->getCurrentBitDepth(); }
    device::ChannelSet getActiveOutputChannels() const override { return dev->getActiveOutputChannels(); }
    device::ChannelSet getActiveInputChannels() const override  { return dev->getActiveInputChannels(); }
    int getOutputLatencyInSamples() override                    { return dev->getOutputLatencyInSamples(); }
    int getInputLatencyInSamples() override                     { return dev->getInputLatencyInSamples(); }
    int getXRunCount() const noexcept override                  { return dev->getXRunCount(); }

private:
    std::unique_ptr<AlsaAudioIODevice> dev;
};
} // namespace

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

        const auto cardCtlId = "hw:" + std::to_string (cardNum);

        snd_ctl_t* ctl = nullptr;
        if (snd_ctl_open (&ctl, cardCtlId.c_str(), SND_CTL_NONBLOCK) < 0)
            continue;

        if (snd_ctl_card_info (ctl, cardInfo) < 0)
        {
            snd_ctl_close (ctl);
            continue;
        }

        // Card "id" is the symbolic short name (e.g. "UMC1820") used for the
        // hw: PCM identifier; "name" is the human-readable form (e.g.
        // "UMC1820, USB Audio") shown in the UI dropdown.
        std::string cardSym (snd_ctl_card_info_get_id   (cardInfo));
        std::string cardName (snd_ctl_card_info_get_name (cardInfo));
        if (cardSym.empty())  cardSym  = std::to_string (cardNum);
        if (cardName.empty()) cardName = cardSym;

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
            std::string pcmName;

            snd_pcm_info_set_stream (pcmInfo, SND_PCM_STREAM_CAPTURE);
            const bool isInput = (snd_ctl_pcm_info (ctl, pcmInfo) >= 0);
            if (isInput)
                pcmName = snd_pcm_info_get_name (pcmInfo);

            snd_pcm_info_set_stream (pcmInfo, SND_PCM_STREAM_PLAYBACK);
            const bool isOutput = (snd_ctl_pcm_info (ctl, pcmInfo) >= 0);
            if (isOutput && pcmName.empty())
                pcmName = snd_pcm_info_get_name (pcmInfo);

            if (! (isInput || isOutput))
                continue;

            const std::string id   = "hw:" + cardSym + "," + std::to_string (device);
            const std::string name = pcmName.empty() ? cardName
                                                     : (cardName + ", " + pcmName);

            if (isInput)
            {
                inputNames.push_back (name);
                inputIds.push_back   (id);
            }
            if (isOutput)
            {
                outputNames.push_back (name);
                outputIds.push_back   (id);
            }
        }

        snd_ctl_close (ctl);
    }

    appendNumbersToDuplicates (inputNames);
    appendNumbersToDuplicates (outputNames);

    hasScanned = true;
}

std::vector<std::string> AlsaAudioIODeviceType::getDeviceNames (bool wantInputNames) const
{
    assert (hasScanned);
    return wantInputNames ? inputNames : outputNames;
}

int AlsaAudioIODeviceType::getDefaultDeviceIndex (bool forInput) const
{
    assert (hasScanned);

    // Heuristic: prefer the first device that doesn't look like a built-in
    // motherboard codec, an HDMI output, or a webcam. Most users running
    // Linux for audio plug in a USB or PCIe interface; first-run defaulting
    // to the laptop's onboard HDA + 5.1 surround mapping forces them to
    // dig into the dropdown to find their actual interface, which is the
    // exact "doesn't work out of the box" experience we want to avoid.
    //
    // This only matters on first launch and after a saved device name no
    // longer resolves; once the user's selection is persisted via the
    // DeviceManager state blob, that takes precedence.
    const auto& names = forInput ? inputNames : outputNames;
    for (int i = 0; i < (int) names.size(); ++i)
    {
        const auto& n = names[(size_t) i];
        if (dusk::text::startsWith (dusk::text::toUpperCase (n), "HDA "))    continue;
        if (dusk::text::containsIgnoreCase (n, "HDMI"))                      continue;
        if (forInput && dusk::text::containsIgnoreCase (n, "Webcam"))        continue;
        return i;
    }

    // Only built-in / unwanted devices are present - fall back to the
    // first entry rather than returning -1, so the dialog still opens
    // something rather than refusing to enumerate.
    return names.empty() ? -1 : 0;
}

int AlsaAudioIODeviceType::getIndexOfDevice (device::IODevice* dev, bool asInput) const
{
    assert (hasScanned);
    if (auto* handle = dynamic_cast<AlsaDeviceHandle*> (dev))
        return indexOfString (asInput ? inputIds : outputIds,
                              asInput ? handle->inner()->inputId : handle->inner()->outputId);
    return -1;
}

std::unique_ptr<device::IODevice> AlsaAudioIODeviceType::createDevice (const std::string& outputDeviceName,
                                                                       const std::string& inputDeviceName)
{
    assert (hasScanned);
    const int outIdx = indexOfString (outputNames, outputDeviceName);
    const int inIdx  = indexOfString (inputNames,  inputDeviceName);

    if (outIdx < 0 && inIdx < 0)
        return nullptr;

    const std::string outId = outIdx >= 0 ? outputIds[(size_t) outIdx] : std::string();
    const std::string inId  = inIdx  >= 0 ? inputIds [(size_t) inIdx]  : std::string();
    const std::string name  = outIdx >= 0 ? outputDeviceName : inputDeviceName;

    return std::make_unique<AlsaDeviceHandle> (
        std::make_unique<AlsaAudioIODevice> (name, inId, outId));
}
} // namespace duskstudio
