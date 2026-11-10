#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "MappedVst3Probe.h"

#include <algorithm>
#include <array>

namespace Steinberg::Vst
{
namespace
{
namespace Probe = duskstudio::test::mappedvst3;
namespace EveryCc = duskstudio::test::everyccvst3;
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

// A mapping that says nothing about whether the synth acts on all notes off:
// every controller is assigned, and the voices only end on note-offs. Its
// sustain pedal (CC 64) holds a released voice until the pedal comes up, a
// voice that ends rings on as a tail, and only all sound off (CC 120) cuts a
// tail short. Parameter points and events go in sample order, points first.
class EveryCcProbeInstrument final : public SingleComponentEffect, public IMidiMapping
{
public:
    static FUnknown* createInstance (void*)
    {
        return static_cast<IAudioProcessor*> (new EveryCcProbeInstrument);
    }

    // The same probe reporting a fixed latency, so the host schedules its MIDI
    // that far ahead of the block.
    static FUnknown* createLatentInstance (void*)
    {
        auto* probe = new EveryCcProbeInstrument;
        probe->latencySamples = EveryCc::kLatentSamples;
        return static_cast<IAudioProcessor*> (probe);
    }

    uint32 PLUGIN_API getLatencySamples() override { return latencySamples; }

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
        if (bus != 0 || channel < 0 || channel >= kChannels || controller < 0 || controller >= kCountCtrlNumber)
            return kResultFalse;
        id = kParameterBase + (ParamID) (channel * kCountCtrlNumber + controller);
        return kResultTrue;
    }

    tresult PLUGIN_API initialize (FUnknown* context) override
    {
        const auto result = SingleComponentEffect::initialize (context);
        if (result != kResultOk) return result;
        addEventInput (STR16 ("Event In"), kChannels);
        addAudioOutput (STR16 ("Output"), SpeakerArr::kStereo);
        const std::array<const TChar*, 13> names {
            STR16 ("Held Notes"), STR16 ("Note Offs Seen"), STR16 ("Panic Points Seen"), STR16 ("Retriggers"),
            STR16 ("Sustained Notes"), STR16 ("Pedal Down Channels"), STR16 ("Pedal Lifts Seen"),
            STR16 ("Tails Ringing"), STR16 ("Tails Cut"),
            STR16 ("Modulation"), STR16 ("Expression"), STR16 ("Pitch Bend"), STR16 ("Pressure")
        };
        for (std::size_t i = 0; i < names.size(); ++i)
            parameters.addParameter (names[i], nullptr, i == 0 ? EveryCc::kMaxHeldNotes : kMaxNoteOffsSeen, 0.0,
                                     ParameterInfo::kIsReadOnly, EveryCc::HeldNotes + (ParamID) i);
        // Hosts allocate MIDI parameter queues from the enumerated parameter IDs.
        for (int32 index = 0; index < kChannels * kCountCtrlNumber; ++index)
            parameters.addParameter (STR16 ("MIDI Controller"), nullptr, 0, 0.0,
                                     ParameterInfo::kIsHidden, kParameterBase + (ParamID) index);
        return kResultOk;
    }

