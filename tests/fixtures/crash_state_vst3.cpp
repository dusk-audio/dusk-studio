#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/base/ibstream.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace Steinberg::Vst
{
namespace
{
constexpr ParamID kStateGain = 100;
constexpr ParamID kTerminateChild = 101;
constexpr ParamID kLargeState = 102;

class CrashStateEffect final : public SingleComponentEffect
{
public:
    static FUnknown* createInstance (void*)
    {
        return static_cast<IAudioProcessor*> (new CrashStateEffect);
    }

    tresult PLUGIN_API initialize (FUnknown* context) override
    {
        const auto result = SingleComponentEffect::initialize (context);
        if (result != kResultOk) return result;
        addAudioInput (STR16 ("Input"), SpeakerArr::kStereo);
        addAudioOutput (STR16 ("Output"), SpeakerArr::kStereo);
        parameters.addParameter (STR16 ("State Gain"), nullptr, 0, 1.0,
                                 ParameterInfo::kCanAutomate, kStateGain);
        parameters.addParameter (STR16 ("Terminate Child"), nullptr, 1, 0.0,
                                 ParameterInfo::kNoFlags, kTerminateChild);
        parameters.addParameter (STR16 ("Large State"), nullptr, 1, 0.0,
                                 ParameterInfo::kNoFlags, kLargeState);
        return kResultOk;
    }

    tresult PLUGIN_API setParamNormalized (ParamID id, ParamValue value) override
    {
        const auto result = SingleComponentEffect::setParamNormalized (id, value);
        if (result != kResultOk) return result;
        if (id == kStateGain) stateGain.store (static_cast<float> (value), std::memory_order_relaxed);
        if (id == kTerminateChild) terminateChild.store (value >= 0.5, std::memory_order_relaxed);
        if (id == kLargeState) largeState.store (value >= 0.5, std::memory_order_relaxed);
        return result;
    }

    tresult PLUGIN_API setState (IBStream* state) override
    {
        if (state == nullptr) return kInvalidArgument;
        std::array<uint8, 4> bytes {};
        int32 count = 0;
        if (state->read (bytes.data(), static_cast<int32> (bytes.size()), &count) != kResultOk
            || count != static_cast<int32> (bytes.size())) return kResultFalse;
        std::uint32_t bits = 0;
        for (std::size_t index = 0; index < bytes.size(); ++index)
            bits |= static_cast<std::uint32_t> (bytes[index]) << (index * 8);
        float gain = 0.0f;
        static_assert (sizeof (gain) == sizeof (bits));
        std::memcpy (&gain, &bits, sizeof (gain));
        if (! std::isfinite (gain) || gain < 0.0f || gain > 1.0f) return kResultFalse;
        stateGain.store (gain, std::memory_order_relaxed);
        terminateChild.store (false, std::memory_order_relaxed);
        largeState.store (false, std::memory_order_relaxed);
        SingleComponentEffect::setParamNormalized (kStateGain, gain);
        SingleComponentEffect::setParamNormalized (kTerminateChild, 0.0);
        SingleComponentEffect::setParamNormalized (kLargeState, 0.0);
        return kResultOk;
    }

    tresult PLUGIN_API getState (IBStream* state) override
    {
        if (state == nullptr) return kInvalidArgument;
        const float gain = stateGain.load (std::memory_order_relaxed);
        std::uint32_t bits = 0;
        std::memcpy (&bits, &gain, sizeof (gain));
        std::array<uint8, 4> bytes {};
        for (std::size_t index = 0; index < bytes.size(); ++index)
            bytes[index] = static_cast<uint8> (bits >> (index * 8));
        int32 count = 0;
        if (state->write (bytes.data(), static_cast<int32> (bytes.size()), &count) != kResultOk
            || count != static_cast<int32> (bytes.size())) return kResultFalse;
        if (largeState.load (std::memory_order_relaxed))
        {
            std::array<uint8, 4096> padding {};
            for (int block = 0; block < 1025; ++block)
            {
                count = 0;
                if (state->write (padding.data(), static_cast<int32> (padding.size()), &count) != kResultOk
                    || count != static_cast<int32> (padding.size())) return kResultFalse;
            }
        }
        return kResultOk;
    }

    tresult PLUGIN_API setProcessing (TBool) override { return kResultOk; }

    tresult PLUGIN_API process (ProcessData& data) override
    {
        if (terminateChild.load (std::memory_order_relaxed)) std::_Exit (86);
        if (data.symbolicSampleSize != kSample32 || data.numInputs != 1 || data.inputs == nullptr
            || data.numOutputs != 1 || data.outputs == nullptr) return kResultFalse;
        const auto valid = [] (const AudioBusBuffers& bus)
        {
            return bus.numChannels == 2 && bus.channelBuffers32 != nullptr
                && bus.channelBuffers32[0] != nullptr && bus.channelBuffers32[1] != nullptr;
        };
        if (! valid (data.inputs[0]) || ! valid (data.outputs[0])) return kResultFalse;
        const float gain = stateGain.load (std::memory_order_relaxed)
                         * (largeState.load (std::memory_order_relaxed) ? 0.5f : 1.0f);
        for (int32 channel = 0; channel < 2; ++channel)
            for (int32 frame = 0; frame < data.numSamples; ++frame)
                data.outputs[0].channelBuffers32[channel][frame]
                    = data.inputs[0].channelBuffers32[channel][frame] * gain;
        return kResultOk;
    }

private:
    std::atomic<float> stateGain { 1.0f };
    std::atomic<bool> terminateChild { false };
    std::atomic<bool> largeState { false };
};
}
}

BEGIN_FACTORY_DEF ("Dusk Audio", "https://dusk.audio", "support@dusk.audio")
    DEF_CLASS2 (INLINE_UID (0x7F378D44, 0x6D75493B, 0xA4036527, 0xC641196A),
                PClassInfo::kManyInstances, kVstAudioEffectClass,
                "Dusk Crash State Fixture", 0, "Fx", "1.0.0", kVstVersionString,
                Steinberg::Vst::CrashStateEffect::createInstance)
END_FACTORY
