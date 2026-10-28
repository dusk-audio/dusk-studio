#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "StateProbeVst3.h"

#include <algorithm>
#include <cmath>

namespace Steinberg::Vst
{
namespace
{
namespace Probe = duskstudio::test::stateprobevst3;

// The processor keeps its own copy of the gain, changed only by parameter
// changes delivered to process() and saved as the component state, the way a
// processor separate from its controller does.
class StateProbeEffect final : public SingleComponentEffect
{
public:
    static FUnknown* createInstance (void*)
    {
        return static_cast<IAudioProcessor*> (new StateProbeEffect);
    }

    tresult PLUGIN_API initialize (FUnknown* context) override
    {
        const auto result = SingleComponentEffect::initialize (context);
        if (result != kResultOk) return result;
        addAudioInput (STR16 ("Input"), SpeakerArr::kStereo);
        addAudioOutput (STR16 ("Output"), SpeakerArr::kStereo);
        parameters.addParameter (STR16 ("Gain"), nullptr, 0, Probe::kDefaultGain,
                                 ParameterInfo::kCanAutomate, Probe::kGain);
        parameters.addParameter (STR16 ("Meter"), nullptr, 0, 0.0,
                                 ParameterInfo::kIsReadOnly, Probe::kMeter);
        parameters.addParameter (STR16 ("Flushes"), nullptr, 0, 0.0,
                                 ParameterInfo::kIsReadOnly, Probe::kFlushes);
        parameters.addParameter (STR16 ("Queue Errors"), nullptr, 0, 0.0,
                                 ParameterInfo::kIsReadOnly, Probe::kQueueErrors);
        parameters.addParameter (STR16 ("Activations"), nullptr, 0, 0.0,
                                 ParameterInfo::kIsReadOnly, Probe::kActivations);
        parameters.addParameter (STR16 ("Refuse Flushes"), nullptr, 1, 0.0,
                                 ParameterInfo::kNoFlags, Probe::kRefuseFlushes);
        return kResultOk;
    }

    tresult PLUGIN_API setParamNormalized (ParamID id, ParamValue value) override
    {
        if (id == Probe::kRefuseFlushes) refuseFlushes = value >= 0.5;
        return SingleComponentEffect::setParamNormalized (id, value);
    }

    ParamValue PLUGIN_API getParamNormalized (ParamID id) override
    {
        switch (id)
        {
            case Probe::kFlushes:     return (double) flushes / Probe::kCounterScale;
            case Probe::kQueueErrors: return (double) queueErrors / Probe::kCounterScale;
            case Probe::kActivations: return (double) activations / Probe::kCounterScale;
            default:                  return SingleComponentEffect::getParamNormalized (id);
        }
    }

    tresult PLUGIN_API setActive (TBool state) override
    {
        if (state) ++activations;
        active = state != 0;
        return SingleComponentEffect::setActive (state);
    }

    tresult PLUGIN_API setProcessing (TBool) override { return kResultOk; }

    tresult PLUGIN_API process (ProcessData& data) override
    {
        if (! active) ++queueErrors;
        if (data.numSamples == 0)
        {
            if (data.numInputs != 0 || data.numOutputs != 0) ++queueErrors;
            ++flushes;
            if (refuseFlushes) return kResultFalse;
        }
        if (auto* changes = data.inputParameterChanges)
            for (int32 i = 0; i < changes->getParameterCount(); ++i)
            {
                auto* queue = changes->getParameterData (i);
                if (queue == nullptr || queue->getParameterId() != Probe::kGain) continue;
                int32 offset = 0;
                ParamValue value = 0.0;
                if (queue->getPoint (queue->getPointCount() - 1, offset, value) == kResultTrue)
                    dspGain = value;
            }
        if (data.numSamples == 0) return kResultOk;

        if (data.numInputs != 1 || data.numOutputs != 1 || data.inputs == nullptr
            || data.outputs == nullptr || data.inputs[0].numChannels != 2
            || data.outputs[0].numChannels != 2)
            return kResultFalse;
        float peak = 0.0f;
        for (int32 channel = 0; channel < 2; ++channel)
        {
            const auto* in = data.inputs[0].channelBuffers32[channel];
            auto* out = data.outputs[0].channelBuffers32[channel];
            for (int32 i = 0; i < data.numSamples; ++i)
            {
                peak = std::max (peak, std::abs (in[i]));
                out[i] = in[i];
            }
        }

        auto* outChanges = data.outputParameterChanges;
        if (outChanges == nullptr) return kResultOk;
        report (*outChanges, Probe::kMeter, 0, peak);
        report (*outChanges, Probe::kMeter, data.numSamples - 1, peak);
        const float trigger = data.inputs[0].channelBuffers32[0][0];
        if (trigger == Probe::kReportGain)
        {
            dspGain = Probe::kReportedGain;
            report (*outChanges, Probe::kGain, 0, dspGain);
        }
        else if (trigger == Probe::kReportUndeclared)
            report (*outChanges, Probe::kUndeclared, 0, 1.0);
        else if (trigger == Probe::kReportEmptyQueue)
        {
            int32 index = -1;
            if (outChanges->addParameterData (Probe::kGain, index) == nullptr) ++queueErrors;
        }
        return kResultOk;
    }

    tresult PLUGIN_API getState (IBStream* state) override
    {
        if (state == nullptr) return kInvalidArgument;
        int32 written = 0;
        return state->write (&dspGain, sizeof (dspGain), &written) == kResultOk
                   && written == sizeof (dspGain)
            ? kResultOk : kResultFalse;
    }

    tresult PLUGIN_API setState (IBStream* state) override
    {
        if (state == nullptr) return kInvalidArgument;
        double value = 0.0;
        int32 bytesRead = 0;
        if (state->read (&value, sizeof (value), &bytesRead) != kResultOk || bytesRead != sizeof (value)
            || ! (value >= 0.0 && value <= 1.0))
            return kResultFalse;
        dspGain = value;
        SingleComponentEffect::setParamNormalized (Probe::kGain, dspGain);
        return kResultOk;
    }

private:
    // Writes one point, then reads the collector back as a plug-in may.
    void report (IParameterChanges& changes, ParamID id, int32 offset, double value)
    {
        int32 index = -1;
        auto* queue = changes.addParameterData (id, index);
        if (queue == nullptr || index < 0 || changes.getParameterData (index) != queue
            || queue->getParameterId() != id)
        {
            ++queueErrors;
            return;
        }
        int32 pointIndex = -1;
        if (queue->addPoint (offset, value, pointIndex) != kResultTrue || pointIndex < 0)
        {
            ++queueErrors;
            return;
        }
        int32 readOffset = -1;
        ParamValue readValue = -1.0;
        if (queue->getPoint (pointIndex, readOffset, readValue) != kResultTrue
            || readOffset != offset || readValue != value)
            ++queueErrors;
    }

    double dspGain = Probe::kDefaultGain;
    int32 flushes = 0, queueErrors = 0, activations = 0;
    bool active = false, refuseFlushes = false;
};
} // namespace
} // namespace Steinberg::Vst

BEGIN_FACTORY_DEF ("Dusk Audio", "https://dusk.audio", "support@dusk.audio")
    DEF_CLASS2 (INLINE_UID (0x5E0B7C21, 0x9A4F4E3B, 0x8C2D17F0, 0x3B6A91D4),
                PClassInfo::kManyInstances, kVstAudioEffectClass,
                "Dusk State Probe Fixture", 0, "Fx", "1.0.0", kVstVersionString,
                Steinberg::Vst::StateProbeEffect::createInstance)
END_FACTORY
