// The panic-probe and no-window fixture plugins, exercised on their own: the
// scenario suite reads these counters to decide whether a transport panic
// reached the plugin, so the fixtures need coverage of their own.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "foundation/MidiBuffer.h"

#if DUSKSTUDIO_HAS_NATIVE_CLAP
 #include "engine/clap/NativeClapSlot.h"
#endif

#if DUSKSTUDIO_HAS_NATIVE_VST3
 #include "engine/vst3/NativeVst3Slot.h"
 #include "engine/vst3/Vst3Bundle.h"
#endif

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{
constexpr int kBlock = 64;

void addMidi (dusk::MidiBuffer& buffer, std::uint8_t status, std::uint8_t d1,
              std::uint8_t d2)
{
    const std::array<std::uint8_t, 3> bytes { status, d1, d2 };
    buffer.addEvent (bytes.data(), (int) bytes.size(), 0);
}
} // namespace

#if DUSKSTUDIO_HAS_NATIVE_CLAP
namespace
{
union AnyClapEvent
{
    clap_event_header_t header;
    clap_event_note_t   note;
    clap_event_midi_t   midi;
};

// A hand-built CLAP event list: the host picks the note port itself, so
// addressing port 1 (or a wildcard choke) needs the plugin driven directly.
struct ClapEventList
{
    ClapEventList()
    {
        view.ctx = this;
        view.size = [] (const clap_input_events_t* list) -> uint32_t
        { return (uint32_t) static_cast<const ClapEventList*> (list->ctx)->events.size(); };
        view.get = [] (const clap_input_events_t* list, uint32_t index) -> const clap_event_header_t*
        {
            const auto& self = *static_cast<const ClapEventList*> (list->ctx);
            return index < self.events.size() ? &self.events[index].header : nullptr;
        };
    }

    void addNote (uint16_t type, int16_t port, int16_t channel, int16_t key)
    {
        AnyClapEvent event {};
        event.note.header = { (uint32_t) sizeof (clap_event_note_t), 0,
                              CLAP_CORE_EVENT_SPACE_ID, type, 0 };
        event.note.note_id = -1;
        event.note.port_index = port;
        event.note.channel = channel;
        event.note.key = key;
        event.note.velocity = type == CLAP_EVENT_NOTE_ON ? 0.8 : 0.0;
        events.push_back (event);
    }

    void addMidi (uint16_t port, std::uint8_t status, std::uint8_t d1, std::uint8_t d2)
    {
        AnyClapEvent event {};
        event.midi.header = { (uint32_t) sizeof (clap_event_midi_t), 0,
                              CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
        event.midi.port_index = port;
        event.midi.data[0] = status;
        event.midi.data[1] = d1;
        event.midi.data[2] = d2;
        events.push_back (event);
    }

    std::vector<AnyClapEvent> events;
    clap_input_events_t view {};
};

struct ClapProbe
{
    duskstudio::clap::NativeClapSlot slot;
    std::array<float, kBlock> inL {}, inR {}, outL {}, outR {};

    bool load (const char* bundle)
    {
        std::string error;
        return slot.load (std::filesystem::u8path (bundle), 48000.0, kBlock, error);
    }

    const clap_plugin_t* plugin()
    {
        auto* instance = slot.getInstance();
        return instance != nullptr ? instance->getPlugin() : nullptr;
    }

    // Drives the plugin directly so the event list addresses whichever note
    // port the case is about. Returns the block's DC level.
    float runRaw (ClapEventList& list)
    {
        outL.fill (0.0f);
        outR.fill (0.0f);
        float* channels[2] = { outL.data(), outR.data() };
        clap_audio_buffer_t out {};
        out.data32 = channels;
        out.channel_count = 2;

        clap_output_events_t sink {};
        sink.ctx = nullptr;
        sink.try_push = [] (const clap_output_events_t*, const clap_event_header_t*)
        { return true; };

        clap_process_t process {};
        process.steady_time = -1;
        process.frames_count = (uint32_t) kBlock;
        process.audio_outputs = &out;
        process.audio_outputs_count = 1;
        process.in_events = &list.view;
        process.out_events = &sink;

        const auto* p = plugin();
        REQUIRE (p != nullptr);
        REQUIRE (p->process (p, &process) != CLAP_PROCESS_ERROR);
        return outL[kBlock - 1];
    }

