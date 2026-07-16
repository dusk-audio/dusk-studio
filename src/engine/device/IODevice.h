#pragma once

#include "ChannelSet.h"

#include <string>
#include <vector>

// One open audio device (a sink, a source, or a duplex pair), mirroring the
// JUCE's AudioIODevice interface the PipeWire and ALSA backends implement. All
// currency is dusk / STL: names are std::string, capability lists std::vector,
// channel masks ChannelSet. A bespoke device manager drives this; no JUCE.
namespace duskstudio::device
{
class IODeviceCallback;

class IODevice
{
public:
    virtual ~IODevice() = default;

    virtual std::string getName() const = 0;

    virtual std::vector<std::string> getOutputChannelNames() = 0;
    virtual std::vector<std::string> getInputChannelNames()  = 0;
    virtual std::vector<double>      getAvailableSampleRates() = 0;
    virtual std::vector<int>         getAvailableBufferSizes() = 0;
    virtual int                      getDefaultBufferSize()    = 0;

    // Open the device with the given active channels, sample rate and block size.
    // Returns an empty string on success, or a human-readable error otherwise.
    virtual std::string open (const ChannelSet& inputChannels,
                              const ChannelSet& outputChannels,
                              double sampleRate, int bufferSizeSamples) = 0;
    virtual void close()  = 0;
    virtual bool isOpen() = 0;

    // Begin streaming, delivering blocks to `callback` on the RT thread. stop()
    // ends streaming; the callback is not retained after stop() returns.
    virtual void start (IODeviceCallback* callback) = 0;
    virtual void stop()      = 0;
    virtual bool isPlaying() = 0;

    virtual std::string getLastError() = 0;

    // The values actually in force after open() (a backend may round the request
    // to a graph- or kernel-supported value).
    virtual int    getCurrentBufferSizeSamples() = 0;
    virtual double getCurrentSampleRate()        = 0;
    virtual int    getCurrentBitDepth()          = 0;

    virtual ChannelSet getActiveOutputChannels() const = 0;
    virtual ChannelSet getActiveInputChannels()  const = 0;

    virtual int getOutputLatencyInSamples() = 0;
    virtual int getInputLatencyInSamples()  = 0;

    virtual int getXRunCount() const noexcept = 0;
};
} // namespace duskstudio::device
