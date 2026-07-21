#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/device/DeviceManager.h"
#include "engine/device/DeviceStateBlob.h"
#include "engine/device/IODevice.h"
#include "engine/device/IODeviceCallback.h"
#include "engine/device/IODeviceType.h"
#include "foundation/MessageThread.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

// The native DeviceManager orchestrator (Linux DeviceManager.cpp) driven against
// mock IODeviceType / IODevice backends: open/start/stop/close ordering, the
// callback fan-out (prime-before-insert, remove-stops, pre-sized summing, zero-
// and error-dispatch), the state-blob seeding / fallback semantics, the
// device-change pending-flag rules, and the async-coalesced change broadcast.
// No real audio device, so it links JUCE-free save for the message-loop pump the
// broadcast test needs.

using duskstudio::device::CallbackContext;
using duskstudio::device::ChannelSet;
using duskstudio::device::DeviceManager;
using duskstudio::device::DeviceSetup;
using duskstudio::device::DeviceStateBlob;
using duskstudio::device::IODevice;
using duskstudio::device::IODeviceCallback;
using duskstudio::device::IODeviceType;

namespace
{
// Ordered record of every lifecycle event across all mocks, so a test can assert
// relative order (stop before close before create before open before start).
struct Log
{
    std::vector<std::string> events;
    void add (const std::string& e) { events.push_back (e); }
    int idx (const std::string& needle) const
    {
        for (int i = 0; i < (int) events.size(); ++i)
            if (events[(size_t) i].find (needle) != std::string::npos) return i;
        return -1;
    }
    bool has (const std::string& needle) const { return idx (needle) >= 0; }
};

std::vector<std::string> channelNames (int n, const char* prefix)
{
    std::vector<std::string> v;
    for (int i = 0; i < n; ++i) v.push_back (std::string (prefix) + std::to_string (i));
    return v;
}

class MockDevice final : public IODevice
{
public:
    MockDevice (Log* logIn, std::string nameIn, int inCh, int outCh,
                std::vector<double> rateList, int defaultBuf, std::string openError)
        : name (std::move (nameIn)), log (logIn), inChans (inCh), outChans (outCh),
          rates (std::move (rateList)), defBuf (defaultBuf), openErr (std::move (openError)) {}

    std::string getName() const override { return name; }
    std::vector<std::string> getOutputChannelNames() override { return channelNames (outChans, "out"); }
    std::vector<std::string> getInputChannelNames()  override { return channelNames (inChans, "in"); }
    std::vector<double> getAvailableSampleRates() override { return rates; }
    std::vector<int>    getAvailableBufferSizes() override { return { 64, 128, 256, 512 }; }
    int getDefaultBufferSize() override { return defBuf; }

    std::string open (const ChannelSet& in, const ChannelSet& out, double sr, int bs) override
    {
        log->add (name + ".open");
        if (! openErr.empty()) return openErr;
        inMask = in; outMask = out; appliedRate = sr; appliedBuf = bs; opened = true;
        return {};
    }
    void close() override { if (opened) log->add (name + ".close"); opened = false; }
    bool isOpen() override { return opened; }

    void start (IODeviceCallback* c) override
    {
        log->add (name + ".start");
        cb = c; playing = true;
        cb->audioDeviceAboutToStart (this);   // synchronous, mirroring the JUCE contract
    }
    void stop() override
    {
        if (playing) { log->add (name + ".stop"); playing = false; if (cb) cb->audioDeviceStopped(); }
        cb = nullptr;
    }
    bool isPlaying() override { return playing; }

