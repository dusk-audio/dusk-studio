#include "AuInstance.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace duskstudio::au
{
namespace
{
constexpr UInt32 kMissingElement = std::numeric_limits<UInt32>::max();

std::string statusText (const char* operation, OSStatus status)
{
    return std::string (operation) + " failed (OSStatus " + std::to_string (status) + ")";
}

UInt32 elementCount (AudioUnit unit, AudioUnitScope scope) noexcept
{
    UInt32 count = 0;
    UInt32 size = sizeof count;
    return AudioUnitGetProperty (unit, kAudioUnitProperty_ElementCount, scope, 0,
                                 &count, &size) == noErr ? count : 0;
}

UInt32 channelCount (AudioUnit unit, AudioUnitScope scope, UInt32 element) noexcept
{
    AudioStreamBasicDescription format {};
    UInt32 size = sizeof format;
    if (AudioUnitGetProperty (unit, kAudioUnitProperty_StreamFormat, scope, element,
                              &format, &size) != noErr)
        return 0;
    return format.mChannelsPerFrame;
}

std::string cfString (CFStringRef value)
{
    if (value == nullptr) return {};
    const auto length = CFStringGetLength (value);
    const auto maximum = CFStringGetMaximumSizeForEncoding (length, kCFStringEncodingUTF8) + 1;
    if (maximum <= 1) return {};
    std::string out (static_cast<std::size_t> (maximum), '\0');
    if (! CFStringGetCString (value, out.data(), maximum, kCFStringEncodingUTF8))
        return {};
    out.resize (std::char_traits<char>::length (out.c_str()));
    return out;
}

std::string parameterLabel (const AudioUnitParameterInfo& info)
{
    if (info.unit == kAudioUnitParameterUnit_CustomUnit) return cfString (info.unitName);
    if (info.unit == kAudioUnitParameterUnit_Percent) return "%";
    if (info.unit == kAudioUnitParameterUnit_Seconds) return "s";
    if (info.unit == kAudioUnitParameterUnit_Hertz) return "Hz";
    if (info.unit == kAudioUnitParameterUnit_Decibels) return "dB";
    if (info.unit == kAudioUnitParameterUnit_Milliseconds) return "ms";
    return {};
}
} // namespace

AuInstance::~AuInstance()
{
    dispose();
}

void AuInstance::dispose() noexcept
{
    deactivate();
    removeListeners();
    if (audioUnit != nullptr)
    {
        AudioComponentInstanceDispose (audioUnit);
        audioUnit = nullptr;
    }
    parameters.clear();
    observedParameters.clear();
    layout = {};
}

bool AuInstance::create (AuBundle& bundle, const std::string& identifier,
                         std::string& errorOut)
{
    dispose();
    if (bundle.component() == nullptr || bundle.plugins().empty())
    {
        errorOut = "empty Audio Unit component";
        return false;
    }
    if (identifier != bundle.plugins().front().id.toString())
    {
        errorOut = "Audio Unit identifier does not match component";
        return false;
    }

    const auto status = AudioComponentInstanceNew (bundle.component(), &audioUnit);
    if (status != noErr || audioUnit == nullptr)
    {
        audioUnit = nullptr;
        errorOut = statusText ("AudioComponentInstanceNew", status);
        return false;
    }

    componentId = bundle.plugins().front().id;
    enumerateParameters();
    installListeners();
    return true;
}

bool AuInstance::configureStream (AudioUnitScope scope, UInt32 element, UInt32 channels,
                                  double sampleRate, std::string& errorOut)
{
    AudioStreamBasicDescription format {};
    format.mSampleRate = sampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked
                        | kAudioFormatFlagIsNonInterleaved;
    format.mBytesPerPacket = sizeof (float);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof (float);
    format.mChannelsPerFrame = std::max<UInt32> (1, channels);
    format.mBitsPerChannel = 8u * sizeof (float);
    const auto status = AudioUnitSetProperty (audioUnit, kAudioUnitProperty_StreamFormat,
                                              scope, element, &format, sizeof format);
    if (status != noErr)
    {
        errorOut = statusText ("stream-format negotiation", status);
        return false;
    }
    return true;
}

bool AuInstance::configureBuses (double sampleRate, std::string& errorOut)
{
    layout = {};
    mainInputElement = 0;
    sidechainInputElement = kMissingElement;
    mainOutputElement = 0;
    outputChannels = 0;

    const bool instrument = AuBundle::isInstrumentType (componentId.type);
    const UInt32 inputCount = elementCount (audioUnit, kAudioUnitScope_Input);
    const UInt32 outputCount = elementCount (audioUnit, kAudioUnitScope_Output);

    if (! instrument && inputCount > 0)
    {
        const UInt32 channels = std::max<UInt32> (
            1, channelCount (audioUnit, kAudioUnitScope_Input, 0));
        if (! configureStream (kAudioUnitScope_Input, 0, channels, sampleRate, errorOut))
            return false;
        layout.mainInIndex = static_cast<int> (layout.inputs.size());
        layout.inputs.push_back ({ hosting::BusInfo::Kind::Audio,
                                   hosting::BusInfo::Direction::Input,
                                   hosting::BusInfo::Role::Main,
                                   static_cast<int> (channels), true, false, "Main Input" });
    }

    if (! instrument && inputCount > 1)
    {
        sidechainInputElement = 1;
        const UInt32 channels = std::max<UInt32> (
            1, channelCount (audioUnit, kAudioUnitScope_Input, 1));
        if (! configureStream (kAudioUnitScope_Input, 1, channels, sampleRate, errorOut))
            return false;
        layout.sidechainInIndex = static_cast<int> (layout.inputs.size());
        layout.inputs.push_back ({ hosting::BusInfo::Kind::Audio,
                                   hosting::BusInfo::Direction::Input,
                                   hosting::BusInfo::Role::Sidechain,
                                   static_cast<int> (channels), true, false, "Sidechain" });
    }

    if (outputCount == 0)
    {
        errorOut = "Audio Unit has no audio output bus";
        return false;
    }
    outputChannels = std::max<UInt32> (
        1, channelCount (audioUnit, kAudioUnitScope_Output, mainOutputElement));
    if (! configureStream (kAudioUnitScope_Output, mainOutputElement, outputChannels,
                           sampleRate, errorOut))
        return false;
    layout.mainOutIndex = static_cast<int> (layout.outputs.size());
    layout.outputs.push_back ({ hosting::BusInfo::Kind::Audio,
                                hosting::BusInfo::Direction::Output,
                                hosting::BusInfo::Role::Main,
                                static_cast<int> (outputChannels), true, false, "Main Output" });

    const bool acceptsMidi = componentId.type == kAudioUnitType_MusicDevice
                          || componentId.type == kAudioUnitType_MusicEffect;
    if (acceptsMidi)
    {
        layout.eventInIndex = static_cast<int> (layout.inputs.size());
        layout.inputs.push_back ({ hosting::BusInfo::Kind::Event,
                                   hosting::BusInfo::Direction::Input,
                                   hosting::BusInfo::Role::Main,
                                   0, true, true, "MIDI Input" });
    }
    layout.isInstrument = instrument;
    return true;
}

bool AuInstance::activate (double sampleRate, int maxBlockFrames, std::string& errorOut)
{
    if (audioUnit == nullptr)
    {
        errorOut = "no Audio Unit instance";
        return false;
    }
    if (sampleRate <= 0.0 || maxBlockFrames <= 0)
    {
        errorOut = "invalid processing specification";
        return false;
    }
    deactivate();

    UInt32 maximum = static_cast<UInt32> (maxBlockFrames);
    auto status = AudioUnitSetProperty (audioUnit, kAudioUnitProperty_MaximumFramesPerSlice,
                                        kAudioUnitScope_Global, 0, &maximum, sizeof maximum);
    if (status != noErr)
    {
        errorOut = statusText ("maximum-frame negotiation", status);
        return false;
    }
    if (! configureBuses (sampleRate, errorOut)) return false;

    AURenderCallbackStruct callback { &AuInstance::inputRenderCallback, this };
    for (UInt32 bus = 0; bus < elementCount (audioUnit, kAudioUnitScope_Input); ++bus)
    {
        status = AudioUnitSetProperty (audioUnit, kAudioUnitProperty_SetRenderCallback,
                                       kAudioUnitScope_Input, bus, &callback, sizeof callback);
        if (status != noErr && ! layout.isInstrument)
        {
            errorOut = statusText ("input render callback", status);
            return false;
        }
    }

    host.setSampleRate (sampleRate);
    const auto& callbacks = host.callbacks();
    AudioUnitSetProperty (audioUnit, kAudioUnitProperty_HostCallbacks,
                          kAudioUnitScope_Global, 0, &callbacks, sizeof callbacks);

    maximumFrames = maxBlockFrames;
    currentSampleRate = sampleRate;
    // We install a silence-producing callback on every advertised input bus,
    // including extra buses the insert contract does not route. Size a unique
    // stripe for the widest one so an in-place render can never make two
    // unconnected channels alias each other.
    silenceChannels = 1;
    for (UInt32 bus = 0; bus < elementCount (audioUnit, kAudioUnitScope_Input); ++bus)
        silenceChannels = std::max (
            silenceChannels,
            std::max<UInt32> (1, channelCount (audioUnit, kAudioUnitScope_Input, bus)));
    silence.assign (static_cast<std::size_t> (silenceChannels)
                        * static_cast<std::size_t> (maximumFrames), 0.0f);
    const auto outputListBytes = offsetof (AudioBufferList, mBuffers)
        + static_cast<std::size_t> (outputChannels) * sizeof (AudioBuffer);
    outputListStorage = std::make_unique<std::byte[]> (outputListBytes);
    std::memset (outputListStorage.get(), 0, outputListBytes);
    outputBufferList()->mNumberBuffers = outputChannels;

    status = AudioUnitInitialize (audioUnit);
    if (status != noErr)
    {
        outputListStorage.reset();
        silence.clear();
        silenceChannels = 0;
        maximumFrames = 0;
        currentSampleRate = 0.0;
        errorOut = statusText ("AudioUnitInitialize", status);
        return false;
    }

    sampleTime = 0.0;
    refreshLatency();
    latencyDirty.store (false, std::memory_order_relaxed);
    active.store (true, std::memory_order_release);
    return true;
}

void AuInstance::deactivate()
{
    active.store (false, std::memory_order_release);
    currentIo = nullptr;
    if (audioUnit != nullptr && maximumFrames > 0)
    {
        AudioUnitReset (audioUnit, kAudioUnitScope_Global, 0);
        AudioUnitUninitialize (audioUnit);
    }
    outputListStorage.reset();
    silence.clear();
    silenceChannels = 0;
    maximumFrames = 0;
    currentSampleRate = 0.0;
    latencySamples.store (0, std::memory_order_relaxed);
}

bool AuInstance::reactivate (double sampleRate, int maxBlockFrames, std::string& errorOut)
{
    deactivate();
    return activate (sampleRate, maxBlockFrames, errorOut);
}

AudioBufferList* AuInstance::outputBufferList() noexcept
{
    return reinterpret_cast<AudioBufferList*> (outputListStorage.get());
}

OSStatus AuInstance::inputRenderCallback (void* ref, AudioUnitRenderActionFlags*,
                                          const AudioTimeStamp*, UInt32 bus,
                                          UInt32 frames, AudioBufferList* data) noexcept
{
    return static_cast<AuInstance*> (ref)->provideInput (bus, frames, data);
}

OSStatus AuInstance::provideInput (UInt32 bus, UInt32 frames, AudioBufferList* data) noexcept
{
    if (currentIo == nullptr || data == nullptr
        || frames > static_cast<UInt32> (maximumFrames)
        || data->mNumberBuffers > silenceChannels)
        return kAudioUnitErr_CannotDoInCurrentContext;

    float* const* source = nullptr;
    int sourceChannels = 0;
    if (bus == mainInputElement)
    {
        source = currentIo->mainIn;
        sourceChannels = currentIo->mainInChannels;
    }
    else if (bus == sidechainInputElement)
    {
        source = currentIo->sidechainIn;
        sourceChannels = currentIo->sidechainInChannels;
    }

    for (UInt32 channel = 0; channel < data->mNumberBuffers; ++channel)
    {
        auto& buffer = data->mBuffers[channel];
        float* input = source != nullptr && channel < static_cast<UInt32> (sourceChannels)
            && source[channel] != nullptr ? source[channel] : nullptr;
        const auto bytes = static_cast<UInt32> (
            static_cast<std::size_t> (frames) * sizeof (float));
        if (buffer.mData != nullptr)
        {
            if (input != nullptr)
            {
                if (buffer.mData != input) std::memcpy (buffer.mData, input, bytes);
            }
            else
                std::memset (buffer.mData, 0, bytes);
        }
        else if (input != nullptr)
            buffer.mData = input;
        else
        {
            // The unit asked us to hand it a pointer, and it may render in place
            // into what we hand over - so an unconnected channel gets its own
            // zeroed stripe, never one shared buffer that every silent channel
            // would alias (and the first in-place write would poison).
            float* stripe = silence.data() + static_cast<std::size_t> (channel)
                * static_cast<std::size_t> (maximumFrames);
            std::memset (stripe, 0, bytes);
            buffer.mData = stripe;
        }
        buffer.mNumberChannels = 1;
        buffer.mDataByteSize = bytes;
    }
    return noErr;
}

void AuInstance::processBlock (const hosting::PortBuffers& io) noexcept
{
    if (! active.load (std::memory_order_acquire) || audioUnit == nullptr
        || io.numFrames <= 0 || io.numFrames > maximumFrames
        || io.mainOut == nullptr || io.mainOutChannels < static_cast<int> (outputChannels))
        return;

    currentIo = &io;
    host.beginBlock (io.transport, io.numFrames);

    if (io.midiIn != nullptr && layout.acceptsMidi())
    {
        for (const auto event : *io.midiIn)
        {
            const UInt32 offset = static_cast<UInt32> (
                std::clamp (event.samplePosition, 0, io.numFrames - 1));
            if (event.numBytes <= 3)
            {
                const UInt32 b0 = event.numBytes > 0 ? event.data[0] : 0;
                const UInt32 b1 = event.numBytes > 1 ? event.data[1] : 0;
                const UInt32 b2 = event.numBytes > 2 ? event.data[2] : 0;
                MusicDeviceMIDIEvent (audioUnit, b0, b1, b2, offset);
            }
            else
            {
                MusicDeviceSysEx (audioUnit, event.data,
                                  static_cast<UInt32> (event.numBytes));
            }
        }
    }

    auto* output = outputBufferList();
    output->mNumberBuffers = outputChannels;
    for (UInt32 channel = 0; channel < outputChannels; ++channel)
    {
        auto& buffer = output->mBuffers[channel];
        buffer.mNumberChannels = 1;
        buffer.mDataByteSize = static_cast<UInt32> (
            static_cast<std::size_t> (io.numFrames) * sizeof (float));
        buffer.mData = io.mainOut[channel];
    }

    AudioTimeStamp timestamp {};
    timestamp.mSampleTime = io.transport != nullptr
        ? static_cast<Float64> (io.transport->timeInSamples) : sampleTime;
    timestamp.mFlags = kAudioTimeStampSampleTimeValid;
    AudioUnitRenderActionFlags flags = 0;
    const auto status = AudioUnitRender (audioUnit, &flags, &timestamp, mainOutputElement,
                                         static_cast<UInt32> (io.numFrames), output);
    if (status != noErr)
        for (UInt32 channel = 0; channel < outputChannels; ++channel)
            std::memset (io.mainOut[channel], 0,
                         static_cast<std::size_t> (io.numFrames) * sizeof (float));
    sampleTime = timestamp.mSampleTime + io.numFrames;
    currentIo = nullptr;
}

bool AuInstance::saveState (std::vector<std::uint8_t>& out) const
{
    out.clear();
    if (audioUnit == nullptr) return false;
    CFPropertyListRef state = nullptr;
    UInt32 size = sizeof state;
    if (AudioUnitGetProperty (audioUnit, kAudioUnitProperty_ClassInfo,
                              kAudioUnitScope_Global, 0, &state, &size) != noErr
        || state == nullptr)
        return false;

    CFErrorRef error = nullptr;
    CFDataRef data = CFPropertyListCreateData (kCFAllocatorDefault, state,
                                               kCFPropertyListBinaryFormat_v1_0,
                                               0, &error);
    CFRelease (state);
    if (error != nullptr) CFRelease (error);
    if (data == nullptr) return false;
    const auto length = CFDataGetLength (data);
    const auto* bytes = CFDataGetBytePtr (data);
    if (length > 0 && bytes != nullptr)
        out.assign (bytes, bytes + length);
    CFRelease (data);
    return ! out.empty();
}

bool AuInstance::loadState (const std::vector<std::uint8_t>& in)
{
    if (audioUnit == nullptr || in.empty()) return false;
    // A copy, not CFDataCreateWithBytesNoCopy: an immutable property list may
    // alias the bytes it was parsed from, and the unit can retain those objects
    // long after this caller's vector dies.
    CFDataRef data = CFDataCreate (kCFAllocatorDefault, in.data(),
                                   static_cast<CFIndex> (in.size()));
    if (data == nullptr) return false;
    CFErrorRef error = nullptr;
    CFPropertyListRef state = CFPropertyListCreateWithData (
        kCFAllocatorDefault, data, kCFPropertyListImmutable, nullptr, &error);
    CFRelease (data);
    if (error != nullptr) CFRelease (error);
    if (state == nullptr) return false;
    const auto status = AudioUnitSetProperty (audioUnit, kAudioUnitProperty_ClassInfo,
                                              kAudioUnitScope_Global, 0,
                                              &state, sizeof state);
    CFRelease (state);
    return status == noErr;
}

const AuInstance::ParamInfo* AuInstance::paramInfo (int index) const noexcept
{
    return index >= 0 && index < static_cast<int> (parameters.size())
        ? &parameters[static_cast<std::size_t> (index)] : nullptr;
}

bool AuInstance::getParamValue (std::uint32_t id, double& out) const
{
    if (audioUnit == nullptr) return false;
    AudioUnitParameterValue value = 0.0f;
    if (AudioUnitGetParameter (audioUnit, id, kAudioUnitScope_Global, 0, &value) != noErr)
        return false;
    out = value;
    return true;
}

void AuInstance::setParamValue (std::uint32_t id, double value)
{
    if (audioUnit == nullptr) return;
    AudioUnitParameter parameter { audioUnit, id, kAudioUnitScope_Global, 0 };
    AUParameterSet (parameterListener, nullptr, &parameter,
                    static_cast<AudioUnitParameterValue> (value), 0);
}

void AuInstance::enumerateParameters()
{
    parameters.clear();
    observedParameters.clear();
    if (audioUnit == nullptr) return;

    UInt32 listSize = 0;
    if (AudioUnitGetPropertyInfo (audioUnit, kAudioUnitProperty_ParameterList,
                                  kAudioUnitScope_Global, 0, &listSize, nullptr) != noErr
        || listSize == 0 || listSize % sizeof (AudioUnitParameterID) != 0)
        return;
    std::vector<AudioUnitParameterID> ids (listSize / sizeof (AudioUnitParameterID));
    if (AudioUnitGetProperty (audioUnit, kAudioUnitProperty_ParameterList,
                              kAudioUnitScope_Global, 0, ids.data(), &listSize) != noErr)
        return;

    for (const auto id : ids)
    {
        AudioUnitParameterInfo info {};
        UInt32 size = sizeof info;
        if (AudioUnitGetProperty (audioUnit, kAudioUnitProperty_ParameterInfo,
                                  kAudioUnitScope_Global, id, &info, &size) != noErr)
            continue;
        ParamInfo parameter;
        parameter.id = id;
        parameter.name = (info.flags & kAudioUnitParameterFlag_HasCFNameString) != 0
            ? cfString (info.cfNameString)
            : std::string (info.name, strnlen (info.name, sizeof info.name));
        parameter.label = parameterLabel (info);
        parameter.minValue = info.minValue;
        parameter.maxValue = info.maxValue;
        parameter.defaultValue = info.defaultValue;
        parameter.writable = (info.flags & kAudioUnitParameterFlag_IsWritable) != 0;
        parameters.push_back (std::move (parameter));
        observedParameters.push_back ({ audioUnit, id, kAudioUnitScope_Global, 0 });

        if ((info.flags & kAudioUnitParameterFlag_CFNameRelease) != 0)
        {
            if (info.cfNameString != nullptr) CFRelease (info.cfNameString);
            if (info.unit == kAudioUnitParameterUnit_CustomUnit && info.unitName != nullptr)
                CFRelease (info.unitName);
        }
    }
}

void AuInstance::installListeners()
{
    if (audioUnit == nullptr) return;
    AudioUnitAddPropertyListener (audioUnit, kAudioUnitProperty_Latency,
                                  &AuInstance::propertyChanged, this);
    if (AUListenerCreate (&AuInstance::parameterChanged, this, CFRunLoopGetMain(),
                          kCFRunLoopDefaultMode, 0.05f, &parameterListener) != noErr)
        parameterListener = nullptr;
    if (parameterListener != nullptr)
        for (auto& parameter : observedParameters)
            AUListenerAddParameter (parameterListener, this, &parameter);
}

void AuInstance::removeListeners() noexcept
{
    if (parameterListener != nullptr)
    {
        AUListenerDispose (parameterListener);
        parameterListener = nullptr;
    }
    if (audioUnit != nullptr)
        AudioUnitRemovePropertyListenerWithUserData (
            audioUnit, kAudioUnitProperty_Latency, &AuInstance::propertyChanged, this);
}

void AuInstance::parameterChanged (void* user, void*, const AudioUnitParameter* parameter,
                                   AudioUnitParameterValue) noexcept
{
    auto& self = *static_cast<AuInstance*> (user);
    if (parameter == nullptr) return;
    for (std::size_t i = 0; i < self.observedParameters.size(); ++i)
        if (self.observedParameters[i].mParameterID == parameter->mParameterID)
        {
            self.lastTouched.store (static_cast<int> (i), std::memory_order_relaxed);
            return;
        }
}

void AuInstance::propertyChanged (void* user, AudioUnit, AudioUnitPropertyID property,
                                  AudioUnitScope, AudioUnitElement) noexcept
{
    if (property == kAudioUnitProperty_Latency)
        static_cast<AuInstance*> (user)->latencyDirty.store (true, std::memory_order_release);
}

void AuInstance::refreshLatency() noexcept
{
    Float64 seconds = 0.0;
    UInt32 size = sizeof seconds;
    if (audioUnit == nullptr || currentSampleRate <= 0.0
        || AudioUnitGetProperty (audioUnit, kAudioUnitProperty_Latency,
                                 kAudioUnitScope_Global, 0, &seconds, &size) != noErr)
        seconds = 0.0;
    latencySamples.store (std::max (0, static_cast<int> (std::lround (
        seconds * currentSampleRate))), std::memory_order_relaxed);
}

bool AuInstance::refreshLatencyIfChanged() noexcept
{
    if (! latencyDirty.exchange (false, std::memory_order_acquire)) return false;
    refreshLatency();
    return true;
}
} // namespace duskstudio::au
