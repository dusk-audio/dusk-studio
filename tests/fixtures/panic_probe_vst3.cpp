#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/vst/ivstevents.h"

#include <algorithm>
#include <array>

namespace Steinberg::Vst
{
namespace
{
constexpr ParamID kHeldNotes = 200;
constexpr int32 kMaxHeldNotes = 16;

class PanicProbeInstrument final : public SingleComponentEffect
{
public:
    static FUnknown* createInstance (void*)
    {
        return static_cast<IAudioProcessor*> (new PanicProbeInstrument);
    }

    tresult PLUGIN_API initialize (FUnknown* context) override
    {
        const auto result = SingleComponentEffect::initialize (context);
        if (result != kResultOk)
            return result;

        addEventInput (STR16 ("Event In"), 16);
        addAudioOutput (STR16 ("Output"), SpeakerArr::kStereo);
        parameters.addParameter (STR16 ("Held Notes"), nullptr, kMaxHeldNotes, 0.0,
                                 ParameterInfo::kIsReadOnly, kHeldNotes);
        held = 0;
        return kResultOk;
    }

    tresult PLUGIN_API setProcessing (TBool) override { return kResultOk; }

    tresult PLUGIN_API process (ProcessData& data) override
    {
        if (data.symbolicSampleSize != kSample32 || data.numInputs != 0
            || data.numOutputs != 1 || data.outputs == nullptr)
            return kResultFalse;

        if (data.inputEvents != nullptr)
        {
            const int32 count = data.inputEvents->getEventCount();
            for (int32 i = 0; i < count; ++i)
            {
                Event event {};
                if (data.inputEvents->getEvent (i, event) != kResultOk)
                    continue;
                if (event.type == Event::kNoteOnEvent)
                    addNote (event.noteOn.channel, event.noteOn.pitch);
                else if (event.type == Event::kNoteOffEvent)
                    removeNote (event.noteOff.channel, event.noteOff.pitch);
            }
        }
        EditController::setParamNormalized (kHeldNotes,
                                            (ParamValue) held / (ParamValue) kMaxHeldNotes);

        auto& out = data.outputs[0];
        if (out.numChannels != 2 || out.channelBuffers32 == nullptr
            || out.channelBuffers32[0] == nullptr || out.channelBuffers32[1] == nullptr)
            return kResultFalse;

        const float dc = (float) held * 0.1f;
        for (int32 channel = 0; channel < 2; ++channel)
            std::fill_n (out.channelBuffers32[channel], data.numSamples, dc);
        return kResultOk;
    }

private:
    // No IMidiMapping and no controller assignment for CC 120/123: the host has
    // to fall back to synthesising note-offs for the notes it tracked, which is
    // the path this fixture exists to exercise.
    struct Note { int16 channel; int16 pitch; };

    void addNote (int16 channel, int16 pitch)
    {
        for (int32 i = 0; i < held; ++i)
            if (notes[(size_t) i].channel == channel && notes[(size_t) i].pitch == pitch)
                return;
        if (held >= kMaxHeldNotes)
            return;
        notes[(size_t) held++] = { channel, pitch };
    }

    void removeNote (int16 channel, int16 pitch)
    {
        int32 kept = 0;
        for (int32 i = 0; i < held; ++i)
        {
            const auto& n = notes[(size_t) i];
            if (n.channel != channel || n.pitch != pitch)
                notes[(size_t) kept++] = n;
        }
        held = kept;
    }

    std::array<Note, (size_t) kMaxHeldNotes> notes {};
    int32 held = 0;
};
} // namespace
} // namespace Steinberg::Vst

BEGIN_FACTORY_DEF ("Dusk Audio", "https://dusk.audio", "support@dusk.audio")
    DEF_CLASS2 (INLINE_UID (0x2C41E7A5, 0x5B7A4D0E, 0x9F3B61C2, 0x7D18A340),
                PClassInfo::kManyInstances, kVstAudioEffectClass,
                "Dusk Panic Probe Fixture", 0, "Instrument", "1.0.0", kVstVersionString,
                Steinberg::Vst::PanicProbeInstrument::createInstance)
END_FACTORY