    std::string getLastError() override { return {}; }
    int    getCurrentBufferSizeSamples() override { return appliedBuf; }
    double getCurrentSampleRate()        override { return appliedRate; }
    int    getCurrentBitDepth()          override { return 24; }
    ChannelSet getActiveOutputChannels() const override { return outMask; }
    ChannelSet getActiveInputChannels()  const override { return inMask; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples()  override { return 0; }
    int getXRunCount() const noexcept override { return 0; }

    // Drive one synthetic block / error through whatever start() installed (the
    // manager's fan-out).
    void deliverBlock (const float* const* in, int numIn, float* const* out, int numOut, int numSamples)
    {
        CallbackContext ctx;
        if (cb) cb->audioDeviceIOCallback (in, numIn, out, numOut, numSamples, ctx);
    }
    void deliverError (const std::string& m) { if (cb) cb->audioDeviceError (m); }

    std::string name;

private:
    Log* log;
    int inChans, outChans;
    std::vector<double> rates;
    int defBuf;
    std::string openErr;
    ChannelSet inMask, outMask;
    double appliedRate = 0.0;
    int appliedBuf = 0;
    bool opened = false, playing = false;
    IODeviceCallback* cb = nullptr;
};

class MockType final : public IODeviceType
{
public:
    MockType (Log* logIn, std::string typeNameIn, std::vector<std::string> outNamesIn,
              std::vector<std::string> inNamesIn, int inCh, int outCh,
              std::vector<double> rateList, int defaultBuf)
        : typeName (std::move (typeNameIn)), log (logIn), outNames (std::move (outNamesIn)),
          inNames (std::move (inNamesIn)), inChans (inCh), outChans (outCh),
          rates (std::move (rateList)), defBuf (defaultBuf) {}

    std::string getTypeName() const override { return typeName; }
    void scanForDevices() override {}
    std::vector<std::string> getDeviceNames (bool wantInputNames) const override
        { return wantInputNames ? inNames : outNames; }
    int getDefaultDeviceIndex (bool forInput) const override { return forInput ? defIn : defOut; }
    int getIndexOfDevice (IODevice*, bool) const override { return -1; }

    std::unique_ptr<IODevice> createDevice (const std::string& outputName,
                                            const std::string& inputName) override
    {
        const std::string devName = ! outputName.empty() ? outputName : inputName;
        log->add (typeName + ":create:" + devName);
        std::string err = busy.count (devName) ? std::string ("device busy") : std::string();
        auto d = std::make_unique<MockDevice> (log, devName, inChans, outChans, rates, defBuf, err);
        created.push_back (d.get());
        return d;
    }

    std::string typeName;
    int defOut = 0, defIn = 0;
    std::set<std::string> busy;          // device names whose open() fails
    std::vector<MockDevice*> created;    // back-pointers; the manager owns them

private:
    Log* log;
    std::vector<std::string> outNames, inNames;
    int inChans, outChans;
    std::vector<double> rates;
    int defBuf;
};

class MockCallback final : public IODeviceCallback
{
public:
    MockCallback() = default;
    MockCallback (Log* l, std::string t, float f) : log (l), tag (std::move (t)), fill (f) {}

    void audioDeviceIOCallback (const float* const*, int, float* const* out, int numOut,
                                int numSamples, const CallbackContext&) override
    {
        ++blocks;
        for (int ch = 0; ch < numOut; ++ch)
            if (out[ch] != nullptr)
                for (int s = 0; s < numSamples; ++s) out[ch][s] = fill;
    }
    void audioDeviceAboutToStart (IODevice*) override { ++aboutToStart; if (log) log->add (tag + ".aboutToStart"); }
    void audioDeviceStopped() override { ++stopped; if (log) log->add (tag + ".stopped"); }
    void audioDeviceError (const std::string& m) override { ++errors; lastError = m; }

    int aboutToStart = 0, stopped = 0, errors = 0, blocks = 0;
    std::string lastError;

private:
    Log* log = nullptr;
    std::string tag;
    float fill = 0.0f;
};

// Two-type harness: an ALSA-like type (index 1) and a PipeWire-like type
// (index 0), plus raw back-pointers the test configures/inspects.
struct Harness
{
    Log log;
    MockType* pw = nullptr;
    MockType* alsa = nullptr;

