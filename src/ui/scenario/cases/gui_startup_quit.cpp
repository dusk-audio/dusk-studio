#include "../GuiScenarioSupport.h"
#include "../../../foundation/AppMusicDir.h"
#include "../../../foundation/Fs.h"
#include "../../../session/SessionSerializer.h"
#include "../../../session/SessionTemplates.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
// For the bb-quit-save-rate leg only: the quit prompt's Save on a session that
// has never been saved, which goes through Save As with the callback already
// detached and then ends the process, so the case never reports. It saves into
// the Music folder the leg names, under a folder that carries the device's rate
// for the leg to hold the saved session to.
std::optional<ScenarioResult> runQuitSaveStampsRate (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    const double rate = engine.getCurrentSampleRate();
    if (! engine.isAudioCallbackRegistered() || rate <= 0.0)
        return ScenarioResult::skip ("requires a running audio device");
    const auto music = dusk::fs::appMusicDir();
    if (music.empty()) return ScenarioResult::skip ("requires a Music folder");
    const auto target = music / ("Quit save at " + std::to_string (std::llround (rate)));

    host.startUnsavedSessionIn (ctx.tempDir());
    session.sessionSampleRate = 0.0;
    auto& fader = session.track (0).strip.faderDb;
    fader.store (fader.load() - 3.0f);

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.requestQuit(), "the edited never-saved session did not ask before quitting"); } });
    steps->push_back ({ 400, [&host, &ctx]
    {
        if (ctx.expect (host.modalText().rfind ("Save changes before quitting?", 0) == 0,
                        "Quit showed '" + host.modalText() + "' rather than its prompt"))
            ctx.expect (host.clickModalButton ("Save"), "the quit prompt did not offer Save");
    } });
    steps->push_back ({ 700, [&host, &ctx, target]
    {
        if (! ctx.expect (host.modalText().rfind ("Save session as...", 0) == 0,
                          "the quit prompt's Save showed '" + host.modalText() + "' rather than Save As")
            || ! ctx.expect (host.engineDetached(), "the quit prompt's Save left the callback attached")
            || ! ctx.expect (host.focusFileName(), "the Save As browser has no name field"))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        typeReplacing (host, target.string());
        ctx.expect (host.clickModalButton ("Save"), "the Save As browser has no Save button");
    } });
    runSteps (ctx, steps, [] {});
    return std::nullopt;
}

const ScenarioRegistrar quitSaveStampsRate { Scenario {
    "gui.quit_save_stamps_rate", { "helper" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runQuitSaveStampsRate (host, ctx); }
} };

std::optional<ScenarioResult> runSessionSwitch (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    const auto sampleRate = engine.getCurrentSampleRate();
    if (sampleRate <= 0.0)
        return ScenarioResult::skip ("requires a positive engine sample rate");
    const auto originalDir = currentSessionDirectory (session);
    const auto restoreFile = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restoreFile))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, &engine, originalDir, restoreFile]
    {
        engine.stop();
        drainModals (host);
        reopenSavedSession (host, restoreFile);
        applySessionDirectory (session, originalDir);
        session.setTrackArmed (0, false);
    });
    const auto outgoing = ctx.tempDir() / "outgoing" / "session.json";
    const auto incoming = ctx.tempDir() / "incoming" / "session.json";
    std::filesystem::create_directories (outgoing.parent_path());
    std::filesystem::create_directories (incoming.parent_path());
    session.track (0).mode.store ((int) Track::Mode::Midi);
    session.track (0).midiInputIndex.store (engine.getVirtualKeyboardInputIndex());
    session.track (0).strip.faderDb.store (0.0f);
    if (! SessionSerializer::save (session, outgoing))
        return ScenarioResult::fail ("could not save the outgoing session");
    session.track (0).strip.faderDb.store (-18.0f);
    if (! SessionSerializer::save (session, incoming))
        return ScenarioResult::fail ("could not save the incoming session");
    auto steps = std::make_shared<std::vector<Step>>();
    for (const std::string action : { "Cancel", "Don't Save", "Save" })
    {
        steps->push_back ({ 200, [&host, &ctx, &session, &engine, outgoing]
        {
            // An autosave the heartbeat wrote into it during an earlier pass
            // would raise the recovery prompt; the saved file is what is meant.
            ctx.expect (host.openSession (outgoing), "could not open the outgoing session");
            host.answerRecovery (GuiHost::Recovery::LoadSaved);
            session.setTrackArmed (0, true);
            engine.record();
            ctx.expect (engine.getTransport().isRecording(), "the take did not start");
        } });
        steps->push_back ({ 200, [&engine]
        {
            const std::uint8_t note[] { 0x90, 64, 100 };
            engine.postVirtualKeyboardMidi (note, 3);
        } });
        steps->push_back ({ 150, [&engine]
        {
            const std::uint8_t note[] { 0x80, 64, 0 };
            engine.postVirtualKeyboardMidi (note, 3);
        } });
        // A switch from outside runs a message-loop turn after the request,
        // and each prompt button acts a turn after its press, so the steps
        // wait for the prompt, the refusal and the switch themselves.
        steps->push_back ({ 200, [&host, incoming] { host.requestSessionSwitch (incoming); } });
        steps->push_back ({ 0, [&host, &ctx, &engine, &session, outgoing, incoming]
        {
            ctx.expect (engine.getTransport().isStopped(), "the session switch did not stop recording");
            ctx.expect (currentSessionDirectory (session) == outgoing.parent_path(),
                        "the session changed before the prompt was answered");
            const auto& regions = session.track (0).midiRegions.current();
            ctx.expect (regions.size() == 1 && ! regions.front().notes.empty(),
                        "the session switch did not commit the MIDI take before prompting");
            host.requestSessionSwitch (incoming);
        },
        [&host] { return host.modalText().rfind ("Save changes before opening another session?", 0) == 0; },
        "the session switch did not ask about the unsaved take" });
        steps->push_back ({ 0, [&host, &ctx, action]
        { ctx.expect (host.clickModalButton (action), "the prompt did not offer " + action); },
        [&host] { return host.statusMessage() == "Session not switched: close the open prompt first"; },
        "a second session switch did not report the open prompt" });
        steps->push_back ({ 0, [&host, &ctx, &session, action, outgoing, incoming]
        {
            ctx.expect (host.modalStackEmpty(), action + " left the prompt open");
            ctx.expect (currentSessionDirectory (session)
                            == (action == "Cancel" ? outgoing.parent_path() : incoming.parent_path()),
                        action + " opened the wrong session");
            Session saved;
            ctx.expect (SessionSerializer::load (saved, outgoing), "could not read the outgoing session");
            const auto& regions = saved.track (0).midiRegions.current();
            ctx.expect (action == "Save" ? regions.size() == 1 && ! regions.front().notes.empty()
                                         : regions.empty(),
                        action + " persisted the wrong take state");
            if (action == "Cancel")
                ctx.expect (session.track (0).midiRegions.current().size() == 1,
                            "Cancel discarded the committed take");
            else
                ctx.expect (nearly (session.track (0).strip.faderDb.load(), -18.0f),
                            "the incoming session contents did not load");
        },
        [&host, &session, action, outgoing, incoming]
        {
            return host.modalStackEmpty()
                && currentSessionDirectory (session) == (action == "Cancel" ? outgoing.parent_path() : incoming.parent_path());
        },
        action + " did not close the prompt and settle on its session" });
    }
    steps->push_back ({ 200, [&host, &ctx, &session, outgoing, sampleRate]
    {
        MidiRegion region;
        region.lengthInSamples = static_cast<std::int64_t> (sampleRate * 600.0);
        region.lengthInTicks = 576000;
        session.track (0).midiRegions.publish (
            std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { region }));
        host.startMixdown();
        ctx.expect (host.mixdownRunning(), "the mixdown did not start");
        host.requestSessionSwitch (outgoing);
    } });
    // The refusal comes a message-loop turn after the request. A mixdown that
    // ends first cannot give it, and the checks below then say why.
    steps->push_back ({ 0, [&host, &ctx, &session, incoming]
    {
        ctx.expect (host.statusMessage() == "Session not switched: finish or cancel the mixdown first",
                    "a session switch did not report the running mixdown");
        ctx.expect (currentSessionDirectory (session) == incoming.parent_path(),
                    "the session switched during the bounce");
        ctx.expect (host.mixdownRunning(), "the refused session switch stopped the bounce");
        ctx.expect (host.clickModalButton ("Cancel"), "the mixdown did not offer Cancel");
    },
    [&host]
    {
        return host.statusMessage() == "Session not switched: finish or cancel the mixdown first"
            || ! host.mixdownRunning();
    },
    "a session switch did not report the running mixdown" });
    pushCancelledMixdownCloseSteps (host, *steps, true);
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar sessionSwitch { Scenario {
    "gui.session_switch_commits_take", { "gui", "session" }, Needs::Engine | Needs::Gui,
    {}, {}, 90000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSessionSwitch (host, ctx); }
} };

