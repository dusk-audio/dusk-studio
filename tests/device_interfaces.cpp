#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/device/ChannelSet.h"
#include "engine/device/DeviceSetup.h"
#include "engine/device/IODevice.h"
#include "engine/device/IODeviceCallback.h"
#include "engine/device/IODeviceType.h"

#include <juce_core/juce_core.h>

#include <memory>
#include <string>
#include <vector>

using namespace duskstudio::device;

// ChannelSet replaces the juce::BigInteger channel mask the device layer carried.
// A/B every operation the device code + selector use against juce::BigInteger so
// the replacement is provably bit-identical.

namespace
{
// Apply the same edit script to both a ChannelSet and a juce::BigInteger, then
// assert they agree on every channel + the aggregate queries.
void assertMatches (const ChannelSet& cs, const juce::BigInteger& bi)
{
    REQUIRE (cs.count()        == bi.countNumberOfSetBits());
    REQUIRE (cs.highestSetBit() == bi.getHighestBit());
    for (int i = 0; i < ChannelSet::kMaxChannels; ++i)
        REQUIRE (cs[i] == bi[i]);
}
} // namespace

TEST_CASE ("ChannelSet matches juce::BigInteger across setRange / setBit", "[device][channelset]")
{
    SECTION ("stereo from zero")
    {
        ChannelSet cs; juce::BigInteger bi;
        cs.setRange (0, 2, true);  bi.setRange (0, 2, true);
        assertMatches (cs, bi);
        REQUIRE (cs.count() == 2);
    }

    SECTION ("wide multichannel range")
    {
        ChannelSet cs; juce::BigInteger bi;
        cs.setRange (0, 24, true);  bi.setRange (0, 24, true);
        assertMatches (cs, bi);
    }

    SECTION ("sparse individual bits")
    {
        ChannelSet cs; juce::BigInteger bi;
        for (int b : { 0, 3, 7, 15, 31 })
        {
            cs.setBit (b, true);
            bi.setBit (b, true);
        }
        assertMatches (cs, bi);
        REQUIRE (cs.highestSetBit() == 31);
    }

    SECTION ("clearing a sub-range")
    {
        ChannelSet cs; juce::BigInteger bi;
        cs.setRange (0, 8, true);   bi.setRange (0, 8, true);
        cs.setRange (2, 3, false);  bi.setRange (2, 3, false);
        assertMatches (cs, bi);
    }

    SECTION ("empty set")
    {
        ChannelSet cs; juce::BigInteger bi;
        REQUIRE (cs.isZero());
        REQUIRE (cs.highestSetBit() == -1);
        assertMatches (cs, bi);
    }
}

TEST_CASE ("ChannelSet ignores out-of-range bits and round-trips raw()", "[device][channelset]")
{
    ChannelSet cs;
    cs.setBit (ChannelSet::kMaxChannels, true);   // out of range: no-op
    cs.setBit (-1, true);                          // out of range: no-op
    REQUIRE (cs.isZero());

    cs.setRange (0, 4, true);
    REQUIRE (ChannelSet::fromRaw (cs.raw()) == cs);
}

// Minimal implementations proving the three interfaces are complete +
// implementable, standing in for the PipeWire / ALSA backends and the engine
// callback that arrive in later phases.
namespace
{
class StubDevice final : public IODevice
{
public:
    std::string getName() const override { return "Stub"; }
    std::vector<std::string> getOutputChannelNames() override { return { "L", "R" }; }
    std::vector<std::string> getInputChannelNames()  override { return {}; }
    std::vector<double> getAvailableSampleRates() override { return { 44100.0, 48000.0 }; }
    std::vector<int>    getAvailableBufferSizes() override { return { 128, 256, 512 }; }
    int getDefaultBufferSize() override { return 512; }

    std::string open (const ChannelSet&, const ChannelSet& out, double sr, int bs) override
    {
        outChans = out; rate = sr; block = bs; opened = true;
        return {};
    }
    void close() override { opened = false; }
    bool isOpen() override { return opened; }

    void start (IODeviceCallback* cb) override { callback = cb; playing = true; }
    void stop() override { playing = false; callback = nullptr; }
    bool isPlaying() override { return playing; }

    std::string getLastError() override { return {}; }
    int    getCurrentBufferSizeSamples() override { return block; }
    double getCurrentSampleRate()        override { return rate; }
    int    getCurrentBitDepth()          override { return 32; }
    ChannelSet getActiveOutputChannels() const override { return outChans; }
    ChannelSet getActiveInputChannels()  const override { return {}; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples()  override { return 0; }
    int getXRunCount() const noexcept override { return 0; }

    IODeviceCallback* callback = nullptr;

private:
    ChannelSet outChans;
    double rate = 0.0;
    int block = 0;
    bool opened = false, playing = false;
};

class StubType final : public IODeviceType
{
public:
    std::string getTypeName() const override { return "StubType"; }
    void scanForDevices() override { scanned = true; }
    std::vector<std::string> getDeviceNames (bool wantInputNames) const override
    {
        return wantInputNames ? std::vector<std::string>{} : std::vector<std::string>{ "Stub" };
    }
    int getDefaultDeviceIndex (bool) const override { return 0; }
    int getIndexOfDevice (IODevice*, bool) const override { return 0; }
    std::unique_ptr<IODevice> createDevice (const std::string&, const std::string&) override
    {
        return std::make_unique<StubDevice>();
    }
    bool scanned = false;
};

class CountingCallback final : public IODeviceCallback
{
public:
    void audioDeviceIOCallback (const float* const*, int,
                                float* const* out, int numOut,
                                int numSamples, const CallbackContext&) override
    {
        for (int c = 0; c < numOut; ++c)
            for (int i = 0; i < numSamples; ++i)
                out[c][i] = 0.0f;
        ++blocks;
    }
    void audioDeviceAboutToStart (IODevice*) override { started = true; }
    void audioDeviceStopped() override { started = false; }

    int  blocks  = 0;
    bool started = false;
};
} // namespace

TEST_CASE ("device interfaces are implementable and drive a callback", "[device][interface]")
{
    StubType type;
    type.scanForDevices();
    REQUIRE (type.scanned);
    REQUIRE (type.getDeviceNames (false).size() == 1);

    auto device = type.createDevice ("Stub", {});
    REQUIRE (device != nullptr);

    ChannelSet out; out.setRange (0, 2, true);
    REQUIRE (device->open ({}, out, 48000.0, 256).empty());
    REQUIRE (device->isOpen());
    REQUIRE_THAT (device->getCurrentSampleRate(), Catch::Matchers::WithinAbs (48000.0, 1e-9));
    REQUIRE (device->getActiveOutputChannels().count() == 2);

    CountingCallback cb;
    cb.audioDeviceAboutToStart (device.get());
    device->start (&cb);
    REQUIRE (device->isPlaying());

    // Drive one block through the callback contract by hand.
    constexpr int n = 64;
    std::vector<float> l (n, 1.0f), r (n, 1.0f);
    float* outArr[2] = { l.data(), r.data() };
    CallbackContext ctx;
    cb.audioDeviceIOCallback (nullptr, 0, outArr, 2, n, ctx);
    REQUIRE (cb.blocks == 1);
    REQUIRE_THAT (l[0], Catch::Matchers::WithinAbs (0.0, 1e-9));

    device->stop();
    cb.audioDeviceStopped();
    device->close();
    REQUIRE_FALSE (device->isOpen());
    REQUIRE_FALSE (cb.started);
}
