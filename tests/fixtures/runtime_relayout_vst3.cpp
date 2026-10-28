#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/common/pluginview.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <algorithm>

namespace Steinberg::Vst
{
namespace
{
constexpr ParamID kExpandOutputs = 100;
constexpr ParamID kLatencyMode = 101;
constexpr ParamID kTouchReport = 102;
constexpr ParamID kRepeatIoOnActivate = 103;
constexpr ParamID kFlipOutputsOnActivate = 104;
constexpr ParamID kAnnounceLatencyChanges = 105;
constexpr ParamID kReportTransport = 106;
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
        parameters.addParameter (STR16 ("Touch Report"), nullptr, 0, 0.0,
                                 ParameterInfo::kCanAutomate, kTouchReport);
        parameters.addParameter (STR16 ("Repeat IO On Activate"), nullptr, 1, 0.0,
                                 ParameterInfo::kNoFlags, kRepeatIoOnActivate);
        parameters.addParameter (STR16 ("Flip Outputs On Activate"), nullptr, 1, 0.0,
                                 ParameterInfo::kNoFlags, kFlipOutputsOnActivate);
        parameters.addParameter (STR16 ("Announce Latency Changes"), nullptr, 1, 0.0,
                                 ParameterInfo::kNoFlags, kAnnounceLatencyChanges);
        parameters.addParameter (STR16 ("Report Transport"), nullptr, 1, 0.0,
                                 ParameterInfo::kNoFlags, kReportTransport);
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

        if (id == kTouchReport)
        {
            // A host can tell an edit the plug-in reported from one it made
            // itself. A real plug-in sends this from its own editor when the
            // user moves a knob; there is no editor here to move one in.
            beginEdit (kTouchReport);
            performEdit (kTouchReport, value);
            endEdit (kTouchReport);
            return result;
        }
        if (id == kLatencyMode)
        {
            const bool changed = highLatency != (value >= 0.5);
            highLatency = value >= 0.5;
            if (changed && announceLatencyChanges && componentHandler)
                componentHandler->restartComponent (RestartFlags::kLatencyChanged);
            return result;
        }
        if (id == kAnnounceLatencyChanges)
        {
            announceLatencyChanges = value >= 0.5;
            return result;
        }
        if (id == kReportTransport)
        {
            reportTransport = value >= 0.5;
            return result;
        }
        if (id == kRepeatIoOnActivate)
        {
            repeatIoOnActivate = value >= 0.5;
            return result;
        }
        if (id == kFlipOutputsOnActivate)
        {
            flipOutputsOnActivate = value >= 0.5;
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

        uint8 stateFlags = 0;
        int32 bytesRead = 0;
        if (state->read (&stateFlags, sizeof (stateFlags), &bytesRead) != kResultOk
            || bytesRead != sizeof (stateFlags))
            return kResultFalse;

        highLatency = (stateFlags & 1u) != 0;
        reportTransport = (stateFlags & 2u) != 0;
        SingleComponentEffect::setParamNormalized (kLatencyMode, highLatency ? 1.0 : 0.0);
        SingleComponentEffect::setParamNormalized (kReportTransport, reportTransport ? 1.0 : 0.0);
        return kResultOk;
    }

    tresult PLUGIN_API getState (IBStream* state) override
    {
        if (state == nullptr)
            return kInvalidArgument;

        uint8 stateFlags = (uint8) ((highLatency ? 1u : 0u) | (reportTransport ? 2u : 0u));
        int32 bytesWritten = 0;
        if (state->write (&stateFlags, sizeof (stateFlags), &bytesWritten) != kResultOk
            || bytesWritten != sizeof (stateFlags))
            return kResultFalse;
        return kResultOk;
    }

    tresult PLUGIN_API setEditorState (IBStream* state) override
    {
        if (state == nullptr) return kInvalidArgument;
        uint8 value = 0;
        int32 count = 0;
        if (state->read (&value, sizeof (value), &count) != kResultOk
            || count != sizeof (value) || value > 1)
            return kResultFalse;
        controllerState = value;
        return kResultOk;
    }

