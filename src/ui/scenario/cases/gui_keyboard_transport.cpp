#include "../GuiScenarioSupport.h"
#include "../../../engine/McuController.h"
#include "../../../engine/McuProtocol.h"
#include "../../../engine/audiofile/FileWriter.h"
#include "../../../engine/audiofile/FileReader.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../session/SessionSerializer.h"

#include <algorithm>
#include <array>
#include <chrono>
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
#include <regex>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#if defined (__linux__)
 #include "../../../engine/RealtimeKit.h"
 #include <csignal>
 #include <sys/resource.h>
#endif

namespace duskstudio::scenario::gui
{
namespace
{
std::optional<ScenarioResult> runTimelineKeys (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    if (engine.getStage() != AudioEngine::Stage::Recording
        && engine.getStage() != AudioEngine::Stage::Mixing)
        return ScenarioResult::skip ("requires a stage with the timeline");
    const auto originalDir = currentSessionDirectory (session);
    const auto playhead = engine.getTransport().getPlayhead();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    const auto originalView = host.tapeView();
    const bool timeline = host.setTimelineShown (true);
    ctx.cleanup ([&host, &engine, &session, originalDir, restore, timeline, playhead, originalView]
    {
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        for (int i = 0; i < Session::kNumTracks; ++i) session.setTrackArmed (i, false);
        host.setTimelineShown (timeline);
        host.restoreTapeView (originalView);
        engine.getTransport().locate (playhead);
    });
    for (int i = 0; i < Session::kNumTracks; ++i)
    {
        session.track (i).regions.clear();
        session.track (i).midiRegions.publish (std::make_unique<std::vector<MidiRegion>>());
        session.setTrackArmed (i, false);
    }
    // Arming every track to overflow the rows needs an input for each: track N
    // follows input N, and Session refuses to arm one the device does not offer.
    ctx.keep (session.deviceCaptureChannels);
    session.deviceCaptureChannels.store (Session::kNumTracks);
    AudioRegion region;
    region.lengthInSamples = (std::int64_t) engine.getCurrentSampleRate() * 30;
    session.track (0).regions.push_back (region);
    const auto key = [&host, &ctx] (const std::string& description, char text)
    { ctx.expect (host.pressPeerKey (description, text), "timeline shortcut was not handled"); };
    const auto fit = std::make_shared<double> (0.0);
    const auto anchor = std::make_shared<std::int64_t> (0);
    const auto beforeWheel = std::make_shared<std::vector<double>>();
    const auto view = [&host] { return host.tapeView(); };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [key] { key ("0", '0'); } });
    steps->push_back ({ 100, [view, fit, key] { *fit = view()[0]; key ("=", '='); } });
    steps->push_back ({ 100, [&ctx, view, fit, key]
    { ctx.expect (std::abs (view()[0] - *fit * 1.15) < 0.0001, "equals did not zoom in"); key ("=", '+'); } });
    steps->push_back ({ 100, [&ctx, view, fit, key]
    { ctx.expect (std::abs (view()[0] - *fit * 1.15 * 1.15) < 0.0001, "plus did not zoom in"); key ("-", '-'); } });
    steps->push_back ({ 100, [&ctx, view, fit, &host, anchor]
    {
        ctx.expect (std::abs (view()[0] - *fit * 1.15) < 0.0001, "minus did not zoom out");
        *anchor = host.tapeRulerSample (0.6f);
        ctx.expect (host.tapeWheel (0.6f, notch (1.0f), true, false), "command wheel unavailable");
    } });
    steps->push_back ({ 100, [&ctx, view, fit, &host, anchor, beforeWheel]
    {
        ctx.expect (std::abs (view()[0] - *fit * 1.15 * 1.15) < 0.0001, "command wheel did not zoom");
        ctx.expect (std::abs (host.tapeRulerSample (0.6f) - *anchor) <= 2, "wheel zoom moved the cursor's sample");
        *beforeWheel = view();
        ctx.expect (host.tapeWheel (0.6f, notch (-1.0f), false, true), "shift wheel unavailable");
    } });
    steps->push_back ({ 100, [&ctx, &host, view, beforeWheel]
    {
        ctx.expect (view()[1] > (*beforeWheel)[1] && std::abs (view()[0] - (*beforeWheel)[0]) < 0.0001,
                    "shift wheel did not scroll the zoomed timeline horizontally");
        *beforeWheel = view();
        ctx.expect (host.tapeWheel (0.6f, notch (-1.0f), false, false), "plain wheel unavailable");
    } });
    steps->push_back ({ 100, [&ctx, view, beforeWheel, key]
    {
        ctx.expect (view()[1] > (*beforeWheel)[1], "plain wheel did not scroll horizontally");
        key ("0", '0');
    } });
    steps->push_back ({ 100, [&ctx, view, fit, key]
    {
        ctx.expect (std::abs (view()[0] - *fit) < 0.0001 && std::abs (view()[1]) < 0.5, "fit did not reset zoom and scroll");
        key ("T", 't');
    } });
    steps->push_back ({ 100, [&ctx, view, key]
    { ctx.expect (view()[3] < 0.5, "T did not hide timeline"); key ("T", 't'); } });
    steps->push_back ({ 100, [&ctx, view]
    { ctx.expect (view()[3] > 0.5, "T did not restore timeline"); } });
    steps->push_back ({ 100, [&session, &ctx]
    {
        for (int i = 0; i < Session::kNumTracks; ++i)
        {
            session.setTrackArmed (i, true);
            ctx.expect (session.track (i).recordArmed.load(),
                        "track " + std::to_string (i + 1) + " would not arm");
        }
    } });
    steps->push_back ({ 300, [&host, &ctx, view, beforeWheel]
    {
        for (int i = 0; i < 8; ++i) host.tapeWheel (0.6f, notch (1.0f), true, true);
        *beforeWheel = view();
        ctx.expect (host.tapeWheel (0.6f, notch (-1.0f), false, false), "overflow wheel unavailable");
    } });
    steps->push_back ({ 100, [&host, &ctx, view, beforeWheel]
    {
        ctx.expect (view()[2] > (*beforeWheel)[2] && std::abs (view()[1] - (*beforeWheel)[1]) < 0.5,
                    "plain wheel did not scroll overflowing rows vertically");
        *beforeWheel = view();
        ctx.expect (host.tapeWheel (0.6f, notch (1.0f), false, true), "overflow shift wheel unavailable");
    } });
    steps->push_back ({ 100, [&ctx, view, beforeWheel]
    {
        ctx.expect (view()[2] < (*beforeWheel)[2] && std::abs (view()[1] - (*beforeWheel)[1]) < 0.5,
                    "shift wheel did not scroll overflowing rows vertically");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar timelineKeys { Scenario {
    "gui.timeline_keys", { "gui", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTimelineKeys (host, ctx); }
} };

std::optional<ScenarioResult> runFileKeys (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalPlayhead = engine.getTransport().getPlayhead();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    ctx.cleanup ([&host, &engine, &session, originalDir, originalPlayhead, restore]
    {
        drainModals (host);
        engine.setRenderOversamplingOverride (0);
        engine.reattachAudioCallback();
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        engine.getTransport().locate (originalPlayhead);
    });
    if (! host.openSession (restore)) return ScenarioResult::fail ("could not establish saved fixture");
    const auto song = ctx.tempDir() / "NewSong";
    const auto copied = ctx.tempDir() / "CopiedSong";
    const auto sourceDir = ctx.tempDir() / "Input";
    std::filesystem::create_directory (sourceDir);
    const auto source = sourceDir / "Input.wav";
    const auto bounce = ctx.tempDir() / "Keyboard bounce.wav";
    dusk::audio::WriteSpec spec;
    spec.sampleRate = 48000;
    spec.numChannels = 1;
    auto writer = dusk::audio::FileWriter::create (source, spec);
    std::vector<float> signal (2400, 0.1f);
    const float* channels[] = { signal.data() };
    if (! writer || ! writer->write (channels, 1, 2400) || ! writer->flush())
        return ScenarioResult::fail ("could not create import fixture");
    writer.reset();
    const auto key = [&host, &ctx] (char letter, bool shift = false)
    {
       #if defined (__APPLE__)
        const std::string modifier = "command + ";
       #else
        const std::string modifier = "ctrl + ";
       #endif
        ctx.expect (host.pressPeerKey (modifier + (shift ? "shift + " : "") + letter, letter), "File shortcut was not handled");
    };
    const auto path = [&host, &ctx] (const std::filesystem::path& value, bool directory = false)
    {
        if (! ctx.expect (directory ? host.clickFileBrowserControl (true) : host.focusFileName(),
                          "File shortcut did not open a browser")) return;
       #if defined (__APPLE__)
        host.pressPeerKey ("command + A", 'a');
       #else
        host.pressPeerKey ("ctrl + A", 'a');
       #endif
        for (const char ch : value.string()) host.pressPeerKey (ch == ' ' ? "Space" : std::string (1, ch), ch);
    };
    const auto button = [&host, &ctx] (const std::string& name)
    { ctx.expect (host.clickModalButton (name), "dialog button unavailable: " + name); };
    const auto saved = [&ctx] (const std::filesystem::path& dir, float fader)
    {
        Session probe;
        if (ctx.expect (SessionSerializer::load (probe, dir / "session.json"), "saved session did not load"))
            ctx.expect (std::abs (probe.track (0).strip.faderDb.load() - fader) < 0.001f, "Save did not persist fader");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 400, [key] { key ('n'); } });
    steps->push_back ({ 200, [path, song] { path (song); } });
    steps->push_back ({ 150, [button] { button ("Save"); } });
    steps->push_back ({ 600, [&ctx, &session, song, key]
    {
        ctx.expect (currentSessionDirectory (session) == song && std::filesystem::exists (song / "session.json"), "New did not create a session");
        ctx.expect (session.track (0).regions.empty(), "New did not clear regions");
        key ('i');
    } });
    steps->push_back ({ 200, [path, sourceDir, &host] { path (sourceDir, true); host.pressPeerKey ("Return", 0); } });
    steps->push_back ({ 500, [&host, &ctx]
    {
        ctx.expect (host.clickFileBrowserControl (false), "Import file list unavailable");
        host.pressPeerKey ("Home", 0);
    } });
    steps->push_back ({ 150, [button] { button ("Open"); } });
    steps->push_back ({ 250, [button] { button ("Import"); } });
    steps->push_back ({ 400, [&session, &ctx, key]
    {
        ctx.expect (session.track (0).regions.size() == 1, "Import shortcut did not import audio");
        session.track (0).strip.faderDb.store (-8.0f);
        key ('s');
    } });
    steps->push_back ({ 250, [saved, song, key] { saved (song, -8.0f); key ('s', true); } });
    steps->push_back ({ 200, [path, copied] { path (copied); } });
    steps->push_back ({ 150, [button] { button ("Save"); } });
    steps->push_back ({ 500, [&ctx, &session, copied, saved, key]
    {
        ctx.expect (currentSessionDirectory (session) == copied, "Save As did not change session directory");
        saved (copied, -8.0f);
        const auto& regions = session.track (0).regions;
        if (ctx.expect (regions.size() == 1, "Save As lost imported region"))
            ctx.expect (regions[0].file.existsAsFile()
                        && regions[0].file.getParentDirectory().getFullPathName().toStdString() == (copied / "audio").string(),
                        "Save As did not copy audio to new session");
        session.track (0).strip.faderDb.store (-3.0f);
        key ('s');
    } });
    steps->push_back ({ 250, [saved, copied, key] { saved (copied, -3.0f); key ('o'); } });
    steps->push_back ({ 200, [path, song] { path (song / "session.json"); } });
    steps->push_back ({ 150, [button] { button ("Open"); } });
    steps->push_back ({ 600, [&ctx, &session, song, key]
    {
        ctx.expect (currentSessionDirectory (session) == song && std::abs (session.track (0).strip.faderDb.load() + 8.0f) < 0.001f,
                    "Open did not restore the chosen session");
        key ('b');
    } });
    steps->push_back ({ 200, [path, bounce] { path (bounce); } });
    steps->push_back ({ 150, [button] { button ("Save"); } });
    runSteps (ctx, steps, [&host, &ctx, &session, bounce, key, button]
    {
        ctx.waitUntil ([&host] { return host.clickModalButton ("Close"); }, 20000,
            [&host, &ctx, &session, bounce, key, button]
            {
                auto reader = dusk::audio::FileReader::open (bounce);
                ctx.expect (reader != nullptr && reader->info().numChannels == 2 && reader->info().bitsPerSample == 24
                            && reader->info().numFrames > 2400, "Bounce shortcut did not render stereo 24-bit WAV");
                auto quitSteps = std::make_shared<std::vector<Step>>();
                quitSteps->push_back ({ 200, [&session, key]
                { session.track (0).strip.faderDb.store (-2.0f); key ('q'); } });
                quitSteps->push_back ({ 200, [&host, &ctx, button]
                { ctx.expect (! host.modalStackEmpty(), "dirty Quit did not prompt"); button ("Cancel"); } });
                quitSteps->push_back ({ 200, [&host, &ctx, &session]
                {
                    ctx.expect (host.modalStackEmpty() && std::abs (session.track (0).strip.faderDb.load() + 2.0f) < 0.001f,
                                "Quit cancellation did not retain the session");
                } });
                runSteps (ctx, quitSteps, [&ctx] { ctx.complete (ctx.verdict()); });
            }, "keyboard bounce did not complete");
    });
    return std::nullopt;
}

const ScenarioRegistrar fileKeys { Scenario {
    "gui.file_keys", { "gui", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runFileKeys (host, ctx); }
} };

std::optional<ScenarioResult> runStageAudioFlow (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (0);
    const auto originalStage = engine.getStage();
    const auto originalPosition = engine.getTransport().getPlayhead();
    const auto originalRegions = track.regions;
    ctx.cleanup ([&host, &engine, &track, originalStage, originalPosition, originalRegions]
    {
        engine.stop();
        track.regions = originalRegions;
        engine.getPlaybackEngine().preparePlayback();
        engine.getTransport().setPlayhead (originalPosition);
        host.switchToStage (guiStage (originalStage));
    });
    ctx.keep (track.mode);
    ctx.keep (track.strip.faderDb);
    ctx.keep (track.strip.mute);
    engine.stop();
    const auto rate = engine.getCurrentSampleRate();
    const auto frames = static_cast<std::int64_t> (rate * 15.0);
    std::vector<float> samples ((size_t) frames);
    for (std::int64_t i = 0; i < frames; ++i)
        samples[(size_t) i] = 0.125f * static_cast<float> (std::sin (6.283185307179586 * 440.0 * static_cast<double> (i) / rate));
    const auto path = ctx.tempDir() / "stage-tone.wav";
    dusk::audio::WriteSpec spec;
    spec.sampleRate = rate;
    spec.numChannels = 1;
    spec.bitsPerSample = 32;
    auto writer = dusk::audio::FileWriter::create (path, spec);
    const float* channels[] = { samples.data() };
    if (writer == nullptr || ! writer->write (channels, 1, frames) || ! writer->flush())
        return ScenarioResult::fail ("could not write the playback tone");
    writer.reset();
    AudioRegion region;
    using File = std::decay_t<decltype (region.file)>;
    region.file = File (path.u8string().c_str());
    region.lengthInSamples = frames;
    region.numChannels = 1;
    track.regions = { region };
    track.mode.store ((int) Track::Mode::Mono);
    track.strip.faderDb.store (0.0f);
    track.strip.mute.store (false);
    host.switchToStage (GuiHost::Stage::Recording);
    engine.getTransport().setPlayhead (0);
    engine.play();
    struct Flow
    {
        std::int64_t position = 0;
        int overruns = 0;
    };
    auto flow = std::make_shared<Flow>();
    // The strip resets its output meter to -100 dB when it starts a block and
    // publishes the peak when it finishes, so a read from this thread that
    // lands inside a block sees silence while the tone plays on. The tone
    // still has to show within the deadline, with the transport rolling,
    // after every switch.
    static constexpr int kSettleMs = 150;
    static constexpr int kToneDeadlineMs = 1500;
    auto run = std::make_shared<std::function<void (std::size_t)>>();
    std::weak_ptr<std::function<void (std::size_t)>> weakRun = run;
    *run = [&host, &ctx, &engine, flow, weakRun] (std::size_t index)
    {
        constexpr std::array<GuiHost::Stage, 6> stages { GuiHost::Stage::Mixing, GuiHost::Stage::Aux,
                                                        GuiHost::Stage::Recording, GuiHost::Stage::Aux,
                                                        GuiHost::Stage::Mixing, GuiHost::Stage::Recording };
        if (index == stages.size()) { ctx.complete (ctx.verdict()); return; }
        const auto stage = stages[index];
        if (! ctx.expect (host.clickStage (stage), "the stage button was not visible"))
        { ctx.complete (ctx.verdict()); return; }
        ctx.later (kSettleMs, [&ctx, &engine, flow, stage, index, next = weakRun.lock()]
        {
            ctx.waitUntil ([&engine]
                           {
                               return ! engine.getTransport().isPlaying()
                                   || engine.getChannelStrip (0).getOutLDb() > -30.0f;
                           },
                           kToneDeadlineMs,
                [&ctx, &engine, flow, stage, index, next]
                {
                    auto& transport = engine.getTransport();
                    const auto expected = stage == GuiHost::Stage::Recording ? AudioEngine::Stage::Recording
                                        : stage == GuiHost::Stage::Mixing ? AudioEngine::Stage::Mixing
                                        : AudioEngine::Stage::Aux;
                    ctx.expect (engine.getStage() == expected, "the stage button did not switch stages");
                    if (! ctx.expect (transport.isPlaying(), "switching stages stopped playback"))
                    { ctx.complete (ctx.verdict()); return; }
                    const auto at = transport.getPlayhead();
                    ctx.expect (at > flow->position, "playback did not advance across a stage switch");
                    flow->position = at;
                    ctx.expect (engine.getXRunCount() == flow->overruns,
                                "a stage switch caused an engine overrun");
                    ctx.later (100, [next, index] { (*next) (index + 1); });
                }, "the playback tone fell silent across a stage switch");
        });
    };
    ctx.later (300, [&engine, flow, run]
    {
        flow->overruns = engine.getXRunCount();
        (*run) (0);
    });
    return std::nullopt;
}

const ScenarioRegistrar stageAudioFlow { Scenario {
    "gui.stage_audio_flow", { "gui", "transport" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runStageAudioFlow (host, ctx); }
} };

std::optional<ScenarioResult> runStageViews (GuiHost& host, ScenarioContext& ctx)
{
    using Stage = GuiHost::Stage;
    auto original = Stage::Recording;
    switch (ctx.engine().getStage())
    {
        case AudioEngine::Stage::Recording: break;
        case AudioEngine::Stage::Mixing: original = Stage::Mixing; break;
        case AudioEngine::Stage::Aux: original = Stage::Aux; break;
        case AudioEngine::Stage::Mastering: original = Stage::Mastering; break;
    }
    auto& transport = ctx.engine().getTransport();
    ctx.cleanup ([&host, &transport, original, state = transport.getState(),
                  position = transport.getPlayhead()]
    {
        host.switchToStage (original);
        transport.setPlayhead (position);
        transport.setState (state);
    });
    transport.setState (Transport::State::Stopped);

    auto run = std::make_shared<std::function<void (std::size_t)>>();
    std::weak_ptr<std::function<void (std::size_t)>> weakRun = run;
    *run = [&host, &ctx, weakRun] (std::size_t index)
    {
        constexpr std::array<Stage, 6> stages { Stage::Mixing, Stage::Recording, Stage::Aux,
                                               Stage::Mastering, Stage::Mixing, Stage::Recording };
        if (index == stages.size()) { ctx.complete (ctx.verdict()); return; }
        const auto stage = stages[index];
        if (! ctx.expect (host.clickStage (stage), "the stage button is not visible"))
        { ctx.complete (ctx.verdict()); return; }
        ctx.waitUntil ([&host, stage] { return host.stageViewMatches (stage); }, 3000,
            [&host, &ctx, stage, index, next = weakRun.lock()]
            {
                if (stage == Stage::Recording || stage == Stage::Mixing)
                    for (int track = 0; track < Session::kNumTracks; ++track)
                        ctx.expect (host.stripStageControlsMatch (track, stage == Stage::Mixing),
                                    "tracking controls or aux sends are wrong on strip "
                                    + std::to_string (track + 1));
                (*next) (index + 1);
            }, "the stage click did not select its button and show only its view");
    };
    (*run) (0);
    return std::nullopt;
}

// ------------------------------------------------------------- registration

const ScenarioRegistrar stageViews { Scenario {
    "gui.stage_views_and_controls", { "gui", "stage" }, Needs::Engine | Needs::Gui,
    {}, {}, 25000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runStageViews (host, ctx); }
} };

std::optional<ScenarioResult> runClockFormats (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& transport = ctx.engine().getTransport();
    ctx.keep (session.timeDisplayMode);
    ctx.keep (session.tempoBpm);
    ctx.keep (session.beatsPerBar);
    ctx.keep (session.midiLearnPending);
    ctx.cleanup ([&session, points = session.tempoMap.points()]
                 { session.tempoMap.setPoints (points); });
    ctx.cleanup ([&transport, position = transport.getPlayhead(), state = transport.getState()]
    {
        transport.setPlayhead (position);
        transport.setState (state);
    });
    transport.setState (Transport::State::Stopped);
    session.tempoMap.clear();
    session.tempoBpm.store (120.0f);
    session.beatsPerBar.store (4);
    session.midiLearnPending.store (-1);
    session.timeDisplayMode.store ((int) TimeDisplayMode::Bars);
    if (ctx.engine().getCurrentSampleRate() <= 0.0)
        ctx.engine().prepareForSelfTest (ScenarioContext::kSampleRate, ScenarioContext::kBlockSize);
    transport.setPlayhead ((std::int64_t) std::llround (ctx.engine().getCurrentSampleRate() * 4.75));

    ctx.waitUntil ([&host] { return host.clockText() == "3.2.240"; }, 3000,
        [&host, &ctx, &session]
        {
            if (! ctx.expect (host.clickTimeFormat(), "the time-format button is not visible"))
            { ctx.complete (ctx.verdict()); return; }
            ctx.waitUntil ([&host] { return host.clockText() == "00:04.750"; }, 3000,
                [&host, &ctx, &session]
                {
                    ctx.expect (session.timeDisplayMode.load() == (int) TimeDisplayMode::Time,
                                "the clock changed without publishing Time mode");
                    if (! ctx.expect (host.clickTimeFormat(), "the time-format button disappeared"))
                    { ctx.complete (ctx.verdict()); return; }
                    ctx.waitUntil ([&host] { return host.clockText() == "3.2.240"; }, 3000,
                        [&ctx, &session]
                        {
                            ctx.expect (session.timeDisplayMode.load() == (int) TimeDisplayMode::Bars,
                                        "the second click did not restore Bars mode");
                            ctx.complete (ctx.verdict());
                        }, "the clock did not return to Bars.Beats.Ticks");
                }, "the clock did not display minutes, seconds and milliseconds");
        }, "the clock did not display the initial Bars.Beats.Ticks position");
    return std::nullopt;
}

const ScenarioRegistrar clockFormats { Scenario {
    "gui.clock_format_roundtrip", { "gui", "transport" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runClockFormats (host, ctx); }
} };

std::optional<ScenarioResult> runTrackShortcuts (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& transport = ctx.engine().getTransport();
    const auto stage = ctx.engine().getStage();
    ctx.cleanup ([&host, &transport, stage, state = transport.getState(),
                  position = transport.getPlayhead()]
    {
        switch (stage)
        {
            case AudioEngine::Stage::Recording: host.switchToStage (GuiHost::Stage::Recording); break;
            case AudioEngine::Stage::Mixing: host.switchToStage (GuiHost::Stage::Mixing); break;
            case AudioEngine::Stage::Aux: host.switchToStage (GuiHost::Stage::Aux); break;
            case AudioEngine::Stage::Mastering: host.switchToStage (GuiHost::Stage::Mastering); break;
        }
        transport.setPlayhead (position);
        transport.setState (state);
    });
    transport.setState (Transport::State::Stopped);
    ctx.keep (session.activeBank);
    ctx.keep (session.mcu.bank);
    ctx.cleanup (host.preserveKeyboardFocus());
    for (int index = 0; index < Session::kNumTracks; ++index)
    {
        auto& track = session.track (index);
        ctx.keep (track.mode);
        ctx.keep (track.strip.mute);
        ctx.cleanup ([&session, index, armed = track.recordArmed.load(), solo = track.strip.solo.load()]
        {
            session.setTrackArmed (index, armed);
            session.setTrackSoloed (index, solo);
        });
        track.mode.store ((int) Track::Mode::Midi);
        session.setTrackArmed (index, false);
        session.setTrackSoloed (index, false);
        track.strip.mute.store (false);
    }
    auto steps = std::make_shared<std::vector<Step>>();
    for (const auto targetStage : { GuiHost::Stage::Recording, GuiHost::Stage::Mixing })
    {
        steps->push_back ({ 0, [&host, targetStage] { host.switchToStage (targetStage); } });
        steps->push_back ({ 50, [&host, &ctx, &session]
        {
            for (int i = 0; i < Session::kNumTracks; ++i)
                ctx.expect (host.pressKey ("cursor left"), "left arrow did not focus a strip");
            for (int target = 0; target < Session::kNumTracks; ++target)
            {
                for (const auto key : { "A", "S", "X" })
                    for (const bool enabled : { true, false })
                    {
                        ctx.expect (host.pressKey (key), std::string (key) + " was not handled");
                        for (int index = 0; index < Session::kNumTracks; ++index)
                        {
                            const auto& track = session.track (index);
                            const bool selected = enabled && index == target;
                            ctx.expect (track.recordArmed.load() == (selected && key[0] == 'A')
                                        && track.strip.solo.load() == (selected && key[0] == 'S')
                                        && track.strip.mute.load() == (selected && key[0] == 'X'),
                                        std::string (key) + " changed the wrong state on track "
                                        + std::to_string (index + 1));
                        }
                        ctx.expect (session.anyTrackArmed() == (enabled && key[0] == 'A'),
                                    "arm shortcut left the armed-track counter stale");
                    }
                ctx.expect (host.pressKey ("cursor right"), "right arrow did not move strip focus");
            }
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar trackShortcuts { Scenario {
    "gui.keyboard_track_shortcuts", { "gui", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTrackShortcuts (host, ctx); }
} };
std::optional<ScenarioResult> runTapTempo (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    ctx.keep (session.tempoBpm);
    ctx.cleanup ([&session, points = session.tempoMap.points()]
                 { session.tempoMap.setPoints (points); });
    session.tempoMap.setPoints ({});
    // The expected tempo comes from the times the app stamped each tap with, not
    // from when this case's timer happened to fire, so a late step or a coarse
    // tap clock moves both sides alike. A gap past the two-second timeout starts
    // a new count, whether the case meant one or a loaded host made it.
    //
    // The app's clock is held to this case's own: B is handled inside
    // pressKey, on this thread, so each interval between the app's stamps
    // matches the interval between the readings taken just before each press
    // to within the clock's resolution, however late the steps themselves run.
    static constexpr std::int64_t kTapTimeoutMs = 2000;
    static constexpr double kClockToleranceMs = 50.0;
    auto stamps = std::make_shared<std::vector<std::int64_t>>();
    auto evicted = std::make_shared<bool> (false);
    auto previous = std::make_shared<std::pair<std::int64_t, double>> (0, 0.0);
    const auto tap = [&host, &ctx, &session, stamps, evicted, previous] (bool afterTimeout)
    {
        const float before = session.tempoBpm.load();
        const double pressedMs = std::chrono::duration<double, std::milli> (
            std::chrono::steady_clock::now().time_since_epoch()).count();
        ctx.expect (host.pressKey ("B"), "tap tempo shortcut was not handled");
        const auto at = host.lastTapMs();
        if (! ctx.expect (at > 0 && (stamps->empty() || at != stamps->back()), "TAP did not record the tap"))
            return;
        if (previous->first > 0)
        {
            const auto appInterval = static_cast<double> (at - previous->first);
            const auto ownInterval = pressedMs - previous->second;
            ctx.expect (std::abs (appInterval - ownInterval) < kClockToleranceMs,
                        "TAP timed a " + std::to_string (ownInterval) + " ms interval as "
                            + std::to_string (appInterval) + " ms");
        }
        *previous = { at, pressedMs };
        const bool restarted = stamps->empty() || at - stamps->back() > kTapTimeoutMs;
        if (restarted)
        {
            if (! stamps->empty() && ! afterTimeout)
                ctx.note ("a tap came " + std::to_string (at - stamps->back()) + " ms after the last, so the count restarted");
            stamps->clear();
        }
        else if (afterTimeout)
        {
            ctx.note ("the tap after the timeout came only " + std::to_string (at - stamps->back()) + " ms after the last");
        }
        stamps->push_back (at);
        if (stamps->size() < 2)
        {
            ctx.expect (std::abs (session.tempoBpm.load() - before) < 0.001f,
                        afterTimeout ? "a first tap after timeout changed the tempo"
                                     : "the first tap of a count changed the tempo");
            return;
        }
        const auto intervals = std::min<std::size_t> (4, stamps->size() - 1);
        if (stamps->size() > 5) *evicted = true;
        const auto duration = stamps->back() - (*stamps)[stamps->size() - 1 - intervals];
        const double expected = std::clamp (60000.0 * static_cast<double> (intervals) / static_cast<double> (duration),
                                            30.0, 300.0);
        ctx.expect (std::abs (session.tempoBpm.load() - expected) < 0.01,
                    "TAP did not average the most recent four intervals: "
                        + std::to_string (session.tempoBpm.load()) + " BPM against " + std::to_string (expected));
    };
    auto steps = std::make_shared<std::vector<Step>>();
    for (const int delay : { 2200, 250, 500, 750, 1000, 300 })
        steps->push_back ({ delay, [tap] { tap (false); } });
    steps->push_back ({ 0, [&ctx, evicted]
    {
        if (! *evicted && ctx.verdict().status == ScenarioStatus::Pass)
            ctx.complete (ScenarioResult::skip ("the host stalled past the tap timeout between taps, "
                                                "so no count reached a fifth interval to evict"));
    } });
    steps->push_back ({ 2200, [tap] { tap (true); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar tapTempo { Scenario {
    "gui.tap_tempo_intervals", { "gui", "keyboard", "transport" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTapTempo (host, ctx); }
} };
std::optional<ScenarioResult> runArmInputRefusal (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& track = session.track (0);
    ctx.keep (track.frozen);
    ctx.cleanup ([&session, &track, channels = session.deviceCaptureChannels.load(),
                  mode = track.mode.load(), input = track.inputSource.load(), armed = track.recordArmed.load()]
    {
        session.deviceCaptureChannels.store (channels);
        track.mode.store (mode);
        track.inputSource.store (input);
        session.setTrackArmed (0, armed);
    });
    const auto stage = ctx.engine().getStage();
    ctx.cleanup ([&host, stage]
    {
        drainModals (host);
        switch (stage)
        {
            case AudioEngine::Stage::Recording: host.switchToStage (GuiHost::Stage::Recording); break;
            case AudioEngine::Stage::Mixing: host.switchToStage (GuiHost::Stage::Mixing); break;
            case AudioEngine::Stage::Aux: host.switchToStage (GuiHost::Stage::Aux); break;
            case AudioEngine::Stage::Mastering: host.switchToStage (GuiHost::Stage::Mastering); break;
        }
    });
    host.switchToStage (GuiHost::Stage::Recording);
    session.deviceCaptureChannels.store (1);
    track.mode.store ((int) Track::Mode::Mono);
    track.frozen.store (false);
    session.setTrackArmed (0, false);
    auto* strip = host.strip (0);
    if (! ctx.expect (strip != nullptr, "the recording strip is missing")) return ctx.verdict();
    auto steps = std::make_shared<std::vector<Step>>();
    for (const int input : { 7, -1 })
    {
        steps->push_back ({ 100, [&ctx, &track, strip, input]
        {
            track.inputSource.store (input);
            ctx.expect (strip->clickArm(), "ARM is not visible");
        } });
        steps->push_back ({ 100, [&host, &ctx, &track, strip, input]
        {
            ctx.expect (! track.recordArmed.load() && ! strip->armLit(),
                        "a refused input left ARM enabled");
            const auto text = host.modalText();
            ctx.expect (text.find ("No input for " + track.name.toStdString()) != std::string::npos,
                        "the refusal did not name the track");
            const auto reason = input == -1 ? "has no input selected" : "records from In 8, and the audio device has 1 input(s)";
            ctx.expect (text.find (reason) != std::string::npos, "the refusal did not explain the missing input");
            ctx.expect (host.clickModalButton ("OK"), "the refusal has no usable OK button");
        } });
        steps->push_back ({ 100, [&host, &ctx, strip]
        {
            ctx.expect (strip->inputSettingsOpen(), "acknowledging the refusal did not open input settings");
            host.closeTopModal();
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar armInputRefusal { Scenario {
    "gui.arm_input_refusal", { "gui", "recording" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runArmInputRefusal (host, ctx); }
} };
std::optional<ScenarioResult> runAboutDetails (GuiHost& host, ScenarioContext& ctx)
{
    ctx.cleanup ([&host] { if (! host.modalStackEmpty()) host.closeTopModal(); });
    host.openAbout();
    ctx.waitUntil ([&host] { return ! host.modalStackEmpty(); }, 3000, [&host, &ctx]
    {
        const auto text = host.modalText();
        ctx.expect (text.find ("About Dusk Studio\nDusk Studio " JUCE_APPLICATION_VERSION_STRING) == 0,
                    "About does not show the application's version");
        ctx.expect (text.find ("Portastudio-style DAW.") != std::string::npos,
                    "About is missing the application description");
        ctx.expect (std::regex_search (text, std::regex (
                        "Built [A-Z][a-z]{2} [ 0-9][0-9] [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}")),
                    "About does not show its build date and time");
        if (! ctx.expect (host.clickModalButton ("OK"), "About has no usable OK button"))
        { ctx.complete (ctx.verdict()); return; }
        ctx.waitUntil ([&host] { return host.modalStackEmpty(); }, 3000,
                       [&ctx] { ctx.complete (ctx.verdict()); }, "OK did not dismiss About");
    }, "About did not open");
    return std::nullopt;
}

const ScenarioRegistrar aboutDetails { Scenario {
    "gui.about_details", { "gui", "messages" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAboutDetails (host, ctx); }
} };

std::optional<ScenarioResult> runUnarmedRecord (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& transport = ctx.engine().getTransport();
    ctx.cleanup ([&host, &transport, state = transport.getState(), position = transport.getPlayhead()]
    {
        if (! host.modalStackEmpty()) host.closeTopModal();
        transport.setPlayhead (position);
        transport.setState (state);
    });
    std::array<std::size_t, Session::kNumTracks> audioCounts {}, midiCounts {};
    for (int index = 0; index < Session::kNumTracks; ++index)
    {
        auto& track = session.track (index);
        ctx.cleanup ([&session, index, armed = track.recordArmed.load()] { session.setTrackArmed (index, armed); });
        session.setTrackArmed (index, false);
        audioCounts[static_cast<std::size_t> (index)] = track.regions.size();
        midiCounts[static_cast<std::size_t> (index)] = track.midiRegions.current().size();
    }
    transport.setState (Transport::State::Stopped);
    transport.setPlayhead (48000);
    ctx.expect (host.pressKey ("R", 'r'), "Record shortcut was not handled");
    ctx.waitUntil ([&host] { return ! host.modalStackEmpty(); }, 3000,
        [&host, &ctx, &session, &transport, audioCounts, midiCounts]
        {
            ctx.expect (transport.isStopped() && transport.getPlayhead() == 48000,
                        "unarmed Record changed transport state or position");
            ctx.expect (host.modalText().find ("Cannot record\nNo track is armed.") == 0,
                        "unarmed Record did not explain the refusal");
            for (int index = 0; index < Session::kNumTracks; ++index)
                ctx.expect (session.track (index).regions.size() == audioCounts[static_cast<std::size_t> (index)]
                            && session.track (index).midiRegions.current().size() == midiCounts[static_cast<std::size_t> (index)],
                            "unarmed Record changed a track's regions");
            if (! ctx.expect (host.clickModalButton ("OK"), "the refusal has no usable OK button"))
            { ctx.complete (ctx.verdict()); return; }
            ctx.waitUntil ([&host] { return host.modalStackEmpty(); }, 3000,
                           [&ctx] { ctx.complete (ctx.verdict()); }, "the refusal did not close");
        }, "unarmed Record did not show its refusal");
    return std::nullopt;
}

const ScenarioRegistrar unarmedRecord { Scenario {
    "gui.record_requires_armed_track", { "gui", "recording" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runUnarmedRecord (host, ctx); }
} };

std::optional<ScenarioResult> runWindowKeys (GuiHost& host, ScenarioContext& ctx)
{
    const bool fullscreen = host.fullScreen();
    const bool expanded = host.timelineViewMatches (true);
    ctx.cleanup ([&host, fullscreen, expanded]
    {
        host.closeTopModal();
        if (host.fullScreen() != fullscreen) host.pressKey ("F11");
        if (! host.timelineViewMatches (expanded)) host.pressKey ("T", 't');
    });
    // macOS animates into and out of full screen: the state flips when the
    // animation starts, and a toggle made before it ends is dropped. So each
    // F11 waits for the state and then for the animation.
    static constexpr int kFullScreenAnimationMs = 1500;
    const auto round = [&host, &ctx, fullscreen, expanded] (bool restore, std::function<void()> next)
    {
        ctx.expect (host.pressKey ("F11"), "F11 was not handled");
        const bool wanted = restore ? fullscreen : ! fullscreen;
        ctx.waitUntil ([&host, wanted] { return host.fullScreen() == wanted; }, 5000,
                       [&host, &ctx, expanded, restore, next]
                       {
                           ctx.later (kFullScreenAnimationMs, [&host, &ctx, expanded, restore, next]
                           {
                               ctx.expect (host.pressKey ("command + " + keyCodeDescription ('\\'), (char) 0x1c),
                                           "the timeline window shortcut was not handled");
                               ctx.later (100, [&host, &ctx, expanded, restore, next]
                               {
                                   ctx.expect (host.timelineViewMatches (restore ? expanded : ! expanded),
                                               "the timeline shortcut did not toggle visibility");
                                   next();
                               });
                           });
                       },
                       "F11 did not toggle the native window state");
    };
    const auto escapeCheck = [&host, &ctx]
    {
        auto steps = std::make_shared<std::vector<Step>>();
        steps->push_back ({ 100, [&host] { host.openAbout(); } });
        steps->push_back ({ 200, [&host, &ctx]
        {
            ctx.expect (! host.modalStackEmpty(), "About did not open for the Escape check");
            ctx.expect (host.pressPeerKey ("escape"), "the focused modal did not handle Escape");
        } });
        steps->push_back ({ 300, [&host, &ctx]
        { ctx.expect (host.modalStackEmpty(), "Escape left the modal open"); } });
        runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    };
    ctx.later (100, [round, escapeCheck] { round (false, [round, escapeCheck] { round (true, escapeCheck); }); });
    return std::nullopt;
}

const ScenarioRegistrar windowKeys { Scenario {
    "gui.window_keys", { "gui", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runWindowKeys (host, ctx); }
} };

// A prompt deciding what happens to the session takes no shortcuts. R and Space
// under the quit prompt, a session-switch prompt and the autosave recovery
// prompt leave the transport stopped, also once a click beside the prompt has
// handed the window the keyboard. A dialog that passes the transport keys on
// still does, and the keys work again once the prompts are answered.
std::optional<ScenarioResult> runPromptsWithholdKeys (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    if (! transport.isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
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

    const auto home = ctx.tempDir() / "Home" / "session.json";
    const auto other = ctx.tempDir() / "Other" / "session.json";
    std::filesystem::create_directories (home.parent_path());
    std::filesystem::create_directories (other.parent_path());
    auto& fader = session.track (0).strip.faderDb;
    const float savedFader = fader.load();
    session.track (0).mode.store ((int) Track::Mode::Midi);
    session.track (0).midiInputIndex.store (engine.getVirtualKeyboardInputIndex());
    session.track (0).midiRegions.publish (std::make_unique<std::vector<MidiRegion>>());
    if (! SessionSerializer::save (session, home) || ! SessionSerializer::save (session, other))
        return ScenarioResult::fail ("could not save the sessions");
    // An autosave that differs from Other's session.json, so opening Other asks.
    fader.store (savedFader - 6.0f);
    const bool autosaved = SessionSerializer::save (session, other.parent_path() / "session.json.autosave");
    fader.store (savedFader);
    if (! autosaved) return ScenarioResult::fail ("could not write Other's autosave");

    auto steps = std::make_shared<std::vector<Step>>();
    // One key per step: R then Space in a row would start a take and stop it
    // again, and the transport would read stopped either way.
    const auto triesKeys = [&host, &ctx, &engine, &transport, steps] (const std::string& where)
    {
        steps->push_back ({ 200, [&host] { host.pressPeerKey (keyCodeDescription ('r'), 'r'); } });
        steps->push_back ({ 200, [&host, &ctx, &engine, &transport, where]
        {
            if (! ctx.expect (transport.isStopped(), "R " + where + " started recording"))
                engine.stop();
            host.pressPeerKey ("spacebar", ' ');
        } });
        steps->push_back ({ 200, [&ctx, &engine, &transport, where]
        {
            if (! ctx.expect (transport.isStopped(), "Space " + where + " started the transport"))
                engine.stop();
        } });
    };
    const auto promptUp = [&host, &ctx] (const std::string& title, const std::string& where)
    {
        return ctx.expect (host.modalText().rfind (title, 0) == 0,
                           where + ": '" + host.modalText() + "' is up rather than " + title);
    };

    steps->push_back ({ 200, [&host, &ctx, &session, &fader, home, savedFader]
    {
        ctx.expect (host.openSession (home), "could not open the session");
        session.setTrackArmed (0, true);
        fader.store (savedFader - 3.0f);
        ctx.expect (host.requestQuit(), "the edited session did not ask before quitting");
    } });
    steps->push_back ({ 400, [promptUp] { promptUp ("Save changes before quitting?", "Quit"); } });
    triesKeys ("under the quit prompt");
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.clickModalBackdrop(), "could not click beside the quit prompt"); } });
    triesKeys ("after a click beside the quit prompt");
    steps->push_back ({ 100, [&host, &ctx, promptUp]
    {
        if (promptUp ("Save changes before quitting?", "After the keys"))
            ctx.expect (host.clickModalButton ("Cancel"), "the quit prompt did not offer Cancel");
    } });
    steps->push_back ({ 300, [&host, &ctx, other]
    {
        ctx.expect (host.modalStackEmpty(), "Cancel left '" + host.modalText() + "' up");
        host.requestSessionSwitch (other);
    } });
    steps->push_back ({ 400, [promptUp]
    { promptUp ("Save changes before opening another session?", "The session switch"); } });
    triesKeys ("under the session-switch prompt");
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.clickModalBackdrop(), "could not click beside the session-switch prompt"); } });
    triesKeys ("after a click beside the session-switch prompt");
    steps->push_back ({ 100, [&host, &ctx, promptUp]
    {
        if (promptUp ("Save changes before opening another session?", "After the keys"))
            ctx.expect (host.clickModalButton ("Cancel"), "the session-switch prompt did not offer Cancel");
    } });
    steps->push_back ({ 300, [&host, &ctx, promptUp, other]
    {
        ctx.expect (host.modalStackEmpty(), "Cancel left '" + host.modalText() + "' up");
        host.openSession (other);
        promptUp ("Recover from autosave?", "Opening a session with a newer autosave");
    } });
    triesKeys ("under the recovery prompt");
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.answerRecovery (GuiHost::Recovery::Cancel), "the recovery prompt was not up for Cancel"); } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.modalStackEmpty(), "Cancel left '" + host.modalText() + "' up");
        ctx.expect (host.pressKey ("shift + /", '?'), "the shortcuts key was not handled");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.shortcutsOpen(), "? did not open Keyboard Shortcuts");
        host.pressPeerKey ("spacebar", ' ');
    } });
    steps->push_back ({ 300, [&host, &ctx, &engine, &transport]
    {
        ctx.expect (transport.isPlaying(), "Space under Keyboard Shortcuts did not start playback");
        engine.stop();
        host.closeTopModal();
    } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.modalStackEmpty(), "Keyboard Shortcuts did not close");
        host.pressPeerKey (keyCodeDescription ('r'), 'r');
    } });
    steps->push_back ({ 300, [&ctx, &engine, &transport]
    {
        ctx.expect (transport.isRecording(), "R did not record once the prompts were answered");
        engine.stop();
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar promptsWithholdKeys { Scenario {
    "gui.prompts_withhold_transport_keys", { "gui", "keyboard", "session" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPromptsWithholdKeys (host, ctx); }
} };

std::optional<ScenarioResult> runRecordingUndoKey (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    auto& track = session.track (0);
    ctx.keep (track.mode);
    ctx.keep (track.midiInputIndex);
    ctx.keep (session.countInEnabled);
    for (int index = 0; index < Session::kNumTracks; ++index)
    {
        const bool armed = session.track (index).recordArmed.load();
        ctx.cleanup ([&session, index, armed] { session.setTrackArmed (index, armed); });
        session.setTrackArmed (index, false);
    }
    ctx.cleanup ([&engine, &transport, &track, regions = track.midiRegions.current(),
                  loop = transport.isLoopEnabled(), punch = transport.isPunchEnabled(),
                  position = transport.getPlayhead()]
    {
        const std::uint8_t off[] { 0x80, 64, 0 };
        engine.postVirtualKeyboardMidi (off, 3);
        engine.stop();
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (regions));
        transport.setLoopEnabled (loop);
        transport.setPunchEnabled (punch);
        transport.setPlayhead (position);
        engine.getUndoManager().clearUndoHistory();
    });
    engine.stop();
    transport.setPlayhead (0);
    transport.setLoopEnabled (false);
    transport.setPunchEnabled (false);
    session.countInEnabled.store (false);
    track.mode.store ((int) Track::Mode::Midi);
    track.midiInputIndex.store (engine.getVirtualKeyboardInputIndex());
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>>());
    session.setTrackArmed (0, true);
    engine.getUndoManager().clearUndoHistory();
    host.switchToStage (GuiHost::Stage::Recording);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressKey ("R", 'r'), "the Record key was not handled"); } });
    steps->push_back ({ 100, [&engine, &transport, &ctx]
    {
        ctx.expect (transport.isRecording(), "R did not start the armed MIDI recording");
        const std::uint8_t on[] { 0x90, 64, 100 };
        engine.postVirtualKeyboardMidi (on, 3);
    } });
    steps->push_back ({ 150, [&engine]
    {
        const std::uint8_t off[] { 0x80, 64, 0 };
        engine.postVirtualKeyboardMidi (off, 3);
    } });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressKey ("spacebar", ' '), "Space did not stop recording"); } });
    steps->push_back ({ 100, [&host, &ctx, &track, &transport]
    {
        ctx.expect (transport.isStopped(), "recording did not stop");
        const auto& regions = track.midiRegions.current();
        if (ctx.expect (regions.size() == 1, "recording did not commit exactly one MIDI region"))
            ctx.expect (regions[0].notes.size() == 1 && regions[0].notes[0].noteNumber == 64,
                        "recording did not retain the virtual keyboard note");
        ctx.expect (host.pressKey ("command + Z"), "the recording Undo shortcut was not handled");
    } });
    steps->push_back ({ 100, [&ctx, &track]
    {
        ctx.expect (track.midiRegions.current().empty(), "Cmd+Z did not undo the recorded take");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar recordingUndoKey { Scenario {
    "gui.recording_undo_key", { "gui", "recording", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runRecordingUndoKey (host, ctx); }
} };

std::optional<ScenarioResult> runRecordingSetupAlert (GuiHost& host, ScenarioContext& ctx, bool keyboard = false)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    ctx.keep (session.countInEnabled);
    session.countInEnabled.store (false);
    ctx.keep (session.deviceCaptureChannels);
    session.deviceCaptureChannels.store (1);
    for (int index = 0; index < Session::kNumTracks; ++index)
    {
        const bool armed = session.track (index).recordArmed.load();
        ctx.cleanup ([&session, index, armed] { session.setTrackArmed (index, armed); });
        session.setTrackArmed (index, false);
    }
    const auto audioDir = std::filesystem::u8path (session.getAudioDirectory().getFullPathName().toStdString());
    std::error_code error;
    if (! ctx.expect (std::filesystem::remove (audioDir, error) && ! error,
                     "the temporary audio directory was not empty")) return ctx.verdict();
    ctx.cleanup ([&host, &engine, audioDir]
    {
        engine.stop();
        host.closeTopModal();
        std::error_code ignored;
        std::filesystem::remove (audioDir, ignored);
        std::filesystem::create_directories (audioDir, ignored);
    });
    {
        std::ofstream obstruction (audioDir);
        obstruction << "not a directory";
        if (! ctx.expect (obstruction.good(), "could not create the audio-directory obstruction")) return ctx.verdict();
    }
    for (const int index : { 0, 2 })
    {
        auto& track = session.track (index);
        ctx.keep (track.mode);
        ctx.keep (track.inputSource);
        track.mode.store ((int) Track::Mode::Mono);
        track.inputSource.store (0);
        session.setTrackArmed (index, true);
    }
    if (! ctx.expect (keyboard ? host.pressKey ("R", 'r') : host.clickRecord(),
                     "the recording input was not handled")) return ctx.verdict();
    ctx.waitUntil ([&host] { return ! host.modalStackEmpty(); }, 3000,
        [&host, &ctx, &engine]
        {
            const auto text = host.modalText();
            ctx.expect (text.find ("Recording setup failed\n") == 0
                        && text.find ("Tracks 1, 3") != std::string::npos,
                        "the setup failure did not name the two failed tracks");
            ctx.expect (engine.getRecordManager().getLastSetupFailures() == std::vector<int> { 0, 2 },
                        "the dialog did not correspond to the failed writers");
            ctx.expect (text.find ("NOT capturing audio") != std::string::npos,
                        "the setup failure did not explain the capture loss");
            engine.stop();
            ctx.expect (host.clickModalButton ("OK"), "the setup failure has no usable OK button");
            ctx.waitUntil ([&host] { return host.modalStackEmpty(); }, 3000,
                           [&ctx] { ctx.complete (ctx.verdict()); }, "OK did not dismiss the setup failure");
        }, "failed writers did not produce a recording setup alert");
    return std::nullopt;
}

const ScenarioRegistrar recordingSetupAlert { Scenario {
    "gui.recording_setup_alert", { "gui", "recording", "messages" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runRecordingSetupAlert (host, ctx); }
} };

const ScenarioRegistrar recordingSetupAlertKey { Scenario {
    "gui.recording_setup_alert_key", { "gui", "recording", "messages", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runRecordingSetupAlert (host, ctx, true); }
} };

// A modal sits centred over a dimmed window and holds the keyboard; Escape, bare
// and wherever it lands, or a click on the dim outside it closes it. A native
// panel the modal pushes aside hands the keyboard back late, and the modal keeps
// it all the same.
std::optional<ScenarioResult> runModalDismissal (GuiHost& host, ScenarioContext& ctx)
{
    if (! host.modalStackEmpty() || host.virtualKeyboardOpen())
        return ScenarioResult::skip ("requires no modal and no virtual keyboard");
    ctx.cleanup ([&host]
    {
        host.closeVirtualKeyboard();
        drainModals (host);
    });
    auto steps = std::make_shared<std::vector<Step>>();
    const auto open = [&host, &ctx, steps]
    {
        steps->push_back ({ 200, [&host, &ctx]
        { ctx.expect (host.pressKey ("shift + /", '?'), "the shortcuts key was not handled"); } });
    };
    const auto closedBy = [&host, &ctx, steps] (const std::string& how)
    {
        steps->push_back ({ 200, [&host, &ctx, how]
        { ctx.expect (host.modalStackEmpty(), how + " did not close the modal"); } });
    };
    open();
    steps->push_back ({ 200, [&host, &ctx]
    {
        const auto layout = host.modalLayout();
        if (! ctx.expect (host.shortcutsOpen() && layout.size() == 7, "? did not open Keyboard Shortcuts")) return;
        ctx.expect (std::abs ((layout[0] + layout[2] / 2) - layout[4] / 2) <= 1
                    && std::abs ((layout[1] + layout[3] / 2) - layout[5] / 2) <= 1,
                    "the modal is not centred in the window");
        ctx.expect (layout[6] == 1, "the window behind the modal is not dimmed");
        ctx.expect (host.modalHasKeyboardFocus(), "the modal did not take the keyboard");
        ctx.expect (host.pressPeerKey ("escape"), "Escape was not handled");
    } });
    closedBy ("Escape");
    // Escape delivered to the window behind the modal rather than to the modal.
    open();
    steps->push_back ({ 200, [&host, &ctx]
    {
        if (! ctx.expect (host.shortcutsOpen(), "? did not reopen Keyboard Shortcuts")) return;
        host.pressKey ("shift + escape");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    {
        if (! ctx.expect (host.shortcutsOpen(), "Shift+Escape at the window closed the modal")) return;
        ctx.expect (host.pressKey ("escape"), "Escape at the window was not handled");
    } });
    closedBy ("Escape at the window");
    open();
    steps->push_back ({ 200, [&host, &ctx]
    {
        if (! ctx.expect (host.shortcutsOpen(), "? did not reopen Keyboard Shortcuts")) return;
        ctx.expect (host.clickModalBackdrop(), "the dimmed window did not take a click");
    } });
    closedBy ("a click outside the modal");
   #if DUSKSTUDIO_HAS_NATIVE_UI
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.pressKey ("K", 'k'), "K was not handled"); } });
    steps->push_back ({ 600, [&host, &ctx]
    {
        if (! ctx.expect (host.virtualKeyboardOpen(), "K did not open the virtual keyboard")) return;
        host.raiseAlert ("Audio device lost", "The audio device went away.");
    } });
    steps->push_back ({ 600, [&host, &ctx]
    {
        if (! ctx.expect (! host.modalStackEmpty() && ! host.virtualKeyboardOpen(),
                          "the modal did not push the virtual keyboard aside")) return;
        ctx.expect (host.modalHasKeyboardFocus(), "the closing virtual keyboard took the keyboard from the modal");
        ctx.expect (host.pressPeerKey ("escape"), "Escape was not handled");
    } });
    closedBy ("Escape after the virtual keyboard closed");
   #endif
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar modalDismissal { Scenario {
    "gui.modal_escape_and_backdrop", { "gui", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runModalDismissal (host, ctx); }
} };

