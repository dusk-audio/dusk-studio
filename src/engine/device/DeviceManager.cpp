#include "DeviceManager.h"
#include "IODeviceCallback.h"

#include <juce_audio_devices/juce_audio_devices.h>

#if defined(DUSKSTUDIO_HAS_PIPEWIRE)
 #include "../pipewire/PipeWireAudioIODeviceType.h"
#endif
#if defined(DUSKSTUDIO_HAS_ALSA)
 #include "../alsa/AlsaAudioIODeviceType.h"
#endif

#include <atomic>
#include <map>

namespace duskstudio::device
{
namespace
{
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
    v.reserve ((size_t) a.size());
    for (const auto& s : a) v.push_back (s.toStdString());
    return v;
}

template <typename T>
std::vector<T> toVector (const juce::Array<T>& a)
{
    std::vector<T> v;
    v.reserve ((size_t) a.size());
    for (const auto& x : a) v.push_back (x);
    return v;
}

// dusk IODevice over a juce::AudioIODevice (non-owning: the wrapped juce device
// is owned by the juce::AudioDeviceManager). Repointed as the current device
// changes so query call sites always read the live device.
class JuceDeviceAdapter final : public IODevice
{
public:
    explicit JuceDeviceAdapter (juce::AudioIODevice* d) noexcept : dev (d) {}
    void repoint (juce::AudioIODevice* d) noexcept { dev = d; }

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

    // start()/stop() are driven through the manager's callback path in this seam,
    // not per-device; forward for interface completeness.
    void start (IODeviceCallback*) override {}
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
    juce::AudioIODevice* dev = nullptr;
};

// dusk IODeviceType over a juce::AudioIODeviceType (non-owning: owned by the
// juce::AudioDeviceManager). Used for the selector's + recovery's enumeration.
class JuceDeviceTypeAdapter final : public IODeviceType
{
public:
    explicit JuceDeviceTypeAdapter (juce::AudioIODeviceType* t) noexcept : type (t) {}

    std::string getTypeName() const override { return type->getTypeName().toStdString(); }
    void scanForDevices() override { type->scanForDevices(); }
    std::vector<std::string> getDeviceNames (bool wantInputNames) const override
        { return toStrings (type->getDeviceNames (wantInputNames)); }
    int getDefaultDeviceIndex (bool forInput) const override { return type->getDefaultDeviceIndex (forInput); }
    int getIndexOfDevice (IODevice*, bool) const override { return -1; }

    // Not used in the seam - the juce::AudioDeviceManager constructs devices
    // internally on setSetup(); the native phase implements this.
    std::unique_ptr<IODevice> createDevice (const std::string&, const std::string&) override { return nullptr; }

private:
    juce::AudioIODeviceType* type = nullptr;
};

// juce::AudioIODeviceCallback forwarding to a dusk IODeviceCallback. Registered
// with the juce::AudioDeviceManager; the RT path is a straight forward with a
// context translation, no allocation. Holds its own device adapter for the
// aboutToStart hand-off.
class CallbackBridge final : public juce::AudioIODeviceCallback
{
public:
    CallbackBridge (IODeviceCallback* c, std::atomic<bool>& pendingFlag) noexcept
        : cb (c), deviceChangePending (&pendingFlag) {}

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn,
                                           float* const* out, int numOut, int numSamples,
                                           const juce::AudioIODeviceCallbackContext& ctx) override
    {
        CallbackContext dctx;
        dctx.hostTimeNs = ctx.hostTimeNs;
        cb->audioDeviceIOCallback (in, numIn, out, numOut, numSamples, dctx);
    }

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        // Synchronous counterpart to the change-listener clear: a device that
        // starts and is pulled again before the (async) change broadcast lands
        // would otherwise leave the flag set with no live device to clear it.
        deviceChangePending->store (false, std::memory_order_release);

        adapter.repoint (device);
        cb->audioDeviceAboutToStart (&adapter);
    }

    void audioDeviceStopped() override { cb->audioDeviceStopped(); }

    void audioDeviceError (const juce::String& message) override
        { cb->audioDeviceError (message.toStdString()); }

private:
    IODeviceCallback*  cb;
    std::atomic<bool>* deviceChangePending;
    JuceDeviceAdapter  adapter { nullptr };
};
} // namespace

struct DeviceManager::Impl : private juce::ChangeListener
{
    Impl() { mgr.addChangeListener (this); }
    ~Impl() override
    {
        // Detach every bridge from mgr before the bridge objects die. Members
        // destruct in reverse order (bridges before mgr), so a bridge left
        // registered would leave mgr holding a dangling callback while the
        // device is still streaming.
        for (auto& entry : bridges)
            mgr.removeAudioCallback (entry.second.get());
        bridges.clear();
        mgr.removeChangeListener (this);
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        // A live device means whatever deliberate change closed the previous one
        // has landed. Cleared here rather than from the device-started callback
        // so it does not depend on anyone having registered one.
        if (mgr.getCurrentAudioDevice() != nullptr)
            deviceChangePending.store (false, std::memory_order_release);

        fireListeners();
    }