// A take, Cmd+S, then a second take quit partway through. The second take is
// not in the session until the transport stops, so a quit that decided first
// would read the session as saved and quit without asking.
std::optional<ScenarioResult> runQuitMidTake (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    if (! transport.isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    const auto sampleRate = engine.getCurrentSampleRate();
    if (sampleRate <= 0.0)
        return ScenarioResult::skip ("requires a positive engine sample rate");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    keepStage (host, ctx);
    ctx.cleanup ([&host, &session, &engine, originalDir, restore]
    {
        engine.stop();
        drainModals (host);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
        session.setTrackArmed (0, false);
    });
    const auto takes = ctx.tempDir() / "Takes" / "session.json";
    std::filesystem::create_directories (takes.parent_path());
    session.track (0).mode.store ((int) Track::Mode::Midi);
    session.track (0).midiInputIndex.store (engine.getVirtualKeyboardInputIndex());
    session.track (0).midiRegions.publish (std::make_unique<std::vector<MidiRegion>>());
    if (! SessionSerializer::save (session, takes))
        return ScenarioResult::fail ("could not save the session to record into");

    // A take recorded over an earlier one stacks on its region and pushes the
    // earlier one into that region's previous takes.
    const auto countTakes = [] (const std::vector<MidiRegion>& regions)
    {
        int count = 0;
        for (const auto& region : regions)
        {
            count += region.notes.empty() ? 0 : 1;
            for (const auto& take : region.previousTakes)
                count += take.notes.empty() ? 0 : 1;
        }
        return count;
    };
    const auto takesInMemory = [&session, countTakes] { return countTakes (session.track (0).midiRegions.current()); };
    const auto takesOnDisk = [takes, countTakes]
    {
        Session saved;
        return SessionSerializer::load (saved, takes) ? countTakes (saved.track (0).midiRegions.current()) : -1;
    };
    const auto note = [&engine] (bool on)
    {
        const std::uint8_t message[] { (std::uint8_t) (on ? 0x90 : 0x80), 64, (std::uint8_t) (on ? 100 : 0) };
        engine.postVirtualKeyboardMidi (message, 3);
    };
    const auto isQuitPrompt = [&host, &ctx] (const std::string& how)
    {
        return ctx.expect (host.modalText().rfind ("Save changes before quitting?", 0) == 0,
                           how + " showed '" + host.modalText() + "' rather than the quit prompt");
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx, &session, &engine, &transport, takes]
    {
        ctx.expect (host.openSession (takes), "could not open the session to record into");
        ctx.expect (currentSessionDirectory (session) == takes.parent_path(), "the session to record into did not open");
        session.setTrackArmed (0, true);
        engine.record();
        ctx.expect (transport.isRecording(), "the first take did not start");
    } });
    steps->push_back ({ 200, [note] { note (true); } });
    steps->push_back ({ 150, [note] { note (false); } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.pressPeerKey ("spacebar", ' '), "the window did not handle Space"); } });
    steps->push_back ({ 300, [&host, &ctx, &transport, takesInMemory]
    {
        ctx.expect (transport.isStopped(), "Space did not stop the first take");
        ctx.expect (takesInMemory() == 1, "the first take was not committed");
        ctx.expect (host.pressPeerKey (commandChord ('s'), 's'), "the window did not handle Cmd+S");
    } });
    steps->push_back ({ 400, [&host, &ctx, &engine, &transport, takesOnDisk]
    {
        ctx.expect (host.modalStackEmpty(), "Cmd+S showed '" + host.modalText() + "'");
        ctx.expect (takesOnDisk() == 1, "Cmd+S did not save the first take in place");
        engine.record();
        ctx.expect (transport.isRecording(), "the second take did not start");
    } });
    steps->push_back ({ 200, [note] { note (true); } });
    steps->push_back ({ 150, [note] { note (false); } });
    steps->push_back ({ 200, [&host, &ctx, &transport]
    {
        ctx.expect (transport.isRecording(), "the second take stopped before the quit");
        // A quit that finds nothing to ask about ends the run, and every step
        // after this one assumes the prompt is up.
        if (! ctx.expect (host.requestQuit(), "quitting mid-take in a saved session did not ask before quitting"))
            ctx.complete (ctx.verdict());
    } });
    steps->push_back ({ 400, [&host, &ctx, &transport, takesInMemory, takesOnDisk, isQuitPrompt]
    {
        if (! isQuitPrompt ("Quitting mid-take"))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        ctx.expect (transport.isStopped(), "the quit prompt came up with the take still recording");
        ctx.expect (takesInMemory() == 2, "the quit did not commit the take before asking; "
                                              + std::to_string (takesInMemory()) + " takes in memory");
        ctx.expect (takesOnDisk() == 1, "the quit saved the session before it was answered");
        ctx.expect (host.clickModalButton ("Cancel"), "the quit prompt did not offer Cancel");
    } });
    steps->push_back ({ 400, [&host, &ctx, &engine, &session, &transport, takes, takesInMemory]
    {
        ctx.expect (host.modalStackEmpty(), "Cancel left '" + host.modalText() + "' up");
        ctx.expect (engine.isAudioCallbackRegistered() && ! host.engineDetached() && host.autosaveRunning(),
                    "Cancel in the quit prompt left audio or autosave parked");
        ctx.expect (transport.isStopped() && takesInMemory() == 2
                        && currentSessionDirectory (session) == takes.parent_path(),
                    "Cancel did not leave the session stopped with the committed take");
        ctx.expect (host.pressPeerKey (commandChord ('s'), 's'), "the window did not handle Cmd+S");
    } });
    steps->push_back ({ 400, [&ctx, takesOnDisk]
    { ctx.expect (takesOnDisk() == 2, "the take the quit committed did not save"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar quitMidTake { Scenario {
    "gui.quit_mid_take_asks_to_save", { "gui", "session", "recording" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runQuitMidTake (host, ctx); }
} };

// A control surface or a MIDI binding queues its Record for the engine to pick
// up whatever modal is up, so a take can start under a save prompt or under the
// Save As browser. Save has to write that take with the session: the quit's
// Save ends it before anything is written, and a Save As has to end it before
// it copies the session's audio, or the saved session points at a file left
// behind in the never-saved session's folder.
std::optional<ScenarioResult> runPromptSaveKeepsBoundTake (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    if (! transport.isStopped() || ! host.modalStackEmpty()
        || ! engine.isAudioCallbackRegistered() || ! host.autosaveRunning() || host.engineDetached())
        return ScenarioResult::skip ("requires a stopped transport, no modal, and live audio and autosave");
    if (session.deviceCaptureChannels.load() <= 0)
        return ScenarioResult::skip ("requires a running audio device with an input to record from");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Recording);
    ctx.keep (session.pendingTransportAction);
    ctx.keep (session.countInEnabled);
    ctx.cleanup ([&host, &session, &engine, &transport, originalDir, restore,
                  loopStart = transport.getLoopStart(), loopEnd = transport.getLoopEnd(),
                  looping = transport.isLoopEnabled(), punchIn = transport.getPunchIn(),
                  punchOut = transport.getPunchOut(), punching = transport.isPunchEnabled(),
                  playhead = transport.getPlayhead()]
    {
        engine.stop();
        drainModals (host);
        engine.reattachAudioCallback();
        session.setTrackArmed (0, false);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
        transport.setLoopRange (loopStart, loopEnd);
        transport.setLoopEnabled (looping);
        transport.setPunchRange (punchIn, punchOut);
        transport.setPunchEnabled (punching);
        transport.locate (playhead);
    });

    auto& track = session.track (0);
    track.mode.store ((int) Track::Mode::Mono);
    track.inputSource.store (0);
    track.regions.clear();
    track.takes.clear();
    session.countInEnabled.store (false);
    transport.setLoopEnabled (false);
    transport.setPunchEnabled (false);
    const auto other = ctx.tempDir() / "Other" / "session.json";
    fs::create_directories (other.parent_path());
    if (! SessionSerializer::save (session, other))
        return ScenarioResult::fail ("could not save the session to switch to");
    const auto kept = ctx.tempDir() / "Kept";
    // No space in the name: typed into the browser, a space the field did not
    // take would reach the window as Space and stop the take itself.
    const auto keptAgain = ctx.tempDir() / "KeptAgain";

    auto steps = std::make_shared<std::vector<Step>>();
    // What an MCU REC button or a MIDI binding on Record does.
    const auto recordsUnder = [&ctx, &session, &transport, steps] (const std::string& where)
    {
        steps->push_back ({ 100, [&session]
        {
            session.pendingTransportAction.store ((int) PendingTransportAction::Record, std::memory_order_release);
        } });
        steps->push_back ({ 400, [&ctx, &transport, where]
        { ctx.expect (transport.isRecording(), "a bound Record under " + where + " did not start a take"); } });
    };
    const auto promptUp = [&host, &ctx] (const std::string& title, const std::string& where)
    {
        return ctx.expect (host.modalText().rfind (title, 0) == 0,
                           where + ": '" + host.modalText() + "' is up rather than " + title);
    };
    const auto saveAs = [&host, &ctx] (const std::string& how, const fs::path& target)
    {
        if (! ctx.expect (host.focusFileName(), how + ": the Save As browser has no name field")) return;
        typeReplacing (host, target.string());
        ctx.expect (host.clickModalButton ("Save"), how + ": the Save As browser has no Save button");
    };
    const auto keptTakes = [&ctx] (const std::string& how, const fs::path& dir, std::size_t takes)
    {
        Session saved;
        applySessionDirectory (saved, dir);
        if (! ctx.expect (SessionSerializer::load (saved, dir / "session.json"), how + ": the saved session does not load"))
            return;
        const auto& savedTrack = saved.track (0);
        ctx.expect (savedTrack.takes.size() == takes, how + ": the saved session holds " + std::to_string (savedTrack.takes.size())
                                                 + " takes rather than " + std::to_string (takes));
        ctx.expect (! savedTrack.regions.empty(), how + ": the saved session has no playback region for its takes");
        const auto checkFile = [&ctx, &how, &dir] (const auto& audioFile)
        {
            const auto file = fs::u8path (audioFile.getFullPathName().toStdString());
            std::error_code ignored;
            ctx.expect (file.parent_path() == dir / "audio" && fs::is_regular_file (file, ignored),
                        how + ": the saved session's audio is at '" + file.string() + "', outside its folder");
        };
        for (const auto& take : savedTrack.takes) checkFile (take.file);
        for (const auto& region : savedTrack.regions) checkFile (region.file);
        ctx.expect (saved.missingAudioFilesAfterLoad.empty(), how + ": the saved session misses audio");
    };

    steps->push_back ({ 200, [&host, &ctx, &session, &transport]
    {
        host.startUnsavedSessionIn (ctx.tempDir());
        ctx.expect (! host.sessionOnDisk(), "the new session reads as saved");
        transport.setPlayhead (0);
        session.setTrackArmed (0, true);
        ctx.expect (session.track (0).recordArmed.load(), "track 1 would not arm on input 1");
        session.track (0).strip.faderDb.store (-7.0f);
        ctx.expect (host.requestQuit(), "the edited never-saved session did not ask before quitting");
    } });
    steps->push_back ({ 400, [promptUp] { promptUp ("Save changes before quitting?", "Quit"); } });
    recordsUnder ("the quit prompt");
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Save"), "the quit prompt did not offer Save"); } });
    steps->push_back ({ 700, [&host, &ctx, &session, &transport, promptUp]
    {
        if (! promptUp ("Save session as...", "Save in the quit prompt")) return;
        ctx.expect (transport.isStopped() && session.track (0).takes.size() == 1,
                    "Save in the quit prompt did not end and keep the take before saving");
        ctx.expect (host.clickModalButton ("Cancel"), "the Save As browser has no Cancel");
    } });
    steps->push_back ({ 500, [&host, &ctx, &engine, other]
    {
        ctx.expect (host.modalStackEmpty(), "Cancel left '" + host.modalText() + "' up");
        ctx.expect (engine.isAudioCallbackRegistered() && ! host.engineDetached() && host.autosaveRunning(),
                    "cancelling the quit's Save As left audio or autosave parked");
        host.requestSessionSwitch (other);
    } });
    steps->push_back ({ 400, [promptUp]
    { promptUp ("Save changes before opening another session?", "The session switch"); } });
    recordsUnder ("the session-switch prompt");
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Save"), "the session-switch prompt did not offer Save"); } });
    steps->push_back ({ 700, [promptUp, saveAs, kept]
    {
        if (promptUp ("Save session as...", "Save in the session-switch prompt"))
            saveAs ("Save in the session-switch prompt", kept);
    } });
    steps->push_back ({ 1000, [&host, &ctx, &session, other, kept, keptTakes]
    {
        ctx.expect (host.modalStackEmpty(), "the switch after Save showed '" + host.modalText() + "'");
        ctx.expect (currentSessionDirectory (session) == other.parent_path(),
                    "the switch did not go on after Save; the session is in '"
                        + currentSessionDirectory (session).string() + "'");
        keptTakes ("Save in the session-switch prompt", kept, 2);

        host.startUnsavedSessionIn (ctx.tempDir());
        session.setTrackArmed (0, true);
        ctx.expect (host.clickFileMenu(), "the File menu is unavailable");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Save as..."), "the File menu has no Save as..."); } });
    steps->push_back ({ 700, [promptUp] { promptUp ("Save session as...", "File > Save as..."); } });
    recordsUnder ("the Save As browser");
    steps->push_back ({ 200, [saveAs, keptAgain] { saveAs ("Save As", keptAgain); } });
    steps->push_back ({ 1000, [&host, &ctx, &session, &transport, keptAgain, keptTakes]
    {
        ctx.expect (host.modalStackEmpty(), "Save As showed '" + host.modalText() + "'");
        ctx.expect (transport.isStopped(), "Save As left the take recording");
        ctx.expect (currentSessionDirectory (session) == keptAgain, "Save As did not move the session");
        keptTakes ("Save As", keptAgain, 1);
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar promptSaveKeepsBoundTake { Scenario {
    "gui.prompt_save_keeps_bound_take", { "gui", "session", "recording" }, Needs::Engine | Needs::Gui,
    {}, {}, 25000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPromptSaveKeepsBoundTake (host, ctx); }
} };

// An alert that is up when a session loads stays above the console the load
// rebuilds, as the notice of a missing audio device is at startup, so it shows and
// a click outside it is not taken by a dim hidden under the console.
std::optional<ScenarioResult> runModalAboveSessionLoad (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    if (! ctx.engine().getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto folder = ctx.tempDir() / "load";
    std::filesystem::create_directories (folder);
    const auto saved = folder / "session.json";
    if (! SessionSerializer::save (session, saved)) return ScenarioResult::fail ("could not save session");
    ctx.cleanup ([&host, &session, originalDir]
    {
        drainModals (host);
        applySessionDirectory (session, originalDir);
    });
    host.raiseAlert ("Audio device unavailable", "No audio device could be opened.");

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 150, [&host, &ctx, saved]
    {
        ctx.expect (host.modalBodyOnTop(), "the alert was not on top before the load");
        ctx.expect (host.openSession (saved), "the session did not load");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.modalCount() == 1, "the session load closed the alert");
        ctx.expect (host.modalBodyOnTop(), "the session load left the alert under the console");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar modalAboveSessionLoad { Scenario {
    "gui.modal_above_session_load", { "gui", "session" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runModalAboveSessionLoad (host, ctx); }
} };

const ScenarioRegistrar firstLaunch { Scenario {
    "gui.first_launch", { "gui", "startup" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
        for (const auto& error : host.firstLaunchErrors())
            ctx.expect (false, error);
        return ctx.verdict();
    }
} };

std::optional<ScenarioResult> runStartupClicks (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native UI");
   #else
    auto& session = ctx.session();
    if (! ctx.engine().getTransport().isStopped() || ! host.modalStackEmpty() || host.startupDialogOpen())
        return ScenarioResult::skip ("requires a stopped transport and no dialog");
    const auto fixture = ctx.tempDir() / "fixture";
    std::filesystem::create_directories (fixture);
    const auto restore = fixture / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    const auto originalDir = currentSessionDirectory (session);
    ctx.cleanup ([&host, &session, restore, originalDir]
    {
        host.closeStartupDialog();
        drainModals (host);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });
    // Clean, so opening a recent row loads it without the unsaved-changes prompt.
    const auto clean = ctx.tempDir() / "clean" / "session.json";
    std::filesystem::create_directories (clean.parent_path());
    if (! SessionSerializer::save (session, clean)) return ScenarioResult::fail ("could not save the clean session");
    reopenSavedSession (host, clean);

    // More rows than the table shows, so the wheel has somewhere to go.
    std::vector<std::filesystem::path> recents;
    for (int i = 0; i < 20; ++i)
    {
        recents.push_back (ctx.tempDir() / "recents" / ("Recent " + std::to_string (100 + i).substr (1)));
        std::filesystem::create_directories (recents.back());
    }
    if (! SessionSerializer::save (session, recents[1] / "session.json"))
        return ScenarioResult::fail ("could not save the recent session");
    const auto recentChoice = [recents] (int index) { return "recent:" + recents[(std::size_t) index].u8string(); };
    // A dialog a failed step left up is closed first, so each later control reports
    // its own verdict.
    const auto reopen = [&host, &ctx, recents] (bool runChoice)
    {
        host.closeStartupDialog();
        ctx.expect (host.openStartupDialog (recents, runChoice), "the startup dialog did not open");
    };
    const auto click = [&host, &ctx] (const std::string& control)
    { ctx.expect (host.clickStartupControl (control), "the startup dialog did not show " + control); };
    const auto chosen = [&host, &ctx] (const std::string& expected, const std::string& what)
    {
        ctx.expect (host.startupChoice() == expected, what + " chose '" + host.startupChoice() + "'");
        ctx.expect (! host.startupDialogOpen(), what + " left the startup dialog open");
    };

    reopen (false);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 400, [&host, &ctx, click]
    {
        ctx.expect (host.startupSelectedRow() == 0, "the newest recent session was not selected on open");
        click ("row:1");
    } });
    steps->push_back ({ 300, [&host, &ctx, click]
    {
        ctx.expect (host.startupSelectedRow() == 1, "clicking the second recent row did not select it");
        ctx.expect (host.startupDialogOpen() && host.startupChoice().empty(),
                    "a single click on a recent row dismissed the dialog");
        click ("open");
    } });
    steps->push_back ({ 300, [reopen, chosen, recentChoice]
    {
        chosen (recentChoice (1), "Open after selecting the second row");
        reopen (false);
    } });
    steps->push_back ({ 400, [click] { click ("row:2"); } });
    steps->push_back ({ 100, [click] { click ("row:2"); } });
    steps->push_back ({ 300, [reopen, chosen, recentChoice]
    {
        chosen (recentChoice (2), "double-clicking the third row");
        reopen (false);
    } });
    steps->push_back ({ 400, [click] { click ("tab-new"); } });
    steps->push_back ({ 300, [click] { click ("template:2"); } });
    steps->push_back ({ 300, [reopen, chosen]
    {
        chosen ("new:2", "the third template under NEW");
        reopen (false);
    } });
    // A trackpad's quarter notches, one per frame, add up to one row.
    for (int i = 0; i < 4; ++i)
        steps->push_back ({ i == 0 ? 400 : 100, [&host, &ctx]
        { ctx.expect (host.scrollStartupList (-0.25f), "the startup dialog did not take the wheel"); } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (! host.clickStartupControl ("row:0") && host.clickStartupControl ("row:1"),
                    "four quarter wheel notches did not scroll the recent list by one row");
    } });
    steps->push_back ({ 300, [click] { click ("tab-new"); } });
    steps->push_back ({ 300, [click] { click ("tab-recent"); } });
    steps->push_back ({ 300, [click] { click ("scroll-down"); } });
    steps->push_back ({ 300, [&host, &ctx, click]
    {
        ctx.expect (! host.clickStartupControl ("row:0"), "the wheel did not scroll the recent list");
        click ("row:15");
    } });
    steps->push_back ({ 300, [&host, &ctx, click]
    {
        ctx.expect (host.startupSelectedRow() == 15, "clicking a row scrolled into view did not select it");
        click ("tab-open");
    } });
    steps->push_back ({ 300, [reopen, chosen]
    {
        chosen ("open-file", "the OPEN tab");
        reopen (false);
    } });
    steps->push_back ({ 400, [click] { click ("quit"); } });
    steps->push_back ({ 300, [reopen, chosen]
    {
        chosen ("quit", "Quit");
        reopen (true);
    } });
    steps->push_back ({ 400, [click] { click ("row:1"); } });
    steps->push_back ({ 100, [click] { click ("row:1"); } });
    steps->push_back ({ 1000, [&ctx, &session, chosen, recentChoice, recents]
    {
        chosen (recentChoice (1), "double-clicking the second row to open it");
        std::error_code error;
        ctx.expect (std::filesystem::equivalent (currentSessionDirectory (session), recents[1], error),
                    "double-clicking a recent row did not open that session");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar startupClicks { Scenario {
    "gui.startup_recent_clicks", { "gui", "startup" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runStartupClicks (host, ctx); }
} };

std::optional<ScenarioResult> runStartupCancel (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native UI");
   #else
    auto& session = ctx.session();
    if (! ctx.engine().getTransport().isStopped() || ! host.modalStackEmpty() || host.startupDialogOpen())
        return ScenarioResult::skip ("requires a stopped transport and no dialog");
    const auto fixture = ctx.tempDir() / "fixture";
    std::filesystem::create_directories (fixture);
    const auto restore = fixture / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    const auto originalDir = currentSessionDirectory (session);
    ctx.cleanup ([&host, &session, restore, originalDir]
    {
        host.closeStartupDialog();
        drainModals (host);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });

    // Clean, so a pick goes straight to the browser; a later pass dirties it to
    // reach the unsaved-changes prompt instead.
    const auto clean = ctx.tempDir() / "clean" / "session.json";
    std::filesystem::create_directories (clean.parent_path());
    if (! SessionSerializer::save (session, clean)) return ScenarioResult::fail ("could not save the clean session");
    reopenSavedSession (host, clean);
    const auto cleanDir = currentSessionDirectory (session);

    const auto click = [&host, &ctx] (const std::string& control)
    { ctx.expect (host.clickStartupControl (control), "the startup dialog did not show " + control); };
    const auto shown = [&host, &ctx] (const std::string& title, const std::string& what)
    {
        ctx.expect (! host.startupDialogOpen(), what + " left the startup dialog up");
        ctx.expect (host.modalText().rfind (title, 0) == 0,
                    what + " showed '" + host.modalText() + "' rather than " + title);
    };
    const auto cancel = [&host, &ctx, shown] (const std::string& title, const std::string& what)
    {
        shown (title, what);
        ctx.expect (host.clickModalButton ("Cancel"), title + " did not offer Cancel");
    };
    const auto back = [&host, &ctx, &session, cleanDir] (const std::string& what)
    {
        ctx.expect (host.modalStackEmpty(), what + " left a modal open");
        ctx.expect (host.startupDialogOpen(), what + " did not bring the startup dialog back");
        ctx.expect (currentSessionDirectory (session) == cleanDir, what + " switched the session");
    };
    const auto browse = [&host, &ctx] (const std::filesystem::path& path, const std::string& accept)
    {
        if (! ctx.expect (host.focusFileName(), "the file browser has no name field")) return;
       #if defined (__APPLE__)
        host.pressPeerKey ("command + A", 'a');
       #else
        host.pressPeerKey ("ctrl + A", 'a');
       #endif
        for (const char ch : path.string())
            host.pressPeerKey (ch == ' ' ? "Space" : std::string (1, ch), ch);
        ctx.expect (host.clickModalButton (accept), "the file browser has no " + accept + " button");
    };
    const auto failed = [&host, &ctx, back] (const std::string& status, const std::string& what)
    {
        back (what);
        ctx.expect (host.statusMessage().rfind (status, 0) == 0,
                    what + " left the status '" + host.statusMessage() + "' rather than " + status);
    };
    const auto taken = ctx.tempDir() / "taken";
    std::filesystem::create_directories (taken);
    std::ofstream (taken / "session.json") << "{}";
    const auto corrupt = ctx.tempDir() / "corrupt" / "session.json";
    std::filesystem::create_directories (corrupt.parent_path());
    std::ofstream (corrupt) << "not a session";
    const auto future = ctx.tempDir() / "future" / "session.json";
    std::filesystem::create_directories (future.parent_path());
    std::ofstream (future) << R"({"version":9999,"tracks":[{"name":"From the future"}]})";
    // Recent rows: a folder with no session.json, and a session whose autosave
    // differs from it, which raises the recovery prompt.
    const auto noSession = ctx.tempDir() / "no-session";
    std::filesystem::create_directories (noSession);
    const auto recovering = ctx.tempDir() / "recovering";
    std::filesystem::create_directories (recovering);
    auto& fader = session.track (0).strip.faderDb;
    const float faderAtStart = fader.load();
    fader.store (-11.0f);
    const bool autosaved = SessionSerializer::save (session, recovering / "session.json.autosave");
    fader.store (faderAtStart);
    if (! autosaved || ! SessionSerializer::save (session, recovering / "session.json"))
        return ScenarioResult::fail ("could not write the recovery fixture");
    const std::vector<std::filesystem::path> recents { noSession, recovering };
    const auto reopen = [&host, &ctx, recents]
    {
        host.closeStartupDialog();
        ctx.expect (host.openStartupDialog (recents, true), "the startup dialog did not reopen");
    };

    host.closeStartupDialog();
    ctx.expect (host.openStartupDialog ({}, true), "the startup dialog did not open");
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 400, [click] { click ("tab-open"); } });
    steps->push_back ({ 600, [cancel] { cancel ("Open session.json", "OPEN"); } });
    steps->push_back ({ 700, [&host, &ctx, back, click]
    {
        ctx.expect (host.startupChoice() == "open-file", "OPEN chose '" + host.startupChoice() + "'");
        back ("Cancel in the browser after OPEN");
        click ("tab-new");
    } });
    steps->push_back ({ 400, [click] { click ("template:1"); } });
    steps->push_back ({ 600, [cancel] { cancel ("Name your new session", "a NEW template"); } });
    steps->push_back ({ 700, [back, click]
    {
        back ("Cancel in the browser after a NEW template");
        click ("tab-open");
    } });
    steps->push_back ({ 600, [&host, &ctx, shown]
    {
        shown ("Open session.json", "OPEN from the returned dialog");
        ctx.expect (host.pressPeerKey ("escape"), "the browser did not handle Escape");
    } });
    steps->push_back ({ 700, [back, click]
    {
        back ("Escape in the browser");
        click ("tab-open");
    } });
    steps->push_back ({ 600, [&host, &ctx, shown]
    {
        shown ("Open session.json", "OPEN after Escape");
        ctx.expect (host.clickModalBackdrop(), "the browser left no backdrop to click");
    } });
    // The same Cancels from the File menu leave the DAW as it is.
    const auto stays = [&host, &ctx] (const std::string& what)
    {
        ctx.expect (host.modalStackEmpty(), what + " left a modal open");
        ctx.expect (! host.startupDialogOpen(), what + " brought the startup dialog up");
    };
    steps->push_back ({ 700, [back, click]
    {
        back ("a click outside the browser");
        click ("tab-new");
    } });
    steps->push_back ({ 400, [click] { click ("template:1"); } });
    steps->push_back ({ 600, [shown, browse, taken]
    {
        shown ("Name your new session", "a NEW template over an existing session");
        browse (taken, "Save");
    } });
    steps->push_back ({ 700, [failed, click]
    {
        failed ("A session already exists at", "a NEW over an existing session");
        click ("tab-open");
    } });
    steps->push_back ({ 600, [shown, browse, corrupt]
    {
        shown ("Open session.json", "OPEN of an unreadable session");
        browse (corrupt, "Open");
    } });
    steps->push_back ({ 700, [failed, click]
    {
        failed ("Load failed", "an OPEN of an unreadable session");
        click ("tab-open");
    } });
    steps->push_back ({ 600, [shown, browse, future]
    {
        shown ("Open session.json", "OPEN of a newer session format");
        browse (future, "Open");
    } });
    steps->push_back ({ 700, [&host, &ctx, failed]
    {
        failed ("Session format 9999 is newer than supported format 11. Upgrade Dusk Studio to open",
                "an OPEN of a newer session format");
        host.closeStartupDialog();
        ctx.expect (host.clickFileMenu(), "the File menu is unavailable");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("New session..."), "the File menu has no New session..."); } });
    steps->push_back ({ 600, [cancel] { cancel ("Name your new session", "File > New"); } });
    steps->push_back ({ 700, [stays, reopen]
    {
        stays ("Cancel in the browser after File > New");
        reopen();
    } });
    const auto openRow = [&steps, click] (const std::string& row)
    {
        steps->push_back ({ 400, [click, row] { click (row); } });
        steps->push_back ({ 100, [click, row] { click (row); } });
    };
    openRow ("row:0");
    steps->push_back ({ 700, [&host, &ctx, failed, reopen, noSession]
    {
        ctx.expect (host.startupChoice() == "recent:" + noSession.u8string(),
                    "the first recent row chose '" + host.startupChoice() + "'");
        failed ("No session at", "a recent session with no session.json");
        reopen();
    } });
    openRow ("row:1");
    steps->push_back ({ 700, [cancel]
    { cancel ("Recover from autosave?", "a recent session with a newer autosave"); } });
    steps->push_back ({ 700, [back, reopen]
    {
        back ("Cancel in the recovery prompt after a recent pick");
        reopen();
    } });
    openRow ("row:1");
    steps->push_back ({ 700, [&host, &ctx, shown]
    {
        shown ("Recover from autosave?", "the recent session with a newer autosave, again");
        ctx.expect (host.pressPeerKey ("escape"), "the recovery prompt did not handle Escape");
    } });
    steps->push_back ({ 700, [&host, &ctx, &session, back]
    {
        back ("Escape in the recovery prompt");
        host.closeStartupDialog();
        ctx.expect (host.openStartupDialog ({}, true), "the startup dialog did not reopen");
        session.track (0).strip.faderDb.store (-7.0f);
    } });
    steps->push_back ({ 400, [click] { click ("tab-open"); } });
    steps->push_back ({ 600, [cancel]
    { cancel ("Save changes before opening another session?", "OPEN over unsaved changes"); } });
    steps->push_back ({ 700, [back, click]
    {
        back ("Cancel in the unsaved-changes prompt after OPEN");
        click ("tab-new");
    } });
    steps->push_back ({ 400, [click] { click ("template:1"); } });
    steps->push_back ({ 600, [cancel]
    { cancel ("Save changes before starting a new session?", "a NEW template over unsaved changes"); } });
    steps->push_back ({ 700, [back, click]
    {
        back ("Cancel in the unsaved-changes prompt after a NEW template");
        click ("tab-open");
    } });
    // With no session.json to save into, Save asks where to save, and cancelling
    // that is a save that did not complete.
    steps->push_back ({ 600, [&host, &ctx, shown, cleanDir]
    {
        shown ("Save changes before opening another session?", "OPEN over unsaved changes, again");
        std::error_code error;
        std::filesystem::remove (cleanDir / "session.json", error);
        ctx.expect (host.clickModalButton ("Save"), "the unsaved-changes prompt did not offer Save");
    } });
    steps->push_back ({ 700, [cancel] { cancel ("Save session as...", "Save with no session.json"); } });
    steps->push_back ({ 700, [&host, &ctx, back]
    {
        back ("Cancel in Save As from the unsaved-changes prompt");
        host.closeStartupDialog();
        ctx.expect (host.clickFileMenu(), "the File menu is unavailable");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Open..."), "the File menu has no Open..."); } });
    steps->push_back ({ 600, [cancel]
    { cancel ("Save changes before opening another session?", "File > Open over unsaved changes"); } });
    steps->push_back ({ 700, [stays] { stays ("Cancel in the unsaved-changes prompt after File > Open"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar startupCancel { Scenario {
    "gui.startup_cancel_returns", { "gui", "startup" }, Needs::Engine | Needs::Gui,
    {}, {}, 45000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runStartupCancel (host, ctx); }
} };

// Save in the quit prompt parks the audio callback and the autosave heartbeat
// for the shutdown. With no session.json that Save is a Save As, and backing out
// of it abandons the quit, so each way out has to hand both back.
std::optional<ScenarioResult> runQuitSaveCancel (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty()
        || ! engine.isAudioCallbackRegistered() || ! host.autosaveRunning() || host.engineDetached())
        return ScenarioResult::skip ("requires a stopped transport, no modal, and live audio and autosave");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    ctx.cleanup ([&host, &engine, &session, originalDir, restore]
    {
        drainModals (host);
        engine.reattachAudioCallback();
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
    });

    const auto untitled = ctx.tempDir() / "Untitled";
    std::filesystem::create_directories (untitled);
    const auto sessionJson = untitled / "session.json";
    applySessionDirectory (session, untitled);
    auto& fader = session.track (0).strip.faderDb;
    const float dirtied = fader.load() - 3.0f;
    fader.store (dirtied);

    // Save is clicked only on the quit prompt of a session with no session.json:
    // anywhere else it could complete, and a completed Save ends the process.
    const auto save = [&host, &ctx, sessionJson] (const std::string& how)
    {
        if (! ctx.expect (host.modalText().rfind ("Save changes before quitting?", 0) == 0,
                          how + ": Quit showed '" + host.modalText() + "' rather than its prompt")
            || ! ctx.expect (! std::filesystem::exists (sessionJson), how + ": the session gained a session.json"))
            return;
        ctx.expect (host.clickModalButton ("Save"), how + ": the quit prompt did not offer Save");
    };
    const auto dismiss = [&host, &ctx] (const std::string& how)
    {
        if (! ctx.expect (host.modalText().rfind ("Save session as...", 0) == 0,
                          how + ": Save showed '" + host.modalText() + "' rather than Save As"))
            return;
        const bool reached = how == "Cancel" ? host.clickModalButton ("Cancel")
                           : how == "Escape" ? host.pressPeerKey ("escape")
                                             : host.clickModalBackdrop();
        ctx.expect (reached, how + " did not reach the Save As browser");
    };
    const auto running = [&host, &ctx, &engine, &session, &fader, untitled, sessionJson, dirtied]
                         (const std::string& how)
    {
        const auto what = how + " in the Save As from the quit prompt";
        ctx.expect (host.modalStackEmpty(), what + " left a modal open");
        ctx.expect (engine.isAudioCallbackRegistered(), what + " left the audio callback detached");
        ctx.expect (! host.engineDetached(), what + " left the engine latched as detached");
        ctx.expect (host.autosaveRunning(), what + " left the autosave timer stopped");
        ctx.expect (currentSessionDirectory (session) == untitled && nearly (fader.load(), dirtied)
                        && ! std::filesystem::exists (sessionJson),
                    what + " did not leave the session as it was");
    };

    // Each round quits again from where the last one left off.
    auto steps = std::make_shared<std::vector<Step>>();
    for (const std::string how : { "Cancel", "Escape", "a click outside" })
    {
        steps->push_back ({ 200, [&host, &ctx, how]
        { ctx.expect (host.requestQuit(), how + ": the unsaved session did not ask before quitting"); } });
        steps->push_back ({ 400, [save, how] { save (how); } });
        steps->push_back ({ 600, [dismiss, how] { dismiss (how); } });
        steps->push_back ({ 600, [running, how] { running (how); } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar quitSaveCancel { Scenario {
    "gui.quit_save_cancel_keeps_running", { "gui", "session" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runQuitSaveCancel (host, ctx); }
} };

// A pick in the startup dialog over unsaved changes waits on the Save As its
// unsaved-changes prompt's Save opens. Cmd+Q there, then Save in the quit's
// prompt, opens the quit's Save As in that browser's place and cancels it. The
// startup dialog stays away while the quit's browser is up, and comes back once
// Cancel there has given the audio back.
std::optional<ScenarioResult> runStartupSaveReplaced (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native UI");
   #else
    auto& session = ctx.session();
    if (! ctx.engine().getTransport().isStopped() || ! host.modalStackEmpty() || host.startupDialogOpen())
        return ScenarioResult::skip ("requires a stopped transport and no dialog");
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    const auto originalDir = currentSessionDirectory (session);
    ctx.cleanup ([&host, &session, restore, originalDir]
    {
        host.closeStartupDialog();
        drainModals (host);
        host.abandonPendingFlows();
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });
    const auto untitled = ctx.tempDir() / "Untitled";
    std::filesystem::create_directories (untitled);
    applySessionDirectory (session, untitled);
    session.track (0).strip.faderDb.store (-7.0f);

    host.closeStartupDialog();
    ctx.expect (host.openStartupDialog ({}, true), "the startup dialog did not open");
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 400, [&host, &ctx]
    { ctx.expect (host.clickStartupControl ("tab-open"), "the startup dialog did not show its Open tab"); } });
    steps->push_back ({ 600, [&host, &ctx]
    {
        ctx.expect (host.modalText().rfind ("Save changes before opening another session?", 0) == 0,
                    "OPEN over unsaved changes showed '" + host.modalText() + "'");
        ctx.expect (host.clickModalButton ("Save"), "the unsaved-changes prompt did not offer Save");
    } });
    steps->push_back ({ 700, [&host, &ctx]
    {
        if (! ctx.expect (host.modalText().rfind ("Save session as...", 0) == 0,
                          "Save in the unsaved-changes prompt showed '" + host.modalText() + "' rather than Save As"))
            return;
        ctx.expect (host.pressPeerKey ("command + Q"), "the switch's Save As did not pass Cmd+Q on");
    } });
    steps->push_back ({ 400, [&host, &ctx]
    {
        if (! ctx.expect (host.modalText().rfind ("Save changes before quitting?", 0) == 0,
                          "Cmd+Q in the switch's Save As showed '" + host.modalText() + "' rather than the quit prompt"))
            return;
        ctx.expect (host.clickModalButton ("Save"), "the quit prompt did not offer Save");
    } });
    steps->push_back ({ 700, [&host, &ctx]
    {
        ctx.expect (host.modalText().rfind ("Save session as...", 0) == 0 && host.modalCount() == 1,
                    "the quit's Save showed '" + host.modalText() + "' rather than its own Save As alone");
        ctx.expect (! host.startupDialogOpen(), "the startup dialog came back over the quit's Save As");
        ctx.expect (host.engineDetached(), "the quit's Save As is not holding the audio");
        ctx.expect (host.clickModalButton ("Cancel"), "the quit's Save As has no Cancel");
    } });
    steps->push_back ({ 700, [&host, &ctx, &session, untitled]
    {
        ctx.expect (host.modalStackEmpty(), "Cancel in the quit's Save As left '" + host.modalText() + "' up");
        ctx.expect (! host.engineDetached() && ctx.engine().isAudioCallbackRegistered() && host.autosaveRunning(),
                    "Cancel in the quit's Save As did not give the audio and the autosave back");
        ctx.expect (host.startupDialogOpen(), "the startup dialog did not come back after the quit was cancelled");
        ctx.expect (currentSessionDirectory (session) == untitled, "the cancelled switch and quit switched the session");
        host.closeStartupDialog();
        drainModals (host);
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar startupSaveReplaced { Scenario {
    "gui.startup_save_replaced", { "gui", "startup", "session" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runStartupSaveReplaced (host, ctx); }
} };

// NEW, a template, a fresh name typed into the browser and Save: the folder
// gains a session.json holding that template's tracks, and the window opens it.
std::optional<ScenarioResult> runStartupNewCreates (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native UI");
   #else
    auto& session = ctx.session();
    if (! ctx.engine().getTransport().isStopped() || ! host.modalStackEmpty() || host.startupDialogOpen())
        return ScenarioResult::skip ("requires a stopped transport and no dialog");
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    const auto originalDir = currentSessionDirectory (session);
    ctx.cleanup ([&host, &session, restore, originalDir]
    {
        host.closeStartupDialog();
        drainModals (host);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });
    // Clean, so the pick goes straight to the naming prompt.
    const auto clean = ctx.tempDir() / "clean" / "session.json";
    std::filesystem::create_directories (clean.parent_path());
    if (! SessionSerializer::save (session, clean)) return ScenarioResult::fail ("could not save the clean session");
    reopenSavedSession (host, clean);

    static constexpr auto kTemplate = SessionTemplate::SingerSongwriter;
    const auto templateIndex = std::to_string ((int) kTemplate);
    std::shared_ptr<const Session> expected = []
    {
        auto stamped = std::make_shared<Session>();
        applyTemplate (*stamped, kTemplate);
        return stamped;
    }();
    const auto created = ctx.tempDir() / "Fresh Song";
    const auto sessionJson = created / "session.json";
    const auto matches = [&ctx, expected] (const Session& got, const std::string& what)
    {
        for (int i = 0; i < Session::kNumTracks; ++i)
        {
            const auto& want = expected->track (i);
            const auto& track = got.track (i);
            if (! ctx.expect (track.name == want.name && track.colour == want.colour
                                  && track.mode.load() == want.mode.load(),
                              what + ": track " + std::to_string (i + 1) + " is '" + track.name.toStdString()
                                  + "' in mode " + std::to_string (track.mode.load()) + ", the template's is '"
                                  + want.name.toStdString() + "' in mode " + std::to_string (want.mode.load())))
                return;
        }
    };

    host.closeStartupDialog();
    if (! host.openStartupDialog ({}, true)) return ScenarioResult::fail ("the startup dialog did not open");
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 400, [&host, &ctx]
    { ctx.expect (host.clickStartupControl ("tab-new"), "the startup dialog did not show its NEW tab"); } });
    steps->push_back ({ 300, [&host, &ctx, templateIndex]
    {
        ctx.expect (host.clickStartupControl ("template:" + templateIndex),
                    "NEW did not list Singer-Songwriter as template " + templateIndex);
    } });
    steps->push_back ({ 700, [&host, &ctx, created]
    {
        if (! ctx.expect (host.modalText().rfind ("Name your new session", 0) == 0,
                          "Singer-Songwriter showed '" + host.modalText() + "' rather than the naming prompt")
            || ! ctx.expect (host.focusFileName(), "the naming prompt has no name field"))
            return;
        typeReplacing (host, created.string());
        ctx.expect (host.clickModalButton ("Save"), "the naming prompt has no Save button");
    } });
    steps->push_back ({ 1500, [&host, &ctx, &session, matches, templateIndex, created, sessionJson]
    {
        ctx.expect (host.startupChoice() == "new:" + templateIndex,
                    "the startup dialog recorded '" + host.startupChoice() + "' for the pick");
        ctx.expect (! host.startupDialogOpen(),
                    "the startup dialog came back rather than the new session opening; status '"
                        + host.statusMessage() + "'");
        ctx.expect (host.modalStackEmpty(), "creating the session left '" + host.modalText() + "' up");
        ctx.expect (currentSessionDirectory (session) == created,
                    "the window has '" + currentSessionDirectory (session).string() + "' open, not the new session");
        ctx.expect (host.statusMessage() == "New session: Fresh Song",
                    "the status bar says '" + host.statusMessage() + "'");
        auto written = std::make_unique<Session>();
        if (ctx.expect (SessionSerializer::load (*written, sessionJson),
                        "the new folder holds no loadable session.json"))
            matches (*written, "the new session.json");
        matches (session, "the opened session");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar startupNewCreates { Scenario {
    "gui.startup_new_creates_session", { "gui", "startup" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runStartupNewCreates (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