    // Drives the plugin the way the mixer does, so the host's own MIDI
    // translation is part of the result.
    float runHosted (const dusk::MidiBuffer& midi)
    {
        inL.fill (0.0f);
        inR.fill (0.0f);
        outL.fill (0.0f);
        outR.fill (0.0f);
        slot.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock, &midi);
        return outL[kBlock - 1];
    }

    double counter (const char* name)
    {
        for (int i = 0; i < slot.paramCount(); ++i)
        {
            const auto* info = slot.paramInfo (i);
            if (info == nullptr || info->name != name) continue;
            double value = 0.0;
            if (slot.getParamValue (info->id, value)) return value;
        }
        return -1.0;
    }
};
} // namespace

TEST_CASE ("panic-probe CLAP fixture loads as an instrument with read-only counters",
           "[clap][fixture][panic]")
{
    ClapProbe probe;
    REQUIRE (probe.load (DUSKSTUDIO_PANIC_PROBE_CLAP_FIXTURE_PATH));
    REQUIRE (probe.slot.isLoadedInstrument());
    REQUIRE (probe.slot.paramCount() == 5);

    for (const char* name : { "voicesHeld", "chokesSeen", "cc120Seen", "cc123Seen",
                              "noteOffsSeen" })
    {
        INFO ("counter: " << name);
        REQUIRE_THAT (probe.counter (name), WithinAbs (0.0, 1.0e-9));
    }

    for (int i = 0; i < probe.slot.paramCount(); ++i)
    {
        const auto* info = probe.slot.paramInfo (i);
        REQUIRE (info != nullptr);
        REQUIRE ((info->flags & CLAP_PARAM_IS_READONLY) != 0);
        REQUIRE ((info->flags & CLAP_PARAM_IS_STEPPED) != 0);
    }
}

TEST_CASE ("panic-probe CLAP fixture tracks voices on both note ports",
           "[clap][fixture][panic]")
{
    ClapProbe probe;
    REQUIRE (probe.load (DUSKSTUDIO_PANIC_PROBE_CLAP_FIXTURE_PATH));

    ClapEventList on;
    on.addNote (CLAP_EVENT_NOTE_ON, 0, 0, 60);
    REQUIRE_THAT (probe.runRaw (on), WithinAbs (0.1, 1.0e-6));
    REQUIRE_THAT (probe.counter ("voicesHeld"), WithinAbs (1.0, 1.0e-9));

    ClapEventList onSecondPort;
    onSecondPort.addNote (CLAP_EVENT_NOTE_ON, 1, 0, 64);
    REQUIRE_THAT (probe.runRaw (onSecondPort), WithinAbs (0.2, 1.0e-6));
    REQUIRE_THAT (probe.counter ("voicesHeld"), WithinAbs (2.0, 1.0e-9));

    ClapEventList off;
    off.addNote (CLAP_EVENT_NOTE_OFF, 0, 0, 60);
    off.addNote (CLAP_EVENT_NOTE_OFF, 1, 0, 64);
    REQUIRE_THAT (probe.runRaw (off), WithinAbs (0.0, 1.0e-6));
    REQUIRE_THAT (probe.counter ("voicesHeld"), WithinAbs (0.0, 1.0e-9));
    REQUIRE_THAT (probe.counter ("noteOffsSeen"), WithinAbs (2.0, 1.0e-9));
    REQUIRE_THAT (probe.counter ("chokesSeen"), WithinAbs (0.0, 1.0e-9));
}