    tresult PLUGIN_API getEditorState (IBStream* state) override
    {
        if (state == nullptr) return kInvalidArgument;
        int32 count = 0;
        return state->write (&controllerState, sizeof (controllerState), &count) == kResultOk
                   && count == sizeof (controllerState)
            ? kResultOk : kResultFalse;
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

    // A plug-in may announce a bus-layout change from inside its own
    // activation: one that changes nothing, or one that really moves its
    // outputs after the host has read them.
    tresult PLUGIN_API setActive (TBool state) override
    {
        const auto result = SingleComponentEffect::setActive (state);
        if (result != kResultOk || ! state || ! componentHandler)
            return result;
        if (flipOutputsOnActivate)
        {
            expanded = ! expanded;
            rebuildBusses();
        }
        if (repeatIoOnActivate || flipOutputsOnActivate)
            componentHandler->restartComponent (RestartFlags::kIoChanged);
        return result;
    }

    tresult PLUGIN_API setProcessing (TBool) override { return kResultOk; }

    tresult PLUGIN_API process (ProcessData& data) override
    {
        const int32 expectedOutputs = expanded ? 3 : 2;
        if (data.symbolicSampleSize != kSample32
            || data.numInputs != 1 || data.inputs == nullptr
            || data.numOutputs < 1 || (! reportTransport && data.numOutputs != expectedOutputs)
            || data.outputs == nullptr)
            return kResultFalse;

        const auto validStereoBus = [] (const AudioBusBuffers& bus)
        {
            return bus.numChannels == 2 && bus.channelBuffers32 != nullptr
                && bus.channelBuffers32[0] != nullptr && bus.channelBuffers32[1] != nullptr;
        };
        if (! validStereoBus (data.inputs[0]))
            return kResultFalse;
        for (int32 bus = 0; bus < data.numOutputs; ++bus)
            if (! validStereoBus (data.outputs[bus]) && (! reportTransport || bus == 0))
                return kResultFalse;

        for (int32 channel = 0; channel < 2; ++channel)
            std::copy_n (data.inputs[0].channelBuffers32[channel], data.numSamples,
                         data.outputs[0].channelBuffers32[channel]);
        for (int32 bus = 1; bus < data.numOutputs; ++bus)
            if (validStereoBus (data.outputs[bus]))
                for (int32 channel = 0; channel < 2; ++channel)
                    std::fill_n (data.outputs[bus].channelBuffers32[channel], data.numSamples, 0.0f);
        if (reportTransport)
        {
            float report[11] {};
            if (const auto* transport = data.processContext)
            {
                report[0] = 1.0f;
                report[1] = (float) transport->sampleRate;
                report[2] = (float) transport->projectTimeSamples;
                report[3] = (float) transport->projectTimeMusic;
                report[4] = (float) transport->barPositionMusic;
                report[5] = (float) transport->tempo;
                report[6] = (float) transport->timeSigNumerator;
                report[7] = (float) transport->timeSigDenominator;
                report[8] = (float) transport->state;
                report[9] = (float) transport->cycleStartMusic;
                report[10] = (float) transport->cycleEndMusic;
            }
            for (int32 channel = 0; channel < 2; ++channel)
            {
                auto* output = data.outputs[0].channelBuffers32[channel];
                std::fill_n (output, data.numSamples, 0.0f);
                std::copy_n (report, std::min (data.numSamples, (int32) 11), output);
            }
        }
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

    uint8 controllerState = 0;
    bool expanded = false;
    bool highLatency = false;
    bool announceLatencyChanges = false;
    bool repeatIoOnActivate = false;
    bool flipOutputsOnActivate = false;
    bool reportTransport = false;
};
} // namespace
} // namespace Steinberg::Vst

BEGIN_FACTORY_DEF ("Dusk Audio", "https://dusk.audio", "support@dusk.audio")
    DEF_CLASS2 (INLINE_UID (0x18FF00B1, 0xA0B44BCA, 0x934C196E, 0x051E85B2),
                PClassInfo::kManyInstances, kVstAudioEffectClass,
                "Dusk Runtime Relayout Fixture", 0, "Fx", "1.0.0", kVstVersionString,
                Steinberg::Vst::RuntimeRelayoutEffect::createInstance)
END_FACTORY
