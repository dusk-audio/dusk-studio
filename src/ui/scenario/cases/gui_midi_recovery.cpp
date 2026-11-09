#include "../GuiScenarioSupport.h"
#include "../../SaveTargetChecks.h"
#include "../../../engine/midi/MidiFileReader.h"
#include "../../../session/SessionSerializer.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace duskstudio::scenario::gui
{
namespace
{
namespace fs = std::filesystem;

std::vector<std::string> namesIn (const fs::path& folder)
{
    std::vector<std::string> names;
    std::error_code error;
    for (fs::directory_iterator it (folder, error), end; ! error && it != end; it.increment (error))
        names.push_back (it->path().filename().u8string());
    std::sort (names.begin(), names.end());
    return names;
}

// The note-ons of a file as { key, velocity }, in file order.
std::vector<std::array<int, 2>> noteOnsIn (const fs::path& file)
{
    std::vector<std::array<int, 2>> notes;
    midi::MidiFileReader reader;
    if (! reader.readFile (file)) return notes;
    for (const auto& part : reader.tracks())
        for (const auto& event : part)
            if (event.isNoteOn()) notes.push_back ({ event.noteNumber(), event.velocity() });
    return notes;
}

bool absent (const fs::path& path)
{
    std::error_code error;
    return fs::symlink_status (path, error).type() == fs::file_type::not_found;
}

void setVelocity (Track& track, int velocity)
{
    track.midiRegions.mutate ([velocity] (std::vector<MidiRegion>& regions)
                              { regions.front().notes.front().velocity = velocity; });
}

// Opening, saving and autosaving keep each MIDI track in midi/tracks as a
// Standard MIDI File, a new one only when the track has changed since the
// newest one there, and again when an edit an autosave caught is undone. Save
// As takes midi/ along to the new folder. A session
// never saved keeps its files in its Untitled folder, which then does not take
// another new session, and its first save moves them out, so the folder is
// free again.
std::optional<ScenarioResult> runSavesKeepMidiTrackFiles (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &engine, &session, originalDir, restore]
    {
        engine.stop();
        drainModals (host);
        engine.flushMidiRecoveryFiles();
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });

    const auto folder = ctx.tempDir() / "MIDI session";
    {
        auto saved = std::make_unique<Session>();
        applySessionDirectory (*saved, folder);
        auto& track = saved->track (0);
        track.name = "Keys";
        track.mode.store ((int) Track::Mode::Midi);
        MidiRegion region;
        region.lengthInTicks = 960;
        region.lengthInSamples = 48000;
        region.notes.push_back ({ 1, 60, 100, 0, 480 });
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (1, region));
        if (! SessionSerializer::save (*saved, folder / "session.json"))
            return ScenarioResult::fail ("could not write the MIDI session");
    }
    reopenSavedSession (host, folder / "session.json");
    auto& track = session.track (0);
    if (currentSessionDirectory (session) != folder || track.midiRegions.current().size() != 1)
        return ScenarioResult::fail ("could not open the MIDI session");

    // Opening a session whose MIDI has no files yet writes them, and opening
    // it again unchanged writes nothing.
    const auto tracks = folder / "midi" / "tracks";
    engine.flushMidiRecoveryFiles();
    const auto opened = namesIn (tracks);
    if (! ctx.expect (opened.size() == 1 && opened.front().size() > 17
                          && opened.front().substr (16) == "_track01_Keys.mid",
                      "opening the session left " + std::to_string (opened.size())
                          + " files in midi/tracks, not the one playing file"))
        return ctx.verdict();
    ctx.expect (noteOnsIn (tracks / opened.front()) == std::vector<std::array<int, 2>> { { 60, 100 } },
                "the playing file the open wrote does not hold the region's note");
    ctx.expect (absent (folder / "midi" / "takes"), "opening the session wrote take files");
    reopenSavedSession (host, folder / "session.json");
    engine.flushMidiRecoveryFiles();
    ctx.expect (namesIn (tracks) == opened, "opening the unchanged session again wrote another track file");

    const auto saved = std::make_shared<std::vector<std::string>>();
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&session] { setVelocity (session.track (0), 64); } });
    pushFileMenuSteps (host, ctx, *steps, "Save");
    steps->push_back ({ 500, [&host, &ctx, &engine, tracks, saved]
    {
        ctx.expect (host.statusMessage() == "Saved: MIDI session", "Save reported '" + host.statusMessage() + "'");
        engine.flushMidiRecoveryFiles();
        *saved = namesIn (tracks);
        if (ctx.expect (saved->size() == 2, "Save after a MIDI edit left " + std::to_string (saved->size())
                                                 + " files in midi/tracks, not 2"))
            ctx.expect (noteOnsIn (tracks / saved->back()) == std::vector<std::array<int, 2>> { { 60, 64 } },
                        "the track file Save wrote does not hold the edit");
    } });
    pushFileMenuSteps (host, ctx, *steps, "Save");
    steps->push_back ({ 500, [&ctx, &engine, &track, tracks, saved]
    {
        engine.flushMidiRecoveryFiles();
        ctx.expect (namesIn (tracks) == *saved, "a second Save with nothing changed wrote another track file");
        setVelocity (track, 70);
    } });
    const auto autosaved = std::make_shared<std::vector<std::string>>();
    steps->push_back ({ 100, [&host, &ctx, &engine, tracks, autosaved]
    {
        host.autosaveTick();
        engine.flushMidiRecoveryFiles();
        *autosaved = namesIn (tracks);
        if (ctx.expect (autosaved->size() == 3, "an autosave after a MIDI edit left "
                                                    + std::to_string (autosaved->size()) + " files in midi/tracks, not 3"))
            ctx.expect (noteOnsIn (tracks / autosaved->back()) == std::vector<std::array<int, 2>> { { 60, 70 } },
                        "the track file the autosave wrote does not hold the edit");
    } });
    // Back to the saved state: the next autosave removes the one it wrote, and
    // the track files go again so the newest holds the saved MIDI.
    steps->push_back ({ 100, [&host, &ctx, &engine, &track, folder, tracks, autosaved]
    {
        setVelocity (track, 64);
        host.autosaveTick();
        engine.flushMidiRecoveryFiles();
        ctx.expect (absent (folder / "session.json.autosave"), "back at the saved state, the autosave is still there");
        *autosaved = namesIn (tracks);
        if (ctx.expect (autosaved->size() == 4, "going back to the saved state left "
                                                    + std::to_string (autosaved->size()) + " files in midi/tracks, not 4"))
            ctx.expect (noteOnsIn (tracks / autosaved->back()) == std::vector<std::array<int, 2>> { { 60, 64 } },
                        "the newest track file does not hold the saved state");
        host.autosaveTick();
        engine.flushMidiRecoveryFiles();
        ctx.expect (namesIn (tracks) == *autosaved, "a second tick at the saved state wrote another track file");
    } });

    const auto copy = ctx.tempDir() / "MIDI copy";
    pushSaveAsSteps (host, ctx, *steps, copy);
    steps->push_back ({ 500, [&ctx, &engine, &session, tracks, copy, autosaved]
    {
        ctx.expect (currentSessionDirectory (session) == copy,
                    "Save As left the session at '" + currentSessionDirectory (session).string() + "'");
        engine.flushMidiRecoveryFiles();
        const auto copied = copy / "midi" / "tracks";
        ctx.expect (namesIn (copied) == *autosaved,
                    "Save As did not take midi/tracks along, or wrote a file the copy already held");
        for (const auto& name : *autosaved)
            ctx.expect (fileBytes (copied / name) == fileBytes (tracks / name), "Save As changed " + name);
        ctx.expect (namesIn (tracks) == *autosaved, "Save As of a session on disk took its MIDI files from its folder");
    } });

    // A session never saved, in the first Untitled folder of a sessions folder.
    const auto parent = ctx.tempDir() / "Sessions";
    const auto untitled = parent / "Untitled";
    steps->push_back ({ 200, [&host, &ctx, &engine, &session, &track, parent, untitled]
    {
        host.startUnsavedSessionIn (parent);
        if (! ctx.expect (currentSessionDirectory (session) == untitled && ! host.sessionOnDisk(),
                          "the new session started in '" + currentSessionDirectory (session).string() + "'"))
            return;
        setVelocity (track, 90);
        engine.queueMidiTrackFiles();
        engine.flushMidiRecoveryFiles();
        ctx.expect (namesIn (untitled / "midi" / "tracks").size() == 1,
                    "the never-saved session's track file is not in its Untitled folder");
        ctx.expect (savecheck::unsavedSessionFolder (parent) == parent / "Untitled 2",
                    "a folder holding a session's MIDI files was offered to the next new session");
    } });
    const auto firstSave = parent / "First save";
    pushSaveAsSteps (host, ctx, *steps, firstSave);
    steps->push_back ({ 500, [&host, &ctx, &engine, &session, parent, untitled, firstSave]
    {
        ctx.expect (currentSessionDirectory (session) == firstSave && host.sessionOnDisk(),
                    "the first save left the session at '" + currentSessionDirectory (session).string() + "'");
        engine.flushMidiRecoveryFiles();
        const auto moved = namesIn (firstSave / "midi" / "tracks");
        ctx.expect (moved.size() == 1, "the first save left " + std::to_string (moved.size())
                                           + " files in the new folder's midi/tracks, not 1");
        if (moved.size() == 1)
            ctx.expect (noteOnsIn (firstSave / "midi" / "tracks" / moved.front())
                            == std::vector<std::array<int, 2>> { { 60, 90 } },
                        "the track file the first save took along does not hold the session's note");
        ctx.expect (absent (untitled / "midi"), "the first save left a midi folder in the Untitled folder");
        ctx.expect (savecheck::unsavedSessionFolder (parent) == untitled,
                    "the Untitled folder was not free again after the first save");
    } });
    const auto foreignParent = ctx.tempDir() / "Foreign session";
    const auto foreign = foreignParent / "Untitled";
    const auto privateDir = dusk::fs::appConfigDir() / "unsaved-session";
    const auto preserved = ctx.tempDir() / "private-recovery-before";
    const auto rescued = foreignParent / "Rescued";
    auto diverted = std::make_shared<std::vector<std::string>>();
    steps->push_back ({ 100, [&host, &ctx, &engine, &track, foreignParent, foreign, privateDir, preserved, diverted]
    {
        engine.flushMidiRecoveryFiles();
        std::error_code error;
        if (fs::exists (privateDir, error)) fs::rename (privateDir, preserved);
        ctx.cleanup ([&engine, privateDir, preserved]
        {
            engine.flushMidiRecoveryFiles();
            std::error_code ignored;
            fs::remove_all (privateDir, ignored);
            if (fs::exists (preserved, ignored)) fs::rename (preserved, privateDir, ignored);
        });
        host.startUnsavedSessionIn (foreignParent);
        std::ofstream (foreign / "session.json") << "foreign session";
        setVelocity (track, 91);
        engine.queueMidiTrackFiles();
        engine.flushMidiRecoveryFiles();
        *diverted = namesIn (privateDir / "midi" / "tracks");
        ctx.expect (diverted->size() == 1, "MIDI recovery did not use the private sidecar folder");
        ctx.expect (absent (foreign / "midi"), "MIDI recovery wrote into a folder claimed by a foreign session");
        host.autosaveTick();
        engine.flushMidiRecoveryFiles();
        ctx.expect (namesIn (privateDir / "midi" / "tracks") == *diverted,
                    "autosave did not deduplicate the private MIDI snapshot");
    } });
    pushSaveAsSteps (host, ctx, *steps, rescued);
    steps->push_back ({ 500, [&host, &ctx, &engine, &session, foreign, privateDir, rescued, diverted]
    {
        engine.flushMidiRecoveryFiles();
        ctx.expect (currentSessionDirectory (session) == rescued && host.sessionOnDisk(),
                    "the foreign-folder session did not save to its chosen destination");
        ctx.expect (fileBytes (foreign / "session.json") == "foreign session" && absent (foreign / "midi"),
                    "Save As changed the foreign session's folder");
        ctx.expect (namesIn (rescued / "midi" / "tracks") == *diverted && diverted->size() == 1,
                    "Save As did not carry the private MIDI recovery files");
        ctx.expect (absent (privateDir / "midi"), "the first save left its private MIDI source behind");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar savesKeepMidiTrackFiles { Scenario {
    "gui.saves_keep_midi_track_files", { "gui", "session", "autosave", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 40000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSavesKeepMidiTrackFiles (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
