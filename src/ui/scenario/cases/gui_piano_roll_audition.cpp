#include "../GuiScenarioSupport.h"
#include "../../../session/SessionSerializer.h"
#include "../../../engine/scenario/cases/MidiProbeHarness.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if DUSKSTUDIO_HAS_ALSA
 #include <alsa/asoundlib.h>
#endif

namespace duskstudio::scenario::gui
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_CLAP
constexpr int kRollTrack = 0;
constexpr int kKeyboardTrack = 1;
// What the status bar shows as the region's channel, and so what the keyboard
// plays on.
constexpr int kRegionChannel = 3;
// Far enough in that the playback leg never reaches the region's own note.
constexpr std::int64_t kSeedTick = 15360;
constexpr std::int64_t kGridTick = 960;

struct Heard
{
    int ons = 0, offs = 0, voices = 0, onKey = 0, onVelocity = 0, onChannel = 0, offKey = 0;
};

Heard heardOn (ScenarioContext& ctx, int track)
{
    auto& engine = ctx.engine();
    engine.suspendProcessing();
    const auto read = [&ctx, track] (const char* name)
    { return (int) std::lround (midiprobe::counter (ctx, track, name)); };
    const Heard heard { read ("noteOnsSeen"), read ("noteOffsSeen"), read ("voicesHeld"),
                        read ("lastNoteOnKey"), read ("lastNoteOnVelocity"), read ("lastNoteOnChannel"),
                        read ("lastNoteOffKey") };
    engine.resumeProcessing();
    return heard;
}

