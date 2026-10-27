#include "../GuiScenarioSupport.h"
#include "../../../foundation/AppConfigDir.h"
#include "../../../foundation/Fs.h"
#include "../../../engine/audiofile/FileWriter.h"
#include "../../../engine/audiofile/FileReader.h"
#include "../../../engine/scenario/cases/OopStubHarness.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../dsp/AuxLaneStrip.h"
#include "../../../session/RegionEditActions.h"
#include "../../../session/SessionSerializer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace duskstudio::scenario::gui
{
namespace
{
// Save holds the audio thread out while it reads the plug-ins' state, then lets
// it go. On a mixer stage the transport rolls on through the save, the playhead
// keeps moving and an insert keeps processing, and the engine never leaves the
// device. The Utility on track 1 takes the tone 24 dB down, so the strip meter
// tells an insert still at work from a dry strip, and the meter is cleared once
// the save returns, so only a block the mixer ran afterwards can refill it.
std::optional<ScenarioResult> runSaveKeepsMixerRunning (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    if (! transport.isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    const double rate = engine.getCurrentSampleRate();
    if (! engine.isAudioCallbackRegistered() || rate <= 0.0)
        return ScenarioResult::skip ("requires a running audio device");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    keepStage (host, ctx);
    ctx.keep (session.master().mute);
    ctx.cleanup ([&host, &engine, &session, originalDir, restore]
    {
        engine.observeAudioRegistrationForScenario ({});
        engine.stop();
        drainModals (host);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });

    const auto frames = static_cast<std::int64_t> (rate * 10.0);
    const auto folder = ctx.tempDir() / "Mixer session";
    const auto tone = folder / "audio" / "tone.wav";
    fs::create_directories (tone.parent_path());
    {
        std::vector<float> samples ((size_t) frames);
        for (std::int64_t i = 0; i < frames; ++i)
            samples[(size_t) i] = 0.5f * static_cast<float> (std::sin (6.283185307179586 * 440.0 * static_cast<double> (i) / rate));
        dusk::audio::WriteSpec spec;
        spec.sampleRate = rate;
        spec.numChannels = 1;
        spec.bitsPerSample = 32;
        auto writer = dusk::audio::FileWriter::create (tone, spec);
        const float* channels[] = { samples.data() };
        if (writer == nullptr || ! writer->write (channels, 1, frames) || ! writer->flush())
            return ScenarioResult::fail ("could not write the tone");
    }
    {
        auto saved = std::make_unique<Session>();
        applySessionDirectory (*saved, folder);
        AudioRegion region;
        using File = std::decay_t<decltype (region.file)>;
        region.file = File (tone.u8string().c_str());
        region.lengthInSamples = frames;
        region.numChannels = 1;
        saved->track (0).regions = { region };
        saved->track (0).mode.store ((int) Track::Mode::Mono);
        if (! SessionSerializer::save (*saved, folder / "session.json"))
            return ScenarioResult::fail ("could not write the session");
    }
    reopenSavedSession (host, folder / "session.json");
    auto& track = session.track (0);
    if (currentSessionDirectory (session) != folder || track.regions.empty())
        return ScenarioResult::fail ("could not open the session with its tone");

    auto& strip = engine.getChannelStrip (0);
    const auto mode = strip.insertMode.load();
    std::string error;
    engine.suspendProcessing();
    const bool loaded = strip.loadBuiltin ("dusk.builtin.utility", error);
    if (loaded) strip.insertMode.store (ChannelStrip::kInsertPlugin);
    engine.resumeProcessing();
    if (! loaded) return ScenarioResult::fail ("could not load Utility: " + error);
    ctx.cleanup ([&host, &engine, &strip, mode]
    {
        engine.suspendProcessing();
        strip.unloadBuiltin();
        strip.insertMode.store (mode);
        engine.resumeProcessing();
        if (auto* handle = host.strip (0)) handle->refreshInsertButton();
    });
    auto& utility = strip.getBuiltinSlot();
    int gainIndex = -1;
    for (int index = 0; index < utility.paramCount(); ++index)
        if (const auto* info = utility.paramInfo (index);
            info != nullptr && info->id != nullptr && std::string_view (info->id) == "gain_db") gainIndex = index;
    if (gainIndex < 0) return ScenarioResult::fail ("Utility has no gain parameter");
    utility.setParamValue (gainIndex, -24.0f);
    if (auto* handle = host.strip (0)) handle->refreshInsertButton();

    host.switchToStage (GuiHost::Stage::Mixing);
    session.master().mute.store (true);
    transport.setPlayhead (0);
    engine.play();

    auto registration = std::make_shared<std::vector<bool>>();
    auto before = std::make_shared<float> (-100.0f);
    auto mark = std::make_shared<std::int64_t> (0);
    const auto savedWhilePlaying = [&host, &ctx, &engine, &transport, &track, registration, mark]
                                   (const std::string& how, const std::string& status)
    {
        ctx.expect (host.modalStackEmpty(), how + " left '" + host.modalText() + "' up");
        ctx.expect (host.statusMessage() == status, how + " reported '" + host.statusMessage() + "'");
        ctx.expect (! engine.isProcessingSuspended(), how + " left the audio thread held out");
        ctx.expect (registration->empty(), how + " took the engine off the audio device");
        ctx.expect (transport.isPlaying(), how + " stopped the rolling transport");
        *mark = transport.getPlayhead();
        track.meterOutLDb.store (-100.0f);
    };
    const auto mixerRanOn = [&ctx, &transport, &track, &strip, before, mark, rate] (const std::string& how)
    {
        const auto moved = transport.getPlayhead() - *mark;
        ctx.expect (moved > static_cast<std::int64_t> (rate * 0.2),
                    "the playhead moved " + std::to_string (moved) + " samples in the half second after " + how);
        const float after = track.meterOutLDb.load();
        ctx.expect (strip.isBuiltinLoaded() && std::abs (after - *before) < 3.0f,
                    "after " + how + " the strip read " + std::to_string (after) + " dB, against "
                        + std::to_string (*before) + " dB through the Utility before it");
    };

    const auto copy = ctx.tempDir() / "Mixer copy";
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 600, [&ctx, &engine, &track, before, registration]
    {
        // The tone peaks at -6 dBFS dry, so a reading this far down is the
        // Utility's 24 dB at work.
        *before = track.meterOutLDb.load();
        ctx.expect (*before > -45.0f && *before < -24.0f,
                    "the tone through the Utility read " + std::to_string (*before) + " dB");
        engine.observeAudioRegistrationForScenario ([registration] (bool attached) { registration->push_back (attached); });
    } });
    pushFileMenuSteps (host, ctx, *steps, "Save");
    steps->push_back ({ 500, [savedWhilePlaying] { savedWhilePlaying ("Save", "Saved: Mixer session"); } });
    // Fixed waits on purpose: the playhead and the meter move on the device's
    // real-time thread, which a busy message loop does not hold up, and a late
    // timer only gives them longer. Only audio that stopped fails these.
    steps->push_back ({ 500, [mixerRanOn] { mixerRanOn ("Save"); } });
    pushSaveAsSteps (host, ctx, *steps, copy);
    steps->push_back ({ 800, [&ctx, &session, savedWhilePlaying, copy]
    {
        ctx.expect (currentSessionDirectory (session) == copy,
                    "Save As left the session at '" + currentSessionDirectory (session).string() + "'");
        savedWhilePlaying ("Save As", "Saved: Mixer copy");
    } });
    steps->push_back ({ 500, [&engine, mixerRanOn]
    {
        mixerRanOn ("Save As");
        engine.observeAudioRegistrationForScenario ({});
        engine.stop();
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar saveKeepsMixerRunning { Scenario {
    "gui.save_keeps_mixer_running", { "gui", "session" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSaveKeepsMixerRunning (host, ctx); }
} };

// A session remembers the sample rate its audio was made at, and a session that
// has never been saved learns it at its first save. That save can run with the
// audio callback detached: the quit prompt's Save detaches it first, and a
// detached engine reads a running rate of 0. So File > Save on a never-saved
// session runs once with the callback attached and once detached the way the
// quit prompt leaves it; the bb-quit-save-rate leg drives the quit prompt's own
// Save, which ends the process.
std::optional<ScenarioResult> runFreshSessionSaveStampsRate (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty() || host.engineDetached())
        return ScenarioResult::skip ("requires a stopped transport, no modal and the callback attached");
    const double rate = engine.getCurrentSampleRate();
    if (! engine.isAudioCallbackRegistered() || rate <= 0.0)
        return ScenarioResult::skip ("requires a running audio device");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &engine, &session, originalDir, restore]
    {
        drainModals (host);
        if (! engine.isAudioCallbackRegistered()) engine.reattachAudioCallback();
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });

    // The launch session has no rate of its own until its first save.
    const auto startFresh = [&host, &ctx, &session]
    {
        host.startUnsavedSessionIn (ctx.tempDir());
        session.sessionSampleRate = 0.0;
        ctx.expect (host.clickFileMenu(), "the File menu is unavailable");
    };
    const auto stamped = [&ctx, rate] (const fs::path& folder, const std::string& how)
    {
        auto saved = std::make_unique<Session>();
        applySessionDirectory (*saved, folder);
        if (! ctx.expect (SessionSerializer::load (*saved, folder / "session.json"),
                          how + " wrote no session that loads"))
            return;
        ctx.expect (std::abs (saved->sessionSampleRate - rate) < 0.5,
                    how + " saved the session's rate as " + std::to_string (saved->sessionSampleRate)
                        + " Hz, not the device's " + std::to_string (rate) + " Hz");
    };
    const auto saveAs = [&host, &ctx] (std::vector<Step>& steps, const fs::path& target, const std::string& how)
    {
        steps.push_back ({ 200, [&host, &ctx, how]
        { ctx.expect (host.clickContextMenuItem ("Save"), how + ": the File menu has no Save"); } });
        steps.push_back ({ 700, [&host, &ctx, target, how]
        {
            if (! ctx.expect (host.modalText().rfind ("Save session as...", 0) == 0,
                              how + " showed '" + host.modalText() + "' rather than Save As")
                || ! ctx.expect (host.focusFileName(), how + ": the Save As browser has no name field"))
                return;
            typeReplacing (host, target.string());
            ctx.expect (host.clickModalButton ("Save"), how + ": the Save As browser has no Save button");
        } });
    };

    const auto attached = ctx.tempDir() / "Saved attached";
    const auto detached = ctx.tempDir() / "Saved detached";
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [startFresh] { startFresh(); } });
    saveAs (*steps, attached, "File > Save");
    steps->push_back ({ 800, [&host, &ctx, &engine, startFresh, stamped, attached]
    {
        ctx.expect (host.modalStackEmpty(), "File > Save left '" + host.modalText() + "' up");
        stamped (attached, "File > Save");
        engine.detachAudioCallback();
        ctx.expect (engine.getCurrentSampleRate() <= 0.0, "a detached engine still reads a running rate");
        startFresh();
    } });
    saveAs (*steps, detached, "File > Save with the callback detached");
    steps->push_back ({ 800, [&host, &ctx, &engine, stamped, detached]
    {
        ctx.expect (host.modalStackEmpty(), "File > Save with the callback detached left '" + host.modalText() + "' up");
        stamped (detached, "File > Save with the callback detached");
        engine.reattachAudioCallback();
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar freshSessionSaveStampsRate { Scenario {
    "gui.fresh_session_save_stamps_rate", { "gui", "session" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runFreshSessionSaveStampsRate (host, ctx); }
} };

// The heartbeat writes session.json.autosave only when the session changed
// since the last save or autosave, never touches session.json and leaves no
// temp file behind. Opening a session whose autosave says something else
// offers recovery, and each answer does what the prompt says.
std::optional<ScenarioResult> runAutosave (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    static constexpr float kEdited = -12.0f;
    static constexpr float kLater  = -6.0f;

    auto& session = ctx.session();
    auto& fader = session.track (0).strip.faderDb;
    const float original = fader.load (std::memory_order_relaxed);
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, &fader, original, originalDir, restore]
    {
        drainModals (host);
        host.openSession (restore);
        fader.store (original, std::memory_order_relaxed);
        applySessionDirectory (session, originalDir);
    });

    const auto dir = ctx.tempDir() / "autosaved";
    std::error_code error;
    fs::create_directories (dir, error);
    const auto sessionJson = dir / "session.json";
    const auto autosave = dir / "session.json.autosave";
    session.track (0).name = "Aa";
    if (! SessionSerializer::save (session, sessionJson))
        return ScenarioResult::fail ("could not write the session to open");
    if (! host.openSession (sessionJson) || ! host.modalStackEmpty())
        return ScenarioResult::fail ("the session did not open without a prompt");

    host.autosaveTick();
    ctx.expect (! fs::exists (autosave, error), "the heartbeat wrote an autosave for an unchanged session");

    session.track (0).name = "BB";
    host.autosaveTick();
    ctx.expect (fs::exists (autosave, error), "a track rename colliding with the saved hash was not autosaved");
    session.track (0).name = "Aa";
    host.autosaveTick();
    ctx.expect (! fs::exists (autosave, error), "undoing a rename kept its recovery file");

    fader.store (kEdited, std::memory_order_relaxed);
    host.autosaveTick();
    ctx.expect (fs::exists (autosave, error), "the heartbeat did not write the change");
    ctx.expect (! fs::exists (dir / "session.json.autosave.tmp", error), "the autosave left its temp file behind");
    ctx.expect (nearly (savedFaderOf (autosave), kEdited), "the autosave does not hold the change");
    ctx.expect (nearly (savedFaderOf (sessionJson), original), "the heartbeat touched session.json");

    session.track (0).name = "BB";
    host.autosaveTick();
    auto renamed = std::make_unique<Session>();
    ctx.expect (SessionSerializer::load (*renamed, autosave) && renamed->track (0).nameUtf8() == "BB",
                "a track rename colliding with the previous autosave hash was not written");
    session.track (0).name = "Aa";
    host.autosaveTick();

    fader.store (original, std::memory_order_relaxed);
    host.autosaveTick();
    ctx.expect (! fs::exists (autosave, error), "returning to the saved state kept a stale recovery file");
    fader.store (kEdited, std::memory_order_relaxed);
    host.autosaveTick();
    ctx.expect (fs::exists (autosave, error), "editing again after returning to saved state wrote no recovery file");

    fs::remove (autosave, error);
    host.autosaveTick();
    ctx.expect (! fs::exists (autosave, error), "the heartbeat rewrote a state it had already written");

    // Recover: the autosave's state loads and becomes session.json.
    SessionSerializer::save (session, autosave);
    if (! ctx.expect (host.openSession (sessionJson) && ! host.modalStackEmpty(),
                      "a session with a newer autosave opened without offering recovery"))
        return ctx.verdict();
    ctx.expect (host.answerRecovery (GuiHost::Recovery::Recover), "the recovery prompt was not the modal up");
    ctx.expect (nearly (fader.load (std::memory_order_relaxed), kEdited), "Recover did not load the autosave");
    ctx.expect (nearly (savedFaderOf (sessionJson), kEdited), "Recover did not write the recovered state to session.json");
    ctx.expect (! fs::exists (autosave, error), "Recover left the autosave behind");

    // Load saved session: session.json wins and the autosave is discarded.
    fader.store (kLater, std::memory_order_relaxed);
    SessionSerializer::save (session, autosave);
    if (ctx.expect (host.openSession (sessionJson) && ! host.modalStackEmpty(),
                    "the second newer autosave offered no recovery"))
    {
        host.answerRecovery (GuiHost::Recovery::LoadSaved);
        ctx.expect (nearly (fader.load (std::memory_order_relaxed), kEdited), "Load saved session did not load session.json");
        ctx.expect (! fs::exists (autosave, error), "Load saved session kept the autosave");
    }

    // Cancel: nothing loads and the recovery point stays.
    fader.store (kLater, std::memory_order_relaxed);
    SessionSerializer::save (session, autosave);
    fader.store (kEdited, std::memory_order_relaxed);
    if (ctx.expect (host.openSession (sessionJson) && ! host.modalStackEmpty(),
                    "the third newer autosave offered no recovery"))
    {
        host.answerRecovery (GuiHost::Recovery::Cancel);
        ctx.expect (host.modalStackEmpty(), "Cancel left the prompt up");
        ctx.expect (fs::exists (autosave, error), "Cancel discarded the autosave");
    }
    return ctx.verdict();
}

const ScenarioRegistrar autosave { Scenario {
    "gui.autosave_writes_and_recovers",
    { "gui", "session", "autosave" },
    Needs::Engine | Needs::Gui,
    {},
    {},
    60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAutosave (host, ctx); }
} };

std::optional<ScenarioResult> runRevertedAutosaveQuitsClean (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, originalDir, restore]
    {
        drainModals (host);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });

    const auto folder = ctx.tempDir() / "Reverted autosave";
    fs::create_directories (folder);
    const auto canonical = folder / "session.json";
    const auto autosaveFile = folder / "session.json.autosave";
    auto saved = std::make_unique<Session>();
    saved->sessionSampleRate = engine.getCurrentSampleRate();
    saved->track (0).strip.faderDb.store (-3.0f);
    if (! SessionSerializer::save (*saved, canonical)
        || ! host.openSession (canonical) || ! host.modalStackEmpty())
        return ScenarioResult::fail ("could not open the saved session");

    auto& fader = session.track (0).strip.faderDb;
    fader.store (-12.0f);
    host.autosaveTick();
    if (! ctx.expect (fs::exists (autosaveFile) && nearly (savedFaderOf (autosaveFile), -12.0f),
                      "the edit did not create its recovery file"))
        return ctx.verdict();
    fader.store (-3.0f);

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&host, &ctx, autosaveFile]
    {
        // Quit must recognize the saved state before the next heartbeat.
        ctx.expect (! host.requestQuit(), "reverting an autosaved edit still asked to save before quitting");
        ctx.expect (! fs::exists (autosaveFile), "a clean quit check kept the stale recovery file");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        if (! host.modalStackEmpty())
            ctx.expect (host.clickModalButton ("Cancel"), "the unexpected quit prompt could not be cancelled");
    } });
    steps->push_back ({ 300, [&host, &ctx, &fader, canonical, autosaveFile]
    {
        ctx.expect (host.modalStackEmpty(), "cancelling the unexpected quit prompt left a modal up");
        fader.store (-12.0f);
        host.autosaveTick();
        ctx.expect (fs::exists (autosaveFile) && nearly (savedFaderOf (autosaveFile), -12.0f),
                    "editing again after a clean quit check wrote no recovery file");
        ctx.expect (nearly (savedFaderOf (canonical), -3.0f), "the clean quit check changed the manual save");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar revertedAutosaveQuitsClean { Scenario {
    "gui.reverted_autosave_quits_clean", { "gui", "session", "autosave", "quit" },
    Needs::Engine | Needs::Gui, {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runRevertedAutosaveQuitsClean (host, ctx); }
} };

std::optional<ScenarioResult> runUnclaimedAutosaveSurvivesCleanQueries (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, originalDir, restore]
    {
        drainModals (host);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });

    const auto folder = ctx.tempDir() / "Unclaimed autosave";
    fs::create_directories (folder);
    const auto canonical = folder / "session.json";
    const auto autosaveFile = folder / "session.json.autosave";
    const auto replacement = folder / "replacement.json";
    auto saved = std::make_unique<Session>();
    saved->sessionSampleRate = engine.getCurrentSampleRate();
    saved->track (0).strip.faderDb.store (-3.0f);
    if (! SessionSerializer::save (*saved, canonical)
        || ! host.openSession (canonical) || ! host.modalStackEmpty())
        return ScenarioResult::fail ("could not open the saved session");

    saved->track (0).strip.faderDb.store (-12.0f);
    if (! SessionSerializer::save (*saved, autosaveFile))
        return ScenarioResult::fail ("could not create the unclaimed recovery file");
    const auto unclaimedBytes = fileBytes (autosaveFile);
    saved->track (0).strip.faderDb.store (-18.0f);
    if (! SessionSerializer::save (*saved, replacement))
        return ScenarioResult::fail ("could not create the replacement recovery file");
    const auto replacementBytes = fileBytes (replacement);

    auto& fader = session.track (0).strip.faderDb;
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&host, &ctx, autosaveFile, unclaimedBytes]
    {
        ctx.expect (! host.requestQuit(), "an unclaimed recovery file made the unchanged session ask to save");
        ctx.expect (fileBytes (autosaveFile) == unclaimedBytes, "a clean quit check removed the unclaimed recovery file");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        if (! host.modalStackEmpty())
            ctx.expect (host.clickModalButton ("Cancel"), "the unexpected quit prompt could not be cancelled");
    } });
    steps->push_back ({ 300, [&host, &ctx, &fader, autosaveFile, unclaimedBytes, replacement, replacementBytes]
    {
        ctx.expect (host.modalStackEmpty(), "cancelling the unexpected quit prompt left a modal up");
        host.autosaveTick();
        ctx.expect (fileBytes (autosaveFile) == unclaimedBytes, "a clean heartbeat removed the unclaimed recovery file");

        fader.store (-6.0f);
        host.autosaveTick();
        ctx.expect (nearly (savedFaderOf (autosaveFile), -6.0f), "the live edit did not create an owned recovery file");
        std::error_code error;
        fs::copy_file (replacement, autosaveFile, fs::copy_options::overwrite_existing, error);
        if (! ctx.expect (! error, "could not replace the owned recovery file")) return;
        fader.store (-3.0f);
        ctx.expect (! host.requestQuit(), "an externally replaced recovery file made the reverted session ask to save");
        ctx.expect (fileBytes (autosaveFile) == replacementBytes, "a clean quit check removed the replaced recovery file");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        if (! host.modalStackEmpty())
            ctx.expect (host.clickModalButton ("Cancel"), "the replacement's unexpected quit prompt could not be cancelled");
    } });
    steps->push_back ({ 300, [&host, &ctx, &fader, canonical, autosaveFile, replacementBytes]
    {
        ctx.expect (host.modalStackEmpty(), "cancelling the replacement's quit prompt left a modal up");
        host.autosaveTick();
        ctx.expect (fileBytes (autosaveFile) == replacementBytes, "a clean heartbeat removed the replaced recovery file");
        fader.store (-6.0f);
        host.autosaveTick();
        ctx.expect (nearly (savedFaderOf (autosaveFile), -6.0f),
                    "the replaced recovery file left the previous live edit deduplicated");
        ctx.expect (nearly (savedFaderOf (canonical), -3.0f), "a clean check changed the manual save");
    } });
    steps->push_back ({ 300, [&host, &ctx, &session, &fader, canonical]
    {
        reopenSavedSession (host, canonical);
        host.startUnsavedSessionIn (ctx.tempDir() / "Sidecar switch");
        const auto folder = currentSessionDirectory (session);
        const auto owned = folder / "session.json.autosave";
        fader.store (-6.0f);
        host.autosaveTick();
        if (! ctx.expect (fs::exists (owned), "the unsaved session did not create its recovery file")) return;
        const auto privateDir = dusk::fs::appConfigDir() / "unsaved-session";
        const auto other = privateDir / "session.json.autosave";
        const bool existed = fs::exists (other);
        const auto previousBytes = existed ? fileBytes (other) : std::string();
        ctx.cleanup ([other, existed, previousBytes]
        {
            if (existed) dusk::fs::writeStringToFile (other, previousBytes);
            else { std::error_code ignored; fs::remove (other, ignored); }
        });
        fs::create_directories (privateDir);
        std::error_code error;
        fs::copy_file (owned, other, fs::copy_options::overwrite_existing, error);
        if (! ctx.expect (! error, "could not prepare the other recovery file")) return;
        const auto otherBytes = fileBytes (other);
        auto occupant = std::make_unique<Session>();
        occupant->track (0).name = "Another session";
        if (! ctx.expect (SessionSerializer::save (*occupant, folder / "session.json"),
                          "could not simulate a session appearing in the unsaved folder")) return;
        fader.store (-3.0f);
        ctx.expect (! host.requestQuit(), "a sidecar-folder switch made the reverted session dirty");
        ctx.expect (fs::exists (other) && fileBytes (other) == otherBytes,
                    "a clean check deleted an identical recovery file at a different path");
        ctx.expect (fs::exists (owned), "a sidecar-folder switch deleted the previous recovery file");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar unclaimedAutosaveSurvivesCleanQueries { Scenario {
    "gui.unclaimed_autosave_survives_clean_queries", { "gui", "session", "autosave", "quit" },
    Needs::Engine | Needs::Gui, {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runUnclaimedAutosaveSurvivesCleanQueries (host, ctx); }
} };

std::optional<ScenarioResult> runBuiltinRecovery (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty()
        || ! engine.isAudioCallbackRegistered())
        return ScenarioResult::skip ("requires stopped live audio and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    engine.publishPluginStateForSave (true);
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &engine, &session, originalDir, restore]
    {
        engine.stop();
        drainModals (host);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });

    const auto folder = ctx.tempDir() / "Built-in recovery";
    std::filesystem::create_directories (folder);
    const auto canonical = folder / "session.json";
    const auto autosaveFile = folder / "session.json.autosave";
    auto saved = std::make_unique<Session>();
    saved->track (0).builtinUnitId = "dusk.builtin.utility";
    saved->auxLane (0).builtinUnitId[0] = "dusk.builtin.delay";
    if (! SessionSerializer::save (*saved, canonical))
        return ScenarioResult::fail ("could not create the built-in session");
    reopenSavedSession (host, canonical);

    const auto parameter = [] (const builtin::NativeBuiltinSlot& slot, const char* id)
    {
        for (int i = 0; i < slot.paramCount(); ++i)
            if (const auto* info = slot.paramInfo (i);
                info != nullptr && info->id != nullptr && std::string_view (info->id) == id)
                return i;
        return -1;
    };
    auto steps = std::make_shared<std::vector<Step>>();
    pushFileMenuSteps (host, ctx, *steps, "Save");
    steps->push_back ({ 300, [&host, &ctx, &engine, parameter]
    {
        if (host.requestQuit())
        {
            ctx.expect (false, "the freshly saved built-ins were dirty before an edit");
            host.clickModalButton ("Cancel");
        }
        auto& track = engine.getChannelStrip (0).getBuiltinSlot();
        auto& aux = engine.getAuxLaneStrip (0).getBuiltinSlot (0);
        const int gain = parameter (track, "gain_db"), mix = parameter (aux, "mix");
        if (! ctx.expect (gain >= 0 && mix >= 0, "the track or aux built-in did not load")) return;
        track.setParamValue (gain, -12.0f);
        aux.setParamValue (mix, 0.24f);
        engine.play();
    } });
    steps->push_back ({ 300, [&host, &ctx, &engine, autosaveFile]
    {
        host.autosaveTick();
        ctx.expect (std::filesystem::exists (autosaveFile), "built-in-only edits wrote no recovery file");
        ctx.expect (engine.getTransport().isPlaying() && ! engine.isProcessingSuspended(),
                    "capturing built-in recovery state interrupted playback");
        engine.stop();
        ctx.expect (host.requestQuit(), "built-in-only edits did not ask to save before quitting");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        if (! host.modalStackEmpty())
            ctx.expect (host.clickModalButton ("Cancel"), "the quit prompt could not be cancelled");
    } });
    steps->push_back ({ 300, [&host, &ctx, &engine, canonical, autosaveFile, parameter]
    {
        if (! ctx.expect (host.openSession (canonical) && ! host.modalStackEmpty(),
                          "built-in-only edits did not offer recovery")) return;
        if (! ctx.expect (host.answerRecovery (GuiHost::Recovery::Recover), "built-in recovery failed")) return;
        auto& track = engine.getChannelStrip (0).getBuiltinSlot();
        auto& aux = engine.getAuxLaneStrip (0).getBuiltinSlot (0);
        const int gain = parameter (track, "gain_db"), mix = parameter (aux, "mix");
        if (ctx.expect (gain >= 0 && mix >= 0, "recovery lost a built-in unit"))
        {
            ctx.expect (nearly (track.getParamValue (gain), -12.0f), "recovery lost the track's control edit");
            ctx.expect (nearly (aux.getParamValue (mix), 0.24f), "recovery lost the aux control edit");
        }
        ctx.expect (! std::filesystem::exists (autosaveFile), "recovery left its sidecar behind");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar builtinRecovery { Scenario {
    "gui.builtin_edits_autosave_and_prompt", { "gui", "session", "builtin", "autosave" },
    Needs::Engine | Needs::Gui, {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runBuiltinRecovery (host, ctx); }
} };

std::optional<ScenarioResult> runMultisampleRecovery (GuiHost& host, ScenarioContext& ctx)
{
#if ! DUSKSTUDIO_HAS_MULTISAMPLE
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("multisample support is not built");
#else
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty()
        || ! engine.isAudioCallbackRegistered())
        return ScenarioResult::skip ("requires stopped live audio and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    engine.publishPluginStateForSave (true);
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &engine, &session, originalDir, restore]
    {
        engine.stop();
        drainModals (host);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });
    const auto folder = ctx.tempDir() / "Multisample recovery";
    std::filesystem::create_directories (folder);
    const auto soundfont = folder / "Fixture.sfz";
    if (! writeRampFixture (folder / "Tone.wav", 48000.0, 4096))
        return ScenarioResult::fail ("could not write the multisample recovery WAV");
    {
        std::ofstream file (soundfont);
        file << "<region> sample=Tone.wav key=60\n";
        if (! file) return ScenarioResult::fail ("could not write the multisample recovery SFZ");
    }
    const auto canonical = folder / "session.json";
    const auto autosaveFile = folder / "session.json.autosave";
    auto saved = std::make_unique<Session>();
    saved->track (0).mode.store (static_cast<int> (Track::Mode::Midi));
    saved->track (0).nativeMultisamplePath = soundfont.u8string();
    if (! SessionSerializer::save (*saved, canonical))
        return ScenarioResult::fail ("could not create the multisample session");
    reopenSavedSession (host, canonical);
    const auto loaded = [&engine]
    {
        const auto* instance = engine.getChannelStrip (0).getNativeMultisampleSlot().getInstance();
        return ! engine.hasPendingMultisampleLoads() && instance != nullptr && instance->hasLoadedRuntime();
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [] {}, loaded, "the multisample recovery instrument did not load" });
    pushFileMenuSteps (host, ctx, *steps, "Save");
    steps->push_back ({ 300, [&host, &ctx, &engine]
    {
        if (host.requestQuit())
        {
            ctx.expect (false, "the freshly saved multisample was dirty before an edit");
            host.clickModalButton ("Cancel");
        }
        auto* instance = engine.getChannelStrip (0).getNativeMultisampleSlot().getInstance();
        if (! ctx.expect (instance != nullptr, "the multisample instance disappeared")) return;
        engine.play();
        instance->getOverrides().masterVolDb.store (-12.0f, std::memory_order_relaxed);
        instance->getOverrides().masterTuneCents.store (37.0f, std::memory_order_relaxed);
        instance->setPolyphony (96);
        instance->setHDCC (74, 0.23f);
        instance->setHDCC (258, 0.61f);
    } });
    steps->push_back ({ 300, [&host, &ctx, &engine, autosaveFile]
    {
        host.autosaveTick();
        ctx.expect (std::filesystem::exists (autosaveFile), "multisample-only edits wrote no recovery file");
        ctx.expect (engine.getTransport().isPlaying() && ! engine.isProcessingSuspended(),
                    "capturing multisample recovery state interrupted playback");
        engine.stop();
        ctx.expect (host.requestQuit(), "multisample-only edits did not ask to save before quitting");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        if (! host.modalStackEmpty())
            ctx.expect (host.clickModalButton ("Cancel"), "the multisample quit prompt could not be cancelled");
    } });
    steps->push_back ({ 300, [&host, &ctx, canonical]
    {
        if (! ctx.expect (host.openSession (canonical) && ! host.modalStackEmpty(),
                          "multisample-only edits did not offer recovery")) return;
        ctx.expect (host.answerRecovery (GuiHost::Recovery::Recover), "multisample recovery failed");
    } });
    steps->push_back ({ 100, [&ctx, &engine, autosaveFile]
    {
        const auto* instance = engine.getChannelStrip (0).getNativeMultisampleSlot().getInstance();
        if (! ctx.expect (instance != nullptr, "recovery lost the multisample instance")) return;
        const auto& controls = instance->getOverrides();
        ctx.expect (nearly (controls.masterVolDb.load (std::memory_order_relaxed), -12.0f), "recovery lost multisample volume");
        ctx.expect (nearly (controls.masterTuneCents.load (std::memory_order_relaxed), 37.0f), "recovery lost multisample tune");
        ctx.expect (controls.polyphony.load (std::memory_order_relaxed) == 96, "recovery lost multisample polyphony");
        ctx.expect (nearly (instance->getHDCC (74), 0.23f) && nearly (instance->getHDCC (258), 0.61f),
                    "recovery lost standard or extended ARIA control overrides");
        ctx.expect (! std::filesystem::exists (autosaveFile), "multisample recovery left its sidecar behind");
    }, loaded, "the recovered multisample instrument did not load" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
#endif
}

const ScenarioRegistrar multisampleRecovery { Scenario {
    "gui.multisample_edits_autosave_and_prompt", { "gui", "session", "multisample", "autosave" },
    Needs::Engine | Needs::Gui, {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMultisampleRecovery (host, ctx); }
} };

// The Save As browser a quit's Save opens holds the quit's audio and autosave
// until it answers. Cmd+S and Cmd+Shift+S typed into it do nothing, so the quit
// still waits on it. Cmd+Q there asks again, and that quit's Save opens its own
// browser in place of the first, which is cancelled; the audio and the autosave
// stay off for the quit now waiting, and its Cancel gives them back.
std::optional<ScenarioResult> runSaveFromQuitBrowser (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty()
        || ! engine.isAudioCallbackRegistered() || ! host.autosaveRunning() || host.engineDetached())
        return ScenarioResult::skip ("requires a stopped transport, no modal, and live audio and autosave");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    ctx.cleanup ([&host, &session, originalDir, restore]
    {
        drainModals (host);
        host.abandonPendingFlows();
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
    });

    const auto untitled = ctx.tempDir() / "Untitled";
    std::filesystem::create_directories (untitled);
    const auto sessionJson = untitled / "session.json";
    applySessionDirectory (session, untitled);
    auto& fader = session.track (0).strip.faderDb;
    fader.store (fader.load() - 3.0f);

    const auto quitToSaveAs = [&host, &ctx, sessionJson] (const std::string& how)
    {
        if (! ctx.expect (host.modalText().rfind ("Save changes before quitting?", 0) == 0,
                          how + ": Quit showed '" + host.modalText() + "' rather than its prompt")
            || ! ctx.expect (! std::filesystem::exists (sessionJson), how + ": the session gained a session.json"))
            return;
        ctx.expect (host.clickModalButton ("Save"), how + ": the quit prompt did not offer Save");
    };
    const auto running = [&host, &ctx, &engine, sessionJson] (const std::string& what)
    {
        ctx.expect (engine.isAudioCallbackRegistered() && ! host.engineDetached(), what + " left the audio detached");
        ctx.expect (host.autosaveRunning(), what + " left the autosave timer stopped");
        ctx.expect (! std::filesystem::exists (sessionJson), what + " saved the session");
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.requestQuit(), "the unsaved session did not ask before quitting"); } });
    steps->push_back ({ 400, [quitToSaveAs] { quitToSaveAs ("the first quit"); } });
    steps->push_back ({ 600, [&host, &ctx]
    {
        if (! ctx.expect (host.modalText().rfind ("Save session as...", 0) == 0,
                          "Save in the quit prompt showed '" + host.modalText() + "' rather than Save As"))
            return;
        host.pressPeerKey ("command + S");
        host.pressPeerKey ("command + shift + S");
    } });
    steps->push_back ({ 300, [&host, &ctx, &engine, sessionJson]
    {
        ctx.expect (host.fileBrowserPanels() == 1, "Cmd+S or Cmd+Shift+S in the quit's Save As opened another browser");
        ctx.expect (host.modalText().rfind ("Save session as...", 0) == 0,
                    "Cmd+S in the quit's Save As left '" + host.modalText() + "' up");
        ctx.expect (host.engineDetached() && ! engine.isAudioCallbackRegistered(),
                    "Cmd+S in the quit's Save As let go of the quit");
        ctx.expect (! std::filesystem::exists (sessionJson), "Cmd+S in the quit's Save As saved the session");
        ctx.expect (host.clickModalButton ("Cancel"), "the quit's Save As has no Cancel");
    } });
    steps->push_back ({ 600, [&host, &ctx, running]
    {
        ctx.expect (host.modalStackEmpty(), "Cancel in the quit's Save As left '" + host.modalText() + "' up");
        running ("Cancel in the quit's Save As after Cmd+S");
        ctx.expect (host.requestQuit(), "the unsaved session did not ask before quitting again");
    } });
    steps->push_back ({ 400, [quitToSaveAs] { quitToSaveAs ("the second quit"); } });
    steps->push_back ({ 600, [&host, &ctx]
    {
        if (! ctx.expect (host.modalText().rfind ("Save session as...", 0) == 0,
                          "the second quit's Save showed '" + host.modalText() + "' rather than Save As"))
            return;
        ctx.expect (host.pressPeerKey ("command + Q"), "the quit's Save As did not pass Cmd+Q on");
    } });
    steps->push_back ({ 400, [quitToSaveAs] { quitToSaveAs ("Cmd+Q in the quit's Save As"); } });
    steps->push_back ({ 600, [&host, &ctx, &engine, sessionJson]
    {
        ctx.expect (host.modalText().rfind ("Save session as...", 0) == 0,
                    "the third quit's Save showed '" + host.modalText() + "' rather than Save As");
        ctx.expect (host.modalCount() == 1, "the third quit's Save As left another dialog under it");
        ctx.expect (host.engineDetached() && ! engine.isAudioCallbackRegistered() && ! host.autosaveRunning(),
                    "the cancelled Save As gave the audio or the autosave back while the third quit waits");
        ctx.expect (! std::filesystem::exists (sessionJson), "the third quit saved the session");
        ctx.expect (host.clickModalButton ("Cancel"), "the third quit's Save As has no Cancel");
    } });
    steps->push_back ({ 600, [&host, &ctx, running]
    {
        ctx.expect (host.modalStackEmpty(), "Cancel in the third quit's Save As left '" + host.modalText() + "' up");
        running ("Cancel in the third quit's Save As");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar saveFromQuitBrowser { Scenario {
    "gui.save_from_quit_browser", { "gui", "session", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSaveFromQuitBrowser (host, ctx); }
} };

std::vector<std::string> folderListing (const std::filesystem::path& dir)
{
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : std::filesystem::recursive_directory_iterator (dir, error))
        names.push_back (entry.path().lexically_relative (dir).generic_string());
    std::sort (names.begin(), names.end());
    return names;
}

// Save as... into a folder that holds another session is refused and leaves
// that session alone: from the File menu, and from the Save that a never-saved
// session offers before opening another session and before quitting, neither
// of which may then carry on as though it had saved.
std::optional<ScenarioResult> runSaveAsOtherSession (GuiHost& host, ScenarioContext& ctx)
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
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });

    const auto mine = ctx.tempDir() / "Mine";
    std::filesystem::create_directories (mine);
    if (! SessionSerializer::save (session, mine / "session.json")) return ScenarioResult::fail ("could not save the session");
    reopenSavedSession (host, mine / "session.json");
    if (currentSessionDirectory (session) != mine) return ScenarioResult::fail ("could not open the saved session");
    const auto other = ctx.tempDir() / "Other";
    std::filesystem::create_directories (other);
    {
        auto theirs = std::make_unique<Session>();
        theirs->track (0).name = "Theirs";
        if (! SessionSerializer::save (*theirs, other / "session.json"))
            return ScenarioResult::fail ("could not write the other session");
    }
    const auto untitled = ctx.tempDir() / "Untitled";
    std::filesystem::create_directories (untitled);
    const auto mineBytes = fileBytes (mine / "session.json");
    const auto otherBytes = fileBytes (other / "session.json");
    const auto otherListing = folderListing (other);

    const auto saveIntoOther = [&host, &ctx, other] (const std::string& how)
    {
        if (! ctx.expect (host.modalText().rfind ("Save session as...", 0) == 0,
                          how + " showed '" + host.modalText() + "' rather than Save As")
            || ! ctx.expect (host.focusFileName(), how + ": the Save As browser has no name field"))
            return;
        typeReplacing (host, other.string());
        ctx.expect (host.clickModalButton ("Save"), how + ": the Save As browser has no Save button");
    };
    const auto refused = [&host, &ctx, other, otherBytes, otherListing] (const std::string& how)
    {
        const auto expected = "Folder holds another session\nThis folder already holds another session:\n\n    "
                            + other.string()
                            + "\n\nSaving here would replace it, so nothing was saved and nothing was changed. "
                              "Choose a new name, or a folder without a session.";
        ctx.expect (host.modalText() == expected, how + " showed '" + host.modalText() + "' rather than the refusal");
        ctx.expect (host.statusMessage() == "A session already exists at: Other",
                    how + " left the status '" + host.statusMessage() + "'");
        ctx.expect (fileBytes (other / "session.json") == otherBytes && folderListing (other) == otherListing,
                    how + " changed the other session's folder");
        ctx.expect (host.clickModalButton ("OK"), how + ": the refusal has no OK button");
    };
    const auto kept = [&host, &ctx, &session] (const std::string& how, const std::filesystem::path& dir)
    {
        ctx.expect (host.modalStackEmpty(), how + ": OK left '" + host.modalText() + "' up");
        ctx.expect (currentSessionDirectory (session) == dir,
                    how + " moved the session to '" + currentSessionDirectory (session).string() + "'");
    };
    auto& fader = session.track (0).strip.faderDb;
    const float dirtied = fader.load() - 3.0f;

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx] { ctx.expect (host.clickFileMenu(), "the File menu is unavailable"); } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Save as..."), "the File menu has no Save as..."); } });
    steps->push_back ({ 700, [saveIntoOther] { saveIntoOther ("File > Save as..."); } });
    steps->push_back ({ 700, [refused] { refused ("File > Save as..."); } });
    steps->push_back ({ 400, [&host, &ctx, &session, &fader, kept, mine, mineBytes, untitled, dirtied]
    {
        kept ("File > Save as...", mine);
        ctx.expect (fileBytes (mine / "session.json") == mineBytes, "a refused Save As rewrote the session's own file");
        applySessionDirectory (session, untitled);
        fader.store (dirtied);
        ctx.expect (host.clickFileMenu(), "the File menu is unavailable");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Open..."), "the File menu has no Open..."); } });
    steps->push_back ({ 600, [&host, &ctx]
    {
        ctx.expect (host.modalText().rfind ("Save changes before opening another session?", 0) == 0,
                    "File > Open over unsaved changes showed '" + host.modalText() + "'");
        ctx.expect (host.clickModalButton ("Save"), "the unsaved-changes prompt did not offer Save");
    } });
    steps->push_back ({ 700, [saveIntoOther] { saveIntoOther ("Save before File > Open"); } });
    steps->push_back ({ 700, [refused] { refused ("Save before File > Open"); } });
    steps->push_back ({ 700, [&host, &ctx, &fader, kept, untitled, dirtied]
    {
        kept ("Save before File > Open", untitled);
        ctx.expect (nearly (fader.load(), dirtied) && ! std::filesystem::exists (untitled / "session.json"),
                    "a refused Save before File > Open did not leave the session as it was");
        // A refusal that did not hold would let the quit prompt's Save end the process.
        if (ctx.verdict().status != ScenarioStatus::Pass)
        {
            ctx.complete (ctx.verdict());
            return;
        }
        ctx.expect (host.requestQuit(), "the unsaved session did not ask before quitting");
    } });
    steps->push_back ({ 400, [&host, &ctx]
    {
        ctx.expect (host.modalText().rfind ("Save changes before quitting?", 0) == 0,
                    "Quit showed '" + host.modalText() + "' rather than its prompt");
        ctx.expect (host.clickModalButton ("Save"), "the quit prompt did not offer Save");
    } });
    steps->push_back ({ 700, [saveIntoOther] { saveIntoOther ("Save in the quit prompt"); } });
    steps->push_back ({ 700, [refused] { refused ("Save in the quit prompt"); } });
    steps->push_back ({ 600, [&host, &ctx, &engine, &fader, kept, untitled, dirtied]
    {
        kept ("Save in the quit prompt", untitled);
        ctx.expect (engine.isAudioCallbackRegistered() && ! host.engineDetached() && host.autosaveRunning(),
                    "a refused Save in the quit prompt left audio or autosave parked for a shutdown");
        ctx.expect (nearly (fader.load(), dirtied) && ! std::filesystem::exists (untitled / "session.json"),
                    "a refused Save in the quit prompt did not leave the session as it was");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar saveAsOtherSession { Scenario {
    "gui.save_as_refuses_other_session", { "gui", "session", "messages" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSaveAsOtherSession (host, ctx); }
} };

// File > Open..., browse to a session's folder and choose its session.json from
// the list: that session loads.
std::optional<ScenarioResult> runOpenSessionFolder (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty() || host.engineDetached()
        || ! engine.isAudioCallbackRegistered() || engine.getCurrentSampleRate() <= 0.0 || engine.isProcessingSuspended())
        return ScenarioResult::skip ("requires stopped live audio, no processing suspension and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    ctx.cleanup ([&host, &session, &engine, originalDir, restore]
    {
        drainModals (host);
        if (! engine.isAudioCallbackRegistered()) engine.reattachAudioCallback();
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });
    // Clean, so Open goes straight to the browser.
    const auto clean = ctx.tempDir() / "clean" / "session.json";
    std::filesystem::create_directories (clean.parent_path());
    if (! SessionSerializer::save (session, clean)) return ScenarioResult::fail ("could not save the clean session");
    reopenSavedSession (host, clean);

    // Holds session.json alone, so the first row of its listing is that file.
    const auto target = ctx.tempDir() / "Open Me";
    std::filesystem::create_directories (target);
    {
        auto other = std::make_unique<Session>();
        other->track (0).name = "Opened";
        other->track (0).strip.faderDb.store (-13.0f);
        if (! SessionSerializer::save (*other, target / "session.json"))
            return ScenarioResult::fail ("could not write the session to open");
    }

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx] { ctx.expect (host.clickFileMenu(), "the File menu is unavailable"); } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Open..."), "the File menu has no Open..."); } });
    steps->push_back ({ 700, [&host, &ctx, target]
    {
        if (! ctx.expect (host.modalText().rfind ("Open session.json", 0) == 0,
                          "File > Open showed '" + host.modalText() + "' rather than its browser")
            || ! ctx.expect (host.clickFileBrowserControl (true), "the Open browser has no path box"))
            return;
        typeReplacing (host, target.string());
        ctx.expect (host.pressPeerKey ("Return", 0), "the path box did not take Return");
    } });
    steps->push_back ({ 700, [&host, &ctx]
    {
        ctx.expect (host.clickFileBrowserControl (false), "the Open browser has no file list");
        host.pressPeerKey ("Home", 0);
    } });
    steps->push_back ({ 200, [&host, &ctx, &engine]
    {
        engine.detachAudioCallback();
        ctx.expect (! engine.isAudioCallbackRegistered() && engine.getCurrentSampleRate() <= 0.0,
                    "the session-load fixture did not detach its callback");
        ctx.expect (host.clickModalButton ("Open"), "the Open browser has no Open button");
    } });
    steps->push_back ({ 1000, [&host, &ctx, &session, &engine, target]
    {
        ctx.expect (host.modalStackEmpty(), "Open left '" + host.modalText() + "' up");
        ctx.expect (currentSessionDirectory (session) == target,
                    "Open left '" + currentSessionDirectory (session).string() + "' open; status '"
                        + host.statusMessage() + "'");
        ctx.expect (session.track (0).name.toStdString() == "Opened" && nearly (session.track (0).strip.faderDb.load(), -13.0f),
                    "the window does not hold the chosen session's track 1");
        ctx.expect (! engine.isAudioCallbackRegistered() && engine.getCurrentSampleRate() <= 0.0,
                    "opening a session reattached the deliberately detached audio callback");
        ctx.expect (! engine.isProcessingSuspended(), "opening a session left processing suspended");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar openSessionFolder { Scenario {
    "gui.open_session_folder", { "gui", "session" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runOpenSessionFolder (host, ctx); }
} };

// A session.json that cannot be written: Save, from the File menu and by
// Cmd+S, raises "Save failed" as the manual quotes it, and the edited session
// stays in memory while session.json on disk stays as it was.
std::optional<ScenarioResult> runSaveFailedAlert (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    auto& session = ctx.session();
    if (! ctx.engine().getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    ctx.cleanup ([&host, &session, originalDir, restore]
    {
        drainModals (host);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });
    const auto locked = ctx.tempDir() / "Locked session";
    const auto sessionJson = locked / "session.json";
    fs::create_directories (locked / "audio");
    if (! SessionSerializer::save (session, sessionJson)) return ScenarioResult::fail ("could not save the session to lock");
    reopenSavedSession (host, sessionJson);
    if (currentSessionDirectory (session) != locked) return ScenarioResult::fail ("could not open the session to lock");
    auto& fader = session.track (0).strip.faderDb;
    const float onDisk = savedFaderOf (sessionJson);
    const float edited = onDisk - 4.0f;
    fader.store (edited);

    bool unwritable = false;
   #if ! defined (_WIN32)
    std::error_code error;
    fs::permissions (locked, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace, error);
    ctx.cleanup ([locked]
    {
        std::error_code ignored;
        std::filesystem::permissions (locked, std::filesystem::perms::owner_all,
                                      std::filesystem::perm_options::replace, ignored);
    });
    {
        const auto probe = locked / "probe";
        std::ofstream out (probe);
        unwritable = ! error && ! out.is_open();
        out.close();
        std::error_code ignored;
        if (! unwritable) fs::remove (probe, ignored);
    }
   #endif
    // Root writes regardless of the permission bits, and on Windows they only
    // set a read-only attribute that a folder ignores. There, a folder that is
    // not empty where the save writes its temporary copy fails the same write.
    if (! unwritable)
    {
        const auto blocker = locked / "session.json.tmp";
        fs::create_directories (blocker);
        std::ofstream (blocker / "keep") << "theirs";
        if (! fs::is_regular_file (blocker / "keep"))
            return ScenarioResult::fail ("could not block the session's temporary copy");
    }

    const auto alerted = [&host, &ctx] (const std::string& how)
    {
        const auto text = host.modalText();
        ctx.expect (text.rfind ("Save failed\nDusk Studio could not write the session file:", 0) == 0,
                    how + " showed '" + text + "' rather than the Save failed alert");
        ctx.expect (text.find ("Locked session") != std::string::npos && text.find ("session.json") != std::string::npos,
                    how + ": the alert does not name the session file");
        ctx.expect (text.find ("Common causes: disk full, missing write permission, or the parent folder was moved "
                               "since the session was opened. The session is unchanged in memory; try Save As to a "
                               "different location.") != std::string::npos,
                    how + ": the alert does not give the manual's causes and advice");
        ctx.expect (host.statusMessage() == "Save failed: Locked session",
                    how + " left the status '" + host.statusMessage() + "'");
        ctx.expect (host.clickModalButton ("OK"), how + ": the alert has no OK button");
    };
    const auto kept = [&host, &ctx, &session, &fader, locked, sessionJson, onDisk, edited] (const std::string& how)
    {
        ctx.expect (host.modalStackEmpty(), how + ": OK left '" + host.modalText() + "' up");
        ctx.expect (currentSessionDirectory (session) == locked && nearly (fader.load(), edited),
                    how + " did not keep the edited session in memory");
        ctx.expect (nearly (savedFaderOf (sessionJson), onDisk), how + " changed session.json on disk");
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx] { ctx.expect (host.clickFileMenu(), "the File menu is unavailable"); } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Save"), "the File menu has no Save"); } });
    steps->push_back ({ 800, [alerted] { alerted ("File > Save"); } });
    steps->push_back ({ 400, [&host, &ctx, kept]
    {
        kept ("File > Save");
        ctx.expect (host.pressPeerKey (commandChord ('s'), 's'), "the window did not handle Cmd+S");
    } });
    steps->push_back ({ 800, [alerted] { alerted ("Cmd+S"); } });
    steps->push_back ({ 400, [kept] { kept ("Cmd+S"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar saveFailedAlert { Scenario {
    "gui.save_failed_alert", { "gui", "session", "messages" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSaveFailedAlert (host, ctx); }
} };

// Save As that fails after the audio was copied, at session.json and at the
// notepad sidecar: the alert says the session is unchanged, so it must still
// live in its old folder with its region there, the new folder must hold only
// what it held before, and a following Save writes the old folder's files.
std::optional<ScenarioResult> runSaveAsFailureKeepsSession (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    auto& session = ctx.session();
    if (! ctx.engine().getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    ctx.cleanup ([&host, &session, originalDir, restore]
    {
        drainModals (host);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });

    const auto mine = ctx.tempDir() / "Mine";
    const auto take = mine / "audio" / "take.wav";
    fs::create_directories (take.parent_path());
    {
        dusk::audio::WriteSpec spec;
        spec.sampleRate = 48000;
        spec.numChannels = 1;
        auto writer = dusk::audio::FileWriter::create (take, spec);
        std::vector<float> silence (4800, 0.0f);
        const float* channels[] = { silence.data() };
        if (! writer || ! writer->write (channels, 1, 4800) || ! writer->flush())
            return ScenarioResult::fail ("could not write the take");
    }
    {
        std::ofstream notes (mine / "notepad.md", std::ios::binary);
        notes << "notes";
    }
    AudioRegion region;
    using File = std::decay_t<decltype (region.file)>;
    region.file = File (take.u8string().c_str());
    region.lengthInSamples = 4800;
    region.numChannels = 1;
    session.track (0).regions = { region };
    if (! SessionSerializer::save (session, mine / "session.json")) return ScenarioResult::fail ("could not save the session");
    reopenSavedSession (host, mine / "session.json");
    const auto regionFile = [&session]
    {
        const auto& regions = session.track (0).regions;
        return regions.empty() ? fs::path() : fs::u8path (regions[0].file.getFullPathName().toStdString());
    };
    if (currentSessionDirectory (session) != mine || regionFile() != take)
        return ScenarioResult::fail ("could not open the session with its take");

    // A folder that is not empty where the save writes a file makes that
    // write fail for any user, root included; an empty one would be replaced.
    const auto blockedJson = ctx.tempDir() / "Blocked json";
    const auto blockedNotes = ctx.tempDir() / "Blocked notes";
    for (const auto& blocker : { blockedJson / "session.json", blockedNotes / "notepad.md" })
    {
        fs::create_directories (blocker);
        std::ofstream (blocker / "keep") << "theirs";
    }
    const auto jsonListing = folderListing (blockedJson);
    const auto notesListing = folderListing (blockedNotes);

    auto steps = std::make_shared<std::vector<Step>>();
    const auto saveAs = [&host, &ctx, steps] (const fs::path& target) { pushSaveAsSteps (host, ctx, *steps, target); };
    const auto kept = [&host, &ctx, &session, regionFile, mine, take] (const std::string& how,
                                                                        const fs::path& target,
                                                                        const std::vector<std::string>& before)
    {
        ctx.expect (host.clickModalButton ("OK"), how + ": the alert has no OK button");
        ctx.expect (currentSessionDirectory (session) == mine,
                    how + " moved the session to '" + currentSessionDirectory (session).string() + "'");
        ctx.expect (regionFile() == take, how + " left the region at '" + regionFile().string() + "'");
        ctx.expect (folderListing (target) == before, how + " left files in the new folder");
        ctx.expect (fs::is_regular_file (take), how + " removed the original take");
    };

    // OK only closes the alert, so a press the window lost is pressed again.
    const auto alertClosed = [&host, steps] (std::function<void()> then)
    {
        steps->push_back ({ 0, std::move (then), [&host] { return host.modalStackEmpty(); },
                            "OK did not close the alert", [&host] { host.clickModalButton ("OK"); } });
    };

    saveAs (blockedJson);
    steps->push_back ({ 0, [&host, &ctx, kept, blockedJson, jsonListing]
    {
        const auto text = host.modalText();
        ctx.expect (text.rfind ("Save failed\nDusk Studio could not write the session file:", 0) == 0
                        && text.find ("Blocked json") != std::string::npos,
                    "a failed session.json write showed '" + text + "' rather than Save failed");
        kept ("a failed session.json write", blockedJson, jsonListing);
    } });
    alertClosed ([&host, &ctx]
    { ctx.expect (host.pressPeerKey (commandChord ('s'), 's'), "the window did not handle Cmd+S"); });
    steps->push_back ({ 800, [&host, &ctx, mine, take]
    {
        const auto saved = fileBytes (mine / "session.json");
        ctx.expect (host.statusMessage() == "Saved: Mine" && saved.find ("Blocked json") == std::string::npos,
                    "Cmd+S after the failed Save As did not save the session where it lives; status '"
                        + host.statusMessage() + "'");
        auto reloaded = std::make_unique<Session>();
        applySessionDirectory (*reloaded, mine);
        ctx.expect (SessionSerializer::load (*reloaded, mine / "session.json") && ! reloaded->track (0).regions.empty()
                        && fs::u8path (reloaded->track (0).regions[0].file.getFullPathName().toStdString()) == take,
                    "the session saved after the failed Save As does not point at its take");
    } });
    saveAs (blockedNotes);
    steps->push_back ({ 0, [&host, &ctx, kept, blockedNotes, notesListing]
    {
        const auto text = host.modalText();
        ctx.expect (text.rfind ("Notepad save failed\nDusk Studio could not write:", 0) == 0
                        && text.find ("The session was not saved.") != std::string::npos,
                    "a failed notepad write showed '" + text + "' rather than Notepad save failed");
        kept ("a failed notepad write", blockedNotes, notesListing);
    } });
    alertClosed ([] {});
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar saveAsFailureKeepsSession { Scenario {
    "gui.save_as_failure_keeps_session", { "gui", "session", "messages" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSaveAsFailureKeepsSession (host, ctx); }
} };

// The launch session's folder can already hold a real session someone saved
// under that name. The launch session has still never been saved, so File >
// Save, Cmd+S and the quit prompt's Save each ask where to save and leave that
// session's files alone; a session opened from disk still saves in place.
std::optional<ScenarioResult> runFreshSessionSaveAsks (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty()
        || ! engine.isAudioCallbackRegistered() || ! host.autosaveRunning() || host.engineDetached())
        return ScenarioResult::skip ("requires a stopped transport, no modal, and live audio and autosave");
    if (host.sessionOnDisk()) return ScenarioResult::skip ("requires the launch session never to have been saved");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    ctx.cleanup ([&host, &engine, &session, originalDir, restore]
    {
        drainModals (host);
        engine.reattachAudioCallback();
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });

    const auto untitled = ctx.tempDir() / "Untitled";
    const auto untitledJson = untitled / "session.json";
    const auto untitledNotes = untitled / "notepad.md";
    fs::create_directories (untitled / "audio");
    auto& fader = session.track (0).strip.faderDb;
    const float launchFader = fader.load();
    fader.store (launchFader - 6.0f);
    const bool wrote = SessionSerializer::save (session, untitledJson);
    fader.store (launchFader);
    std::ofstream (untitledNotes) << "Notes of the session saved as Untitled\n";
    if (! wrote || ! fs::exists (untitledNotes)) return ScenarioResult::fail ("could not write the Untitled session");
    const auto read = [] (const fs::path& path)
    {
        std::ifstream input (path, std::ios::binary);
        return std::string (std::istreambuf_iterator<char> (input), std::istreambuf_iterator<char>());
    };
    const auto jsonBytes = read (untitledJson);
    const auto notesBytes = read (untitledNotes);

    applySessionDirectory (session, untitled);
    const float dirtied = launchFader - 3.0f;
    fader.store (dirtied);

    const auto asked = std::make_shared<int> (0);
    const auto askedWhere = [&host, &ctx, read, asked, untitledJson, untitledNotes, jsonBytes, notesBytes]
                            (const std::string& how)
    {
        const bool saveAs = ctx.expect (host.modalText().rfind ("Save session as...", 0) == 0,
                                        how + " showed '" + host.modalText() + "' rather than Save As");
        ctx.expect (read (untitledJson) == jsonBytes, how + " wrote over the Untitled session's session.json");
        ctx.expect (read (untitledNotes) == notesBytes, how + " wrote over the Untitled session's notepad.md");
        if (saveAs && ctx.expect (host.clickModalButton ("Cancel"), how + ": Save As has no Cancel"))
            ++*asked;
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx] { ctx.expect (host.clickFileMenu(), "the File menu is unavailable"); } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Save"), "the File menu has no Save"); } });
    steps->push_back ({ 700, [askedWhere] { askedWhere ("File > Save"); } });
    steps->push_back ({ 400, [&host, &ctx]
    {
        ctx.expect (host.modalStackEmpty(), "Cancel left '" + host.modalText() + "' up");
        ctx.expect (host.pressPeerKey (commandChord ('s'), 's'), "the window did not handle Cmd+S");
    } });
    steps->push_back ({ 700, [askedWhere] { askedWhere ("Cmd+S"); } });
    // A Save from the quit prompt that completes ends the process, so it is only
    // pressed once the other two have shown they ask first.
    steps->push_back ({ 400, [&host, &ctx, asked]
    {
        if (*asked == 2)
            ctx.expect (host.requestQuit(), "the edited session did not ask before quitting");
    } });
    steps->push_back ({ 400, [&host, &ctx, asked]
    {
        if (*asked != 2) return;
        if (ctx.expect (host.modalText().rfind ("Save changes before quitting?", 0) == 0,
                        "Quit showed '" + host.modalText() + "' rather than its prompt"))
            ctx.expect (host.clickModalButton ("Save"), "the quit prompt did not offer Save");
    } });
    steps->push_back ({ 700, [askedWhere, asked]
    { if (*asked == 2) askedWhere ("the quit prompt's Save"); } });

    const auto opened = ctx.tempDir() / "Opened";
    const auto openedJson = opened / "session.json";
    const float onDisk = launchFader - 9.0f;
    const float edited = launchFader - 12.0f;
    steps->push_back ({ 600, [&host, &ctx, &engine, &session, &fader, untitled, dirtied, opened, openedJson, onDisk, edited]
    {
        ctx.expect (host.modalStackEmpty(), "Cancel left '" + host.modalText() + "' up");
        ctx.expect (engine.isAudioCallbackRegistered() && ! host.engineDetached() && host.autosaveRunning(),
                    "backing out of the quit prompt's Save As left audio or autosave parked");
        ctx.expect (currentSessionDirectory (session) == untitled && nearly (fader.load(), dirtied),
                    "backing out of Save As did not leave the session as it was");
        fs::create_directories (opened / "audio");
        fader.store (onDisk);
        if (! ctx.expect (SessionSerializer::save (session, openedJson), "could not write the session to open"))
            return;
        reopenSavedSession (host, openedJson);
        ctx.expect (currentSessionDirectory (session) == opened, "the saved session did not open");
        fader.store (edited);
        ctx.expect (host.clickFileMenu(), "the File menu is unavailable");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Save"), "the File menu has no Save"); } });
    steps->push_back ({ 700, [&host, &ctx, openedJson, edited]
    {
        ctx.expect (host.modalStackEmpty(), "File > Save on an opened session showed '" + host.modalText() + "'");
        ctx.expect (nearly (savedFaderOf (openedJson), edited), "File > Save did not save the opened session in place");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar freshSessionSaveAsks { Scenario {
    "gui.fresh_session_save_asks_where", { "gui", "session" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runFreshSessionSaveAsks (host, ctx); }
} };

// A real session can be saved as Untitled. A never-saved session whose folder
// holds one keeps its autosave and notes in a private folder, where the
// autosave holds its latest edit; Don't Save and Save As discard that autosave
// rather than the other session's; Save As into that folder is refused, and
// Save As elsewhere takes the notes along but not the other session's plug-in
// state. A new session starts beside it in Untitled 2, so its takes and
// autosave land there, and Save As takes the take along. The session saved as
// Untitled never changes.
std::optional<ScenarioResult> runUnsavedSessionSparesUntitled (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty()
        || ! engine.isAudioCallbackRegistered() || ! host.autosaveRunning() || host.engineDetached())
        return ScenarioResult::skip ("requires a stopped transport, no modal, and live audio and autosave");
    if (host.sessionOnDisk()) return ScenarioResult::skip ("requires the launch session never to have been saved");
    if (session.deviceCaptureChannels.load() <= 0)
        return ScenarioResult::skip ("requires an input device with capture channels");
    const auto config = dusk::fs::appConfigDir();
    if (config.empty()) return ScenarioResult::skip ("requires a configuration folder");
    const auto privateDir = config / "unsaved-session";
    const auto privateAutosave = privateDir / "session.json.autosave";
    std::error_code error;
    fs::remove_all (privateDir, error);
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    ctx.cleanup ([&host, &engine, &session, &transport, originalDir, restore, privateDir,
                  loop = transport.isLoopEnabled(), loopStart = transport.getLoopStart(), loopEnd = transport.getLoopEnd(),
                  punch = transport.isPunchEnabled(), punchIn = transport.getPunchIn(), punchOut = transport.getPunchOut()]
    {
        engine.stop();
        session.setTrackArmed (0, false);
        drainModals (host);
        engine.reattachAudioCallback();
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
        transport.setLoopRange (loopStart, loopEnd);
        transport.setLoopEnabled (loop);
        transport.setPunchRange (punchIn, punchOut);
        transport.setPunchEnabled (punch);
        std::error_code ignored;
        fs::remove_all (privateDir, ignored);
    });

    transport.setLoopEnabled (false);
    transport.setPunchEnabled (false);

    const auto untitled = ctx.tempDir() / "Untitled";
    fs::create_directories (untitled / "audio");
    fs::create_directories (untitled / "state" / "lv2" / "track01");
    auto& fader = session.track (0).strip.faderDb;
    const float launchFader = fader.load();
    fader.store (launchFader - 6.0f);
    bool wrote = SessionSerializer::save (session, untitled / "session.json");
    fader.store (launchFader - 9.0f);
    wrote = SessionSerializer::save (session, untitled / "session.json.autosave") && wrote;
    fader.store (launchFader);
    std::ofstream (untitled / "notepad.md") << "Notes of the session saved as Untitled\n";
    std::ofstream (untitled / "state" / "lv2" / "track01" / "state.ttl") << "# its plug-in state\n";
    const auto theirTake = untitled / "audio" / "Their take.wav";
    std::ofstream (theirTake, std::ios::binary) << std::string (4096, 't');
    if (! wrote || ! fs::exists (untitled / "state" / "lv2" / "track01" / "state.ttl") || ! fs::exists (theirTake))
        return ScenarioResult::fail ("could not write the Untitled session");
    const auto contents = [untitled]
    {
        std::vector<std::string> files;
        std::error_code ignored;
        for (const auto& name : folderListing (untitled))
            files.push_back (name + "\n"
                             + (fs::is_regular_file (untitled / name, ignored) ? fileBytes (untitled / name) : ""));
        return files;
    };
    const auto before = contents();
    const auto untouched = [&ctx, contents, before] (const std::string& how)
    { ctx.expect (contents() == before, how + " changed the files of the session saved as Untitled"); };

    applySessionDirectory (session, untitled);
    const float edited = launchFader - 3.0f;
    fader.store (edited);
    host.autosaveTick();
    untouched ("An autosave tick");
    ctx.expect (nearly (savedFaderOf (privateAutosave), edited), "the never-saved session's autosave does not hold its edit");
    const std::string notes = "Notes of the launch session\n";
    ctx.expect (host.closeNotepadAfterTyping (notes), "closing the notepad did not save the notes");
    untouched ("Closing the notepad");
    ctx.expect (fileBytes (privateDir / "notepad.md") == notes, "the never-saved session's notes were not kept");

    const auto saveAs = [&host, &ctx] (const std::string& how, const fs::path& target)
    {
        if (! ctx.expect (host.modalText().rfind ("Save session as...", 0) == 0,
                          how + " showed '" + host.modalText() + "' rather than Save As")
            || ! ctx.expect (host.focusFileName(), how + ": the Save As browser has no name field"))
            return;
        typeReplacing (host, target.string());
        ctx.expect (host.clickModalButton ("Save"), how + ": the Save As browser has no Save button");
    };
    const float editedAgain = launchFader - 4.0f;
    const auto saved = ctx.tempDir() / "Saved";

    auto steps = std::make_shared<std::vector<Step>>();
    // Every take in the folder belongs to the other session, so Clean out
    // would offer to delete them all.
    steps->push_back ({ 200, [&host, &ctx] { ctx.expect (host.clickFileMenu(), "the File menu is unavailable"); } });
    steps->push_back ({ 200, [&host, &ctx]
    {
        ctx.expect (host.clickContextMenuItem ("Clean out unreferenced files..."),
                    "the File menu has no Clean out unreferenced files...");
    } });
    steps->push_back ({ 400, [&host, &ctx, untouched, theirTake]
    {
        const auto text = host.modalText();
        if (! host.confirmationText().empty())
        {
            ctx.expect (false, "Clean out offered to delete the other session's audio: '" + text + "'");
            host.clickModalButton ("Cancel");
        }
        else
        {
            ctx.expect (text == "Clean out\nSave this session before cleaning out. Another session has since been "
                                "saved in the folder this one records into, and Clean out would count that "
                                "session's recordings as unreferenced and delete them.",
                        "Clean out in the other session's folder showed '" + text + "'");
            ctx.expect (host.clickModalButton ("OK"), "the Clean out refusal has no OK button");
        }
        std::error_code ignored;
        ctx.expect (fs::exists (theirTake, ignored), "Clean out deleted the other session's take");
        untouched ("Clean out");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.modalStackEmpty(), "Clean out left '" + host.modalText() + "' up");
        ctx.expect (host.clickFileMenu(), "the File menu is unavailable");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Open..."), "the File menu has no Open..."); } });
    steps->push_back ({ 600, [&host, &ctx]
    {
        ctx.expect (host.modalText().rfind ("Save changes before opening another session?", 0) == 0,
                    "File > Open over unsaved changes showed '" + host.modalText() + "'");
        ctx.expect (host.clickModalButton ("Don't Save"), "the unsaved-changes prompt did not offer Don't Save");
    } });
    steps->push_back ({ 700, [&host, &ctx, untouched, privateAutosave]
    {
        ctx.expect (host.modalText().rfind ("Open session.json", 0) == 0,
                    "Don't Save showed '" + host.modalText() + "' rather than the Open browser");
        untouched ("Don't Save before File > Open");
        std::error_code ignored;
        ctx.expect (! fs::exists (privateAutosave, ignored), "Don't Save kept the never-saved session's autosave");
        ctx.expect (host.clickModalButton ("Cancel"), "the Open browser has no Cancel");
    } });
    steps->push_back ({ 400, [&host, &ctx, &session, &fader, untouched, untitled, editedAgain, privateAutosave]
    {
        ctx.expect (host.modalStackEmpty(), "Cancel left '" + host.modalText() + "' up");
        ctx.expect (currentSessionDirectory (session) == untitled, "backing out of File > Open moved the session");
        fader.store (editedAgain);
        host.autosaveTick();
        untouched ("A second autosave tick");
        ctx.expect (nearly (savedFaderOf (privateAutosave), editedAgain),
                    "the next autosave after Don't Save does not hold the edit");
        ctx.expect (host.clickFileMenu(), "the File menu is unavailable");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Save as..."), "the File menu has no Save as..."); } });
    steps->push_back ({ 700, [saveAs, untitled] { saveAs ("Save As into the launch folder", untitled); } });
    steps->push_back ({ 700, [&host, &ctx, untouched]
    {
        ctx.expect (host.modalText().rfind ("Folder holds another session\n", 0) == 0,
                    "Save As into the launch folder showed '" + host.modalText() + "' rather than the refusal");
        untouched ("Save As into the launch folder");
        ctx.expect (host.clickModalButton ("OK"), "the refusal has no OK button");
    } });
    steps->push_back ({ 400, [&host, &ctx, &session, untitled]
    {
        ctx.expect (host.modalStackEmpty(), "OK left '" + host.modalText() + "' up");
        ctx.expect (currentSessionDirectory (session) == untitled, "a refused Save As moved the session");
        ctx.expect (host.clickFileMenu(), "the File menu is unavailable");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Save as..."), "the File menu has no Save as..."); } });
    steps->push_back ({ 700, [saveAs, saved] { saveAs ("Save As from the never-saved session", saved); } });
    steps->push_back ({ 800, [&host, &ctx, &session, untouched, saved, notes, editedAgain, privateAutosave]
    {
        ctx.expect (host.modalStackEmpty(), "Save As showed '" + host.modalText() + "'");
        ctx.expect (currentSessionDirectory (session) == saved, "Save As did not move the session to its new folder");
        untouched ("Save As from the never-saved session");
        ctx.expect (nearly (savedFaderOf (saved / "session.json"), editedAgain), "Save As did not save the session");
        ctx.expect (fileBytes (saved / "notepad.md") == notes, "Save As did not take the notes along");
        std::error_code ignored;
        ctx.expect (! fs::exists (saved / "state", ignored), "Save As carried the other session's plug-in state");
        ctx.expect (! fs::exists (privateAutosave, ignored), "Save As left the never-saved session's autosave behind");
    } });

    const auto fresh = ctx.tempDir() / "Untitled 2";
    const auto taken = ctx.tempDir() / "Taken";
    const auto takeFile = std::make_shared<fs::path>();
    const auto regionFile = [&session]
    {
        const auto& regions = session.track (0).regions;
        return regions.size() == 1 ? fs::u8path (regions.front().file.getFullPathName().toStdString())
                                   : fs::path();
    };
    steps->push_back ({ 300, [&host, &ctx, &engine, &session, fresh]
    {
        host.startUnsavedSessionIn (ctx.tempDir());
        ctx.expect (currentSessionDirectory (session) == fresh,
                    "a new session started in '" + currentSessionDirectory (session).string()
                        + "' rather than beside the session saved as Untitled");
        auto& track = session.track (0);
        track.mode.store ((int) Track::Mode::Mono);
        track.inputSource.store (-2);
        session.setTrackArmed (0, true);
        engine.getTransport().setPlayhead (0);
        engine.record();
        ctx.expect (engine.getTransport().isRecording(), "the take did not start");
    } });
    steps->push_back ({ 600, [&engine] { engine.stop(); } });
    steps->push_back ({ 400, [&host, &ctx, &session, untouched, fresh, takeFile, regionFile]
    {
        session.setTrackArmed (0, false);
        *takeFile = regionFile();
        ctx.expect (! takeFile->empty(), "the take left no region");
        untouched ("Recording in the new session");
        ctx.expect (takeFile->parent_path() == fresh / "audio", "the take was written to '" + takeFile->string() + "'");
        host.autosaveTick();
        std::error_code ignored;
        ctx.expect (fs::exists (fresh / "session.json.autosave", ignored), "the new session's autosave is not beside it");
        untouched ("The new session's autosave");
        ctx.expect (host.clickFileMenu(), "the File menu is unavailable");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Save as..."), "the File menu has no Save as..."); } });
    steps->push_back ({ 700, [saveAs, taken] { saveAs ("Save As of the recorded session", taken); } });
    steps->push_back ({ 800, [&host, &ctx, &session, untouched, fresh, taken, takeFile, regionFile]
    {
        ctx.expect (host.modalStackEmpty(), "Save As of the recorded session showed '" + host.modalText() + "'");
        ctx.expect (currentSessionDirectory (session) == taken, "Save As did not move the recorded session");
        untouched ("Save As of the recorded session");
        const auto moved = regionFile();
        ctx.expect (! takeFile->empty() && moved == taken / "audio" / takeFile->filename(),
                    "Save As left the take at '" + moved.string() + "'");
        const auto reader = dusk::audio::FileReader::open (moved);
        ctx.expect (reader != nullptr && reader->info().numFrames > 0, "the saved take does not open as audio");
        Session reopened;
        applySessionDirectory (reopened, taken);
        ctx.expect (SessionSerializer::load (reopened, taken / "session.json")
                        && reopened.track (0).regions.size() == 1
                        && reopened.missingAudioFilesAfterLoad.empty(),
                    "the saved session does not find its take");
        std::error_code ignored;
        ctx.expect (! fs::exists (fresh / "session.json.autosave", ignored), "Save As left the new session's autosave behind");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar unsavedSessionSparesUntitled { Scenario {
    "gui.unsaved_session_spares_untitled", { "gui", "session", "autosave", "recording" }, Needs::Engine | Needs::Gui,
    {}, {}, 40000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runUnsavedSessionSparesUntitled (host, ctx); }
} };

// A folder the case may lock: false when locking it leaves it listable, as it
// does for root.
bool permissionsBite (const std::filesystem::path& probe)
{
    namespace fs = std::filesystem;
    std::error_code error;
    fs::create_directories (probe, error);
    fs::permissions (probe, fs::perms::none, fs::perm_options::replace, error);
    std::error_code listing;
    fs::directory_iterator it (probe, listing);
    fs::permissions (probe, fs::perms::owner_all, fs::perm_options::replace, error);
    return ! error && static_cast<bool> (listing);
}

// File > Clean out unreferenced files... in each state the manual lists: no
// audio folder, one it cannot read, nothing to clean, and stray takes. Cancel
// keeps the strays; Delete removes only the unreferenced .wav files in the
// audio folder and clears the undo history.
std::optional<ScenarioResult> runCleanOutAlerts (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    const auto rate = engine.getCurrentSampleRate();
    if (rate <= 0.0) return ScenarioResult::skip ("requires a positive engine sample rate");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    const auto folder = ctx.tempDir() / "Clean session";
    const auto audio = folder / "audio";
    ctx.cleanup ([&host, &engine, &session, originalDir, restore, audio]
    {
        std::error_code ignored;
        std::filesystem::permissions (audio, std::filesystem::perms::owner_all,
                                      std::filesystem::perm_options::replace, ignored);
        drainModals (host);
        engine.getUndoManager().clearUndoHistory();
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });
    fs::create_directories (folder);
    if (! SessionSerializer::save (*std::make_unique<Session>(), folder / "session.json"))
        return ScenarioResult::fail ("could not write the empty session");
    reopenSavedSession (host, folder / "session.json");
    if (currentSessionDirectory (session) != folder) return ScenarioResult::fail ("could not open the empty session");
    const bool canLock = permissionsBite (ctx.tempDir() / "probe");
    if (! canLock) ctx.note ("an unreadable folder stays listable here, as it does for root; that leg is skipped");

    const auto take = audio / "Take.wav";
    const std::vector<fs::path> strays { audio / "Stray 1.wav", audio / "Stray 2.wav" };
    const std::vector<fs::path> kept { take, audio / "Notes.txt", audio / "Freeze" / "Frozen.wav" };

    auto steps = std::make_shared<std::vector<Step>>();
    const auto choose = [&host, &ctx, steps] (const std::string& how, std::function<void()> before)
    {
        steps->push_back ({ 300, [&host, &ctx, how, before]
        {
            if (before) before();
            ctx.expect (host.modalStackEmpty(), how + ": a modal was still open");
            ctx.expect (host.clickFileMenu(), how + ": the File menu is unavailable");
        } });
        steps->push_back ({ 200, [&host, &ctx, how]
        {
            ctx.expect (host.clickContextMenuItem ("Clean out unreferenced files..."),
                        how + ": the File menu has no Clean out unreferenced files...");
        } });
    };
    const auto alerted = [&host, &ctx, steps] (const std::string& how, const std::string& body)
    {
        steps->push_back ({ 400, [&host, &ctx, how, body]
        {
            ctx.expect (host.modalText() == "Clean out\n" + body, how + ": the alert read '" + host.modalText() + "'");
            ctx.expect (host.clickModalButton ("OK"), how + ": the alert has no OK button");
        } });
    };

    choose ("with no audio folder", [audio]
    {
        std::error_code ignored;
        std::filesystem::remove_all (audio, ignored);
    });
    alerted ("with no audio folder", "This session has no audio directory yet, so there's nothing to clean.");
    if (canLock)
    {
        choose ("with an unreadable audio folder", [audio]
        {
            std::error_code ignored;
            std::filesystem::create_directories (audio, ignored);
            std::filesystem::permissions (audio, std::filesystem::perms::none,
                                          std::filesystem::perm_options::replace, ignored);
        });
        alerted ("with an unreadable audio folder",
                 "Could not read this session's audio directory, so there is no telling what is unreferenced. "
                 "Check the folder's permissions and that its drive is still connected, then try again.");
    }
    choose ("with nothing to clean", [&ctx, &session, audio, take, rate]
    {
        std::error_code ignored;
        std::filesystem::permissions (audio, std::filesystem::perms::owner_all,
                                      std::filesystem::perm_options::replace, ignored);
        std::filesystem::create_directories (audio / "Freeze", ignored);
        ctx.expect (writeRampFixture (take, rate, (std::int64_t) (rate / 2.0))
                        && writeBytes (audio / "Notes.txt", 64, 'n')
                        && writeBytes (audio / "Freeze" / "Frozen.wav", 4096, 'f'),
                    "could not write the audio folder's files");
        AudioRegion region;
        region.lengthInSamples = (std::int64_t) (rate / 2.0);
        region.file = decltype (region.file) (take.u8string().c_str());
        session.track (0).regions = { region };
    });
    alerted ("with nothing to clean", "No unreferenced files found. The audio directory is already clean.");

    const std::string confirm =
        "Found 2 unreferenced .wav file(s) totalling 1.5 MB.\n\nThese were created by past record passes that no "
        "longer have any region or take pointing at them. Deleting cannot be undone, and the session's undo "
        "history will be cleared (undone edits may still reference these files).";
    const auto confirmed = [&host, &ctx, steps, confirm] (const std::string& how, const std::string& button)
    {
        steps->push_back ({ 400, [&host, &ctx, confirm, how, button]
        {
            const auto text = host.confirmationText();
            ctx.expect (text == std::vector<std::string> { "Clean out unreferenced files", confirm },
                        how + ": the confirmation read '" + joinedLines (text) + "'");
            ctx.expect (host.clickModalButton (button), how + ": the confirmation has no " + button + " button");
        } });
    };
    const auto allExist = [] (const std::vector<fs::path>& files)
    {
        return std::all_of (files.begin(), files.end(), [] (const fs::path& file) { return fs::exists (file); });
    };
    choose ("with stray takes", [&ctx, &engine, &session, strays]
    {
        for (const auto& stray : strays)
            ctx.expect (writeBytes (stray, 786432, 's'), "could not write " + stray.filename().string());
        if (! ctx.expect (session.track (0).regions.size() == 1, "the take's region is missing")) return;
        const auto before = session.track (0).regions.front();
        auto after = before;
        after.gainDb = -1.0f;
        engine.getUndoManager().perform (new RegionEditAction (session, engine, 0, 0, before, after));
        ctx.expect (engine.getUndoManager().canUndo(), "the region edit left nothing to undo");
    });
    confirmed ("Cancel", "Cancel");
    steps->push_back ({ 300, [&host, &ctx, &engine, strays, kept, allExist]
    {
        ctx.expect (host.modalStackEmpty(), "Cancel left '" + host.modalText() + "' up");
        ctx.expect (allExist (strays) && allExist (kept), "Cancel deleted a file");
        ctx.expect (engine.getUndoManager().canUndo(), "Cancel cleared the undo history");
    } });
    choose ("Delete", {});
    confirmed ("Delete", "Delete");
    steps->push_back ({ 400, [&host, &ctx, &engine, &session, strays, kept, allExist, take]
    {
        ctx.expect (host.modalStackEmpty(), "Delete left '" + host.modalText() + "' up");
        for (const auto& stray : strays)
            ctx.expect (! fs::exists (stray), "Delete kept the unreferenced " + stray.filename().string());
        ctx.expect (allExist (kept), "Delete removed the referenced take, a non-WAV file or a file in a subfolder");
        ctx.expect (session.track (0).regions.size() == 1
                        && fs::u8path (session.track (0).regions.front().file.getFullPathName().toStdString()) == take,
                    "Delete changed the region that points at the take");
        ctx.expect (! engine.getUndoManager().canUndo(), "Delete left the undo history in place");
        ctx.expect (host.statusMessage() == "Deleted 2 unreferenced file(s).",
                    "the status bar says '" + host.statusMessage() + "'");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar cleanOutAlerts { Scenario {
    "gui.clean_out_alerts", { "gui", "session", "messages" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runCleanOutAlerts (host, ctx); }
} };

// A take's WAV has no region pointing at it until Stop, so Clean out must not
// run while one is open: not when chosen mid-take, and not when a control
// surface starts a take while the confirmation is up and Delete comes after.
// Each take still commits with its file in place, and once the transport is
// stopped Clean out removes only the stray.
std::optional<ScenarioResult> runCleanOutWhileRecording (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    if (! transport.isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    if (! engine.isAudioCallbackRegistered() || session.deviceCaptureChannels.load() <= 0)
        return ScenarioResult::skip ("requires a running audio device with an input to record from");
    auto& track = session.track (0);
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Recording);
    ctx.keep (track.mode);
    ctx.keep (track.inputSource);
    ctx.keep (session.countInEnabled);
    for (int index = 0; index < Session::kNumTracks; ++index)
    {
        const bool armed = session.track (index).recordArmed.load();
        ctx.cleanup ([&session, index, armed] { session.setTrackArmed (index, armed); });
        session.setTrackArmed (index, false);
    }
    ctx.cleanup ([&host, &engine, &session, &transport, &track, regions = track.regions, takes = track.takes,
                  originalDir = currentSessionDirectory (session), loop = transport.isLoopEnabled(),
                  punch = transport.isPunchEnabled(), at = transport.getPlayhead()]
    {
        drainModals (host);
        engine.stop();
        track.regions = regions;
        track.takes = takes;
        engine.getPlaybackEngine().preparePlayback();
        transport.setLoopEnabled (loop);
        transport.setPunchEnabled (punch);
        transport.setPlayhead (at);
        engine.getUndoManager().clearUndoHistory();
        applySessionDirectory (session, originalDir);
    });

    const auto audio = ctx.tempDir() / "Clean out take session" / "audio";
    applySessionDirectory (session, audio.parent_path());
    engine.stop();
    track.regions.clear();
    engine.getPlaybackEngine().preparePlayback();
    engine.getUndoManager().clearUndoHistory();
    transport.setPlayhead (0);
    transport.setLoopEnabled (false);
    transport.setPunchEnabled (false);
    session.countInEnabled.store (false);
    track.mode.store ((int) Track::Mode::Mono);
    track.inputSource.store (0);
    session.setTrackArmed (0, true);
    if (! track.recordArmed.load())
        return ScenarioResult::fail ("track 1 would not arm on input 1");
    std::error_code created;
    fs::create_directories (audio, created);
    const auto stray = audio / "Stray.wav";
    if (created || ! writeBytes (stray, 1048576, 's'))
        return ScenarioResult::fail ("could not write the stray take");

    const auto takes = [audio]
    {
        std::vector<fs::path> found;
        std::error_code error;
        for (fs::directory_iterator it (audio, error), end; ! error && it != end; it.increment (error))
            if (it->path().filename().u8string().rfind ("track01_", 0) == 0)
                found.push_back (it->path().lexically_normal());
        std::sort (found.begin(), found.end());
        return found;
    };
    const auto regionFiles = [&track]
    {
        std::vector<fs::path> files;
        const auto add = [&files] (const auto& file)
        { files.push_back (fs::u8path (file.getFullPathName().toStdString()).lexically_normal()); };
        for (const auto& region : track.regions)
            add (region.file);
        std::sort (files.begin(), files.end());
        files.erase (std::unique (files.begin(), files.end()), files.end());
        return files;
    };
    const std::string refusal =
        "Clean out\nStop recording before cleaning out. The take being recorded has no region pointing at its "
        "file until you stop, so Clean out would count it as unreferenced and delete it.";

    auto steps = std::make_shared<std::vector<Step>>();
    const auto choose = [&host, &ctx, steps] (const std::string& how)
    {
        steps->push_back ({ 300, [&host, &ctx, how]
        {
            ctx.expect (host.modalStackEmpty(), how + ": a modal was still open");
            ctx.expect (host.clickFileMenu(), how + ": the File menu is unavailable");
        } });
        steps->push_back ({ 200, [&host, &ctx, how]
        {
            ctx.expect (host.clickContextMenuItem ("Clean out unreferenced files..."),
                        how + ": the File menu has no Clean out unreferenced files...");
        } });
    };
    // Without the guard the confirmation lists the take; pressing Delete on it,
    // as a user would, is what loses the take.
    const auto refused = [&host, &ctx, steps, refusal] (const std::string& how)
    {
        steps->push_back ({ 400, [&host, &ctx, how, refusal]
        {
            if (! host.confirmationText().empty())
            {
                ctx.expect (false, how + ": Clean out offered '" + joinedLines (host.confirmationText()) + "'");
                host.clickModalButton ("Delete");
                return;
            }
            ctx.expect (host.modalText() == refusal, how + ": the alert read '" + host.modalText() + "'");
            ctx.expect (host.clickModalButton ("OK"), how + ": the alert has no OK button");
        } });
    };

    steps->push_back ({ 100, [&engine, &ctx, &transport]
    {
        engine.record();
        ctx.expect (transport.isRecording(), "the armed track did not start recording");
    } });
    choose ("mid-take");
    refused ("mid-take");
    steps->push_back ({ 300, [&host, &ctx, &engine, &transport, takes, stray]
    {
        ctx.expect (host.modalStackEmpty(), "mid-take: '" + host.modalText() + "' is still up");
        ctx.expect (transport.isRecording(), "mid-take: Clean out stopped the take");
        ctx.expect (takes().size() == 1, "mid-take: the take's file is gone from the audio folder");
        ctx.expect (fs::exists (stray), "mid-take: Clean out deleted the stray");
        engine.stop();
    } });
    steps->push_back ({ 100, [&ctx, &transport, &track, takes, regionFiles]
    {
        ctx.expect (transport.isStopped(), "the first take did not stop");
        ctx.expect (track.regions.size() == 1 && regionFiles() == takes(),
                    "the first take did not commit a region on its own file");
    } });

    choose ("recording under the confirmation");
    steps->push_back ({ 400, [&host, &ctx, &engine, &transport]
    {
        const auto text = host.confirmationText();
        ctx.expect (text.size() == 2 && text[0] == "Clean out unreferenced files"
                        && text[1].rfind ("Found 1 unreferenced .wav file(s) totalling 1.0 MB.", 0) == 0,
                    "stopped: the confirmation read '" + joinedLines (text) + "'");
        engine.record();
        ctx.expect (transport.isRecording(), "a take would not start under the confirmation");
        ctx.expect (host.clickModalButton ("Delete"), "the confirmation has no Delete button");
    } });
    refused ("recording under the confirmation");
    steps->push_back ({ 300, [&host, &ctx, &engine, &transport, takes, stray]
    {
        ctx.expect (host.modalStackEmpty(), "under the confirmation: '" + host.modalText() + "' is still up");
        ctx.expect (transport.isRecording(), "under the confirmation: Delete stopped the take");
        ctx.expect (takes().size() == 2, "under the confirmation: a take's file is gone from the audio folder");
        ctx.expect (fs::exists (stray), "under the confirmation: Delete went ahead while recording");
        ctx.expect (engine.getUndoManager().canUndo(), "under the confirmation: Delete cleared the undo history");
        engine.stop();
    } });
    steps->push_back ({ 100, [&ctx, &transport, takes, regionFiles]
    {
        ctx.expect (transport.isStopped(), "the second take did not stop");
        ctx.expect (regionFiles() == takes(), "the takes' regions do not point at their two files");
    } });

    choose ("stopped");
    steps->push_back ({ 400, [&host, &ctx]
    {
        ctx.expect (host.clickModalButton ("Delete"), "stopped: the confirmation has no Delete button");
    } });
    steps->push_back ({ 400, [&host, &ctx, takes, regionFiles, stray]
    {
        ctx.expect (host.modalStackEmpty(), "stopped: '" + host.modalText() + "' is still up");
        ctx.expect (! fs::exists (stray), "stopped: Delete kept the stray");
        ctx.expect (takes().size() == 2 && regionFiles() == takes(), "stopped: Delete removed a take");
        ctx.expect (host.statusMessage() == "Deleted 1 unreferenced file(s).",
                    "stopped: the status bar says '" + host.statusMessage() + "'");
    } });

    // A stop that finds an audio-thread call still inside the recorder waits
    // for it to leave, however long that takes, and commits the take on its
    // own file, so Clean out still has nothing to offer.
    steps->push_back ({ 100, [&ctx, &engine, &transport]
    {
        engine.record();
        ctx.expect (transport.isRecording(), "the third take did not start");
    } });
    steps->push_back ({ 300, [&ctx, &engine, &transport, &track, takes, regionFiles]
    {
        struct HeldCall
        {
            RecordManager& recorder;
            int leaveAfterPasses = 0;
            int passes = 0;
            static void pass (void* context)
            {
                auto& self = *static_cast<HeldCall*> (context);
                if (++self.passes == self.leaveAfterPasses)
                    self.recorder.holdAudioInFlightForTest (false);
            }
        };
        auto& recorder = engine.getRecordManager();
        HeldCall held { recorder, 5000 };
        recorder.holdAudioInFlightForTest (true);
        recorder.setAudioWaitObserverForTest (&held, &HeldCall::pass);
        engine.stop();
        recorder.setAudioWaitObserverForTest (nullptr, nullptr);
        if (held.passes < held.leaveAfterPasses)
            recorder.holdAudioInFlightForTest (false);
        ctx.expect (held.passes == held.leaveAfterPasses,
                    "the stop gave up on the audio thread after " + std::to_string (held.passes) + " passes");
        ctx.expect (transport.isStopped(), "the stop that waited left the transport rolling");
        ctx.expect (track.regions.size() == 3 && takes().size() == 3 && regionFiles() == takes(),
                    "the stop that waited for the audio thread did not commit its take");
    } });
    choose ("after a stop that waited");
    steps->push_back ({ 400, [&host, &ctx]
    {
        if (! host.confirmationText().empty())
        {
            ctx.expect (false, "after a stop that waited: Clean out offered '"
                                   + joinedLines (host.confirmationText()) + "'");
            host.clickModalButton ("Cancel");
            return;
        }
        ctx.expect (host.modalText() == "Clean out\nNo unreferenced files found. The audio directory is already clean.",
                    "after a stop that waited: the alert read '" + host.modalText() + "'");
        ctx.expect (host.clickModalButton ("OK"), "after a stop that waited: the alert has no OK button");
    } });
    steps->push_back ({ 300, [&host, &ctx, takes, regionFiles]
    {
        ctx.expect (host.modalStackEmpty(), "after a stop that waited: '" + host.modalText() + "' is still up");
        ctx.expect (takes().size() == 3 && regionFiles() == takes(),
                    "after a stop that waited: Clean out removed a take");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar cleanOutWhileRecording { Scenario {
    "gui.clean_out_while_recording", { "gui", "session", "recording", "messages" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runCleanOutWhileRecording (host, ctx); }
} };

std::optional<ScenarioResult> runOopCaptureFailureSave (GuiHost& host, ScenarioContext& ctx,
                                                       bool saveAs)
{
#if DUSKSTUDIO_HAS_OOP_PLUGINS
    namespace fs = std::filesystem;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& manager = engine.getPluginManager();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty()
        || ! engine.isAudioCallbackRegistered() || engine.getCurrentSampleRate() <= 0.0)
        return ScenarioResult::skip ("requires stopped live audio and no modal");
    const auto fixture = ctx.fixture ("crash_state.vst3");
    if (! fixture) return ScenarioResult::fail ("the state fixture is unavailable");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    engine.publishPluginStateForSave (true);
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not preserve the initial session");
    const bool wasOop = manager.isOopEnabled();
    ctx.cleanup ([&host, &engine, &session, &manager, wasOop, originalDir, restore]
    {
        engine.stop();
        drainModals (host);
        engine.getChannelStrip (0).getPluginSlot().unload();
        manager.setOopEnabled (wasOop);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });

    const auto folder = ctx.tempDir() / "OOP capture session";
    const auto copy = ctx.tempDir() / "OOP capture copy";
    const auto tone = folder / "audio" / "source.wav";
    fs::create_directories (tone.parent_path());
    {
        dusk::audio::WriteSpec spec;
        spec.sampleRate = engine.getCurrentSampleRate();
        spec.numChannels = 1;
        auto writer = dusk::audio::FileWriter::create (tone, spec);
        const std::vector<float> samples (256, 0.25f);
        const float* input[] { samples.data() };
        if (! writer || ! writer->write (input, 1, 256) || ! writer->flush())
            return ScenarioResult::fail ("could not create the source audio");
    }
    {
        auto saved = std::make_unique<Session>();
        applySessionDirectory (*saved, folder);
        AudioRegion region;
        using File = std::decay_t<decltype (region.file)>;
        region.file = File (tone.u8string().c_str());
        region.lengthInSamples = 256;
        saved->track (1).regions = { region };
        saved->track (1).mode.store ((int) Track::Mode::Mono);
        if (! SessionSerializer::save (*saved, folder / "session.json"))
            return ScenarioResult::fail ("could not create the source session");
    }
    if (! dusk::fs::writeStringToFile (folder / "notepad.md", "saved notes\n"))
        return ScenarioResult::fail ("could not create the source notepad");
    fs::create_directories (folder / "state" / "lv2" / "sentinel");
    dusk::fs::writeStringToFile (folder / "state" / "lv2" / "sentinel" / "bank.bin", "keep source state");
    fs::create_directories (copy / "audio");
    dusk::fs::writeStringToFile (copy / "audio" / "existing.wav", "keep target media");
    dusk::fs::writeStringToFile (copy / "notepad.md", "keep target notes\n");
    reopenSavedSession (host, folder / "session.json");

    using File = std::decay_t<decltype (session.getSessionDirectory())>;
    using Text = std::decay_t<decltype (session.getSessionDirectory().getFullPathName())>;
    Text loadError;
    auto probe = manager.createPluginInstance (File (fixture->u8string().c_str()),
        engine.getCurrentSampleRate(), ScenarioContext::kBlockSize, loadError);
    if (! probe) return ScenarioResult::fail ("could not inspect the state fixture: " + loadError.toStdString());
    struct State
    {
        bool done = false, loaded = false;
        std::string error, originalState;
        std::optional<PluginDescriptor> originalDescriptor;
        int largeIndex = -1;
        std::map<fs::path, std::string> sourceFiles, targetFiles;
    };
    auto state = std::make_shared<State>();
    auto descriptor = manager.descriptorForInstance (*probe);
    descriptor.location = fixture->u8string();
    const auto& parameters = probe->getParameters();
    for (int index = 0; index < parameters.size(); ++index)
        if (parameters[index]->getName (100) == "Large State") state->largeIndex = index;
    probe.reset();
    if (descriptor.backend != PluginBackend::JuceLegacy || state->largeIndex < 0)
        return ScenarioResult::fail ("the fixture lacks its legacy large-state control");
    manager.setOopEnabled (true);
    auto& slot = engine.getChannelStrip (0).getPluginSlot();
    slot.loadFromDescriptorAsync (descriptor, [state] (bool loaded, auto error)
    {
        state->loaded = loaded;
        state->error = error.toStdString();
        state->done = true;
    });
    const auto tree = [&ctx] (const fs::path& root)
    {
        std::map<fs::path, std::string> files;
        std::error_code error;
        for (fs::recursive_directory_iterator it (root, error), end;
             ! error && it != end; it.increment (error))
        {
            if (it->is_directory (error) && ! error)
                files.emplace (it->path().lexically_relative (root), "directory");
            else if (! error && it->is_regular_file (error) && ! error)
                files.emplace (it->path().lexically_relative (root),
                               "file:" + dusk::fs::loadFileAsString (it->path()));
        }
        ctx.expect (! error, "could not inspect fixture files: " + error.message());
        return files;
    };
    const auto stateApplied = [&engine, &slot] (bool large)
    {
        std::array<float, ScenarioContext::kBlockSize> left {}, right {};
        left.fill (0.25f); right.fill (0.25f);
        oopstub::SlotMidiBuffer midi;
        const int count = std::clamp (engine.getCurrentBlockSize(), 1, ScenarioContext::kBlockSize);
        engine.suspendProcessing();
        slot.processStereoBlock (left.data(), right.data(), count, midi);
        engine.resumeProcessing();
        return std::abs (left.front() - (large ? 0.125f : 0.25f)) < 1.0e-6f;
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&ctx, &engine, &slot, state]
    {
        ctx.expect (state->loaded && slot.isRemote(), "the actual fixture child did not load: " + state->error);
        engine.getChannelStrip (0).insertMode.store (ChannelStrip::kInsertPlugin);
    }, [state] { return state->done; }, "the fixture child did not finish loading" });
    if (! saveAs)
        for (int pass = 0; pass < 2; ++pass)
        {
            pushFileMenuSteps (host, ctx, *steps, "Save");
            steps->push_back ({ 0, [&host, &ctx, &session]
            {
                ctx.expect (host.modalStackEmpty() && host.statusMessage().rfind ("Saved:", 0) == 0,
                            "a healthy repeated Save failed: " + host.modalText());
                ctx.expect (session.track (0).pluginStateBase64.isNotEmpty(),
                            "a healthy child state was not saved");
            } });
        }
    if (! saveAs)
    {
        steps->push_back ({ 0, [&host, &ctx, &slot, state]
        {
            if (host.requestQuit())
            {
                ctx.expect (false, "the healthy saved child was dirty before an opaque edit");
                host.clickModalButton ("Cancel");
            }
            slot.setParamNormalised (state->largeIndex, 1.0f);
        } });
        steps->push_back ({ 0, [] {}, [stateApplied] { return stateApplied (true); }, "the opaque-only large-state edit did not reach the child" });
        pushFileMenuSteps (host, ctx, *steps, "Save");
        steps->push_back ({ 0, [&host, &ctx]
        {
            ctx.expect (host.modalText().find ("could not capture plug-in state") != std::string::npos,
                        "the opaque-only edit did not reach a capture-failure alert: " + host.modalText());
            drainModals (host);
        } });
        steps->push_back ({ 200, [&host, &ctx]
        {
            ctx.expect (host.requestQuit(), "an opaque-only failed Save did not ask to save before quitting");
            if (! host.modalStackEmpty())
                ctx.expect (host.modalText().find ("Save changes before quitting?") != std::string::npos,
                            "failed Save raised the wrong quit prompt: " + host.modalText());
        } });
        steps->push_back ({ 200, [&host, &ctx]
        {
            if (! host.modalStackEmpty())
                ctx.expect (host.clickModalButton ("Cancel"), "the failed-Save quit prompt could not be cancelled");
        } });
        steps->push_back ({ 200, [&host, &ctx]
        {
            ctx.expect (host.modalStackEmpty(), "Cancel left the failed-Save quit prompt open");
        } });
        steps->push_back ({ 0, [&slot, state]
        {
            slot.setParamNormalised (state->largeIndex, 0.0f);
        } });
        steps->push_back ({ 0, [] {}, [stateApplied] { return stateApplied (false); },
                            "the child did not return to a serializable state" });
        pushFileMenuSteps (host, ctx, *steps, "Save");
        steps->push_back ({ 0, [&host, &ctx]
        {
            ctx.expect (host.modalStackEmpty() && host.statusMessage().rfind ("Saved:", 0) == 0,
                        "a successful retry did not save after the child state shrank");
            if (host.requestQuit())
            {
                ctx.expect (false, "a successful Save retained the earlier capture-failure dirty state");
                host.clickModalButton ("Cancel");
            }
        } });
    }
    steps->push_back ({ 0, [&host, &ctx, &session, &slot, state, folder]
    {
        ctx.expect (host.closeNotepadAfterTyping ("notes pending in memory\n"),
                    "could not set up the in-memory notepad");
        ctx.expect (dusk::fs::writeStringToFile (folder / "notepad.md", "saved notes\n"),
                    "could not restore the canonical notepad sentinel");
        state->originalState = session.track (0).pluginStateBase64.toStdString();
        state->originalDescriptor = session.track (0).pluginDescriptor;
        slot.setParamNormalised (state->largeIndex, 1.0f);
    } });
    steps->push_back ({ 0, [] {}, [stateApplied] { return stateApplied (true); }, "the fixture child did not apply the large-state switch" });
    steps->push_back ({ 0, [&host, &ctx, &session, state, tree, folder, copy]
    {
        session.track (0).strip.faderDb.store (-6.0f);
        host.autosaveTick();
        const auto autosave = folder / "session.json.autosave";
        ctx.expect (fs::exists (autosave) && std::abs (savedFaderOf (autosave) + 6.0f) < 1.0e-6f,
                    "live cheap controls did not reach autosave with a large opaque state");
        state->sourceFiles = tree (folder);
        state->targetFiles = tree (copy);
    } });
    if (saveAs) pushSaveAsSteps (host, ctx, *steps, copy);
    else pushFileMenuSteps (host, ctx, *steps, "Save");
    steps->push_back ({ 0, [&host, &ctx, &engine, &session, &slot, state, tree, folder, copy]
    {
        const auto message = host.modalText();
        ctx.expect (message.find ("Save failed") != std::string::npos
                        && message.find ("state") != std::string::npos
                        && message.find ("Track 1") != std::string::npos
                        && message.find ("Dusk Crash State Fixture") != std::string::npos,
                    "Save did not explain the child state capture failure: " + message);
        ctx.expect (currentSessionDirectory (session) == folder, "failed Save As changed the session directory");
        ctx.expect (tree (folder) == state->sourceFiles, "failed Save changed canonical/audio/notepad/state/autosave files");
        ctx.expect (tree (copy) == state->targetFiles, "failed Save As changed existing target files or copied new files");
        ctx.expect (session.track (0).pluginStateBase64.toStdString() == state->originalState
                        && session.track (0).pluginDescriptor == state->originalDescriptor,
                    "failed capture replaced the original opaque state or identity");
        ctx.expect (session.track (1).regions.size() == 1
                        && session.track (1).regions.front().file.getFullPathName().toStdString()
                        == (folder / "audio" / "source.wav").u8string(),
                    "failed Save As repointed source audio");
        ctx.expect (! engine.isProcessingSuspended() && slot.isRemote(), "failed save left audio gated or discarded the child");
        ctx.expect (slot.getStateBase64ForSave (0).toStdString() == state->originalState,
                    "failed capture lost the child's last known state");
        const auto autosave = folder / "session.json.autosave";
        session.track (0).strip.faderDb.store (-9.0f);
        host.autosaveTick();
        ctx.expect (fs::exists (autosave) && std::abs (savedFaderOf (autosave) + 9.0f) < 1.0e-6f,
                    "failed Save stopped later control edits from reaching autosave");
        const auto recoveryBytes = dusk::fs::loadFileAsString (autosave);
        session.track (0).strip.faderDb.store (savedFaderOf (folder / "session.json"));
        host.autosaveTick();
        ctx.expect (fs::exists (autosave) && dusk::fs::loadFileAsString (autosave) == recoveryBytes,
                    "a matching control snapshot discarded recovery after a failed Save");
        drainModals (host);
    }, [&host, &session, folder, copy, saveAs]
    {
        return host.modalText().find ("Save failed") != std::string::npos
            || (saveAs ? currentSessionDirectory (session) == copy
                       : ! fs::exists (folder / "session.json.autosave"));
    }, "Save neither completed nor reported its capture failure" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
#else
    (void) host; (void) ctx; (void) saveAs;
    return ScenarioResult::skip ("sandboxed plug-ins are unavailable");
#endif
}

const ScenarioRegistrar oopCaptureFailureSave { Scenario {
    "gui.oop_capture_failure_save", { "gui", "session", "save", "ipc", "messages" }, Needs::Engine | Needs::Gui,
    { "crash_state.vst3" }, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runOopCaptureFailureSave (host, ctx, false); }
} };
const ScenarioRegistrar oopCaptureFailureSaveAs { Scenario {
    "gui.oop_capture_failure_save_as", { "gui", "session", "save", "ipc", "messages" }, Needs::Engine | Needs::Gui,
    { "crash_state.vst3" }, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runOopCaptureFailureSave (host, ctx, true); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