TEST_CASE ("panic-probe CLAP fixture chokes voices and survives reset",
           "[clap][fixture][panic]")
{
    ClapProbe probe;
    REQUIRE (probe.load (DUSKSTUDIO_PANIC_PROBE_CLAP_FIXTURE_PATH));

    ClapEventList notes;
    notes.addNote (CLAP_EVENT_NOTE_ON, 0, 0, 60);
    notes.addNote (CLAP_EVENT_NOTE_ON, 0, 1, 64);
    REQUIRE_THAT (probe.runRaw (notes), WithinAbs (0.2, 1.0e-6));

    SECTION ("a targeted choke releases only the named voice")
    {
        ClapEventList choke;
        choke.addNote (CLAP_EVENT_NOTE_CHOKE, 0, 0, 60);
        REQUIRE_THAT (probe.runRaw (choke), WithinAbs (0.1, 1.0e-6));
        REQUIRE_THAT (probe.counter ("voicesHeld"), WithinAbs (1.0, 1.0e-9));
        REQUIRE_THAT (probe.counter ("chokesSeen"), WithinAbs (1.0, 1.0e-9));
        REQUIRE_THAT (probe.counter ("noteOffsSeen"), WithinAbs (0.0, 1.0e-9));
    }

    SECTION ("a wildcard choke releases every voice")
    {
        ClapEventList choke;
        choke.addNote (CLAP_EVENT_NOTE_CHOKE, 0, -1, -1);
        REQUIRE_THAT (probe.runRaw (choke), WithinAbs (0.0, 1.0e-6));
        REQUIRE_THAT (probe.counter ("voicesHeld"), WithinAbs (0.0, 1.0e-9));
        REQUIRE_THAT (probe.counter ("chokesSeen"), WithinAbs (1.0, 1.0e-9));
        REQUIRE_THAT (probe.counter ("noteOffsSeen"), WithinAbs (0.0, 1.0e-9));
    }

    SECTION ("reset drops the voices and keeps the counters")
    {
        ClapEventList choke;
        choke.addNote (CLAP_EVENT_NOTE_CHOKE, 0, -1, -1);
        probe.runRaw (choke);

        ClapEventList revoice;
        revoice.addNote (CLAP_EVENT_NOTE_ON, 0, 0, 72);
        REQUIRE_THAT (probe.runRaw (revoice), WithinAbs (0.1, 1.0e-6));

        const auto* plugin = probe.plugin();
        REQUIRE (plugin != nullptr);
        plugin->reset (plugin);

        ClapEventList idle;
        REQUIRE_THAT (probe.runRaw (idle), WithinAbs (0.0, 1.0e-6));
        REQUIRE_THAT (probe.counter ("voicesHeld"), WithinAbs (0.0, 1.0e-9));
        REQUIRE_THAT (probe.counter ("chokesSeen"), WithinAbs (1.0, 1.0e-9));
    }
}

TEST_CASE ("panic-probe CLAP fixture counts raw MIDI on its MIDI-dialect port",
           "[clap][fixture][panic]")
{
    ClapProbe probe;
    REQUIRE (probe.load (DUSKSTUDIO_PANIC_PROBE_CLAP_FIXTURE_PATH));

    ClapEventList notes;
    notes.addMidi (1, 0x90, 60, 100);
    notes.addMidi (1, 0x90, 64, 100);
    REQUIRE_THAT (probe.runRaw (notes), WithinAbs (0.2, 1.0e-6));

    SECTION ("CC 120 silences every voice")
    {
        ClapEventList allSoundOff;
        allSoundOff.addMidi (1, 0xB0, 120, 0);
        REQUIRE_THAT (probe.runRaw (allSoundOff), WithinAbs (0.0, 1.0e-6));
        REQUIRE_THAT (probe.counter ("cc120Seen"), WithinAbs (1.0, 1.0e-9));
        REQUIRE_THAT (probe.counter ("cc123Seen"), WithinAbs (0.0, 1.0e-9));
        REQUIRE_THAT (probe.counter ("voicesHeld"), WithinAbs (0.0, 1.0e-9));
    }

    SECTION ("CC 123 silences every voice")
    {
        ClapEventList allNotesOff;
        allNotesOff.addMidi (1, 0xB0, 123, 0);
        REQUIRE_THAT (probe.runRaw (allNotesOff), WithinAbs (0.0, 1.0e-6));
        REQUIRE_THAT (probe.counter ("cc123Seen"), WithinAbs (1.0, 1.0e-9));
        REQUIRE_THAT (probe.counter ("cc120Seen"), WithinAbs (0.0, 1.0e-9));
        REQUIRE_THAT (probe.counter ("voicesHeld"), WithinAbs (0.0, 1.0e-9));
    }

    SECTION ("a raw note-off behaves like the CLAP note event")
    {
        ClapEventList off;
        off.addMidi (1, 0x80, 60, 0);
        off.addMidi (1, 0x90, 64, 0);
        REQUIRE_THAT (probe.runRaw (off), WithinAbs (0.0, 1.0e-6));
        REQUIRE_THAT (probe.counter ("noteOffsSeen"), WithinAbs (2.0, 1.0e-9));
        REQUIRE_THAT (probe.counter ("voicesHeld"), WithinAbs (0.0, 1.0e-9));
    }
}