    ParamValue PLUGIN_API getParamNormalized (ParamID id) override
    {
        const auto counter = [] (int32 value) { return (double) value / EveryCc::kCounterScale; };
        switch (id)
        {
            case EveryCc::HeldNotes: return (double) std::min (count (Voice::Held), (int32) EveryCc::kMaxHeldNotes) / EveryCc::kMaxHeldNotes;
            case EveryCc::NoteOffsSeen: return counter (noteOffsSeen);
            case EveryCc::PanicPointsSeen: return counter (panicPointsSeen);
            case EveryCc::Retriggers: return counter (retriggers);
            case EveryCc::SustainedNotes: return counter (count (Voice::Sustained));
            case EveryCc::PedalDownChannels: return counter ((int32) pedalDown);
            case EveryCc::PedalLiftsSeen: return counter (pedalLifts);
            case EveryCc::TailsRinging: return counter (count (Voice::Tail));
            case EveryCc::TailsCut: return counter (tailsCut);
            case EveryCc::Modulation: return lastValue[0];
            case EveryCc::Expression: return lastValue[1];
            case EveryCc::PitchBend: return lastValue[2];
            case EveryCc::Pressure: return lastValue[3];
            default: return SingleComponentEffect::getParamNormalized (id);
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

        std::size_t items = 0;
        if (data.inputParameterChanges != nullptr)
            for (int32 i = 0; i < data.inputParameterChanges->getParameterCount(); ++i)
            {
                auto* queue = data.inputParameterChanges->getParameterData (i);
                if (queue == nullptr || queue->getParameterId() < kParameterBase) continue;
                for (int32 p = 0; p < queue->getPointCount() && items < block.size(); ++p)
                {
                    Item item {};
                    item.isPoint = true;
                    item.id = queue->getParameterId();
                    if (queue->getPoint (p, item.offset, item.value) == kResultOk)
                        block[items++] = item;
                }
            }
        if (data.inputEvents != nullptr)
            for (int32 i = 0; i < data.inputEvents->getEventCount() && items < block.size(); ++i)
            {
                Item item {};
                if (data.inputEvents->getEvent (i, item.event) != kResultOk) continue;
                item.offset = item.event.sampleOffset;
                block[items++] = item;
            }
        std::stable_sort (block.begin(), block.begin() + (std::ptrdiff_t) items, [] (const Item& a, const Item& b)
        {
            return a.offset < b.offset || (a.offset == b.offset && a.isPoint && ! b.isPoint);
        });
        for (std::size_t i = 0; i < items; ++i)
            block[i].isPoint ? point (block[i].id, block[i].value) : event (block[i].event);

        for (auto& channel : voices)
            for (auto& voice : channel)
                if (voice.state == Voice::Tail && (voice.tailLeft -= data.numSamples) <= 0)
                    voice.state = Voice::Silent;

        // Hosts that cache controller values need the processor's feedback.
        if (data.outputParameterChanges != nullptr && data.numSamples > 0)
            for (ParamID id = EveryCc::HeldNotes; id <= EveryCc::Pressure; ++id)
            {
                int32 queueIndex = 0;
                if (auto* queue = data.outputParameterChanges->addParameterData (id, queueIndex))
                {
                    int32 pointIndex = 0;
                    queue->addPoint (data.numSamples - 1, getParamNormalized (id), pointIndex);
                }
            }

        const auto sounding = (float) (count (Voice::Held) + count (Voice::Sustained));
        for (int32 channel = 0; channel < 2; ++channel)
            std::fill_n (out.channelBuffers32[channel], data.numSamples, sounding * 0.1f);
        return kResultOk;
    }

private:
    static constexpr int16 kChannels = 16;
    static constexpr ParamID kParameterBase = 20000;
    static constexpr int32 kTailSamples = 12000;

    struct Voice
    {
        enum State : std::uint8_t { Silent, Held, Sustained, Tail };
        State state = Silent;
        int32 tailLeft = 0;
    };
    struct Item
    {
        int32 offset = 0;
        bool isPoint = false;
        ParamID id = 0;
        ParamValue value = 0.0;
        Event event {};
    };

    static bool validNote (int16 channel, int16 pitch) noexcept
    {
        return channel >= 0 && channel < kChannels && pitch >= 0 && pitch < 128;
    }

    int32 count (Voice::State state) const noexcept
    {
        int32 n = 0;
        for (const auto& channel : voices)
            for (const auto& voice : channel)
                n += voice.state == state ? 1 : 0;
        return n;
    }

    void ring (Voice& voice) noexcept
    {
        voice.state = Voice::Tail;
        voice.tailLeft = kTailSamples;
    }

    void point (ParamID id, ParamValue value) noexcept
    {
        const auto index = id - kParameterBase;
        const auto channel = (std::size_t) (index / kCountCtrlNumber);
        const auto controller = (int32) (index % kCountCtrlNumber);
        if (channel >= (std::size_t) kChannels) return;
        if (channel == 0)
        {
            if (controller == kCtrlModWheel) lastValue[0] = value;
            else if (controller == kCtrlExpression) lastValue[1] = value;
            else if (controller == kPitchBend) lastValue[2] = value;
            else if (controller == kAfterTouch) lastValue[3] = value;
        }
        if (controller == kCtrlAllSoundsOff || controller == kCtrlAllNotesOff)
            panicPointsSeen = std::min (kMaxNoteOffsSeen, panicPointsSeen + 1);
        if (controller == kCtrlAllSoundsOff)
            for (auto& voice : voices[channel])
                if (voice.state == Voice::Tail)
                {
                    voice.state = Voice::Silent;
                    tailsCut = std::min (kMaxNoteOffsSeen, tailsCut + 1);
                }
        if (controller != kCtrlSustainOnOff) return;
        const auto mask = (std::uint16_t) (1u << channel);
        const bool down = value >= 0.5;
        if (! down && (pedalDown & mask) != 0)
        {
            pedalLifts = std::min (kMaxNoteOffsSeen, pedalLifts + 1);
            for (auto& voice : voices[channel])
                if (voice.state == Voice::Sustained) ring (voice);
        }
        pedalDown = (std::uint16_t) (down ? (pedalDown | mask) : (pedalDown & ~mask));
    }

    void event (const Event& e) noexcept
    {
        if (e.type == Event::kNoteOnEvent && validNote (e.noteOn.channel, e.noteOn.pitch))
        {
            auto& voice = voices[(std::size_t) e.noteOn.channel][(std::size_t) e.noteOn.pitch];
            if (voice.state == Voice::Held) retriggers = std::min (kMaxNoteOffsSeen, retriggers + 1);
            voice.state = Voice::Held;
        }
        else if (e.type == Event::kNoteOffEvent && validNote (e.noteOff.channel, e.noteOff.pitch))
        {
            noteOffsSeen = std::min (kMaxNoteOffsSeen, noteOffsSeen + 1);
            auto& voice = voices[(std::size_t) e.noteOff.channel][(std::size_t) e.noteOff.pitch];
            if (voice.state != Voice::Held) return;
            if ((pedalDown & (1u << e.noteOff.channel)) != 0) voice.state = Voice::Sustained;
            else ring (voice);
        }
    }

    std::array<std::array<Voice, 128>, kChannels> voices {};
    std::array<Item, 4096> block {};
    std::uint16_t pedalDown = 0;
    int32 noteOffsSeen = 0, panicPointsSeen = 0, retriggers = 0, pedalLifts = 0, tailsCut = 0;
    std::array<ParamValue, 4> lastValue { 0.0, 1.0, 8192.0 / 16383.0, 0.0 };
    uint32 latencySamples = 0;
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
    DEF_CLASS2 (INLINE_UID (0x4E1B9C27, 0x7A3F4C85, 0xB6D20E91, 0x5C8A13F4),
                PClassInfo::kManyInstances, kVstAudioEffectClass,
                duskstudio::test::everyccvst3::kName, 0, "Instrument", "1.0.0", kVstVersionString,
                Steinberg::Vst::EveryCcProbeInstrument::createInstance)
    DEF_CLASS2 (INLINE_UID (0x1F6A2D83, 0x9C4B47E2, 0x8A35D0C6, 0x2E71B95F),
                PClassInfo::kManyInstances, kVstAudioEffectClass,
                duskstudio::test::everyccvst3::kLatentName, 0, "Instrument", "1.0.0", kVstVersionString,
                Steinberg::Vst::EveryCcProbeInstrument::createLatentInstance)
END_FACTORY
