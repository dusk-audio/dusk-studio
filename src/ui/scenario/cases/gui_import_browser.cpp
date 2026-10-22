#include "../GuiScenarioSupport.h"
#include "../../../engine/audiofile/FileWriter.h"
#include "../../../engine/audiofile/FileReader.h"
#include "../../../session/ParamEditAction.h"
#include "../../../session/RegionEditActions.h"
#include "../../../session/SessionSerializer.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace duskstudio::scenario::gui
{
namespace
{
std::optional<ScenarioResult> runMultiImportTargets (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    if (engine.getStage() != AudioEngine::Stage::Recording && engine.getStage() != AudioEngine::Stage::Mixing)
        return ScenarioResult::skip ("requires a stage with the timeline");
    for (const int index : { 22, 23 })
    {
        const auto& track = session.track (index);
        if (! track.regions.empty() || track.frozen.load() || track.mode.load() != (int) Track::Mode::Mono)
            return ScenarioResult::skip ("requires empty mono destination tracks");
    }
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save initial session");
    const bool shown = host.setTimelineShown (true);
    ctx.cleanup ([&host, &session, originalDir, restore, shown]
    {
        drainModals (host);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        host.setTimelineShown (shown);
    });
    const auto importDir = ctx.tempDir() / "session";
    std::filesystem::create_directories (importDir / "audio");
    applySessionDirectory (session, importDir);
    std::vector<std::filesystem::path> files;
    for (int index = 0; index < 2; ++index)
    {
        files.push_back (ctx.tempDir() / (index == 0 ? "First stem.wav" : "Second stem.wav"));
        dusk::audio::WriteSpec spec;
        spec.sampleRate = 48000;
        spec.numChannels = 1;
        auto writer = dusk::audio::FileWriter::create (files.back(), spec);
        std::vector<float> silence ((size_t) (2400 * (index + 1)));
        const float* data[] = { silence.data() };
        if (! writer || ! writer->write (data, 1, (std::int64_t) silence.size()) || ! writer->flush())
            return ScenarioResult::fail ("could not write multi-import fixtures");
    }
    const auto rows = [&ctx, &host] (int first, int second)
    {
        ctx.expect (host.multiImportRows() == std::vector<std::string> {
            "First stem.wav\t" + std::to_string (first), "Second stem.wav\t" + std::to_string (second) },
            "multi-import file labels or selected tracks are wrong");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 250, [&host, &ctx, files]
    { ctx.expect (host.dropFilesOnTrack (0, files), "multi-file drop was rejected"); } });
    steps->push_back ({ 250, [&host, &ctx, rows]
    {
        rows (-1, -1);
        ctx.expect (! host.clickModalButton ("Import"), "unassigned files can be imported");
        ctx.expect (host.clickModalButton ("Auto-assign"), "Auto-assign is unavailable");
    } });
    steps->push_back ({ 200, [&host, &ctx, rows]
    {
        rows (0, 1);
        ctx.expect (host.clickModalButton ("Clear"), "Auto-assign did not become Clear");
    } });
    steps->push_back ({ 200, [&host, &ctx, rows]
    {
        rows (-1, -1);
        ctx.expect (! host.clickModalButton ("Import"), "Clear left Import enabled");
    } });
    for (int row = 0; row < 2; ++row)
    {
        steps->push_back ({ 200, [&host, &ctx, row]
        { ctx.expect (host.clickMultiImportTarget (row), "file target dropdown is unavailable"); } });
        steps->push_back ({ 150, [&host, &ctx]
        { ctx.expect (host.pressPeerKey ("end", 0), "dropdown End key was not handled"); } });
        steps->push_back ({ 150, [&host, &ctx]
        { ctx.expect (host.pressPeerKey ("return", '\r'), "dropdown Return key was not handled"); } });
    }
    steps->push_back ({ 200, [&host, &ctx, rows]
    {
        rows (23, 22);
        ctx.expect (host.clickModalButton ("Import"), "assigned files cannot be imported");
    } });
    steps->push_back ({ 700, [&ctx, &host, &session, &engine]
    {
        ctx.expect (host.modalStackEmpty(), "multi-import left a modal open");
        for (int index = 0; index < 2; ++index)
        {
            const auto& regions = session.track (23 - index).regions;
            if (! ctx.expect (regions.size() == 1, "chosen destination did not receive one region")) continue;
            const auto expected = (std::int64_t) std::llround (2400.0 * (index + 1) * engine.getCurrentSampleRate() / 48000.0);
            ctx.expect (regions[0].file.existsAsFile() && regions[0].numChannels == 1
                        && regions[0].lengthInSamples == expected, "wrong source reached the assigned track");
        }
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar multiImportTargets { Scenario {
    "gui.multi_import_targets", { "gui", "import" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMultiImportTargets (host, ctx); }
} };

std::optional<ScenarioResult> runImportModeConfirmation (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& track = session.track (0);
    if (! engine.getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    if (! host.modalStackEmpty() || ! track.regions.empty() || ! track.midiRegions.current().empty() || track.frozen.load())
        return ScenarioResult::skip ("requires an empty unfrozen track and no modal");
    if (engine.getStage() != AudioEngine::Stage::Recording && engine.getStage() != AudioEngine::Stage::Mixing)
        return ScenarioResult::skip ("requires a stage with the timeline");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save initial session");
    const bool shown = host.setTimelineShown (true);
    ctx.cleanup ([&host, &session, originalDir, restore, shown]
    {
        drainModals (host);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        host.setTimelineShown (shown);
    });
    const auto importDir = ctx.tempDir() / "session";
    std::filesystem::create_directories (importDir / "audio");
    applySessionDirectory (session, importDir);
    track.mode.store ((int) Track::Mode::Mono);
    for (int channels = 1; channels <= 2; ++channels)
    {
        dusk::audio::WriteSpec spec;
        spec.sampleRate = 48000;
        spec.numChannels = channels;
        auto writer = dusk::audio::FileWriter::create (ctx.tempDir() / (channels == 1 ? "Mono.wav" : "Stereo.wav"), spec);
        std::array<float, 4800> silence {};
        const float* data[] = { silence.data(), silence.data() };
        if (! writer || ! writer->write (data, channels, 4800) || ! writer->flush())
            return ScenarioResult::fail ("could not write audio import fixture");
    }
    const unsigned char midi[] = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 1, 0xe0,
        'M', 'T', 'r', 'k', 0, 0, 0, 13,
        0, 0x90, 60, 100, 0x83, 0x60, 0x80, 60, 0, 0, 0xff, 0x2f, 0
    };
    std::ofstream output (ctx.tempDir() / "Notes.mid", std::ios::binary);
    output.write (reinterpret_cast<const char*> (midi), sizeof (midi));
    output.close();
    if (! output) return ScenarioResult::fail ("could not write MIDI import fixture");
    struct Leg { const char* file; const char* from; const char* to; Track::Mode mode; bool midi; };
    const std::array<Leg, 3> legs {{
        { "Stereo.wav", "Mono", "Stereo", Track::Mode::Stereo, false },
        { "Notes.mid", "Stereo", "MIDI", Track::Mode::Midi, true },
        { "Mono.wav", "MIDI", "Mono", Track::Mode::Mono, false }
    }};
    auto steps = std::make_shared<std::vector<Step>>();
    for (const auto leg : legs)
    {
        auto before = std::make_shared<std::array<int, 3>>();
        steps->push_back ({ 250, [&host, &ctx, &track, leg, before]
        {
            *before = { track.mode.load(), (int) track.regions.size(), (int) track.midiRegions.current().size() };
            ctx.expect (host.dropFilesOnTrack (0, { ctx.tempDir() / leg.file }), "file drop was rejected");
        } });
        const auto unchanged = [&ctx, &track, before]
        {
            ctx.expect (track.mode.load() == (*before)[0] && (int) track.regions.size() == (*before)[1]
                        && (int) track.midiRegions.current().size() == (*before)[2], "unconfirmed import changed the track");
        };
        for (const bool accept : { false, true })
        {
            steps->push_back ({ 250, [&host, &ctx]
            { ctx.expect (host.clickModalButton ("Import"), "target picker Import button is unavailable"); } });
            steps->push_back ({ 250, [&host, &ctx, leg, unchanged, accept]
            {
                const auto text = host.confirmationText();
                const std::string title = std::string ("Switch track to ") + leg.to + "?";
                const std::string message = std::string ("Track 1 is currently in ") + leg.from
                    + " mode. Importing this " + (leg.midi ? "MIDI" : "audio")
                    + " file will switch the track to " + leg.to + " mode. Proceed?";
                ctx.expect (text == std::vector<std::string> { title, message }, "mode-switch confirmation text is wrong");
                unchanged();
                ctx.expect (host.clickModalButton (accept ? "Switch" : "Cancel"), "mode-switch action is unavailable");
            } });
            if (! accept) steps->push_back ({ 250, unchanged });
        }
        steps->push_back ({ 500, [&ctx, &host, &track, leg, before]
        {
            ctx.expect (host.modalStackEmpty(), "confirmed import left a modal open");
            ctx.expect (track.mode.load() == (int) leg.mode, "Switch did not change track mode");
            ctx.expect ((int) track.regions.size() == (*before)[1] + (leg.midi ? 0 : 1)
                        && (int) track.midiRegions.current().size() == (*before)[2] + (leg.midi ? 1 : 0),
                        "Switch did not import exactly one region");
            if (leg.midi && ! track.midiRegions.current().empty())
                ctx.expect (track.midiRegions.current().back().notes.size() == 1, "imported MIDI note is missing");
            if (! leg.midi && ! track.regions.empty())
                ctx.expect (track.regions.back().file.existsAsFile()
                            && track.regions.back().numChannels == (leg.mode == Track::Mode::Stereo ? 2 : 1),
                            "imported audio file or channel layout is wrong");
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar importModeConfirmation { Scenario {
    "gui.import_mode_confirmation", { "gui", "import" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runImportModeConfirmation (host, ctx); }
} };

// Checks a single-file picker case can run, puts the session back after it,
// and leaves every track an empty Mono track with nothing to undo.
std::optional<ScenarioResult> beginImportPickerCase (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    if (engine.getStage() != AudioEngine::Stage::Recording && engine.getStage() != AudioEngine::Stage::Mixing)
        return ScenarioResult::skip ("requires a stage with the timeline");
    for (int t = 0; t < Session::kNumTracks; ++t)
        if (session.track (t).frozen.load())
            return ScenarioResult::skip ("requires no frozen track");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save the initial session");
    const bool shown = host.setTimelineShown (true);
    ctx.cleanup ([&host, &session, &engine, originalDir, restore, shown]
    {
        drainModals (host);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        host.setTimelineShown (shown);
        engine.getUndoManager().clearUndoHistory();
    });
    // An import copies its file into the session's audio folder.
    const auto importDir = ctx.tempDir() / "session";
    std::error_code madeDir;
    std::filesystem::create_directories (importDir / "audio", madeDir);
    applySessionDirectory (session, importDir);
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& track = session.track (t);
        track.mode.store ((int) Track::Mode::Mono);
        track.regions.clear();
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>>());
    }
    engine.getUndoManager().clearUndoHistory();
    return std::nullopt;
}

void giveMidiRegion (ScenarioContext& ctx, int track)
{
    MidiRegion region;
    region.lengthInTicks = 3840;
    region.lengthInSamples = ctx.session().ticksToSamples (region.lengthInTicks, ctx.engine().getCurrentSampleRate());
    ctx.session().track (track).mode.store ((int) Track::Mode::Midi);
    ctx.session().track (track).midiRegions.publish (
        std::make_unique<std::vector<MidiRegion>> (std::initializer_list<MidiRegion> { region }));
}

bool writeStereoSilence (const std::filesystem::path& path)
{
    dusk::audio::WriteSpec spec;
    spec.sampleRate = 48000;
    spec.numChannels = 2;
    auto writer = dusk::audio::FileWriter::create (path, spec);
    std::array<float, 4800> silence {};
    const float* data[] = { silence.data(), silence.data() };
    return writer != nullptr && writer->write (data, 2, 4800) && writer->flush();
}

// The single-file picker's row for a track, split at its tabs: the number, what
// it holds, "RECOMMENDED", the switch hint, "selected" and "in view", "-" where
// one does not apply. Empty when the picker is not up or has no such row.
std::vector<std::string> importTargetRow (const GuiHost& host, int trackNumber)
{
    const auto prefix = std::to_string (trackNumber) + "\t";
    for (const auto& row : host.importTargetRows())
    {
        if (row.rfind (prefix, 0) != 0) continue;
        std::vector<std::string> parts;
        std::size_t start = 0;
        for (auto tab = row.find ('\t'); tab != std::string::npos; tab = row.find ('\t', start))
        {
            parts.push_back (row.substr (start, tab - start));
            start = tab + 1;
        }
        parts.push_back (row.substr (start));
        return parts;
    }
    return {};
}

bool switchesToStereo (const std::string& hint) { return hint.find ("will switch to Stereo") != std::string::npos; }

std::string joinedRow (const std::vector<std::string>& fields)
{
    std::string joined;
    for (const auto& field : fields) joined += (joined.empty() ? "" : " | ") + field;
    return joined;
}

// The single-file picker says what each track holds, whichever kind of file is
// coming, marks every track the import would switch, recommended or not, and
// recommends no track whose content a switch would leave behind. A stereo file
// dropped on a MIDI track with a region keeps that track selected, still asks
// before switching it, and recommends the empty stereo track instead. With
// every track holding regions it would switch, none is recommended.
std::optional<ScenarioResult> runImportTargetRows (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginImportPickerCase (host, ctx)) return early;
    auto& session = ctx.session();
    giveMidiRegion (ctx, 0);
    session.track (2).mode.store ((int) Track::Mode::Stereo);

    const auto file = ctx.tempDir() / "Stereo.wav";
    if (! writeStereoSilence (file)) return ScenarioResult::fail ("could not write the audio import fixture");

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 250, [&host, &ctx, file]
    { ctx.expect (host.dropFilesOnTrack (0, { file }), "the drop on the MIDI track was rejected"); } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        const auto midi = importTargetRow (host, 1);
        if (! ctx.expect (midi.size() == 6, "the picker has no row for the MIDI track")) return;
        ctx.expect (midi[1] == "1 region", "the MIDI track holding a region reads '" + midi[1] + "'");
        ctx.expect (midi[2] == "-", "the MIDI track an audio import would switch, region and all, is recommended");
        ctx.expect (switchesToStereo (midi[3]), "the MIDI track an audio import would switch shows no switch hint");
        ctx.expect (midi[4] == "selected", "the track the file was dropped on is not the one selected");
        ctx.expect (midi[5] == "in view", "the selected MIDI track is scrolled out of view");
        const auto stereo = importTargetRow (host, 3);
        ctx.expect (stereo.size() == 6 && stereo[1] == "empty" && stereo[2] == "RECOMMENDED" && stereo[3] == "-",
                    "the empty stereo track is not the recommended one: '" + joinedRow (stereo) + "'");
        const auto rows = host.importTargetRows();
        ctx.expect (! rows.empty() && rows.front().rfind ("3\t", 0) == 0 && rows.back().rfind ("1\t", 0) == 0,
                    "the empty stereo track is not first and the MIDI track holding a region not last");
        ctx.expect (host.clickModalButton ("Import"), "the picker's Import button is unavailable");
    } });
    steps->push_back ({ 250, [&host, &ctx]
    {
        ctx.expect (host.confirmationText() == std::vector<std::string> {
                        "Switch track to Stereo?",
                        "Track 1 is currently in MIDI mode. Importing this audio file will switch the track to Stereo mode. Proceed?" },
                    "importing onto the MIDI track did not ask before switching it");
        ctx.expect (host.clickModalButton ("Cancel"), "the switch prompt has no Cancel");
    } });
    steps->push_back ({ 250, [&host, &ctx, &session]
    {
        const auto& track = session.track (0);
        ctx.expect (track.mode.load() == (int) Track::Mode::Midi && track.regions.empty()
                        && track.midiRegions.current().size() == 1, "Cancel on the switch prompt changed the MIDI track");
        drainModals (host);
    } });
    steps->push_back ({ 250, [&host, &ctx, file]
    { ctx.expect (host.dropFilesOnTrack (1, { file }), "the drop on the empty mono track was rejected"); },
                       [&host] { return host.modalStackEmpty(); }, "the first picker did not close" });
    steps->push_back ({ 300, [&host, &ctx]
    {
        const auto mono = importTargetRow (host, 2);
        if (! ctx.expect (mono.size() == 6, "the picker has no row for the mono track")) return;
        ctx.expect (mono[1] == "empty" && mono[2] == "RECOMMENDED" && mono[4] == "selected",
                    "the empty mono track the file was dropped on is not recommended and selected: '"
                        + joinedRow (mono) + "'");
        ctx.expect (switchesToStereo (mono[3]), "the recommended mono track a stereo import would switch shows no switch hint");
        drainModals (host);
    } });
    steps->push_back ({ 250, [&host, &ctx, file]
    {
        for (int t = 0; t < Session::kNumTracks; ++t)
            giveMidiRegion (ctx, t);
        ctx.expect (host.dropFilesOnTrack (1, { file }), "the drop with every track holding MIDI was rejected");
    }, [&host] { return host.modalStackEmpty(); }, "the second picker did not close" });
    steps->push_back ({ 300, [&host, &ctx]
    {
        const auto rows = host.importTargetRows();
        ctx.expect (rows.size() == (std::size_t) Session::kNumTracks, "the picker does not list every track");
        for (int t = 1; t <= Session::kNumTracks; ++t)
        {
            const auto row = importTargetRow (host, t);
            if (! ctx.expect (row.size() == 6 && row[2] == "-" && switchesToStereo (row[3]),
                              "with every track holding regions a switch would leave, track " + std::to_string (t)
                                  + " reads '" + joinedRow (row) + "'"))
                return;
        }
        const auto dropped = importTargetRow (host, 2);
        ctx.expect (dropped[4] == "selected" && dropped[5] == "in view",
                    "the track the file was dropped on is not selected and in view: '" + joinedRow (dropped) + "'");
        drainModals (host);
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar importTargetRows { Scenario {
    "gui.import_target_rows", { "gui", "import" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runImportTargetRows (host, ctx); }
} };

// An undo under the single-file picker can put the track it would import onto
// back in another mode. Its rows follow the undo, and Import still asks before
// switching that track rather than landing audio on a MIDI track.
std::optional<ScenarioResult> runImportTargetFollowsUndo (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginImportPickerCase (host, ctx)) return early;
    auto& session = ctx.session();
    session.track (0).mode.store ((int) Track::Mode::Midi);
    const auto file = ctx.tempDir() / "Stereo.wav";
    if (! writeStereoSilence (file)) return ScenarioResult::fail ("could not write the audio import fixture");
    const auto trackIs = [&session] (Track::Mode mode, std::size_t regions)
    {
        const auto& track = session.track (0);
        return track.mode.load() == (int) mode && track.regions.size() == regions && track.midiRegions.current().empty();
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 250, [&host, &ctx, file]
    { ctx.expect (host.dropFilesOnTrack (0, { file }), "the drop on the empty MIDI track was rejected"); } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        const auto row = importTargetRow (host, 1);
        ctx.expect (row.size() == 6 && switchesToStereo (row[3]), "the empty MIDI track reads '" + joinedRow (row) + "'");
        ctx.expect (host.clickModalButton ("Import"), "the picker's Import button is unavailable");
    } });
    steps->push_back ({ 250, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Switch"), "importing onto the MIDI track did not ask to switch it"); } });
    steps->push_back ({ 250, [&host, &ctx, trackIs, file]
    {
        ctx.expect (trackIs (Track::Mode::Stereo, 1), "the first import did not switch track 1 to Stereo with its region");
        ctx.expect (host.dropFilesOnTrack (0, { file }), "the drop on the stereo track was rejected");
    }, [&host] { return host.modalStackEmpty(); }, "the first import left a modal up" });
    steps->push_back ({ 300, [&host, &ctx]
    {
        const auto row = importTargetRow (host, 1);
        ctx.expect (row.size() == 6 && row[1] == "1 region" && row[3] == "-" && row[4] == "selected",
                    "the stereo track holding the first import reads '" + joinedRow (row) + "'");
        ctx.expect (host.pressPeerKey ("command + Z"), "Undo under the picker was not handled");
    } });
    steps->push_back ({ 250, [&host, &ctx, trackIs]
    {
        if (! ctx.expect (trackIs (Track::Mode::Midi, 0), "Undo under the picker did not put track 1 back to an empty MIDI track"))
            return;
        const auto row = importTargetRow (host, 1);
        ctx.expect (row.size() == 6 && row[1] == "empty" && switchesToStereo (row[3]) && row[4] == "selected",
                    "after the undo the picker's row for track 1 reads '" + joinedRow (row) + "'");
        ctx.expect (host.clickModalButton ("Import"), "the picker did not stay up through the undo");
    } });
    steps->push_back ({ 250, [&host, &ctx]
    {
        ctx.expect (host.confirmationText() == std::vector<std::string> {
                        "Switch track to Stereo?",
                        "Track 1 is currently in MIDI mode. Importing this audio file will switch the track to Stereo mode. Proceed?" },
                    "Import after the undo did not ask before switching the MIDI track");
        if (! host.modalStackEmpty() && host.confirmationText().size() == 2)
            ctx.expect (host.clickModalButton ("Cancel"), "the switch prompt has no Cancel");
    } });
    steps->push_back ({ 250, [&host, &ctx, trackIs]
    {
        ctx.expect (trackIs (Track::Mode::Midi, 0), "Import after the undo put audio on track 1 while it was in MIDI mode");
        drainModals (host);
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar importTargetFollowsUndo { Scenario {
    "gui.import_target_follows_undo", { "gui", "import", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runImportTargetFollowsUndo (host, ctx); }
} };

// The multi-import picker marks a destination whose mode the import would switch,
// and the mark follows an undo and a redo under the picker that change that
// track's mode. The single-file picker's rows follow a take landing under it.
std::optional<ScenarioResult> runImportPickersFollowHistory (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginImportPickerCase (host, ctx)) return early;
    auto& session = ctx.session();
    auto& um = ctx.engine().getUndoManager();
    const auto first = ctx.tempDir() / "First.wav";
    const auto second = ctx.tempDir() / "Second.wav";
    if (! writeStereoSilence (first) || ! writeStereoSilence (second))
        return ScenarioResult::fail ("could not write the audio import fixtures");
    auto& mode = session.track (0).mode;
    um.beginNewTransaction ("Track 1 to Stereo");
    um.perform (new ParamEditAction ([&mode] { mode.store ((int) Track::Mode::Stereo); },
                                     [&mode] { mode.store ((int) Track::Mode::Mono); }));
    const auto flips = [&host] (std::size_t row)
    {
        const auto text = host.multiImportTargetText();
        return row < text.size() && text[row].find ("mode will flip") != std::string::npos;
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 250, [&host, &ctx, first, second]
    { ctx.expect (host.dropFilesOnTrack (0, { first, second }), "the two-file drop was rejected"); } });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Auto-assign"), "the multi-file picker has no Auto-assign"); } });
    steps->push_back ({ 300, [&host, &ctx, flips]
    {
        ctx.expect (host.multiImportRows() == std::vector<std::string> { "First.wav\t0", "Second.wav\t1" },
                    "Auto-assign did not put the files on the first two tracks");
        ctx.expect (! flips (0) && flips (1), "before the undo the rows read '" + joinedRow (host.multiImportTargetText()) + "'");
        ctx.expect (host.pressPeerKey ("command + Z"), "Undo under the multi-import picker was not handled");
    } });
    steps->push_back ({ 300, [&host, &ctx, &mode, flips]
    {
        ctx.expect (mode.load() == (int) Track::Mode::Mono, "Undo under the multi-import picker did not put track 1 back to Mono");
        ctx.expect (flips (0), "after the undo track 1's row reads '" + joinedRow (host.multiImportTargetText()) + "'");
        ctx.expect (host.pressPeerKey ("command + shift + Z"), "Redo under the multi-import picker was not handled");
    } });
    steps->push_back ({ 300, [&host, &ctx, &mode, flips]
    {
        ctx.expect (mode.load() == (int) Track::Mode::Stereo, "Redo under the multi-import picker did not switch track 1 back");
        ctx.expect (! flips (0), "after the redo track 1's row reads '" + joinedRow (host.multiImportTargetText()) + "'");
        drainModals (host);
    } });
    steps->push_back ({ 250, [&host, &ctx, first]
    { ctx.expect (host.dropFilesOnTrack (0, { first }), "the single-file drop was rejected"); },
                        [&host] { return host.modalStackEmpty(); }, "the multi-import picker did not close" });
    steps->push_back ({ 300, [&host, &ctx, &session]
    {
        const auto row = importTargetRow (host, 2);
        if (! ctx.expect (row.size() == 6 && row[1] == "empty", "track 2 reads '" + joinedRow (row) + "' before the take"))
            return;
        ctx.expect (landTake (ctx, session.track (1), 0, 24000, true).has_value(), "the take did not land on track 2");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        const auto row = importTargetRow (host, 2);
        ctx.expect (row.size() == 6 && row[1] == "1 region",
                    "after a take landed under the picker track 2 reads '" + joinedRow (row) + "'");
        drainModals (host);
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar importPickersFollowHistory { Scenario {
    "gui.import_pickers_follow_history", { "gui", "import", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runImportPickersFollowHistory (host, ctx); }
} };

// A window too short for the whole single-file picker shrinks its list, and the
// track the file was dropped on stays in view even when it sorts last.
std::optional<ScenarioResult> runImportTargetShortWindow (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("the UI scale needs the native UI");
   #else
    const auto launch = host.mainWindowSize();
    if (launch.size() < 4) return ScenarioResult::fail ("the main window reported no size");
    if (auto early = beginImportPickerCase (host, ctx)) return early;
    const auto originalScale = static_cast<float> (host.uiScale());
    ctx.cleanup ([&host, originalScale, launch]
    {
        drainModals (host);
        host.restoreUiScale (originalScale);
        host.resizeMainWindow (launch[0], launch[1]);
    });
    giveMidiRegion (ctx, 0);
    const auto file = ctx.tempDir() / "Stereo.wav";
    if (! writeStereoSilence (file)) return ScenarioResult::fail ("could not write the audio import fixture");

    // The UI scale lowers the window's smallest height below the picker's.
    static constexpr float kScale = 1.25f;
    static constexpr int kShortHeight = 540;
    static constexpr int kPickerHeight = 560;
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&host] { host.restoreUiScale (kScale); } });
    steps->push_back ({ 100, [&host, &ctx]
    {
        const auto size = host.mainWindowSize();
        if (size.size() < 4 || ! host.resizeMainWindow (size[0], kShortHeight))
            ctx.complete (ScenarioResult::skip ("the window cannot be made shorter than the picker here"));
    }, [&host] { return std::abs (host.uiScale() - kScale) < 0.001; }, "the UI scale did not change" });
    steps->push_back ({ 200, [&host, &ctx, file]
    { ctx.expect (host.dropFilesOnTrack (0, { file }), "the drop on the MIDI track was rejected"); } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        const auto layout = host.modalLayout();
        if (! ctx.expect (layout.size() == 7, "the picker reported no layout")) return;
        if (layout[3] >= kPickerHeight)
        {
            ctx.complete (ScenarioResult::skip ("a " + std::to_string (layout[5]) + "-unit window still holds the whole picker"));
            return;
        }
        const auto rows = host.importTargetRows();
        ctx.expect (! rows.empty() && rows.back().rfind ("1\t", 0) == 0, "the MIDI track holding a region does not sort last");
        const auto row = importTargetRow (host, 1);
        ctx.expect (row.size() == 6 && row[4] == "selected" && row[5] == "in view",
                    "in a " + std::to_string (layout[3]) + "-unit picker the dropped-on track reads '"
                        + joinedRow (row) + "'");
        drainModals (host);
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar importTargetShortWindow { Scenario {
    "gui.import_target_short_window", { "gui", "import" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runImportTargetShortWindow (host, ctx); }
} };

std::optional<ScenarioResult> runDpImportConfirmation (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no open modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save initial session");
    ctx.cleanup ([&host, &session, &engine, originalDir, restore]
    {
        drainModals (host);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        engine.getUndoManager().clearUndoHistory();
    });
    for (const auto* name : { "ZZ0000_1.wav", "ZZ0001_1.wav", "ZZ0001_2.wav", "ZZ0003_2.wav" })
    {
        dusk::audio::WriteSpec spec;
        spec.sampleRate = 48000;
        spec.numChannels = 1;
        spec.bitsPerSample = 16;
        auto writer = dusk::audio::FileWriter::create (ctx.tempDir() / name, spec);
        std::array<float, 256> silence {};
        const float* channels[] = { silence.data() };
        if (! writer || ! writer->write (channels, 1, 256) || ! writer->flush())
            return ScenarioResult::fail ("could not create DP audio fixture");
    }
    const auto read = [] (const std::filesystem::path& path)
    {
        std::ifstream input (path);
        return std::string (std::istreambuf_iterator<char> (input), std::istreambuf_iterator<char>());
    };
    const auto before = read (restore);
    const auto unchanged = [&ctx, &session, before, read]
    {
        const auto after = ctx.tempDir() / "after.json";
        ctx.expect (SessionSerializer::save (session, after), "could not save comparison snapshot");
        ctx.expect (! before.empty() && read (after) == before, "DP confirmation changed the session before import");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickFileMenu(), "File menu is unavailable"); } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Import DP-24/32 Session (experimental)..."), "DP import menu item is unavailable"); } });
    const auto chooseFile = [&host, &ctx]
    {
        ctx.expect (host.focusFileName(), "DP file browser did not open");
       #if defined (__APPLE__)
        host.pressPeerKey ("command + A", 'a');
       #else
        host.pressPeerKey ("ctrl + A", 'a');
       #endif
        for (const char ch : (ctx.tempDir() / "ZZ0000_1.wav").string())
            host.pressPeerKey (ch == ' ' ? "Space" : std::string (1, ch), ch);
        ctx.expect (host.clickModalButton ("Open"), "DP browser Open button is unavailable");
    };
    steps->push_back ({ 250, chooseFile });
    steps->push_back ({ 500, [&host, &ctx, unchanged]
    {
        const auto text = host.dpImportSummary();
        if (ctx.expect (text.size() == 3, "DP confirmation is not visible and ready to import"))
        {
            ctx.expect (text[0] == "Import DP-24/32 Session", "DP confirmation title is wrong");
            for (const auto* part : { "3 tracks", "1 stereo pair", "48.0 kHz / 16-bit" })
                ctx.expect (text[1].find (part) != std::string::npos, std::string ("DP summary omits ") + part);
            ctx.expect (text[2].find ("ZZ0003: right channel without left; imported as mono.") != std::string::npos,
                        "DP confirmation omits the orphan-channel warning");
        }
        unchanged();
        ctx.expect (host.clickModalButton ("Cancel"), "DP confirmation Cancel is unavailable");
    } });
    // Then an import itself, which clears the undo history.
    steps->push_back ({ 250, [&host, &ctx, &session, &engine, unchanged]
    {
        ctx.expect (host.modalStackEmpty(), "Cancel left the DP confirmation open");
        unchanged();
        const auto importDir = ctx.tempDir() / "session";
        std::error_code error;
        std::filesystem::create_directories (importDir / "audio", error);
        applySessionDirectory (session, importDir);
        auto& um = engine.getUndoManager();
        um.beginNewTransaction ("Edit before the import");
        um.perform (new ParamEditAction ([] {}, [] {}));
        ctx.expect (host.clickFileMenu(), "File menu is unavailable");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Import DP-24/32 Session (experimental)..."), "DP import menu item is unavailable"); } });
    steps->push_back ({ 250, chooseFile });
    // The job writes into the audio folder the session had when it started, so
    // Save and Save As wait for it the way they wait for a render.
    steps->push_back ({ 500, [&host, &ctx]
    {
        if (! ctx.expect (host.clickModalButton ("Import"), "DP confirmation Import is unavailable")) return;
        for (const auto* chord : { "command + S", "command + shift + S" })
        {
            host.pressPeerKey (chord);
            ctx.expect (host.statusMessage() == "Session not saved: finish or cancel the DP import first",
                        std::string (chord) + " during the DP import left the status '" + host.statusMessage() + "'");
            ctx.expect (host.modalText().rfind ("Save session as...", 0) != 0,
                        std::string (chord) + " during the DP import opened Save As");
        }
    } });
    Step imported { 300, [&host, &ctx, &engine]
    {
        ctx.expect (host.modalText().rfind ("Import DP-24/32 Session\nImported ", 0) == 0,
                    "the DP import ended with '" + host.modalText() + "'");
        ctx.expect (! engine.getUndoManager().canUndo(), "the DP import left the undo history from before it");
        dismissAlert (host);
    } };
    imported.until = [&host] { return host.modalText().rfind ("Import DP-24/32 Session\n", 0) == 0; };
    imported.timeout = "the DP import did not finish";
    steps->push_back (std::move (imported));
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar dpImportConfirmation { Scenario {
    "gui.dp_import_confirmation", { "gui", "import" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runDpImportConfirmation (host, ctx); }
} };

// Open in an import browser with no file picked closes it like Cancel. Both
// browsers start in the Music folder, and the folder shown must not come back
// from Open as the user's choice.
std::optional<ScenarioResult> runImportOpenNothingPicked (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    if (! ctx.engine().getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no open modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save initial session");
    ctx.cleanup ([&host, &session, originalDir, restore]
    {
        drainModals (host);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
    });
    const auto textOnly = ctx.tempDir() / "TextOnly";
    std::filesystem::create_directory (textOnly);
    if (! (std::ofstream (textOnly / "notes.txt") << "not audio"))
        return ScenarioResult::fail ("could not write text fixture");
    const auto read = [] (const std::filesystem::path& path)
    {
        std::ifstream input (path);
        return std::string (std::istreambuf_iterator<char> (input), std::istreambuf_iterator<char>());
    };
    const auto before = read (restore);
    const auto unchanged = [&ctx, &session, before, read] (const std::string& browser)
    {
        const auto after = ctx.tempDir() / "after.json";
        ctx.expect (SessionSerializer::save (session, after), "could not save comparison snapshot");
        ctx.expect (! before.empty() && read (after) == before, browser + " Open with nothing picked changed the session");
    };
    const auto pickNothing = [&host, &ctx] (
        const std::string& menuItem, const std::filesystem::path& folder)
    {
        return std::vector<Step> {
            { 150, [&host, &ctx] { ctx.expect (host.clickFileMenu(), "File menu is unavailable"); } },
            { 200, [&host, &ctx, menuItem]
            { ctx.expect (host.clickContextMenuItem (menuItem), "menu item is unavailable: " + menuItem); } },
            { 250, [&host, &ctx, menuItem, folder]
            {
                if (! ctx.expect (host.clickFileBrowserControl (true), menuItem + " opened no file browser")) return;
               #if defined (__APPLE__)
                host.pressPeerKey ("command + A", 'a');
               #else
                host.pressPeerKey ("ctrl + A", 'a');
               #endif
                for (const char ch : folder.string()) host.pressPeerKey (ch == ' ' ? "Space" : std::string (1, ch), ch);
                host.pressPeerKey ("Return", 0);
            } },
            { 400, [&host, &ctx, menuItem]
            { ctx.expect (host.clickModalButton ("Open"), menuItem + " browser has no Open button"); } },
            { 600, [&host, &ctx, menuItem]
            {
                ctx.expect (host.modalStackEmpty(),
                            menuItem + " Open with nothing picked left a modal up: " + host.modalText());
                drainModals (host);
            } },
        };
    };
    auto steps = std::make_shared<std::vector<Step>>();
    for (const auto* item : { "Import Audio or MIDI...", "Import DP-24/32 Session (experimental)..." })
    {
        for (auto& step : pickNothing (item, textOnly)) steps->push_back (std::move (step));
        steps->push_back ({ 50, [unchanged, item] { unchanged (item); } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar importOpenNothingPicked { Scenario {
    "gui.import_open_nothing_picked", { "gui", "import" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runImportOpenNothingPicked (host, ctx); }
} };

// Opens the import browser on a folder of `files` empty WAVs, which takes it a
// while to list, and runs `then` once it is listing.
void browseCrowdedFolder (GuiHost& host, ScenarioContext& ctx, int files, std::function<void()> then)
{
    const auto crowded = ctx.tempDir() / "Crowded";
    std::filesystem::create_directory (crowded);
    for (int index = 0; index < files; ++index)
        std::ofstream (crowded / ("take " + std::to_string (index) + ".wav"));
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressPeerKey (commandChord ('i'), 'i'), "the import shortcut was not handled"); } });
    steps->push_back ({ 300, [&host, &ctx, crowded]
    {
        if (! ctx.expect (host.clickFileBrowserControl (true), "the import browser did not open")) return;
        typeReplacing (host, crowded.string());
        host.pressPeerKey ("Return", 0);
    } });
    steps->push_back ({ 200, [] {}, [&host, crowded]
                        { return host.fileBrowserFolder().lexically_normal() == crowded.lexically_normal(); },
                        "the browser did not move to the crowded folder" });
    runSteps (ctx, steps, std::move (then));
}

std::optional<ScenarioResult> runFileBrowserCancelScan (GuiHost& host, ScenarioContext& ctx)
{
    // Enough files that listing them takes the browser several seconds.
    static constexpr int kFiles = 30000;
    static constexpr int kStopMs = 1500;
    ctx.cleanup ([&host] { drainModals (host); });
    browseCrowdedFolder (host, ctx, kFiles, [&host, &ctx]
    {
        if (! host.fileBrowserScanning())
        {
            ctx.complete (ctx.verdict().status == ScenarioStatus::Pass
                              ? ScenarioResult::skip ("the folder was listed before the browser could be cancelled")
                              : ctx.verdict());
            return;
        }
        const int panels = host.fileBrowserPanels();
        if (! ctx.expect (host.clickModalButton ("Cancel"), "the browser has no Cancel button"))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        // The closed modal destroys the browser a tick later, which retires it only
        // if its scan is still running then; retired browsers are swept every 250 ms.
        ctx.waitUntil ([&host, panels]
                       { return host.retiredFileBrowserScans() == 1 || host.fileBrowserPanels() < panels; },
                       kStopMs,
                       [&host, &ctx]
                       {
                           if (host.retiredFileBrowserScans() == 0)
                           {
                               ctx.complete (ctx.verdict().status == ScenarioStatus::Pass
                                                 ? ScenarioResult::skip ("the folder was listed before the cancelled "
                                                                         "browser was destroyed")
                                                 : ctx.verdict());
                               return;
                           }
                           ctx.waitUntil ([&host] { return host.retiredFileBrowserScans() == 0; }, kStopMs,
                                          [&ctx] { ctx.complete (ctx.verdict()); },
                                          "a browser cancelled mid-scan was still listing its folder "
                                              + std::to_string (kStopMs) + " ms later");
                       },
                       "the browser cancelled mid-scan was neither destroyed nor kept for its scan "
                           + std::to_string (kStopMs) + " ms later");
    });
    return std::nullopt;
}

const ScenarioRegistrar fileBrowserCancelScan { Scenario {
    "gui.file_browser_cancel_scan", { "gui", "import" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runFileBrowserCancelScan (host, ctx); }
} };

// macOS holds a protected folder such as Desktop until the user answers its
// privacy prompt. The window has to keep handling events meanwhile, and the
// browser moves to the folder once the prompt is answered.
std::optional<ScenarioResult> runFileBrowserHeldFolder (GuiHost& host, ScenarioContext& ctx)
{
    static constexpr int kTickMs = 100;
    static constexpr int kLateMs = 1000;
    static constexpr int kMoveMs = 2000;
    const auto held = ctx.tempDir() / "Held";
    std::filesystem::create_directory (held);
    ctx.cleanup ([&host]
    {
        host.holdFileBrowserFolderChecks (false);
        drainModals (host);
    });
    auto before = std::make_shared<std::filesystem::path>();
    auto returnPressed = std::make_shared<std::chrono::steady_clock::time_point>();
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressPeerKey (commandChord ('i'), 'i'), "the import shortcut was not handled"); } });
    steps->push_back ({ 300, [&host, &ctx, held, before, returnPressed]
    {
        if (! ctx.expect (host.clickFileBrowserControl (true), "the import browser did not open")) return;
        *before = host.fileBrowserFolder();
        host.holdFileBrowserFolderChecks (true);
        typeReplacing (host, held.string());
        *returnPressed = std::chrono::steady_clock::now();
        host.pressPeerKey ("Return", 0);
    } });
    steps->push_back ({ kTickMs, [&host, &ctx, before, returnPressed]
    {
        const auto late = std::chrono::duration_cast<std::chrono::milliseconds> (
                              std::chrono::steady_clock::now() - *returnPressed).count() - kTickMs;
        ctx.expect (late < kLateMs, "the window handled nothing for " + std::to_string (late)
                                        + " ms while the typed folder was being checked");
        ctx.expect (host.fileBrowserFolder() == *before, "the browser moved before the typed folder was checked");
        host.holdFileBrowserFolderChecks (false);
    } });
    runSteps (ctx, steps, [&host, &ctx, held]
    {
        ctx.waitUntil ([&host, held] { return host.fileBrowserFolder().lexically_normal() == held.lexically_normal(); },
                       kMoveMs, [&ctx] { ctx.complete (ctx.verdict()); },
                       "the browser did not move to the typed folder once it was checked");
    });
    return std::nullopt;
}

const ScenarioRegistrar fileBrowserHeldFolder { Scenario {
    "gui.file_browser_held_folder", { "gui", "import" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runFileBrowserHeldFolder (host, ctx); }
} };

// Up clicks made while a folder check is held each go up a level, and a folder
// the user creates meanwhile is where the browser stays.
std::optional<ScenarioResult> runFileBrowserHeldUp (GuiHost& host, ScenarioContext& ctx)
{
    static constexpr int kMoveMs = 2000;
    static constexpr int kSettleMs = 500;
    const auto top = (ctx.tempDir() / "Top").lexically_normal();
    const auto deepest = top / "Middle" / "Bottom";
    const auto created = top / "New folder";
    std::filesystem::create_directories (deepest);
    ctx.cleanup ([&host]
    {
        host.holdFileBrowserFolderChecks (false);
        drainModals (host);
    });
    const auto shown = [&host] { return host.fileBrowserFolder().lexically_normal(); };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx] { ctx.expect (host.clickFileMenu(), "the File menu is unavailable"); } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Save as..."), "the File menu has no Save as..."); } });
    steps->push_back ({ 700, [&host, &ctx, deepest]
    {
        if (! ctx.expect (host.clickFileBrowserControl (true), "the Save As browser did not open")) return;
        typeReplacing (host, deepest.string());
        host.pressPeerKey ("Return", 0);
    } });
    runSteps (ctx, steps, [&host, &ctx, shown, top, deepest, created]
    {
        ctx.waitUntil ([shown, deepest] { return shown() == deepest.lexically_normal(); }, kMoveMs, [&host, &ctx, shown, top, created]
        {
            auto upSteps = std::make_shared<std::vector<Step>>();
            upSteps->push_back ({ 0, [&host, &ctx]
            {
                host.holdFileBrowserFolderChecks (true);
                ctx.expect (host.clickFileBrowserUp(), "the browser has no Up button");
            } });
            upSteps->push_back ({ 100, [&host, &ctx] { ctx.expect (host.clickFileBrowserUp(), "the browser has no Up button"); } });
            upSteps->push_back ({ 100, [&host] { host.holdFileBrowserFolderChecks (false); } });
            runSteps (ctx, upSteps, [&host, &ctx, shown, top, created]
            {
                ctx.waitUntil ([shown, top] { return shown() == top; }, kMoveMs, [&host, &ctx, shown, top, created]
                {
                    auto createSteps = std::make_shared<std::vector<Step>>();
                    createSteps->push_back ({ kSettleMs, [&host, &ctx, shown, top]
                    {
                        ctx.expect (shown() == top, "two Up clicks during a held check went past the folder two levels up, to "
                                                        + shown().string());
                        host.holdFileBrowserFolderChecks (true);
                        ctx.expect (host.clickFileBrowserUp(), "the browser has no Up button");
                        ctx.expect (host.clickModalButton ("New folder..."), "the Save As browser has no New folder button");
                    } });
                    createSteps->push_back ({ 100, [&host, &ctx]
                    { ctx.expect (host.clickModalButton ("Create"), "the new folder row has no Create button"); } });
                    createSteps->push_back ({ 100, [&host, &ctx, shown, created]
                    {
                        ctx.expect (shown() == created, "Create did not open the new folder");
                        host.holdFileBrowserFolderChecks (false);
                    } });
                    createSteps->push_back ({ kSettleMs, [&ctx, shown, created]
                    {
                        ctx.expect (shown() == created, "an Up held on its check ran after the new folder opened and moved the browser to "
                                                            + shown().string());
                    } });
                    runSteps (ctx, createSteps, [&ctx] { ctx.complete (ctx.verdict()); });
                }, "two Up clicks during a held check did not reach the folder two levels up");
            });
        }, "the Save As browser did not move to the typed folder");
    });
    return std::nullopt;
}

const ScenarioRegistrar fileBrowserHeldUp { Scenario {
    "gui.file_browser_held_up", { "gui", "import" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runFileBrowserHeldUp (host, ctx); }
} };

// Keys a file browser does not take stay in it. Typed into the Import
// browser's "file:" field, read-only there, no key does anything, the transport
// keys included: R, P, L, ., Space and [ leave the transport, the loop and punch
// alone. From the Import browser's file list and the Save As file list, M, C,
// Shift+C, T and ? leave the session and the window alone, and L still toggles
// the loop.
std::optional<ScenarioResult> runFileBrowserKeysStayInside (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& transport = ctx.engine().getTransport();
    if (! transport.isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save the initial session");
    const bool loop = transport.isLoopEnabled();
    const bool punch = transport.isPunchEnabled();
    const bool shown = host.setTimelineShown (true);
    ctx.keep (session.metronomeEnabled);
    ctx.keep (session.countInEnabled);
    ctx.cleanup ([&host, &session, &transport, originalDir, restore, loop, punch, shown]
    {
        drainModals (host);
        if (! transport.isStopped()) transport.setState (Transport::State::Stopped);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
        transport.setLoopEnabled (loop);
        transport.setPunchEnabled (punch);
        host.setTimelineShown (shown);
    });

    struct Browser { const char* menuItem; const char* where; bool fileField; };
    const std::array<Browser, 2> browsers {{
        { "Import Audio or MIDI...", "the Import file list", true },
        { "Save as...", "the Save As file list", false },
    }};
    auto steps = std::make_shared<std::vector<Step>>();
    for (const auto browser : browsers)
    {
        const std::string where = browser.where;
        pushFileMenuSteps (host, ctx, *steps, browser.menuItem);
        if (browser.fileField)
        {
            auto loopRange = std::make_shared<std::pair<std::int64_t, std::int64_t>>();
            steps->push_back ({ 400, [&host, &ctx]
            {
                ctx.expect (! host.focusFileName(), "the Import browser's file field takes typing");
                ctx.expect (host.clickFileBrowserFileField(), "the Import browser's file field is not there to click");
            } });
            steps->push_back ({ 100, [&host, &ctx, &session, &transport, loopRange]
            {
                ctx.expect (host.fileBrowserFocus() == "file",
                            "the click left the keyboard on '" + host.fileBrowserFocus() + "', not the file field");
                *loopRange = { transport.getLoopStart(), transport.getLoopEnd() };
                const auto markers = session.getMarkers().size();
                const bool click = session.metronomeEnabled.load();
                for (const char key : std::string ("mcrpl.[ "))
                    host.pressPeerKey (key == ' ' ? std::string ("spacebar") : keyCodeDescription (key), key);
                ctx.expect (session.getMarkers().size() == markers, "M typed into the file field dropped a marker");
                ctx.expect (session.metronomeEnabled.load() == click, "C typed into the file field toggled the click");
            } });
            steps->push_back ({ 300, [&host, &ctx, &transport, loop, punch, loopRange]
            {
                ctx.expect (transport.isStopped(), "R or Space typed into the file field started the transport");
                ctx.expect (transport.isLoopEnabled() == loop, "L typed into the file field toggled the loop");
                ctx.expect (transport.isPunchEnabled() == punch, "P typed into the file field toggled punch");
                ctx.expect (transport.getLoopStart() == loopRange->first && transport.getLoopEnd() == loopRange->second,
                            "[ typed into the file field moved the loop");
                ctx.expect (! host.shortcutsOpen() && ! host.fileBrowserFolder().empty(),
                            "the Import browser is no longer on top after typing into its file field: '"
                                + host.modalText() + "'");
            } });
        }
        steps->push_back ({ 200, [&host, &ctx, where]
        { ctx.expect (host.clickFileBrowserControl (false), where + " is not there to click"); } });
        auto before = std::make_shared<std::array<bool, 3>>();
        auto markers = std::make_shared<std::size_t>();
        steps->push_back ({ 100, [&host, &ctx, &session, before, markers, where]
        {
            ctx.expect (host.fileBrowserFocus() == "Files",
                        "the click left the keyboard on '" + host.fileBrowserFocus() + "', not " + where);
            *markers = session.getMarkers().size();
            *before = { session.metronomeEnabled.load(), session.countInEnabled.load(), host.timelineViewMatches (true) };
            host.pressPeerKey (keyCodeDescription ('m'), 'm');
            host.pressPeerKey (keyCodeDescription ('c'), 'c');
            host.pressPeerKey ("shift + C", 'C');
            host.pressPeerKey ("T", 't');
            host.pressPeerKey ("shift + /", '?');
        } });
        steps->push_back ({ 300, [&host, &ctx, &session, &transport, before, markers, where]
        {
            ctx.expect (session.getMarkers().size() == *markers, "M in " + where + " dropped a marker");
            ctx.expect (session.metronomeEnabled.load() == (*before)[0], "C in " + where + " toggled the click");
            ctx.expect (session.countInEnabled.load() == (*before)[1], "Shift+C in " + where + " toggled the count-in");
            ctx.expect (host.timelineViewMatches (true) == (*before)[2], "T in " + where + " toggled the timeline");
            ctx.expect (! host.shortcutsOpen() && ! host.fileBrowserFolder().empty(),
                        "the File browser is no longer on top after keys in " + where + ": '" + host.modalText() + "'");
            *before = { transport.isLoopEnabled(), false, false };
            host.pressPeerKey (keyCodeDescription ('l'), 'l');
        } });
        steps->push_back ({ 200, [&host, &ctx, &transport, before, where]
        {
            ctx.expect (transport.isLoopEnabled() != (*before)[0], "L in " + where + " did not reach the loop");
            transport.setLoopEnabled ((*before)[0]);
            drainModals (host);
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar fileBrowserKeysStayInside { Scenario {
    "gui.file_browser_keys_stay_inside", { "gui", "import", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runFileBrowserKeysStayInside (host, ctx); }
} };

// Tab from the path box, which takes typing in a text box of its own, goes on to
// the browser's next control rather than back to its first, and Shift+Tab from
// there comes back to the path box.
std::optional<ScenarioResult> runFileBrowserTabFromPath (GuiHost& host, ScenarioContext& ctx)
{
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no modal");
    ctx.cleanup ([&host] { drainModals (host); });
    auto steps = std::make_shared<std::vector<Step>>();
    pushFileMenuSteps (host, ctx, *steps, "Import Audio or MIDI...");
    steps->push_back ({ 400, [&host, &ctx]
    { ctx.expect (host.clickFileBrowserControl (true), "the Import browser's path box is not there to click"); } });
    steps->push_back ({ 150, [&host, &ctx]
    {
        if (! ctx.expect (host.fileBrowserFocus() == "path",
                          "the click left the keyboard on '" + host.fileBrowserFocus() + "', not the path box"))
            return;
        host.pressPeerKey ("tab");
        const auto next = host.fileBrowserFocus();
        ctx.expect (! next.empty() && next != "path", "Tab from the path box left the keyboard on '" + next + "'");
        host.pressPeerKey ("shift + tab");
        ctx.expect (host.fileBrowserFocus() == "path", "Shift+Tab from '" + next + "' went to '"
                                                           + host.fileBrowserFocus() + "', not back to the path box");
        drainModals (host);
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar fileBrowserTabFromPath { Scenario {
    "gui.file_browser_tab_from_path", { "gui", "import", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runFileBrowserTabFromPath (host, ctx); }
} };

// The import pickers sit inside the main window's content, so a key they decline
// goes straight on to the shortcuts. They pass on only the transport keys and
// Undo, Redo, Save, Save As and Quit, by whatever route the key arrives: from the
// picker itself, from the window after a click on the multi-import picker's dim
// took the keyboard, or with nothing holding the keyboard at all. M, C, Shift+C,
// T and ? do nothing behind the picker, L toggles the loop, and Cmd+Z, Cmd+Y and
// Cmd+Shift+Z reach the history. Tab moves the keyboard round the picker and
// never out to the window behind it.
std::optional<ScenarioResult> runDialogKeysStayInside (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginImportPickerCase (host, ctx)) return early;
    auto& session = ctx.session();
    auto& transport = ctx.engine().getTransport();
    auto& um = ctx.engine().getUndoManager();
    const bool loop = transport.isLoopEnabled();
    ctx.keep (session.metronomeEnabled);
    ctx.keep (session.countInEnabled);
    auto giveBack = std::make_shared<std::function<void()>>();
    ctx.cleanup ([&transport, loop, giveBack]
    {
        if (*giveBack) (*giveBack)();
        transport.setLoopEnabled (loop);
    });
    const auto first = ctx.tempDir() / "First.wav";
    const auto second = ctx.tempDir() / "Second.wav";
    if (! writeStereoSilence (first) || ! writeStereoSilence (second))
        return ScenarioResult::fail ("could not write the audio import fixtures");
    // A session on disk, so Save writes it in place rather than asking where.
    const auto sessionJson = ctx.tempDir() / "Keys" / "session.json";
    std::error_code madeDir;
    std::filesystem::create_directories (sessionJson.parent_path(), madeDir);
    if (! SessionSerializer::save (session, sessionJson) || ! host.openSession (sessionJson))
        return ScenarioResult::fail ("could not open a saved session");

    enum class Route { Picker, AfterDimClick, NothingFocused };
    struct Leg { Route route; const char* where; std::vector<std::filesystem::path> files; };
    const std::vector<Leg> legs {
        { Route::Picker,         "the import target picker",                         { first } },
        { Route::AfterDimClick,  "the multi-import picker after a click on its dim", { first, second } },
        { Route::NothingFocused, "the import target picker with nothing focused",    { first } },
    };
    const auto pickerUp = [&host] (const Leg& leg)
    {
        return leg.files.size() == 1 ? ! host.importTargetRows().empty() : ! host.multiImportRows().empty();
    };
    auto edits = std::make_shared<int> (0);
    auto steps = std::make_shared<std::vector<Step>>();
    for (const auto& leg : legs)
    {
        const std::string where = leg.where;
        steps->push_back ({ 250, [&host, &ctx, leg, where]
        {
            host.setTimelineShown (true);
            ctx.expect (host.dropFilesOnTrack (0, leg.files), "the drop for " + where + " was rejected");
        }, [&host] { return host.modalStackEmpty(); }, "the previous picker did not close" });
        auto before = std::make_shared<std::array<bool, 3>>();
        auto markers = std::make_shared<std::size_t>();
        steps->push_back ({ 300, [&host, &ctx, &session, leg, where, pickerUp, before, markers, giveBack]
        {
            if (! ctx.expect (pickerUp (leg), where + " did not open")) return;
            if (leg.route == Route::AfterDimClick)
            {
                ctx.expect (host.clickModalBackdrop(), "the dim behind " + where + " took no click");
                ctx.expect (pickerUp (leg), "a click on the dim closed " + where);
                ctx.expect (! host.modalHasKeyboardFocus(),
                            "the click on the dim left the keyboard in the picker, so the window's route goes untested");
            }
            else if (leg.route == Route::NothingFocused)
            {
                *giveBack = host.unfocusWindow();
                ctx.expect (! host.modalHasKeyboardFocus(), "the picker kept the keyboard");
            }
            else
            {
                ctx.expect (host.modalHasKeyboardFocus(), where + " did not take the keyboard");
            }
            *markers = session.getMarkers().size();
            *before = { session.metronomeEnabled.load(), session.countInEnabled.load(), host.timelineViewMatches (true) };
            host.pressPeerKey (keyCodeDescription ('m'), 'm');
            host.pressPeerKey (keyCodeDescription ('c'), 'c');
            host.pressPeerKey ("shift + C", 'C');
            host.pressPeerKey ("T", 't');
            host.pressPeerKey ("shift + /", '?');
        } });
        steps->push_back ({ 300, [&host, &ctx, &session, &transport, leg, where, pickerUp, before, markers]
        {
            ctx.expect (session.getMarkers().size() == *markers, "M in " + where + " dropped a marker");
            ctx.expect (session.metronomeEnabled.load() == (*before)[0], "C in " + where + " toggled the click");
            ctx.expect (session.countInEnabled.load() == (*before)[1], "Shift+C in " + where + " toggled the count-in");
            ctx.expect (host.timelineViewMatches (true) == (*before)[2], "T in " + where + " toggled the timeline");
            ctx.expect (! host.shortcutsOpen() && pickerUp (leg),
                        where + " is no longer on top after the keys: '" + host.modalText() + "'");
            (*before)[0] = transport.isLoopEnabled();
            host.pressPeerKey (keyCodeDescription ('l'), 'l');
        } });
        steps->push_back ({ 200, [&host, &ctx, &transport, &um, where, before, edits]
        {
            ctx.expect (transport.isLoopEnabled() != (*before)[0], "L in " + where + " did not reach the loop");
            transport.setLoopEnabled ((*before)[0]);
            um.beginNewTransaction ("Dialog key probe");
            um.perform (new ParamEditAction ([edits] { ++*edits; }, [edits] { --*edits; }));
            const int start = *edits;
            struct Chord { const char* description; const char* name; int after; };
            for (const auto& chord : { Chord { "command + Z", "Cmd+Z", start - 1 },
                                       Chord { "command + Y", "Cmd+Y", start },
                                       Chord { "command + Z", "Cmd+Z", start - 1 },
                                       Chord { "command + shift + Z", "Cmd+Shift+Z", start } })
            {
                host.pressPeerKey (chord.description);
                if (! ctx.expect (*edits == chord.after, std::string (chord.name) + " in " + where + " did not reach the history"))
                    return;
            }
        } });
        if (leg.route == Route::Picker)
        {
            steps->push_back ({ 200, [&host, &ctx, leg, where, pickerUp, sessionJson]
            {
                std::error_code error;
                const auto backdated = std::filesystem::file_time_type::clock::now() - std::chrono::hours (1);
                std::filesystem::last_write_time (sessionJson, backdated, error);
                if (! ctx.expect (! error, "could not backdate the saved session")) return;
                host.pressPeerKey ("command + S");
                ctx.expect (std::filesystem::last_write_time (sessionJson, error) > backdated && ! error,
                            "Cmd+S in " + where + " did not save the session in place");
                ctx.expect (pickerUp (leg), "Cmd+S in " + where + " took the picker down: '" + host.modalText() + "'");
                host.pressPeerKey ("command + shift + S");
            } });
            steps->push_back ({ 300, [&host, &ctx, leg, where, pickerUp]
            {
                if (! ctx.expect (host.modalText().rfind ("Save session as...", 0) == 0,
                                  "Cmd+Shift+S in " + where + " showed '" + host.modalText() + "' rather than Save As"))
                    return;
                host.closeTopModal();
                ctx.expect (pickerUp (leg), "closing Save As did not leave " + where + " on top");
            } });
        }
        steps->push_back ({ 100, [&host, &ctx, leg, where, pickerUp, giveBack]
        {
            for (int press = 0; press < 8; ++press)
            {
                host.pressPeerKey (press % 3 == 2 ? "shift + tab" : "tab");
                if (! ctx.expect (host.modalHasKeyboardFocus() && pickerUp (leg),
                                  "Tab " + std::to_string (press + 1) + " in " + where
                                      + " left the keyboard outside the picker"))
                    break;
            }
            if (*giveBack)
            {
                (*giveBack)();
                *giveBack = {};
            }
            drainModals (host);
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar dialogKeysStayInside { Scenario {
    "gui.dialog_keys_stay_inside", { "gui", "import", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runDialogKeysStayInside (host, ctx); }
} };

// A file dropped on the tape strip lands at the playhead, where the drop line
// stands while the file is held over the strip. With Alt held it lands, and
// the line stands, under the pointer, and Alt pressed or let go with the file
// held still moves the line. With the landing point scrolled out of view there
// is no line.
std::optional<ScenarioResult> runDropAtPlayhead (GuiHost& host, ScenarioContext& ctx)
{
    AudioRegion region;
    if (auto early = seedTapeRegion (host, ctx, region)) return early;
    auto& session = ctx.session();
    for (const int t : { 1, 2 })
    {
        if (! session.track (t).regions.empty() || ! session.track (t).midiRegions.current().empty())
            return ScenarioResult::skip ("requires tracks 2 and 3 empty");
        session.track (t).mode.store ((int) Track::Mode::Mono);
        session.track (t).frozen.store (false);
    }
    ctx.cleanup ([&host]
    {
        host.tapeDropLeave();
        host.forceDropAtMouse (std::nullopt);
    });
    host.forceDropAtMouse (false);
    std::error_code error;
    std::filesystem::create_directories (ctx.tempDir() / "session" / "audio", error);
    const auto file = ctx.tempDir() / "Dropped.wav";
    {
        dusk::audio::WriteSpec spec;
        spec.sampleRate = ctx.engine().getCurrentSampleRate();
        spec.numChannels = 1;
        auto writer = dusk::audio::FileWriter::create (file, spec);
        std::array<float, 4800> silence {};
        const float* data[] = { silence.data() };
        if (! writer || ! writer->write (data, 1, 4800) || ! writer->flush())
            return ScenarioResult::fail ("could not write the drop fixture");
    }
    auto& transport = ctx.engine().getTransport();
    const auto playhead = region.timelineStart / 2;
    transport.locate (playhead);
    const auto pointerSample = std::make_shared<std::int64_t> (-1);
    auto steps = std::make_shared<std::vector<Step>>();
    const auto importAfterDrop = [&host, &ctx, steps, file] (int track, std::function<void()> before)
    {
        steps->push_back ({ 300, [&host, &ctx, file, track, before]
        {
            before();
            ctx.expect (host.dropFilesOnTrack (track, { file }), "the drop was rejected");
        } });
        steps->push_back ({ 300, [&host, &ctx]
        { ctx.expect (host.clickModalButton ("Import"), "the import target picker did not open"); } });
    };
    importAfterDrop (1, [&host, &ctx, file, playhead]
    {
        const auto shown = host.tapeDropHover (1, { file });
        if (! ctx.expect (shown.size() == 2, "the drop row is not on screen")) return;
        ctx.expect (shown[0] == host.tapeXForSample (playhead), "the drop line does not stand at the playhead");
        ctx.expect (shown[0] != shown[1], "the pointer sits on the playhead, so the drop proves nothing");
    });
    steps->push_back ({ 700, [&ctx, &session, playhead]
    {
        const auto& regions = session.track (1).regions;
        ctx.expect (regions.size() == 1 && regions.front().timelineStart == playhead,
                    "the dropped file did not land at the playhead");
    } });
    // Alt pressed, then let go, with the files held still: the line follows
    // the key within a tick, so it never stands where the drop would not land.
    const auto heldX = std::make_shared<int> (-1);
    steps->push_back ({ 300, [&host, &ctx, file, playhead, heldX]
    {
        const auto shown = host.tapeDropHold (2, { file });
        if (! ctx.expect (shown.size() == 2 && shown[0] == host.tapeXForSample (playhead),
                          "files held over the strip do not stand the drop line at the playhead"))
            return;
        *heldX = shown[1];
        host.forceDropAtMouse (true);
    } });
    steps->push_back ({ 300, [&host, &ctx, heldX]
    {
        ctx.expect (host.tapeDropLine() == *heldX, "with Alt pressed and the files held still, the drop line did not move to the pointer");
        host.forceDropAtMouse (false);
    } });
    steps->push_back ({ 300, [&host, &ctx, playhead]
    {
        ctx.expect (host.tapeDropLine() == host.tapeXForSample (playhead),
                    "with Alt let go and the files held still, the drop line did not go back to the playhead");
        host.tapeDropLeave();
    } });
    importAfterDrop (2, [&host, &ctx, file, pointerSample, playhead]
    {
        host.forceDropAtMouse (true);
        const auto shown = host.tapeDropHover (2, { file });
        if (! ctx.expect (shown.size() == 2, "the drop row is not on screen")) return;
        ctx.expect (shown[0] == shown[1], "with Alt held the drop line does not stand under the pointer");
        *pointerSample = host.tapeDropPointSample (2);
        ctx.expect (*pointerSample != playhead, "the pointer sits on the playhead, so the drop proves nothing");
    });
    steps->push_back ({ 700, [&ctx, &session, pointerSample]
    {
        const auto& regions = session.track (2).regions;
        ctx.expect (regions.size() == 1 && regions.front().timelineStart == *pointerSample,
                    "with Alt held the dropped file did not land under the pointer");
    } });
    // Scrolled past the playhead, the line hides rather than stand over the names.
    steps->push_back ({ 300, [&host, &ctx, file, playhead]
    {
        host.forceDropAtMouse (false);
        const auto view = host.tapeView();
        if (! ctx.expect (view.size() == 7, "the tape strip is missing")) return;
        auto scrolled = view;
        scrolled[1] = (double) (playhead + (std::int64_t) ctx.engine().getCurrentSampleRate() * 10);
        host.restoreTapeView (scrolled);
        const auto shown = host.tapeDropHover (1, { file });
        ctx.expect (shown.size() == 2 && shown[0] == -1, "the drop line stands for a playhead scrolled out of view");
        host.restoreTapeView (view);
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar dropAtPlayhead { Scenario {
    "gui.drop_at_playhead", { "gui", "import" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runDropAtPlayhead (host, ctx); }
} };

// File > Import Audio or MIDI... refuses while playback runs, and names each
// file it cannot use: bytes that are not audio, a file it may not read, and a
// WAV that opens but holds no audio.
std::optional<ScenarioResult> runImportRefusals (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    if (! transport.isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    const auto rate = engine.getCurrentSampleRate();
    if (rate <= 0.0) return ScenarioResult::skip ("requires a positive engine sample rate");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    const auto locked = ctx.tempDir() / "Unreadable" / "Locked.wav";
    ctx.cleanup ([&host, &engine, &session, &transport, originalDir, restore, locked, state = transport.getState(),
                  position = transport.getPlayhead()]
    {
        std::error_code ignored;
        std::filesystem::permissions (locked, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                      std::filesystem::perm_options::replace, ignored);
        drainModals (host);
        engine.stop();
        transport.setPlayhead (position);
        transport.setState (state);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });
    const auto folder = ctx.tempDir() / "Import session";
    fs::create_directories (folder);
    if (! SessionSerializer::save (*std::make_unique<Session>(), folder / "session.json"))
        return ScenarioResult::fail ("could not write the empty session");
    reopenSavedSession (host, folder / "session.json");
    if (currentSessionDirectory (session) != folder) return ScenarioResult::fail ("could not open the empty session");

    // One file to a folder, so the browser's first row is the file.
    const auto garbage = ctx.tempDir() / "Garbage" / "Garbage.wav";
    const auto empty = ctx.tempDir() / "Empty" / "Empty.wav";
    for (const auto& file : { garbage, locked, empty }) fs::create_directories (file.parent_path());
    if (! writeBytes (garbage, 4096, 'g') || ! writeRampFixture (locked, rate, 4800))
        return ScenarioResult::fail ("could not write the import fixtures");
    if (dusk::audio::FileReader::open (garbage) != nullptr)
        return ScenarioResult::fail ("the garbage fixture opens as audio");
    if (dusk::audio::FileWriter::create (empty, { rate, 1, 24 }) == nullptr)
        return ScenarioResult::fail ("could not write the empty WAV");
    {
        auto reader = dusk::audio::FileReader::open (empty);
        if (reader == nullptr || reader->info().numFrames != 0)
            return ScenarioResult::fail ("the empty WAV fixture does not open as a stream with no frames");
    }
    std::error_code error;
    fs::permissions (locked, fs::perms::none, fs::perm_options::replace, error);
    const bool canLock = ! error && ! std::ifstream (locked).is_open();
    if (! canLock) ctx.note ("an unreadable file stays readable here, as it does for root; that leg is skipped");

    const auto regionCount = [&session]
    {
        std::size_t count = 0;
        for (int index = 0; index < Session::kNumTracks; ++index)
            count += session.track (index).regions.size() + session.track (index).midiRegions.current().size();
        return count;
    };
    const auto audioFolderEmpty = [folder]
    {
        std::error_code ignored;
        return fs::is_empty (folder / "audio", ignored);
    };
    auto steps = std::make_shared<std::vector<Step>>();
    const auto openImport = [&host, &ctx, steps] (const std::string& how)
    {
        steps->push_back ({ 300, [&host, &ctx, how]
        {
            ctx.expect (host.modalStackEmpty(), how + ": a modal was still open");
            ctx.expect (host.clickFileMenu(), how + ": the File menu is unavailable");
        } });
        steps->push_back ({ 200, [&host, &ctx, how]
        {
            ctx.expect (host.clickContextMenuItem ("Import Audio or MIDI..."),
                        how + ": the File menu has no Import Audio or MIDI...");
        } });
    };
    const auto pick = [&host, &ctx, steps, openImport] (const std::string& how, const fs::path& file)
    {
        openImport (how);
        steps->push_back ({ 700, [&host, &ctx, how, file]
        {
            if (! ctx.expect (host.modalText().rfind ("Import audio or MIDI file(s)", 0) == 0,
                              how + ": Import showed '" + host.modalText() + "' rather than its browser")
                || ! ctx.expect (host.clickFileBrowserControl (true), how + ": the Import browser has no path box"))
                return;
            typeReplacing (host, file.parent_path().string());
            ctx.expect (host.pressPeerKey ("Return", 0), how + ": the path box did not take Return");
        } });
        steps->push_back ({ 700, [&host, &ctx, how]
        {
            ctx.expect (host.clickFileBrowserControl (false), how + ": the Import browser has no file list");
            host.pressPeerKey ("Home", 0);
        } });
        steps->push_back ({ 200, [&host, &ctx, how]
        { ctx.expect (host.clickModalButton ("Open"), how + ": the Import browser has no Open button"); } });
    };
    const auto alerted = [&host, &ctx, steps, regionCount, audioFolderEmpty] (const std::string& how,
                                                                              const std::string& expected)
    {
        steps->push_back ({ 700, [&host, &ctx, how, expected, regionCount, audioFolderEmpty]
        {
            ctx.expect (host.modalText() == expected, how + ": the alert read '" + host.modalText() + "'");
            ctx.expect (host.clickModalButton ("OK"), how + ": the alert has no OK button");
            ctx.expect (regionCount() == 0 && audioFolderEmpty(), how + " imported something");
        } });
    };

    steps->push_back ({ 200, [&transport] { transport.setState (Transport::State::Playing); } });
    openImport ("while playing");
    alerted ("while playing", "Import\nStop playback before importing files.");
    steps->push_back ({ 100, [&engine, &transport]
    {
        engine.stop();
        transport.setPlayhead (0);
    } });
    pick ("bytes that are not audio", garbage);
    alerted ("bytes that are not audio", "Import audio\nUnsupported or unreadable audio file: Garbage.wav");
    if (canLock)
    {
        pick ("a file it may not read", locked);
        alerted ("a file it may not read", "Import audio\nUnsupported or unreadable audio file: Locked.wav");
    }
    pick ("a WAV with no audio", empty);
    steps->push_back ({ 700, [&host, &ctx]
    {
        ctx.expect (host.clickModalButton ("Import"),
                    "a WAV with no audio: '" + host.modalText() + "' rather than the target picker's Import");
    } });
    alerted ("a WAV with no audio", "Import audio failed\nAudio file reports an empty or invalid stream");
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar importRefusals { Scenario {
    "gui.import_refusals", { "gui", "import", "messages" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runImportRefusals (host, ctx); }
} };

bool writeMidiImportFixture (const std::filesystem::path& path)
{
    const unsigned char midi[] = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 1, 0xe0,
        'M', 'T', 'r', 'k', 0, 0, 0, 13,
        0, 0x90, 60, 100, 0x83, 0x60, 0x80, 60, 0, 0, 0xff, 0x2f, 0
    };
    std::ofstream output (path, std::ios::binary);
    output.write (reinterpret_cast<const char*> (midi), sizeof (midi));
    output.close();
    return static_cast<bool> (output);
}

bool writeSilenceFixture (const std::filesystem::path& path, double sampleRate, int frames)
{
    auto writer = dusk::audio::FileWriter::create (path, { sampleRate, 1, 24 });
    std::vector<float> silence ((std::size_t) frames);
    const float* data[] = { silence.data() };
    return writer != nullptr && writer->write (data, 1, frames) && writer->flush();
}

// Each imported file is its own undo step. Undoing a recording made on the
// track before the import leaves the imported region on it, once the import's
// own step has gone, and redo brings both back; the track takes its old name
// back with the import's undo. Audio and MIDI alike. An import that switched the
// track's mode puts the old mode back with the same undo.
std::optional<ScenarioResult> runImportUndoStep (GuiHost& host, ScenarioContext& ctx)
{
    AudioRegion region;
    if (auto early = seedTapeRegion (host, ctx, region)) return early;
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    static constexpr int audioTrack = 1;
    static constexpr int midiTrack = 2;
    for (const int t : { audioTrack, midiTrack })
    {
        auto& track = session.track (t);
        if (! track.regions.empty() || ! track.midiRegions.current().empty())
            return ScenarioResult::skip ("requires tracks 2 and 3 empty");
        ctx.keep (track.mode);
        ctx.keep (track.frozen);
        track.frozen.store (false);
        track.name = std::to_string (t + 1).c_str();
    }
    session.track (audioTrack).mode.store ((int) Track::Mode::Mono);
    session.track (midiTrack).mode.store ((int) Track::Mode::Midi);
    const auto wav = ctx.tempDir() / "Dropped.wav";
    const auto mid = ctx.tempDir() / "Notes.mid";
    if (! writeSilenceFixture (wav, engine.getTimelineSampleRate(), 4800) || ! writeMidiImportFixture (mid))
        return ScenarioResult::fail ("could not write the import fixtures");
    auto& transport = engine.getTransport();
    transport.locate (0);

    // The take a record pass would have committed, as RecordCommitAction holds it.
    auto recorded = region;
    MidiRegion played;
    played.timelineStart = region.timelineStart;
    played.lengthInTicks = 1920;
    played.lengthInSamples = region.lengthInSamples;
    const auto count = [&session] (int t)
    {
        return t == audioTrack ? (int) session.track (t).regions.size() : (int) session.track (t).midiRegions.current().size();
    };
    auto steps = std::make_shared<std::vector<Step>>();
    for (const int t : { audioTrack, midiTrack })
    {
        const std::string what = t == audioTrack ? "audio" : "MIDI";
        steps->push_back ({ 300, [&host, &ctx, &session, &engine, t, recorded, played, wav, mid]
        {
            RecordCommitAction::TrackDiff diff;
            diff.trackIndex = t;
            if (t == audioTrack)
            {
                diff.audioAfter = { recorded };
                session.track (t).regions = diff.audioAfter;
            }
            else
            {
                diff.midiAfter = { played };
                session.track (t).midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (diff.midiAfter));
            }
            auto& um = engine.getUndoManager();
            um.beginNewTransaction ("Record");
            um.perform (new RecordCommitAction (session, engine, { diff }));
            ctx.expect (host.dropFilesOnTrack (t, { t == audioTrack ? wav : mid }), "the drop was rejected");
        } });
        steps->push_back ({ 300, [&host, &ctx, what]
        { ctx.expect (host.clickModalButton ("Import"), "the import target picker did not open for the " + what + " file"); } });
        steps->push_back ({ 700, [&host, &ctx, &session, &engine, t, what, count]
        {
            ctx.expect (count (t) == 2, "the " + what + " import did not add a region beside the recorded one");
            ctx.expect (session.track (t).name.toStdString() == (t == audioTrack ? "Dropped" : "Notes"),
                        "the " + what + " import did not name the track after the file");
            ctx.expect (engine.getUndoManager().getUndoDescription() == "Import file",
                        "the " + what + " import is not its own undo step");
            ctx.expect (host.pressKey ("command + Z"), "Undo was not handled");
        } });
        steps->push_back ({ 300, [&host, &ctx, &session, t, what, count]
        {
            ctx.expect (count (t) == 1, "undoing the " + what + " import did not take off its region alone");
            ctx.expect (session.track (t).name.toStdString() == std::to_string (t + 1),
                        "undoing the " + what + " import did not give the track its name back");
            ctx.expect (host.pressKey ("command + Z"), "Undo was not handled");
        } });
        steps->push_back ({ 300, [&host, &ctx, t, count]
        {
            ctx.expect (count (t) == 0, "undoing the recording did not take its region off");
            ctx.expect (host.pressKey ("command + shift + Z") && host.pressKey ("command + shift + Z"), "Redo was not handled");
        } });
        steps->push_back ({ 300, [&ctx, &session, t, what, count]
        {
            ctx.expect (count (t) == 2, "redo did not bring back the recording and the " + what + " import");
            ctx.expect (session.track (t).name.toStdString() == (t == audioTrack ? "Dropped" : "Notes"),
                        "redoing the " + what + " import did not name the track again");
        } });
    }
    // The audio track again, now holding the recording and the imported file.
    auto& switched = session.track (audioTrack);
    const auto midiCount = [&switched] { return (int) switched.midiRegions.current().size(); };
    steps->push_back ({ 300, [&host, &ctx, mid]
    { ctx.expect (host.dropFilesOnTrack (audioTrack, { mid }), "the drop on the audio track was rejected"); } });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Import"), "the import target picker did not open for the switching import"); } });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Switch"), "the import did not ask to switch the track to MIDI"); } });
    steps->push_back ({ 700, [&host, &ctx, &engine, &switched, midiCount]
    {
        ctx.expect (switched.mode.load() == (int) Track::Mode::Midi && midiCount() == 1,
                    "the switching import did not turn the track to MIDI and add its region");
        ctx.expect (engine.getUndoManager().getUndoDescription() == "Import file",
                    "the switching import is not its own undo step");
        ctx.expect (host.pressKey ("command + Z"), "Undo was not handled");
    } });
    steps->push_back ({ 300, [&host, &ctx, &switched, midiCount]
    {
        ctx.expect (midiCount() == 0 && switched.regions.size() == 2,
                    "undoing the switching import did not take its region alone off");
        ctx.expect (switched.mode.load() == (int) Track::Mode::Mono,
                    "undoing the switching import left the track in MIDI mode");
        ctx.expect (host.pressKey ("command + shift + Z"), "Redo was not handled");
    } });
    steps->push_back ({ 300, [&ctx, &switched, midiCount]
    {
        ctx.expect (switched.mode.load() == (int) Track::Mode::Midi && midiCount() == 1,
                    "redoing the switching import did not switch the track and bring its region back");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar importUndoStep { Scenario {
    "gui.import_undo_step", { "gui", "import", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runImportUndoStep (host, ctx); }
} };

// The channel strip shows the mode its track is in however the mode changed: an
// import that switched the track, the undo of that import and its redo.
std::optional<ScenarioResult> runImportSwitchStripMode (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& track = session.track (kStripIndex);
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    if (! track.regions.empty() || ! track.midiRegions.current().empty())
        return ScenarioResult::skip ("requires an empty first track");
    auto* strip = host.strip (kStripIndex);
    if (strip == nullptr) return ScenarioResult::fail ("the first channel strip is unavailable");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save the initial session");
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Recording);
    const bool shown = host.setTimelineShown (true);
    ctx.cleanup ([&host, &session, &engine, originalDir, restore, shown]
    {
        drainModals (host);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        engine.getUndoManager().clearUndoHistory();
        host.setTimelineShown (shown);
    });
    const auto mid = ctx.tempDir() / "Notes.mid";
    if (! writeMidiImportFixture (mid)) return ScenarioResult::fail ("could not write the MIDI import fixture");
    const auto importDir = ctx.tempDir() / "session";
    std::filesystem::create_directories (importDir / "audio");
    applySessionDirectory (session, importDir);
    ctx.keep (track.frozen);
    track.frozen.store (false);
    track.mode.store ((int) Track::Mode::Mono);
    engine.getUndoManager().clearUndoHistory();

    const auto shows = [strip] (Track::Mode mode) { return [strip, mode] { return strip->modeControlsMatch ((int) mode); }; };
    constexpr int kFollowBoundMs = 3000;
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&host, &ctx, mid]
    { ctx.expect (host.dropFilesOnTrack (kStripIndex, { mid }), "the drop on the first track was rejected"); },
      shows (Track::Mode::Mono), "the strip did not show the Mono track the case set up", {}, kFollowBoundMs });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Import"), "the import target picker did not open"); } });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Switch"), "the import did not ask to switch the track to MIDI"); } });
    steps->push_back ({ 300, [&host, &ctx, &track]
    {
        ctx.expect (track.mode.load() == (int) Track::Mode::Midi, "the import did not switch the track to MIDI");
        ctx.expect (host.pressKey ("command + Z"), "Undo was not handled");
    }, shows (Track::Mode::Midi), "the strip's mode selector did not follow the import's switch to MIDI", {},
       kFollowBoundMs });
    steps->push_back ({ 0, [&host, &ctx, &track]
    {
        ctx.expect (track.mode.load() == (int) Track::Mode::Mono, "undoing the import left the track in MIDI mode");
        ctx.expect (host.pressKey ("command + shift + Z"), "Redo was not handled");
    }, shows (Track::Mode::Mono), "the strip's mode selector did not follow the undo back to Mono", {},
       kFollowBoundMs });
    steps->push_back ({ 0, [&ctx, &track]
    { ctx.expect (track.mode.load() == (int) Track::Mode::Midi, "redoing the import did not switch the track again"); },
      shows (Track::Mode::Midi), "the strip's mode selector did not follow the redo to MIDI", {}, kFollowBoundMs });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar importSwitchStripMode { Scenario {
    "gui.import_switch_strip_mode", { "gui", "import", "undo", "strip" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runImportSwitchStripMode (host, ctx); }
} };

// A row of the multi-file picker whose track is in the wrong mode for its file
// says the mode will flip, and the import flips it as part of that file's undo
// step: a stereo file onto a mono track, a MIDI file onto another.
std::optional<ScenarioResult> runMultiImportSwitchesMode (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    if (engine.getStage() != AudioEngine::Stage::Recording && engine.getStage() != AudioEngine::Stage::Mixing)
        return ScenarioResult::skip ("requires a stage with the timeline");
    for (const int t : { 0, 1 })
        if (! session.track (t).regions.empty() || ! session.track (t).midiRegions.current().empty())
            return ScenarioResult::skip ("requires the first two tracks empty");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save the initial session");
    const bool shown = host.setTimelineShown (true);
    ctx.cleanup ([&host, &session, &engine, originalDir, restore, shown]
    {
        drainModals (host);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        engine.getUndoManager().clearUndoHistory();
        host.setTimelineShown (shown);
    });
    const auto importDir = ctx.tempDir() / "session";
    std::filesystem::create_directories (importDir / "audio");
    applySessionDirectory (session, importDir);
    const auto wav = ctx.tempDir() / "Wide.wav";
    const auto mid = ctx.tempDir() / "Notes.mid";
    {
        auto writer = dusk::audio::FileWriter::create (wav, { engine.getTimelineSampleRate(), 2, 24 });
        std::vector<float> silence (4800);
        const float* data[] = { silence.data(), silence.data() };
        if (writer == nullptr || ! writer->write (data, 2, (std::int64_t) silence.size()) || ! writer->flush()
            || ! writeMidiImportFixture (mid))
            return ScenarioResult::fail ("could not write the import fixtures");
    }
    for (const int t : { 0, 1 })
    {
        ctx.keep (session.track (t).frozen);
        session.track (t).frozen.store (false);
        session.track (t).mode.store ((int) Track::Mode::Mono);
    }
    engine.getUndoManager().clearUndoHistory();

    const auto modes = [&session] (Track::Mode first, Track::Mode second)
    {
        return session.track (0).mode.load() == (int) first && session.track (1).mode.load() == (int) second;
    };
    const auto regions = [&session] (int audio, int midi)
    {
        return (int) session.track (0).regions.size() == audio
            && (int) session.track (1).midiRegions.current().size() == midi;
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx, wav, mid]
    { ctx.expect (host.dropFilesOnTrack (0, { wav, mid }), "the two-file drop was rejected"); } });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Auto-assign"), "the multi-file picker has no Auto-assign"); } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.multiImportRows() == std::vector<std::string> { "Wide.wav\t0", "Notes.mid\t1" },
                    "Auto-assign did not put the files on the first two tracks");
        ctx.expect (host.clickModalButton ("Import"), "the assigned files could not be imported");
    } });
    steps->push_back ({ 700, [&host, &ctx, &session, &engine, modes, regions]
    {
        ctx.expect (host.modalStackEmpty(), "the import left '" + host.modalText() + "' up");
        ctx.expect (modes (Track::Mode::Stereo, Track::Mode::Midi),
                    "the import did not flip the tracks to Stereo and MIDI as the picker said it would");
        ctx.expect (regions (1, 1), "each track did not take its file's region");
        if (! session.track (0).regions.empty())
            ctx.expect (session.track (0).regions.front().numChannels == 2, "the stereo file landed as mono");
        ctx.expect (engine.getUndoManager().getUndoDescription() == "Import file", "the last file is not its own undo step");
        ctx.expect (host.pressKey ("command + Z"), "Undo was not handled");
    } });
    steps->push_back ({ 300, [&host, &ctx, modes, regions]
    {
        ctx.expect (modes (Track::Mode::Stereo, Track::Mode::Mono) && regions (1, 0),
                    "undoing the MIDI file did not take its region off and put its track back to Mono");
        ctx.expect (host.pressKey ("command + Z"), "Undo was not handled");
    } });
    steps->push_back ({ 300, [&host, &ctx, modes, regions]
    {
        ctx.expect (modes (Track::Mode::Mono, Track::Mode::Mono) && regions (0, 0),
                    "undoing the stereo file did not take its region off and put its track back to Mono");
        ctx.expect (host.pressKey ("command + shift + Z") && host.pressKey ("command + shift + Z"), "Redo was not handled");
    } });
    steps->push_back ({ 300, [&ctx, modes, regions]
    {
        ctx.expect (modes (Track::Mode::Stereo, Track::Mode::Midi) && regions (1, 1),
                    "redo did not flip both tracks again and bring their regions back");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar multiImportSwitchesMode { Scenario {
    "gui.multi_import_switches_mode", { "gui", "import", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMultiImportSwitchesMode (host, ctx); }
} };

// With no audio device running a file is imported at the rate the timeline
// measures time at, the rate the device last ran at: an audio file is
// converted to it, a session with no rate yet takes it, and a MIDI file's
// length is worked out at it.
std::optional<ScenarioResult> runImportWithoutDevice (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    const double rate = engine.getCurrentSampleRate();
    if (host.engineDetached() || ! engine.isAudioCallbackRegistered() || rate <= 0.0)
        return ScenarioResult::skip ("requires a running audio device");
    AudioRegion region;
    if (auto early = seedTapeRegion (host, ctx, region)) return early;
    auto& session = ctx.session();
    static constexpr int audioTrack = 1;
    static constexpr int midiTrack = 2;
    for (const int t : { audioTrack, midiTrack })
    {
        auto& track = session.track (t);
        if (! track.regions.empty() || ! track.midiRegions.current().empty())
            return ScenarioResult::skip ("requires tracks 2 and 3 empty");
        ctx.keep (track.mode);
        ctx.keep (track.frozen);
        track.frozen.store (false);
    }
    session.track (audioTrack).mode.store ((int) Track::Mode::Mono);
    session.track (midiTrack).mode.store ((int) Track::Mode::Midi);
    // Away from the 48 kHz an importer would fall back to.
    const double standIn = std::abs (rate - 44100.0) < 0.5 ? 88200.0 : 44100.0;
    ctx.cleanup ([&engine, &session, rate, sessionRate = session.sessionSampleRate]
    {
        session.sessionSampleRate = sessionRate;
        engine.setLastDeviceSampleRateForScenario (rate);
        if (! engine.isAudioCallbackRegistered()) engine.reattachAudioCallback();
    });
    // A session never saved has no rate of its own until its audio gives it one.
    session.sessionSampleRate = 0.0;
    const auto wav = ctx.tempDir() / "Dropped.wav";
    const auto mid = ctx.tempDir() / "Notes.mid";
    static constexpr int frames = 4800;
    if (! writeSilenceFixture (wav, rate, frames) || ! writeMidiImportFixture (mid))
        return ScenarioResult::fail ("could not write the import fixtures");
    engine.getTransport().locate (0);

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx, &engine, standIn, wav]
    {
        engine.detachAudioCallback();
        engine.setLastDeviceSampleRateForScenario (standIn);
        ctx.expect (engine.getCurrentSampleRate() <= 0.0 && std::abs (engine.getTimelineSampleRate() - standIn) < 0.5,
                    "the detached engine does not measure the timeline at the stand-in rate");
        engine.refreshMidiInputs();
        ctx.expect (! engine.isAudioCallbackRegistered() && engine.getCurrentSampleRate() <= 0.0,
                    "MIDI refresh reattached the deliberately detached audio callback");
        ctx.expect (std::abs (engine.getTimelineSampleRate() - standIn) < 0.5,
                    "MIDI refresh changed the detached timeline's rate");
        ctx.expect (host.dropFilesOnTrack (audioTrack, { wav }), "the audio drop was rejected");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Import"), "the import target picker did not open for the audio file"); } });
    steps->push_back ({ 700, [&host, &ctx, &session, rate, standIn, mid]
    {
        const auto& regions = session.track (audioTrack).regions;
        if (ctx.expect (regions.size() == 1, "the audio file was not imported"))
        {
            const auto expected = (std::int64_t) std::llround (frames * standIn / rate);
            ctx.expect (regions.front().lengthInSamples == expected,
                        "the imported region is " + std::to_string (regions.front().lengthInSamples)
                            + " samples long, not the " + std::to_string (expected) + " it runs at the timeline's rate");
            auto reader = dusk::audio::FileReader::open (regions.front().filePath());
            ctx.expect (reader != nullptr && std::abs (reader->info().sampleRate - standIn) < 0.5,
                        "the imported file was not written at the timeline's rate");
            ctx.expect (std::abs (session.sessionSampleRate - standIn) < 0.5,
                        "the session did not take the imported audio's rate as its own");
        }
        ctx.expect (host.dropFilesOnTrack (midiTrack, { mid }), "the MIDI drop was rejected");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Import"), "the import target picker did not open for the MIDI file"); } });
    steps->push_back ({ 700, [&ctx, &session, standIn]
    {
        const auto& regions = session.track (midiTrack).midiRegions.current();
        if (! ctx.expect (regions.size() == 1, "the MIDI file was not imported")) return;
        const auto expected = ticksToSamples (regions.front().lengthInTicks, standIn,
                                              session.tempoBpm.load (std::memory_order_relaxed));
        ctx.expect (regions.front().lengthInSamples == expected,
                    "the imported MIDI region is " + std::to_string (regions.front().lengthInSamples)
                        + " samples long, not the " + std::to_string (expected) + " it runs at the timeline's rate");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar importWithoutDevice { Scenario {
    "gui.import_without_device", { "gui", "import" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runImportWithoutDevice (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