std::optional<ScenarioResult> runPianoKeyboardAudition (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no open modal");
    const int keyboardInput = engine.getVirtualKeyboardInputIndex();
    if (keyboardInput < 0) return ScenarioResult::skip ("the MIDI input bank has no on-screen keyboard");
    const auto fixture = ctx.fixture ("panic_probe.clap");
    if (! fixture) return ScenarioResult::skip ("panic_probe.clap did not resolve");
    auto* rollStrip = host.strip (kRollTrack);
    auto* keyboardStrip = host.strip (kKeyboardTrack);
    if (rollStrip == nullptr || keyboardStrip == nullptr) return ScenarioResult::fail ("the probe tracks have no strips");

    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save initial session");
    ctx.keep (session.countInEnabled);
    ctx.cleanup ([&host, &engine, &session, rollStrip, keyboardStrip, originalDir, restore,
                  loop = engine.getTransport().isLoopEnabled(), punch = engine.getTransport().isPunchEnabled(),
                  editMode = session.editMode]
    {
        host.pausePianoFrames (false);
        host.pianoKeyPointer (60, false);
        host.closePiano();
        engine.stop();
        session.editMode = editMode;
        engine.getTransport().setLoopEnabled (loop);
        engine.getTransport().setPunchEnabled (punch);
        session.setTrackArmed (kRollTrack, false);
        session.track (kRollTrack).strip.mute.store (false);
        session.track (kRollTrack).frozen.store (false, std::memory_order_release);
        session.track (kKeyboardTrack).strip.solo.store (false);
        session.recomputeRtCounters();
        for (auto* strip : { rollStrip, keyboardStrip })
        {
            strip->unloadNativePlugins();
            strip->refreshInsertButton();
        }
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
    });

    // The roll's track takes no live input at all: IN off, unarmed, no port,
    // and a channel filter that drops the region's channel. The other track
    // listens to the on-screen keyboard, the route the roll's keyboard must not
    // take.
    MidiRegion region;
    region.lengthInTicks = 2 * kSeedTick;
    region.lengthInSamples = session.ticksToSamples (region.lengthInTicks, engine.getCurrentSampleRate());
    region.notes = { { kRegionChannel, 48, 90, kSeedTick, 480 } };
    auto& rollTrack = session.track (kRollTrack);
    rollTrack.mode.store ((int) Track::Mode::Midi);
    rollTrack.inputMonitor.store (false);
    rollTrack.recordArmed.store (false);
    rollTrack.midiInputIndex.store (-1);
    rollTrack.midiChannel.store (kRegionChannel + 2);
    rollTrack.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::initializer_list<MidiRegion> { region }));
    midiprobe::makeLiveMidiTrack (ctx, kKeyboardTrack, keyboardInput);
    session.recomputeRtCounters();
    engine.getTransport().locate (0);

    for (auto* strip : { rollStrip, keyboardStrip })
    {
        std::string error;
        const bool loaded = strip->loadNativeClap (*fixture, midiprobe::kPanicProbeClapId, error);
        strip->refreshInsertButton();
        if (! ctx.expect (loaded, "a probe instrument did not load: " + error)) return ctx.verdict();
    }
    if (! host.openPiano (kRollTrack, 0)) return ScenarioResult::fail ("piano roll did not open");

    const auto undoSteps = engine.getUndoManager().getUndoDescriptions().size();
    const auto base = std::make_shared<std::uint64_t> (engine.auditionMessagesDelivered());
    const auto delivered = [&engine, base] (int count) -> std::function<bool()>
    {
        return [&engine, base, count] { return engine.auditionMessagesDelivered() >= *base + (std::uint64_t) count; };
    };
    // What the roll's track heard so far, and the last keys it heard start and
    // end; the other track must have heard nothing.
    const auto check = [&ctx] (int ons, int offs, int voices, int onKey, int offKey, const std::string& when)
    {
        const auto got = heardOn (ctx, kRollTrack);
        ctx.expect (got.ons == ons && got.offs == offs && got.voices == voices,
                    when + ": the roll's track heard " + std::to_string (got.ons) + " note-ons and "
                        + std::to_string (got.offs) + " note-offs, holding " + std::to_string (got.voices)
                        + "; expected " + std::to_string (ons) + ", " + std::to_string (offs)
                        + " and " + std::to_string (voices));
        ctx.expect (got.onKey == onKey && got.onVelocity == 100 && got.onChannel == kRegionChannel - 1,
                    when + ": the last note-on was key " + std::to_string (got.onKey) + " at velocity "
                        + std::to_string (got.onVelocity) + " on channel " + std::to_string (got.onChannel + 1)
                        + "; expected key " + std::to_string (onKey) + " at 100 on channel "
                        + std::to_string (kRegionChannel));
        if (offs > 0)
            ctx.expect (got.offKey == offKey, when + ": the last note-off was key " + std::to_string (got.offKey)
                                                  + "; expected " + std::to_string (offKey));
        const auto other = heardOn (ctx, kKeyboardTrack);
        ctx.expect (other.ons == 0 && other.offs == 0,
                    when + ": the on-screen keyboard's track heard the roll's keyboard");
    };
    const auto untouched = [&ctx, &engine, &rollTrack, region, undoSteps]
    {
        const auto& regions = rollTrack.midiRegions.current();
        ctx.expect (regions.size() == 1 && regions.front().notes == region.notes,
                    "playing the keyboard changed the region");
        ctx.expect (engine.getUndoManager().getUndoDescriptions().size() == undoSteps,
                    "playing the keyboard added an undo step");
        ctx.expect (! engine.getRecordManager().isActive(), "playing the keyboard started a recording");
    };
    const auto outDb = [&engine] { return engine.getChannelStrip (kRollTrack).getOutLDb(); };

    // A step's until is what it waits for before it runs.
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&ctx, &host, outDb]
    {
        ctx.expect (outDb() <= midiprobe::kSilentDb, "the roll's track was not silent before a key was pressed");
        ctx.expect (host.pianoKeyPointer (60, true), "the keyboard's C4 key was not under the pointer");
    } });
    steps->push_back ({ 0, [check] { check (1, 0, 1, 60, 0, "pressing C4"); },
        delivered (1), "the pressed key never reached the track", {}, 4000 });
    steps->push_back ({ 0, [] {}, [outDb] { return outDb() > -60.0f; },
        "a key pressed with the transport stopped and IN off was not heard", {}, 4000 });

    // Gliding: each key ends before the next one sounds. Past the keyboard's
    // edge the key at the pointer's height plays on.
    steps->push_back ({ 150, [&ctx, &host]
    { ctx.expect (host.pianoKeyPointer (61, true), "the keyboard's C#4 key was not under the pointer"); } });
    steps->push_back ({ 150, [&ctx, &host]
    { ctx.expect (host.pianoKeyPointer (62, true), "the keyboard's D4 key was not under the pointer"); } });
    steps->push_back ({ 0, [check] { check (3, 2, 1, 62, 61, "gliding up to D4"); },
        delivered (5), "the glide's keys never reached the track", {}, 4000 });
    steps->push_back ({ 150, [&ctx, &host]
    { ctx.expect (host.pianoNotePointer (kGridTick, 64, true), "the grid was not under the pointer"); } });
    steps->push_back ({ 0, [check] { check (4, 3, 1, 64, 62, "dragging out over the grid at E4"); },
        delivered (7), "leaving the keyboard did not follow the pointer's height", {}, 4000 });
    steps->push_back ({ 150, [&host] { host.pianoNotePointer (kGridTick, 64, false); } });
    steps->push_back ({ 0, [check, untouched] { check (4, 4, 0, 64, 64, "releasing over the grid"); untouched(); },
        delivered (8), "releasing the button never ended the note", {}, 4000 });
    steps->push_back ({ 0, [] {}, [outDb] { return outDb() <= midiprobe::kSilentDb; },
        "the released note never died away", {}, 8000 });

    // Mute and solo apply: the instrument still gets the note, the strip lets
    // none of it out. A frozen track's instrument hears nothing at all.
    struct Silenced { const char* what; std::function<void (bool)> apply; int ons, offs; };
    const std::vector<Silenced> silenced {
        { "muted", [&rollTrack] (bool on) { rollTrack.strip.mute.store (on); }, 1, 1 },
        { "soloed out", [&session] (bool on)
          {
              session.track (kKeyboardTrack).strip.solo.store (on);
              session.recomputeRtCounters();
          }, 1, 1 },
        { "frozen", [&rollTrack] (bool on) { rollTrack.frozen.store (on, std::memory_order_release); }, 0, 0 } };
    auto heard = std::make_shared<Heard> (Heard { 4, 4, 0, 64, 0, 0, 64 });
    int sent = 8;
    for (const auto& leg : silenced)
    {
        const std::string what = leg.what;
        steps->push_back ({ 0, [&ctx, &host, leg]
        {
            leg.apply (true);
            ctx.expect (host.pianoKeyPointer (60, true), "the keyboard's C4 key was not under the pointer");
        } });
        steps->push_back ({ 0, [check, heard, leg, what]
        {
            heard->ons += leg.ons;
            if (leg.ons > 0) heard->onKey = 60;
            check (heard->ons, heard->offs, leg.ons, heard->onKey, heard->offKey, "pressing C4 " + what);
        }, delivered (++sent), "the key pressed on a " + what + " track never reached it", {}, 4000 });
        steps->push_back ({ 300, [&ctx, outDb, what]
        { ctx.expect (outDb() <= midiprobe::kSilentDb, "a " + what + " track let the auditioned note through"); } });
        steps->push_back ({ 0, [&host] { host.pianoKeyPointer (60, false); } });
        steps->push_back ({ 0, [check, heard, leg, what]
        {
            heard->offs += leg.offs;
            if (leg.offs > 0) heard->offKey = 60;
            check (heard->ons, heard->offs, 0, heard->onKey, heard->offKey, "releasing C4 " + what);
            leg.apply (false);
        }, delivered (++sent), "the key released on a " + what + " track never reached it", {}, 4000 });
    }
    steps->push_back ({ 200, [&engine] { engine.play(); } });

    // While the transport rolls.
    steps->push_back ({ 200, [&ctx, &engine, &host]
    {
        ctx.expect (engine.getTransport().isPlaying(), "the transport did not start");
        ctx.expect (host.pianoKeyPointer (65, true), "the keyboard's F4 key was not under the pointer");
    } });
    steps->push_back ({ 0, [check] { check (7, 6, 1, 65, 60, "pressing F4 during playback"); },
        delivered (15), "the key pressed during playback never reached the track", {}, 4000 });
    steps->push_back ({ 100, [&host] { host.pianoKeyPointer (65, false); } });
    steps->push_back ({ 0, [&engine, check, untouched]
    {
        check (7, 7, 0, 65, 65, "releasing F4 during playback");
        engine.stop();
        untouched();
    }, delivered (16), "the key released during playback never ended", {}, 4000 });

    // The window losing the keyboard focus ends a held key, since the button's
    // release may never arrive; the release that does come later sends nothing.
    steps->push_back ({ 200, [&ctx, &host]
    { ctx.expect (host.pianoKeyPointer (66, true), "the keyboard's F#4 key was not under the pointer"); } });
    steps->push_back ({ 0, [&ctx, &host, check]
    {
        check (8, 7, 1, 66, 65, "holding F#4");
        ctx.expect (host.pianoKeyboardFocus (false), "the piano roll could not lose the keyboard focus");
    }, delivered (17), "the key pressed before the focus went never reached the track", {}, 4000 });
    steps->push_back ({ 0, [&ctx, &host, check]
    {
        check (8, 8, 0, 66, 66, "losing the focus on F#4");
        ctx.expect (host.pianoKeyboardFocus (true), "the piano roll could not take the keyboard focus back");
        host.pianoKeyPointer (66, false);
    }, delivered (18), "losing the keyboard focus left the held key sounding", {}, 4000 });

    // Closing the editor while a key is held ends it.
    steps->push_back ({ 200, [&ctx, &host]
    { ctx.expect (host.pianoKeyPointer (67, true), "the keyboard's G4 key was not under the pointer"); } });
    steps->push_back ({ 0, [&host, check]
    {
        check (9, 8, 1, 67, 66, "holding G4");
        host.closePiano();
    }, delivered (19), "the key pressed before closing never reached the track", {}, 4000 });
    steps->push_back ({ 0, [check, untouched] { check (9, 9, 0, 67, 67, "closing the editor on G4"); untouched(); },
        delivered (20), "closing the editor left the held key sounding", {}, 4000 });

    const auto stalled = std::make_shared<Heard>();
    steps->push_back ({ 200, [&ctx, &host, &engine, base, stalled]
    {
        host.openPianoRoll (kRollTrack, 0);
        ctx.expect (host.pianoRollOpen(), "the piano roll did not reopen for paused drawing");
        *base = engine.auditionMessagesDelivered();
        *stalled = heardOn (ctx, kRollTrack);
    } });
    steps->push_back ({ 0, [] {}, [&host] { return host.pianoKeyPointer (60, true); },
        "the reopened piano keyboard did not become ready", {}, 4000 });
    steps->push_back ({ 0, [&ctx, &host, &engine, stalled]
    {
        ctx.expect (heardOn (ctx, kRollTrack).voices == 1, "the key before paused drawing was not held");
        ctx.expect (host.pausePianoFrames (true), "the piano roll could not pause drawing");
        const auto frames = host.pianoCompletedFrames();
        const auto deliveredBefore = engine.auditionMessagesDelivered();
        host.pianoKeyPointer (60, false);
        // Resume independently of runSteps, which waits for the queued release to draw.
        ctx.later (250, [&ctx, &host, &engine, stalled, frames, deliveredBefore]
        {
            ctx.expect (host.pianoCompletedFrames() == frames, "the paused piano roll still drew frames");
            ctx.expect (engine.auditionMessagesDelivered() == deliveredBefore,
                        "the queued release was consumed without a frame");
            const auto held = heardOn (ctx, kRollTrack);
            ctx.expect (held.voices == 1 && held.offs == stalled->offs,
                        "the held key ended before drawing resumed");
            host.pausePianoFrames (false);
        });
    }, delivered (1), "the key before paused drawing never reached the track", {}, 4000 });
    steps->push_back ({ 0, [&ctx, untouched, stalled]
    {
        const auto released = heardOn (ctx, kRollTrack);
        ctx.expect (released.voices == 0 && released.ons == stalled->ons + 1 && released.offs == stalled->offs + 1,
                    "resuming drawing did not release the held key exactly once");
        untouched();
    }, delivered (2), "resuming drawing left the audition held", {}, 4000 });

    const auto beforeExtra = std::make_shared<Heard>();
    steps->push_back ({ 200, [&ctx, &host, &engine, base, beforeExtra]
    {
        if (! host.pianoRollOpen()) host.openPianoRoll (kRollTrack, 0);
        ctx.expect (host.pianoRollOpen(), "the piano roll did not reopen for Escape");
        *base = engine.auditionMessagesDelivered();
        *beforeExtra = heardOn (ctx, kRollTrack);
    } });
    steps->push_back ({ 0, [] {}, [&host] { return host.pianoKeyPointer (60, true); },
        "the reopened piano keyboard did not become ready", {}, 4000 });
    steps->push_back ({ 0, [&host] { host.pianoKeyPointer (61, true); },
        delivered (1), "the key before the Escape glide was not heard", {}, 4000 });
    steps->push_back ({ 0, [&ctx, &host]
    { ctx.expect (host.pressPianoRollKey ("Escape"), "Escape did not cancel the glide"); },
        delivered (3), "the Escape glide was not heard", {}, 4000 });
    steps->push_back ({ 0, [&host]
    {
        host.pianoNotePointer (kGridTick, 62, true);
        host.pianoNotePointer (kGridTick, 62, false);
    }, delivered (4), "Escape left the gliding key held", {}, 4000 });
    steps->push_back ({ 200, [&ctx, beforeExtra, untouched]
    {
        const auto after = heardOn (ctx, kRollTrack);
        ctx.expect (after.ons == beforeExtra->ons + 2 && after.offs == beforeExtra->offs + 2 && after.voices == 0,
                    "movement or release after Escape restarted the audition");
        untouched();
    } });

    for (const bool anotherTrack : { false, true })
    {
        const int destination = anotherTrack ? kKeyboardTrack : kRollTrack;
        const int destinationChannel = anotherTrack ? 7 : 5;
        const auto destinationBefore = std::make_shared<Heard>();
        steps->push_back ({ 0, [&ctx, &host, &engine, &session, base, beforeExtra,
                                destinationBefore, region, anotherTrack, destination, destinationChannel]
        {
            auto otherRegion = region;
            otherRegion.notes.front().channel = destinationChannel;
            auto regions = std::make_unique<std::vector<MidiRegion>>();
            if (! anotherTrack) regions->push_back (region);
            regions->push_back (std::move (otherRegion));
            session.track (destination).midiRegions.publish (std::move (regions));
            if (! host.pianoRollOpen()) host.openPianoRoll (kRollTrack, 0);
            ctx.expect (host.pianoRollOpen(), "the source piano roll did not open");
            *base = engine.auditionMessagesDelivered();
            *beforeExtra = heardOn (ctx, kRollTrack);
            *destinationBefore = heardOn (ctx, destination);
        } });
        steps->push_back ({ 0, [] {}, [&host] { return host.pianoKeyPointer (64, true); },
            "the source region's keyboard did not become ready", {}, 4000 });
        steps->push_back ({ 0, [&ctx, &host, destination, anotherTrack]
        {
            host.openPianoRoll (destination, anotherTrack ? 0 : 1);
            ctx.expect (host.pianoRollOpen() && host.pianoRollRegion() == (anotherTrack ? 0 : 1),
                        "the held audition could not change region");
        },
            delivered (1), "the key before changing region was not heard", {}, 4000 });
        steps->push_back ({ 0, [&ctx, beforeExtra]
        {
            const auto after = heardOn (ctx, kRollTrack);
            ctx.expect (after.offs == beforeExtra->offs + 1 && after.voices == 0,
                        "changing region left the old key held");
        }, delivered (2), "changing region did not release its held key", {}, 4000 });
        steps->push_back ({ 0, [] {}, [&host] { return host.pianoKeyPointer (64, false); },
            "the destination region's keyboard did not become ready", {}, 4000 });
        steps->push_back ({ 0, [&host] { host.pianoKeyPointer (65, true); } });
        steps->push_back ({ 0, [&ctx, &host, destination, destinationChannel]
        {
            const auto after = heardOn (ctx, destination);
            ctx.expect (after.onKey == 65 && after.onChannel == destinationChannel - 1 && after.onVelocity == 100,
                        "the new region's audition used the old channel or velocity");
            host.pianoKeyPointer (65, false);
        }, delivered (3), "the new region's audition was not heard", {}, 4000 });
        steps->push_back ({ 200, [&ctx, &host, &rollTrack, destinationBefore, destination, anotherTrack, region]
        {
            const auto after = heardOn (ctx, destination);
            const int expected = anotherTrack ? 1 : 2;
            ctx.expect (after.ons == destinationBefore->ons + expected
                            && after.offs == destinationBefore->offs + expected && after.voices == 0,
                        "a late pointer release duplicated a note-off after changing region");
            rollTrack.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (
                std::initializer_list<MidiRegion> { region }));
            host.openPianoRoll (kRollTrack, 0);
            ctx.expect (host.pianoRollOpen(), "the original piano roll did not reopen");
        }, delivered (4), "the new region's key did not end", {}, 4000 });
    }

    steps->push_back ({ 200, [&ctx, &host, &engine, base, beforeExtra]
    {
        *base = engine.auditionMessagesDelivered();
        *beforeExtra = heardOn (ctx, kRollTrack);
        ctx.expect (host.pressPianoRollKey ("D"), "Draw mode was unavailable");
        host.pianoNotePointer (kGridTick, 60, true);
        host.pianoNotePointer (kGridTick + 240, 60, false);
    } });
    steps->push_back ({ 200, [&ctx, &host, &rollTrack, beforeExtra]
    {
        ctx.expect (rollTrack.midiRegions.current().front().notes.size() == 2,
                    "drawing did not add the stopped note");
        ctx.expect (heardOn (ctx, kRollTrack).ons == beforeExtra->ons,
                    "drawing a stopped note unexpectedly auditioned it");
        host.pianoKeyPointer (62, true);
    } });
    steps->push_back ({ 0, [&host] { host.pianoKeyPointer (62, false); },
        delivered (1), "the keyboard did not audition in Draw mode", {}, 4000 });
    steps->push_back ({ 0, [&ctx, &host, &rollTrack, region]
    {
        ctx.expect (host.pressPianoRollKey ("command + Z"), "the drawn note could not be undone");
        ctx.expect (rollTrack.midiRegions.current().front().notes == region.notes,
                    "keyboard audition changed the drawn note's undo transaction");
        host.pianoNotePointer (kGridTick, 60, true);
    }, delivered (2), "Draw-mode keyboard audition did not end", {}, 4000 });
    steps->push_back ({ 0, [&ctx, &host]
    { ctx.expect (host.pressPianoRollKey ("Escape"), "Escape did not cancel drawing"); },
        [&host] { return host.pianoSelection() == std::vector<int> { 1 }; },
        "the partial draw never began", {}, 4000 });
    steps->push_back ({ 0, [&host] { host.pianoNotePointer (kGridTick, 60, false); } });
    steps->push_back ({ 100, [&ctx, &host, &rollTrack, region]
    {
        ctx.expect (rollTrack.midiRegions.current().front().notes == region.notes,
                    "Escape kept a partially drawn note");
        host.pressPianoRollKey ("G");
    } });
    steps->push_back ({ 200, [&engine, base] { *base = engine.auditionMessagesDelivered() - 20; } });

    // Recording on the roll's own track, IN off: a key played on the
    // on-screen keyboard is the take, and the roll's keyboard is not in it.
    static constexpr int kRecordedKey = 50, kAuditionedKey = 70;
    const auto postKeyboard = [&engine] (bool on)
    {
        const std::uint8_t message[] { (std::uint8_t) (on ? 0x90 : 0x80), (std::uint8_t) kRecordedKey,
                                       (std::uint8_t) (on ? 100 : 0) };
        engine.postVirtualKeyboardMidi (message, 3);
    };
    const auto takeNotes = [&rollTrack] (int pitch)
    {
        int count = 0;
        for (const auto& r : rollTrack.midiRegions.current())
        {
            for (const auto& n : r.notes) count += n.noteNumber == pitch ? 1 : 0;
            for (const auto& take : r.previousTakes)
                for (const auto& n : take.notes) count += n.noteNumber == pitch ? 1 : 0;
        }
        return count;
    };
    steps->push_back ({ 200, [&ctx, &host, &engine, &session, &rollTrack, keyboardInput]
    {
        if (! host.pianoRollOpen()) host.openPianoRoll (kRollTrack, 0);
        ctx.expect (host.pianoRollOpen(), "the piano roll did not reopen");
        auto& transport = engine.getTransport();
        transport.setLoopEnabled (false);
        transport.setPunchEnabled (false);
        transport.setPlayhead (0);
        session.countInEnabled.store (false);
        rollTrack.midiChannel.store (0);
        rollTrack.midiInputIndex.store (keyboardInput);
        session.setTrackArmed (kRollTrack, true);
        session.recomputeRtCounters();
    } });
    steps->push_back ({ 300, [&ctx, &host] { ctx.expect (host.clickRecord(), "Record was unavailable"); } });
    steps->push_back ({ 0, [postKeyboard] { postKeyboard (true); },
        [&engine] { return engine.getTransport().isRecording(); }, "the armed track never started recording", {}, 4000 });
    steps->push_back ({ 100, [&ctx, &host]
    { ctx.expect (host.pianoKeyPointer (kAuditionedKey, true), "the keyboard's A#4 key was not under the pointer"); } });
    steps->push_back ({ 150, [&host, postKeyboard]
    {
        host.pianoKeyPointer (kAuditionedKey, false);
        postKeyboard (false);
    }, delivered (21), "the key pressed while recording never reached the track", {}, 4000 });
    steps->push_back ({ 100, [&engine] { engine.pressStop(); }, delivered (22),
        "the key released while recording never ended", {}, 4000 });
    steps->push_back ({ 0, [&ctx, takeNotes]
    {
        ctx.expect (takeNotes (kAuditionedKey) == 0, "the roll's keyboard was recorded into the take");
    }, [&engine, takeNotes] { return engine.getTransport().isStopped() && takeNotes (kRecordedKey) > 0; },
        "the take never held the on-screen keyboard's note", {}, 4000 });

    const auto nextSession = ctx.tempDir() / "audition-switch" / "session.json";
    steps->push_back ({ 200, [&ctx, &host, &engine, &session, base, nextSession]
    {
        session.setTrackArmed (kRollTrack, false);
        std::filesystem::create_directories (nextSession.parent_path());
        ctx.expect (engine.publishPluginStateForSave (true), "the audition probe state could not be saved");
        ctx.expect (SessionSerializer::save (session, nextSession), "the next audition session could not be saved");
        if (! host.pianoRollOpen()) host.openPianoRoll (kRollTrack, 0);
        ctx.expect (host.pianoRollOpen(), "the piano roll did not open before a failed session change");
        *base = engine.auditionMessagesDelivered();
    } });
    steps->push_back ({ 0, [] {}, [&host] { return host.pianoKeyPointer (69, true); },
        "the piano keyboard before session changes did not become ready", {}, 4000 });
    steps->push_back ({ 0, [&ctx, &host]
    {
        ctx.expect (! host.openSession (ctx.tempDir() / "missing" / "session.json"), "opening a missing session succeeded");
        host.closeTopModal();
        if (! host.pianoRollOpen()) host.openPianoRoll (kRollTrack, 0);
        ctx.expect (host.pianoRollOpen(), "a failed open made the old piano roll unusable");
        host.pianoKeyPointer (69, false);
    }, delivered (1), "the key before the failed open was not heard", {}, 4000 });
    steps->push_back ({ 200, [&host, &engine, base]
    {
        *base = engine.auditionMessagesDelivered();
        host.pianoKeyPointer (70, true);
    }, [&ctx] { return heardOn (ctx, kRollTrack).voices == 0; },
        "a failed session open left the audition held", {}, 4000 });
    steps->push_back ({ 0, [&ctx, &host, nextSession]
    { ctx.expect (host.openSession (nextSession), "the next audition session did not load"); },
        delivered (1), "the key before the successful open was not heard", {}, 4000 });
    steps->push_back ({ 250, [&ctx, &host, beforeExtra]
    {
        ctx.expect (heardOn (ctx, kRollTrack).voices == 0, "a held audition leaked into the new session");
        if (! host.pianoRollOpen()) host.openPianoRoll (kRollTrack, 0);
        ctx.expect (host.pianoRollOpen(), "the new session's piano roll did not open");
        *beforeExtra = heardOn (ctx, kRollTrack);
    } });
    steps->push_back ({ 0, [] {}, [&host] { return host.pianoKeyPointer (70, false); },
        "the new session's keyboard did not become ready", {}, 4000 });
    steps->push_back ({ 0, [&host] { host.pianoKeyPointer (60, true); } });
    steps->push_back ({ 0, [&host] { host.pianoKeyPointer (60, false); },
        [&ctx] { return heardOn (ctx, kRollTrack).voices == 1; }, "the new session could not audition", {}, 4000 });
    steps->push_back ({ 200, [&ctx, beforeExtra]
    {
        const auto after = heardOn (ctx, kRollTrack);
        ctx.expect (after.ons == beforeExtra->ons + 1 && after.offs == beforeExtra->offs + 1 && after.voices == 0,
                    "the old session's pointer release affected the new audition");
    }, [&ctx] { return heardOn (ctx, kRollTrack).voices == 0; }, "the new session's audition never ended", {}, 4000 });

    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pianoKeyboardAudition { Scenario {
    "gui.piano_keyboard_audition", { "gui", "piano", "midi" }, Needs::Engine | Needs::Gui,
    { "panic_probe.clap" }, {}, 90000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoKeyboardAudition (host, ctx); }
} };