    void fireListeners()
    {
        // Snapshot the owner keys, not the closures: a listener may add or
        // remove subscribers (e.g. the settings UI relays out). Firing a
        // copied closure whose owner was already removed by an earlier
        // callback would invoke a dead listener. So re-check each owner
        // against the live map and pull its current callback before calling;
        // copy that callback so an owner removing itself mid-call is safe.
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

    juce::AudioDeviceManager mgr;

    // Set by a deliberate device-type / setup change, cleared once a device
    // starts. See DeviceManager::isDeviceChangePending.
    std::atomic<bool> deviceChangePending { false };
    // Keyed by the juce type pointer (stable for the manager's lifetime), so an
    // adapter is created once and never moved or destroyed while handed out - a
    // returned IODeviceType* stays valid across later calls.
    std::map<juce::AudioIODeviceType*, std::unique_ptr<JuceDeviceTypeAdapter>> typeAdapters;
    JuceDeviceAdapter currentDevice { nullptr };
    std::map<IODeviceCallback*, std::unique_ptr<CallbackBridge>> bridges;
    std::map<void*, std::function<void()>> listeners;
    bool backendsRegistered = false;

    JuceDeviceTypeAdapter* adapterFor (juce::AudioIODeviceType* t)
    {
        auto& slot = typeAdapters[t];
        if (! slot) slot = std::make_unique<JuceDeviceTypeAdapter> (t);
        return slot.get();
    }

    void registerBackends()
    {
        if (backendsRegistered) return;
        backendsRegistered = true;

       #if defined(DUSKSTUDIO_HAS_PIPEWIRE)
        mgr.addAudioDeviceType (std::make_unique<PipeWireAudioIODeviceType>());
       #endif
       #if defined(DUSKSTUDIO_HAS_ALSA)
        mgr.addAudioDeviceType (std::make_unique<AlsaAudioIODeviceType>());
       #endif

       #if JUCE_WINDOWS
        // Preference order: ASIO (only present when the SDK was built in) ->
        // WASAPI exclusive -> WASAPI shared -> DirectSound. The default pick lands
        // on the first registered type that enumerates devices.
       #if JUCE_ASIO
        if (auto* asio = juce::AudioIODeviceType::createAudioIODeviceType_ASIO())
            mgr.addAudioDeviceType (std::unique_ptr<juce::AudioIODeviceType> (asio));
       #endif
        if (auto* wasapiExclusive = juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (
                juce::WASAPIDeviceMode::exclusive))
            mgr.addAudioDeviceType (std::unique_ptr<juce::AudioIODeviceType> (wasapiExclusive));
        if (auto* wasapiShared = juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (
                juce::WASAPIDeviceMode::shared))
            mgr.addAudioDeviceType (std::unique_ptr<juce::AudioIODeviceType> (wasapiShared));
        if (auto* directSound = juce::AudioIODeviceType::createAudioIODeviceType_DirectSound())
            mgr.addAudioDeviceType (std::unique_ptr<juce::AudioIODeviceType> (directSound));
       #endif

        for (auto* t : mgr.getAvailableDeviceTypes())
            if (t != nullptr) t->scanForDevices();
    }
};

DeviceManager::DeviceManager() : impl (std::make_unique<Impl>()) {}
DeviceManager::~DeviceManager() = default;

std::vector<IODeviceType*> DeviceManager::getAvailableDeviceTypes()
{
    std::vector<IODeviceType*> out;
    for (auto* t : impl->mgr.getAvailableDeviceTypes())
        if (t != nullptr)
            out.push_back (impl->adapterFor (t));
    return out;
}

void DeviceManager::scanAllDeviceTypes()
{
    for (auto* t : impl->mgr.getAvailableDeviceTypes())
        if (t != nullptr) t->scanForDevices();
}

std::string DeviceManager::initialise (int numInputChannels, int numOutputChannels,
                                       const std::string& savedState, bool selectDefaultOnFailure)
{
    impl->registerBackends();

    std::unique_ptr<juce::XmlElement> state;
    if (! savedState.empty())
        state = juce::parseXML (juce::String (savedState));

    return impl->mgr.initialise (numInputChannels, numOutputChannels,
                                 state.get(), selectDefaultOnFailure).toStdString();
}

std::string DeviceManager::getStateBlob() const
{
    if (auto xml = impl->mgr.createStateXml())
        return xml->toString().toStdString();
    return {};
}

std::string DeviceManager::outputDeviceNameFromState (const std::string& savedState) const
{
    if (savedState.empty()) return {};
    if (auto xml = juce::parseXML (juce::String (savedState)))
        return xml->getStringAttribute ("audioOutputDeviceName",
                   xml->getStringAttribute ("audioInputDeviceName")).toStdString();
    return {};
}

IODevice* DeviceManager::getCurrentDevice()
{
    auto* d = impl->mgr.getCurrentAudioDevice();
    if (d == nullptr) return nullptr;
    impl->currentDevice.repoint (d);
    return &impl->currentDevice;
}

IODeviceType* DeviceManager::getCurrentDeviceType()
{
    auto* t = impl->mgr.getCurrentDeviceTypeObject();
    return t != nullptr ? impl->adapterFor (t) : nullptr;
}

void DeviceManager::setCurrentDeviceType (const std::string& typeName, bool treatAsChosen)
{
    // Only arm for a request that will actually move: the manager ignores an
    // unknown type name or one that is already current, closing nothing and
    // broadcasting nothing. Arming for those would strand the flag set while a
    // device is still live, and swallow the next genuine disconnection.
    const juce::String wanted (typeName);
    bool willChange = wanted != impl->mgr.getCurrentAudioDeviceType();
    if (willChange)
    {
        willChange = false;
        for (auto* t : impl->mgr.getAvailableDeviceTypes())
            if (t != nullptr && t->getTypeName() == wanted) { willChange = true; break; }
    }
    if (willChange)
        impl->deviceChangePending.store (true, std::memory_order_release);

    impl->mgr.setCurrentAudioDeviceType (wanted, treatAsChosen);
}

bool DeviceManager::isDeviceChangePending() const noexcept
{
    return impl->deviceChangePending.load (std::memory_order_acquire);
}

DeviceSetup DeviceManager::getSetup() const
{
    const auto s = impl->mgr.getAudioDeviceSetup();
    DeviceSetup d;
    d.outputDeviceName = s.outputDeviceName.toStdString();
    d.inputDeviceName  = s.inputDeviceName.toStdString();
    d.sampleRate = s.sampleRate;
    d.bufferSize = s.bufferSize;
    d.inputChannels  = fromBig (s.inputChannels);
    d.outputChannels = fromBig (s.outputChannels);
    d.useDefaultInputChannels  = s.useDefaultInputChannels;
    d.useDefaultOutputChannels = s.useDefaultOutputChannels;
    return d;
}

std::string DeviceManager::setSetup (const DeviceSetup& d, bool treatAsChosen)
{
    auto s = impl->mgr.getAudioDeviceSetup();
    s.outputDeviceName = juce::String (d.outputDeviceName);
    s.inputDeviceName  = juce::String (d.inputDeviceName);
    s.sampleRate = d.sampleRate;
    s.bufferSize = d.bufferSize;
    s.inputChannels  = toBig (d.inputChannels);
    s.outputChannels = toBig (d.outputChannels);
    s.useDefaultInputChannels  = d.useDefaultInputChannels;
    s.useDefaultOutputChannels = d.useDefaultOutputChannels;

    // Same reasoning as setCurrentDeviceType: an unchanged setup against a live
    // device is a no-op the manager returns from without broadcasting.
    if (s != impl->mgr.getAudioDeviceSetup() || impl->mgr.getCurrentAudioDevice() == nullptr)
        impl->deviceChangePending.store (true, std::memory_order_release);

    return impl->mgr.setAudioDeviceSetup (s, treatAsChosen).toStdString();
}

void DeviceManager::addCallback (IODeviceCallback* callback)
{
    if (callback == nullptr) return;
    // Claim the map slot before creating + registering the bridge, so a repeat
    // call for the same callback can't register a second bridge that is then
    // dropped (leaving mgr with a dangling pointer).
    auto [it, inserted] = impl->bridges.emplace (callback, nullptr);
    if (! inserted) return;
    it->second = std::make_unique<CallbackBridge> (callback, impl->deviceChangePending);
    impl->mgr.addAudioCallback (it->second.get());
}

void DeviceManager::removeCallback (IODeviceCallback* callback)
{
    auto it = impl->bridges.find (callback);
    if (it == impl->bridges.end()) return;
    impl->mgr.removeAudioCallback (it->second.get());
    impl->bridges.erase (it);
}

void DeviceManager::closeDevice()
{
    // An explicit close is never a disconnection, whoever asked for it. Nothing
    // to arm when there is no device to close.
    if (impl->mgr.getCurrentAudioDevice() != nullptr)
        impl->deviceChangePending.store (true, std::memory_order_release);
    impl->mgr.closeAudioDevice();
}

void DeviceManager::addChangeListener (void* owner, std::function<void()> onChange)
{
    if (owner != nullptr) impl->listeners[owner] = std::move (onChange);
}

void DeviceManager::removeChangeListener (void* owner) { impl->listeners.erase (owner); }

void DeviceManager::notifyChange() { impl->fireListeners(); }

#if ! defined(__linux__)
juce::AudioDeviceManager& DeviceManager::juceManager() { return impl->mgr; }
#endif
} // namespace duskstudio::device
