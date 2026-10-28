// A VST3 effect with thousands of parameters that, on cue, reports every one of
// them in a single block, and counts the output queues the host refused.

#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include "ManyParamsVst3.h"

#include <algorithm>
#include <atomic>

namespace Steinberg::Vst
{
namespace
{
namespace Fixture = duskstudio::test::manyparamsvst3;

class ManyParamsEffect final : public SingleComponentEffect
{
public:
    static FUnknown* createInstance (void*)
    {
        return static_cast<IAudioProcessor*> (new ManyParamsEffect);
    }

    tresult PLUGIN_API initialize (FUnknown* context) override
    {
        const auto result = SingleComponentEffect::initialize (context);
        if (result != kResultOk) return result;
        addAudioInput (STR16 ("Input"), SpeakerArr::kStereo);
        addAudioOutput (STR16 ("Output"), SpeakerArr::kStereo);
        for (std::uint32_t id = 0; id < Fixture::kParameterCount; ++id)
            parameters.addParameter (STR16 ("Value"), nullptr, 0, 0.0, ParameterInfo::kCanAutomate, id);
        parameters.addParameter (STR16 ("Refused Queues"), nullptr, 0, 0.0,
                                 ParameterInfo::kIsReadOnly, Fixture::kRefusedQueues);
        return kResultOk;
    }

    ParamValue PLUGIN_API getParamNormalized (ParamID id) override
    {
        if (id == Fixture::kRefusedQueues)
            return refused.load (std::memory_order_relaxed) / Fixture::kCounterScale;
        return SingleComponentEffect::getParamNormalized (id);
    }

    tresult PLUGIN_API setState (IBStream*) override { return kResultOk; }
    tresult PLUGIN_API getState (IBStream*) override { return kResultOk; }
    tresult PLUGIN_API setProcessing (TBool) override { return kResultOk; }

    tresult PLUGIN_API process (ProcessData& data) override
    {
        if (data.numSamples <= 0 || data.numInputs < 1 || data.numOutputs < 1
            || data.symbolicSampleSize != kSample32)
            return kResultOk;
        const auto& input = data.inputs[0];
        auto& output = data.outputs[0];
        const auto channels = std::min (input.numChannels, output.numChannels);
        if (channels < 1 || input.channelBuffers32 == nullptr || output.channelBuffers32 == nullptr)
            return kResultOk;

        if (input.channelBuffers32[0] != nullptr && input.channelBuffers32[0][0] == Fixture::kReportAll
            && data.outputParameterChanges != nullptr)
        {
            for (std::uint32_t id = 0; id < Fixture::kParameterCount; ++id)
            {
                int32 queueIndex = -1;
                auto* queue = data.outputParameterChanges->addParameterData (id, queueIndex);
                if (queue == nullptr)
                {
                    refused.fetch_add (1, std::memory_order_relaxed);
                    continue;
                }
                int32 pointIndex = -1;
                queue->addPoint (0, Fixture::kReportedValue, pointIndex);
            }
        }

        for (int32 channel = 0; channel < channels; ++channel)
        {
            const auto* in = input.channelBuffers32[channel];
            auto* out = output.channelBuffers32[channel];
            if (in == nullptr || out == nullptr) continue;
            std::copy (in, in + data.numSamples, out);
        }
        return kResultOk;
    }

private:
    std::atomic<int> refused { 0 };
};
}
}

BEGIN_FACTORY_DEF ("Dusk Audio", "https://dusk.audio", "support@dusk.audio")
    DEF_CLASS2 (INLINE_UID (0x3D5E7A19, 0x42C64B80, 0x9E1F0A63, 0x7B28C4D5),
                PClassInfo::kManyInstances, kVstAudioEffectClass,
                "Dusk Many Parameters Fixture", 0, "Fx", "1.0.0", kVstVersionString,
                Steinberg::Vst::ManyParamsEffect::createInstance)
END_FACTORY
