#pragma once

#include <cstdint>

// The real-time audio callback the device layer drives, mirroring the slice of
// JUCE's AudioIODeviceCallback the engine implements. A backend (PipeWire, ALSA)
// invokes audioDeviceIOCallback on its RT thread once per block; aboutToStart /
// stopped bracket the stream on start()/stop(). Nothing here touches JUCE.
namespace duskstudio::device
{
class IODevice;

// Extra per-callback data, mirroring JUCE's AudioIODeviceCallbackContext. A null
// hostTimeNs means the backend supplied no hardware timestamp for this block.
struct CallbackContext
{
    const std::uint64_t* hostTimeNs = nullptr;
};

class IODeviceCallback
{
public:
    virtual ~IODeviceCallback() = default;

    // RT thread. inputChannelData / outputChannelData are arrays of
    // numInputChannels / numOutputChannels per-channel pointers, each numSamples
    // long. Output buffers are the callee's to fill; unwritten channels must be
    // cleared by the callback (the backend does not pre-zero them).
    virtual void audioDeviceIOCallback (const float* const* inputChannelData,
                                        int numInputChannels,
                                        float* const* outputChannelData,
                                        int numOutputChannels,
                                        int numSamples,
                                        const CallbackContext& context) = 0;

    // Message thread (or the backend's start path). The stream is opened and
    // about to run at device->getCurrentSampleRate() / bufferSize.
    virtual void audioDeviceAboutToStart (IODevice* device) = 0;

    // The stream has stopped; no further audioDeviceIOCallback will arrive until
    // the next aboutToStart.
    virtual void audioDeviceStopped() = 0;

    // The backend can no longer service I/O (fatal error). A backend may not
    // deliver a following audioDeviceStopped, so callers treat this as terminal.
    // Default empty - most callbacks don't need it.
    virtual void audioDeviceError (const std::string& message) { (void) message; }
};
} // namespace duskstudio::device
