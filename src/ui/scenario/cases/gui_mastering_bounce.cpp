#include "../GuiScenarioSupport.h"
#include "../../../engine/audiofile/FileWriter.h"
#include "../../../engine/audiofile/FileReader.h"
#include "../../../session/SessionSerializer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
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
// Descriptors this process holds on `file`; 0 where they can't be counted
// (only Linux lists them), so callers pair it with a rename check.
int openHandlesInProcess (const std::filesystem::path& file)
{
    int handles = 0;
   #if defined (__linux__)
    std::error_code ec;
    const auto target = std::filesystem::weakly_canonical (file, ec);
    if (ec) return 0;
    for (const auto& entry : std::filesystem::directory_iterator ("/proc/self/fd", ec))
    {
        std::error_code linkEc;
        if (std::filesystem::read_symlink (entry.path(), linkEc) == target && ! linkEc)
            ++handles;
    }
   #else
    (void) file;
   #endif
    return handles;
}

// Descriptors this process still holds on `file` after it was deleted, which
// Linux lists under the old path; 0 elsewhere.
int deletedHandlesInProcess (const std::filesystem::path& file)
{
    int handles = 0;
   #if defined (__linux__)
    std::error_code ec;
    const auto target = std::filesystem::weakly_canonical (file, ec).string() + " (deleted)";
    if (ec) return 0;
    for (const auto& entry : std::filesystem::directory_iterator ("/proc/self/fd", ec))
    {
        std::error_code linkEc;
        if (std::filesystem::read_symlink (entry.path(), linkEc).string() == target && ! linkEc)
            ++handles;
    }
   #else
    (void) file;
   #endif
    return handles;
}

// Windows refuses to rename a file this process still has open.
bool renameSucceeds (const std::filesystem::path& from, const std::filesystem::path& to)
{
    std::error_code ec;
    std::filesystem::rename (from, to, ec);
    return ! ec;
}

bool widenMasteringWindow (GuiHost& host, ScenarioContext& ctx)
{
    const auto size = host.mainWindowSize();
    if (size.size() < 2) return false;
    ctx.cleanup ([&host, size] { host.resizeMainWindow (size[0], size[1]); });
    return host.resizeMainWindow (std::max (1440, size[0]), std::max (900, size[1]));
}

std::optional<ScenarioResult> runMasteringExportWorkflow (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& player = engine.getMasteringPlayer();
    if (! engine.getTransport().isStopped() || player.isPlaying() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalStage = engine.getStage();
    const auto originalFile = player.getLoadedFile();
    const auto originalPosition = player.getPlayhead();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    ctx.cleanup ([&host, &engine, &session, &player, originalDir, originalStage, originalFile, originalPosition, restore]
    {
        engine.observeAudioRegistrationForScenario ({});
        drainModals (host);
        engine.setRenderOversamplingOverride (0);
        engine.reattachAudioCallback();
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
        if (originalFile.existsAsFile()) player.loadFile (originalFile);
        else player.unloadFile();
        player.setPlayhead (originalPosition);
        host.refreshMasteringSource();
        host.switchToStage (guiStage (originalStage));
    });
    const auto source = ctx.tempDir() / "mixdown.wav";
    const auto output = ctx.tempDir() / "Finished master.wav";
    dusk::audio::WriteSpec spec;
    spec.sampleRate = 48000;
    spec.numChannels = 2;
    auto writer = dusk::audio::FileWriter::create (source, spec);
    std::vector<float> signal (4800);
    for (size_t i = 0; i < signal.size(); ++i)
        signal[i] = 0.1f * (float) std::sin (6.283185307179586 * 440.0 * (double) i / 48000.0);
    const float* channels[] = { signal.data(), signal.data() };
    if (! writer || ! writer->write (channels, 2, 4800) || ! writer->flush())
        return ScenarioResult::fail ("could not write mixdown");
    writer.reset();
    applySessionDirectory (session, ctx.tempDir());
    host.switchToStage (GuiHost::Stage::Mastering);
    auto registration = std::make_shared<std::vector<bool>>();
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 400, [&host, &ctx]
    { ctx.expect (host.clickMasteringButton ("Load latest mixdown"), "latest mixdown button unavailable"); } });
    steps->push_back ({ 600, [&host, &ctx, &engine, &player, source, registration]
    {
        ctx.expect (player.isLoaded() && player.getLoadedFile().getFullPathName().toStdString() == source.string(),
                    "latest mixdown did not load the session mix");
        ctx.expect (engine.isAudioCallbackRegistered(), "audio callback was not registered before export");
        engine.observeAudioRegistrationForScenario ([registration] (bool attached) { registration->push_back (attached); });
        ctx.expect (host.clickMasteringButton ("Export master..."), "export button unavailable");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("WAV 24-bit \xe2\x80\x94 session rate"), "archive preset unavailable"); } });
    steps->push_back ({ 200, [&host, &ctx, output]
    {
        ctx.expect (host.focusFileName(), "export file browser unavailable");
       #if defined (__APPLE__)
        host.pressPeerKey ("command + A", 'a');
       #else
        host.pressPeerKey ("ctrl + A", 'a');
       #endif
        for (const char ch : output.string())
            host.pressPeerKey (ch == ' ' ? "Space" : std::string (1, ch), ch);
    } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Save"), "export destination was not accepted"); } });

    // Export again onto that file, named without its extension: the file it
    // would really write is asked about, Cancel keeps it and Replace exports
    // over it.
    const auto exportOverExisting = [&host, &ctx, output]
    {
        std::ofstream (output, std::ios::binary | std::ios::trunc) << "not a master";
        const auto before = fileBytes (output);
        const auto exportAs = [&host, &ctx, output] (std::vector<Step>& into)
        {
            into.push_back ({ 300, [&host, &ctx]
            { ctx.expect (host.clickMasteringButton ("Export master..."), "export button unavailable"); } });
            into.push_back ({ 200, [&host, &ctx]
            { ctx.expect (host.clickContextMenuItem ("WAV 24-bit \xe2\x80\x94 session rate"), "archive preset unavailable"); } });
            into.push_back ({ 200, [&host, &ctx, output]
            {
                ctx.expect (host.focusFileName(), "export file browser unavailable");
                typeReplacing (host, (output.parent_path() / output.stem()).string());
            } });
            into.push_back ({ 150, [&host, &ctx]
            { ctx.expect (host.clickModalButton ("Save"), "export destination was not accepted"); } });
        };
        const auto answer = [&host, &ctx] (const std::string& button)
        {
            ctx.expect (host.confirmationText() == std::vector<std::string> {
                            "Replace file?",
                            "This file already exists and will be replaced:\n\nFinished master.wav\n\nContinue?" },
                        "Export master onto an existing file did not ask first; top modal '" + host.modalText() + "'");
            ctx.expect (host.clickModalButton (button), "the replace prompt has no " + button + " button");
        };
        auto again = std::make_shared<std::vector<Step>>();
        exportAs (*again);
        again->push_back ({ 400, [answer] { answer ("Cancel"); } });
        again->push_back ({ 400, [&host, &ctx, output, before]
        {
            ctx.expect (host.modalStackEmpty(), "Cancel in the replace prompt left '" + host.modalText() + "' up");
            ctx.expect (fileBytes (output) == before, "Cancel in the replace prompt still exported");
        } });
        exportAs (*again);
        again->push_back ({ 400, [answer] { answer ("Replace"); } });
        runSteps (ctx, again, [&host, &ctx, output]
        {
            ctx.waitUntil ([&host] { return host.clickModalButton ("Close"); }, 20000, [&ctx, output]
            {
                auto reader = dusk::audio::FileReader::open (output);
                ctx.expect (reader != nullptr && reader->info().numFrames > 0, "Replace did not export over the file");
                ctx.complete (ctx.verdict());
            }, "the export after Replace did not finish");
        });
    };
    runSteps (ctx, steps, [&host, &ctx, &engine, &session, output, registration, exportOverExisting]
    {
        ctx.waitUntil ([&host] { return host.clickModalButton ("Close"); }, 20000,
            [&ctx, &engine, &session, output, registration, exportOverExisting]
            {
                ctx.expect (*registration == std::vector<bool> { false, true },
                            "export did not deregister and restore the live audio callback");
                ctx.expect (engine.isAudioCallbackRegistered(), "audio callback was not registered after export");
                engine.observeAudioRegistrationForScenario ({});
                auto reader = dusk::audio::FileReader::open (output);
                if (ctx.expect (reader != nullptr, "master export did not create a readable WAV"))
                {
                    const auto& info = reader->info();
                    ctx.expect (info.numChannels == 2 && info.bitsPerSample == 24, "export format is not stereo 24-bit WAV");
                    ctx.expect (std::abs (info.sampleRate - engine.getCurrentSampleRate()) < 0.01,
                                "archive export did not use session rate");
                    ctx.expect (info.numFrames == (std::int64_t) std::llround (info.sampleRate * 5.1),
                                "export length does not include source and tail");
                    std::vector<float> left (4096), right (4096);
                    float* channelsOut[] = { left.data(), right.data() };
                    ctx.expect (reader->read (channelsOut, 2, 0, 4096) == 4096, "could not read exported audio");
                    const auto peak = *std::max_element (left.begin(), left.end());
                    ctx.expect (peak > 0.001f && peak <= 1.0f, "export did not carry the loaded mix signal");
                }
                reader.reset();
                // The live re-prepare after the render must not clear what the
                // meter measured of the master it wrote. The page shows what the
                // live callbacks publish, so read it once a few have run.
                auto settled = std::make_shared<std::vector<Step>>();
                settled->push_back ({ 400, [&ctx, &engine, &session]
                {
                    const float integrated = session.mastering().meterIntegratedLufs.load();
                    const float truePeak   = session.mastering().meterTruePeakDb.load();
                    ctx.expect (integrated > -70.0f && truePeak > -70.0f,
                                "the finished export left the loudness readings at " + std::to_string (integrated)
                                    + " LUFS, " + std::to_string (truePeak) + " dBTP");
                    engine.refreshMidiInputs();
                    ctx.expect (engine.isAudioCallbackRegistered(), "MIDI refresh detached the live audio callback");
                } });
                settled->push_back ({ 400, [&ctx, &session]
                {
                    ctx.expect (session.mastering().meterIntegratedLufs.load() > -70.0f
                                    && session.mastering().meterTruePeakDb.load() > -70.0f,
                                "MIDI refresh cleared the finished export's loudness readings");
                } });
                runSteps (ctx, settled, exportOverExisting);
            }, "master export did not finish");
    });
    return std::nullopt;
}

const ScenarioRegistrar masteringExportWorkflow { Scenario {
    "gui.mastering_export_workflow", { "gui", "mastering" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringExportWorkflow (host, ctx); }
} };

// Export master reads the loaded mix while it writes the master, so a target
// that reaches the mix - by its name, without its extension, through a link,
// or in another case where the filesystem ignores case - is refused before the
// replace prompt and before any render starts. The mix keeps every byte and
// still plays.
std::optional<ScenarioResult> runMasterExportOntoMixRefused (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& player = engine.getMasteringPlayer();
    if (! engine.getTransport().isStopped() || player.isPlaying() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and mastering player and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    keepStage (host, ctx);
    ctx.cleanup ([&host, &session, &engine, &player, originalDir, restore]
    {
        engine.observeAudioRegistrationForScenario ({});
        drainModals (host);
        player.stop();
        engine.reattachAudioCallback();
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
        host.refreshMasteringSource();
    });

    const auto folder = ctx.tempDir() / "Mix session";
    const auto mix = folder / "mixdown.wav";
    fs::create_directories (folder);
    {
        auto writer = dusk::audio::FileWriter::create (mix, { 48000.0, 2, 24 });
        std::vector<float> tone (48000);
        for (size_t i = 0; i < tone.size(); ++i)
            tone[i] = 0.1f * (float) std::sin (6.283185307179586 * 440.0 * (double) i / 48000.0);
        const float* channels[] = { tone.data(), tone.data() };
        if (! writer || ! writer->write (channels, 2, (std::int64_t) tone.size()) || ! writer->flush())
            return ScenarioResult::fail ("could not write the mix");
    }
    std::error_code ec;
    const auto mixBytes = fileBytes (mix);
    const auto mixStamp = fs::last_write_time (mix, ec);

    struct Attempt { std::string typed, shownName; };
    std::vector<Attempt> attempts { { mix.string(), "mixdown.wav" },
                                    { (folder / "mixdown").string(), "mixdown.wav" } };
    const auto link = ctx.tempDir() / "link.wav";
    fs::create_symlink (mix, link, ec);
    if (! ec) attempts.push_back ({ link.string(), "link.wav" });
    if (fs::exists (folder / "MIXDOWN.wav", ec))
        attempts.push_back ({ (folder / "MIXDOWN.wav").string(), "MIXDOWN.wav" });

    applySessionDirectory (session, folder);
    host.switchToStage (GuiHost::Stage::Mastering);
    const auto onMix = [&player, mix]
    { return player.isLoaded() && player.getLoadedFile().getFullPathName().toStdString() == mix.string(); };
    auto registration = std::make_shared<std::vector<bool>>();

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 400, [&host, &ctx]
    { ctx.expect (host.clickMasteringButton ("Load latest mixdown"), "latest mixdown button unavailable"); } });
    steps->push_back ({ 600, [&ctx, &engine, onMix, registration]
    {
        ctx.expect (onMix(), "latest mixdown did not load the session mix");
        ctx.expect (engine.isAudioCallbackRegistered(), "audio callback was not registered before the export");
        engine.observeAudioRegistrationForScenario ([registration] (bool attached) { registration->push_back (attached); });
    } });
    for (const auto& attempt : attempts)
    {
        steps->push_back ({ 300, [&host, &ctx]
        { ctx.expect (host.clickMasteringButton ("Export master..."), "export button unavailable"); } });
        steps->push_back ({ 200, [&host, &ctx]
        { ctx.expect (host.clickContextMenuItem ("WAV 24-bit \xe2\x80\x94 session rate"), "archive preset unavailable"); } });
        steps->push_back ({ 200, [&host, &ctx, typed = attempt.typed]
        {
            ctx.expect (host.focusFileName(), "export file browser unavailable");
            typeReplacing (host, typed);
        } });
        steps->push_back ({ 150, [&host, &ctx]
        { ctx.expect (host.clickModalButton ("Save"), "export destination was not accepted"); } });
        steps->push_back ({ 300, [&host, &ctx, onMix, registration, mix, mixBytes, attempt]
        {
            const auto what = "Export master onto '" + attempt.typed + "'";
            const auto expected = "File is the loaded mix\nThis file is the mix the master is rendered from:\n\n    "
                                + attempt.shownName + "\n\nExporting over it would destroy the mix, so nothing was "
                                  "exported and nothing was changed. Choose another name for the master.";
            ctx.expect (host.modalText() == expected, what + " was not refused; top modal '" + host.modalText() + "'");
            ctx.expect (host.confirmationText().empty(), what + " asked to replace the mix");
            ctx.expect (! host.renderRunning() && registration->empty(), what + " started a render");
            ctx.expect (fileBytes (mix) == mixBytes, what + " changed the mix");
            ctx.expect (onMix(), what + " moved the mastering player off the mix");
            ctx.expect (host.clickModalButton (host.confirmationText().empty() ? "OK" : "Cancel"),
                        what + ": the top modal has no way out");
        } });
        steps->push_back ({ 150, [&host, &ctx, attempt]
        { ctx.expect (host.modalStackEmpty(), "OK after refusing '" + attempt.typed + "' left '" + host.modalText() + "' up"); } });
    }
    steps->push_back ({ 100, [&host, &ctx, mix, mixStamp]
    {
        std::error_code error;
        ctx.expect (fs::last_write_time (mix, error) == mixStamp, "the refused exports touched the mix");
        ctx.expect (host.clickMasteringButton ("Play"), "Play is unavailable");
    } });
    steps->push_back ({ 200, [&host, &ctx, &player, onMix]
    {
        ctx.expect (onMix() && player.isPlaying() && player.getPlayhead() > 0, "the mix does not play after the refusals");
        ctx.expect (host.clickMasteringButton ("Stop"), "Stop is unavailable");
    } });
    runSteps (ctx, steps, [&ctx, &player, registration]
    {
        ctx.expect (! player.isPlaying(), "Stop did not stop the mix");
        ctx.expect (registration->empty(), "a refused export detached the audio callback");
        ctx.complete (ctx.verdict());
    });
    return std::nullopt;
}

