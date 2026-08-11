// macOS / Windows DeviceManager: the JUCE-wrapped implementation, compiled only
// off Linux (CMake gates this TU vs the native DeviceManager.cpp). It wraps a
// JUCE AudioDeviceManager and adapts JUCE's device / callback types behind the
// dusk device API, and keeps the juceManager() hatch the JUCE MIDI fallback
// drives. Moved here essentially verbatim from the pre-native DeviceManager.cpp
// (native-MIDI-tower JuceMidiBackend precedent); allowlisted for that reason.
#include "DeviceManager.h"
#include "IODeviceCallback.h"
#include "../../util/CrashHandler.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <map>

namespace duskstudio::device
{
namespace
{
template <typename Text>
std::string utf8 (const Text& text)
{
    return std::string (text.toRawUTF8());
}

// std::to_string pads a double to six decimals, and every number logged here is
// a rate or a count, so "44100.000000" is pure noise in a support artifact.
template <typename T>
std::string number (T value)
{
    auto text = std::to_string (value);
    if (text.find ('.') == std::string::npos) return text;
    const auto lastKept = text.find_last_not_of ('0');
    text.erase (text[lastKept] == '.' ? lastKept : lastKept + 1);
    return text;
}

template <typename Values>
std::string numberList (const Values& values)
{
    std::string out;
    for (const auto value : values)
    {
        if (! out.empty()) out += ",";
        out += number (value);
    }
    return out;
}

// One snapshot is one writeDiagnostics call. The FileLogger behind it reopens
// and closes the log file per message, so a per-channel call on a 32-in/32-out
// interface stalls the message thread long enough to move the device
// transitions this build exists to observe.
template <typename DeviceType>
void logDeviceTypeSnapshot (DeviceType* type, const char* context)
{
    if (! crash_handler::diagnosticsEnabled() || type == nullptr) return;

    const auto inputs = type->getDeviceNames (true);
    const auto outputs = type->getDeviceNames (false);
    const int defaultInput = type->getDefaultDeviceIndex (true);
    const int defaultOutput = type->getDefaultDeviceIndex (false);

    std::string out = std::string (context) + ": backend=\"" + utf8 (type->getTypeName())
        + "\" inputs=" + std::to_string (inputs.size())
        + " outputs=" + std::to_string (outputs.size())
        + " default-input=" + std::to_string (defaultInput)
        + " default-output=" + std::to_string (defaultOutput);

    for (int i = 0; i < inputs.size(); ++i)
        out += "\n    input[" + std::to_string (i) + "]=\"" + utf8 (inputs[i]) + "\""
             + (i == defaultInput ? " default=yes" : " default=no");
    for (int i = 0; i < outputs.size(); ++i)
        out += "\n    output[" + std::to_string (i) + "]=\"" + utf8 (outputs[i]) + "\""
             + (i == defaultOutput ? " default=yes" : " default=no");

    crash_handler::writeDiagnostics (out);
}

template <typename Manager>
void logCurrentDeviceSnapshot (Manager& manager, const char* context)
{
    if (! crash_handler::diagnosticsEnabled()) return;

    const auto setup = manager.getAudioDeviceSetup();
    std::string out = std::string (context) + ": setup type=\""
        + utf8 (manager.getCurrentAudioDeviceType())
        + "\" input=\"" + utf8 (setup.inputDeviceName) + "\" output=\""
        + utf8 (setup.outputDeviceName) + "\" rate=" + number (setup.sampleRate)
        + " buffer=" + std::to_string (setup.bufferSize)
        + " default-input-channels=" + (setup.useDefaultInputChannels ? "yes" : "no")
        + " default-output-channels=" + (setup.useDefaultOutputChannels ? "yes" : "no");

    auto* device = manager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        crash_handler::writeDiagnostics (out + "\n    current device=(none)");
        return;
    }

    out += std::string ("\n    current device=\"") + utf8 (device->getName())
        + "\" type=\"" + utf8 (device->getTypeName()) + "\" open="
        + (device->isOpen() ? "yes" : "no") + " playing="
        + (device->isPlaying() ? "yes" : "no") + " rate="
        + number (device->getCurrentSampleRate()) + " buffer="
        + std::to_string (device->getCurrentBufferSizeSamples()) + " bit-depth="
        + std::to_string (device->getCurrentBitDepth()) + " input-latency="
        + std::to_string (device->getInputLatencyInSamples()) + " output-latency="
        + std::to_string (device->getOutputLatencyInSamples()) + " xruns="
        + std::to_string (device->getXRunCount()) + " last-error=\""
        + utf8 (device->getLastError()) + "\""
        + "\n    available rates=[" + numberList (device->getAvailableSampleRates())
        + "] buffers=[" + numberList (device->getAvailableBufferSizes()) + "]";