// With the UI scale above 1 the window can be shorter than the shortcut list,
// which its minimum at scale 1 rules out. The rows squeeze to fit: each keeps the
// height the panel chose, inside the panel, clear of every other.
std::optional<ScenarioResult> runShortcutsShortWindow (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("the UI scale needs the native UI");
   #else
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no modal");
    const auto launch = host.mainWindowSize();
    if (launch.size() < 4) return ScenarioResult::fail ("the main window reported no size");
    const auto originalScale = static_cast<float> (host.uiScale());
    ctx.cleanup ([&host, originalScale, launch]
    {
        drainModals (host);
        host.restoreUiScale (originalScale);
        host.resizeMainWindow (launch[0], launch[1]);
    });
    // Short enough to squeeze the rows, tall enough for the smallest of them.
    static constexpr float kScale = 1.25f;
    static constexpr int kShortHeight = 680;
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&host] { host.restoreUiScale (kScale); } });
    // A window manager that holds the window to its minimum leaves nothing to
    // check: the list then always fits.
    steps->push_back ({ 100, [&host, &ctx]
    {
        const auto size = host.mainWindowSize();
        if (size.size() < 4 || ! host.resizeMainWindow (size[0], kShortHeight))
            ctx.complete (ScenarioResult::skip ("the window cannot be made shorter than the shortcut list here"));
    }, [&host] { return std::abs (host.uiScale() - kScale) < 0.001; }, "the UI scale did not change" });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressKey ("shift + /", '?'), "the shortcuts key was not handled"); } });
    steps->push_back ({ 0, [&host, &ctx]
    {
        const auto layout = host.shortcutsLayout();
        if (! ctx.expect (layout.size() > 5 && (layout.size() - 5) % 4 == 0, "the shortcut panel reported no rows"))
            return;
        const int width = layout[0], height = layout[1], rowH = layout[2], fullRowH = layout[3], minRowH = layout[4];
        ctx.expect (rowH < fullRowH, "the rows kept their full height in a " + std::to_string (height)
                    + "-unit panel");
        ctx.expect (rowH >= minRowH, "the rows squeezed below " + std::to_string (minRowH) + " units");
        struct Row { int x, y, w, h; };
        std::vector<Row> rows;
        for (std::size_t at = 5; at + 3 < layout.size(); at += 4)
            rows.push_back ({ layout[at], layout[at + 1], layout[at + 2], layout[at + 3] });
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            const auto& r = rows[i];
            if (! ctx.expect (r.h == rowH && r.x >= 0 && r.y >= 0 && r.x + r.w <= width && r.y + r.h <= height,
                              "shortcut row " + std::to_string (i) + " at y " + std::to_string (r.y) + ", "
                                  + std::to_string (r.h) + " high, is not a whole row inside the "
                                  + std::to_string (height) + "-unit panel"))
                return;
            for (std::size_t j = i + 1; j < rows.size(); ++j)
            {
                const auto& o = rows[j];
                if (! ctx.expect (r.x + r.w <= o.x || o.x + o.w <= r.x || r.y + r.h <= o.y || o.y + o.h <= r.y,
                                  "shortcut rows " + std::to_string (i) + " and " + std::to_string (j) + " overlap"))
                    return;
            }
        }
    }, [&host] { return host.shortcutsOpen(); }, "? did not open Keyboard Shortcuts" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar shortcutsShortWindow { Scenario {
    "gui.shortcuts_short_window", { "gui", "keyboard", "settings" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runShortcutsShortWindow (host, ctx); }
} };

struct HoldSample
{
    double heldMs;
    std::int64_t playhead;
};

std::string wholeText (double value) { return std::to_string (std::llround (value)); }

// The scrub runs on a 20 Hz timer whose first tick past the threshold only
// starts the clock; every later tick moves the playhead ten times the time
// since the tick before, so a late tick moves it further instead of losing
// time. The samples pin each move between two reads, and every judgement below
// is drawn from those bounds rather than from when a step happened to run.
void judgeHoldScrub (ScenarioContext& ctx, const std::string& title, int direction,
                     double samplesPerMs, const std::vector<HoldSample>& samples,
                     std::string& undecided)
{
    static constexpr double kThresholdMs = 400.0;
    static constexpr double kTickMs = 50.0;
    // Both the app and the samples read millisecond clocks.
    static constexpr double kClockSlackMs = 3.0;
    // A longer stall around the threshold lets a late tick make a 300 or
    // 600 ms threshold look like 400.
    static constexpr double kMaxStallMs = 60.0;
    static constexpr double kMaxRateSpread = 1.1;
    static constexpr double kRateTolerance = 0.02;

    const auto decide = [&undecided] (std::string reason)
    {
        if (undecided.empty()) undecided = std::move (reason);
    };
    const auto from = samples.front().playhead;
    const auto end = samples.back().playhead;
    const auto travelled = [from, direction] (const HoldSample& s)
    { return (double) direction * (double) (s.playhead - from); };

    for (std::size_t i = 1; i < samples.size(); ++i)
    {
        if (! ctx.expect (travelled (samples[i]) >= travelled (samples[i - 1]),
                          title + " moved the playhead the wrong way "
                              + wholeText (samples[i].heldMs) + " ms into the hold"))
            return;
    }
    const auto moved = std::find_if (samples.begin(), samples.end(),
                                     [from] (const HoldSample& s) { return s.playhead != from; });
    if (! ctx.expect (moved != samples.end(),
                      title + " held " + wholeText (samples.back().heldMs) + " ms did not scrub"))
        return;
    const auto& still = *(moved - 1);
    const auto settled = std::find_if (moved, samples.end(),
                                       [end] (const HoldSample& s) { return s.playhead == end; });

    // A move is only possible once a tick has seen the threshold pass.
    ctx.expect (moved->heldMs >= kThresholdMs - kClockSlackMs,
                title + " moved the playhead " + wholeText (moved->heldMs)
                    + " ms into the hold, before a 400 ms threshold could");

    // The distance of the first move dates the tick that started the scrub
    // clock. That tick comes at most one period after the threshold, plus a
    // stall of the message thread between the threshold and the first move,
    // which holds up the samples too and shows as a gap between them.
    double stallMs = 0.0;
    double stallAtMs = moved->heldMs;
    for (auto s = moved; s != samples.begin(); --s)
    {
        const double gap = s->heldMs - (s - 1)->heldMs;
        if (gap > stallMs)
        {
            stallMs = gap;
            stallAtMs = s->heldMs;
        }
        if ((s - 1)->heldMs < kThresholdMs) break;
    }
    const double coveredMs = travelled (*moved) / samplesPerMs;
    const double startedLo = still.heldMs - coveredMs;
    const double startedHi = moved->heldMs - coveredMs;
    const double latestMs = kThresholdMs + kTickMs + stallMs + kClockSlackMs;
    ctx.note (title + ": " + std::to_string (samples.size()) + " samples, longest gap near the threshold "
              + wholeText (stallMs) + " ms at " + wholeText (stallAtMs) + " ms, first move at "
              + wholeText (moved->heldMs) + " ms, scrub clock started " + wholeText (startedLo)
              + " to " + wholeText (startedHi) + " ms into the hold");
    ctx.expect (startedHi >= kThresholdMs - kClockSlackMs,
                title + " started scrubbing by " + wholeText (startedHi)
                    + " ms into the hold, before the 400 ms threshold");
    ctx.expect (startedLo <= latestMs,
                title + " started scrubbing no sooner than " + wholeText (startedLo)
                    + " ms into the hold, later than the first tick past a 400 ms threshold can come ("
                    + wholeText (latestMs) + " ms)");
    if (stallMs > kMaxStallMs)
        decide (title + " went " + wholeText (stallMs) + " ms without a sample, "
                + wholeText (stallAtMs) + " ms into the hold, too long to pin the threshold");

    const double shortestMs = settled == moved ? 0.0 : (settled - 1)->heldMs - moved->heldMs;
    if (shortestMs <= 0.0)
    {
        decide (title + " made its whole scrub between two samples, too few to time it");
        return;
    }
    // As a multiple of 10x, over the span from the first move to the last.
    const double distance = travelled (*settled) - travelled (*moved);
    const double slowest = distance / ((settled->heldMs - still.heldMs) * samplesPerMs);
    const double fastest = distance / (shortestMs * samplesPerMs);
    ctx.note (title + " scrubbed at " + std::to_string (10.0 * slowest) + " to "
              + std::to_string (10.0 * fastest) + " times real time");
    ctx.expect (slowest <= 1.0 + kRateTolerance && fastest >= 1.0 - kRateTolerance,
                title + " scrubbed at " + std::to_string (10.0 * slowest) + " to "
                    + std::to_string (10.0 * fastest) + " times real time, not 10x");
    if (fastest / slowest > kMaxRateSpread)
        decide (title + " could only pin its scrub rate between " + std::to_string (10.0 * slowest)
                + " and " + std::to_string (10.0 * fastest) + " times real time");
}

// Held past 400 ms, Rewind and Forward scrub the stopped playhead at ten times
// real time, and the release that ends a scrub is not also read as a tap. A
// stall too long to judge the threshold or the rate skips instead of failing.
std::optional<ScenarioResult> runTransportHoldScrubs (GuiHost& host, ScenarioContext& ctx)
{
    using Clock = std::chrono::steady_clock;
    static constexpr int kSampleMs = 10;
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    const double rate = engine.getCurrentSampleRate();
    if (! transport.isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    if (rate <= 0.0) return ScenarioResult::skip ("requires a running audio device");
    const auto original = transport.getPlayhead();
    ctx.cleanup ([&host, &transport, original]
    {
        host.pressTitledControl ("Rewind", false);
        host.pressTitledControl ("Fast forward", false);
        transport.setPlayhead (original);
    });
    const auto start = (std::int64_t) (rate * 120.0);
    transport.setPlayhead (start);

    struct Hold
    {
        Clock::time_point pressed;
        bool released = false;
        std::vector<HoldSample> samples;
    };
    auto undecided = std::make_shared<std::string>();
    auto steps = std::make_shared<std::vector<Step>>();
    const auto hold = [&host, &ctx, &transport, rate, steps, undecided] (const std::string& title, int direction)
    {
        auto state = std::make_shared<Hold>();
        const auto sample = [&transport, state]
        {
            const double heldMs = std::chrono::duration<double, std::milli> (Clock::now() - state->pressed).count();
            state->samples.push_back ({ heldMs, transport.getPlayhead() });
        };
        auto poll = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> weakPoll = poll;
        *poll = [&ctx, state, sample, weakPoll]
        {
            if (state->released) return;
            sample();
            if (auto self = weakPoll.lock())
                ctx.later (kSampleMs, [self] { (*self)(); });
        };
        steps->push_back ({ 200, [&host, &ctx, title, state, poll]
        {
            state->pressed = Clock::now();
            (*poll)();
            ctx.expect (host.pressTitledControl (title, true), title + " did not take a press");
        } });
        steps->push_back ({ 1400, [&host, &ctx, &transport, title, state, sample]
        {
            sample();
            state->released = true;
            ctx.expect (host.pressTitledControl (title, false), title + " did not take a release");
            ctx.expect (transport.getPlayhead() == state->samples.back().playhead,
                        title + " jumped on the release that ended the scrub");
        } });
        steps->push_back ({ 300, [&ctx, &transport, title, direction, rate, state, undecided]
        {
            ctx.expect (transport.getPlayhead() == state->samples.back().playhead,
                        title + " moved the playhead after the release that ended the scrub");
            ctx.expect (transport.isStopped(), title + " scrubbing started the transport");
            judgeHoldScrub (ctx, title, direction, 10.0 * rate / 1000.0, state->samples, *undecided);
        } });
    };
    hold ("Rewind", -1);
    hold ("Fast forward", 1);
    runSteps (ctx, steps, [&ctx, undecided]
    {
        auto result = ctx.verdict();
        if (result.status == ScenarioStatus::Pass && ! undecided->empty())
            result = ScenarioResult::skip (*undecided);
        ctx.complete (std::move (result));
    });
    return std::nullopt;
}

const ScenarioRegistrar transportHoldScrubs { Scenario {
    "gui.transport_hold_scrubs", { "gui", "transport" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTransportHoldScrubs (host, ctx); }
} };

std::optional<ScenarioResult> runTempoEntry (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    if (! ctx.expect (transport.isStopped(), "tempo entry requires a stopped fixture")) return ctx.verdict();
    ctx.cleanup ([&host, &engine, &session, &transport, points = session.tempoMap.points(),
                  bpm = session.tempoBpm.load(), position = transport.getPlayhead()]
    {
        host.closeTopModal();
        session.tempoBpm.store (bpm);
        engine.setTempoPoints (points);
        transport.setPlayhead (position);
        engine.getUndoManager().clearUndoHistory();
    });
    session.tempoBpm.store (120.0f);
    engine.setTempoPoints ({});
    auto steps = std::make_shared<std::vector<Step>>();
    for (const bool mapped : { false, true })
    {
        steps->push_back ({ 500, [&host, &ctx, &engine, &transport, mapped]
        {
            if (mapped) engine.setTempoPoints ({ { 0, 100.0f }, { 48000, 120.0f }, { 96000, 140.0f } });
            transport.setPlayhead (mapped ? 72000 : 0);
            ctx.expect (host.doubleClickTempo(), "the BPM readout was unavailable for double-click");
        } });
        steps->push_back ({ 200, [&host, &ctx, mapped]
        {
            if (! ctx.expect (host.focusModalTextInput(), "double-clicking BPM did not open a text input")) return;
            ctx.expect (host.pressPeerKey ("command + A"), "the tempo input did not accept Select All");
            const std::string text = mapped ? "127.6" : "133.5";
            for (const char character : text)
                ctx.expect (host.pressPeerKey (std::string (1, character), character), "the tempo input rejected a character");
            ctx.expect (host.pressPeerKey ("return"), "the tempo input did not accept Return");
        } });
        steps->push_back ({ 300, [&host, &ctx, &session, &transport, mapped]
        {
            ctx.expect (host.modalStackEmpty(), "accepting tempo left the prompt open");
            if (! mapped)
                ctx.expect (session.tempoMap.empty() && std::abs (session.tempoBpm.load() - 133.5f) < 0.001f,
                            "constant-tempo entry did not retain its fractional BPM");
            else
            {
                const auto points = session.tempoMap.points();
                if (! ctx.expect (points.size() == 3, "editing mapped tempo changed the point count")) return;
                ctx.expect (points[0].timelineSamples == 0 && points[1].timelineSamples == 48000
                            && points[2].timelineSamples == 96000, "editing BPM moved a tempo point");
                ctx.expect (std::abs (points[0].bpm - 100.0f) < 0.001f
                            && std::abs (points[1].bpm - 127.6f) < 0.001f
                            && std::abs (points[2].bpm - 140.0f) < 0.001f
                            && std::abs (session.tempoBpm.load() - 100.0f) < 0.001f,
                            "BPM entry did not edit only the point governing the playhead");
                ctx.expect (transport.getPlayhead() == 72000, "editing BPM moved the stopped playhead");
            }
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar tempoEntry { Scenario {
    "gui.tempo_entry", { "gui", "transport" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTempoEntry (host, ctx); }
} };

std::optional<ScenarioResult> runPunchMenu (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no modal");
    ctx.keep (session.preRollEnabled);
    ctx.keep (session.postRollEnabled);
    ctx.keep (session.preRollSeconds);
    ctx.keep (session.postRollSeconds);
    ctx.cleanup ([&host] { drainModals (host); });
    session.preRollEnabled.store (false);
    session.postRollEnabled.store (false);
    session.preRollSeconds.store (0.0f);
    session.postRollSeconds.store (0.0f);
    const bool punch = ctx.engine().getTransport().isPunchEnabled();
    auto steps = std::make_shared<std::vector<Step>>();
    for (const bool enabled : { true, false })
        for (const bool post : { false, true })
        {
            steps->push_back ({ 500, [&host, &ctx]
            { ctx.expect (host.rightClickPunch(), "the Punch button was unavailable for right-click"); } });
            steps->push_back ({ 150, [&host, &ctx, post]
            { ctx.expect (host.clickModalAt (0.5f, (post ? 112.0f : 48.0f) / 166.0f), "the roll enable row was unavailable"); } });
            steps->push_back ({ 150, [&host, &ctx, &session, post, enabled, punch]
            {
                ctx.expect ((post ? session.postRollEnabled.load() : session.preRollEnabled.load()) == enabled,
                            "the Punch menu did not toggle the selected roll mode");
                ctx.expect (ctx.engine().getTransport().isPunchEnabled() == punch,
                            "a Punch context-menu gesture toggled punch recording");
                ctx.expect (host.modalStackEmpty(), "choosing a roll enable row left its menu open");
            } });
        }
    for (const bool off : { false, true })
        for (const bool post : { false, true })
        {
            steps->push_back ({ 500, [&host, &ctx]
            { ctx.expect (host.rightClickPunch(), "the Punch button was unavailable for its preset menu"); } });
            steps->push_back ({ 150, [&host, &ctx, post]
            { ctx.expect (host.clickModalAt (0.5f, (post ? 144.0f : 80.0f) / 166.0f), "the roll-duration submenu was unavailable"); } });
            steps->push_back ({ 150, [&host, &ctx, post, off]
            {
                const int row = off ? 0 : post ? 4 : 2;
                ctx.expect (host.clickModalAt (0.5f, (static_cast<float> (row) + 0.5f) / 6.0f),
                            "the roll-duration preset was unavailable");
            } });
            steps->push_back ({ 150, [&host, &ctx, &session, post, off]
            {
                const float expected = off ? 0.0f : post ? 5.0f : 2.0f;
                ctx.expect (std::abs ((post ? session.postRollSeconds.load() : session.preRollSeconds.load()) - expected) < 0.001f,
                            "the Punch submenu did not set the selected roll duration");
                ctx.expect (host.modalStackEmpty(), "choosing a preset left its parent or submenu open");
            } });
        }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar punchMenu { Scenario {
    "gui.punch_menu", { "gui", "transport" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPunchMenu (host, ctx); }
} };

// How much brighter a lit transport toggle paints than an unlit one, on average.
constexpr double kToggleOnLift = 40.0;

// The page buttons and the timeline toolbar share the transport row with MIDI Merge,
// centred under the stage tabs and pushed left toward the toggle when the window is
// too narrow to centre them. However far they are pushed, the toggle stays uncovered
// and takes a click on its centre, and its on state reads apart from off.
std::optional<ScenarioResult> runMidiMergeTransportLayout (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    if (! ctx.engine().getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    const auto window = host.mainWindowSize();
    if (window.size() < 4) return ScenarioResult::fail ("the main window reported no size");
    ctx.keep (session.midiMergeEnabled);
    keepStage (host, ctx);
    const bool timeline = host.setTimelineShown (false);
    ctx.cleanup ([&host, window, timeline]
    {
        host.resizeMainWindow (window[0], window[1]);
        host.setTimelineShown (timeline);
    });
    session.midiMergeEnabled.store (false);
    host.switchToStage (GuiHost::Stage::Recording);
    const int height = std::max (window[1], window[3]);
    std::vector<int> widths { window[2], std::max (window[2], 1440), window[0] };
    widths.erase (std::unique (widths.begin(), widths.end()), widths.end());
    auto steps = std::make_shared<std::vector<Step>>();
    for (const int width : widths)
        for (const bool shown : { false, true })
        {
            const auto where = std::to_string (width) + " px with the timeline " + (shown ? "open" : "closed");
            steps->push_back ({ 0, [&host, &ctx, width, height, shown, where]
            {
                ctx.expect (host.resizeMainWindow (width, height), "the window could not be sized to " + where);
                host.setTimelineShown (shown);
            } });
            steps->push_back ({ 300, [&host, &ctx, where]
            {
                for (const auto& line : host.midiMergeObstructions())
                    ctx.expect (false, "MIDI Merge at " + where + ": " + line);
            }, [&host, width]
            {
                const auto size = host.mainWindowSize();
                return size.size() >= 1 && size[0] == width;
            }, "the window never took " + where });
        }
    const auto lit = [&host] (bool on) { return [&host, on] { return host.midiMergeControls()[0] == (on ? 1 : 0); }; };
    auto off = std::make_shared<std::uint32_t> (0);
    steps->push_back ({ 0, [&host, &ctx, off]
    {
        *off = host.midiMergePaint();
        ctx.expect (host.clickMidiMerge(), "the transport MIDI Merge toggle was not clickable");
    }, lit (false), "the transport toggle did not start off" });
    steps->push_back ({ 100, [&host, &ctx, &session, off]
    {
        ctx.expect (session.midiMergeEnabled.load(), "the transport toggle did not enable MIDI Merge");
        const auto on = host.midiMergePaint();
        const auto brightness = [] (std::uint32_t argb)
        {
            return 0.299 * ((argb >> 16) & 0xff) + 0.587 * ((argb >> 8) & 0xff) + 0.114 * (argb & 0xff);
        };
        char text[96];
        std::snprintf (text, sizeof text, "off paints #%06x and on #%06x", (unsigned) (*off & 0xffffff),
                       (unsigned) (on & 0xffffff));
        ctx.expect (*off != 0 && on != 0, std::string ("the toggle painted nothing: ") + text);
        ctx.expect (brightness (on) - brightness (*off) >= kToggleOnLift,
                    std::string ("MIDI Merge on does not stand out from off: ") + text);
        ctx.expect (host.clickMidiMerge(), "the transport MIDI Merge toggle did not take a second click");
    }, lit (true), "the transport toggle did not turn on" });
    steps->push_back ({ 0, [&ctx, &session]
    { ctx.expect (! session.midiMergeEnabled.load(), "the second click did not turn MIDI Merge off"); },
        lit (false), "the transport toggle did not turn back off" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar midiMergeTransportLayout { Scenario {
    "gui.midi_merge_transport_layout", { "gui", "transport", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMidiMergeTransportLayout (host, ctx); }
} };

std::optional<ScenarioResult> runTransportKeys (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    if (! transport.isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    ctx.keep (session.metronomeEnabled);
    ctx.keep (session.countInEnabled);
    ctx.keep (session.timeDisplayMode);
    ctx.keep (session.beatsPerBar);
    ctx.keep (session.beatUnit);
    ctx.cleanup ([&host] { drainModals (host); });
    ctx.cleanup ([&host, &engine, &session, &transport, markers = session.getMarkers(),
                  at = transport.getPlayhead(), loop = transport.isLoopEnabled(),
                  loopStart = transport.getLoopStart(), loopEnd = transport.getLoopEnd(),
                  punch = transport.isPunchEnabled(),
                  punchIn = transport.getPunchIn(), punchOut = transport.getPunchOut()]
    {
        engine.stop();
        session.getMarkers() = markers;
        transport.setPlayhead (at);
        transport.setLoopRange (loopStart, loopEnd);
        transport.setLoopEnabled (loop);
        transport.setPunchRange (punchIn, punchOut);
        transport.setPunchEnabled (punch);
        if (host.tunerOpen()) host.pressKey (keyCodeDescription ('u'), 'u');
    });
    const auto rate = engine.getCurrentSampleRate();
    const auto second = (std::int64_t) std::llround (rate);
    session.getMarkers().clear();
    transport.setLoopEnabled (false);
    transport.setPunchEnabled (false);
    session.metronomeEnabled.store (false);
    session.countInEnabled.store (false);
    session.timeDisplayMode.store ((int) TimeDisplayMode::Bars);
    session.beatsPerBar.store (4);
    session.beatUnit.store (4);

    // The keys as X11 sends them - an unmodified letter arrives lowercase, and
    // a shifted bracket arrives as the shifted glyph.
    const auto plain = [&host] (char letter)
    { return host.pressKey (keyCodeDescription (letter), letter); };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&ctx, &host, &engine, &transport, second]
    {
        transport.setPlayhead (second * 3);
        ctx.expect (host.pressKey ("home"), "Home was not handled");
        ctx.expect (transport.getPlayhead() == 0, "Home did not seek to the start");
        engine.play();
    } });
    steps->push_back ({ 200, [&ctx, &host, &transport]
    {
        ctx.expect (transport.isPlaying(), "the transport did not start for the stop-and-rewind key");
        ctx.expect (host.pressKey (".", '.'), "the stop-and-rewind key was not handled");
        ctx.expect (transport.isStopped() && transport.getPlayhead() == 0,
                    "'.' did not stop the transport and rewind to the start");
    } });
    steps->push_back ({ 100, [&ctx, &host, &transport, plain, second]
    {
        transport.setPlayhead (second);
        ctx.expect (host.pressKey ("[", '['), "the loop-in key was not handled");
        transport.setPlayhead (second * 3);
        ctx.expect (host.pressKey ("]", ']'), "the loop-out key was not handled");
        ctx.expect (transport.getLoopStart() == second && transport.getLoopEnd() == second * 3
                    && transport.isLoopEnabled(),
                    "the bracket keys did not place and arm the loop range");
        ctx.expect (plain ('l'), "the loop toggle was not handled");
        ctx.expect (! transport.isLoopEnabled(), "L did not turn the loop off");
        ctx.expect (plain ('l') && transport.isLoopEnabled(), "L did not turn the loop back on");
    } });
    steps->push_back ({ 100, [&ctx, &host, &transport, plain, second]
    {
        transport.setPlayhead (second * 2);
        ctx.expect (host.pressKey ("shift + " + keyCodeDescription ('{'), '{'), "the punch-in key was not handled");
        transport.setPlayhead (second * 5);
        ctx.expect (host.pressKey ("shift + " + keyCodeDescription ('}'), '}'), "the punch-out key was not handled");
        ctx.expect (transport.getPunchIn() == second * 2 && transport.getPunchOut() == second * 5
                    && transport.isPunchEnabled(),
                    "the shifted bracket keys did not place and arm the punch range");
        ctx.expect (plain ('p'), "the punch toggle was not handled");
        ctx.expect (! transport.isPunchEnabled(), "P did not turn punch off");
        ctx.expect (plain ('p') && transport.isPunchEnabled(), "P did not turn punch back on");
    } });
    steps->push_back ({ 100, [&ctx, &host, &session, plain]
    {
        ctx.expect (plain ('c') && session.metronomeEnabled.load(), "C did not turn the metronome on");
        ctx.expect (plain ('c') && ! session.metronomeEnabled.load(), "C did not turn the metronome off");
        ctx.expect (host.pressKey ("shift + C", 'C') && session.countInEnabled.load(),
                    "Shift+C did not turn the count-in on");
        ctx.expect (host.pressKey ("shift + C", 'C') && ! session.countInEnabled.load(),
                    "Shift+C did not turn the count-in off");
    } });
    steps->push_back ({ 100, [&ctx, &session, plain]
    {
        ctx.expect (plain ('f') && session.timeDisplayMode.load() == (int) TimeDisplayMode::Time,
                    "F did not switch the clock to minutes and seconds");
        ctx.expect (plain ('f') && session.timeDisplayMode.load() == (int) TimeDisplayMode::Bars,
                    "F did not switch the clock back to bars and beats");
    } });
    steps->push_back ({ 100, [&ctx, plain, &host]
    {
        ctx.expect (plain ('u'), "the tuner key was not handled");
        ctx.expect (host.tunerOpen(), "U did not open the tuner");
        ctx.expect (plain ('u'), "the tuner key was not handled a second time");
        ctx.expect (! host.tunerOpen(), "U did not close the tuner");
    } });
    steps->push_back ({ 100, [&ctx, &host]
    { ctx.expect (host.pressKey ("shift + M", 'M'), "the time-signature key was not handled"); } });
    steps->push_back ({ 200, [&ctx, &host]
    { ctx.expect (host.clickContextMenuItem ("3/4"), "the time-signature menu did not offer 3/4"); } });
    steps->push_back ({ 200, [&ctx, &host, &session, plain, &transport, second]
    {
        ctx.expect (session.beatsPerBar.load() == 3 && session.beatUnit.load() == 4,
                    "the time-signature menu did not apply the chosen signature");
        ctx.expect (host.modalStackEmpty(), "the time-signature menu stayed open");
        transport.setPlayhead (second * 4);
        ctx.expect (plain ('m'), "the marker key was not handled");
        const auto& markers = session.getMarkers();
        if (ctx.expect (markers.size() == 1, "M did not drop exactly one marker"))
            ctx.expect (markers[0].timelineSamples == second * 4, "M did not drop the marker at the playhead");
    } });
    steps->push_back ({ 200, [&ctx, &host]
    {
        ctx.expect (! host.modalStackEmpty(), "M did not offer to name the new marker");
        ctx.expect (host.pressPeerKey ("escape"), "the marker name prompt did not take Escape");
    } });
    runSteps (ctx, steps, [&host, &ctx, &session]
    {
        ctx.waitUntil ([&host] { return host.modalStackEmpty(); }, 3000,
            [&ctx, &session]
            {
                ctx.expect (session.getMarkers().size() == 1, "Escape on the name prompt removed the marker");
                ctx.complete (ctx.verdict());
            }, "Escape did not close the marker name prompt");
    });
    return std::nullopt;
}

// The tuner covers the timeline, so it keeps the keys a dialog keeps: Delete
// leaves the selected region it hides alone and M drops no marker, while L
// still reaches the loop. Escape closes it, and so does U again.
std::optional<ScenarioResult> runTunerKeys (GuiHost& host, ScenarioContext& ctx)
{
    AudioRegion region;
    if (auto early = seedTapeRegion (host, ctx, region)) return early;
    auto& session = ctx.session();
    auto& transport = ctx.engine().getTransport();
    ctx.cleanup ([&host] { if (host.tunerOpen()) host.pressPeerKey (keyCodeDescription ('u'), 'u'); });
    const auto plain = [&host] (char letter) { return host.pressPeerKey (keyCodeDescription (letter), letter); };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickAudioRegion (0, 0), "could not select the region with a timeline click"); } });
    steps->push_back ({ 200, [&host, &ctx, plain]
    {
        plain ('u');
        ctx.expect (host.tunerOpen(), "U did not open the tuner");
    } });
    steps->push_back ({ 200, [&host, &ctx, &session, &transport, plain]
    {
        host.pressPeerKey ("delete", 0);
        plain ('m');
        ctx.expect (session.track (0).regions.size() == 1, "Delete behind the tuner removed the region it hides");
        ctx.expect (session.getMarkers().empty() && host.modalStackEmpty(), "M behind the tuner dropped a marker");
        ctx.expect (host.tunerOpen(), "the keys behind the tuner closed it");
        const bool loopWas = transport.isLoopEnabled();
        plain ('l');
        ctx.expect (transport.isLoopEnabled() != loopWas, "L behind the tuner did not reach the loop");
        transport.setLoopEnabled (loopWas);
        ctx.expect (host.pressPeerKey ("escape"), "Escape over the tuner was not handled");
    } });
    steps->push_back ({ 200, [&host, &ctx, plain]
    {
        ctx.expect (! host.tunerOpen(), "Escape did not close the tuner");
        plain ('u');
        ctx.expect (host.tunerOpen(), "U did not open the tuner again");
        plain ('u');
        ctx.expect (! host.tunerOpen(), "U did not close the tuner");
        host.pressPeerKey ("delete", 0);
    } });
    steps->push_back ({ 200, [&ctx, &session]
    { ctx.expect (session.track (0).regions.empty(), "Delete with the tuner closed did not remove the selected region"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar tunerKeys { Scenario {
    "gui.tuner_keys", { "gui", "keyboard", "transport" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTunerKeys (host, ctx); }
} };

const ScenarioRegistrar transportKeys { Scenario {
    "gui.transport_keys", { "gui", "keyboard", "transport" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTransportKeys (host, ctx); }
} };

std::optional<ScenarioResult> runDspReadout (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no modal");
    if (host.dspReadout().empty()) return ScenarioResult::skip ("the window has no status bar");
    // A live callback keeps scoring its own overruns, which would move the
    // counter out from under the assertions. The gate stops the callback
    // before it starts timing itself, so the counts hold still.
    engine.suspendProcessing();
    ctx.cleanup ([&engine] { engine.resetXRunCounts(); engine.resumeProcessing(); });
    engine.resetXRunCounts();
    for (int overrun = 0; overrun < 3; ++overrun) engine.noteEngineXRun();

    // The readout is timer-driven, so the count appears on a later tick.
    ctx.waitUntil ([&host] { return host.dspReadout().find (" (3/") != std::string::npos; }, 3000,
        [&host, &ctx, &engine]
        {
            const auto text = host.dspReadout();
            ctx.expect (std::regex_match (text, std::regex (R"(DSP: \d+% \(3/\d+\))")),
                        "the DSP readout does not carry a load percentage beside the dropout pair: " + text);
            ctx.expect (engine.getXRunCount() == 3, "the readout reported a count the engine does not hold");
            if (! ctx.expect (host.doubleClickDspReadout(), "the DSP readout did not take a double-click"))
            { ctx.complete (ctx.verdict()); return; }
            ctx.expect (engine.getXRunCount() == 0, "the double-click did not zero the engine dropout count");
            ctx.expect (engine.getBackendXRunCount() == 0, "the double-click did not zero the backend dropout count");
            // mouseDoubleClick refreshes the readout itself rather than
            // leaving a stale warning up until the next tick.
            ctx.expect (host.dspReadout().find (" (0/0)") != std::string::npos,
                        "the readout still showed the old counters after the reset: " + host.dspReadout());
            ctx.complete (ctx.verdict());
        }, "the DSP readout never showed the engine's dropout count");
    return std::nullopt;
}

const ScenarioRegistrar dspReadout { Scenario {
    "gui.dsp_readout_counters", { "gui", "transport" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runDspReadout (host, ctx); }
} };

// A realtime thread that computes past the kernel's CPU-time warning is moved
// to normal priority, and the DSP readout counts it until the double-click. The
// warning here is a real SIGXCPU through the real handler, raised on the
// message thread, which is not realtime: the handler counts it as a warning
// it found no realtime thread for, which the readout counts too.
std::optional<ScenarioResult> runDspReadoutRealtime (GuiHost& host, ScenarioContext& ctx)
{
   #if defined (__linux__)
    auto& engine = ctx.engine();
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no modal");
    if (host.dspReadout().empty()) return ScenarioResult::skip ("the window has no status bar");

    // The guard catches SIGXCPU only under a finite limit. A soft one can be
    // put back up to the hard one afterwards, which leaves the hard one alone.
    rlimit was {};
    if (getrlimit (RLIMIT_RTTIME, &was) != 0) return ScenarioResult::skip ("no RLIMIT_RTTIME");
    if (was.rlim_cur == RLIM_INFINITY)
    {
        rlimit finite = was;
        finite.rlim_cur = 60'000'000;
        if (finite.rlim_max != RLIM_INFINITY) finite.rlim_cur = std::min (finite.rlim_cur, finite.rlim_max);
        if (setrlimit (RLIMIT_RTTIME, &finite) != 0) return ScenarioResult::skip ("RLIMIT_RTTIME would not take a soft limit");
    }
    ctx.cleanup ([was] { setrlimit (RLIMIT_RTTIME, &was); });
    if (! rt::guardRealtimeCpuTime())
        return ScenarioResult::skip ("SIGXCPU has a handler that is not Dusk Studio's");

    engine.suspendProcessing();
    ctx.cleanup ([&engine] { engine.resetXRunCounts(); engine.resumeProcessing(); });
    engine.resetXRunCounts();
    std::raise (SIGXCPU);
    ctx.expect (engine.getRealtimeDemotionCount() == 1, "the warning was not counted");

    ctx.waitUntil ([&host] { return host.dspReadout().find (" RT 1") != std::string::npos; }, 3000,
        [&host, &ctx, &engine]
        {
            const auto text = host.dspReadout();
            ctx.expect (std::regex_match (text, std::regex (R"(DSP: \d+% \(0/\d+\)( @\dx)? RT 1)")),
                        "the DSP readout does not end in the realtime count: " + text);
            if (! ctx.expect (host.doubleClickDspReadout(), "the DSP readout did not take a double-click"))
            { ctx.complete (ctx.verdict()); return; }
            ctx.expect (engine.getRealtimeDemotionCount() == 0, "the double-click did not zero the realtime count");
            ctx.expect (host.dspReadout().find ("RT") == std::string::npos,
                        "the readout still showed the realtime count after the reset: " + host.dspReadout());
            ctx.complete (ctx.verdict());
        }, "the DSP readout never showed the realtime count");
    return std::nullopt;
   #else
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("the realtime CPU-time guard is Linux-only");
   #endif
}

const ScenarioRegistrar dspReadoutRealtime { Scenario {
    "gui.dsp_readout_realtime_count", { "gui", "transport" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runDspReadoutRealtime (host, ctx); }
} };

// The Quick Guide's first take: with a track armed, R rolls and records, Space
// stops, and the take lands as a region on that track's row of the tape strip.
std::optional<ScenarioResult> runRecordKeyRegion (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    if (! transport.isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    if (! engine.isAudioCallbackRegistered() || session.deviceCaptureChannels.load() <= 0)
        return ScenarioResult::skip ("requires a running audio device with an input to record from");
    const auto rate = engine.getCurrentSampleRate();
    if (rate <= 0.0) return ScenarioResult::skip ("requires a positive engine sample rate");
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
    const bool timeline = host.setTimelineShown (true);
    ctx.cleanup ([&host, timeline] { host.setTimelineShown (timeline); });
    auto regionsAtStart = std::make_shared<std::vector<std::decay_t<decltype (track.regions)>>>();
    auto takesAtStart = std::make_shared<std::vector<std::decay_t<decltype (track.takes)>>>();
    for (int index = 0; index < Session::kNumTracks; ++index)
    {
        regionsAtStart->push_back (session.track (index).regions);
        takesAtStart->push_back (session.track (index).takes);
    }
    ctx.cleanup ([&engine, &session, &transport, regionsAtStart, takesAtStart, originalDir = currentSessionDirectory (session),
                  loop = transport.isLoopEnabled(), punch = transport.isPunchEnabled(), at = transport.getPlayhead()]
    {
        engine.stop();
        for (int index = 0; index < Session::kNumTracks; ++index)
        {
            session.track (index).regions = (*regionsAtStart)[(std::size_t) index];
            session.track (index).takes = (*takesAtStart)[(std::size_t) index];
        }
        engine.getPlaybackEngine().preparePlayback();
        transport.setLoopEnabled (loop);
        transport.setPunchEnabled (punch);
        transport.setPlayhead (at);
        engine.getUndoManager().clearUndoHistory();
        applySessionDirectory (session, originalDir);
    });

    // The take is written under the session folder, so that is scratch space.
    const auto takeDir = ctx.tempDir() / "Take session";
    applySessionDirectory (session, takeDir);
    engine.stop();
    track.regions.clear();
    engine.getPlaybackEngine().preparePlayback();
    transport.setPlayhead (0);
    transport.setLoopEnabled (false);
    transport.setPunchEnabled (false);
    session.countInEnabled.store (false);
    track.mode.store ((int) Track::Mode::Mono);
    track.inputSource.store (0);
    session.setTrackArmed (0, true);
    if (! track.recordArmed.load())
        return ScenarioResult::fail ("track 1 would not arm on input 1");

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.pressPeerKey (keyCodeDescription ('r'), 'r'), "the window did not handle R"); } });
    steps->push_back ({ 300, [&host, &ctx, &transport]
    {
        ctx.expect (transport.isRecording(),
                    "R did not start recording the armed track"
                        + (host.modalStackEmpty() ? std::string() : "; up: '" + host.modalText() + "'"));
    } });
    steps->push_back ({ 1500, [&host, &ctx, &transport, rate]
    {
        ctx.expect (transport.isRecording() && transport.getPlayhead() > (std::int64_t) (rate * 0.5),
                    "the transport did not roll while recording; playhead at "
                        + std::to_string (transport.getPlayhead()));
        ctx.expect (host.pressPeerKey ("spacebar", ' '), "the window did not handle Space");
    } });
    runSteps (ctx, steps, [&host, &ctx, &session, &transport, &track, regionsAtStart, takeDir, rate]
    {
        ctx.waitUntil ([&transport, &track] { return transport.isStopped() && ! track.regions.empty(); }, 5000,
            [&host, &ctx, &session, &track, regionsAtStart, takeDir, rate]
            {
                ctx.expect (track.regions.size() == 1,
                            "the take left " + std::to_string (track.regions.size()) + " regions on track 1");
                const auto& region = track.regions.front();
                ctx.expect (region.lengthInSamples >= (std::int64_t) (rate * 0.5),
                            "the take's region is only " + std::to_string (region.lengthInSamples) + " samples long");
                const auto file = region.file.getFullPathName().toStdString();
                ctx.expect (region.file.existsAsFile() && file.rfind (takeDir.string(), 0) == 0,
                            "the take's file '" + file + "' is not in the session folder");
                for (int index = 1; index < Session::kNumTracks; ++index)
                    ctx.expect (session.track (index).regions.size() == (*regionsAtStart)[(std::size_t) index].size(),
                                "unarmed track " + std::to_string (index + 1) + " gained a region");
                ctx.expect (host.tapeRegionShown (0, 0), "the tape strip does not lay the take out on track 1's row");
                ctx.complete (ctx.verdict());
            }, "Space did not stop the transport with a region on track 1");
    });
    return std::nullopt;
}

const ScenarioRegistrar recordKeyRegion { Scenario {
    "gui.record_key_draws_region", { "gui", "recording", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runRecordKeyRegion (host, ctx); }
} };

// Typing a new BPM with a tempo-locked MIDI region in the session asks first,
// in the manual's words. Cancel leaves everything where it was; Apply retimes
// the MIDI region and leaves the audio region and the marker where they were.
std::optional<ScenarioResult> runTempoChangeConfirmation (GuiHost& host, ScenarioContext& ctx)
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
    keepStage (host, ctx);
    ctx.cleanup ([&host, &engine, &session, originalDir, restore, points = session.tempoMap.points(),
                  bpm = session.tempoBpm.load()]
    {
        drainModals (host);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
        session.tempoBpm.store (bpm);
        engine.setTempoPoints (points);
        engine.getUndoManager().clearUndoHistory();
    });
    const auto folder = ctx.tempDir() / "Tempo session";
    std::filesystem::create_directories (folder);
    if (! SessionSerializer::save (*std::make_unique<Session>(), folder / "session.json"))
        return ScenarioResult::fail ("could not write the empty session");
    reopenSavedSession (host, folder / "session.json");
    if (currentSessionDirectory (session) != folder) return ScenarioResult::fail ("could not open the empty session");
    host.switchToStage (GuiHost::Stage::Recording);
    engine.setTempoPoints ({});
    session.tempoBpm.store (120.0f);

    const auto second = (std::int64_t) std::llround (rate);
    MidiRegion midi;
    midi.timelineStart = second * 2;
    midi.lengthInTicks = 4 * kMidiTicksPerQuarter;
    midi.lengthInSamples = ticksToSamples (midi.lengthInTicks, rate, 120.0f);
    midi.tempoLock = true;
    midi.recordedAtBPM = 120.0;
    midi.notes = { { 1, 60, 100, 0, kMidiTicksPerQuarter } };
    session.track (0).mode.store ((int) Track::Mode::Midi);
    session.track (0).midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { midi }));
    const auto source = folder / "audio" / "Ramp.wav";
    if (! writeRampFixture (source, rate, second)) return ScenarioResult::fail ("could not write the audio fixture");
    AudioRegion audio;
    audio.timelineStart = second * 3;
    audio.lengthInSamples = second;
    audio.file = decltype (audio.file) (source.u8string().c_str());
    session.track (1).regions = { audio };
    const auto markerAt = second * 5;
    session.getMarkers().clear();
    session.getMarkers().push_back ({ "Verse", markerAt, {} });

    const auto placed = [&ctx, &session, audio, markerAt] (const std::string& how, std::int64_t midiStart,
                                                           std::int64_t midiLength)
    {
        const auto& regions = session.track (0).midiRegions.current();
        ctx.expect (regions.size() == 1 && regions.front().timelineStart == midiStart
                        && regions.front().lengthInSamples == midiLength,
                    how + ": the MIDI region sits at " + (regions.empty() ? std::string ("nothing")
                        : std::to_string (regions.front().timelineStart) + " for "
                              + std::to_string (regions.front().lengthInSamples))
                        + ", not " + std::to_string (midiStart) + " for " + std::to_string (midiLength));
        const auto& tape = session.track (1).regions;
        ctx.expect (tape.size() == 1 && tape.front().timelineStart == audio.timelineStart
                        && tape.front().lengthInSamples == audio.lengthInSamples,
                    how + ": the audio region moved");
        const auto& markers = session.getMarkers();
        ctx.expect (markers.size() == 1 && markers.front().timelineSamples == markerAt, how + ": the marker moved");
    };
    const std::string body = "Change tempo from 120 to 100 BPM?\n\n"
                             "    1 tempo-locked MIDI region will retime to keep musical position.\n"
                             "\nAudio regions and markers are NOT retimed.";
    auto steps = std::make_shared<std::vector<Step>>();
    const auto enter = [&host, &ctx, &session, steps, body, placed, midi] (const std::string& how)
    {
        steps->push_back ({ 300, [&host, &ctx, how]
        { ctx.expect (host.doubleClickTempo(), how + ": the BPM readout was unavailable for double-click"); } });
        steps->push_back ({ 300, [&host, &ctx, how]
        {
            if (! ctx.expect (host.focusModalTextInput(), how + ": double-clicking BPM did not open a text input"))
                return;
            typeReplacing (host, "100");
            ctx.expect (host.pressPeerKey ("return"), how + ": the tempo input did not accept Return");
        } });
        steps->push_back ({ 400, [&host, &ctx, &session, how, body, placed, midi]
        {
            const auto text = host.confirmationText();
            ctx.expect (text == std::vector<std::string> { "Confirm tempo change", body },
                        how + ": the confirmation read '" + joinedLines (text) + "'");
            ctx.expect (std::abs (session.tempoBpm.load() - 120.0f) < 0.001f, how + ": the tempo changed before the answer");
            placed (how + " before the answer", midi.timelineStart, midi.lengthInSamples);
            ctx.expect (host.clickModalButton (how), how + ": the confirmation has no " + how + " button");
        } });
    };
    enter ("Cancel");
    steps->push_back ({ 300, [&host, &ctx, &session, placed, midi]
    {
        ctx.expect (host.modalStackEmpty(), "Cancel left '" + host.modalText() + "' up");
        ctx.expect (std::abs (session.tempoBpm.load() - 120.0f) < 0.001f, "Cancel changed the tempo");
        placed ("Cancel", midi.timelineStart, midi.lengthInSamples);
    } });
    enter ("Apply");
    steps->push_back ({ 300, [&host, &ctx, &session, placed, midi, rate]
    {
        ctx.expect (host.modalStackEmpty(), "Apply left '" + host.modalText() + "' up");
        ctx.expect (std::abs (session.tempoBpm.load() - 100.0f) < 0.001f,
                    "Apply left the tempo at " + std::to_string (session.tempoBpm.load()));
        placed ("Apply", std::llround ((double) midi.timelineStart * 1.2),
                ticksToSamples (midi.lengthInTicks, rate, 100.0f));
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar tempoChangeConfirmation { Scenario {
    "gui.tempo_change_confirmation", { "gui", "transport", "messages" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTempoChangeConfirmation (host, ctx); }
} };

// With no audio device running a Mackie control surface's timecode counts bars
// at the rate the timeline measures time at, the rate the device last ran at.
std::optional<ScenarioResult> runMcuTimecodeWithoutDevice (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    const double rate = engine.getCurrentSampleRate();
    if (host.engineDetached() || ! engine.isAudioCallbackRegistered() || rate <= 0.0)
        return ScenarioResult::skip ("requires a running audio device");
    if (! transport.isStopped()) return ScenarioResult::skip ("requires stopped transport");
    auto* const mcu = engine.getMcuController();
    if (mcu == nullptr) return ScenarioResult::skip ("requires the Mackie controller");
    ctx.keep (session.tempoBpm);
    ctx.keep (session.beatsPerBar);
    const double standIn = std::abs (rate - 44100.0) < 0.5 ? 88200.0 : 44100.0;
    ctx.cleanup ([&engine, &transport, mcu, rate, playhead = transport.getPlayhead()]
    {
        engine.setLastDeviceSampleRateForScenario (rate);
        if (! engine.isAudioCallbackRegistered()) engine.reattachAudioCallback();
        transport.setPlayhead (playhead);
        mcu->forceResync();
    });
    session.tempoBpm.store (120.0f);
    session.beatsPerBar.store (4);

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&ctx, &engine, &transport, mcu, standIn]
    {
        engine.detachAudioCallback();
        engine.setLastDeviceSampleRateForScenario (standIn);
        // One bar of 4/4 at 120 BPM is two seconds.
        transport.setPlayhead ((std::int64_t) std::llround (standIn * 2.0));
        std::string shown;
        for (const auto meta : mcu->buildBufferForTest())
        {
            const auto& message = meta.getMessage();
            const auto* raw = message.getRawData();
            const int size = message.getRawDataSize();
            const int digits = mcu::sysex::kTimecodeDigits;
            if (raw != nullptr && size >= (int) mcu::sysex::kPrefixLen + 1 + digits
                && raw[mcu::sysex::kPrefixLen] == mcu::sysex::kCmdTimecode)
                shown.assign (reinterpret_cast<const char*> (raw) + mcu::sysex::kPrefixLen + 1, (std::size_t) digits);
        }
        ctx.expect (shown.rfind ("0020101", 0) == 0,
                    "two seconds in, with no device running, the timecode reads '" + shown + "', not bar 2 beat 1");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar mcuTimecodeWithoutDevice { Scenario {
    "gui.mcu_timecode_without_device", { "gui", "mcu" }, Needs::Engine | Needs::Gui,
    {}, {}, 5000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMcuTimecodeWithoutDevice (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