const ScenarioRegistrar masterExportOntoMixRefused { Scenario {
    "gui.master_export_onto_mix_refused", { "gui", "mastering" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasterExportOntoMixRefused (host, ctx); }
} };

std::optional<ScenarioResult> runMasteringTargets (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& meters = ctx.session().mastering();
    if (! engine.getTransport().isStopped() || engine.getMasteringPlayer().isPlaying())
        return ScenarioResult::skip ("requires stopped transport and mastering player");
    const auto originalStage = engine.getStage();
    const auto originalTarget = meters.targetPresetIndex.load();
    const auto originalIntegrated = meters.meterIntegratedLufs.load();
    const auto originalPeak = meters.meterTruePeakDb.load();
    host.switchToStage (GuiHost::Stage::Mastering);
    engine.suspendProcessing();
    ctx.cleanup ([&host, &engine, &meters, originalStage, originalTarget, originalIntegrated, originalPeak]
    {
        drainModals (host);
        host.restoreMasteringTarget (originalTarget);
        meters.targetPresetIndex.store (originalTarget);
        meters.meterIntegratedLufs.store (originalIntegrated);
        meters.meterTruePeakDb.store (originalPeak);
        engine.resumeProcessing();
        host.switchToStage (guiStage (originalStage));
    });
    const std::array<const char*, 4> names { "Off", "Spotify / YouTube / Tidal", "Apple Music", "Broadcast (EBU R128)" };
    const std::array<float, 4> targets { 0.0f, -14.0f, -16.0f, -23.0f };
    // The picker opens a message-loop turn after the click, the choice reaches
    // the session through the combo's asynchronous change notification, and the
    // readings are recoloured on the view's timer, so each check waits for the
    // state it reads rather than for a fixed time.
    auto steps = std::make_shared<std::vector<Step>>();
    for (int index = 0; index < (int) names.size(); ++index)
    {
        const float listHeight = 8.0f + 26.0f * static_cast<float> (names.size());
        const auto rowClick = [&host, index, listHeight]
        { return host.clickModalAt (0.5f, (4.0f + 26.0f * (static_cast<float> (index) + 0.5f)) / listHeight); };
        const std::string name = names[(std::size_t) index];
        steps->push_back ({ 100, [&host, &ctx]
        { ctx.expect (host.clickMasteringTarget(), "the mastering target picker is not on screen"); },
        [&host] { return host.modalStackEmpty(); }, "the previous target picker never closed" });
        // While the picker is still to open, the combo ignores a second click.
        steps->push_back ({ 0, [&ctx, rowClick]
        { ctx.expect (rowClick(), "mastering target row did not receive a click"); },
        [&host] { return ! host.modalStackEmpty(); }, "the mastering target picker did not open",
        [&host] { host.clickMasteringTarget(); } });
        // A picked row closes the picker at once, so one still up missed the
        // click and takes the same row again.
        steps->push_back ({ 0, [&host, &ctx, &meters, name, index]
        {
            ctx.expect (meters.targetPresetIndex.load() == index, "target picker expected " + std::to_string (index) + " but selected " + std::to_string (meters.targetPresetIndex.load()));
            ctx.expect (host.masteringTargetText().find (name) == 0,
                        "target picker label differs from selected platform: " + host.masteringTargetText());
        },
        [&host, &meters, name, index]
        {
            return host.modalStackEmpty() && meters.targetPresetIndex.load() == index
                && host.masteringTargetText().find (name) == 0;
        },
        "the target picker did not select " + name,
        [&host, rowClick] { if (! host.modalStackEmpty()) rowClick(); } });
        for (int band = 0; band < 4; ++band)
        {
            const std::array<float, 4> offsets { 0.5f, 2.0f, 2.1f, -100.0f };
            const float lufs = band == 3 ? -100.0f : targets[(std::size_t) index] + offsets[(std::size_t) band];
            const float truePeak = band == 3 ? -100.0f : band == 0 ? -1.0f : -0.9f;
            steps->push_back ({ 0, [&meters, lufs, truePeak]
            {
                meters.meterIntegratedLufs.store (lufs);
                meters.meterTruePeakDb.store (truePeak);
            } });
            // The view shows and colours a reading on the same tick, so the
            // cells are compared once they show this one; every step's
            // integrated reading differs from the one before it.
            char integratedText[32], peakText[32];
            std::snprintf (integratedText, sizeof (integratedText), "I %6.1f LUFS", lufs);
            std::snprintf (peakText, sizeof (peakText), "TP %5.1f dBTP", truePeak);
            const std::string shownIntegrated = band == 3 ? "I     -" : integratedText;
            const std::string shownPeak = band == 3 ? "TP   -" : peakText;
            const bool neutral = index == 0 || band == 3;
            const std::array<std::uint32_t, 3> colours { 0xff1a3a1a, 0xff3a3a1a, 0xff3a1a1a };
            const std::uint32_t integrated = neutral ? 0xff1a2228 : colours[(std::size_t) band];
            const std::uint32_t peak = neutral ? 0xff121214 : band == 0 ? 0xff1a3a1a : 0xff3a1a1a;
            const std::string reading = name + " reading band " + std::to_string (band);
            steps->push_back ({ 0, [&host, &ctx, integrated, peak, reading]
            {
                ctx.expect (host.masteringLoudnessColour (false) == integrated,
                            "the integrated loudness colour differs from the target band for " + reading);
                ctx.expect (host.masteringLoudnessColour (true) == peak,
                            "the true-peak colour differs from the platform ceiling for " + reading);
            },
            [&host, shownIntegrated, shownPeak]
            {
                return host.masteringLoudnessText (false) == shownIntegrated
                    && host.masteringLoudnessText (true) == shownPeak;
            },
            "the loudness cells never showed " + reading + " ('" + shownIntegrated + "', '" + shownPeak + "')" });
        }
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar masteringTargets { Scenario {
    "gui.mastering_targets", { "gui", "mastering" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringTargets (host, ctx); }
} };

std::optional<ScenarioResult> runMasteringComboKeepsPanels (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped() || engine.getMasteringPlayer().isPlaying())
        return ScenarioResult::skip ("requires stopped transport and mastering player");
    if (! host.canEmbedPluginEditors())
        return ScenarioResult::skip ("requires a window the native mastering panels can embed into");
    if (! widenMasteringWindow (host, ctx))
        return ScenarioResult::fail ("could not widen the mastering workspace");
    const auto originalStage = engine.getStage();
    host.switchToStage (GuiHost::Stage::Mastering);
    ctx.cleanup ([&host, originalStage]
    {
        drainModals (host);
        host.switchToStage (guiStage (originalStage));
    });

    // The stage opens its framework children from its own message-loop sync,
    // so the case starts once they are up.
    ctx.waitUntil ([&host]
                   {
                       const int open = host.masteringPanelsOpen();
                       return open < 0 || open == 3;
                   }, 15000,
                   [&host, &ctx]
    {
        if (host.masteringPanelsOpen() < 0)
        {
            ctx.complete (ScenarioResult::skip ("this build has no native mastering panels"));
            return;
        }
        auto steps = std::make_shared<std::vector<Step>>();
        steps->push_back ({ 100, [&host, &ctx]
        {
            if (! host.clickMasteringCompPreset())
                ctx.complete (ScenarioResult::skip ("this build lays out no multiband preset picker"));
        } });
        // A hidden panel goes down on the next tick after the popup opens, so
        // the checks sit far enough behind each click to catch it.
        steps->push_back ({ 400, [&host, &ctx]
        {
            ctx.expect (! host.modalStackEmpty(), "the preset picker did not open");
            ctx.expect (host.masteringPanelsOpen() == 3,
                        "the open preset popup took a native mastering panel down");
            ctx.expect (host.pressPeerKey ("escape"), "the preset popup did not handle Escape");
        } });
        steps->push_back ({ 400, [&host, &ctx]
        {
            ctx.expect (host.modalStackEmpty(), "Escape left the preset popup open");
            ctx.expect (host.masteringPanelsOpen() == 3,
                        "a native mastering panel did not survive the preset popup");
        } });
        runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    },
    "the native mastering panels never opened");
    return std::nullopt;
}

const ScenarioRegistrar masteringComboKeepsPanels { Scenario {
    "gui.mastering_combo_keeps_panels", { "gui", "mastering" }, Needs::Engine | Needs::Gui,
    {}, {}, 25000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringComboKeepsPanels (host, ctx); }
} };