    const auto inputChannels = device->getInputChannelNames();
    const auto outputChannels = device->getOutputChannelNames();
    // Both return a fresh mask by value, so hoist them out of the loops.
    const auto activeInputs = device->getActiveInputChannels();
    const auto activeOutputs = device->getActiveOutputChannels();
    for (int i = 0; i < inputChannels.size(); ++i)
        out += "\n    input-channel[" + std::to_string (i) + "]=\""
             + utf8 (inputChannels[i]) + "\" active=" + (activeInputs[i] ? "yes" : "no");
    for (int i = 0; i < outputChannels.size(); ++i)
        out += "\n    output-channel[" + std::to_string (i) + "]=\""
             + utf8 (outputChannels[i]) + "\" active=" + (activeOutputs[i] ? "yes" : "no");

    crash_handler::writeDiagnostics (out);
}

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

        logCurrentDeviceSnapshot (mgr, "device-change");
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

       #if JUCE_WINDOWS
        // Preference order: ASIO (only present when the SDK was built in) ->
        // WASAPI exclusive -> WASAPI shared -> DirectSound. The default pick lands
        // on the first registered type that enumerates devices.
       #if JUCE_ASIO
        crash_handler::writeDiagnostics ("backend registration: ASIO compiled=yes");
        if (auto* asio = juce::AudioIODeviceType::createAudioIODeviceType_ASIO())
            mgr.addAudioDeviceType (std::unique_ptr<juce::AudioIODeviceType> (asio));
       #else
        crash_handler::writeDiagnostics ("backend registration: ASIO compiled=no");
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

        const auto& types = mgr.getAvailableDeviceTypes();
        if (crash_handler::diagnosticsEnabled())
            crash_handler::writeDiagnostics ("backend registration: available types="
                                              + std::to_string (types.size()));
        for (auto* t : types)
            if (t != nullptr)
            {
                t->scanForDevices();
                logDeviceTypeSnapshot (t, "initial-scan");
            }
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
        if (t != nullptr)
        {
            t->scanForDevices();
            logDeviceTypeSnapshot (t, "rescan");
        }
}

std::string DeviceManager::initialise (int numInputChannels, int numOutputChannels,
                                       const std::string& savedState, bool selectDefaultOnFailure)
{
    impl->registerBackends();

    std::unique_ptr<juce::XmlElement> state;
    if (! savedState.empty())
        state = juce::parseXML (juce::String (savedState));

    if (crash_handler::diagnosticsEnabled())
        crash_handler::writeDiagnostics (
            "initialise: requested-inputs=" + std::to_string (numInputChannels)
            + " requested-outputs=" + std::to_string (numOutputChannels)
            + " saved-state-bytes=" + std::to_string (savedState.size())
            + " saved-state-parse=" + (savedState.empty() ? "not-present"
                                          : (state != nullptr ? "ok" : "failed"))
            + " select-default-on-failure=" + (selectDefaultOnFailure ? "yes" : "no"));
    const auto result = impl->mgr.initialise (numInputChannels, numOutputChannels,
                                              state.get(), selectDefaultOnFailure);
    if (crash_handler::diagnosticsEnabled())
        crash_handler::writeDiagnostics ("initialise: result=\"" + utf8 (result) + "\"");
    logCurrentDeviceSnapshot (impl->mgr, "initialise");
    return result.toStdString();
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

    if (crash_handler::diagnosticsEnabled())
        crash_handler::writeDiagnostics (
            "set-device-type: requested=\"" + typeName + "\" current=\""
            + utf8 (impl->mgr.getCurrentAudioDeviceType()) + "\" will-change="
            + (willChange ? "yes" : "no") + " treat-as-chosen="
            + (treatAsChosen ? "yes" : "no"));
    impl->mgr.setCurrentAudioDeviceType (wanted, treatAsChosen);
    logCurrentDeviceSnapshot (impl->mgr, "set-device-type");
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

    if (crash_handler::diagnosticsEnabled())
        crash_handler::writeDiagnostics (
            "set-setup: input=\"" + d.inputDeviceName + "\" output=\""
            + d.outputDeviceName + "\" rate=" + number (d.sampleRate)
            + " buffer=" + std::to_string (d.bufferSize) + " treat-as-chosen="
            + (treatAsChosen ? "yes" : "no"));
    const auto result = impl->mgr.setAudioDeviceSetup (s, treatAsChosen);
    if (crash_handler::diagnosticsEnabled())
        crash_handler::writeDiagnostics ("set-setup: result=\"" + utf8 (result) + "\"");
    logCurrentDeviceSnapshot (impl->mgr, "set-setup");
    return result.toStdString();
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
    crash_handler::writeDiagnostics ("close-device: requested");
    impl->mgr.closeAudioDevice();
    logCurrentDeviceSnapshot (impl->mgr, "close-device");
}

void DeviceManager::addChangeListener (void* owner, std::function<void()> onChange)
{
    if (owner != nullptr) impl->listeners[owner] = std::move (onChange);
}

void DeviceManager::removeChangeListener (void* owner) { impl->listeners.erase (owner); }

void DeviceManager::notifyChange() { impl->fireListeners(); }

// The native mock suite is Linux-only; off Linux this seam is never referenced.
void DeviceManager::setDeviceTypesForTest (std::vector<std::unique_ptr<IODeviceType>>) {}

#if ! defined(__linux__)
juce::AudioDeviceManager& DeviceManager::juceManager() { return impl->mgr; }
#endif
} // namespace duskstudio::device
