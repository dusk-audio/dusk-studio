#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "MappedVst3Probe.h"

#include <algorithm>
#include <array>

namespace Steinberg::Vst
{
namespace
{
namespace Probe = duskstudio::test::mappedvst3;
constexpr ParamID kHeldNotes = 200;
constexpr ParamID kNoteOffsSeen = 201;
constexpr int32 kMaxHeldNotes = 16;
constexpr int32 kMaxNoteOffsSeen = 65535;

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
        parameters.addParameter (STR16 ("Note Offs Seen"), nullptr, kMaxNoteOffsSeen, 0.0,
                                 ParameterInfo::kIsReadOnly, kNoteOffsSeen);
        held = 0;
        noteOffsSeen = 0;
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
                {
                    noteOffsSeen = std::min (kMaxNoteOffsSeen, noteOffsSeen + 1);
                    removeNote (event.noteOff.channel, event.noteOff.pitch);
                }
            }
        }
        EditController::setParamNormalized (kHeldNotes,
                                            (ParamValue) held / (ParamValue) kMaxHeldNotes);
        EditController::setParamNormalized (kNoteOffsSeen,
                                            (ParamValue) noteOffsSeen / (ParamValue) kMaxNoteOffsSeen);

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
    int32 noteOffsSeen = 0;
};

class MappedPanicProbeInstrument final : public SingleComponentEffect, public IMidiMapping
{
public:
    static FUnknown* createInstance (void*)
    {
        return static_cast<IAudioProcessor*> (new MappedPanicProbeInstrument);
    }

    tresult PLUGIN_API queryInterface (const TUID iid, void** object) override
    {
        if (FUnknownPrivate::iidEqual (iid, IMidiMapping::iid))
        {
            *object = static_cast<IMidiMapping*> (this);
            addRef();
            return kResultOk;
        }
        return SingleComponentEffect::queryInterface (iid, object);
    }
    uint32 PLUGIN_API addRef() override { return SingleComponentEffect::addRef(); }
    uint32 PLUGIN_API release() override { return SingleComponentEffect::release(); }

    tresult PLUGIN_API getMidiControllerAssignment (int32 bus, int16 channel, CtrlNumber controller, ParamID& id) override
    {
        if (bus != 0 || channel < 0 || channel >= Probe::kChannels) return kResultFalse;
        for (std::size_t i = 0; i < Probe::kControllers.size(); ++i)
            if (controller == Probe::kControllers[i])
            {
                id = Probe::parameterId (channel, (Probe::Controller) i);
                return kResultOk;
            }
        return kResultFalse;
    }

    tresult PLUGIN_API initialize (FUnknown* context) override
    {
        const auto result = SingleComponentEffect::initialize (context);
        if (result != kResultOk) return result;
        addEventInput (STR16 ("Event In"), Probe::kChannels);
        addAudioOutput (STR16 ("Output"), SpeakerArr::kStereo);
        const std::array<const TChar*, 10> names {
            STR16 ("Held Notes"), STR16 ("Note Offs Seen"), STR16 ("Points Seen"), STR16 ("Queue Errors"),
            STR16 ("Modulation Points"), STR16 ("Modulation Offset"), STR16 ("Pitch Points"), STR16 ("Pitch Offset"),
            STR16 ("Pressure Points"), STR16 ("Pressure Offset")
        };
        for (std::size_t i = 0; i < names.size(); ++i)
            parameters.addParameter (names[i], nullptr, i == 0 ? kMaxHeldNotes : kMaxNoteOffsSeen, 0.0,
                                     ParameterInfo::kIsReadOnly, Probe::HeldNotes + (ParamID) i);
        for (std::size_t i = 0; i < values.size(); ++i)
            parameters.addParameter (STR16 ("Mapped Controller"), nullptr, 0, 0.0,
                                     ParameterInfo::kCanAutomate, Probe::kParameterBase + (ParamID) i);
        return kResultOk;
    }

    ParamValue PLUGIN_API getParamNormalized (ParamID id) override
    {
        if (id >= Probe::kParameterBase && id - Probe::kParameterBase < values.size())
            return values[id - Probe::kParameterBase];
        switch (id)
        {
            case Probe::HeldNotes: return (double) held / kMaxHeldNotes;
            case Probe::NoteOffsSeen: return (double) noteOffsSeen / Probe::kCounterScale;
            case Probe::PointsSeen: return (double) pointsSeen / Probe::kCounterScale;
            case Probe::QueueErrors: return (double) queueErrors / Probe::kCounterScale;
            default:
                if (id >= Probe::ModulationPoints && id <= Probe::PressureOffset)
                    return (double) observations[id - Probe::ModulationPoints] / Probe::kCounterScale;
                return SingleComponentEffect::getParamNormalized (id);
        }
    }