std::optional<ScenarioResult> runMasteringPhasePanel (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& player = engine.getMasteringPlayer();
    auto& params = ctx.session().mastering();
    if (! engine.getTransport().isStopped() || player.isPlaying() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    if (! host.canEmbedPluginEditors() || host.engineDetached() || ! engine.isAudioCallbackRegistered()
        || engine.getCurrentSampleRate() <= 0.0)
        return ScenarioResult::skip ("requires native panels and a running audio callback");
    if (! widenMasteringWindow (host, ctx))
        return ScenarioResult::fail ("could not widen the mastering workspace");
    const auto originalStage = engine.getStage();
    const auto originalFile = player.getLoadedFile();
    const auto originalSource = params.sourceFile;
    const auto originalPosition = player.getPlayhead();
    const bool originalMono = engine.getMasteringMonoMonitor();
    ctx.cleanup ([&host, &engine, &player, &params, originalStage, originalFile, originalSource, originalPosition, originalMono]
    {
        player.stop();
        host.resetMasteringPhasePanel();
        drainModals (host);
        if (originalFile.existsAsFile()) player.loadFile (originalFile);
        else player.unloadFile();
        player.setPlayhead (originalPosition);
        params.sourceFile = originalSource;
        engine.setMasteringMonoMonitor (originalMono);
        host.refreshMasteringSource();
        host.switchToStage (guiStage (originalStage));
    });
    ctx.keep (params.phase.bands);
    ctx.keep (params.phase.q);
    ctx.keep (params.phase.averagingMs);
    ctx.keep (params.eqEnabled);
    ctx.keep (params.compEnabled);
    ctx.keep (params.limiterEnabled);
    params.phase.bands.store (4);
    params.phase.q.store (0.25f);
    params.phase.averagingMs.store (50.0f);
    params.eqEnabled.store (false);
    params.compEnabled.store (false);
    params.limiterEnabled.store (false);
    engine.setMasteringMonoMonitor (false);
    const auto source = ctx.tempDir() / "inverted-stereo.wav";
    std::vector<float> left (480000), right (left.size());
    std::uint32_t noise = 0x12345u;
    for (size_t i = 0; i < left.size(); ++i)
    {
        noise = noise * 1664525u + 1013904223u;
        left[i] = ((float) (noise >> 8) / 8388608.0f - 1.0f) * 0.1f;
        right[i] = -left[i];
    }
    dusk::audio::WriteSpec spec;
    spec.sampleRate = 48000.0;
    spec.bitsPerSample = 32;
    auto writer = dusk::audio::FileWriter::create (source, spec);
    const float* channels[] = { left.data(), right.data() };
    if (writer == nullptr || ! writer->write (channels, 2, (std::int64_t) left.size()) || ! writer->flush())
        return ScenarioResult::fail ("could not write the inverted stereo mix");
    writer.reset();
    host.switchToStage (GuiHost::Stage::Mastering);
    if (! host.loadMasteringFile (source)) return ScenarioResult::fail ("could not load the inverted stereo mix");
    ctx.waitUntil ([&host]
    {
        if (host.masteringPanelsOpen() < 0) return true;
        const auto* report = host.masteringPhaseReport();
        return host.masteringPanelsOpen() == 3 && report != nullptr && report->settings.bands == 4
            && std::abs (report->settings.q - 0.25f) < 1.0e-5f
            && std::abs (report->settings.averagingMs - 50.0f) < 1.0e-3f
            && ! report->broadband.minimumValid && ! report->broadband.flagged;
    }, 15000,
                  [&host, &ctx, &player]
    {
        if (host.masteringPanelsOpen() < 0)
        {
            ctx.complete (ScenarioResult::skip ("this build has no native mastering panels"));
            return;
        }
        if (! ctx.expect (host.clickMasteringButton ("Play"), "Play was unavailable"))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        ctx.waitUntil ([&host]
        {
            const auto* report = host.masteringPhaseReport();
            return report != nullptr && report->frames > 0 && report->broadband.correlationValid
                && report->broadband.correlation < -0.99 && report->broadband.minimumValid
                && report->broadband.flagged;
        }, 15000, [&host, &ctx, &player]
        {
            player.stop();
            auto steps = std::make_shared<std::vector<Step>>();
            steps->push_back ({ 300, [&host] { host.switchToStage (GuiHost::Stage::Mixing); } });
            steps->push_back ({ 200, [&host] { host.switchToStage (GuiHost::Stage::Mastering); },
                               [&host] { return host.masteringPanelsOpen() == 0; }, "mastering children stayed open" });
            steps->push_back ({ 200, [&host, &ctx]
            {
                const auto* report = host.masteringPhaseReport();
                ctx.expect (report != nullptr && report->broadband.minimumValid && report->broadband.flagged
                            && report->broadband.minimumCorrelation < -0.99,
                            "native child recreation lost held phase evidence");
            }, [&host] { return host.masteringPanelsOpen() == 3; }, "mastering children did not reopen" });
            const auto click = [&] (std::string control, float position = 0.5f, int modifiers = 0)
            {
                steps->push_back ({ 100, [&host, &ctx, control, position, modifiers]
                { ctx.expect (host.masteringPhasePointer (control, position, true, modifiers), "phase press unavailable: " + control); } });
                steps->push_back ({ 100, [&host, &ctx, control, position, modifiers]
                { ctx.expect (host.masteringPhasePointer (control, position, false, modifiers), "phase release unavailable: " + control); } });
            };
            click ("mode");
            steps->push_back ({ 100, [&host, &ctx]
            { ctx.expect (host.pressPeerKey ("escape"), "Phase menu did not keep Escape"); },
                               [&host]
            {
               #if defined (__APPLE__)
                if (! host.masteringPhaseHasKeyboard()) return false;
               #endif
                return host.masteringPhaseCapturesKeyboard();
            }, "Phase menu never captured the keyboard" });
            steps->push_back ({ 100, [] {}, [&host] { return ! host.masteringPhaseCapturesKeyboard(); },
                               "Escape left the Phase menu active" });
            click ("mono");
            steps->push_back ({ 100, [&ctx]
            { ctx.expect (ctx.engine().getMasteringMonoMonitor(), "Mono control did not change the live monitor"); } });
            click ("reset");
            steps->push_back ({ 100, [&host, &ctx]
            {
                const auto* report = host.masteringPhaseReport();
                ctx.expect (report != nullptr && ! report->broadband.minimumValid && ! report->broadband.flagged,
                            "Reset left held phase evidence");
            } });
            const auto enterValue = [&] (const char* control, std::string value)
            {
                click (control, 0.5f, 2);
                steps->push_back ({ 100, [&host, &ctx, value]
                { ctx.expect (host.typeInMasteringPhase (value), "Phase value entry did not accept text"); },
                                   [&host] { return host.masteringPhaseCapturesKeyboard(); },
                                   "Phase value entry never captured the keyboard" });
                steps->push_back ({ 100, [&host, &ctx]
                { ctx.expect (host.pressPeerKey ("return"), "Phase value entry did not keep Return"); } });
            };
            enterValue ("bands", "64");
            enterValue ("q", "8");
            enterValue ("averaging", "5000");
            steps->push_back ({ 200, [&host, &ctx]
            {
                const auto& settings = ctx.session().mastering().phase;
                ctx.expect (settings.bands.load() == 64 && std::abs (settings.q.load() - 8.0f) < 1.0e-5f
                            && std::abs (settings.averagingMs.load() - 5000.0f) < 1.0e-3f,
                            "native Phase controls did not reach their documented upper bounds");
                ctx.expect (host.masteringPanelsOpen() == 3, "Phase interaction closed a native module");
            }, [&host]
            {
                const auto* report = host.masteringPhaseReport();
                return report != nullptr && report->settings.bands == 64
                    && std::abs (report->settings.q - 8.0f) < 1.0e-5f
                    && std::abs (report->settings.averagingMs - 5000.0f) < 1.0e-3f;
            }, "the host analyzer did not follow the native settings" });
            click ("q", 0.5f, 2);
            steps->push_back ({ 100, [&host, &ctx]
            {
                // Clear the field's initial selection so Select All must reach it.
                ctx.expect (host.pressPeerKey ("cursor right"), "Q entry did not keep Right");
            }, [&host]
            {
               #if defined (__APPLE__)
                if (! host.masteringPhaseHasKeyboard()) return false;
               #endif
                return host.masteringPhaseCapturesKeyboard();
            }, "Q text entry never captured the keyboard" });
            steps->push_back ({ 100, [&host, &ctx]
            { ctx.expect (host.pressPeerKey (commandChord ('A')), "Q entry did not keep Select All"); } });
            steps->push_back ({ 100, [&host, &ctx]
            { ctx.expect (host.typeInMasteringPhase ("3.5"), "Q entry did not accept text"); } });
            steps->push_back ({ 100, [&host, &ctx]
            { ctx.expect (host.pressPeerKey ("return"), "Q entry did not keep Return"); } });
            steps->push_back ({ 200, [&host, &ctx]
            {
                ctx.expect (std::abs (ctx.session().mastering().phase.q.load() - 3.5f) < 1.0e-5f,
                            "parent-routed Select All and Return did not commit Q text");
                ctx.expect (host.masteringPanelsOpen() == 3, "Q entry closed a native module");
            }, [&host] { return ! host.masteringPhaseCapturesKeyboard(); }, "Q entry kept the keyboard after Return" });
            runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
        }, "live mastering never produced a valid negative correlation and sticky warning");
    }, "the native mastering panels never opened");
    return std::nullopt;
}

std::optional<ScenarioResult> runMasteringCompactCover (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped() || engine.getMasteringPlayer().isPlaying() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    if (! host.canEmbedPluginEditors()) return ScenarioResult::skip ("requires native panel embedding");
    const auto size = host.mainWindowSize();
    if (size.size() < 4) return ScenarioResult::fail ("window limits are unavailable");
    const auto originalStage = engine.getStage();
    host.switchToStage (GuiHost::Stage::Mastering);
    const int originalModule = host.masteringModuleIndex();
    ctx.cleanup ([&host, size, originalStage, originalModule]
    {
        drainModals (host);
        host.restoreMasteringModule (originalModule);
        host.setMainWindowMinimumWidth (size[2]);
        host.resizeMainWindow (size[0], size[1]);
        host.switchToStage (guiStage (originalStage));
    });
    if (host.masteringPanelsOpen() < 0) return ScenarioResult::skip ("this build has no native mastering panels");
    host.restoreMasteringModule (3);
    if (! host.setMainWindowMinimumWidth (900) || ! host.resizeMainWindow (1040, std::max (900, size[1])))
        return ScenarioResult::fail ("could not request the compact workspace");
    const int wide = std::max (1440, size[0]), height = std::max (900, size[1]);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [] {}, [&host]
    {
        return host.masteringModuleIndex() == 3 && host.masteringModulePickerShown()
            && host.masteringPanelsOpen() == 1 && host.masteringPhasePointer ("mode", 0.5f, true);
    }, "compact Phase mode control never became ready" });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.masteringPhasePointer ("mode", 0.5f, false), "Phase mode release is unavailable"); } });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressPeerKey ("escape"), "Phase menu did not accept Escape"); },
                       [&host] { return host.masteringPhaseCapturesKeyboard() && host.masteringPhaseHasKeyboard(); },
                       "Phase menu never received native keyboard focus" });
    steps->push_back ({ 100, [] {},
                       [&host] { return ! host.masteringPhaseCapturesKeyboard() && host.masteringPhaseHasKeyboard(); },
                       "Escape did not leave keyboard focus on the Phase child" });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.clickMasteringModulePicker(), "compact module picker is unavailable"); },
                       [&host] { return host.masteringModulePickerShown(); }, "workspace never became compact" });
    steps->push_back ({ 100, [&host, &ctx]
    {
        ctx.expect (host.modalHasKeyboardFocus(), "compact module menu did not own keyboard focus");
        ctx.expect (host.pressPeerKey ("home", 0), "compact module Home was unavailable");
        ctx.expect (host.pressPeerKey ("cursor down", 0), "compact compressor choice was unavailable");
        ctx.expect (host.pressPeerKey ("return", '\r'), "compact module Return was unavailable");
    }, [&host]
    {
        return host.modalCount() > 0 && host.modalHasKeyboardFocus() && host.masteringPanelsOpen() == 0;
    }, "compact module choices never received focus after native teardown" });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickMasteringButton ("Load mix..."), "Load mix was unavailable"); },
                       [&host] { return host.masteringModuleIndex() == 1 && host.masteringPanelsOpen() == 0; },
                       "compact compressor selection left native children up" });
    steps->push_back ({ 100, [&host, &ctx, wide, height]
    { ctx.expect (host.resizeMainWindow (wide, height), "could not widen beneath the file browser"); },
                       [&host] { return ! host.modalStackEmpty(); }, "the file browser never opened" });
    steps->push_back ({ 400, [&host, &ctx]
    {
        ctx.expect (! host.modalStackEmpty(), "wide resize dismissed the file browser");
        ctx.expect (host.masteringPanelsOpen() == 0, "resizing with zero hide tokens opened native children above the browser");
        ctx.expect (host.pressPeerKey ("escape"), "the file browser did not accept Escape");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickMasteringButton ("Load mix..."), "second Load mix was unavailable"); },
                       [&host] { return host.modalStackEmpty() && host.masteringPanelsOpen() == 3; },
                       "native children did not appear after the cover closed" });
    steps->push_back ({ 100, [&host, &ctx, height]
    { ctx.expect (host.resizeMainWindow (1040, height), "could not narrow beneath the file browser"); },
                       [&host] { return ! host.modalStackEmpty(); }, "the second file browser never opened" });
    steps->push_back ({ 400, [&host, &ctx]
    {
        ctx.expect (! host.modalStackEmpty(), "compact resize dismissed the file browser");
        ctx.expect (host.masteringModulePickerShown() && host.masteringPanelsOpen() == 0,
                    "compact resize reopened a covered child");
        ctx.expect (host.pressPeerKey ("escape"), "the second file browser did not accept Escape");
    } });
    steps->push_back ({ 400, [&host, &ctx]
    {
        ctx.expect (host.masteringModuleIndex() == 1 && host.masteringPanelsOpen() == 0,
                    "closing the cover re-showed unselected compact modules");
        ctx.expect (host.clickMasteringModulePicker(), "module picker did not return");
    }, [&host] { return host.modalStackEmpty(); }, "the second cover never closed" });
    steps->push_back ({ 100, [&host, &ctx]
    {
        ctx.expect (host.pressPeerKey ("end", 0), "compact Phase choice was unavailable");
        ctx.expect (host.pressPeerKey ("return", '\r'), "compact Phase Return was unavailable");
    }, [&host]
    {
        return host.modalCount() > 0 && host.modalHasKeyboardFocus() && host.masteringPanelsOpen() == 0;
    }, "compact module choices never regained keyboard focus" });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.masteringModuleIndex() == 3, "Phase was not selected"); },
                       [&host] { return host.masteringPanelsOpen() == 1; }, "compact Phase child did not open" });
    steps->push_back ({ 100, [&host, &ctx, wide, height]
    { ctx.expect (host.resizeMainWindow (wide, height), "could not restore the wide workspace"); } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickMasteringCompPreset(), "wide compressor preset picker is unavailable"); },
                       [&host] { return host.masteringPanelsOpen() == 3; }, "wide native children did not return" });
    steps->push_back ({ 300, [&host, &ctx, height]
    {
        ctx.expect (! host.modalStackEmpty(), "the compressor preset popup never opened");
        ctx.expect (host.masteringPanelsOpen() == 3, "opening the preset popup closed an uncovered panel");
        ctx.expect (host.resizeMainWindow (1040, height), "could not narrow beneath the preset popup");
    } });
    steps->push_back ({ 400, [&host, &ctx]
    {
        ctx.expect (! host.modalStackEmpty(), "narrowing dismissed the preset popup");
        ctx.expect (host.masteringModulePickerShown() && host.masteringPanelsOpen() == 0,
                    "a resized native panel covered the open preset popup");
        ctx.expect (host.pressPeerKey ("escape"), "the preset popup did not accept Escape");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.masteringModuleIndex() == 3, "the preset popup changed the compact selection"); },
                       [&host] { return host.modalStackEmpty() && host.masteringPanelsOpen() == 1; },
                       "the compact Phase child did not return after closing the preset popup" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar masteringPhasePanel { Scenario {
    "gui.mastering_phase_panel", { "gui", "mastering", "phase" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000, [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringPhasePanel (host, ctx); }
} };
const ScenarioRegistrar masteringCompactCover { Scenario {
    "gui.mastering_compact_cover", { "gui", "mastering", "phase" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000, [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringCompactCover (host, ctx); }
} };

// Linux leaves an unpainted area dark and macOS leaves it white, so the check
// reads the page's own render rather than the screen.
std::optional<ScenarioResult> runMasteringCompPanelPainted (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped() || engine.getMasteringPlayer().isPlaying())
        return ScenarioResult::skip ("requires stopped transport and mastering player");
    if (! widenMasteringWindow (host, ctx))
        return ScenarioResult::fail ("could not widen the mastering workspace");
    const auto originalStage = engine.getStage();
    host.switchToStage (GuiHost::Stage::Mastering);
    ctx.cleanup ([&host, originalStage] { host.switchToStage (guiStage (originalStage)); });

    ctx.waitUntil ([&host] { return host.masteringCompPanelUnpaintedPixels() >= 0; }, 10000,
                   [&host, &ctx]
    {
        const int unpainted = host.masteringCompPanelUnpaintedPixels();
        ctx.expect (unpainted == 0, "the multiband comp panel left " + std::to_string (unpainted)
                                        + " pixels around its editor unpainted");
        ctx.complete (ctx.verdict());
    },
    "the mastering page never came on screen");
    return std::nullopt;
}

const ScenarioRegistrar masteringCompPanelPainted { Scenario {
    "gui.mastering_comp_panel_painted", { "gui", "mastering" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringCompPanelPainted (host, ctx); }
} };

std::optional<ScenarioResult> runMasteringLoad (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& player = engine.getMasteringPlayer();
    if (! engine.getTransport().isStopped() || player.isPlaying())
        return ScenarioResult::skip ("requires stopped transport and mastering player");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalStage = engine.getStage();
    const auto originalFile = player.getLoadedFile();
    const auto originalPosition = player.getPlayhead();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, &player, originalDir, originalStage, originalFile, originalPosition, restore]
    {
        drainModals (host);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
        if (originalFile.existsAsFile()) player.loadFile (originalFile);
        else player.unloadFile();
        player.setPlayhead (originalPosition);
        host.refreshMasteringSource();
        host.switchToStage (guiStage (originalStage));
    });
    const auto write = [] (const std::filesystem::path& path, int frames)
    {
        dusk::audio::WriteSpec spec;
        spec.sampleRate = 48000;
        spec.numChannels = 2;
        auto writer = dusk::audio::FileWriter::create (path, spec);
        std::vector<float> silence (static_cast<std::size_t> (frames));
        const float* channels[] = { silence.data(), silence.data() };
        return writer && writer->write (channels, 2, frames) && writer->flush();
    };
    const auto chosen = ctx.tempDir() / "Chosen mix.wav";
    const auto bounce = ctx.tempDir() / "bounce.wav";
    const auto mixdown = ctx.tempDir() / "mixdown.wav";
    if (! write (chosen, 4800) || ! write (bounce, 9600))
        return ScenarioResult::fail ("could not write the mastering load fixtures");
    applySessionDirectory (session, ctx.tempDir());
    host.switchToStage (GuiHost::Stage::Mastering);
    const auto check = [&ctx, &session, &player] (const std::filesystem::path& path, int frames)
    {
        ctx.expect (player.isLoaded(), "mastering player is not loaded");
        ctx.expect (player.getLoadedFile().getFullPathName().toStdString() == path.string(),
                    "mastering player loaded the wrong file");
        ctx.expect (session.mastering().sourceFile.getFullPathName().toStdString() == path.string(),
                    "session did not retain the loaded source path");
        ctx.expect (player.getLengthSamples() == frames, "mastering source length differs from fixture");
        ctx.expect (std::abs (player.getSourceSampleRate() - 48000.0) < 0.01,
                    "mastering source rate differs from fixture");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickMasteringButton ("Load mix..."), "Load mix button is unavailable"); } });
    steps->push_back ({ 200, [&host, &ctx, chosen]
    {
        ctx.expect (host.focusFileName(), "mastering file browser did not open");
       #if defined (__APPLE__)
        host.pressPeerKey ("command + A", 'a');
       #else
        host.pressPeerKey ("ctrl + A", 'a');
       #endif
        for (const char ch : chosen.string())
            host.pressPeerKey (ch == ' ' ? "Space" : std::string (1, ch), ch);
    } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Open"), "Open did not accept the mix path"); } });
    steps->push_back ({ 600, [&host, &ctx, check, chosen]
    {
        check (chosen, 4800);
        ctx.expect (host.clickMasteringButton ("Load latest mixdown"), "Load latest mixdown button is unavailable");
    } });
    steps->push_back ({ 600, [&host, &ctx, check, write, bounce, mixdown]
    {
        check (bounce, 9600);
        ctx.expect (write (mixdown, 14400), "could not write the preferred mixdown");
        ctx.expect (host.clickMasteringButton ("Load latest mixdown"), "second latest-mixdown click failed");
    } });
    // A pick that won't load turns up as a failed load while the working mix
    // stays where it was and the session keeps naming it, so nothing is left
    // to save. The fixture is too short to play through the pick, so the kept
    // position is what shows it was never reloaded.
    const auto broken = ctx.tempDir() / "broken-mix.wav";
    std::ofstream (broken) << "not audio";
    const auto sessionJson = std::make_shared<std::string>();
    steps->push_back ({ 600, [&host, &ctx, &session, &player, check, mixdown, sessionJson]
    {
        check (mixdown, 14400);
        player.setPlayhead (7200);
        *sessionJson = SessionSerializer::serialize (session).toStdString();
        ctx.expect (host.clickMasteringButton ("Load mix..."), "Load mix button is unavailable");
    } });
    steps->push_back ({ 200, [&host, &ctx, broken]
    {
        ctx.expect (host.focusFileName(), "mastering file browser did not open");
        typeReplacing (host, broken.string());
    } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Open"), "Open did not accept the broken mix path"); } });
    steps->push_back ({ 600, [&host, &ctx, &session, &player, check, broken, mixdown, sessionJson]
    {
        check (mixdown, 14400);
        ctx.expect (player.getPlayhead() == 7200, "a mix that would not load reloaded the working one");
        ctx.expect (SessionSerializer::serialize (session).toStdString() == *sessionJson,
                    "a mix that would not load changed the session");
        const auto line = "Failed to load: " + broken.string();
        ctx.expect (host.masteringSourceText() == line,
                    "a mix that would not load reads '" + host.masteringSourceText() + "', not '" + line + "'");
        ctx.expect (host.masteringWaveformFile() == mixdown,
                    "a mix that would not load took the waveform to '" + host.masteringWaveformFile().string() + "'");
    } });
    steps->push_back ({ 100, [&ctx, &player, chosen, bounce, mixdown]
    {
        // The player and the waveform each open the source; neither may keep a
        // replaced or unloaded one open, or Windows can't rename or delete it.
        ctx.expect (openHandlesInProcess (chosen) + openHandlesInProcess (bounce) == 0,
                    "a replaced mastering source is still open");
        player.unloadFile();
        ctx.expect (openHandlesInProcess (mixdown) == 0, "the unloaded mastering source is still open");
        for (const auto& file : { chosen, bounce, mixdown })
        {
            auto moved = file;
            moved += ".moved";
            ctx.expect (renameSucceeds (file, moved), "a released mastering source could not be renamed");
        }
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar masteringLoad { Scenario {
    "gui.mastering_load", { "gui", "mastering" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringLoad (host, ctx); }
} };

// A session saves its mastering source, so every switch brings the Mastering
// page in line with the incoming session: its mix loads, stopped; a session
// without one empties the page; a saved mix that has gone missing is reported
// as failing to load and listed with the missing audio. The outgoing mix is
// closed each time, which is what lets Windows rename or delete it.
std::optional<ScenarioResult> runMasteringFollowsSession (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& player = engine.getMasteringPlayer();
    if (! engine.getTransport().isStopped() || player.isPlaying() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and mastering player and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    keepStage (host, ctx);
    ctx.cleanup ([&host, &session, originalDir, restore]
    {
        drainModals (host);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
        host.refreshMasteringSource();
    });

    // Each fixture opens on the Mastering stage, so the page is up for every check.
    const auto fixture = [&ctx] (const std::string& name, const std::string& mix, bool keepMix)
    {
        const auto folder = ctx.tempDir() / name;
        std::filesystem::create_directories (folder);
        auto saved = std::make_unique<Session>();
        applySessionDirectory (*saved, folder);
        saved->uiStage.store ((int) AudioEngine::Stage::Mastering);
        if (! keepMix)
        {
            // A region whose take is gone too, so the alert has both kinds.
            AudioRegion region;
            region.file = decltype (region.file) ((folder / "audio" / "lost take.wav").u8string().c_str());
            region.lengthInSamples = 4800;
            saved->track (0).regions = { region };
        }
        if (! mix.empty())
        {
            dusk::audio::WriteSpec spec;
            spec.sampleRate = 48000;
            spec.numChannels = 2;
            auto writer = dusk::audio::FileWriter::create (folder / mix, spec);
            std::vector<float> silence (4800);
            const float* channels[] = { silence.data(), silence.data() };
            if (! ctx.expect (writer && writer->write (channels, 2, 4800) && writer->flush(),
                              "could not write the " + name + " mix"))
                return std::filesystem::path();
            writer.reset();
            saved->mastering().sourceFile = decltype (saved->mastering().sourceFile) ((folder / mix).u8string().c_str());
        }
        if (! ctx.expect (SessionSerializer::save (*saved, folder / "session.json"),
                          "could not write the " + name + " session"))
            return std::filesystem::path();
        if (! keepMix) std::filesystem::remove (folder / mix);
        return folder / "session.json";
    };
    const auto first = fixture ("Session A", "mixdown.wav", true);
    const auto second = fixture ("Session B", "master b.wav", true);
    const auto empty = fixture ("Session C", "", true);
    const auto missing = fixture ("Session D", "gone.wav", false);
    if (first.empty() || second.empty() || empty.empty() || missing.empty()) return ctx.verdict();
    const auto firstMix = first.parent_path() / "mixdown.wav";
    const auto secondMix = second.parent_path() / "master b.wav";

    const auto same = [] (const std::filesystem::path& a, const std::filesystem::path& b)
    {
        std::error_code ignored;
        return ! a.empty() && std::filesystem::weakly_canonical (a, ignored)
                                  == std::filesystem::weakly_canonical (b, ignored);
    };
    const auto shows = [&host, &ctx, &player, same] (const std::filesystem::path& mix, const std::string& when)
    {
        const auto held = std::filesystem::u8path (player.getLoadedFile().getFullPathName().toStdString());
        ctx.expect (player.isLoaded() && same (held, mix), when + ": the mastering player holds '" + held.string() + "'");
        ctx.expect (! player.isPlaying(), when + ": the switch left the mastering player playing");
        ctx.expect (player.getPlayhead() == 0, when + ": the mix did not load from its start");
        const auto line = mix.filename().string() + "  (" + mix.parent_path().filename().string() + "/)";
        ctx.expect (host.masteringSourceText() == line,
                    when + ": the page reads '" + host.masteringSourceText() + "', not '" + line + "'");
        ctx.expect (same (host.masteringWaveformFile(), mix),
                    when + ": the waveform shows '" + host.masteringWaveformFile().string() + "'");
    };
    const auto showsNothing = [&host, &ctx, &player] (const std::string& line, const std::string& when)
    {
        ctx.expect (! player.isLoaded(), when + ": the mastering player still holds a mix");
        ctx.expect (host.masteringSourceText() == line,
                    when + ": the page reads '" + host.masteringSourceText() + "', not '" + line + "'");
        ctx.expect (host.masteringWaveformFile().empty(),
                    when + ": the waveform still shows '" + host.masteringWaveformFile().string() + "'");
    };
    // Linux counts the descriptors; everywhere the rename is refused while
    // anything in the process holds the file open on Windows.
    const auto released = [&ctx] (const std::filesystem::path& mix, const std::string& what)
    {
        ctx.expect (openHandlesInProcess (mix) == 0, what + " is still open after the switch");
        auto moved = mix;
        moved += ".moved";
        const bool away = renameSucceeds (mix, moved);
        ctx.expect (away && renameSucceeds (moved, mix), what + " could not be renamed after the switch");
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, first] { reopenSavedSession (host, first); } });
    steps->push_back ({ 300, [&host, &player, shows, firstMix, second]
    {
        shows (firstMix, "opening a session with a mix");
        player.play();
        reopenSavedSession (host, second);
    } });
    steps->push_back ({ 300, [&host, shows, released, firstMix, secondMix, empty]
    {
        shows (secondMix, "switching to a session with another mix");
        released (firstMix, "the first session's mix");
        reopenSavedSession (host, empty);
    } });
    steps->push_back ({ 300, [&host, showsNothing, released, secondMix, missing]
    {
        showsNothing ("No mix loaded", "switching to a session without a mix");
        released (secondMix, "the second session's mix");
        reopenSavedSession (host, missing);
    } });
    // The load posts its missing-audio alert for the next message-loop turn.
    steps->push_back ({ 0, [&host, &ctx, &session, same, showsNothing, missing]
    {
        const auto saved = std::filesystem::u8path (session.mastering().sourceFile.getFullPathName().toStdString());
        ctx.expect (same (saved, missing.parent_path() / "gone.wav"),
                    "the session with a missing mix does not name it: '" + saved.string() + "'");
        showsNothing ("Failed to load: " + saved.u8string(), "switching to a session whose mix is missing");
        // The take is listed under the regions' wording, the mix apart from it.
        const auto text = host.modalText();
        const auto regions = text.find ("These audio files referenced by the session could not be found:");
        const auto mix = text.find ("The mastering mix this session had loaded could not be found:\n\n    "
                                    + saved.u8string() + "\n\nThe MASTERING stage opens without a mix. "
                                    "Put the file back and reload the session, or load another mix.");
        ctx.expect (text.rfind ("Missing audio files", 0) == 0 && regions != std::string::npos
                        && mix != std::string::npos && regions < mix
                        && text.find ("lost take.wav") < mix && text.find ("gone.wav") > mix,
                    "the missing-audio alert does not list the take and the mix apart: '" + text + "'");
        ctx.expect (host.clickModalButton ("OK"), "the missing-audio alert has no OK button");
    },
    [&host] { return ! host.modalStackEmpty(); }, "the session whose mix is missing put up no alert" });
    // OK only closes the alert, so a press the window lost is pressed again.
    steps->push_back ({ 0, [&host, first] { reopenSavedSession (host, first); },
                        [&host] { return host.modalStackEmpty(); }, "OK did not close the missing-audio alert",
                        [&host] { host.clickModalButton ("OK"); } });
    steps->push_back ({ 300, [shows, firstMix] { shows (firstMix, "switching back from an empty page"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar masteringFollowsSession { Scenario {
    "gui.mastering_follows_session", { "gui", "mastering", "session" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringFollowsSession (host, ctx); }
} };

// Save As copies a session-local mix into the new folder with the audio and
// repoints the session at the copy. The player moves onto the copy and plays on
// from where it was, and lets go of the old folder's file; a Save As that fails
// leaves the player on the original, which the session still names.
std::optional<ScenarioResult> runMasteringMixFollowsSaveAs (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& player = engine.getMasteringPlayer();
    if (! engine.getTransport().isStopped() || player.isPlaying() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and mastering player and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    keepStage (host, ctx);
    ctx.cleanup ([&host, &session, originalDir, restore]
    {
        drainModals (host);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
        host.refreshMasteringSource();
    });

    // Thirty seconds, so playback started before the Save As is still running
    // at it however long a loaded host takes over the browser.
    static constexpr int kFrames = 1440000;
    const auto folder = ctx.tempDir() / "Mix session";
    const auto original = folder / "mixdown.wav";
    fs::create_directories (folder);
    {
        dusk::audio::WriteSpec spec;
        spec.sampleRate = 48000;
        spec.numChannels = 2;
        auto writer = dusk::audio::FileWriter::create (original, spec);
        std::vector<float> silence (kFrames);
        const float* channels[] = { silence.data(), silence.data() };
        if (! writer || ! writer->write (channels, 2, kFrames) || ! writer->flush())
            return ScenarioResult::fail ("could not write the mix");
    }
    {
        auto saved = std::make_unique<Session>();
        applySessionDirectory (*saved, folder);
        saved->uiStage.store ((int) AudioEngine::Stage::Mastering);
        saved->mastering().sourceFile = decltype (saved->mastering().sourceFile) (original.u8string().c_str());
        if (! SessionSerializer::save (*saved, folder / "session.json"))
            return ScenarioResult::fail ("could not write the session");
    }
    reopenSavedSession (host, folder / "session.json");

    const auto same = [] (const fs::path& a, const fs::path& b)
    {
        std::error_code ignored;
        return ! a.empty() && fs::weakly_canonical (a, ignored) == fs::weakly_canonical (b, ignored);
    };
    const auto held = [&player]
    { return fs::u8path (player.getLoadedFile().getFullPathName().toStdString()); };
    const auto named = [&session]
    { return fs::u8path (session.mastering().sourceFile.getFullPathName().toStdString()); };
    if (! same (held(), original)) return ScenarioResult::fail ("could not open the session with its mix");
    const auto holds = [&host, &ctx, same, held, named] (const fs::path& mix, const std::string& when)
    {
        ctx.expect (same (held(), mix), when + ": the mastering player holds '" + held().string() + "'");
        ctx.expect (same (named(), mix), when + ": the session names '" + named().string() + "'");
        const auto line = mix.filename().string() + "  (" + mix.parent_path().filename().string() + "/)";
        ctx.expect (host.masteringSourceText() == line,
                    when + ": the page reads '" + host.masteringSourceText() + "', not '" + line + "'");
        ctx.expect (same (host.masteringWaveformFile(), mix),
                    when + ": the waveform shows '" + host.masteringWaveformFile().string() + "'");
    };

    // A folder where session.json would go makes that write fail for any user.
    const auto blocked = ctx.tempDir() / "Blocked json";
    fs::create_directories (blocked / "session.json");
    std::ofstream (blocked / "session.json" / "keep") << "theirs";
    const auto copy = ctx.tempDir() / "Saved copy";
    static constexpr std::int64_t kStart = 48000;

    auto steps = std::make_shared<std::vector<Step>>();
    pushSaveAsSteps (host, ctx, *steps, blocked);
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.modalText().rfind ("Save failed\nDusk Studio could not write the session file:", 0) == 0,
                    "the blocked Save As showed '" + host.modalText() + "' rather than Save failed");
        ctx.expect (host.clickModalButton ("OK"), "the Save failed alert has no OK button");
    } });
    // OK only closes the alert, so a press the window lost is pressed again.
    steps->push_back ({ 0, [&ctx, &player, holds, original, blocked]
    {
        holds (original, "after a failed Save As");
        std::error_code ignored;
        ctx.expect (! fs::exists (blocked / "mixdown.wav", ignored), "the failed Save As left its copy of the mix");
        player.setPlayhead (kStart);
        player.play();
    },
    [&host] { return host.modalStackEmpty(); }, "OK did not close the Save failed alert",
    [&host] { host.clickModalButton ("OK"); } });
    auto atSave = std::make_shared<std::int64_t> (0);
    pushSaveAsSteps (host, ctx, *steps, copy, [&player, atSave] { *atSave = player.getPlayhead(); });
    // The player advances on the audio thread, which a busy message loop does
    // not hold up, so the mix is past where the Save press found it by the time
    // this runs; a playhead the save sent back towards the start is short of it.
    steps->push_back ({ 300, [&ctx, &session, &player, holds, original, copy, atSave]
    {
        ctx.expect (currentSessionDirectory (session) == copy,
                    "Save As left the session at '" + currentSessionDirectory (session).string() + "'");
        holds (copy / "mixdown.wav", "after Save As");
        ctx.expect (player.isPlaying(), "the mix stopped playing at the Save As");
        ctx.expect (*atSave > kStart && player.getPlayhead() > *atSave && player.getPlayhead() < kFrames,
                    "the mix did not carry on from where it was playing: playhead " + std::to_string (player.getPlayhead())
                        + ", " + std::to_string (*atSave) + " at the Save press");
        player.stop();
        ctx.expect (openHandlesInProcess (original) == 0, "the old folder's mix is still open after Save As");
        auto moved = original;
        moved += ".moved";
        const bool away = renameSucceeds (original, moved);
        ctx.expect (away && renameSucceeds (moved, original), "the old folder's mix could not be renamed after Save As");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar masteringMixFollowsSaveAs { Scenario {
    "gui.mastering_mix_follows_save_as", { "gui", "mastering", "session" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringMixFollowsSaveAs (host, ctx); }
} };

// Saving reads every plug-in's state with the audio thread held out, but it
// stays on the device, so nothing re-prepares and the MASTERING loudness
// readings carry on through Save and Save As. A real change of configuration -
// Effect oversampling, or the device's block size where it offers another -
// re-prepares the engine and starts them again, as does opening another
// session. The mix is stopped before each check, so a meter that was cleared
// cannot measure its way back.
std::optional<ScenarioResult> runMasteringLoudnessSurvivesSave (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& player = engine.getMasteringPlayer();
    auto& meters = session.mastering();
    if (! engine.getTransport().isStopped() || player.isPlaying() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and mastering player and no modal");
    if (! engine.isAudioCallbackRegistered() || engine.getCurrentSampleRate() <= 0.0)
        return ScenarioResult::skip ("requires a running audio device");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalFactor = session.oversamplingFactor.load();
    auto& devices = engine.getDeviceManager();
    const auto originalSetup = devices.getSetup();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    keepStage (host, ctx);
    auto registration = std::make_shared<std::vector<bool>>();
    ctx.cleanup ([&host, &engine, &session, &player, &devices, originalDir, originalFactor, originalSetup, restore]
    {
        engine.observeAudioRegistrationForScenario ({});
        engine.stop();
        player.stop();
        drainModals (host);
        if (devices.getSetup().bufferSize != originalSetup.bufferSize)
            devices.setSetup (originalSetup, false);
        if (session.oversamplingFactor.load() != originalFactor)
        {
            session.oversamplingFactor.store (originalFactor);
            engine.restartDspWhenIdle();
        }
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
        host.refreshMasteringSource();
    });

    // Twenty seconds of a steady tone, so every pass plays well inside it. It
    // plays through the real device, so it sits near -50 LUFS: well clear of
    // the -70 gate, and barely audible if the machine has speakers.
    static constexpr int kFrames = 960000;
    const auto folder = ctx.tempDir() / "Tone session";
    const auto mix = folder / "mixdown.wav";
    fs::create_directories (folder);
    {
        dusk::audio::WriteSpec spec;
        spec.sampleRate = 48000;
        spec.numChannels = 2;
        auto writer = dusk::audio::FileWriter::create (mix, spec);
        std::vector<float> tone (kFrames);
        for (int i = 0; i < kFrames; ++i)
            tone[(size_t) i] = 0.004f * (float) std::sin (6.283185307179586 * 997.0 * i / 48000.0);
        const float* channels[] = { tone.data(), tone.data() };
        if (! writer || ! writer->write (channels, 2, kFrames) || ! writer->flush())
            return ScenarioResult::fail ("could not write the mix");
    }
    {
        auto saved = std::make_unique<Session>();
        applySessionDirectory (*saved, folder);
        saved->uiStage.store ((int) AudioEngine::Stage::Mastering);
        saved->mastering().sourceFile = decltype (saved->mastering().sourceFile) (mix.u8string().c_str());
        if (! SessionSerializer::save (*saved, folder / "session.json"))
            return ScenarioResult::fail ("could not write the session");
    }
    reopenSavedSession (host, folder / "session.json");
    if (! player.isLoaded()) return ScenarioResult::fail ("could not open the session with its mix");

    struct Reading { float integrated = -100.0f; float truePeak = -100.0f; };
    auto measured = std::make_shared<Reading>();
    const auto read = [&meters]
    { return Reading { meters.meterIntegratedLufs.load(), meters.meterTruePeakDb.load() }; };
    const auto format = [] (const Reading& r)
    { return std::to_string (r.integrated) + " LUFS, " + std::to_string (r.truePeak) + " dBTP"; };
    // The MASTERING stage returns from the callback before the transport moves,
    // and no plug-in is loaded here, so nothing above would notice a gate the
    // save never lifted; gui.save_keeps_mixer_running covers the mixer side.
    const auto kept = [&ctx, &engine, read, format, measured, registration] (const std::string& after)
    {
        const auto now = read();
        ctx.expect (std::abs (now.integrated - measured->integrated) < 0.01f
                        && std::abs (now.truePeak - measured->truePeak) < 0.01f,
                    after + " changed the loudness readings from " + format (*measured) + " to " + format (now));
        ctx.expect (registration->empty(), after + " took the engine off the audio device");
        ctx.expect (! engine.isProcessingSuspended(), after + " left the audio thread held out");
    };
    const auto cleared = [&ctx, read, format] (const std::string& after)
    {
        const auto now = read();
        ctx.expect (now.integrated <= -99.0f && now.truePeak <= -99.0f,
                    after + " left the loudness readings at " + format (now));
    };
    // Plays a second and a half of the mix, then stops it and lets the meter
    // publish what it measured.
    const auto measure = [&player] (std::vector<Step>& into)
    {
        into.push_back ({ 200, [&player] { player.play(); } });
        into.push_back ({ 1500, [&player] { player.stop(); } });
    };
    const auto remember = [&ctx, read, format, measured] (const std::string& what)
    {
        *measured = read();
        ctx.expect (measured->integrated > -65.0f && measured->integrated < -35.0f
                        && measured->truePeak > -65.0f && measured->truePeak < -35.0f,
                    what + " measured " + format (*measured));
    };

    const auto copy = ctx.tempDir() / "Tone copy";
    auto steps = std::make_shared<std::vector<Step>>();
    measure (*steps);
    steps->push_back ({ 700, [&engine, remember, registration]
    {
        remember ("the mix");
        engine.observeAudioRegistrationForScenario ([registration] (bool attached) { registration->push_back (attached); });
        engine.play();
    } });
    pushFileMenuSteps (host, ctx, *steps, "Save");
    steps->push_back ({ 500, [&host, &ctx, &engine, kept]
    {
        ctx.expect (host.modalStackEmpty(), "Save left '" + host.modalText() + "' up");
        ctx.expect (host.statusMessage() == "Saved: Tone session", "Save reported '" + host.statusMessage() + "'");
        ctx.expect (engine.getTransport().isPlaying(), "Save stopped the rolling transport");
        kept ("Save");
    } });
    pushSaveAsSteps (host, ctx, *steps, copy);
    steps->push_back ({ 800, [&ctx, &engine, &session, &player, kept, copy]
    {
        ctx.expect (currentSessionDirectory (session) == copy,
                    "Save As left the session at '" + currentSessionDirectory (session).string() + "'");
        ctx.expect (fs::u8path (player.getLoadedFile().getFullPathName().toStdString()).parent_path() == copy,
                    "Save As did not move the player onto the copied mix");
        ctx.expect (engine.getTransport().isPlaying(), "Save As stopped the rolling transport");
        engine.stop();
        kept ("Save As");
        engine.observeAudioRegistrationForScenario ({});
    } });

    // Effect oversampling re-prepares every stage that takes it.
    steps->push_back ({ 100, [&engine, &session, originalFactor]
    {
        session.oversamplingFactor.store (originalFactor == 2 ? 1 : 2);
        engine.restartDspWhenIdle();
    } });
    steps->push_back ({ 400, [cleared] { cleared ("changing Effect oversampling"); } });

    // A block size the device offers, if it offers another.
    measure (*steps);
    auto resized = std::make_shared<bool> (false);
    steps->push_back ({ 700, [&ctx, &devices, remember, resized]
    {
        remember ("the mix again");
        auto* device = devices.getCurrentDevice();
        const int current = device != nullptr ? device->getCurrentBufferSizeSamples() : 0;
        int other = 0;
        if (device != nullptr)
            for (const int size : device->getAvailableBufferSizes())
                if (size != current && (other == 0 || std::abs (size - current) < std::abs (other - current)))
                    other = size;
        if (other == 0)
        {
            ctx.note ("the device offers no other block size; that change was not tried");
            return;
        }
        auto setup = devices.getSetup();
        setup.bufferSize = other;
        const auto error = devices.setSetup (setup, false);
        device = devices.getCurrentDevice();
        *resized = error.empty() && device != nullptr && device->getCurrentBufferSizeSamples() == other;
        if (! *resized)
            ctx.note ("the device did not take a block size of " + std::to_string (other) + ": " + error);
    } });
    steps->push_back ({ 400, [cleared, resized]
    {
        if (*resized) cleared ("changing the block size");
    } });

    // So does opening another session.
    measure (*steps);
    steps->push_back ({ 700, [&host, remember, folder]
    {
        remember ("the copied mix");
        reopenSavedSession (host, folder / "session.json");
    } });
    steps->push_back ({ 400, [cleared] { cleared ("opening another session"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar masteringLoudnessSurvivesSave { Scenario {
    "gui.mastering_loudness_survives_save", { "gui", "mastering", "session" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringLoudnessSurvivesSave (host, ctx); }
} };

// Opens a saved session whose track 1 holds a ten-minute MIDI region, so it
// reads clean and a mixdown of it is still rendering when a quit comes.
bool openLongSession (GuiHost& host, ScenarioContext& ctx, const std::filesystem::path& sessionJson)
{
    auto& session = ctx.session();
    MidiRegion region;
    region.lengthInSamples = static_cast<std::int64_t> (ctx.engine().getCurrentSampleRate() * 600.0);
    region.lengthInTicks = 576000;
    session.track (0).mode.store ((int) Track::Mode::Midi);
    session.track (0).midiRegions.publish (
        std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { region }));
    std::filesystem::create_directories (sessionJson.parent_path());
    return SessionSerializer::save (session, sessionJson) && host.openSession (sessionJson)
        && currentSessionDirectory (session) == sessionJson.parent_path();
}

// A quit while a mixdown renders cancels the render the way its Cancel does,
// and asks about the unsaved changes only once the worker has stopped: every
// step of a quit stops the transport or detaches audio, which the worker owns
// until then.
std::optional<ScenarioResult> runQuitCancelsMixdown (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    if (! transport.isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    if (engine.getCurrentSampleRate() <= 0.0)
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
    });
    const auto mix = ctx.tempDir() / "Mix" / "session.json";
    if (! openLongSession (host, ctx, mix))
        return ScenarioResult::fail ("could not open the session to mix down");
    auto& fader = session.track (0).strip.faderDb;
    fader.store (fader.load() - 3.0f);
    const auto isQuitPrompt = [&host] { return host.modalText().rfind ("Save changes before quitting?", 0) == 0; };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx]
    {
        host.startMixdown();
        ctx.expect (host.mixdownRunning(), "the mixdown did not start");
    } });
    // The offline render rolls the transport from its worker thread, so the
    // quit waits for it to start rolling; a mixdown that ends first fails the
    // check below.
    steps->push_back ({ 0, [&host, &ctx, &transport, isQuitPrompt]
    {
        if (! ctx.expect (host.mixdownRunning() && transport.isPlaying(),
                          "the mixdown was not rolling the transport when the quit came")
            || ! ctx.expect (host.requestQuit(), "quitting the edited session during a mixdown did not go ahead"))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        ctx.expect (! isQuitPrompt(), "the quit asked while the mixdown was still rendering");
    },
    [&host, &transport] { return ! host.mixdownRunning() || transport.isPlaying(); },
    "the mixdown never rolled the transport" });
    steps->push_back ({ 0, [&host, &ctx, &engine, mix]
    {
        ctx.expect (! host.mixdownRunning(), "the quit asked before the mixdown had stopped");
        ctx.expect (engine.isAudioCallbackRegistered() && ! host.engineDetached(),
                    "the cancelled mixdown did not hand the audio callback back");
        ctx.expect (! std::filesystem::exists (mix.parent_path() / "mixdown.wav"),
                    "the cancelled mixdown left a file behind");
        ctx.expect (host.clickModalButton ("Cancel"), "the quit prompt did not offer Cancel");
    },
    isQuitPrompt, "the quit never asked after cancelling the mixdown" });
    // The prompt's Cancel acts a message-loop turn after the press, so a
    // second press could run it twice; the step only waits.
    steps->push_back ({ 0, [] {}, [isQuitPrompt] { return ! isQuitPrompt(); },
                       "Cancel did not close the quit prompt" });
    pushCancelledMixdownCloseSteps (host, *steps, false);
    steps->push_back ({ 0, [] {}, [&host] { return host.autosaveRunning(); },
                       "cancelling the quit left autosave stopped" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar quitCancelsMixdown { Scenario {
    "gui.quit_cancels_running_mixdown", { "gui", "session", "bounce" }, Needs::Engine | Needs::Gui,
    {}, {}, 90000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runQuitCancelsMixdown (host, ctx); }
} };

// For the bb-quit-during-mixdown leg only: Cmd+Q on a saved session while a
// mixdown renders, which ends the process, so it never reports and the leg
// reads the shutdown markers and the exit status instead. It registers no
// cleanup, because the context outlives the window any cleanup would touch.
std::optional<ScenarioResult> runQuitDuringMixdown (GuiHost& host, ScenarioContext& ctx)
{
    auto& transport = ctx.engine().getTransport();
    if (ctx.engine().getCurrentSampleRate() <= 0.0)
        return ScenarioResult::skip ("requires a positive engine sample rate");
    if (! openLongSession (host, ctx, ctx.tempDir() / "Mix" / "session.json"))
        return ScenarioResult::fail ("could not open the session to mix down");

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx]
    {
        host.startMixdown();
        ctx.expect (host.mixdownRunning(), "the mixdown did not start");
    } });
    steps->push_back ({ 300, [&host, &ctx, &transport]
    {
        if (! ctx.expect (host.mixdownRunning() && transport.isPlaying(),
                          "the mixdown was not rolling the transport when the quit came"))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        ctx.expect (host.pressPeerKey (commandChord ('q'), 'q'), "the window did not handle Cmd+Q");
    } });
    runSteps (ctx, steps, [] {});
    return std::nullopt;
}

const ScenarioRegistrar quitDuringMixdown { Scenario {
    "gui.quit_during_mixdown", { "helper" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runQuitDuringMixdown (host, ctx); }
} };

// Mixdown writes mixdown.wav in place and deletes it when cancelled. The
// Mastering player holding the session's mix lets go of it before the render
// opens the file, so the cancel leaves nothing playing the truncated render or
// holding the deleted file open, and the page reports the mix as failing to
// load with the waveform and the player empty alongside it. A Bounce... that
// replaces the mix lets go of it the same way and loads the new render once
// it is closed.
std::optional<ScenarioResult> runMixdownCancelReleasesMix (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& player = engine.getMasteringPlayer();
    if (! engine.getTransport().isStopped() || player.isPlaying() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and mastering player and no modal");
    if (engine.getCurrentSampleRate() <= 0.0)
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
        host.refreshMasteringSource();
    });

    const auto folder = ctx.tempDir() / "Mix session";
    const auto mix = folder / "mixdown.wav";
    fs::create_directories (folder);
    static constexpr int kMixFrames = 48000;
    const auto writeMix = [mix]
    {
        dusk::audio::WriteSpec spec;
        spec.sampleRate = 48000;
        spec.numChannels = 2;
        auto writer = dusk::audio::FileWriter::create (mix, spec);
        std::vector<float> silence (kMixFrames);
        const float* channels[] = { silence.data(), silence.data() };
        return writer && writer->write (channels, 2, kMixFrames) && writer->flush();
    };
    if (! writeMix()) return ScenarioResult::fail ("could not write the mix");
    session.mastering().sourceFile = decltype (session.mastering().sourceFile) (mix.u8string().c_str());
    session.uiStage.store ((int) AudioEngine::Stage::Mastering);
    if (! openLongSession (host, ctx, folder / "session.json"))
        return ScenarioResult::fail ("could not open the session to mix down");

    const auto same = [] (const fs::path& a, const fs::path& b)
    {
        std::error_code ignored;
        return ! a.empty() && fs::weakly_canonical (a, ignored) == fs::weakly_canonical (b, ignored);
    };
    const auto held = [&player] { return fs::u8path (player.getLoadedFile().getFullPathName().toStdString()); };
    if (! same (held(), mix)) return ScenarioResult::fail ("the session did not open with its mix loaded");

    // One second of session, so the bounce finishes on its own.
    const auto bounceOverMix = [&host, &ctx, &engine, &session, &player, same, held, writeMix, mix]
    {
        MidiRegion region;
        region.lengthInSamples = static_cast<std::int64_t> (engine.getCurrentSampleRate());
        region.lengthInTicks = 960;
        session.track (0).midiRegions.publish (
            std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { region }));
        if (! ctx.expect (writeMix() && player.loadFile (session.mastering().sourceFile),
                          "could not put the mix back for the bounce"))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        auto bounce = std::make_shared<std::vector<Step>>();
        bounce->push_back ({ 200, [&host, &ctx]
        { ctx.expect (host.pressPeerKey (commandChord ('b'), 'b'), "the window did not handle Cmd+B"); } });
        bounce->push_back ({ 700, [&host, &ctx]
        {
            if (! ctx.expect (host.focusFileName(), "Cmd+B showed '" + host.modalText() + "', not the bounce browser"))
                return;
            typeReplacing (host, "mixdown");
            ctx.expect (host.clickModalButton ("Save"), "the bounce browser has no Save button");
        } });
        bounce->push_back ({ 500, [&host, &ctx]
        { ctx.expect (host.clickModalButton ("Replace"), "a bounce over mixdown.wav did not ask to replace it"); } });
        bounce->push_back ({ 100, [&ctx, &player]
        { ctx.expect (! player.isLoaded(), "the bounce started with the mastering player still holding mixdown.wav"); } });
        runSteps (ctx, bounce, [&host, &ctx, &player, same, held, mix]
        {
            ctx.waitUntil ([&host] { return host.clickModalButton ("Close"); }, 20000, [&host, &ctx, &player, same, held, mix]
            {
                ctx.later (300, [&host, &ctx, &player, same, held, mix]
                {
                    ctx.expect (player.isLoaded() && same (held(), mix),
                                "the finished bounce left the mastering player holding '" + held().string() + "'");
                    ctx.expect (player.getLengthSamples() > kMixFrames,
                                "the mastering player holds the old mix, not the bounce");
                    const auto line = mix.filename().string() + "  (" + mix.parent_path().filename().string() + "/)";
                    ctx.expect (host.masteringSourceText() == line,
                                "after the bounce the page reads '" + host.masteringSourceText() + "', not '" + line + "'");
                    ctx.expect (same (host.masteringWaveformFile(), mix),
                                "after the bounce the waveform shows '" + host.masteringWaveformFile().string() + "'");
                    ctx.complete (ctx.verdict());
                });
            }, "the bounce over mixdown.wav did not finish with a Close button");
        });
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx, &player, mix]
    {
        host.switchToStage (GuiHost::Stage::Mixing);
        host.startMixdown();
        ctx.expect (host.mixdownRunning(), "the mixdown did not start");
        ctx.expect (! player.isLoaded(), "the mixdown started with the mastering player still holding mixdown.wav");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.mixdownRunning(), "the mixdown stopped before it was cancelled");
        ctx.expect (host.clickModalButton ("Cancel"), "the mixdown did not offer Cancel");
    } });
    runSteps (ctx, steps, [&host, &ctx, &session, &player, same, held, mix, bounceOverMix]
    {
        ctx.waitUntil ([&host] { return ! host.mixdownRunning(); }, 5000,
                       [&host, &ctx, &session, &player, same, held, mix, bounceOverMix]
        {
            ctx.later (200, [&host, &ctx, &session, &player, same, held, mix, bounceOverMix]
            {
                ctx.expect (host.clickModalButton ("Close"), "the cancelled mixdown did not offer Close");
                ctx.later (300, [&host, &ctx, &session, &player, same, held, mix, bounceOverMix]
                {
                    ctx.expect (host.modalStackEmpty(), "the cancelled mixdown left '" + host.modalText() + "' up");
                    std::error_code ignored;
                    ctx.expect (! fs::exists (mix, ignored), "the cancelled mixdown left mixdown.wav behind");
                    ctx.expect (! player.isLoaded(), "the mastering player holds '" + held().string()
                                                        + "' after the cancelled mixdown");
                    ctx.expect (openHandlesInProcess (mix) + deletedHandlesInProcess (mix) == 0,
                                "the cancelled mixdown's file is still open");
                    const auto named = fs::u8path (session.mastering().sourceFile.getFullPathName().toStdString());
                    ctx.expect (same (named, mix), "the session no longer names its mix: '" + named.string() + "'");
                    const auto line = "Failed to load: " + named.u8string();
                    ctx.expect (host.masteringSourceText() == line,
                                "the page reads '" + host.masteringSourceText() + "', not '" + line + "'");
                    ctx.expect (host.masteringWaveformFile().empty(),
                                "the waveform still shows '" + host.masteringWaveformFile().string() + "'");
                    bounceOverMix();
                });
            });
        }, "the mixdown did not cancel");
    });
    return std::nullopt;
}

const ScenarioRegistrar mixdownCancelReleasesMix { Scenario {
    "gui.mixdown_cancel_releases_mastering_mix", { "gui", "bounce", "mastering" }, Needs::Engine | Needs::Gui,
    {}, {}, 40000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMixdownCancelReleasesMix (host, ctx); }
} };

// A stem loaded as the mix is one of the files Bounce stems... writes over. The
// player lets go of it before the render opens it, so a cancel deletes it with
// nothing holding it, and a finished bounce is loaded back when it is closed.
std::optional<ScenarioResult> runBounceStemsReleasesMix (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& player = engine.getMasteringPlayer();
    if (! engine.getTransport().isStopped() || player.isPlaying() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and mastering player and no modal");
    if (engine.getCurrentSampleRate() <= 0.0)
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
        host.refreshMasteringSource();
    });

    const auto folder = ctx.tempDir() / "Stems session";
    const auto base = folder / "stems" / "stems.wav";
    const auto stem = folder / "stems" / "stems_01_Kick.wav";
    fs::create_directories (stem.parent_path());
    static constexpr int kStemFrames = 48000;
    const auto writeStem = [stem]
    {
        dusk::audio::WriteSpec spec;
        spec.sampleRate = 48000;
        spec.numChannels = 2;
        auto writer = dusk::audio::FileWriter::create (stem, spec);
        std::vector<float> silence (kStemFrames);
        const float* channels[] = { silence.data(), silence.data() };
        return writer && writer->write (channels, 2, kStemFrames) && writer->flush();
    };
    if (! writeStem()) return ScenarioResult::fail ("could not write the stem");
    session.track (0).name = "Kick";
    session.mastering().sourceFile = decltype (session.mastering().sourceFile) (stem.u8string().c_str());
    session.uiStage.store ((int) AudioEngine::Stage::Mastering);
    if (! openLongSession (host, ctx, folder / "session.json"))
        return ScenarioResult::fail ("could not open the session to bounce");

    const auto same = [] (const fs::path& a, const fs::path& b)
    {
        std::error_code ignored;
        return ! a.empty() && fs::weakly_canonical (a, ignored) == fs::weakly_canonical (b, ignored);
    };
    const auto held = [&player] { return fs::u8path (player.getLoadedFile().getFullPathName().toStdString()); };
    if (! same (held(), stem)) return ScenarioResult::fail ("the session did not open with the stem loaded");

    // The first bounce has ten minutes to render and is still going when it is
    // checked. The second has one second plus the tail, which a fast machine
    // renders before any check can catch it running, so that one is judged by
    // the file it leaves for the player instead.
    const auto bounceStems = [&host, &ctx, &player, base] (std::vector<Step>& into, bool longRender)
    {
        into.push_back ({ 200, [&host, &ctx]
        {
            host.switchToStage (GuiHost::Stage::Mixing);
            ctx.expect (host.clickFileMenu(), "the File menu is unavailable");
        } });
        into.push_back ({ 150, [&host, &ctx]
        { ctx.expect (host.clickContextMenuItem ("Bounce stems..."), "the File menu has no Bounce stems..."); } });
        into.push_back ({ 700, [&host, &ctx, base]
        {
            if (! ctx.expect (host.focusFileName(), "Bounce stems... showed '" + host.modalText() + "', not its browser"))
                return;
            typeReplacing (host, base.string());
            ctx.expect (host.clickModalButton ("Save"), "the stems browser has no Save button");
        } });
        into.push_back ({ 500, [&host, &ctx]
        { ctx.expect (host.clickModalButton ("Overwrite"), "a stems bounce over the loaded stem did not ask to overwrite it"); } });
        into.push_back ({ 100, [&host, &ctx, &player, longRender]
        {
            if (longRender)
                ctx.expect (host.renderRunning(), "the stems bounce did not start");
            ctx.expect (! player.isLoaded(), "the stems bounce started with the mastering player still holding the stem");
        } });
    };

    const auto bounceOverStem = [&host, &ctx, &engine, &session, &player, same, held, writeStem, stem, bounceStems]
    {
        MidiRegion region;
        region.lengthInSamples = static_cast<std::int64_t> (engine.getCurrentSampleRate());
        region.lengthInTicks = 960;
        session.track (0).midiRegions.publish (
            std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { region }));
        if (! ctx.expect (writeStem() && player.loadFile (session.mastering().sourceFile),
                          "could not put the stem back for the bounce"))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        auto bounce = std::make_shared<std::vector<Step>>();
        bounceStems (*bounce, false);
        runSteps (ctx, bounce, [&host, &ctx, &player, same, held, stem]
        {
            ctx.waitUntil ([&host] { return host.clickModalButton ("Close"); }, 20000, [&host, &ctx, &player, same, held, stem]
            {
                ctx.later (300, [&host, &ctx, &player, same, held, stem]
                {
                    ctx.expect (player.isLoaded() && same (held(), stem),
                                "the finished stems bounce left the mastering player holding '" + held().string() + "'");
                    ctx.expect (player.getLengthSamples() > kStemFrames,
                                "the mastering player holds the old stem, not the bounce");
                    const auto line = stem.filename().string() + "  (" + stem.parent_path().filename().string() + "/)";
                    ctx.expect (host.masteringSourceText() == line,
                                "after the stems bounce the page reads '" + host.masteringSourceText() + "', not '" + line + "'");
                    ctx.complete (ctx.verdict());
                });
            }, "the stems bounce over the loaded stem did not finish with a Close button");
        });
    };

    auto steps = std::make_shared<std::vector<Step>>();
    bounceStems (*steps, true);
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.renderRunning(), "the stems bounce stopped before it was cancelled");
        ctx.expect (host.clickModalButton ("Cancel"), "the stems bounce did not offer Cancel");
    } });
    runSteps (ctx, steps, [&host, &ctx, &session, &player, same, held, stem, bounceOverStem]
    {
        ctx.waitUntil ([&host] { return ! host.renderRunning(); }, 5000,
                       [&host, &ctx, &session, &player, same, held, stem, bounceOverStem]
        {
            ctx.later (200, [&host, &ctx, &session, &player, same, held, stem, bounceOverStem]
            {
                ctx.expect (host.clickModalButton ("Close"), "the cancelled stems bounce did not offer Close");
                ctx.later (300, [&host, &ctx, &session, &player, same, held, stem, bounceOverStem]
                {
                    ctx.expect (host.modalStackEmpty(), "the cancelled stems bounce left '" + host.modalText() + "' up");
                    std::error_code ignored;
                    ctx.expect (! fs::exists (stem, ignored), "the cancelled stems bounce left the stem behind");
                    ctx.expect (! player.isLoaded(), "the mastering player holds '" + held().string()
                                                        + "' after the cancelled stems bounce");
                    ctx.expect (openHandlesInProcess (stem) + deletedHandlesInProcess (stem) == 0,
                                "the cancelled stems bounce's file is still open");
                    const auto named = fs::u8path (session.mastering().sourceFile.getFullPathName().toStdString());
                    ctx.expect (same (named, stem), "the session no longer names its mix: '" + named.string() + "'");
                    const auto line = "Failed to load: " + named.u8string();
                    ctx.expect (host.masteringSourceText() == line,
                                "the page reads '" + host.masteringSourceText() + "', not '" + line + "'");
                    bounceOverStem();
                });
            });
        }, "the stems bounce did not cancel");
    });
    return std::nullopt;
}