TEST_CASE ("the host turns a hosted panic into a choke on every CLAP note port",
           "[clap][fixture][panic]")
{
    ClapProbe probe;
    REQUIRE (probe.load (DUSKSTUDIO_PANIC_PROBE_CLAP_FIXTURE_PATH));

    dusk::MidiBuffer notes;
    addMidi (notes, 0x90, 60, 100);
    REQUIRE_THAT (probe.runHosted (notes), WithinAbs (0.1, 1.0e-6));
    REQUIRE_THAT (probe.counter ("voicesHeld"), WithinAbs (1.0, 1.0e-9));

    dusk::MidiBuffer panic;
    addMidi (panic, 0xB0, 120, 0);
    REQUIRE_THAT (probe.runHosted (panic), WithinAbs (0.0, 1.0e-6));
    REQUIRE_THAT (probe.counter ("voicesHeld"), WithinAbs (0.0, 1.0e-9));
    // One choke per CLAP note port, which is what makes a CLAP-only port
    // reachable by a panic at all.
    REQUIRE_THAT (probe.counter ("chokesSeen"), WithinAbs (2.0, 1.0e-9));
    REQUIRE_THAT (probe.counter ("noteOffsSeen"), WithinAbs (0.0, 1.0e-9));
}

TEST_CASE ("no-window CLAP fixture passes audio and advertises a GUI it never fills",
           "[clap][fixture][editor]")
{
   #if defined(_WIN32)
    const char* platformApi = CLAP_WINDOW_API_WIN32;
   #elif defined(__APPLE__)
    const char* platformApi = CLAP_WINDOW_API_COCOA;
   #else
    const char* platformApi = CLAP_WINDOW_API_X11;
   #endif

    ClapProbe probe;
    REQUIRE (probe.load (DUSKSTUDIO_NO_WINDOW_CLAP_FIXTURE_PATH));
    REQUIRE (probe.slot.isLoaded());
    REQUIRE_FALSE (probe.slot.isLoadedInstrument());

    probe.inL.fill (0.25f);
    probe.inR.fill (-0.25f);
    probe.outL.fill (0.0f);
    probe.outR.fill (0.0f);
    probe.slot.processStereo (probe.inL.data(), probe.inR.data(),
                              probe.outL.data(), probe.outR.data(), kBlock);
    REQUIRE_THAT (probe.outL[kBlock - 1], WithinAbs (0.25, 1.0e-6));
    REQUIRE_THAT (probe.outR[kBlock - 1], WithinAbs (-0.25, 1.0e-6));

    const auto* plugin = probe.plugin();
    REQUIRE (plugin != nullptr);
    const auto* gui = static_cast<const clap_plugin_gui_t*> (
        plugin->get_extension (plugin, CLAP_EXT_GUI));
    REQUIRE (gui != nullptr);
    REQUIRE (gui->is_api_supported (plugin, platformApi, false));
    REQUIRE_FALSE (gui->is_api_supported (plugin, platformApi, true));
    REQUIRE (gui->create (plugin, platformApi, false));

    uint32_t width = 0, height = 0;
    REQUIRE (gui->get_size (plugin, &width, &height));
    REQUIRE (width > 0);
    REQUIRE (height > 0);
    REQUIRE (gui->set_size (plugin, width, height));

    clap_window_t window {};
    window.api = platformApi;
    REQUIRE (gui->set_parent (plugin, &window));
    REQUIRE (gui->show (plugin));
    gui->destroy (plugin);
}
#endif // DUSKSTUDIO_HAS_NATIVE_CLAP