    tresult PLUGIN_API setProcessing (TBool) override { return kResultOk; }

    tresult PLUGIN_API process (ProcessData& data) override
    {
        if (data.symbolicSampleSize != kSample32 || data.numInputs != 0
            || data.numOutputs != 1 || data.outputs == nullptr)
            return kResultFalse;
        auto& out = data.outputs[0];
        if (out.numChannels != 2 || out.channelBuffers32 == nullptr
            || out.channelBuffers32[0] == nullptr || out.channelBuffers32[1] == nullptr)
            return kResultFalse;

        pointsSeen = queueErrors = 0;
        observations.fill (0);
        if (data.inputParameterChanges != nullptr)
            for (int32 i = 0; i < data.inputParameterChanges->getParameterCount(); ++i)
            {
                auto* queue = data.inputParameterChanges->getParameterData (i);
                if (queue == nullptr) { ++queueErrors; continue; }
                const auto id = queue->getParameterId();
                if (id < Probe::kParameterBase || id - Probe::kParameterBase >= values.size())
                { ++queueErrors; continue; }
                const auto index = id - Probe::kParameterBase;
                const auto channel = index / Probe::kControllers.size();
                const auto controller = index % Probe::kControllers.size();
                int32 previousOffset = -1;
                for (int32 p = 0; p < queue->getPointCount(); ++p)
                {
                    int32 offset = -1;
                    double value = 0.0;
                    if (queue->getPoint (p, offset, value) != kResultOk || offset <= previousOffset
                        || offset >= data.numSamples || value < 0.0 || value > 1.0)
                    { ++queueErrors; continue; }
                    previousOffset = offset;
                    values[index] = value;
                    ++pointsSeen;
                    if (channel == 0)
                    {
                        const int observation = controller == Probe::Modulation ? 0
                                              : controller == Probe::PitchBend ? 2
                                              : controller == Probe::Pressure ? 4 : -1;
                        if (observation >= 0)
                        {
                            ++observations[(std::size_t) observation];
                            observations[(std::size_t) observation + 1] = offset;
                        }
                    }
                    if (controller == Probe::AllSoundsOff || controller == Probe::AllNotesOff)
                        notes[channel].fill (false);
                }
            }

        if (data.inputEvents != nullptr)
            for (int32 i = 0; i < data.inputEvents->getEventCount(); ++i)
            {
                Event event {};
                if (data.inputEvents->getEvent (i, event) != kResultOk) continue;
                if (event.type == Event::kNoteOnEvent && validNote (event.noteOn.channel, event.noteOn.pitch))
                    notes[(std::size_t) event.noteOn.channel][(std::size_t) event.noteOn.pitch] = true;
                else if (event.type == Event::kNoteOffEvent && validNote (event.noteOff.channel, event.noteOff.pitch))
                {
                    ++noteOffsSeen;
                    notes[(std::size_t) event.noteOff.channel][(std::size_t) event.noteOff.pitch] = false;
                }
            }
        held = 0;
        for (const auto& channel : notes) held += (int32) std::count (channel.begin(), channel.end(), true);
        for (int32 channel = 0; channel < 2; ++channel)
            std::fill_n (out.channelBuffers32[channel], data.numSamples, (float) held * 0.1f);
        return kResultOk;
    }

private:
    static bool validNote (int16 channel, int16 pitch) noexcept
    {
        return channel >= 0 && channel < Probe::kChannels && pitch >= 0 && pitch < 128;
    }
    std::array<double, Probe::kChannels * Probe::kControllers.size()> values {};
    std::array<std::array<bool, 128>, Probe::kChannels> notes {};
    std::array<int32, 6> observations {};
    int32 held = 0, noteOffsSeen = 0, pointsSeen = 0, queueErrors = 0;
};
} // namespace
} // namespace Steinberg::Vst

BEGIN_FACTORY_DEF ("Dusk Audio", "https://dusk.audio", "support@dusk.audio")
    DEF_CLASS2 (INLINE_UID (0x2C41E7A5, 0x5B7A4D0E, 0x9F3B61C2, 0x7D18A340),
                PClassInfo::kManyInstances, kVstAudioEffectClass,
                "Dusk Panic Probe Fixture", 0, "Instrument", "1.0.0", kVstVersionString,
                Steinberg::Vst::PanicProbeInstrument::createInstance)
    DEF_CLASS2 (INLINE_UID (0x65803B4A, 0x381B4A7E, 0x945B209F, 0xB1317A62),
                PClassInfo::kManyInstances, kVstAudioEffectClass,
                duskstudio::test::mappedvst3::kName, 0, "Instrument", "1.0.0", kVstVersionString,
                Steinberg::Vst::MappedPanicProbeInstrument::createInstance)
END_FACTORY