const ScenarioRegistrar bounceStemsReleasesMix { Scenario {
    "gui.bounce_stems_releases_mastering_mix", { "gui", "bounce", "mastering" }, Needs::Engine | Needs::Gui,
    {}, {}, 40000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runBounceStemsReleasesMix (host, ctx); }
} };

// A realtime Mixdown refuses to start while the transport rolls. The render
// never opens mixdown.wav, so the Mastering player keeps the mix it holds, and
// where it stood in it, through the refusal and its Close.
std::optional<ScenarioResult> runRefusedMixdownKeepsMix (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& player = engine.getMasteringPlayer();
    if (! engine.getTransport().isStopped() || player.isPlaying() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and mastering player and no modal");
    if (engine.getCurrentSampleRate() <= 0.0)
        return ScenarioResult::skip ("requires a positive engine sample rate");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    keepStage (host, ctx);
    ctx.cleanup ([&host, &session, &engine, originalDir, restore]
    {
        engine.stop();
        session.track (0).hardwareInsert.enabled.store (false);
        drainModals (host);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
        host.refreshMasteringSource();
    });

    const auto folder = ctx.tempDir() / "Rolling session";
    const auto mix = folder / "mixdown.wav";
    fs::create_directories (folder);
    {
        dusk::audio::WriteSpec spec;
        spec.sampleRate = 48000;
        spec.numChannels = 2;
        auto writer = dusk::audio::FileWriter::create (mix, spec);
        std::vector<float> silence (480000);
        const float* channels[] = { silence.data(), silence.data() };
        if (! writer || ! writer->write (channels, 2, 480000) || ! writer->flush())
            return ScenarioResult::fail ("could not write the mix");
    }
    session.mastering().sourceFile = decltype (session.mastering().sourceFile) (mix.u8string().c_str());
    session.uiStage.store ((int) AudioEngine::Stage::Mastering);
    if (! openLongSession (host, ctx, folder / "session.json"))
        return ScenarioResult::fail ("could not open the session to mix down");
    // Flagged after the open, so the load does not route the strip through a
    // hardware loop; the flag alone is what makes Mixdown offer a realtime pass.
    session.track (0).hardwareInsert.enabled.store (true);

    const auto held = [&player] { return fs::u8path (player.getLoadedFile().getFullPathName().toStdString()); };
    const auto holdsMix = [held, mix]
    {
        std::error_code ignored;
        return fs::weakly_canonical (held(), ignored) == fs::weakly_canonical (mix, ignored);
    };
    if (! holdsMix()) return ScenarioResult::fail ("the session did not open with its mix loaded");
    static constexpr std::int64_t kStart = 96000;

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &engine, &player]
    {
        player.setPlayhead (kStart);
        host.switchToStage (GuiHost::Stage::Mixing);
        engine.play();
    } });
    steps->push_back ({ 300, [&host, &ctx, &engine]
    {
        ctx.expect (engine.getTransport().isPlaying(), "the transport did not start");
        host.startMixdown();
    } });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Realtime"), "Mixdown with a hardware insert did not offer Realtime"); } });
    steps->push_back ({ 300, [&host, &ctx, &engine, &player, held, holdsMix]
    {
        ctx.expect (! host.renderRunning(), "the realtime mixdown started with the transport rolling");
        ctx.expect (engine.getTransport().isPlaying(), "the refused mixdown stopped the transport");
        ctx.expect (player.isLoaded() && holdsMix(), "the refused mixdown let go of the mix: the player holds '"
                                                         + held().string() + "'");
        ctx.expect (host.clickModalButton ("Close"), "the refused mixdown did not offer Close");
    } });
    steps->push_back ({ 300, [&host, &ctx, &engine, &player, held, holdsMix]
    {
        engine.stop();
        ctx.expect (host.modalStackEmpty(), "closing the refused mixdown left '" + host.modalText() + "' up");
        ctx.expect (player.isLoaded() && holdsMix(), "after the refused mixdown the player holds '" + held().string() + "'");
        ctx.expect (player.getPlayhead() == kStart,
                    "the refused mixdown moved the mix's playhead to " + std::to_string (player.getPlayhead()));
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar refusedMixdownKeepsMix { Scenario {
    "gui.refused_mixdown_keeps_mastering_mix", { "gui", "bounce", "mastering" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runRefusedMixdownKeepsMix (host, ctx); }
} };

// A master export reads the Mastering player and owns the audio callback until
// it stops, and its modal covers only the Mastering page, so the menus stay
// live under it. Save, Save as... and a session switch are each refused with
// a status line, leaving the session file, the folder and the mix as they were.
std::optional<ScenarioResult> runSaveRefusedDuringExport (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& player = engine.getMasteringPlayer();
    if (! engine.getTransport().isStopped() || player.isPlaying() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and mastering player and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    keepStage (host, ctx);
    ctx.cleanup ([&host, &session, &engine, originalDir, restore]
    {
        drainModals (host);
        engine.setRenderOversamplingOverride (0);
        engine.reattachAudioCallback();
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
        host.refreshMasteringSource();
    });

    // An hour, so the export is still rendering through every refusal on the
    // fastest host; mono at 4 kHz keeps the file near 29 MB.
    static constexpr int kRate = 4000;
    static constexpr std::int64_t kFrames = (std::int64_t) kRate * 3600;
    const auto folder = ctx.tempDir() / "Export session";
    const auto mix = folder / "mixdown.wav";
    const auto other = ctx.tempDir() / "Other session" / "session.json";
    fs::create_directories (folder);
    fs::create_directories (other.parent_path());
    {
        auto writer = dusk::audio::FileWriter::create (mix, { (double) kRate, 1, 16 });
        std::vector<float> silence (kRate);
        const float* channels[] = { silence.data() };
        bool written = writer != nullptr;
        for (std::int64_t frame = 0; written && frame < kFrames; frame += kRate)
            written = writer->write (channels, 1, kRate);
        if (! written || ! writer->flush())
            return ScenarioResult::fail ("could not write the mix");
    }
    {
        auto saved = std::make_unique<Session>();
        applySessionDirectory (*saved, folder);
        saved->uiStage.store ((int) AudioEngine::Stage::Mastering);
        saved->mastering().sourceFile = decltype (saved->mastering().sourceFile) (mix.u8string().c_str());
        if (! SessionSerializer::save (*saved, folder / "session.json"))
            return ScenarioResult::fail ("could not write the session");
        saved->mastering().sourceFile = decltype (saved->mastering().sourceFile) ();
        applySessionDirectory (*saved, other.parent_path());
        if (! SessionSerializer::save (*saved, other))
            return ScenarioResult::fail ("could not write the other session");
    }
    reopenSavedSession (host, folder / "session.json");
    if (currentSessionDirectory (session) != folder || ! player.isLoaded())
        return ScenarioResult::fail ("could not open the session with its mix");
    auto& fader = session.track (0).strip.faderDb;
    fader.store (fader.load() - 3.0f);
    const auto savedJson = fileBytes (folder / "session.json");
    const auto output = ctx.tempDir() / "Master.wav";
    const auto held = [&player] { return player.getLoadedFile().getFullPathName().toStdString(); };
    const auto heldAtStart = held();

    const auto refused = [&host, &ctx, &session, &player, held, heldAtStart, folder, savedJson]
                         (const std::string& status, const std::string& what)
    {
        ctx.expect (host.statusMessage() == status,
                    what + " during the export read '" + host.statusMessage() + "', not '" + status + "'");
        ctx.expect (host.renderRunning(), what + " stopped the export");
        ctx.expect (currentSessionDirectory (session) == folder,
                    what + " moved the session to '" + currentSessionDirectory (session).string() + "'");
        ctx.expect (fileBytes (folder / "session.json") == savedJson, what + " during the export wrote session.json");
        ctx.expect (player.isLoaded() && held() == heldAtStart,
                    what + " during the export moved the mastering player to '" + held() + "'");
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickMasteringButton ("Export master..."), "export button unavailable"); } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("WAV 24-bit \xe2\x80\x94 session rate"), "archive preset unavailable"); } });
    steps->push_back ({ 200, [&host, &ctx, output]
    {
        ctx.expect (host.focusFileName(), "export file browser unavailable");
        typeReplacing (host, output.string());
    } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Save"), "export destination was not accepted"); } });
    steps->push_back ({ 150, [&host, &ctx]
    {
        ctx.expect (host.renderRunning(), "the export did not start");
        ctx.expect (host.clickFileMenu(), "the File menu is unavailable during the export");
    } });
    steps->push_back ({ 150, [&host, &ctx, refused]
    {
        ctx.expect (host.clickContextMenuItem ("Save"), "the File menu has no Save");
        refused ("Session not saved: finish or cancel the render first", "File > Save");
    } });
    steps->push_back ({ 50, [&host, other] { host.requestSessionSwitch (other); } });
    steps->push_back ({ 100, [&host, &ctx, refused]
    {
        refused ("Session not switched: finish or cancel the master export first", "A session switch");
        ctx.expect (host.clickFileMenu(), "the File menu is unavailable during the export");
    } });
    steps->push_back ({ 150, [&host, &ctx, refused]
    {
        ctx.expect (host.clickContextMenuItem ("Save as..."), "the File menu has no Save as...");
        refused ("Session not saved: finish or cancel the render first", "File > Save as...");
        ctx.expect (host.clickModalButton ("Cancel"), "the export did not offer Cancel");
    } });
    runSteps (ctx, steps, [&host, &ctx, &engine]
    {
        ctx.waitUntil ([&host] { return ! host.renderRunning(); }, 5000, [&host, &ctx, &engine]
        {
            ctx.later (200, [&host, &ctx, &engine]
            {
                ctx.expect (host.clickModalButton ("Close"), "the cancelled export did not offer Close");
                ctx.expect (engine.isAudioCallbackRegistered(), "the cancelled export did not hand the audio callback back");
                ctx.complete (ctx.verdict());
            });
        }, "the export did not cancel");
    });
    return std::nullopt;
}

const ScenarioRegistrar saveRefusedDuringExport { Scenario {
    "gui.save_refused_during_master_export", { "gui", "mastering", "session" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSaveRefusedDuringExport (host, ctx); }
} };

// A session switch while a render runs is refused and the status bar names the
// render. Bounce..., Bounce stems... and a track's FREEZE each run and are
// cancelled in turn; every refusal leaves the session and the render running.
std::optional<ScenarioResult> runSwitchRefusedNamesRender (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    if (engine.getCurrentSampleRate() <= 0.0)
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
        host.refreshMasteringSource();
    });

    const auto folder = ctx.tempDir() / "Render session";
    const auto other = ctx.tempDir() / "Other session" / "session.json";
    std::filesystem::create_directories (other.parent_path());
    {
        auto blank = std::make_unique<Session>();
        applySessionDirectory (*blank, other.parent_path());
        if (! SessionSerializer::save (*blank, other))
            return ScenarioResult::fail ("could not write the other session");
    }
    if (! openLongSession (host, ctx, folder / "session.json"))
        return ScenarioResult::fail ("could not open the session to render");

    struct Leg
    {
        std::string render;
        std::function<void (std::vector<Step>&)> start;
    };
    auto legs = std::make_shared<std::vector<Leg>>();
    legs->push_back ({ "bounce", [&host, &ctx] (std::vector<Step>& into)
    {
        into.push_back ({ 300, [&host, &ctx]
        {
            host.switchToStage (GuiHost::Stage::Mixing);
            ctx.expect (host.pressPeerKey (commandChord ('b'), 'b'), "the window did not handle Cmd+B");
        } });
        into.push_back ({ 700, [&host, &ctx]
        {
            ctx.expect (host.modalText().rfind ("Bounce master mix", 0) == 0,
                        "Cmd+B showed '" + host.modalText() + "' rather than the bounce browser");
            ctx.expect (host.clickModalButton ("Save"), "the bounce browser has no Save button");
        } });
    } });
    legs->push_back ({ "stem bounce", [&host, &ctx, folder] (std::vector<Step>& into)
    {
        into.push_back ({ 300, [&host, &ctx]
        {
            host.switchToStage (GuiHost::Stage::Mixing);
            ctx.expect (host.clickFileMenu(), "the File menu is unavailable");
        } });
        into.push_back ({ 150, [&host, &ctx]
        { ctx.expect (host.clickContextMenuItem ("Bounce stems..."), "the File menu has no Bounce stems..."); } });
        into.push_back ({ 700, [&host, &ctx, folder]
        {
            if (! ctx.expect (host.focusFileName(), "Bounce stems... showed '" + host.modalText() + "', not its browser"))
                return;
            typeReplacing (host, (folder / "stems" / "stems.wav").string());
            ctx.expect (host.clickModalButton ("Save"), "the stems browser has no Save button");
        } });
    } });
    legs->push_back ({ "freeze", [&host, &ctx] (std::vector<Step>& into)
    {
        into.push_back ({ 300, [&host] { host.switchToStage (GuiHost::Stage::Recording); } });
        into.push_back ({ 300, [&host, &ctx]
        {
            ctx.expect (host.clickStripControl (GuiHost::StripKind::Channel, 0, "print", 1, false),
                        "track 1 has no FREEZE button to click");
        } });
    } });

    auto runLeg = std::make_shared<std::function<void (std::size_t)>>();
    std::weak_ptr<std::function<void (std::size_t)>> weakRun = runLeg;
    *runLeg = [&host, &ctx, &session, legs, folder, other, weakRun] (std::size_t index)
    {
        if (index >= legs->size())
        {
            ctx.complete (ctx.verdict());
            return;
        }
        const auto render = (*legs)[index].render;
        auto steps = std::make_shared<std::vector<Step>>();
        (*legs)[index].start (*steps);
        steps->push_back ({ 300, [&host, &ctx, render]
        { ctx.expect (host.renderRunning(), "the " + render + " did not start"); } });
        steps->push_back ({ 50, [&host, other] { host.requestSessionSwitch (other); } });
        steps->push_back ({ 100, [&host, &ctx, &session, folder, render]
        {
            const auto want = "Session not switched: finish or cancel the " + render + " first";
            ctx.expect (host.statusMessage() == want,
                        "a session switch during the " + render + " read '" + host.statusMessage() + "', not '" + want + "'");
            ctx.expect (currentSessionDirectory (session) == folder, "the session switched during the " + render);
            ctx.expect (host.renderRunning(), "the refused session switch stopped the " + render);
            ctx.expect (host.clickModalButton ("Cancel"), "the " + render + " did not offer Cancel");
        } });
        auto self = weakRun.lock();
        runSteps (ctx, steps, [&host, &ctx, render, index, self]
        {
            ctx.waitUntil ([&host] { return ! host.renderRunning(); }, 5000, [&host, &ctx, render, index, self]
            {
                ctx.later (200, [&host, &ctx, render, index, self]
                {
                    ctx.expect (host.clickModalButton ("Close"), "the cancelled " + render + " did not offer Close");
                    ctx.later (300, [&host, &ctx, render, index, self]
                    {
                        ctx.expect (host.modalStackEmpty(), "the cancelled " + render + " left '" + host.modalText() + "' up");
                        (*self) (index + 1);
                    });
                });
            }, "the " + render + " did not cancel");
        });
    };
    (*runLeg) (0);
    return std::nullopt;
}

