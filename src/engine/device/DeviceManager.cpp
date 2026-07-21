#include "DeviceManager.h"
#include "DeviceStateBlob.h"
#include "IODeviceCallback.h"

#include "../../foundation/MessageThread.h"

// The juce-typed backends the native manager drives during this phase: PipeWire
// and ALSA are still JUCE AudioIODeviceType subclasses, wrapped here in OWNING
// adapters (P3/P4 re-base them onto the dusk interfaces and delete the adapters).
// Gated so a build without either backend (the narrow-link test target) compiles
// this TU with no JUCE at all - only the owning-adapter machinery needs it.
#if defined(DUSKSTUDIO_HAS_PIPEWIRE) || defined(DUSKSTUDIO_HAS_ALSA)
 #include <juce_audio_devices/juce_audio_devices.h>
 #if defined(DUSKSTUDIO_HAS_PIPEWIRE)
  #include "../pipewire/PipeWireAudioIODeviceType.h"
 #endif
 #if defined(DUSKSTUDIO_HAS_ALSA)
  #include "../alsa/AlsaAudioIODeviceType.h"
 #endif
 #define DUSKSTUDIO_DEVICE_HAS_JUCE_BACKENDS 1
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace duskstudio::device
{
namespace
{
// Rate defaulting when a setup carries 0: prefer 48k, then 44.1k, then the
// largest listed rate at or above 44.1k, then the max available. Re-derived
// contract (JUCE prefers the 44.1-48k band), pinned by the mock tests.
double chooseSampleRate (const std::vector<double>& rates)
{
    auto listed = [&rates] (double r)
    {
        for (double x : rates) if (std::abs (x - r) < 1.0) return true;
        return false;
    };
    if (listed (48000.0)) return 48000.0;
    if (listed (44100.0)) return 44100.0;

    double best = 0.0;
    for (double x : rates) if (x >= 44100.0 && x > best) best = x;   // largest listed >= 44.1k
    if (best > 0.0) return best;

    for (double x : rates) if (x > best) best = x;                   // max available
    return best > 0.0 ? best : 44100.0;
}

// Equality for the setSetup short-circuit. Sample rate goes through a tolerance
// rather than == (the discrete rates never sit within it, so it is exact in
// practice while sidestepping a float-equality comparison).
bool sameSetup (const DeviceSetup& a, const DeviceSetup& b) noexcept
{
    return a.outputDeviceName == b.outputDeviceName
        && a.inputDeviceName  == b.inputDeviceName
        && std::abs (a.sampleRate - b.sampleRate) < 1e-6
        && a.bufferSize == b.bufferSize
        && a.inputChannels  == b.inputChannels
        && a.outputChannels == b.outputChannels
        && a.useDefaultInputChannels  == b.useDefaultInputChannels
        && a.useDefaultOutputChannels == b.useDefaultOutputChannels;
}

// The single IODeviceCallback handed to IODevice::start. Fans the device's RT
// blocks out to the registered engine callbacks (list size 1 in practice; the
// N-callback path is retained but uses pre-sized scratch, never resizing in the
// callback). The lock is JUCE's audioCallbackLock discipline: uncontended per
// block, contended only during message-thread add/remove.
class CallbackFanout final : public IODeviceCallback
{
public:
    explicit CallbackFanout (std::atomic<bool>* pending) noexcept : deviceChangePending (pending) {}

    void audioDeviceIOCallback (const float* const* in, int numIn,
                                float* const* out, int numOut, int numSamples,
                                const CallbackContext& ctx) override
    {
        std::lock_guard<std::mutex> lock (callbackListLock);

        if (callbacks.empty())
        {
            for (int ch = 0; ch < numOut; ++ch)
                if (out[ch] != nullptr) std::fill (out[ch], out[ch] + numSamples, 0.0f);
            return;
        }

        // The first client writes the device output buffers directly.
        callbacks[0]->audioDeviceIOCallback (in, numIn, out, numOut, numSamples, ctx);
        if (callbacks.size() == 1) return;

        // Defensive: an over-quantum or over-channel block can't be summed into
        // the pre-sized scratch. callbacks[0] already ran; skip the summed extras
        // rather than allocate (PipeWire's over-quantum guard already drops those
        // cycles, so this is belt-and-braces).
        if ((std::size_t) numOut > scratchPtrs.size()
            || (std::size_t) numOut * (std::size_t) numSamples > scratch.size())
            return;

        for (std::size_t c = 1; c < callbacks.size(); ++c)
        {
            for (int ch = 0; ch < numOut; ++ch)
                scratchPtrs[(std::size_t) ch] = scratch.data() + (std::size_t) ch * (std::size_t) numSamples;

            callbacks[c]->audioDeviceIOCallback (in, numIn, scratchPtrs.data(), numOut, numSamples, ctx);

            for (int ch = 0; ch < numOut; ++ch)
                if (out[ch] != nullptr)
                    for (int s = 0; s < numSamples; ++s)
                        out[ch][s] += scratchPtrs[(std::size_t) ch][s];
        }
    }

    void audioDeviceAboutToStart (IODevice* device) override
    {
        // Synchronous counterpart to the change-broadcast clear: a device that
        // starts and is pulled again before the (async) broadcast lands would
        // otherwise leave the flag set with no live device to clear it.
        deviceChangePending->store (false, std::memory_order_release);

        std::lock_guard<std::mutex> lock (callbackListLock);
        const int outCh = std::max (0, (int) device->getOutputChannelNames().size());
        const int bufSz = std::max (0, device->getCurrentBufferSizeSamples());
        scratch.assign ((std::size_t) outCh * (std::size_t) bufSz, 0.0f);
        scratchPtrs.assign ((std::size_t) outCh, nullptr);

        for (auto* cb : callbacks) cb->audioDeviceAboutToStart (device);
    }

    void audioDeviceStopped() override
    {
        std::lock_guard<std::mutex> lock (callbackListLock);
        for (auto* cb : callbacks) cb->audioDeviceStopped();
    }

    void audioDeviceError (const std::string& message) override
    {
        std::lock_guard<std::mutex> lock (callbackListLock);
        for (auto* cb : callbacks) cb->audioDeviceError (message);
    }

    std::mutex callbackListLock;
    std::vector<IODeviceCallback*> callbacks;

private:
    std::atomic<bool>* deviceChangePending;
    std::vector<float>  scratch;     // planar, numOutputChannels x bufferSize
    std::vector<float*> scratchPtrs; // one entry per output channel
};

#if defined(DUSKSTUDIO_DEVICE_HAS_JUCE_BACKENDS)
juce::BigInteger toBig (const ChannelSet& cs)
{
    juce::BigInteger b;
    for (int i = 0; i < ChannelSet::kMaxChannels; ++i)
        if (cs[i]) b.setBit (i);
    return b;
}

ChannelSet fromBig (const juce::BigInteger& b)
{
    ChannelSet cs;
    for (int i = 0; i < ChannelSet::kMaxChannels; ++i)
        if (b[i]) cs.setBit (i);
    return cs;
}

std::vector<std::string> toStrings (const juce::StringArray& a)
{
    std::vector<std::string> v;
    v.reserve ((std::size_t) a.size());
    for (const auto& s : a) v.push_back (s.toStdString());
    return v;
}

template <typename T>
std::vector<T> toVector (const juce::Array<T>& a)
{
    std::vector<T> v;
    v.reserve ((std::size_t) a.size());
    for (const auto& x : a) v.push_back (x);
    return v;
}

// Owning dusk IODevice over a JUCE AudioIODevice. start() installs a small
// juce-callback shim forwarding the device's RT blocks to the dusk callback (the
// manager's CallbackFanout), presenting this adapter as the dusk device.
class JuceDeviceAdapter final : public IODevice
{
public:
    explicit JuceDeviceAdapter (std::unique_ptr<juce::AudioIODevice> d) noexcept : dev (std::move (d)) {}

    std::string getName() const override { return dev ? dev->getName().toStdString() : std::string(); }

    std::vector<std::string> getOutputChannelNames() override
        { return dev ? toStrings (dev->getOutputChannelNames()) : std::vector<std::string>{}; }
    std::vector<std::string> getInputChannelNames() override
        { return dev ? toStrings (dev->getInputChannelNames()) : std::vector<std::string>{}; }
    std::vector<double> getAvailableSampleRates() override
        { return dev ? toVector (dev->getAvailableSampleRates()) : std::vector<double>{}; }
    std::vector<int> getAvailableBufferSizes() override
        { return dev ? toVector (dev->getAvailableBufferSizes()) : std::vector<int>{}; }
    int getDefaultBufferSize() override { return dev ? dev->getDefaultBufferSize() : 0; }

    std::string open (const ChannelSet& in, const ChannelSet& out, double sr, int bs) override
        { return dev ? dev->open (toBig (in), toBig (out), sr, bs).toStdString() : std::string ("no device"); }
    void close() override { if (dev) dev->close(); }
    bool isOpen() override { return dev && dev->isOpen(); }

    void start (IODeviceCallback* cb) override { shim.bind (cb, this); if (dev) dev->start (&shim); }
    void stop() override { if (dev) dev->stop(); }
    bool isPlaying() override { return dev && dev->isPlaying(); }

    std::string getLastError() override { return dev ? dev->getLastError().toStdString() : std::string(); }
    int    getCurrentBufferSizeSamples() override { return dev ? dev->getCurrentBufferSizeSamples() : 0; }
    double getCurrentSampleRate()        override { return dev ? dev->getCurrentSampleRate() : 0.0; }
    int    getCurrentBitDepth()          override { return dev ? dev->getCurrentBitDepth() : 0; }
    ChannelSet getActiveOutputChannels() const override { return dev ? fromBig (dev->getActiveOutputChannels()) : ChannelSet{}; }
    ChannelSet getActiveInputChannels()  const override { return dev ? fromBig (dev->getActiveInputChannels()) : ChannelSet{}; }
    int getOutputLatencyInSamples() override { return dev ? dev->getOutputLatencyInSamples() : 0; }
    int getInputLatencyInSamples()  override { return dev ? dev->getInputLatencyInSamples() : 0; }
    int getXRunCount() const noexcept override { return dev ? dev->getXRunCount() : 0; }

private:
    struct Shim final : juce::AudioIODeviceCallback
    {
        void bind (IODeviceCallback* c, IODevice* ownerDev) noexcept { cb = c; owner = ownerDev; }

        void audioDeviceIOCallbackWithContext (const float* const* in, int numIn,
                                               float* const* out, int numOut, int numSamples,
                                               const juce::AudioIODeviceCallbackContext& ctx) override
        {
            CallbackContext dctx;
            dctx.hostTimeNs = ctx.hostTimeNs;
            cb->audioDeviceIOCallback (in, numIn, out, numOut, numSamples, dctx);
        }
        void audioDeviceAboutToStart (juce::AudioIODevice*) override { cb->audioDeviceAboutToStart (owner); }
        void audioDeviceStopped() override { cb->audioDeviceStopped(); }
        void audioDeviceError (const juce::String& m) override { cb->audioDeviceError (m.toStdString()); }

        IODeviceCallback* cb = nullptr;
        IODevice* owner = nullptr;
    };

    std::unique_ptr<juce::AudioIODevice> dev;
    Shim shim;
};

// Owning dusk IODeviceType over a JUCE AudioIODeviceType. createDevice now has a
// real implementation (the native manager drives device construction directly),
// returning an owning JuceDeviceAdapter.
class JuceDeviceTypeAdapter final : public IODeviceType
{
public:
    explicit JuceDeviceTypeAdapter (std::unique_ptr<juce::AudioIODeviceType> t) noexcept : type (std::move (t)) {}

    std::string getTypeName() const override { return type->getTypeName().toStdString(); }
    void scanForDevices() override { type->scanForDevices(); }
    std::vector<std::string> getDeviceNames (bool wantInputNames) const override
        { return toStrings (type->getDeviceNames (wantInputNames)); }
    int getDefaultDeviceIndex (bool forInput) const override { return type->getDefaultDeviceIndex (forInput); }
    int getIndexOfDevice (IODevice*, bool) const override { return -1; }

    std::unique_ptr<IODevice> createDevice (const std::string& outputName, const std::string& inputName) override
    {
        auto* raw = type->createDevice (juce::String (outputName), juce::String (inputName));
        if (raw == nullptr) return nullptr;
        return std::make_unique<JuceDeviceAdapter> (std::unique_ptr<juce::AudioIODevice> (raw));
    }

private:
    std::unique_ptr<juce::AudioIODeviceType> type;
};
#endif // DUSKSTUDIO_DEVICE_HAS_JUCE_BACKENDS
} // namespace

struct DeviceManager::Impl
{
    // Channel counts the app asks for (the initialise(16, 2) arguments); default
    // channel masks take the first min(needed, deviceChannels) channels.
    int numInputChannelsNeeded  = 0;
    int numOutputChannelsNeeded = 0;

    std::vector<std::unique_ptr<IODeviceType>> types;   // stable pointers for the selector
    IODeviceType*              currentType = nullptr;   // points into `types`
    std::unique_ptr<IODevice>  currentDevice;
    DeviceSetup                currentSetup;             // effective setup as applied

    // The user's chosen configuration, empty until an explicit pick (or a
    // restored blob seeds it). getStateBlob() serialises this; the engine only
    // persists it when a device is live, so a first-launch default never freezes.
    std::optional<DeviceStateBlob> lastExplicitSettings;

    // Set by a deliberate device-type / setup change / close, cleared once a
    // device starts. See DeviceManager::isDeviceChangePending.
    std::atomic<bool> deviceChangePending { false };
    std::shared_ptr<std::atomic<bool>> alive { std::make_shared<std::atomic<bool>> (true) };
    CallbackFanout fanout { &deviceChangePending };

    std::map<void*, std::function<void()>> listeners;
    std::atomic<bool> broadcastPending { false };
    bool backendsRegistered = false;

    ~Impl()
    {
        // Pending async broadcasts must not touch this after teardown (the
        // message pump can outlive the manager during shutdown). Flip the latch
        // first, then stop + close the device so any still-registered client
        // gets its audioDeviceStopped. Never fire listeners or callAsync here.
        alive->store (false, std::memory_order_release);
        if (currentDevice != nullptr)
        {
            currentDevice->stop();
            currentDevice->close();
            currentDevice.reset();
        }
        listeners.clear();
    }

    void registerBackends()
    {
        if (backendsRegistered) return;
        backendsRegistered = true;
       #if defined(DUSKSTUDIO_DEVICE_HAS_JUCE_BACKENDS)
        // Preference order preserved from the wrapped manager: PipeWire first,
        // ALSA second (the default pick lands on the first type that enumerates
        // devices).
        #if defined(DUSKSTUDIO_HAS_PIPEWIRE)
         types.push_back (std::make_unique<JuceDeviceTypeAdapter> (std::make_unique<PipeWireAudioIODeviceType>()));
        #endif
        #if defined(DUSKSTUDIO_HAS_ALSA)
         types.push_back (std::make_unique<JuceDeviceTypeAdapter> (std::make_unique<AlsaAudioIODeviceType>()));
        #endif
        for (auto& t : types) t->scanForDevices();
       #endif
    }

    IODeviceType* findType (const std::string& name)
    {
        for (auto& t : types)
            if (t->getTypeName() == name) return t.get();
        return nullptr;
    }

    IODeviceType* firstTypeWithDevices()
    {
        for (auto& t : types)
        {
            t->scanForDevices();
            if (! t->getDeviceNames (false).empty() || ! t->getDeviceNames (true).empty())
                return t.get();
        }
        return types.empty() ? nullptr : types.front().get();
    }

    // The type's default output/input device with default channel masks and
    // backend-default rate/buffer.
    DeviceSetup defaultSetupFor (IODeviceType* type)
    {
        DeviceSetup s;
        if (type == nullptr) return s;
        type->scanForDevices();
        const auto outNames = type->getDeviceNames (false);
        const auto inNames  = type->getDeviceNames (true);
        const int outIdx = type->getDefaultDeviceIndex (false);
        const int inIdx  = type->getDefaultDeviceIndex (true);
        if (outIdx >= 0 && outIdx < (int) outNames.size())
            s.outputDeviceName = outNames[(std::size_t) outIdx];
        if (numInputChannelsNeeded > 0 && inIdx >= 0 && inIdx < (int) inNames.size())
            s.inputDeviceName = inNames[(std::size_t) inIdx];
        s.useDefaultInputChannels  = true;
        s.useDefaultOutputChannels = true;
        s.sampleRate = 0.0;
        s.bufferSize = 0;
        return s;
    }

    // Stop+close the live device, create the requested one on currentType, open
    // and start it. Empty string on success, else the backend error. treatAsChosen
    // records the applied setup as the user's chosen configuration.
    std::string openDeviceFromSetup (const DeviceSetup& requested, bool treatAsChosen)
    {
        if (currentDevice != nullptr)
        {
            currentDevice->stop();     // fan-out delivers audioDeviceStopped
            currentDevice->close();
            currentDevice.reset();
        }
        if (currentType == nullptr) return "No audio device type selected";

        auto dev = currentType->createDevice (requested.outputDeviceName, requested.inputDeviceName);
        if (! dev) return "Could not create audio device";

        ChannelSet inMask, outMask;
        if (requested.useDefaultInputChannels)
            inMask.setRange (0, std::min (numInputChannelsNeeded, (int) dev->getInputChannelNames().size()), true);
        else
            inMask = requested.inputChannels;
        if (requested.useDefaultOutputChannels)
            outMask.setRange (0, std::min (numOutputChannelsNeeded, (int) dev->getOutputChannelNames().size()), true);
        else
            outMask = requested.outputChannels;

        double rate = requested.sampleRate;
        if (rate <= 0.0) rate = chooseSampleRate (dev->getAvailableSampleRates());
        int buf = requested.bufferSize;
        if (buf <= 0) buf = dev->getDefaultBufferSize();

        const std::string err = dev->open (inMask, outMask, rate, buf);
        if (! err.empty()) { dev->close(); return err; }

        currentDevice = std::move (dev);

        // Effective setup as applied (a backend may round rate/buffer or clamp
        // channels); keep the requested useDefault flags.
        currentSetup = requested;
        currentSetup.inputChannels  = currentDevice->getActiveInputChannels();
        currentSetup.outputChannels = currentDevice->getActiveOutputChannels();
        currentSetup.sampleRate = currentDevice->getCurrentSampleRate();
        currentSetup.bufferSize = currentDevice->getCurrentBufferSizeSamples();

        // start() synchronously drives audioDeviceAboutToStart, clearing
        // deviceChangePending and sizing the fan-out scratch.
        currentDevice->start (&fanout);

        if (treatAsChosen)
        {
            DeviceStateBlob b;
            b.deviceType = currentType->getTypeName();
            b.setup      = currentSetup;
            lastExplicitSettings = b;
        }

        broadcast();
        return {};
    }

    void broadcast()
    {
        // Coalesced + async on the message thread, replicating the JUCE
        // ChangeBroadcaster timing onDeviceManagerChanged's defer logic expects.
        if (broadcastPending.exchange (true, std::memory_order_acq_rel)) return;
        auto keepAlive = alive;
        const bool queued = dusk::callAsync ([this, keepAlive]
        {
            if (! keepAlive->load (std::memory_order_acquire)) return;
            broadcastPending.store (false, std::memory_order_release);
            // A live device means the deliberate change that closed the previous
            // one has landed.
            if (currentDevice != nullptr)
                deviceChangePending.store (false, std::memory_order_release);
            fireListeners();
        });
        if (! queued) broadcastPending.store (false, std::memory_order_release);
    }

    void fireListeners()
    {
        // Snapshot the owner keys, not the closures: a listener may add or remove
        // subscribers (e.g. the settings UI relays out). Re-check each owner
        // against the live map and pull its current callback before calling; copy
        // that callback so an owner removing itself mid-call is safe.
        std::vector<void*> owners;
        owners.reserve (listeners.size());
        for (auto& entry : listeners)
            owners.push_back (entry.first);
        for (auto* owner : owners)
        {
            auto it = listeners.find (owner);
            if (it == listeners.end()) continue;
            auto cb = it->second;
            if (cb) cb();
        }
    }

    void addCallback (IODeviceCallback* cb)
    {
        if (cb == nullptr) return;
        {
            std::lock_guard<std::mutex> lock (fanout.callbackListLock);
            if (std::find (fanout.callbacks.begin(), fanout.callbacks.end(), cb) != fanout.callbacks.end())
                return;   // idempotent
        }
        // Prime the newcomer BEFORE inserting (JUCE's order): a first block must
        // not hit an engine that hasn't seen aboutToStart.
        if (currentDevice != nullptr && currentDevice->isPlaying())
            cb->audioDeviceAboutToStart (currentDevice.get());

        std::lock_guard<std::mutex> lock (fanout.callbackListLock);
        if (std::find (fanout.callbacks.begin(), fanout.callbacks.end(), cb) == fanout.callbacks.end())
            fanout.callbacks.push_back (cb);
    }

    void removeCallback (IODeviceCallback* cb)
    {
        if (cb == nullptr) return;
        // Symmetric with addCallback's prime gate: only a callback that saw
        // aboutToStart (device live AND playing) gets a matching stopped, so a
        // self-stopped device is never stopped a second time on removal.
        bool needsStop = currentDevice != nullptr && currentDevice->isPlaying();
        {
            std::lock_guard<std::mutex> lock (fanout.callbackListLock);
            auto it = std::find (fanout.callbacks.begin(), fanout.callbacks.end(), cb);
            needsStop = needsStop && (it != fanout.callbacks.end());
            if (it != fanout.callbacks.end()) fanout.callbacks.erase (it);
        }
        if (needsStop) cb->audioDeviceStopped();
    }
};

DeviceManager::DeviceManager() : impl (std::make_unique<Impl>()) {}
DeviceManager::~DeviceManager() = default;

std::vector<IODeviceType*> DeviceManager::getAvailableDeviceTypes()
{
    impl->registerBackends();
    std::vector<IODeviceType*> out;
    out.reserve (impl->types.size());
    for (auto& t : impl->types)
        out.push_back (t.get());
    return out;
}

void DeviceManager::scanAllDeviceTypes()
{
    impl->registerBackends();
    for (auto& t : impl->types)
        t->scanForDevices();
}

std::string DeviceManager::initialise (int numInputChannels, int numOutputChannels,
                                       const std::string& savedState, bool selectDefaultOnFailure)
{
    impl->registerBackends();
    impl->numInputChannelsNeeded  = numInputChannels;
    impl->numOutputChannelsNeeded = numOutputChannels;

    // Seed lastExplicitSettings from a parsed blob so a restored selection
    // round-trips without a re-pick (an unparseable/empty blob behaves as a
    // fresh machine, leaving it empty).
    std::optional<DeviceStateBlob> saved = DeviceStateBlob::parse (savedState);
    if (saved) impl->lastExplicitSettings = saved;

    IODeviceType* type = nullptr;
    if (saved && ! saved->deviceType.empty())
        type = impl->findType (saved->deviceType);
    if (type == nullptr)
        type = impl->firstTypeWithDevices();
    if (type == nullptr)
        return "No audio device types available";
    impl->currentType = type;

    const bool usedSaved = saved.has_value();
    const DeviceSetup setup = usedSaved ? saved->setup : impl->defaultSetupFor (type);

    // Open the saved (or default) setup. treatAsChosen=false: lastExplicitSettings
    // is already seeded (or intentionally empty for a fresh machine).
    std::string err = impl->openDeviceFromSetup (setup, /*treatAsChosen*/ false);
    if (err.empty()) return {};
    if (! selectDefaultOnFailure) return err;
    if (! usedSaved) return err;   // the first attempt was already the default

    // Fallback: open the type's default device WITHOUT clobbering
    // lastExplicitSettings, so the saved intent survives a busy device (the
    // engine's DeviceFallbackMessage + outputDeviceNameFromState depend on it).
    return impl->openDeviceFromSetup (impl->defaultSetupFor (type), /*treatAsChosen*/ false);
}

std::string DeviceManager::getStateBlob() const
{
    if (! impl->lastExplicitSettings) return {};
    return impl->lastExplicitSettings->toJson();
}

std::string DeviceManager::outputDeviceNameFromState (const std::string& savedState) const
{
    return DeviceStateBlob::outputDeviceName (savedState);
}

IODevice* DeviceManager::getCurrentDevice() { return impl->currentDevice.get(); }

IODeviceType* DeviceManager::getCurrentDeviceType() { return impl->currentType; }

void DeviceManager::setCurrentDeviceType (const std::string& typeName, bool /*treatAsChosen*/)
{
    // Only arm for a request that will actually move: an unknown name or the
    // already-current type closes nothing and broadcasts nothing, and arming
    // those would strand the flag while a device is still live.
    const bool differsFromCurrent = impl->currentType == nullptr
                                  || impl->currentType->getTypeName() != typeName;
    IODeviceType* target = differsFromCurrent ? impl->findType (typeName) : nullptr;
    if (target == nullptr) return;

    impl->deviceChangePending.store (true, std::memory_order_release);

    // Close the current device (fan-out delivers audioDeviceStopped); leave the
    // device null until the user picks an interface from the new type's list -
    // documented app contract, and the H5 detector's assumption. Do not auto-open.
    if (impl->currentDevice != nullptr)
    {
        impl->currentDevice->stop();
        impl->currentDevice->close();
        impl->currentDevice.reset();
    }
    impl->currentType = target;
    // Nothing is selected on the new type yet, so drop the old type's device
    // names / rate / masks: getSetup() reads empty until the user picks, and a
    // stale cross-backend device name never reaches createDevice.
    impl->currentSetup = DeviceSetup{};
    impl->broadcast();
}

bool DeviceManager::isDeviceChangePending() const noexcept
{
    return impl->deviceChangePending.load (std::memory_order_acquire);
}

DeviceSetup DeviceManager::getSetup() const { return impl->currentSetup; }

std::string DeviceManager::setSetup (const DeviceSetup& d, bool treatAsChosen)
{
    // An unchanged setup against a live device is a no-op: no broadcast, pending
    // untouched (parity with the wrapped manager's short-circuit).
    if (sameSetup (d, impl->currentSetup) && impl->currentDevice != nullptr && impl->currentDevice->isOpen())
        return {};

    impl->deviceChangePending.store (true, std::memory_order_release);
    return impl->openDeviceFromSetup (d, treatAsChosen);
}

void DeviceManager::addCallback (IODeviceCallback* callback) { impl->addCallback (callback); }

void DeviceManager::removeCallback (IODeviceCallback* callback) { impl->removeCallback (callback); }

void DeviceManager::closeDevice()
{
    // An explicit close is never a disconnection. Nothing to arm when there is
    // no device to close.
    if (impl->currentDevice != nullptr)
    {
        impl->deviceChangePending.store (true, std::memory_order_release);
        impl->currentDevice->stop();
        impl->currentDevice->close();
        impl->currentDevice.reset();
        impl->broadcast();
    }
}

void DeviceManager::addChangeListener (void* owner, std::function<void()> onChange)
{
    if (owner != nullptr) impl->listeners[owner] = std::move (onChange);
}

void DeviceManager::removeChangeListener (void* owner) { impl->listeners.erase (owner); }

void DeviceManager::notifyChange() { impl->fireListeners(); }

void DeviceManager::setDeviceTypesForTest (std::vector<std::unique_ptr<IODeviceType>> types)
{
    impl->types = std::move (types);
    impl->backendsRegistered = true;
}
} // namespace duskstudio::device