#if DUSKSTUDIO_HAS_NATIVE_VST3
namespace
{
struct Vst3Probe
{
    duskstudio::vst3::Vst3Bundle bundle;
    duskstudio::vst3::NativeVst3Slot slot;
    std::array<float, kBlock> inL {}, inR {}, outL {}, outR {};
    std::string heldNotesParam;
    uint32_t heldNotesId = 0;

    bool load()
    {
        const std::filesystem::path module =
            std::filesystem::u8path (DUSKSTUDIO_PANIC_PROBE_VST3_FIXTURE_PATH);
        std::string error;
        if (! bundle.load (module.string(), error)) return false;

        std::string classId;
        for (const auto& plugin : bundle.plugins())
            if (plugin.isInstrument) { classId = plugin.id; break; }
        if (classId.empty()) return false;
        if (! slot.load (module, 48000.0, kBlock, error, classId)) return false;

        for (int i = 0; i < slot.paramCount(); ++i)
            if (const auto* info = slot.paramInfo (i); info != nullptr && info->isReadOnly)
            {
                heldNotesParam = info->name;
                heldNotesId = info->id;
                return true;
            }
        return false;
    }

    float run (const dusk::MidiBuffer* midi)
    {
        inL.fill (0.0f);
        inR.fill (0.0f);
        outL.fill (0.0f);
        outR.fill (0.0f);
        slot.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock, midi);
        return outL[kBlock - 1];
    }

    double heldNotes()
    {
        double value = -1.0;
        slot.getParamValue (heldNotesId, value);
        return value;
    }
};
} // namespace

TEST_CASE ("panic-probe VST3 fixture reports held notes through its read-only parameter",
           "[vst3][fixture][panic]")
{
    Vst3Probe probe;
    REQUIRE (probe.load());
    REQUIRE (probe.slot.isLoadedInstrument());
    REQUIRE (probe.heldNotesParam == "Held Notes");
    REQUIRE (probe.slot.getInstance()->midiCcMappingCount() == 0);

    dusk::MidiBuffer notes;
    addMidi (notes, 0x90, 60, 100);
    addMidi (notes, 0x90, 64, 100);
    REQUIRE_THAT (probe.run (&notes), WithinAbs (0.2, 1.0e-6));
    REQUIRE_THAT (probe.heldNotes(), WithinAbs (2.0 / 16.0, 1.0e-6));

    dusk::MidiBuffer off;
    addMidi (off, 0x80, 60, 0);
    REQUIRE_THAT (probe.run (&off), WithinAbs (0.1, 1.0e-6));
    REQUIRE_THAT (probe.heldNotes(), WithinAbs (1.0 / 16.0, 1.0e-6));
}

TEST_CASE ("the host's note-off fallback silences a VST3 synth with no CC mapping",
           "[vst3][fixture][panic]")
{
    Vst3Probe probe;
    REQUIRE (probe.load());

    dusk::MidiBuffer notes;
    addMidi (notes, 0x90, 60, 100);
    REQUIRE_THAT (probe.run (&notes), WithinAbs (0.1, 1.0e-6));

    dusk::MidiBuffer panic;
    addMidi (panic, 0xB0, 123, 0);
    REQUIRE_THAT (probe.run (&panic), WithinAbs (0.0, 1.0e-6));
    REQUIRE_THAT (probe.heldNotes(), WithinAbs (0.0, 1.0e-6));
}
#endif // DUSKSTUDIO_HAS_NATIVE_VST3