const ScenarioRegistrar switchRefusedNamesRender { Scenario {
    "gui.session_switch_names_the_render", { "gui", "session", "bounce" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSwitchRefusedNamesRender (host, ctx); }
} };

std::optional<ScenarioResult> runMasteringTransport (GuiHost& host, ScenarioContext& ctx)
{
    auto& player = ctx.engine().getMasteringPlayer();
    auto& transport = ctx.engine().getTransport();
    if (! ctx.expect (! player.isLoaded(), "the fixture already has a mastering file")) return ctx.verdict();
    const auto path = ctx.tempDir() / "mastering.wav";
    auto writer = dusk::audio::FileWriter::create (path, { 48000.0, 2, 24 });
    if (! ctx.expect (writer != nullptr, "could not create the mastering fixture")) return ctx.verdict();
    std::vector<float> silence (480000, 0.0f);
    const float* channels[] { silence.data(), silence.data() };
    if (! ctx.expect (writer->write (channels, 2, 480000), "could not write the mastering fixture"))
        return ctx.verdict();
    writer.reset();
    const auto stage = ctx.engine().getStage();
    ctx.cleanup ([&host, &ctx, &player, &transport, stage, source = ctx.session().mastering().sourceFile,
                  state = transport.getState(), position = transport.getPlayhead()]
    {
        drainModals (host);
        player.stop();
        player.unloadFile();
        // unloadFile is only the player's half: the view wrote the path into
        // the session too, and this one points inside a temp directory that
        // dies with the case. Left behind it is saved into every later
        // session and reloads as a missing file.
        ctx.session().mastering().sourceFile = source;
        host.refreshMasteringSource();
        host.switchToStage (guiStage (stage));
        transport.setPlayhead (position);
        transport.setState (state);
    });
    ctx.cleanup (host.preserveKeyboardFocus());
    host.switchToStage (GuiHost::Stage::Recording);
    ctx.expect (host.pressKey ("spacebar", ' ') && transport.isPlaying(), "Space did not start multitrack playback");
    ctx.expect (host.pressKey ("command + 3") && host.stageViewMatches (GuiHost::Stage::Mastering),
                "Cmd+3 did not select Mastering");
    ctx.expect (transport.isStopped(), "entering Mastering did not stop multitrack playback");
    if (! ctx.expect (host.loadMasteringFile (path), "Load mix did not accept the fixture")) return ctx.verdict();
    auto steps = std::make_shared<std::vector<Step>>();
    for (const float fraction : { 0.25f, 0.75f })
    {
        steps->push_back ({ 300, [&host, &ctx, fraction]
        { ctx.expect (host.clickMasteringWaveform (fraction), "the Mastering waveform is not available for seeking"); } });
        steps->push_back ({ 100, [&ctx, &player, fraction]
        {
            const auto position = static_cast<double> (player.getPlayhead()) / static_cast<double> (player.getLengthSamples());
            ctx.expect (std::abs (position - fraction) < 0.01, "the waveform click did not seek to its horizontal position");
            ctx.expect (! player.isPlaying(), "clicking the stopped waveform started playback");
        } });
    }
    steps->push_back ({ 100, [&host, &ctx] { ctx.expect (host.clickMasteringButton ("Play"), "Play is unavailable"); } });
    steps->push_back ({ 150, [&host, &ctx, &player, &transport]
    {
        ctx.expect (player.isPlaying() && player.getPlayhead() > 0, "Play did not advance the mastering player");
        ctx.expect (transport.isStopped(), "mastering Play started multitrack playback");
        ctx.expect (host.clickMasteringButton ("Stop"), "Stop is unavailable");
    } });
    steps->push_back ({ 100, [&host, &ctx, &player]
    {
        ctx.expect (! player.isPlaying(), "Stop did not stop the mastering player");
        player.setPlayhead (96000);
        ctx.expect (host.clickMasteringButton ("|<<"), "Rewind is unavailable");
    } });
    steps->push_back ({ 100, [&host, &ctx, &player]
    {
        ctx.expect (player.getPlayhead() == 0, "Rewind did not return to the start");
        ctx.expect (host.pressKey ("spacebar", ' '), "Space was not handled in Mastering");
    } });
    steps->push_back ({ 150, [&host, &ctx, &player, &transport]
    {
        ctx.expect (player.isPlaying() && player.getPlayhead() > 0, "Space did not play the loaded mix");
        ctx.expect (transport.isStopped(), "Space in Mastering started multitrack playback");
        ctx.expect (host.pressKey ("spacebar", ' '), "second Space was not handled");
        ctx.expect (! player.isPlaying(), "second Space did not stop the loaded mix");
    } });
    steps->push_back ({ 100, [&host, &ctx, &player]
    {
        ctx.expect (host.pressKey ("spacebar", ' ') && player.isPlaying(), "could not restart Mastering before leaving");
        ctx.expect (host.pressKey ("command + 1") && host.stageViewMatches (GuiHost::Stage::Recording),
                    "Cmd+1 did not select Recording");
        ctx.expect (! player.isPlaying(), "leaving Mastering did not stop its player");
    } });
    steps->push_back ({ 100, [&host, &ctx, &transport]
    {
        ctx.expect (transport.isStopped(), "leaving Mastering started multitrack playback");
        ctx.expect (host.pressKey ("command + 2") && host.stageViewMatches (GuiHost::Stage::Mixing),
                    "Cmd+2 did not select Mixing");
        for (int page = 0; page < host.consolePageCount(); ++page)
            ctx.expect (host.pressKey (std::to_string (page + 1), (char) ('1' + page)) && host.consolePageMatches (page),
                        "a plain digit did not select its visible console page");
        ctx.expect (host.pressKey ("command + 4") && host.stageViewMatches (GuiHost::Stage::Aux),
                    "Cmd+4 did not select Aux");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    {
        ctx.expect (host.pressKey ("shift + /", '?'), "the shortcuts key was not handled");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    {
        ctx.expect (host.shortcutsOpen(), "? did not open Keyboard Shortcuts");
        host.closeTopModal();
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (! host.shortcutsOpen(), "Keyboard Shortcuts cleanup left the panel open"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar masteringTransport { Scenario {
    "gui.mastering_transport", { "gui", "mastering" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringTransport (host, ctx); }
} };

std::optional<ScenarioResult> runMixdownHandoff (GuiHost& host, ScenarioContext& ctx)
{
    auto& player = ctx.engine().getMasteringPlayer();
    auto& track = ctx.session().track (0);
    if (! ctx.expect (! player.isLoaded(), "the fixture already has a mastering source")) return ctx.verdict();
    ctx.keep (track.mode);
    keepStage (host, ctx);
    ctx.cleanup ([&host, &ctx, &track, &player, regions = track.regions,
                  source = ctx.session().mastering().sourceFile]
    {
        drainModals (host);
        player.stop();
        player.unloadFile();
        ctx.session().mastering().sourceFile = source;
        track.regions = regions;
    });
    const auto input = ctx.tempDir() / "bounce-source.wav";
    auto writer = dusk::audio::FileWriter::create (input, { 48000.0, 1, 24 });
    if (! ctx.expect (writer != nullptr, "could not create the bounce fixture")) return ctx.verdict();
    std::vector<float> silence (4800, 0.0f);
    const float* channels[] { silence.data() };
    if (! ctx.expect (writer->write (channels, 1, 4800), "could not write the bounce fixture")) return ctx.verdict();
    writer.reset();
    AudioRegion region;
    region.file = decltype (region.file) (input.u8string().c_str());
    region.lengthInSamples = 4800;
    track.regions = { region };
    track.mode.store ((int) Track::Mode::Mono);
    host.switchToStage (GuiHost::Stage::Mixing);
    host.startMixdown();
    ctx.waitUntil ([&host] { return host.clickModalButton ("Close"); }, 30000,
        [&host, &ctx, &player]
        {
            ctx.waitUntil ([&host, &player]
            { return host.stageViewMatches (GuiHost::Stage::Mastering) && player.isLoaded(); }, 5000,
                [&host, &ctx, &player]
                {
                    const auto expected = ctx.session().getSessionDirectory().getChildFile ("mixdown.wav");
                    ctx.expect (player.getLoadedFile() == expected, "Mastering loaded a different file than mixdown.wav");
                    ctx.expect (ctx.session().mastering().sourceFile == expected, "the session did not retain the mixdown source");
                    auto reader = dusk::audio::FileReader::open (expected.getFullPathName().toStdString());
                    if (ctx.expect (reader != nullptr, "Mixdown did not write a readable WAV in the session folder"))
                    {
                        ctx.expect (reader->info().numChannels == 2 && reader->info().bitsPerSample == 24,
                                    "Mixdown did not write stereo 24-bit audio");
                        ctx.expect (reader->info().numFrames >= 4800, "Mixdown truncated the session");
                    }
                    const auto fallback = expected.getSiblingFile ("bounce.wav");
                    if (! ctx.expect (! fallback.existsAsFile(), "the fixture already has a fallback bounce"))
                    {
                        ctx.complete (ctx.verdict());
                        return;
                    }
                    ctx.cleanup ([expected, fallback]
                    {
                        if (! expected.existsAsFile()) fallback.moveFileTo (expected);
                        else fallback.deleteFile();
                    });
                    auto steps = std::make_shared<std::vector<Step>>();
                    steps->push_back ({ 100, [&host, &ctx, &player, expected, fallback]
                    {
                        player.unloadFile();
                        ctx.expect (expected.moveFileTo (fallback), "could not prepare the fallback-only fixture");
                        ctx.expect (host.clickMasteringButton ("Load latest mixdown"), "Load latest mixdown is unavailable");
                    } });
                    steps->push_back ({ 200, [&ctx, &player, fallback]
                    {
                        ctx.expect (player.isLoaded() && player.getLoadedFile() == fallback,
                                    "Load latest mixdown did not fall back to bounce.wav");
                        ctx.expect (ctx.session().mastering().sourceFile == fallback,
                                    "the session did not retain the fallback source");
                    } });
                    steps->push_back ({ 100, [&host, &ctx, &player, expected, fallback]
                    {
                        player.unloadFile();
                        ctx.expect (fallback.copyFileTo (expected), "could not prepare both mix candidates");
                        ctx.expect (host.clickMasteringButton ("Load latest mixdown"), "Load latest mixdown is unavailable");
                    } });
                    steps->push_back ({ 200, [&ctx, &player, expected]
                    {
                        ctx.expect (player.isLoaded() && player.getLoadedFile() == expected,
                                    "Load latest mixdown did not prefer mixdown.wav over bounce.wav");
                        ctx.expect (ctx.session().mastering().sourceFile == expected,
                                    "the session did not retain the preferred source");
                    } });
                    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
                }, "closing the completed Mixdown did not load Mastering");
        }, "Mixdown did not expose its completion Close button");
    return std::nullopt;
}

const ScenarioRegistrar mixdownHandoff { Scenario {
    "gui.mixdown_handoff", { "gui", "bounce" }, Needs::Engine | Needs::Gui,
    {}, {}, 40000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMixdownHandoff (host, ctx); }
} };

// Cmd+B opens the bounce browser in the session folder. Its default name
// renders a stereo 24-bit WAV there at the session rate; a name ending in .mp3
// renders an MP3.
std::optional<ScenarioResult> runBounceKeyFormats (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    const auto rate = engine.getCurrentSampleRate();
    if (rate <= 0.0) return ScenarioResult::skip ("requires a positive engine sample rate");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    ctx.cleanup ([&host, &engine, &session, originalDir, restore]
    {
        drainModals (host);
        engine.setRenderOversamplingOverride (0);
        engine.reattachAudioCallback();
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });
    // An empty session, so each render is the tail alone.
    const auto folder = ctx.tempDir() / "Bounce session";
    std::filesystem::create_directories (folder);
    if (! SessionSerializer::save (*std::make_unique<Session>(), folder / "session.json"))
        return ScenarioResult::fail ("could not write the empty session");
    reopenSavedSession (host, folder / "session.json");
    if (currentSessionDirectory (session) != folder)
        return ScenarioResult::fail ("could not open the empty session");

    const auto wav = folder / "bounce.wav";
   #if DUSKSTUDIO_HAS_LAME
    const auto mp3 = folder / "Mix.mp3";
   #else
    // Without the encoder a typed .mp3 falls back to WAV.
    const auto mp3 = folder / "Mix.wav";
   #endif
    const auto shortcut = [&host, &ctx]
    { ctx.expect (host.pressPeerKey (commandChord ('b'), 'b'), "the window did not handle Cmd+B"); };
    // An empty name keeps the one the browser offers.
    const auto name = [&host, &ctx] (const std::string& typed)
    {
        if (! ctx.expect (host.modalText().rfind ("Bounce master mix", 0) == 0,
                          "Cmd+B showed '" + host.modalText() + "' rather than the bounce browser"))
            return;
        if (! typed.empty())
        {
            if (! ctx.expect (host.focusFileName(), "the bounce browser has no name field")) return;
            typeReplacing (host, typed);
        }
        ctx.expect (host.clickModalButton ("Save"), "the bounce browser has no Save button");
    };
    const auto rendered = [&host, &ctx] (std::function<void()> then)
    {
        ctx.waitUntil ([&host] { return host.clickModalButton ("Close"); }, 20000, std::move (then),
                       "the bounce did not finish with a Close button");
    };

    auto first = std::make_shared<std::vector<Step>>();
    first->push_back ({ 300, shortcut });
    first->push_back ({ 700, [name] { name (""); } });
    runSteps (ctx, first, [&ctx, shortcut, name, rendered, wav, mp3, rate]
    {
        rendered ([&ctx, shortcut, name, rendered, wav, mp3, rate]
        {
            auto reader = dusk::audio::FileReader::open (wav);
            if (ctx.expect (reader != nullptr, "the default bounce left no readable bounce.wav in the session folder"))
            {
                const auto& info = reader->info();
                ctx.expect (info.numChannels == 2 && info.bitsPerSample == 24
                                && std::abs (info.sampleRate - rate) < 0.5 && info.numFrames > 0,
                            "bounce.wav is " + std::to_string (info.numChannels) + " channels of "
                                + std::to_string (info.bitsPerSample) + "-bit at " + std::to_string (info.sampleRate)
                                + " Hz, not stereo 24-bit at " + std::to_string (rate));
            }
            auto second = std::make_shared<std::vector<Step>>();
            second->push_back ({ 400, shortcut });
            second->push_back ({ 700, [name] { name ("Mix.mp3"); } });
            runSteps (ctx, second, [&ctx, rendered, mp3]
            {
                rendered ([&ctx, mp3]
                {
                   #if DUSKSTUDIO_HAS_LAME
                    std::ifstream in (mp3, std::ios::binary);
                    unsigned char head[3] {};
                    in.read (reinterpret_cast<char*> (head), 3);
                    const bool id3 = in.gcount() == 3 && head[0] == 'I' && head[1] == 'D' && head[2] == '3';
                    const bool frameSync = in.gcount() >= 2 && head[0] == 0xFF && (head[1] & 0xE0) == 0xE0;
                    ctx.expect (id3 || frameSync, "Mix.mp3 is missing from the session folder or is not an MP3 stream");
                    ctx.expect (! std::filesystem::exists (mp3.parent_path() / "Mix.wav"),
                                "naming the bounce .mp3 also wrote a WAV");
                   #else
                    auto reader = dusk::audio::FileReader::open (mp3);
                    ctx.expect (reader != nullptr && reader->info().bitsPerSample == 24,
                                "a .mp3 name without the encoder did not fall back to a 24-bit Mix.wav");
                   #endif
                    ctx.complete (ctx.verdict());
                });
            });
        });
    });
    return std::nullopt;
}

const ScenarioRegistrar bounceKeyFormats { Scenario {
    "gui.bounce_key_format_by_extension", { "gui", "bounce", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runBounceKeyFormats (host, ctx); }
} };

// The bounce browser never saves under a folder's name. Save on the name of a
// folder opens it, Save with the name box empty does nothing, and a name typed
// in that folder bounces into it.
std::optional<ScenarioResult> runBounceFolderName (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    ctx.cleanup ([&host, &engine, &session, originalDir, restore]
    {
        drainModals (host);
        engine.setRenderOversamplingOverride (0);
        engine.reattachAudioCallback();
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });
    const auto folder = ctx.tempDir() / "Bounce session";
    const auto mixes = folder / "Mixes";
    std::filesystem::create_directories (mixes);
    if (! SessionSerializer::save (*std::make_unique<Session>(), folder / "session.json"))
        return ScenarioResult::fail ("could not write the empty session");
    reopenSavedSession (host, folder / "session.json");
    if (currentSessionDirectory (session) != folder)
        return ScenarioResult::fail ("could not open the empty session");

    const auto browserUp = [&host, &ctx] (const std::string& after)
    {
        return ctx.expect (host.modalText().rfind ("Bounce master mix", 0) == 0,
                           after + " left '" + host.modalText() + "' up rather than the bounce browser");
    };
    const auto noFolderNamedFile = [&ctx, folder, mixes] (const std::string& after)
    {
        for (const auto& wrong : { folder / "Mixes.wav", mixes / "Mixes.wav", ctx.tempDir() / "Bounce session.wav" })
            ctx.expect (! std::filesystem::exists (wrong), after + " wrote " + wrong.string());
    };
    const auto save = [&host, &ctx] (const std::string& typed)
    {
        if (! ctx.expect (host.focusFileName(), "the bounce browser has no name field")) return;
        typeReplacing (host, typed);
        if (typed.empty()) host.pressPeerKey ("backspace", '\b');
        ctx.expect (host.clickModalButton ("Save"), "the bounce browser has no Save button");
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.pressPeerKey (commandChord ('b'), 'b'), "the window did not handle Cmd+B"); } });
    steps->push_back ({ 700, [browserUp, save] { if (browserUp ("Cmd+B")) save ("Mixes"); } });
    steps->push_back ({ 500, [browserUp, noFolderNamedFile, save]
    {
        noFolderNamedFile ("Save on a folder's name");
        if (browserUp ("Save on a folder's name")) save ("");
    } });
    steps->push_back ({ 500, [browserUp, noFolderNamedFile, save]
    {
        noFolderNamedFile ("Save with an empty name");
        if (browserUp ("Save with an empty name")) save ("Inner");
    } });
    runSteps (ctx, steps, [&host, &ctx, mixes, noFolderNamedFile]
    {
        ctx.waitUntil ([&host] { return host.clickModalButton ("Close"); }, 20000, [&ctx, mixes, noFolderNamedFile]
        {
            ctx.expect (std::filesystem::exists (mixes / "Inner.wav"),
                        "a name typed after Save opened Mixes did not bounce into Mixes");
            noFolderNamedFile ("the bounce");
            ctx.complete (ctx.verdict());
        }, "the bounce did not finish with a Close button");
    });
    return std::nullopt;
}

const ScenarioRegistrar bounceFolderName { Scenario {
    "gui.bounce_folder_name_opens_folder", { "gui", "bounce" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runBounceFolderName (host, ctx); }
} };

// A bounce onto a file that is already there asks first, about the name the
// bounce really writes rather than the one typed: Cancel leaves the file as it
// was, and Replace bounces over it.
std::optional<ScenarioResult> runBounceReplaceConfirm (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    ctx.cleanup ([&host, &engine, &session, originalDir, restore]
    {
        drainModals (host);
        engine.setRenderOversamplingOverride (0);
        engine.reattachAudioCallback();
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });
    const auto folder = ctx.tempDir() / "Bounce session";
    std::filesystem::create_directories (folder);
    if (! SessionSerializer::save (*std::make_unique<Session>(), folder / "session.json"))
        return ScenarioResult::fail ("could not write the empty session");
    reopenSavedSession (host, folder / "session.json");
    if (currentSessionDirectory (session) != folder)
        return ScenarioResult::fail ("could not open the empty session");

    const auto existing = folder / "Mix.wav";
    std::ofstream (existing, std::ios::binary) << "not a bounce";
    const auto before = fileBytes (existing);
    std::error_code error;
    const auto stamped = std::filesystem::last_write_time (existing, error);
    if (error || before.empty()) return ScenarioResult::fail ("could not write the file to replace");

    const auto bounceAsMix = [&host, &ctx]
    { ctx.expect (host.pressPeerKey (commandChord ('b'), 'b'), "the window did not handle Cmd+B"); };
    const auto name = [&host, &ctx] (const std::string& how)
    {
        if (! ctx.expect (host.modalText().rfind ("Bounce master mix", 0) == 0,
                          how + ": Cmd+B showed '" + host.modalText() + "' rather than the bounce browser")
            || ! ctx.expect (host.focusFileName(), how + ": the bounce browser has no name field"))
            return;
        typeReplacing (host, "Mix");
        ctx.expect (host.clickModalButton ("Save"), how + ": the bounce browser has no Save button");
    };
    const auto answer = [&host, &ctx] (const std::string& how, const std::string& button)
    {
        ctx.expect (host.confirmationText() == std::vector<std::string> {
                        "Replace file?", "This file already exists and will be replaced:\n\nMix.wav\n\nContinue?" },
                    how + ": a bounce named Mix over Mix.wav did not ask first; top modal '" + host.modalText() + "'");
        ctx.expect (host.clickModalButton (button), how + ": the replace prompt has no " + button + " button");
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, bounceAsMix });
    steps->push_back ({ 700, [name] { name ("Cancel"); } });
    steps->push_back ({ 500, [answer] { answer ("Cancel", "Cancel"); } });
    steps->push_back ({ 500, [&host, &ctx, existing, before, stamped, bounceAsMix]
    {
        ctx.expect (host.modalStackEmpty(), "Cancel in the replace prompt left '" + host.modalText() + "' up");
        std::error_code ignored;
        ctx.expect (fileBytes (existing) == before && std::filesystem::last_write_time (existing, ignored) == stamped,
                    "Cancel in the replace prompt still wrote Mix.wav");
        bounceAsMix();
    } });
    steps->push_back ({ 700, [name] { name ("Replace"); } });
    steps->push_back ({ 500, [answer] { answer ("Replace", "Replace"); } });
    runSteps (ctx, steps, [&host, &ctx, existing]
    {
        ctx.waitUntil ([&host] { return host.clickModalButton ("Close"); }, 20000, [&ctx, existing]
        {
            auto reader = dusk::audio::FileReader::open (existing);
            ctx.expect (reader != nullptr && reader->info().numChannels == 2 && reader->info().numFrames > 0,
                        "Replace did not bounce over Mix.wav");
            ctx.complete (ctx.verdict());
        }, "the bounce after Replace did not finish with a Close button");
    });
    return std::nullopt;
}

const ScenarioRegistrar bounceReplaceConfirm { Scenario {
    "gui.bounce_replace_confirm", { "gui", "bounce", "messages" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runBounceReplaceConfirm (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