#if DUSKSTUDIO_HAS_ALSA
std::optional<ScenarioResult> runPianoKeyboardOutput (GuiHost& host, ScenarioContext& ctx)
{
    snd_seq_t* raw = nullptr;
    if (snd_seq_open (&raw, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0)
        return ScenarioResult::skip ("ALSA sequencer is unavailable");
    auto seq = std::shared_ptr<snd_seq_t> (raw, [] (snd_seq_t* value) { snd_seq_close (value); });
    const auto name = "dusk-audition-" + std::to_string (snd_seq_client_id (raw));
    snd_seq_set_client_name (raw, name.c_str());
    const int port = snd_seq_create_simple_port (raw, "audition destination",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (port < 0) return ScenarioResult::fail ("could not create the audition destination");
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (kRollTrack);
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save initial session");
    ctx.cleanup ([&host, &engine, &session, &track, seq, port, originalDir, restore]
    {
        host.pianoKeyPointer (60, false);
        host.closePiano();
        engine.releaseAuditions();
        track.midiOutputIndex.store (-1);
        track.midiOutputIdentifier.clear();
        snd_seq_delete_simple_port (seq.get(), port);
        engine.refreshMidiInputs();
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
    });
    engine.stop();
    host.closePiano();
    MidiRegion region;
    region.lengthInTicks = 3840;
    region.lengthInSamples = session.ticksToSamples (3840, engine.getCurrentSampleRate());
    region.notes = { { kRegionChannel, 48, 90, 1920, 480 } };
    track.mode.store ((int) Track::Mode::Midi);
    track.inputMonitor.store (false);
    track.recordArmed.store (false);
    track.midiInputIndex.store (-1);
    track.midiInputIdentifier.clear();
    track.midiOutputIndex.store (-1);
    track.midiOutputIdentifier.clear();
    track.midiChannel.store (5);
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::initializer_list<MidiRegion> { region }));
    session.recomputeRtCounters();
    engine.refreshMidiInputs();
    const auto destination = [&engine, name]
    {
        const auto& outputs = engine.getMidiOutputDevices();
        for (int i = 0; i < (int) outputs.size(); ++i)
            if (outputs[(std::size_t) i].name.find (name) != std::string::npos) return i;
        return -1;
    };
    auto received = std::make_shared<std::vector<std::array<int, 4>>>();
    auto drain = std::make_shared<std::pair<std::size_t, int>> (0, 0);
    const auto collect = [seq, received, drain]
    {
        snd_seq_event_t* event = nullptr;
        while (snd_seq_event_input (seq.get(), &event) >= 0)
            if (event && (event->type == SND_SEQ_EVENT_NOTEON || event->type == SND_SEQ_EVENT_NOTEOFF))
                received->push_back ({ event->type, event->data.note.channel, event->data.note.note, event->data.note.velocity });
        const auto count = received->size();
        if (count == drain->first) ++drain->second;
        else { drain->first = count; drain->second = 0; }
        return count >= 2 && drain->second >= 3;
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&ctx, &host, &engine, &track, destination]
    {
        const int output = destination();
        ctx.expect (engine.ensureMidiOutputOpen (output), "the audition output did not open");
        track.midiOutputIdentifier = engine.getMidiOutputDevices()[(std::size_t) output].identifier;
        track.midiOutputIndex.store (output);
        ctx.expect (host.openPiano (kRollTrack, 0), "the output audition piano roll did not open");
    }, [&engine, destination] { return engine.midiHotplugSettledForScenario() && destination() >= 0; },
        "the audition output was not enumerated", {}, 4000 });
    steps->push_back ({ 0, [] {},
        [&host] { return host.pianoKeyPointer (60, true); }, "the output audition keyboard was not ready", {}, 4000 });
    steps->push_back ({ 0, [&host] { host.pianoKeyPointer (60, false); } });
    steps->push_back ({ 100, [&ctx, received]
    {
        ctx.expect (*received == std::vector<std::array<int, 4>> {
            { SND_SEQ_EVENT_NOTEON, kRegionChannel - 1, 60, 100 },
            { SND_SEQ_EVENT_NOTEOFF, kRegionChannel - 1, 60, 0 } },
            "the piano keyboard did not deliver exactly one onset and release on the region's MIDI channel");
    }, collect, "the piano keyboard's note and release did not reach the MIDI output", {}, 4000 });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}
#endif

const ScenarioRegistrar pianoKeyboardOutput { Scenario {
    "gui.piano_keyboard_midi_output", { "gui", "piano", "midi", "output" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_ALSA
        return runPianoKeyboardOutput (host, ctx);
       #else
        (void) host; (void) ctx;
        return ScenarioResult::skip ("virtual MIDI destination fixture requires ALSA sequencer");
       #endif
    }
} };
#endif
} // namespace
} // namespace duskstudio::scenario::gui