    std::vector<std::unique_ptr<IODeviceType>> build (bool firstTypeEmpty = false)
    {
        auto pwType = std::make_unique<MockType> (&log, "PipeWire",
            firstTypeEmpty ? std::vector<std::string>{} : std::vector<std::string>{ "pw-default", "pw-alt" },
            firstTypeEmpty ? std::vector<std::string>{} : std::vector<std::string>{ "pw-default", "pw-alt" },
            2, 2, std::vector<double>{ 44100.0, 48000.0 }, 256);
        auto alsaType = std::make_unique<MockType> (&log, "ALSA",
            std::vector<std::string>{ "busyDev", "hw0" },
            std::vector<std::string>{ "hw0" },
            2, 2, std::vector<double>{ 44100.0, 48000.0 }, 128);
        alsaType->defOut = 1;   // default output = "hw0" (not the busy one)

        pw = pwType.get();
        alsa = alsaType.get();

        std::vector<std::unique_ptr<IODeviceType>> v;
        v.push_back (std::move (pwType));
        v.push_back (std::move (alsaType));
        return v;
    }
};

#if ! defined (__APPLE__)
// Pump the JUCE message loop until `ready()` holds or `timeoutMs` elapses, then
// exit. A repeating timer polls the condition from inside a single dispatch loop
// (JUCE_MODAL_LOOPS_PERMITTED is off, so runDispatchLoopUntil is unavailable and
// stopDispatchLoop latches the quit flag - the loop must be entered and quit
// exactly once). A met condition returns promptly; a miss still stops at the
// timeout, so a failing test terminates deterministically instead of hanging.
struct PumpUntil final : dusk::Timer
{
    std::function<bool()> ready;
    int elapsedMs = 0, timeoutMs = 0, tickMs = 0;
    void timerCallback() override
    {
        elapsedMs += tickMs;
        if (ready() || elapsedMs >= timeoutMs)
        {
            stopTimer();
            juce::MessageManager::getInstance()->stopDispatchLoop();
        }
    }
};

void pumpUntil (std::function<bool()> ready, int timeoutMs = 1000)
{
    if (ready()) return;
    PumpUntil p;
    p.ready     = std::move (ready);
    p.timeoutMs = timeoutMs;
    p.tickMs    = 5;
    p.startTimer (p.tickMs);
    juce::MessageManager::getInstance()->runDispatchLoop();
}
#endif
} // namespace

TEST_CASE ("DeviceManager initialise: empty blob opens first type with devices", "[audio][device]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Harness h;
    DeviceManager dm;
    dm.setDeviceTypesForTest (h.build (/*firstTypeEmpty*/ true));   // PipeWire has no devices

    const auto err = dm.initialise (16, 2, "", /*selectDefaultOnFailure*/ true);
    REQUIRE (err.empty());

    // The empty first type is skipped; ALSA (has devices) is chosen and opened.
    auto* type = dm.getCurrentDeviceType();
    REQUIRE (type != nullptr);
    REQUIRE (type->getTypeName() == "ALSA");
    REQUIRE (dm.getCurrentDevice() != nullptr);

    // A first-launch default pick is never a chosen setup: the blob stays empty
    // until the user explicitly picks (treatAsChosen).
    REQUIRE (dm.getStateBlob().empty());
}

TEST_CASE ("DeviceManager initialise: saved blob opens the named device and type", "[audio][device]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Harness h;
    DeviceManager dm;
    dm.setDeviceTypesForTest (h.build());

    DeviceStateBlob saved;
    saved.deviceType             = "ALSA";
    saved.setup.outputDeviceName = "hw0";
    saved.setup.inputDeviceName  = "hw0";
    saved.setup.sampleRate       = 48000.0;
    saved.setup.bufferSize       = 256;

    const auto err = dm.initialise (16, 2, saved.toJson(), /*selectDefaultOnFailure*/ true);
    REQUIRE (err.empty());

    REQUIRE (dm.getCurrentDeviceType() != nullptr);
    REQUIRE (dm.getCurrentDeviceType()->getTypeName() == "ALSA");
    REQUIRE (dm.getCurrentDevice() != nullptr);
    REQUIRE (dm.getCurrentDevice()->getName() == "hw0");

    // getStateBlob round-trips the saved intent (seeded from the blob).
    const auto out = DeviceStateBlob::parse (dm.getStateBlob());
    REQUIRE (out.has_value());
    REQUIRE (out->deviceType == "ALSA");
    REQUIRE (out->setup.outputDeviceName == "hw0");
}

TEST_CASE ("DeviceManager initialise: busy saved device falls back but keeps saved intent", "[audio][device]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Harness h;
    DeviceManager dm;
    auto types = h.build();
    // Mark the saved device busy so its open() fails; the type's default (hw0) is free.
    h.alsa->busy.insert ("busyDev");
    dm.setDeviceTypesForTest (std::move (types));

    DeviceStateBlob saved;
    saved.deviceType             = "ALSA";
    saved.setup.outputDeviceName = "busyDev";
    saved.setup.inputDeviceName  = "busyDev";
    saved.setup.sampleRate       = 48000.0;
    saved.setup.bufferSize       = 256;

    const auto err = dm.initialise (16, 2, saved.toJson(), /*selectDefaultOnFailure*/ true);
    REQUIRE (err.empty());

    // Fallback opened the type's default device.
    REQUIRE (dm.getCurrentDevice() != nullptr);
    REQUIRE (dm.getCurrentDevice()->getName() == "hw0");

    // The saved intent survives the fallback: getStateBlob still names busyDev,
    // and outputDeviceNameFromState reports the user's INTENDED device.
    const auto out = DeviceStateBlob::parse (dm.getStateBlob());
    REQUIRE (out.has_value());
    REQUIRE (out->setup.outputDeviceName == "busyDev");
    REQUIRE (dm.outputDeviceNameFromState (saved.toJson()) == "busyDev");
}

TEST_CASE ("DeviceManager setSetup: changed reopens stop->close->create->open->start", "[audio][device]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Harness h;
    DeviceManager dm;
    dm.setDeviceTypesForTest (h.build());
    REQUIRE (dm.initialise (16, 2, "", true).empty());

    // Baseline is open on PipeWire's default (pw-default). Switch to pw-alt.
    h.log.events.clear();
    auto setup = dm.getSetup();
    setup.outputDeviceName = "pw-alt";
    setup.inputDeviceName  = "pw-alt";
    const auto err = dm.setSetup (setup, /*treatAsChosen*/ true);
    REQUIRE (err.empty());

    // Ordering: the old device stops + closes, then the new one is created,
    // opened and started.
    const int stop   = h.log.idx ("pw-default.stop");
    const int close  = h.log.idx ("pw-default.close");
    const int create = h.log.idx ("create:pw-alt");
    const int open   = h.log.idx ("pw-alt.open");
    const int start  = h.log.idx ("pw-alt.start");
    REQUIRE (stop  >= 0);
    REQUIRE (close  > stop);
    REQUIRE (create > close);
    REQUIRE (open   > create);
    REQUIRE (start  > open);

    // A chosen setup updates getStateBlob to the applied configuration.
    const auto blob = DeviceStateBlob::parse (dm.getStateBlob());
    REQUIRE (blob.has_value());
    REQUIRE (blob->setup.outputDeviceName == "pw-alt");
}

TEST_CASE ("DeviceManager setSetup: unchanged is a no-op", "[audio][device]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Harness h;
    DeviceManager dm;
    dm.setDeviceTypesForTest (h.build());
    REQUIRE (dm.initialise (16, 2, "", true).empty());

    const bool pendingBefore = dm.isDeviceChangePending();
    h.log.events.clear();

    const auto same = dm.getSetup();
    const auto err = dm.setSetup (same, /*treatAsChosen*/ true);
    REQUIRE (err.empty());

    // No device churn - so openDeviceFromSetup (hence broadcast) was never
    // entered - and the pending flag is untouched.
    REQUIRE (h.log.events.empty());
    REQUIRE (dm.isDeviceChangePending() == pendingBefore);
}

TEST_CASE ("DeviceManager setCurrentDeviceType: arm rules and pending clears", "[audio][device]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Harness h;
    DeviceManager dm;
    dm.setDeviceTypesForTest (h.build());
    REQUIRE (dm.initialise (16, 2, "", true).empty());   // opens on PipeWire (first with devices)

    REQUIRE (dm.getCurrentDeviceType()->getTypeName() == "PipeWire");
    REQUIRE_FALSE (dm.isDeviceChangePending());

    SECTION ("unknown type name does not arm and does not move")
    {
        dm.setCurrentDeviceType ("Nonexistent", true);
        REQUIRE_FALSE (dm.isDeviceChangePending());
        REQUIRE (dm.getCurrentDeviceType()->getTypeName() == "PipeWire");
        REQUIRE (dm.getCurrentDevice() != nullptr);
    }

    SECTION ("current type name does not arm")
    {
        dm.setCurrentDeviceType ("PipeWire", true);
        REQUIRE_FALSE (dm.isDeviceChangePending());
        REQUIRE (dm.getCurrentDevice() != nullptr);
    }

    SECTION ("real switch leaves device null with pending set, cleared on next open")
    {
        dm.setCurrentDeviceType ("ALSA", true);
        REQUIRE (dm.getCurrentDeviceType()->getTypeName() == "ALSA");
        REQUIRE (dm.getCurrentDevice() == nullptr);   // does not auto-open
        REQUIRE (dm.isDeviceChangePending());

        // The async broadcast fires with a null device, so it must NOT clear the
        // pending flag (a backend switch selects nothing until the user picks). A
        // listener observes the broadcast actually landing, so the assertion runs
        // against a fired broadcast rather than an elapsed timer.
#if ! defined (__APPLE__)
        int broadcasts = 0;
        dm.addChangeListener (&broadcasts, [&broadcasts] { ++broadcasts; });
        pumpUntil ([&] { return broadcasts >= 1; });
        REQUIRE (broadcasts == 1);
        REQUIRE (dm.isDeviceChangePending());
        dm.removeChangeListener (&broadcasts);
#endif

        // The user picks a device on the new type: aboutToStart clears pending
        // synchronously at start().
        auto setup = dm.getSetup();
        setup.outputDeviceName = "hw0";
        setup.inputDeviceName  = "hw0";
        REQUIRE (dm.setSetup (setup, true).empty());
        REQUIRE (dm.getCurrentDevice() != nullptr);
        REQUIRE_FALSE (dm.isDeviceChangePending());
    }
}

TEST_CASE ("DeviceManager fan-out: prime, remove-stop, summing, zero, error", "[audio][device]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Harness h;
    DeviceManager dm;
    dm.setDeviceTypesForTest (h.build());
    REQUIRE (dm.initialise (16, 2, "", true).empty());   // device running, no callbacks

    constexpr int numOut = 2, numSamples = 8;
    std::vector<float> ch0 (numSamples), ch1 (numSamples);
    float* outPtrs[2] = { ch0.data(), ch1.data() };
    auto* dev = h.pw->created.back();   // the live device (init opened PipeWire)

    SECTION ("add-while-running primes with aboutToStart before the first block")
    {
        MockCallback cb (&h.log, "cbA", 0.5f);
        dm.addCallback (&cb);
        REQUIRE (cb.aboutToStart == 1);
        REQUIRE (cb.blocks == 0);

        dev->deliverBlock (nullptr, 0, outPtrs, numOut, numSamples);
        REQUIRE (cb.blocks == 1);
        REQUIRE_THAT (ch0[0], Catch::Matchers::WithinAbs (0.5f, 1e-9));

        dm.removeCallback (&cb);
    }

    SECTION ("remove-while-running delivers audioDeviceStopped")
    {
        MockCallback cb (&h.log, "cbA", 0.5f);
        dm.addCallback (&cb);
        REQUIRE (cb.stopped == 0);
        dm.removeCallback (&cb);
        REQUIRE (cb.stopped == 1);
    }

    SECTION ("two callbacks sum bit-exact")
    {
        MockCallback cb0 (&h.log, "cb0", 0.5f);    // callbacks[0], writes directly
        MockCallback cb1 (&h.log, "cb1", 0.25f);   // callbacks[1], summed via scratch
        dm.addCallback (&cb0);
        dm.addCallback (&cb1);

        dev->deliverBlock (nullptr, 0, outPtrs, numOut, numSamples);
        for (int s = 0; s < numSamples; ++s)
        {
            REQUIRE_THAT (ch0[(size_t) s], Catch::Matchers::WithinAbs (0.75f, 1e-9));
            REQUIRE_THAT (ch1[(size_t) s], Catch::Matchers::WithinAbs (0.75f, 1e-9));
        }
        dm.removeCallback (&cb0);
        dm.removeCallback (&cb1);
    }

    SECTION ("zero callbacks produce zeroed output")
    {
        std::fill (ch0.begin(), ch0.end(), 1.0f);
        std::fill (ch1.begin(), ch1.end(), 1.0f);
        dev->deliverBlock (nullptr, 0, outPtrs, numOut, numSamples);
        for (int s = 0; s < numSamples; ++s)
        {
            REQUIRE_THAT (ch0[(size_t) s], Catch::Matchers::WithinAbs (0.0f, 1e-9));
            REQUIRE_THAT (ch1[(size_t) s], Catch::Matchers::WithinAbs (0.0f, 1e-9));
        }
    }

    SECTION ("audioDeviceError reaches every client")
    {
        MockCallback cb0 (&h.log, "cb0", 0.0f);
        MockCallback cb1 (&h.log, "cb1", 0.0f);
        dm.addCallback (&cb0);
        dm.addCallback (&cb1);
        dev->deliverError ("boom");
        REQUIRE (cb0.errors == 1);
        REQUIRE (cb1.errors == 1);
        REQUIRE (cb0.lastError == "boom");
        REQUIRE (cb1.lastError == "boom");
        dm.removeCallback (&cb0);
        dm.removeCallback (&cb1);
    }
}

TEST_CASE ("DeviceManager listeners: owner removed mid-fire is skipped", "[audio][device]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Harness h;
    DeviceManager dm;
    dm.setDeviceTypesForTest (h.build());

    // fireListeners walks the listener map in ascending pointer order
    // (std::map<void*, ...>), which is independent of registration order. Sort
    // three distinct owner keys the same way and assign roles by fire position,
    // so the test holds however the stack lays the locals out.
    int k0 = 0, k1 = 0, k2 = 0;
    std::vector<void*> keys { &k0, &k1, &k2 };
    std::sort (keys.begin(), keys.end(), std::less<void*>{});
    void* firstOwner  = keys[0];   // fires first  - removes the second before its turn
    void* secondOwner = keys[1];   // fires second - must be skipped
    void* thirdOwner  = keys[2];   // fires last   - removes itself mid-call

    int firstCalls = 0, secondCalls = 0, thirdCalls = 0;

    dm.addChangeListener (firstOwner, [&]
    {
        ++firstCalls;
        dm.removeChangeListener (secondOwner);
    });
    dm.addChangeListener (secondOwner, [&] { ++secondCalls; });
    dm.addChangeListener (thirdOwner, [&]
    {
        ++thirdCalls;
        dm.removeChangeListener (thirdOwner);   // remove self mid-call; safe because
                                                // fireListeners copies the callback
    });

    dm.notifyChange();   // synchronous fan-out
    REQUIRE (firstCalls == 1);
    REQUIRE (secondCalls == 0);   // removed before its turn by the earlier-firing listener
    REQUIRE (thirdCalls == 1);

    dm.notifyChange();
    REQUIRE (firstCalls == 2);
    REQUIRE (thirdCalls == 1);   // self-removed last round, not called again

    dm.removeChangeListener (firstOwner);
}

#if ! defined (__APPLE__)
TEST_CASE ("DeviceManager broadcast: async and coalesced on the message thread", "[audio][device]")
{
    // A single message-loop pump per case: JUCE's stopDispatchLoop latches the
    // quit flag, so a second runDispatchLoop would be a no-op.
    juce::ScopedJuceInitialiser_GUI juceInit;
    Harness h;
    DeviceManager dm;
    dm.setDeviceTypesForTest (h.build());

    int fired = 0;
    dm.addChangeListener (&fired, [&fired] { ++fired; });

    // Three operations that each request a broadcast: the initial open plus two
    // buffer-size changes (PipeWire's default is 256, so 128 and 512 are genuine).
    // With no pump between them they coalesce into a single async fire.
    REQUIRE (dm.initialise (16, 2, "", true).empty());
    auto s1 = dm.getSetup(); s1.bufferSize = 128;
    REQUIRE (dm.setSetup (s1, true).empty());
    auto s2 = dm.getSetup(); s2.bufferSize = 512;
    REQUIRE (dm.setSetup (s2, true).empty());

    REQUIRE (fired == 0);   // async: deferred, nothing runs inline

    pumpUntil ([&] { return fired >= 1; });
    REQUIRE (fired == 1);   // coalesced: three requests, one fire

    dm.removeChangeListener (&fired);
}
#endif
