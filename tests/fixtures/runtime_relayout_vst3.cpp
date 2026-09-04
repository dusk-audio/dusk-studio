#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/common/pluginview.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/base/ibstream.h"

#include <algorithm>

namespace Steinberg::Vst
{
namespace
{
constexpr ParamID kExpandOutputs = 100;
constexpr ParamID kLatencyMode = 101;
bool handlerDetachedBeforeTerminate = false;

class LifecycleProbeView final : public CPluginView
{
public:
    tresult PLUGIN_API setFrame (IPlugFrame* frame) override
    {
        observedFrame = frame;
        return kResultTrue;
    }

    tresult PLUGIN_API getSize (ViewRect* size) override
    {
        if (size == nullptr)
            return kInvalidArgument;
        *size = ViewRect (0, 0,
                          observedFrame == nullptr ? 1 : 0,
                          handlerDetachedBeforeTerminate ? 1 : 0);
        return kResultTrue;
    }

private:
    IPlugFrame* observedFrame = nullptr;
};

class RuntimeRelayoutEffect final : public SingleComponentEffect
{
public:
    static FUnknown* createInstance (void*)
    {
        return static_cast<IAudioProcessor*> (new RuntimeRelayoutEffect);
    }

    tresult PLUGIN_API initialize (FUnknown* context) override
    {
        const auto result = SingleComponentEffect::initialize (context);
        if (result != kResultOk)
            return result;

        parameters.addParameter (STR16 ("Expand Outputs"), nullptr, 1, 0.0,
                                 ParameterInfo::kCanAutomate, kExpandOutputs);
        parameters.addParameter (STR16 ("Latency Mode"), nullptr, 1, 0.0,
                                 ParameterInfo::kCanAutomate, kLatencyMode);
        handlerDetachedBeforeTerminate = false;
        rebuildBusses();
        return kResultOk;
    }

    tresult PLUGIN_API terminate() override
    {
        handlerDetachedBeforeTerminate = componentHandler == nullptr;
        return SingleComponentEffect::terminate();
    }

    IPlugView* PLUGIN_API createView (FIDString) override
    {
        return new LifecycleProbeView;
    }

    tresult PLUGIN_API setParamNormalized (ParamID id, ParamValue value) override
    {
        const auto result = SingleComponentEffect::setParamNormalized (id, value);
        if (result != kResultOk)
            return result;

        if (id == kLatencyMode)
        {
            highLatency = value >= 0.5;
            return result;
        }
        if (id != kExpandOutputs)
            return result;

        const bool shouldExpand = value >= 0.5;
        if (shouldExpand != expanded)
        {
            expanded = shouldExpand;
            rebuildBusses();
            if (componentHandler)
                componentHandler->restartComponent (RestartFlags::kIoChanged);
        }
        return result;
    }

    tresult PLUGIN_API setState (IBStream* state) override
    {
        if (state == nullptr)
            return kInvalidArgument;

        uint8 latencyMode = 0;
        int32 bytesRead = 0;
        if (state->read (&latencyMode, sizeof (latencyMode), &bytesRead) != kResultOk
            || bytesRead != sizeof (latencyMode))
            return kResultFalse;

        highLatency = latencyMode != 0;
        SingleComponentEffect::setParamNormalized (kLatencyMode, highLatency ? 1.0 : 0.0);
        return kResultOk;
    }

    tresult PLUGIN_API getState (IBStream* state) override
    {
        if (state == nullptr)
            return kInvalidArgument;

        uint8 latencyMode = highLatency ? 1 : 0;
        int32 bytesWritten = 0;
        if (state->write (&latencyMode, sizeof (latencyMode), &bytesWritten) != kResultOk
            || bytesWritten != sizeof (latencyMode))
            return kResultFalse;
        return kResultOk;
    }

    uint32 PLUGIN_API getLatencySamples() override
    {
        return highLatency ? 64u : 0u;
    }

    tresult PLUGIN_API activateBus (MediaType type, BusDirection direction,
                                    int32 index, TBool state) override
    {
        if (type == kAudio && direction == kOutput && index > 0 && ! state)
            return kResultFalse;
        return SingleComponentEffect::activateBus (type, direction, index, state);
    }

    tresult PLUGIN_API setProcessing (TBool) override { return kResultOk; }

    tresult PLUGIN_API process (ProcessData& data) override
    {
        const int32 expectedOutputs = expanded ? 3 : 2;
        if (data.symbolicSampleSize != kSample32
            || data.numInputs != 1 || data.inputs == nullptr
            || data.numOutputs != expectedOutputs || data.outputs == nullptr)
            return kResultFalse;

        const auto validStereoBus = [] (const AudioBusBuffers& bus)
        {
            return bus.numChannels == 2 && bus.channelBuffers32 != nullptr
                && bus.channelBuffers32[0] != nullptr && bus.channelBuffers32[1] != nullptr;
        };
        if (! validStereoBus (data.inputs[0]))
            return kResultFalse;
        for (int32 bus = 0; bus < data.numOutputs; ++bus)
            if (! validStereoBus (data.outputs[bus]))
                return kResultFalse;

        for (int32 channel = 0; channel < 2; ++channel)
            std::copy_n (data.inputs[0].channelBuffers32[channel], data.numSamples,
                         data.outputs[0].channelBuffers32[channel]);
        for (int32 bus = 1; bus < data.numOutputs; ++bus)
            for (int32 channel = 0; channel < 2; ++channel)
                std::fill_n (data.outputs[bus].channelBuffers32[channel], data.numSamples, 0.0f);
        return kResultOk;
    }

private:
    void rebuildBusses()
    {
        removeAudioBusses();
        addAudioInput (STR16 ("Input"), SpeakerArr::kStereo);
        addAudioOutput (STR16 ("Main Output"), SpeakerArr::kStereo);
        addRequiredAuxOutput (STR16 ("Required Auxiliary"));
        if (expanded)
            addRequiredAuxOutput (STR16 ("Expanded Auxiliary"));
    }

    void addRequiredAuxOutput (const TChar* name)
    {
        auto* bus = addAudioOutput (name, SpeakerArr::kStereo, kAux, 0);
        bus->setActive (true);
    }

    bool expanded = false;
    bool highLatency = false;
};
} // namespace
} // namespace Steinberg::Vst

BEGIN_FACTORY_DEF ("Dusk Audio", "https://dusk.audio", "support@dusk.audio")
    DEF_CLASS2 (INLINE_UID (0x18FF00B1, 0xA0B44BCA, 0x934C196E, 0x051E85B2),
                PClassInfo::kManyInstances, kVstAudioEffectClass,
                "Dusk Runtime Relayout Fixture", 0, "Fx", "1.0.0", kVstVersionString,
                Steinberg::Vst::RuntimeRelayoutEffect::createInstance)
END_FACTORY
