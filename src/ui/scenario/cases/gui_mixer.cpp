#include "../GuiScenarioSupport.h"
#include "../../AppConfig.h"
#include "../../../foundation/AppConfigDir.h"
#include "../../../foundation/Fs.h"
#include "../../../engine/audiofile/FileWriter.h"
#include "../../../session/SessionSerializer.h"

#include <algorithm>
#include <array>
#include <atomic>
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
#include <type_traits>
#include <utility>
#include <vector>

#if DUSKSTUDIO_HAS_ALSA
 #include <alsa/asoundlib.h>
#endif

namespace duskstudio::scenario::gui
{
namespace
{
// -------------------------------------------------------------- automation

bool laneHolds (const std::vector<AutomationPoint>& points, AutomationParam param, float value, float tolerance)
{
    return std::any_of (points.begin(), points.end(), [&] (const AutomationPoint& point)
    {
        return std::abs (denormalizeAutomationValue (param, point.value) - value) < tolerance;
    });
}

bool timesAscend (const std::vector<AutomationPoint>& points)
{
    for (std::size_t i = 1; i < points.size(); ++i)
        if (points[i].timeSamples <= points[i - 1].timeSamples) return false;
    return true;
}

// The strips' own timers record the rides while the transport rolls. Write
// takes the fader, pan, mute and solo; Touch records the fader and pan while
// held and returns to the earlier ride on release; a mute click in Touch
// records nothing; a bus's fader, pan and mute, the master fader and an aux
// return's level and mute record like a track's; a Write pass leaves the ride
// after where it stopped alone.
std::optional<ScenarioResult> runAutomation (GuiHost& host, ScenarioContext& ctx)
{
    static constexpr float kDbTolerance = 0.05f;
    // The aux lanes record from their own strips, which the Aux stage builds
    // the first time it is shown and which keep running once it is hidden.
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Aux);
    host.switchToStage (GuiHost::Stage::Mixing);
    auto* strip = host.strip (kStripIndex);
    if (strip == nullptr)
        return ScenarioResult::skip ("the console has no strip to drive");

    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& track = session.track (kStripIndex);
    auto& params = track.strip;
    auto& bus = session.bus (0).strip;
    auto& master = session.master();
    auto& aux = session.auxLane (0).params;

    const auto lane = [&track] (AutomationParam p) -> auto&
    { return track.automationLanes[(std::size_t) p]; };
    const auto clearAll = [&track, &bus, &master, &aux]
    {
        track.automationMode.store ((int) AutomationMode::Off, std::memory_order_release);
        bus.automationMode.store ((int) AutomationMode::Off, std::memory_order_release);
        master.automationMode.store ((int) AutomationMode::Off, std::memory_order_release);
        aux.automationMode.store ((int) AutomationMode::Off, std::memory_order_release);
        for (auto& l : track.automationLanes) l.publishPoints ({});
        for (auto& l : bus.automationLanes) l.publishPoints ({});
        for (auto& l : master.automationLanes) l.publishPoints ({});
        for (auto& l : aux.automationLanes) l.publishPoints ({});
    };
    ctx.cleanup ([&engine, &session, &params, &bus, &master, &aux, clearAll]
    {
        engine.stop();
        clearAll();
        params.faderDb.store (0.0f, std::memory_order_relaxed);
        params.pan.store (0.0f, std::memory_order_relaxed);
        params.faderTouched.store (false, std::memory_order_relaxed);
        params.panTouched.store (false, std::memory_order_relaxed);
        params.mute.store (false, std::memory_order_relaxed);
        session.setTrackSoloed (kStripIndex, false);
        bus.faderDb.store (0.0f, std::memory_order_relaxed);
        bus.pan.store (0.0f, std::memory_order_relaxed);
        bus.mute.store (false, std::memory_order_relaxed);
        master.faderDb.store (0.0f, std::memory_order_relaxed);
        aux.returnLevelDb.store (0.0f, std::memory_order_relaxed);
        aux.mute.store (false, std::memory_order_relaxed);
    });

    struct Marks
    {
        std::size_t muteCount = 0;
        std::int64_t stoppedAt = 0;
    };
    auto marks = std::make_shared<Marks>();
    auto steps = std::make_shared<std::vector<Step>>();
    const auto roll = [&engine]
    {
        engine.getTransport().setPlayhead (0);
        engine.play();
    };

    // First pass, WRITE: two positions for each continuous control, and a
    // mute on and off, a solo on.
    steps->push_back ({ 0, [&params, &track, &bus, &master, &aux, clearAll, roll]
    {
        clearAll();
        for (auto* mode : { &track.automationMode, &bus.automationMode,
                            &master.automationMode, &aux.automationMode })
            mode->store ((int) AutomationMode::Write, std::memory_order_release);
        params.faderDb.store (-20.0f, std::memory_order_relaxed);
        params.pan.store (-0.5f, std::memory_order_relaxed);
        bus.faderDb.store (-8.0f, std::memory_order_relaxed);
        bus.pan.store (-0.5f, std::memory_order_relaxed);
        master.faderDb.store (-2.0f, std::memory_order_relaxed);
        aux.returnLevelDb.store (-12.0f, std::memory_order_relaxed);
        roll();
    } });
    steps->push_back ({ 400, [&params, &bus, &master, &aux, strip]
    {
        params.faderDb.store (-10.0f, std::memory_order_relaxed);
        params.pan.store (0.5f, std::memory_order_relaxed);
        bus.faderDb.store (-4.0f, std::memory_order_relaxed);
        bus.pan.store (0.5f, std::memory_order_relaxed);
        bus.mute.store (true, std::memory_order_relaxed);
        master.faderDb.store (-4.0f, std::memory_order_relaxed);
        aux.returnLevelDb.store (-6.0f, std::memory_order_relaxed);
        aux.mute.store (true, std::memory_order_relaxed);
        strip->clickMute();
    } });
    steps->push_back ({ 400, [&bus, &aux, strip]
    {
        strip->clickMute();
        strip->clickSolo();
        bus.mute.store (false, std::memory_order_relaxed);
        aux.mute.store (false, std::memory_order_relaxed);
    } });
    // The strips splice a pass on their first timer tick after the stop.
    steps->push_back ({ 400, [&engine] { engine.stop(); } });
    steps->push_back ({ 100, [&ctx, &params, &track, &bus, &master, &aux, lane, marks, roll]
    {
        const auto& fader = lane (AutomationParam::FaderDb).pointsConst();
        ctx.expect (laneHolds (fader, AutomationParam::FaderDb, -20.0f, kDbTolerance)
                        && laneHolds (fader, AutomationParam::FaderDb, -10.0f, kDbTolerance)
                        && timesAscend (fader),
                    "WRITE did not record both fader positions in order");
        const auto& pan = lane (AutomationParam::Pan).pointsConst();
        ctx.expect (laneHolds (pan, AutomationParam::Pan, -0.5f, 0.01f)
                        && laneHolds (pan, AutomationParam::Pan, 0.5f, 0.01f),
                    "WRITE did not record both pan positions");
        const auto onThenOff = [] (const std::vector<AutomationPoint>& points)
        {
            const auto on = std::find_if (points.begin(), points.end(),
                                          [] (const AutomationPoint& p) { return p.value > 0.5f; });
            return on != points.end()
                && std::any_of (on, points.end(), [] (const AutomationPoint& p) { return p.value < 0.5f; });
        };
        const auto& mute = lane (AutomationParam::Mute).pointsConst();
        ctx.expect (onThenOff (mute), "WRITE did not record the mute going on and off");
        ctx.expect (laneHolds (lane (AutomationParam::Solo).pointsConst(), AutomationParam::Solo, 1.0f, 0.1f),
                    "WRITE did not record the solo");
        const auto& busFader = bus.automationLanes[(std::size_t) AutomationParam::FaderDb].pointsConst();
        ctx.expect (laneHolds (busFader, AutomationParam::FaderDb, -8.0f, kDbTolerance)
                        && laneHolds (busFader, AutomationParam::FaderDb, -4.0f, kDbTolerance),
                    "a bus fader in WRITE did not record");
        const auto& busPan = bus.automationLanes[(std::size_t) AutomationParam::Pan].pointsConst();
        ctx.expect (laneHolds (busPan, AutomationParam::Pan, -0.5f, 0.01f)
                        && laneHolds (busPan, AutomationParam::Pan, 0.5f, 0.01f),
                    "a bus pan in WRITE did not record");
        ctx.expect (onThenOff (bus.automationLanes[(std::size_t) AutomationParam::Mute].pointsConst()),
                    "a bus mute in WRITE did not record going on and off");
        const auto& masterFader = master.automationLanes[(std::size_t) AutomationParam::FaderDb].pointsConst();
        ctx.expect (laneHolds (masterFader, AutomationParam::FaderDb, -2.0f, kDbTolerance)
                        && laneHolds (masterFader, AutomationParam::FaderDb, -4.0f, kDbTolerance),
                    "the master fader in WRITE did not record");
        const auto& auxReturn = aux.automationLanes[(std::size_t) AutomationParam::FaderDb].pointsConst();
        ctx.expect (laneHolds (auxReturn, AutomationParam::FaderDb, -12.0f, kDbTolerance)
                        && laneHolds (auxReturn, AutomationParam::FaderDb, -6.0f, kDbTolerance),
                    "an aux return in WRITE did not record");
        ctx.expect (onThenOff (aux.automationLanes[(std::size_t) AutomationParam::Mute].pointsConst()),
                    "an aux mute in WRITE did not record going on and off");
        marks->muteCount = mute.size();

        // Second pass, TOUCH: hold the fader and pan for the first part only.
        track.automationMode.store ((int) AutomationMode::Touch, std::memory_order_release);
        for (auto* mode : { &bus.automationMode, &master.automationMode, &aux.automationMode })
            mode->store ((int) AutomationMode::Off, std::memory_order_release);
        params.faderTouched.store (true, std::memory_order_relaxed);
        params.faderDb.store (-3.0f, std::memory_order_relaxed);
        params.panTouched.store (true, std::memory_order_relaxed);
        params.pan.store (-0.8f, std::memory_order_relaxed);
        roll();
    } });
    steps->push_back ({ 250, [&params, strip]
    {
        params.faderTouched.store (false, std::memory_order_relaxed);
        params.panTouched.store (false, std::memory_order_relaxed);
        strip->clickMute();
    } });
    // The first pass rode -10 dB from 400 ms on.
    steps->push_back ({ 700, [&ctx, &engine, &params]
    {
        const float live = params.liveFaderDb.load (std::memory_order_relaxed);
        ctx.note ("TOUCH: the fader plays " + std::to_string (live) + " dB after the release");
        ctx.expect (std::abs (live - (-10.0f)) < 0.5f,
                    "after TOUCH let go the fader did not return to the earlier ride (-10 dB)");
        engine.stop();
    } });
    steps->push_back ({ 100, [&ctx, &params, &track, lane, marks, roll, strip]
    {
        ctx.expect (laneHolds (lane (AutomationParam::FaderDb).pointsConst(), AutomationParam::FaderDb,
                               -3.0f, kDbTolerance),
                    "TOUCH did not record the fader while it was held");
        ctx.expect (laneHolds (lane (AutomationParam::Pan).pointsConst(), AutomationParam::Pan, -0.8f, 0.01f),
                    "TOUCH did not record the pan while it was held");
        ctx.expect (lane (AutomationParam::Mute).pointsConst().size() == marks->muteCount,
                    "a mute click in TOUCH was recorded");
        strip->clickMute();

        // Third pass, WRITE over the start only.
        track.automationMode.store ((int) AutomationMode::Write, std::memory_order_release);
        params.faderDb.store (-15.0f, std::memory_order_relaxed);
        roll();
    } });
    steps->push_back ({ 250, [&engine, marks]
    {
        engine.stop();
        marks->stoppedAt = engine.getTransport().getPlayhead();
    } });
    steps->push_back ({ 100, [&ctx, &track, lane, marks]
    {
        const auto& fader = lane (AutomationParam::FaderDb).pointsConst();
        const auto stoppedAt = marks->stoppedAt;
        ctx.note ("third pass stopped at " + std::to_string (stoppedAt) + "; the lane ends at "
                  + (fader.empty() ? std::string ("-") : std::to_string (fader.back().timeSamples)));
        ctx.expect (std::any_of (fader.begin(), fader.end(),
                                 [stoppedAt] (const AutomationPoint& p) { return p.timeSamples > stoppedAt + 4800; }),
                    "a WRITE pass erased the automation after where it stopped");
        ctx.expect (timesAscend (fader), "the fader lane is out of time order");
        track.automationMode.store ((int) AutomationMode::Off, std::memory_order_release);
    } });

    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

// A mode set outside a strip's own menu - drawing automation arms READ, a
// session load sets the aux lanes it does not rebuild - shows on every strip's
// label within a tick, and READ locks every strip's fader.
std::optional<ScenarioResult> runModeShownOnEveryStrip (GuiHost& host, ScenarioContext& ctx)
{
    struct Probe { GuiHost::StripKind kind; int index; const char* name; };
    static constexpr Probe kProbes[] = {
        { GuiHost::StripKind::Channel, kStripIndex, "channel" },
        { GuiHost::StripKind::Bus,     0,           "bus" },
        { GuiHost::StripKind::Master,  0,           "master" },
        { GuiHost::StripKind::Aux,     0,           "aux return" },
    };

    // The aux lanes are built the first time their stage shows, and keep
    // running once it is hidden.
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Aux);
    host.switchToStage (GuiHost::Stage::Mixing);

    auto& session = ctx.session();
    const std::array<std::atomic<int>*, 4> modes {
        &session.track (kStripIndex).automationMode, &session.bus (0).strip.automationMode,
        &session.master().automationMode, &session.auxLane (0).params.automationMode };
    const auto setAll = [modes] (AutomationMode mode)
    {
        for (auto* m : modes) m->store ((int) mode, std::memory_order_release);
    };
    ctx.cleanup ([setAll] { setAll (AutomationMode::Off); });

    const auto check = [&ctx, &host] (bool read)
    {
        for (const auto& probe : kProbes)
        {
            std::string label;
            bool faderEnabled = false;
            const std::string name (probe.name);
            if (! ctx.expect (host.automationView (probe.kind, probe.index, label, faderEnabled),
                              name + ": the strip is not built"))
                continue;
            ctx.expect (! label.empty() && label.front() == (read ? 'R' : 'O'),
                        name + ": the mode label shows '" + label + "'");
            ctx.expect (faderEnabled != read, name + (read ? ": the fader takes input in READ"
                                                           : ": the fader stayed locked after READ"));
        }
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [setAll] { setAll (AutomationMode::Read); } });
    steps->push_back ({ 150, [check, setAll] { check (true); setAll (AutomationMode::Off); } });
    steps->push_back ({ 150, [check] { check (false); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

std::optional<ScenarioResult> runAuxSources (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalStage = engine.getStage();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, originalDir, originalStage, restore]
    {
        if (auto* lane = host.auxLane (0)) lane->captureSources (false);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        host.switchToStage (guiStage (originalStage));
    });
    for (int i = 0; i < Session::kNumTracks; ++i)
    {
        auto& track = session.track (i);
        track.name = i == 0 ? "Lead voice" : std::to_string (i + 1);
        track.automationMode.store ((int) AutomationMode::Off, std::memory_order_release);
        track.strip.auxSendDb[0].store (i % 2 == 0 ? -6.0f : ChannelStripParams::kAuxSendOffDb);
        track.strip.auxSendDb[1].store (-3.0f);
    }
    host.switchToStage (GuiHost::Stage::Aux);
    auto* lane = host.auxLane (0);
    if (lane == nullptr || ! lane->captureSources (true))
        return ScenarioResult::fail ("first aux Sources panel is not showing");
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 500, [lane, &ctx, &session]
    {
        const auto rows = lane->sourceRows();
        ctx.expect (rows.size() == Session::kNumTracks, "Sources did not paint all 24 channel rows");
        for (int i = 0; i < Session::kNumTracks && i < (int) rows.size(); ++i)
        {
            const auto number = std::to_string (i + 1);
            const auto expected = number + "\t" + (i == 0 ? "Lead voice" : "Trk " + number)
                                  + (i % 2 == 0 ? "\t-6.0 dB\tmeter" : "\t-inf\tmeter");
            ctx.expect (rows[(std::size_t) i] == expected, "unexpected source row: " + rows[(std::size_t) i]);
        }
        session.track (0).strip.auxSendDb[0].store (ChannelStripParams::kAuxSendOffDb);
        session.track (23).strip.auxSendDb[0].store (-3.5f);
        session.track (23).name = "Last channel";
    } });
    steps->push_back ({ 500, [lane, &ctx]
    {
        const auto rows = lane->sourceRows();
        ctx.expect (! rows.empty() && rows.front() == "1\tLead voice\t-inf\tmeter",
                    "Sources did not refresh a disabled send");
        ctx.expect (rows.size() == Session::kNumTracks && rows.back() == "24\tLast channel\t-3.5 dB\tmeter",
                    "Sources did not refresh the last channel name and send level");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar auxSources { Scenario {
    "gui.aux_sources", { "gui", "aux" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAuxSources (host, ctx); }
} };

void pushSettingsTab (std::vector<Step>& steps, GuiHost& host, const std::string& tab)
{
    const auto control = "tab-" + tab;
    steps.push_back ({ 0, [] {}, [&host, control] { return host.pointerAudioSettings (control, 0.5f, false); },
                       "Settings tab was not drawn: " + tab, {}, kSettleBoundMs });
    steps.push_back ({ 0, [] {}, [&host, control] { return host.clickAudioSettingsControl (control); },
                       "Settings tab could not be selected: " + tab, {}, kSettleBoundMs });
}

std::optional<ScenarioResult> runSettingsDefaults (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires native settings");
   #else
    auto& session = ctx.session();
    const bool tape = appconfig::getTapeStripExpandedDefault();
    const bool follow = appconfig::getFollowPlayheadDefault();
    ctx.expect (host.tapeExpansionState() == (tape ? 3 : 0), "launch ignored the tape-strip default");
    ctx.expect (host.timelineChaseState() == (follow ? 3 : 0), "launch ignored the timeline Chase default");
    const auto config = dusk::fs::appConfigDir() / "app-config.properties";
    const bool hadConfig = std::filesystem::exists (config);
    const auto configText = dusk::fs::loadFileAsString (config);
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, config, hadConfig, configText, originalDir, restore]
    {
        host.closeRegionEditors();
        host.closeAudioSettings();
        if (hadConfig) dusk::fs::writeStringToFile (config, configText);
        else { std::error_code error; std::filesystem::remove (config, error); }
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
    });
    MidiRegion midi;
    midi.lengthInSamples = 48000;
    midi.lengthInTicks = 960;
    session.track (0).midiRegions.publish (
        std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { midi }));
    const auto wave = ctx.tempDir() / "silence.wav";
    dusk::audio::WriteSpec spec;
    spec.sampleRate = 48000;
    spec.numChannels = 1;
    auto writer = dusk::audio::FileWriter::create (wave, spec);
    std::vector<float> silence (48000, 0.0f);
    const float* channels[] = { silence.data() };
    if (! writer || ! writer->write (channels, 1, 48000) || ! writer->flush())
        return ScenarioResult::fail ("could not create the audio editor fixture");
    writer.reset();
    AudioRegion audio;
    using File = std::decay_t<decltype (audio.file)>;
    audio.file = File (wave.u8string().c_str());
    audio.lengthInSamples = 48000;
    audio.numChannels = 1;
    session.track (1).regions = { audio };
    auto steps = std::make_shared<std::vector<Step>>();
    for (const bool isMidi : { true, false })
    {
        steps->push_back ({ 200, [&host, &ctx, isMidi]
        { ctx.expect (host.openRegionEditor (isMidi ? 0 : 1, 0, isMidi), "region editor did not open"); } });
        steps->push_back ({ 200, [&host, &ctx, follow]
        {
            ctx.expect (host.regionEditorChase() == (follow ? 1 : 0), "editor ignored the Chase default");
            host.closeRegionEditors();
        } });
    }
    for (const bool inverted : { true, false })
    {
        steps->push_back ({ 200, [&host, &ctx]
        { ctx.expect (host.openAudioSettings(), "settings did not open"); } });
        pushSettingsTab (*steps, host, "general");
        steps->push_back ({ 200, [&host, &ctx]
        { ctx.expect (host.clickAudioSettingsControl ("tape-default"), "tape default checkbox was not visible"); } });
        steps->push_back ({ 200, [&host, &ctx]
        { ctx.expect (host.clickAudioSettingsControl ("follow-default"), "follow default checkbox was not visible"); } });
        steps->push_back ({ 200, [&host, &ctx, tape, follow, inverted]
        {
            ctx.expect (appconfig::getTapeStripExpandedDefault() == (inverted ? ! tape : tape),
                        "tape default did not persist through the checkbox");
            ctx.expect (appconfig::getFollowPlayheadDefault() == (inverted ? ! follow : follow),
                        "follow default did not persist through the checkbox");
            ctx.expect (host.tapeExpansionState() == (tape ? 3 : 0), "default changed the existing timeline layout");
            ctx.expect (host.timelineChaseState() == (follow ? 3 : 0), "default changed existing timeline Chase");
            host.closeAudioSettings();
        } });
    }
    runSteps (ctx, steps, [&host, &ctx]
    {
        ctx.waitUntil ([&host] { return ! host.audioSettingsOpen(); }, 3000,
                       [&ctx] { ctx.complete (ctx.verdict()); },
                       "audio settings did not finish closing");
    });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar settingsDefaults { Scenario {
    "gui.settings_defaults", { "gui", "settings" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSettingsDefaults (host, ctx); }
} };

std::optional<ScenarioResult> runSettingsUiScale (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires native settings");
   #else
    const auto originalScale = static_cast<float> (host.uiScale());
    const auto originalSaved = appconfig::getUiScaleOverride();
    const auto config = dusk::fs::appConfigDir() / "app-config.properties";
    const bool hadConfig = std::filesystem::exists (config);
    const auto configText = dusk::fs::loadFileAsString (config);
    ctx.cleanup ([&host, originalScale, config, hadConfig, configText]
    {
        host.closeAudioSettings();
        host.restoreUiScale (originalScale);
        if (hadConfig) dusk::fs::writeStringToFile (config, configText);
        else { std::error_code error; std::filesystem::remove (config, error); }
    });
    const auto launchWindow = host.mainWindowSize();
    if (launchWindow.size() < 4) return ScenarioResult::fail ("the main window reported no size");
    const auto windowHeld = [&host, &ctx, launchWindow, originalScale] (const std::string& when)
    {
        const auto now = host.mainWindowSize();
        if (! ctx.expect (now.size() >= 4, "the main window reported no size " + when)) return;
       #if defined (__APPLE__)
        const auto onScreen = [] (int size, double scale) { return (int) std::lround (size * scale); };
        const int width = onScreen (now[0], host.uiScale()), height = onScreen (now[1], host.uiScale());
        const int launchWidth = onScreen (launchWindow[0], originalScale);
        const int launchHeight = onScreen (launchWindow[1], originalScale);
        ctx.expect (std::abs (width - launchWidth) <= 2 && std::abs (height - launchHeight) <= 2,
                    "the window went from " + std::to_string (launchWidth) + "x" + std::to_string (launchHeight)
                        + " to " + std::to_string (width) + "x" + std::to_string (height) + " on screen " + when);
        // AppKit applies the minimum whenever it places the window, which this
        // case does not provoke, so the floor is held to its on-screen width
        // too. The height also carries the title bar, which the scale grows.
        const int minimum = onScreen (now[2], host.uiScale());
        const int launchMinimum = onScreen (launchWindow[2], originalScale);
        ctx.expect (std::abs (minimum - launchMinimum) <= 2,
                    "the window's minimum width went from " + std::to_string (launchMinimum) + " to "
                        + std::to_string (minimum) + " on screen " + when);
       #else
        (void) originalScale;
        ctx.expect (now[2] == launchWindow[2] && now[3] == launchWindow[3],
                    "the window's minimum went from the console's " + std::to_string (launchWindow[2]) + "x"
                        + std::to_string (launchWindow[3]) + " to " + std::to_string (now[2]) + "x"
                        + std::to_string (now[3]) + " " + when);
       #endif
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.openAudioSettings(), "settings did not open"); } });
    pushSettingsTab (*steps, host, "general");
    for (const float position : { 0.4f, 0.5f, 0.6f, 0.7f })
        steps->push_back ({ 200, [&host, &ctx, position]
        { ctx.expect (host.pointerAudioSettings ("ui-scale", position, true),
                      "the UI scale slider was not visible"); } });
    steps->push_back ({ 300, [&host, &ctx, originalSaved, windowHeld]
    {
        ctx.expect (host.uiScale() > 1.3 && host.uiScale() < 1.6,
                    "dragging did not preview the global UI scale");
        windowHeld ("while the scale previewed");
        ctx.expect (nearly (appconfig::getUiScaleOverride(), originalSaved),
                    "the UI scale was persisted before release");
        ctx.expect (host.pointerAudioSettings ("ui-scale", 0.7f, false),
                    "the scale slider could not receive release");
    } });
    steps->push_back ({ 300, [&host, &ctx, windowHeld]
    {
        const auto saved = appconfig::getUiScaleOverride();
        ctx.expect (saved > 1.5f && saved < 1.6f, "release did not persist the slider value");
        ctx.expect (std::abs (host.uiScale() - saved) < 0.001,
                    "the persisted scale differs from the displayed interface");
        windowHeld ("after the slider was released");
        host.closeAudioSettings();
    } });
    steps->push_back ({ 200, [&host, &ctx, windowHeld]
    {
        ctx.expect (! host.audioSettingsOpen(), "settings did not close");
        ctx.expect (std::abs (host.uiScale() - appconfig::getUiScaleOverride()) < 0.001,
                    "closing Settings discarded the scale");
        windowHeld ("after Settings closed");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar settingsUiScale { Scenario {
    "gui.settings_ui_scale", { "gui", "settings" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSettingsUiScale (host, ctx); }
} };

std::optional<ScenarioResult> runSettingsRecordingOffset (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires native settings");
   #else
    auto& engine = ctx.engine();
    const auto original = engine.getRecordingLatencyOffsetSamples();
    const auto config = dusk::fs::appConfigDir() / "app-config.properties";
    const bool hadConfig = std::filesystem::exists (config);
    const auto configText = dusk::fs::loadFileAsString (config);
    ctx.cleanup ([&host, &engine, original, config, hadConfig, configText]
    {
        host.closeAudioSettings();
        engine.setRecordingLatencyOffsetSamples (original);
        if (hadConfig) dusk::fs::writeStringToFile (config, configText);
        else { std::error_code error; std::filesystem::remove (config, error); }
    });
    appconfig::setRecordingLatencyOffsetSamples (0);
    engine.setRecordingLatencyOffsetSamples (0);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.openAudioSettings(), "settings did not open"); } });
    pushSettingsTab (*steps, host, "advanced");
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (! host.pointerAudioSettings ("autosave", 0.5f, false), "hidden General control retained a click target");
        ctx.expect (host.clickAudioSettingsControl ("record-offset-plus"), "recording offset plus button was not visible");
    } });
    steps->push_back ({ 0, [&host, &ctx, &engine]
    {
        ctx.expect (engine.getRecordingLatencyOffsetSamples() == 1
                    && appconfig::getRecordingLatencyOffsetSamples() == 1, "plus did not apply exactly one sample");
        ctx.expect (host.clickAudioSettingsControl ("record-offset-minus"), "recording offset minus button was not visible");
    } });
    steps->push_back ({ 0, [&ctx, &engine]
    { ctx.expect (engine.getRecordingLatencyOffsetSamples() == 0, "minus did not apply exactly one sample"); } });
    for (const auto& value : std::vector<std::pair<std::string, int>> {
            { "-137", -137 }, { "2486", 2486 }, { "192001", 192000 }, { "-192001", -192000 } })
    {
        steps->push_back ({ 0, [] {}, [&host] { return host.clickAudioSettingsControl ("record-offset"); },
                           "recording offset input was not visible", {}, kSettleBoundMs });
        steps->push_back ({ 0, [&host, &ctx]
        { ctx.expect (host.inputAudioSettings ("command + a"), "recording offset text could not be selected"); } });
        steps->push_back ({ 0, [&host, &ctx, value]
        {
            host.inputAudioSettings ("focus-out");
            for (const char character : value.first)
                ctx.expect (host.pressKey (std::string (1, character), character), "recording offset did not receive typed text");
        } });
        steps->push_back ({ 0, [&host, &ctx]
        { ctx.expect (host.inputAudioSettings ("enter"), "recording offset did not accept Enter"); } });
        steps->push_back ({ 0, [&ctx, &engine, value]
        {
            ctx.expect (engine.getRecordingLatencyOffsetSamples() == value.second
                        && appconfig::getRecordingLatencyOffsetSamples() == value.second,
                        "typed recording offset was not committed within its sample bounds: " + value.first);
        } });
    }
    steps->push_back ({ 0, [&host] { host.closeAudioSettings(); } });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.openAudioSettings(), "settings did not reopen"); },
    [&host] { return ! host.audioSettingsOpen(); }, "settings did not finish closing before reopening", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.pointerAudioSettings ("record-offset", 0.5f, false), "Settings did not retain the selected Advanced tab"); } });
    steps->push_back ({ 0, [] {}, [&host] { return host.pointerAudioSettings ("record-offset-plus", 0.5f, false); },
                       "reopened offset plus button was not visible", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickAudioSettingsControl ("record-offset-plus"), "reopened offset plus button could not be clicked"); } });
    steps->push_back ({ 0, [&ctx, &engine]
    { ctx.expect (engine.getRecordingLatencyOffsetSamples() == -191999,
        "reopening Settings did not retain the saved offset: " + std::to_string (engine.getRecordingLatencyOffsetSamples())); } });
    steps->push_back ({ 0, [&host] { host.closeAudioSettings(); } });
    runSteps (ctx, steps, [&host, &ctx]
    {
        ctx.waitUntil ([&host] { return ! host.audioSettingsOpen(); }, kSettleBoundMs,
                       [&ctx] { ctx.complete (ctx.verdict()); }, "recording offset Settings did not close");
    });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar settingsRecordingOffset { Scenario {
    "gui.settings_recording_offset", { "gui", "settings", "recording" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000, runSettingsRecordingOffset
} };

std::optional<ScenarioResult> runSettingsAutosave (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires native settings");
   #else
    namespace fs = std::filesystem;
    auto& session = ctx.session();
    const auto config = dusk::fs::appConfigDir() / "app-config.properties";
    const bool hadConfig = fs::exists (config);
    const auto configText = dusk::fs::loadFileAsString (config);
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, config, hadConfig, configText, originalDir, restore]
    {
        if (hadConfig) dusk::fs::writeStringToFile (config, configText);
        else { std::error_code error; fs::remove (config, error); }
        host.closeAudioSettings();
        drainModals (host);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
    });
    const auto dir = ctx.tempDir() / "cadence";
    fs::create_directories (dir);
    const auto saved = dir / "session.json";
    const auto autosave = dir / "session.json.autosave";
    session.track (0).strip.faderDb.store (0.0f);
    if (! SessionSerializer::save (session, saved) || ! host.openSession (saved))
        return ScenarioResult::fail ("could not open the autosave fixture");
    appconfig::setAutosaveIntervalSeconds (300);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.openAudioSettings(), "settings did not open"); } });
    steps->push_back ({ 200, [&host] { host.closeAudioSettings(); } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.openAudioSettings(), "settings did not reopen"); } });
    pushSettingsTab (*steps, host, "general");
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickAudioSettingsControl ("autosave"), "autosave dropdown is not visible"); } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.inputAudioSettings ("focus-out"), "autosave dropdown did not accept the focus change"); } });
    steps->push_back ({ 200, [&host] { host.inputAudioSettings ("home"); } });
    steps->push_back ({ 200, [&host] { host.inputAudioSettings ("enter"); } });
    steps->push_back ({ 200, [&ctx, &session]
    {
        ctx.expect (appconfig::getAutosaveIntervalSeconds() == 15,
                    "the first autosave option did not select 15 seconds");
        session.track (0).strip.faderDb.store (-12.0f);
    } });
    steps->push_back ({ 16000, [&host, &ctx, autosave]
    {
        ctx.expect (! fs::exists (autosave), "the new cadence applied before Settings closed");
        host.closeAudioSettings();
    } });
    steps->push_back ({ 16000, [&host, &ctx, autosave, saved]
    {
        ctx.expect (fs::exists (autosave), "the real autosave timer did not adopt 15 seconds");
        ctx.expect (nearly (savedFaderOf (autosave), -12.0f), "autosave omitted the edit");
        ctx.expect (nearly (savedFaderOf (saved), 0.0f), "autosave changed the canonical session");
        ctx.expect (host.openAudioSettings(), "settings did not reopen for the maximum cadence");
    } });
    pushSettingsTab (*steps, host, "general");
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickAudioSettingsControl ("autosave"), "autosave dropdown is not visible"); } });
    steps->push_back ({ 200, [&host] { host.inputAudioSettings ("end"); } });
    steps->push_back ({ 200, [&host] { host.inputAudioSettings ("enter"); } });
    steps->push_back ({ 200, [&host, &ctx]
    {
        ctx.expect (appconfig::getAutosaveIntervalSeconds() == 300,
                    "the last autosave option did not select 5 minutes");
        host.closeAudioSettings();
    } });
    runSteps (ctx, steps, [&host, &ctx]
    {
        ctx.waitUntil ([&host] { return ! host.audioSettingsOpen(); }, 3000,
                       [&ctx] { ctx.complete (ctx.verdict()); },
                       "audio settings did not finish closing");
    });
    return std::nullopt;
   #endif
}

// Two 16 s waits on the real autosave timer take most of the budget; the rest is
// a dozen panel steps that each wait for the panel to draw, which a slow renderer
// stretches.
const ScenarioRegistrar settingsAutosave { Scenario {
    "gui.settings_autosave_cadence", { "gui", "settings", "autosave" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSettingsAutosave (host, ctx); }
} };

std::optional<ScenarioResult> runSettingsRescan (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI || ! DUSKSTUDIO_HAS_ALSA
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires native settings and ALSA sequencer");
   #else
    snd_seq_t* raw = nullptr;
    if (snd_seq_open (&raw, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0)
        return ScenarioResult::skip ("ALSA sequencer is unavailable");
    auto seq = std::shared_ptr<snd_seq_t> (raw, [] (snd_seq_t* value) { snd_seq_close (value); });
    const auto name = "dusk-rescan-" + std::to_string (snd_seq_client_id (seq.get()));
    snd_seq_set_client_name (seq.get(), name.c_str());
    auto port = std::make_shared<int> (-1);
    auto& engine = ctx.engine();
    const auto originalPosition = engine.getTransport().getPlayhead();
    ctx.cleanup ([&host, &engine, seq, port, originalPosition]
    {
        host.closeAudioSettings();
        engine.stop();
        engine.getTransport().setPlayhead (originalPosition);
        if (*port >= 0) snd_seq_delete_simple_port (seq.get(), *port);
        engine.refreshMidiInputs();
    });
    const auto listed = [&engine, name]
    {
        const auto& outputs = engine.getMidiOutputDevices();
        return std::any_of (outputs.begin(), outputs.end(), [&name] (const auto& output)
        { return output.name.find (name) != std::string::npos; });
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx, &engine]
    {
        ctx.expect (host.openAudioSettings(), "native audio settings did not open");
        engine.play();
    } });
    steps->push_back ({ 200, [seq, port, &ctx]
    {
        *port = snd_seq_create_simple_port (seq.get(), "destination",
            SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        ctx.expect (*port >= 0, "could not create the private MIDI destination");
    } });
    steps->push_back ({ 700, [&ctx, &engine, listed]
    {
        ctx.expect (engine.getTransport().isPlaying(), "fixture was not playing");
        ctx.expect (! listed(), "automatic hot-plug refreshed the port before Rescan");
    } });
    // The settings window lays its controls out and acts on a click in its own
    // frames. Rescanning again lists the same ports, so pressing Rescan as soon
    // as it is drawn, and once more if the list has not moved, cannot change
    // the verdict.
    pushSettingsTab (*steps, host, "audio");
    steps->push_back ({ 0, [] {}, [&host] { return host.clickAudioSettingsControl ("rescan"); },
                        "Rescan devices button was not drawn" });
    steps->push_back ({ 0, [&host, &engine]
    {
        host.closeAudioSettings();
        engine.stop();
    },
    listed, "Rescan devices did not enumerate the new MIDI destination",
    [&host] { host.clickAudioSettingsControl ("rescan"); } });
    steps->push_back ({ 0, [] {}, [&host] { return ! host.audioSettingsOpen(); },
                       "audio settings did not finish closing" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar settingsRescan { Scenario {
    "gui.settings_rescan_devices", { "gui", "settings", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSettingsRescan (host, ctx); }
} };

std::optional<ScenarioResult> runSettingsMidiBindings (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("built without native settings UI");
   #else
    ctx.cleanup ([&host]
    {
        if (host.midiBindingsOpen()) host.closeTopModal();
        host.closeAudioSettings();
    });
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.openAudioSettings(), "native audio settings did not open"); } });
    pushSettingsTab (*steps, host, "midi-bindings");
    steps->push_back ({ 400, [&host, &ctx]
    { ctx.expect (host.clickAudioSettingsControl ("midi-bindings"), "MIDI Bindings button was not drawn"); } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.midiBindingsOpen(), "MIDI Bindings button did not open its panel");
        ctx.expect (! host.audioSettingsOpen(), "settings did not step aside for MIDI Bindings");
        ctx.expect (host.clickModalButton ("Done"), "MIDI Bindings Done button was not visible");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (! host.midiBindingsOpen(), "Done did not close MIDI Bindings");
        ctx.expect (host.audioSettingsOpen(), "Done did not return to audio settings");
        host.closeAudioSettings();
    } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (! host.audioSettingsOpen(), "audio settings did not finish closing"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar settingsMidiBindings { Scenario {
    "gui.settings_midi_bindings", { "gui", "settings", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSettingsMidiBindings (host, ctx); }
} };

std::optional<ScenarioResult> runMidiSelectors (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_ALSA
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("virtual MIDI destination fixture requires ALSA sequencer");
   #else
    const int noticesBefore = ctx.engine().midiHotplugNoticesForScenario();
    snd_seq_t* raw = nullptr;
    if (snd_seq_open (&raw, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0)
        return ScenarioResult::skip ("ALSA sequencer is unavailable");
    auto seq = std::shared_ptr<snd_seq_t> (raw, [] (snd_seq_t* value) { snd_seq_close (value); });
    const auto name = "dusk-selector-" + std::to_string (snd_seq_client_id (seq.get()));
    snd_seq_set_client_name (seq.get(), name.c_str());
    const int port = snd_seq_create_simple_port (seq.get(), "destination",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (port < 0) return ScenarioResult::fail ("could not create the private MIDI destination");
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& track = session.track (0);
    const auto originalStage = engine.getStage();
    const auto originalDir = currentSessionDirectory (session);
    const auto restoreFile = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restoreFile))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, &engine, seq, port, originalDir, restoreFile, originalStage]
    {
        drainModals (host);
        snd_seq_delete_simple_port (seq.get(), port);
        engine.refreshMidiInputs();
        reopenSavedSession (host, restoreFile);
        applySessionDirectory (session, originalDir);
        host.switchToStage (guiStage (originalStage));
    });
    ctx.keep (track.mode);
    ctx.keep (track.midiInputIndex);
    ctx.keep (track.midiOutputIndex);
    ctx.keep (track.midiChannel);
    ctx.keep (track.inputMonitor);
    track.mode.store ((int) Track::Mode::Midi);
    track.midiInputIndex.store (-1);
    track.midiOutputIndex.store (-1);
    track.midiInputIdentifier.clear();
    track.midiOutputIdentifier.clear();
    track.inputMonitor.store (true);
    engine.refreshMidiInputs();
    const auto& outputs = engine.getMidiOutputDevices();
    int destination = -1;
    for (int i = 0; i < (int) outputs.size(); ++i)
        if (outputs[(size_t) i].name.find (name) != std::string::npos) destination = i;
    if (destination < 0) return ScenarioResult::fail ("private MIDI destination was not enumerated");
    const auto destinationId = outputs[(size_t) destination].identifier;
    const int keyboard = engine.getVirtualKeyboardInputIndex();
    if (keyboard < 0) return ScenarioResult::fail ("virtual keyboard was not enumerated");
    const auto keyboardId = engine.getMidiInputDevices()[(size_t) keyboard].identifier;
    host.switchToStage (GuiHost::Stage::Recording);
    const int maxRows = std::max ({ 17, (int) engine.getMidiInputDevices().size() + 1, (int) outputs.size() + 1 });
    const auto select = [&host, maxRows] (int row)
    {
        for (int i = 0; i < maxRows; ++i) host.pressPeerKey ("cursor up");
        for (int i = 0; i < row; ++i) host.pressPeerKey ("cursor down");
        host.pressPeerKey ("Return");
    };
    // Each picker opens a message-loop turn after its click and hands its
    // choice to the track through an asynchronous change notification, so
    // every pick waits for the picker to be up and then for the track to take
    // the value. The popup that holds the pickers is the one modal under them.
    auto popupDepth = std::make_shared<int> (0);
    const auto pick = [&host, &ctx, select, popupDepth]
                      (std::vector<Step>& into, int kind, int row, std::function<bool()> taken, std::string what)
    {
        into.push_back ({ 0, [&host, &ctx, kind]
        { ctx.expect (host.clickMidiSelector (0, kind), "MIDI selector was not visible"); },
        [&host, popupDepth] { return host.modalCount() == *popupDepth; }, "the previous MIDI picker never closed" });
        // While a picker is still to open its combo ignores a second click.
        into.push_back ({ 0, [select, row] { select (row); },
        [&host, popupDepth] { return host.modalCount() > *popupDepth; }, "the MIDI picker did not open",
        [&host, kind] { host.clickMidiSelector (0, kind); } });
        // The keys choose by position from the top, so going through them
        // again picks the same row.
        into.push_back ({ 0, [] {},
        [&host, popupDepth, taken] { return host.modalCount() == *popupDepth && taken(); }, std::move (what),
        [&host, select, row, popupDepth] { if (host.modalCount() > *popupDepth) select (row); } });
    };
    auto steps = std::make_shared<std::vector<Step>>();
    // The client and port made above reach the app as a hot-plug notice, and
    // the refresh it arms rebuilds every strip's pickers and reopens the MIDI
    // outputs. Picking starts once that has run, so it cannot land mid-pick.
    steps->push_back ({ 0, [&host, &ctx, popupDepth]
    {
        ctx.expect (host.openMidiIo (0), "MIDI I/O popup did not open");
        *popupDepth = host.modalCount();
    },
    [&engine, noticesBefore]
    { return engine.midiHotplugNoticesForScenario() > noticesBefore && engine.midiHotplugSettledForScenario(); },
    "the hot-plug refresh for the private MIDI destination never finished" });
    pick (*steps, 0, keyboard + 1, [&track, keyboard] { return track.midiInputIndex.load() == keyboard; },
          "input picker did not select the virtual keyboard");
    pick (*steps, 1, 3, [&track] { return track.midiChannel.load() == 3; }, "channel picker did not select channel 3");
    pick (*steps, 2, destination + 1, [&track, destination] { return track.midiOutputIndex.load() == destination; },
          "output picker did not select the destination");
    auto heard = std::make_shared<std::pair<bool, bool>> (false, false);
    steps->push_back ({ 0, [&host, &track, &engine, &ctx, keyboard, keyboardId, destination, destinationId]
    {
        ctx.expect (track.midiInputIndex.load() == keyboard && track.midiInputIdentifier.toStdString() == keyboardId,
                    "input picker did not select and identify the virtual keyboard");
        ctx.expect (host.midiSelectorText (0, 0) == "Virtual Keyboard (Dusk Studio)", "input picker displayed the wrong port");
        ctx.expect (track.midiChannel.load() == 3 && host.midiSelectorText (0, 1) == "Ch 3", "channel picker did not select channel 3");
        ctx.expect (track.midiOutputIndex.load() == destination && track.midiOutputIdentifier.toStdString() == destinationId,
                    "output picker did not select and identify the destination");
        const std::uint8_t rejected[] = { 0xb1, 74, 100 };
        const std::uint8_t accepted[] = { 0xb2, 74, 101 };
        engine.postVirtualKeyboardMidi (rejected, 3);
        engine.postVirtualKeyboardMidi (accepted, 3);
    } });
    // The rejected message is posted first, so by the time the accepted one
    // comes out, a filter that let the other through has already sent it.
    steps->push_back ({ 0, [&ctx, heard]
    { ctx.expect (! heard->second, "channel filter passed the rejected channel to MIDI out"); },
    [seq, heard]
    {
        snd_seq_event_t* event = nullptr;
        while (snd_seq_event_input (seq.get(), &event) >= 0)
            if (event != nullptr && event->type == SND_SEQ_EVENT_CONTROLLER && event->data.control.param == 74)
            {
                heard->first |= event->data.control.channel == 2 && event->data.control.value == 101;
                heard->second |= event->data.control.channel == 1 && event->data.control.value == 100;
            }
        return heard->first;
    }, "selected MIDI output did not receive the accepted channel" });
    pick (*steps, 0, 0, [&track] { return track.midiInputIndex.load() == -1; }, "input None did not clear the route");
    pick (*steps, 1, 0, [&track] { return track.midiChannel.load() == 0; }, "Omni did not clear the channel filter");
    pick (*steps, 2, 0, [&track] { return track.midiOutputIndex.load() == -1; }, "output None did not clear the route");
    steps->push_back ({ 0, [&host, &track, &ctx]
    {
        ctx.expect (track.midiInputIndex.load() == -1 && track.midiInputIdentifier.isEmpty()
                    && host.midiSelectorText (0, 0) == "None", "input None did not clear the route");
        ctx.expect (track.midiChannel.load() == 0 && host.midiSelectorText (0, 1) == "Omni", "Omni did not clear the channel filter");
        ctx.expect (track.midiOutputIndex.load() == -1 && track.midiOutputIdentifier.isEmpty()
                    && host.midiSelectorText (0, 2) == "None", "output None did not clear the route");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar midiSelectors { Scenario {
    "gui.midi_io_selectors", { "gui", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMidiSelectors (host, ctx); }
} };

std::optional<ScenarioResult> runFaderEntry (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    const bool hardwareInputs = engine.setHardwareMidiInputsEnabledForScenario (false);
    ctx.cleanup ([&engine, hardwareInputs] { engine.setHardwareMidiInputsEnabledForScenario (hardwareInputs); });
    const auto bindings = session.midiBindings.current();
    const auto originalStage = engine.getStage();
    ctx.cleanup ([&host, &session, bindings, originalStage]
    {
        host.pressPeerKey ("Escape");
        drainModals (host);
        session.midiBindings.mutate ([&] (auto& value) { value = bindings; });
        host.switchToStage (guiStage (originalStage));
    });
    ctx.cleanup (host.preserveKeyboardFocus());
    ctx.keep (session.midiLearnPending);
    ctx.keep (session.midiLearnCapture);
    session.midiLearnPending.store (-1);
    session.midiLearnCapture.store (0);
    session.midiBindings.mutate ([] (auto& value) { value.clear(); });
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& track = session.track (t);
        ctx.keep (track.strip.faderGroupId);
        ctx.keep (track.strip.faderDb);
        ctx.keep (track.automationMode);
        track.strip.faderGroupId.store (t < 2 ? 1 : 0);
        track.automationMode.store ((int) AutomationMode::Off);
        track.strip.faderDb.store (-6.0f - (float) t * 3.0f);
    }
    host.switchToStage (GuiHost::Stage::Mixing);
    const auto type = [&host] (const std::string& value)
    {
       #if defined (__APPLE__)
        host.pressPeerKey ("command + A", 'a');
       #else
        host.pressPeerKey ("ctrl + A", 'a');
       #endif
        for (char ch : value) host.pressPeerKey (std::string (1, ch), ch);
        host.pressPeerKey ("Return", '\r');
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 250, [&host, &ctx] { ctx.expect (host.clickFader (0, true), "fader readout was not visible"); } });
    steps->push_back ({ 100, [&host, &ctx, type]
    {
        ctx.expect (host.faderEditing (0), "clicking the fader readout did not open an editor");
        if (host.faderEditing (0)) type ("-3.5");
    } });
    steps->push_back ({ 150, [&ctx, &session]
    {
        ctx.expect (std::abs (session.track (0).strip.faderDb.load() + 3.5f) < 0.01f, "typed fader value was not applied");
        ctx.expect (std::abs (session.track (1).strip.faderDb.load() + 6.5f) < 0.01f, "typed fader value lost the group offset");
        ctx.expect (! session.track (0).strip.faderTouched.load(), "text entry left the fader touched");
        session.track (0).automationMode.store ((int) AutomationMode::Read);
    } });
    steps->push_back ({ 100, [&host, &ctx] { host.clickFader (0, true); ctx.expect (! host.faderEditing (0), "READ allowed fader text editing"); } });
    steps->push_back ({ 100, [&session]
    {
        session.track (0).automationMode.store ((int) AutomationMode::Off);
        session.track (0).strip.faderGroupId.store (0);
        session.track (1).strip.faderGroupId.store (0);
    } });
    steps->push_back ({ 100, [&host, &ctx] { ctx.expect (host.clickFader (0, false, true), "could not right-click the fader"); } });
    steps->push_back ({ 100, [&host, &ctx] { ctx.expect (host.clickModalAt (0.5f, 48.0f / 102.0f), "MIDI Learn menu did not open"); } });
    steps->push_back ({ 100, [&session, &engine, &ctx]
    {
        ctx.expect (session.midiLearnPending.load() == packLearnTarget (MidiBindingTarget::TrackFader, 0), "menu did not arm fader Learn");
        const std::uint8_t cc[] = { 0xb2, 74, 64 };
        engine.postVirtualKeyboardMidi (cc, 3);
    } });
    steps->push_back ({ 300, [&session, &engine, &ctx]
    {
        const auto& learned = session.midiBindings.current();
        ctx.expect (learned.size() == 1 && learned[0].channel == 3 && learned[0].dataNumber == 74
                    && learned[0].trigger == MidiBindingTrigger::CC && learned[0].target == MidiBindingTarget::TrackFader
                    && learned[0].targetIndex == 0 && session.midiLearnPending.load() == -1, "CC did not learn the selected fader");
        const std::uint8_t cc[] = { 0xb2, 74, 127 };
        engine.postVirtualKeyboardMidi (cc, 3);
    } });
    steps->push_back ({ 300, [&host, &session, &engine, &ctx]
    {
        ctx.expect (std::abs (session.track (0).strip.faderDb.load() - 12.0f) < 0.01f
                    && std::abs (host.faderValue (0) - 12.0) < 0.01, "learned CC did not raise the model and visible fader");
        const std::uint8_t cc[] = { 0xb2, 74, 0 };
        engine.postVirtualKeyboardMidi (cc, 3);
    } });
    steps->push_back ({ 300, [&host, &session, &ctx]
    {
        ctx.expect (std::abs (session.track (0).strip.faderDb.load() + 90.0f) < 0.01f
                    && std::abs (host.faderValue (0) + 90.0) < 0.01, "learned CC did not lower the model and visible fader");
        ctx.expect (std::abs (session.track (2).strip.faderDb.load() + 12.0f) < 0.01f, "fader Learn changed an unbound track");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar faderEntry { Scenario {
    "gui.fader_entry_and_learn", { "gui", "fader", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runFaderEntry (host, ctx); }
} };

std::optional<ScenarioResult> runGroupChips (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    const auto originalStage = ctx.engine().getStage();
    ctx.cleanup ([&host, originalStage]
    {
        host.switchToStage (guiStage (originalStage));
    });
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        ctx.keep (session.track (t).strip.faderGroupId);
        session.track (t).strip.faderGroupId.store (0);
    }
    host.switchToStage (GuiHost::Stage::Mixing);
    const auto check = [&host, &ctx] (int track, const std::string& expectedText, int expectedMaster, bool expectedFilled)
    {
        std::string text;
        int master = -1;
        bool filled = false;
        ctx.expect (host.groupChipView (track, text, master, filled), "the group chip did not render its fill");
        ctx.expect (text == expectedText, "the group chip label was wrong");
        ctx.expect (master == expectedMaster, "the lowest group member was not the master");
        ctx.expect (filled == expectedFilled, "the group chip used the wrong filled/outlined style");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    for (int group = 1; group <= 8; ++group)
    {
        steps->push_back ({ 100, [&session, group]
        { session.track (1).strip.faderGroupId.store (group); } });
        steps->push_back ({ 100, [&session, check, group]
        {
            check (1, "G" + std::to_string (group), 1, true);
            session.track (0).strip.faderGroupId.store (group);
        } });
        steps->push_back ({ 100, [&session, check, group]
        {
            check (0, "G" + std::to_string (group), 0, true);
            check (1, "G" + std::to_string (group), 0, false);
            session.track (0).strip.faderGroupId.store (0);
        } });
        steps->push_back ({ 100, [check, group]
        {
            check (0, "", -1, false);
            check (1, "G" + std::to_string (group), 1, true);
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar groupChips { Scenario {
    "gui.fader_group_chips", { "gui", "groups" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runGroupChips (host, ctx); }
} };

std::optional<ScenarioResult> runMeterClip (GuiHost& host, ScenarioContext& ctx)
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
    ctx.keep (track.recordArmed);
    ctx.keep (track.inputMonitor);
    ctx.keep (track.automationMode);
    track.recordArmed.store (false);
    track.inputMonitor.store (false);
    track.automationMode.store ((int) AutomationMode::Off);
    ctx.keep (session.master().mute);
    session.master().mute.store (true);
    engine.stop();
    const auto rate = engine.getCurrentSampleRate();
    const auto frames = static_cast<std::int64_t> (rate * 5.0);
    std::vector<float> samples ((size_t) frames);
    for (std::int64_t i = 0; i < frames; ++i)
        samples[(size_t) i] = 0.8f * static_cast<float> (std::sin (6.283185307179586 * 440.0 * static_cast<double> (i) / rate));
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
    track.strip.faderDb.store (12.0f);
    track.strip.mute.store (false);
    host.switchToStage (GuiHost::Stage::Recording);
    engine.getTransport().setPlayhead (0);
    engine.play();
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &engine, &ctx, &track]
    {
        // The session copy, published after each block like the meters read
        // it: the strip's own value reads silence while a block is running.
        ctx.expect (track.meterOutLDb.load (std::memory_order_relaxed) > 0.0f, "fixture did not cross 0 dBFS");
        ctx.expect (host.meterClip (0), "overload did not light the red clip bar");
        engine.stop();
    } });
    steps->push_back ({ 700, [&host, &ctx]
    { ctx.expect (host.meterClip (0), "clip bar cleared before one second"); } });
    steps->push_back ({ 600, [&host, &ctx]
    { ctx.expect (! host.meterClip (0), "clip bar did not clear after one second"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar meterClip { Scenario {
    "gui.meter_clip_hold", { "gui", "meter" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMeterClip (host, ctx); }
} };

const ScenarioRegistrar automation { Scenario {
    "gui.automation_write_and_touch",
    { "gui", "automation" },
    Needs::Engine | Needs::Gui,
    {},
    {},
    60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAutomation (host, ctx); }
} };

const ScenarioRegistrar modeShown { Scenario {
    "gui.automation_mode_shown_on_every_strip",
    { "gui", "automation" },
    Needs::Engine | Needs::Gui,
    {},
    {},
    20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runModeShownOnEveryStrip (host, ctx); }
} };

// File > Optimize automation rewrites every lane, so it refuses while the
// transport rolls or any track, aux lane or the master is in Read, Touch or
// Write, and says so; once both are clear it thins the lanes and reports the
// point counts.
std::optional<ScenarioResult> runOptimizeAutomation (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& transport = ctx.engine().getTransport();
    if (! transport.isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no open modal");

    std::vector<AutomationLane*> lanes;
    std::vector<std::atomic<int>*> modes;
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        for (auto& lane : session.track (t).automationLanes) lanes.push_back (&lane);
        modes.push_back (&session.track (t).automationMode);
    }
    for (int a = 0; a < Session::kNumAuxLanes; ++a)
    {
        for (auto& lane : session.auxLane (a).params.automationLanes) lanes.push_back (&lane);
        modes.push_back (&session.auxLane (a).params.automationMode);
    }
    for (auto& lane : session.master().automationLanes) lanes.push_back (&lane);
    modes.push_back (&session.master().automationMode);

    std::vector<std::vector<AutomationPoint>> savedLanes;
    for (auto* lane : lanes) savedLanes.push_back (lane->pointsConst());
    for (auto* mode : modes) ctx.keep (*mode);
    ctx.cleanup ([&host, &transport, lanes, savedLanes, state = transport.getState(),
                  position = transport.getPlayhead()]
    {
        drainModals (host);
        transport.setPlayhead (position);
        transport.setState (state);
        for (std::size_t i = 0; i < lanes.size(); ++i) lanes[i]->publishPoints (savedLanes[i]);
    });
    for (auto* lane : lanes) lane->publishPoints ({});
    for (auto* mode : modes) mode->store ((int) AutomationMode::Off, std::memory_order_release);

    // A straight fader ramp: the three interior points lie on the chord.
    auto& ramp = session.track (kStripIndex).automationLanes[(std::size_t) AutomationParam::FaderDb];
    std::vector<AutomationPoint> seeded;
    for (int i = 0; i < 5; ++i)
        seeded.push_back ({ (std::int64_t) i * 12000, 0.2f + 0.1f * (float) i, 120.0f });
    ramp.publishPoints (seeded);

    auto& readMode  = session.track (5).automationMode;
    auto& touchMode = session.auxLane (0).params.automationMode;
    auto& writeMode = session.master().automationMode;
    const std::string stopPlayback =
        "Optimize automation\nStop playback before optimising automation. The optimiser rewrites "
        "every lane's point data; running it while the audio thread may be reading the lanes is unsafe.";
    const std::string setOff =
        "Optimize automation\nSet every strip's automation mode to Off before optimising. The "
        "optimiser rewrites lane data; doing it while a strip is in Read, Touch or Write can race the audio thread.";

    auto steps = std::make_shared<std::vector<Step>>();
    const auto attempt = [&host, &ctx, &ramp, steps] (const std::string& expected, std::size_t points,
                                                      const std::string& how, std::function<void()> after)
    {
        steps->push_back ({ 300, [&host, &ctx, how]
        {
            ctx.expect (host.modalStackEmpty(), how + ": a modal was still open");
            ctx.expect (host.clickFileMenu(), how + ": the File menu is unavailable");
        } });
        steps->push_back ({ 200, [&host, &ctx, how]
        {
            ctx.expect (host.clickContextMenuItem ("Optimize automation..."),
                        how + ": the File menu has no Optimize automation...");
        } });
        steps->push_back ({ 400, [&host, &ctx, &ramp, expected, points, how, after]
        {
            ctx.expect (host.modalText() == expected, how + ": the alert read '" + host.modalText() + "'");
            ctx.expect (ramp.pointsConst().size() == points,
                        how + ": the lane holds " + std::to_string (ramp.pointsConst().size()) + " points");
            ctx.expect (host.clickModalButton ("OK"), how + ": the alert has no OK button");
            after();
        } });
    };

    transport.setState (Transport::State::Playing);
    attempt (stopPlayback, 5, "while playing", [&transport, &readMode]
    {
        transport.setState (Transport::State::Stopped);
        readMode.store ((int) AutomationMode::Read, std::memory_order_release);
    });
    attempt (setOff, 5, "with a track in Read", [&readMode, &touchMode]
    {
        readMode.store ((int) AutomationMode::Off, std::memory_order_release);
        touchMode.store ((int) AutomationMode::Touch, std::memory_order_release);
    });
    attempt (setOff, 5, "with an aux lane in Touch", [&touchMode, &writeMode]
    {
        touchMode.store ((int) AutomationMode::Off, std::memory_order_release);
        writeMode.store ((int) AutomationMode::Write, std::memory_order_release);
    });
    attempt (setOff, 5, "with the master in Write", [&writeMode]
    { writeMode.store ((int) AutomationMode::Off, std::memory_order_release); });
    attempt ("Optimize automation\nThinned 5 automation points down to 2.", 2, "stopped with every strip Off",
             [&ctx, &ramp, seeded]
    {
        const auto& kept = ramp.pointsConst();
        ctx.expect (kept.size() == 2 && kept.front() == seeded.front() && kept.back() == seeded.back(),
                    "thinning did not keep the ramp's endpoints");
    });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.modalStackEmpty(), "the success alert did not close"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar optimizeAutomation { Scenario {
    "gui.optimize_automation_refusals", { "gui", "automation" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runOptimizeAutomation (host, ctx); }
} };

std::optional<ScenarioResult> runAuxSelectors (GuiHost& host, ScenarioContext& ctx)
{
    const auto stage = ctx.engine().getStage();
    host.switchToStage (GuiHost::Stage::Aux);
    const int original = host.activeAuxLane();
    ctx.cleanup ([&host, original, stage]
    {
        host.switchToStage (GuiHost::Stage::Aux);
        host.clickAuxSelector (original);
        switch (stage)
        {
            case AudioEngine::Stage::Recording: host.switchToStage (GuiHost::Stage::Recording); break;
            case AudioEngine::Stage::Mixing: host.switchToStage (GuiHost::Stage::Mixing); break;
            case AudioEngine::Stage::Aux: break;
            case AudioEngine::Stage::Mastering: host.switchToStage (GuiHost::Stage::Mastering); break;
        }
    });
    auto steps = std::make_shared<std::vector<Step>>();
    for (const int lane : { 3, 1, 0, 2 })
    {
        steps->push_back ({ 100, [&host, &ctx, lane]
        { ctx.expect (host.clickAuxSelector (lane), "the AUX selector is unavailable"); } });
        steps->push_back ({ 100, [&host, &ctx, lane]
        {
            ctx.expect (host.auxLaneLayoutMatches (lane), "AUX did not show exactly the selected lane at full width");
            host.switchToStage (GuiHost::Stage::Mixing);
        } });
        steps->push_back ({ 100, [&host] { host.switchToStage (GuiHost::Stage::Aux); } });
        steps->push_back ({ 100, [&host, &ctx, lane]
        { ctx.expect (host.auxLaneLayoutMatches (lane), "changing stages lost the selected AUX lane"); } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar auxSelectors { Scenario {
    "gui.aux_selectors", { "gui", "aux" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAuxSelectors (host, ctx); }
} };

std::optional<ScenarioResult> runAccessibleControls (GuiHost& host, ScenarioContext& ctx)
{
    auto& strip = ctx.session().track (0).strip;
    ctx.keep (strip.faderDb);
    ctx.keep (strip.pan);
    ctx.keep (strip.hpfFreq);
    ctx.keep (strip.compFetRatio);
    ctx.keep (strip.auxSendDb[0]);
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Recording);
    std::string value, help;
    for (int track = 1; track <= Session::kNumTracks; ++track)
        for (const auto* suffix : { "fader", "pan", "mute", "solo", "record arm", "input monitor",
                                   "high-pass filter frequency", "low-pass filter frequency", "insert slot" })
        {
            const auto title = "Track " + std::to_string (track) + " " + suffix;
            ctx.expect (host.accessibleControl (title, value, help), "missing accessible name: " + title);
        }
    for (const auto* title : { "Play", "Stop", "Record", "Rewind", "Fast forward" })
        ctx.expect (host.accessibleControl (title, value, help), std::string ("missing transport accessible name: ") + title);
    host.switchToStage (GuiHost::Stage::Mixing);
    for (int track = 1; track <= Session::kNumTracks; ++track)
        for (int aux = 1; aux <= Session::kNumAuxLanes; ++aux)
        {
            const auto title = "Track " + std::to_string (track) + " aux " + std::to_string (aux) + " send";
            ctx.expect (host.accessibleControl (title, value, help), "missing accessible name: " + title);
        }
    host.switchToStage (GuiHost::Stage::Aux);
    for (int aux = 1; aux <= Session::kNumAuxLanes; ++aux)
        for (const auto* suffix : { "return fader", "mute", "plugin slot 1" })
        {
            const auto title = "Aux " + std::to_string (aux) + " " + suffix;
            ctx.expect (host.accessibleControl (title, value, help), "missing accessible name: " + title);
        }
    host.switchToStage (GuiHost::Stage::Mixing);
    auto steps = std::make_shared<std::vector<Step>>();
    // An aux send's bottom end stop stores the OFF sentinel and must read
    // OFF, as the strip shows it, rather than an audible -60 dB.
    struct Value { const char* title; const char* input; const char* output; float sendDb = std::nanf (""); };
    const Value values[] {
        { "Track 1 fader", "-4.2", "-4.2 dB" },
        { "Track 1 pan", "-0.42", "L42" },
        { "Track 1 high-pass filter frequency", "OFF", "OFF" },
        { "FET ratio", "0", "4:1" },
        { "Track 1 fader", "-90", "-INF dB" },
        { "Track 1 fader", "-96", "-INF dB" },
        { "Track 1 fader", "-INF", "-INF dB" },
        { "Track 1 fader", "-4.2", "-4.2 dB" },
        { "Track 1 fader", "-INF dB", "-INF dB" },
        { "Track 1 aux 1 send", "-12", "-12.0 dB", -12.0f },
        { "Track 1 aux 1 send", "-60", "OFF", ChannelStripParams::kAuxSendOffDb },
        { "Track 1 aux 1 send", "-59.9", "-59.9 dB", -59.9f },
        { "Track 1 aux 1 send", "OFF", "OFF", ChannelStripParams::kAuxSendOffDb },
        { "Track 1 aux 1 send", "+3 dB", "3.0 dB", 3.0f }
    };
    for (const auto item : values)
    {
        steps->push_back ({ 100, [&host, &ctx, item]
        { ctx.expect (host.setAccessibleValue (item.title, item.input), "the accessible value action is unavailable"); } });
        steps->push_back ({ 100, [&host, &ctx, &strip, item]
        {
            std::string formatted, hint;
            ctx.expect (host.accessibleControl (item.title, formatted, hint) && formatted == item.output,
                        std::string (item.title) + " formatted value: expected " + item.output + ", got " + formatted);
            ctx.expect (! hint.empty(), std::string (item.title) + " has no accessible help");
            if (std::string (item.input).find ("-INF") == 0)
                ctx.expect (std::abs (strip.faderDb.load() - ChannelStripParams::kFaderMinDb) < 0.001f,
                            "the accessible -INF action did not mute the channel fader");
            if (! std::isnan (item.sendDb))
                ctx.expect (std::abs (strip.auxSendDb[0].load() - item.sendDb) < 0.001f,
                            std::string ("aux 1 send after ") + item.input + ": expected "
                                + std::to_string (item.sendDb) + " dB stored, got "
                                + std::to_string (strip.auxSendDb[0].load()));
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar accessibleControls { Scenario {
    "gui.accessible_controls", { "gui", "accessibility" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAccessibleControls (host, ctx); }
} };

// Format 7 stored channel EQ dial positions, and some played past today's
// knobs. The session loads at the Hz it played and keeps each dial, which the
// band plays until its frequency moves; the knob rests on its end stop reading
// that Hz, and nothing writes the end stop back, or drops the dial, until the
// knob moves. The strip's knobs and the EQ editor's each drop only their own
// dial, and Reset EQ drops them all.
std::optional<ScenarioResult> runEqHeldPastKnob (GuiHost& host, ScenarioContext& ctx)
{
    using EqFreq = ChannelStripParams::EqFreq;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Mixing);
    const bool originalCompact = host.setStripCompact (0, false);
    ctx.cleanup ([&host, &session, originalDir, restore, originalCompact]
    {
        host.closeStripModuleEditors (0);
        drainModals (host);
        host.setStripCompact (0, originalCompact);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });

    // A Black LF dial at 400 played about 660 Hz and a Black HPF at 300 about
    // 316 Hz, past the 450 Hz and 300 Hz tops of today's knobs. A Black HF
    // dial at 1000 played about 637 Hz, below the knob's 1.5 kHz.
    const auto legacy = ctx.tempDir() / "legacy" / "session.json";
    std::error_code created;
    std::filesystem::create_directories (legacy.parent_path(), created);
    {
        std::ofstream out (legacy);
        out << R"({"version":7,"tracks":[{"eq":{"enabled":true,"type":"black","lf":{"gain":6,"freq":400},)"
               R"("hf":{"gain":6,"freq":1000}},"hpf":{"enabled":true,"freq":300}}]})";
        if (! out) return ScenarioResult::fail ("could not write the format 7 session");
    }
    if (! host.openSession (legacy)) return ScenarioResult::fail ("could not open the format 7 session");
    auto& strip = session.track (0).strip;
    const float lf = strip.lfFreq.load(), hf = strip.hfFreq.load(), hpf = strip.hpfFreq.load();
    const auto dialOf = [&strip] (EqFreq f)
    {
        float hz = 0.0f;
        return strip.legacyDial (f).dialFor (strip.eqFreq (f), strip.eqBlackMode.load(), hz);
    };
    if (! (lf > ChannelStripParams::kLfFreqMax && hf < ChannelStripParams::kHfFreqMin
           && hpf > ChannelStripParams::kHpfMaxHz))
        return ScenarioResult::fail ("the format 7 session did not load past the knobs, or opening it wrote the end stops back");

    const auto reads = [&host] (const std::string& title)
    {
        std::string value, help;
        return host.accessibleControl (title, value, help) ? value : std::string ("<missing>");
    };
    const auto shown = [] (float hz) { return std::to_string ((int) std::lround (hz)); };
    const auto boxReads = [&host, &ctx] (const std::string& title, const std::string& expected, const std::string& what)
    {
        const auto text = host.valueBoxText (title);
        ctx.expect (text == expected, what + " reads " + text + ", expected " + expected);
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&ctx, &strip, lf, hf, hpf, reads, shown, boxReads, dialOf]
    {
        ctx.expect (dialOf (EqFreq::Lf) > 0.0f && dialOf (EqFreq::Hf) > 0.0f && dialOf (EqFreq::Hpf) > 0.0f,
                    "the format 7 session's bands are not playing their dials");
        ctx.expect (std::abs (strip.lfFreq.load() - lf) < 1.0e-3f, "building the strip wrote the LF knob's end stop back");
        ctx.expect (std::abs (strip.hfFreq.load() - hf) < 1.0e-3f, "building the strip wrote the HF knob's end stop back");
        ctx.expect (std::abs (strip.hpfFreq.load() - hpf) < 1.0e-3f, "building the strip wrote the HPF knob's end stop back");
        ctx.expect (reads ("Track 1 LF frequency") == shown (lf),
                    "the LF knob reads " + reads ("Track 1 LF frequency") + ", the band plays " + shown (lf));
        ctx.expect (reads ("Track 1 high-pass filter frequency") == shown (hpf),
                    "the HPF knob reads " + reads ("Track 1 high-pass filter frequency") + ", the filter plays " + shown (hpf));
        boxReads ("Track 1 LF frequency", shown (lf), "the LF value box");
        boxReads ("Track 1 HF frequency", shown (hf), "the HF value box");
        boxReads ("Track 1 high-pass filter frequency", shown (hpf), "the HPF value box");
        boxReads ("Track 2 high-pass filter frequency", "OFF", "track 2's HPF value box");
    } });
    steps->push_back ({ 100, [&host, &ctx]
    {
        ctx.expect (host.setAccessibleValue ("Track 1 LF frequency", "300"), "the LF knob takes no value");
        ctx.expect (host.setAccessibleValue ("Track 1 high-pass filter frequency", "120"), "the HPF knob takes no value");
    } });
    steps->push_back ({ 100, [&ctx, &strip, reads, dialOf]
    {
        ctx.expect (std::abs (strip.lfFreq.load() - 300.0f) < 0.5f, "moving the LF knob did not bring the band into range");
        ctx.expect (std::abs (strip.hpfFreq.load() - 120.0f) < 0.5f && strip.hpfEnabled.load(),
                    "moving the HPF knob did not bring the filter into range");
        ctx.expect (reads ("Track 1 LF frequency") == "300", "the LF knob reads " + reads ("Track 1 LF frequency"));
        ctx.expect (strip.legacyDial (EqFreq::Lf).raw() == 0 && strip.legacyDial (EqFreq::Hpf).raw() == 0,
                    "moving the LF and HPF knobs kept their format 7 dials");
        ctx.expect (dialOf (EqFreq::Hf) > 0.0f, "moving the LF and HPF knobs dropped the HF band's dial");
        strip.setEqFreq (EqFreq::Hpf, ChannelStripParams::kHpfOffHz);
        strip.hpfEnabled.store (false);
    } });
    // The EQ editor builds its knobs on each open, from the stored values.
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.clickStripModule (0, 0, true, false), "EQ module label is unavailable"); } });
    steps->push_back ({ 500, [&host, &ctx, &strip, hf, shown, boxReads, dialOf]
    {
        ctx.expect (host.stripModuleEditorOpen (0, 0), "the EQ label did not open its editor");
        boxReads ("EQ editor HF frequency", shown (hf) + " Hz", "the editor's HF value box");
        boxReads ("EQ editor LF frequency", "300 Hz", "the editor's LF value box");
        boxReads ("EQ editor high-pass filter frequency", "off", "the editor's HPF value box");
        ctx.expect (std::abs (strip.hfFreq.load() - hf) < 1.0e-3f && dialOf (EqFreq::Hf) > 0.0f,
                    "opening the EQ editor wrote the HF band back or dropped its dial");
        ctx.expect (host.setAccessibleValue ("EQ editor HF frequency", "2000"), "the editor's HF knob takes no value");
    } });
    steps->push_back ({ 100, [&host, &ctx, &strip]
    {
        ctx.expect (std::abs (strip.hfFreq.load() - 2000.0f) < 0.5f, "moving the editor's HF knob did not move the band");
        ctx.expect (strip.legacyDial (EqFreq::Hf).raw() == 0, "moving the editor's HF knob kept its format 7 dial");
        host.closeStripModuleEditors (0);
    } });
    // A write from MIDI or a control surface lands on the strip's own knobs.
    steps->push_back ({ 100, [&strip]
    {
        strip.setEqFreq (EqFreq::Lf, 250.0f);
        strip.lfGainDb.store (5.0f);
        strip.lmQ.store (1.5f);
    } });
    steps->push_back ({ 200, [boxReads]
    {
        boxReads ("Track 1 LF frequency", "250", "the LF value box after an outside write");
        boxReads ("Track 1 LF gain", "+5", "the LF gain value box after an outside write");
        boxReads ("Track 1 LM Q", "1.5", "the LM Q value box after an outside write");
    } });
    // Reset EQ writes each knob's default even onto a knob already there: an
    // LM band stored between the knob's 1 Hz steps, with a dial, reads 600.
    steps->push_back ({ 200, [&host, &ctx, &strip]
    {
        strip.lmFreq.store (600.3f);
        strip.legacyDial (EqFreq::Lm).set (870.0f, 600.3f, strip.eqBlackMode.load());
        ctx.expect (host.clickStripModule (0, 0, true, true), "EQ module right-click failed");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Reset EQ"), "the EQ menu did not offer Reset EQ"); } });
    steps->push_back ({ 200, [&ctx, &strip]
    {
        ctx.expect (std::abs (strip.lmFreq.load() - 600.0f) < 1.0e-3f && strip.legacyDial (EqFreq::Lm).raw() == 0,
                    "Reset EQ left LM between its knob's steps, or kept its dial");
        ctx.expect (std::abs (strip.lfFreq.load() - 100.0f) < 0.5f && std::abs (strip.hfFreq.load() - 8000.0f) < 0.5f,
                    "Reset EQ did not reset LF and HF");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar eqHeldPastKnob { Scenario {
    "gui.eq_held_past_knob", { "gui", "eq" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runEqHeldPastKnob (host, ctx); }
} };

// A filter's value box reads OFF from its switch, not its knob: format 7 could
// leave a filter on at its OFF end, where it plays, and one off anywhere else.
// Both views follow a switch made under them, as MIDI or a control surface
// makes it, and a knob that switches its own filter.
std::optional<ScenarioResult> runEqFilterReadsItsSwitch (GuiHost& host, ScenarioContext& ctx)
{
    using EqFreq = ChannelStripParams::EqFreq;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Mixing);
    const bool originalCompact = host.setStripCompact (0, false);
    ctx.cleanup ([&host, &session, originalDir, restore, originalCompact]
    {
        host.closeStripModuleEditors (0);
        drainModals (host);
        host.setStripCompact (0, originalCompact);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });

    const auto legacy = ctx.tempDir() / "legacy" / "session.json";
    std::error_code created;
    std::filesystem::create_directories (legacy.parent_path(), created);
    {
        std::ofstream out (legacy);
        out << R"({"version":7,"tracks":[{"hpf":{"enabled":true,"freq":20},"lpf":{"enabled":true,"freq":20000}},)"
               R"({"hpf":{"enabled":false,"freq":150},"lpf":{"enabled":false,"freq":8000}}]})";
        if (! out) return ScenarioResult::fail ("could not write the format 7 session");
    }
    if (! host.openSession (legacy)) return ScenarioResult::fail ("could not open the format 7 session");
    auto& on = session.track (0).strip;
    const auto& off = session.track (1).strip;
    if (! (on.hpfEnabled.load() && on.lpfEnabled.load()
           && std::abs (on.hpfFreq.load() - ChannelStripParams::kHpfOffHz) < 1.0e-3f
           && std::abs (on.lpfFreq.load() - ChannelStripParams::kLpfOffHz) < 1.0e-3f
           && ! off.hpfEnabled.load() && ! off.lpfEnabled.load()
           && off.hpfFreq.load() > ChannelStripParams::kHpfOffHz + 0.5f
           && off.lpfFreq.load() < ChannelStripParams::kLpfOffHz - 0.5f))
        return ScenarioResult::fail ("the format 7 session did not load its filters on at OFF and off away from it");

    const auto reads = [&host] (const std::string& title)
    {
        std::string value, help;
        return host.accessibleControl (title, value, help) ? value : std::string ("<missing>");
    };
    const auto boxReads = [&host, &ctx] (const std::string& title, const std::string& expected, const std::string& what)
    {
        const auto text = host.valueBoxText (title);
        ctx.expect (text == expected, what + " reads " + text + ", expected " + expected);
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx, reads, boxReads]
    {
        boxReads ("Track 1 high-pass filter frequency", "20", "an HPF on at OFF");
        boxReads ("Track 1 low-pass filter frequency", "20k", "an LPF on at OFF");
        boxReads ("Track 2 high-pass filter frequency", "OFF", "an HPF off at 150");
        boxReads ("Track 2 low-pass filter frequency", "OFF", "an LPF off at 8k");
        ctx.expect (reads ("Track 1 high-pass filter frequency") == "20",
                    "an HPF on at OFF reports " + reads ("Track 1 high-pass filter frequency"));
        ctx.expect (reads ("Track 2 low-pass filter frequency") == "OFF",
                    "an LPF off at 8k reports " + reads ("Track 2 low-pass filter frequency"));
        ctx.expect (host.clickStripModule (0, 0, true, false), "EQ module label is unavailable");
    } });
    steps->push_back ({ 500, [&host, &ctx, &on, boxReads]
    {
        ctx.expect (host.stripModuleEditorOpen (0, 0), "the EQ label did not open its editor");
        boxReads ("EQ editor high-pass filter frequency", "20 Hz", "the editor's HPF on at OFF");
        boxReads ("EQ editor low-pass filter frequency", "20.0 kHz", "the editor's LPF on at OFF");
        on.setEqFreq (EqFreq::Hpf, ChannelStripParams::kHpfOffHz);
        on.hpfEnabled.store (false);
        on.setEqFreq (EqFreq::Lpf, 8000.0f);
    } });
    steps->push_back ({ 200, [&host, &ctx, boxReads]
    {
        boxReads ("EQ editor high-pass filter frequency", "off", "the editor's HPF switched off under it");
        boxReads ("EQ editor low-pass filter frequency", "8.0 kHz", "the editor's LPF moved under it");
        boxReads ("Track 1 high-pass filter frequency", "OFF", "the strip's HPF switched off under it");
        boxReads ("Track 1 low-pass filter frequency", "8k", "the strip's LPF moved under it");
        ctx.expect (host.setAccessibleValue ("EQ editor high-pass filter frequency", "120"), "the editor's HPF takes no value");
        boxReads ("EQ editor high-pass filter frequency", "120 Hz", "the editor's HPF knob switching it on");
        ctx.expect (host.setAccessibleValue ("EQ editor low-pass filter frequency", "20000"), "the editor's LPF takes no value");
        boxReads ("EQ editor low-pass filter frequency", "off", "the editor's LPF knob switching it off");
    } });
    steps->push_back ({ 200, [&host, &ctx, boxReads]
    {
        boxReads ("Track 1 high-pass filter frequency", "120", "the strip's HPF moved on by the editor");
        boxReads ("Track 1 low-pass filter frequency", "OFF", "the strip's LPF switched off by the editor");
        host.closeStripModuleEditors (0);
        ctx.expect (host.setAccessibleValue ("Track 1 high-pass filter frequency", "OFF"), "the HPF knob takes no value");
        boxReads ("Track 1 high-pass filter frequency", "OFF", "the strip's HPF knob switching it off");
        ctx.expect (host.setAccessibleValue ("Track 1 low-pass filter frequency", "5000"), "the LPF knob takes no value");
        boxReads ("Track 1 low-pass filter frequency", "5k", "the strip's LPF knob switching it on");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar eqFilterReadsItsSwitch { Scenario {
    "gui.eq_filter_reads_its_switch", { "gui", "eq" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runEqFilterReadsItsSwitch (host, ctx); }
} };

// Each band's frequency knob spans the range the manual's EQ table gives.
std::optional<ScenarioResult> runEqBandKnobRanges (GuiHost& host, ScenarioContext& ctx)
{
    auto& strip = ctx.session().track (0).strip;
    struct Band { const char* control; std::atomic<float>* freq; float lo, hi; };
    const Band bands[] {
        { "Track 1 LF frequency", &strip.lfFreq,   30.0f,   450.0f },
        { "Track 1 LM frequency", &strip.lmFreq,  200.0f,  2500.0f },
        { "Track 1 HM frequency", &strip.hmFreq,  600.0f,  7000.0f },
        { "Track 1 HF frequency", &strip.hfFreq, 1500.0f, 16000.0f },
    };
    ctx.keep (strip.eqEnabled);
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Mixing);
    const bool originalCompact = host.setStripCompact (0, false);
    ctx.cleanup ([&host, originalCompact] { host.setStripCompact (0, originalCompact); });
    for (const auto& band : bands)
        ctx.cleanup ([&host, band, was = band.freq->load()]
        {
            host.setAccessibleValue (band.control, std::to_string (was));
            band.freq->store (was);
        });
    for (const auto& band : bands)
    {
        for (const bool top : { true, false })
        {
            const auto label = std::string (band.control) + (top ? " top" : " bottom");
            ctx.expect (host.setAccessibleValue (band.control, top ? "99999" : "1"), label + ": the knob takes no value");
            const float expected = top ? band.hi : band.lo;
            ctx.expect (std::abs (band.freq->load() - expected) < 0.5f,
                        label + " stored " + std::to_string (band.freq->load()) + ", expected " + std::to_string (expected));
        }
    }
    return ctx.verdict();
}

const ScenarioRegistrar eqBandKnobRanges { Scenario {
    "gui.eq_band_knob_ranges", { "gui", "eq" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runEqBandKnobRanges (host, ctx); }
} };

// The strip lists its bands HF first, top to bottom; a binding numbers them
// from LF. Learning on a row has to bind that row's band.
std::optional<ScenarioResult> runEqMidiLearnBand (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty() || session.midiLearnPending.load() >= 0)
        return ScenarioResult::skip ("requires stopped transport, no modal and no pending learn");
    const bool hardwareInputs = engine.setHardwareMidiInputsEnabledForScenario (false);
    ctx.cleanup ([&engine, hardwareInputs] { engine.setHardwareMidiInputsEnabledForScenario (hardwareInputs); });
    const auto capture = session.midiLearnCapture.load();
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Mixing);
    const bool originalCompact = host.setStripCompact (0, false);
    // A click on a strip focuses it and selects its track; reopening the
    // session rebuilds the console without either.
    ctx.cleanup ([&host, &session, capture, originalCompact, originalDir, restore]
    {
        drainModals (host);
        host.setStripCompact (0, originalCompact);
        session.midiLearnPending.store (-1);
        session.midiLearnCapture.store (capture);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });
    struct Row { const char* control; MidiBindingTarget kind; int band; };
    static constexpr Row kRows[] {
        { "Track 1 HF frequency", MidiBindingTarget::TrackEqFreq, 3 },
        { "Track 1 HF gain",      MidiBindingTarget::TrackEqGain, 3 },
        { "Track 1 HM Q",         MidiBindingTarget::TrackEqQ,    2 },
        { "Track 1 LM frequency", MidiBindingTarget::TrackEqFreq, 1 },
        { "Track 1 LF gain",      MidiBindingTarget::TrackEqGain, 0 },
    };
    auto steps = std::make_shared<std::vector<Step>>();
    for (const auto& row : kRows)
    {
        steps->push_back ({ 150, [&host, &ctx, &session, row]
        {
            session.midiLearnPending.store (-1);
            ctx.expect (host.clickTitledControl (row.control, true), std::string (row.control) + " is unavailable");
        } });
        steps->push_back ({ 150, [&host, &ctx, row]
        {
            ctx.expect (host.clickContextMenuItem ("MIDI Learn (this track)..."),
                        std::string (row.control) + " offers no MIDI Learn");
        } });
        steps->push_back ({ 150, [&ctx, &session, row]
        {
            const auto pending = session.midiLearnPending.load();
            const bool bound = pending >= 0 && unpackLearnTargetKind (pending) == row.kind
                               && unpackLearnTargetIndex (pending) == packTrackEqBand (0, row.band);
            ctx.expect (bound, std::string ("MIDI Learn on ") + row.control + " armed band "
                                   + std::to_string (pending >= 0 ? unpackTrackEqBand (unpackLearnTargetIndex (pending)) : -1)
                                   + ", expected " + std::to_string (row.band));
            session.midiLearnPending.store (-1);
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar eqMidiLearnBand { Scenario {
    "gui.eq_midi_learn_band", { "gui", "eq", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runEqMidiLearnBand (host, ctx); }
} };

// The bus highpass knob: its floor is OFF, typed values read back the way the
// knob prints them ("1.2k"), the sweep stops at 3 kHz, and turning it up off
// the floor arms the bus EQ, as the band knobs do. A highpass or band gain set
// behind the strip's back, as the EQ editor and a MIDI binding set them, shows
// on its knob, and Reset EQ turns the highpass OFF from there.
std::optional<ScenarioResult> runBusHpfKnob (GuiHost& host, ScenarioContext& ctx)
{
    auto& bus = ctx.session().bus (0).strip;
    ctx.keep (bus.eqEnabled);
    ctx.keep (bus.hpfEnabled);
    ctx.keep (bus.hpfFreq);
    ctx.keep (bus.eqLfGainDb);
    ctx.keep (bus.eqMidGainDb);
    ctx.keep (bus.eqHfGainDb);
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Mixing);

    const std::string title = "Bus 1 high-pass filter frequency";
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx, &bus, title]
    {
        ctx.expect (host.setAccessibleValue (title, "OFF"), "the bus highpass knob has no accessible value");
        bus.eqEnabled.store (false, std::memory_order_release);
    } });
    struct Value { const char* input; const char* shown; float hz; bool on; };
    const Value values[] {
        { "80", "80", 80.0f, true },
        { "1.2k", "1.2k", 1200.0f, true },
        { "5k", "3k", 3000.0f, true },
        { "OFF", "OFF", BusParams::kHpfOffHz, false },
    };
    for (const auto value : values)
    {
        steps->push_back ({ 100, [&host, &ctx, title, value]
        { ctx.expect (host.setAccessibleValue (title, value.input), "the bus highpass knob refused a value"); } });
        steps->push_back ({ 100, [&host, &ctx, &bus, title, value]
        {
            std::string shown, help;
            const bool found = host.accessibleControl (title, shown, help);
            ctx.expect (found && shown == value.shown,
                        std::string ("typing ") + value.input + " showed " + shown + ", not " + value.shown);
            ctx.expect (! help.empty(), "the bus highpass knob has no accessible help");
            ctx.expect (std::abs (bus.hpfFreq.load() - value.hz) < 0.5f && bus.hpfEnabled.load() == value.on,
                        std::string ("typing ") + value.input + " left the bus highpass at "
                            + std::to_string (bus.hpfFreq.load()) + " Hz, "
                            + (bus.hpfEnabled.load() ? "on" : "off"));
            ctx.expect (bus.eqEnabled.load(), "turning the bus highpass up did not arm the bus EQ");
        } });
    }

    // Second pass with the timeline open: the strips compact and the EQ header
    // becomes the pill, which carries the same menu.
    const bool expanded = host.timelineViewMatches (true);
    ctx.cleanup ([&host, expanded] { host.setTimelineShown (expanded); });
    const std::string lfTitle = "Bus 1 EQ low shelf gain";
    for (const bool compact : { false, true })
    {
        const std::string layout = compact ? " with the timeline open" : "";
        steps->push_back ({ 100, [&host, &bus, compact]
        {
            if (compact) host.setTimelineShown (true);
            bus.hpfFreq.store (150.0f);
            bus.hpfEnabled.store (true);
            bus.eqLfGainDb.store (4.5f);
        } });
        steps->push_back ({ 100, [&host, &ctx, title, lfTitle, layout]
        {
            std::string hpf, lf, help;
            const bool hpfFound = host.accessibleControl (title, hpf, help);
            const bool lfFound = host.accessibleControl (lfTitle, lf, help);
            ctx.expect (hpfFound && hpf == "150",
                        "a 150 Hz highpass set in the session shows " + hpf + " on the strip knob" + layout);
            ctx.expect (lfFound && lf == "4.5", "a +4.5 dB LF set in the session shows " + lf + " on the strip knob" + layout);
            ctx.expect (host.clickStripControl (GuiHost::StripKind::Bus, 0, "eq", 1, true),
                        "the bus EQ header did not take a right-click" + layout);
        } });
        steps->push_back ({ 100, [&host, &ctx, layout]
        { ctx.expect (host.clickContextMenuItem ("Reset EQ"), "the bus EQ menu has no Reset EQ" + layout); } });
        steps->push_back ({ 100, [&host, &ctx, &bus, title, lfTitle, layout]
        {
            ctx.expect (! bus.hpfEnabled.load() && std::abs (bus.hpfFreq.load() - BusParams::kHpfOffHz) < 0.5f,
                        "Reset EQ" + layout + " left the bus highpass at " + std::to_string (bus.hpfFreq.load()) + " Hz, "
                            + (bus.hpfEnabled.load() ? "on" : "off"));
            ctx.expect (std::abs (bus.eqLfGainDb.load()) < 1.0e-6f, "Reset EQ" + layout + " left the LF gain up");
            ctx.expect (bus.eqEnabled.load(), "Reset EQ" + layout + " bypassed the bus EQ");
            std::string hpf, lf, help;
            const bool hpfFound = host.accessibleControl (title, hpf, help);
            const bool lfFound = host.accessibleControl (lfTitle, lf, help);
            ctx.expect (hpfFound && hpf == "OFF", "after Reset EQ" + layout + " the strip's highpass knob shows " + hpf);
            ctx.expect (lfFound && lf == "0.0", "after Reset EQ" + layout + " the strip's LF knob shows " + lf);
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar busHpfKnob { Scenario {
    "gui.bus_hpf_knob", { "gui", "bus" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runBusHpfKnob (host, ctx); }
} };

// The aux lane's return strip: double-click the title to rename, the mute
// button, and a return fader running from off to +12 dB that sets the return
// level. The drawn meter is not checked here.
std::optional<ScenarioResult> runAuxReturnStrip (GuiHost& host, ScenarioContext& ctx)
{
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no modal");
    auto& lane = ctx.session().auxLane (0);
    ctx.keep (lane.params.mute);
    ctx.keep (lane.params.returnLevelDb);
    ctx.cleanup ([&lane, name = lane.name] { lane.name = name; });
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Aux);
    lane.params.mute.store (false);
    lane.params.returnLevelDb.store (-60.0f);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    {
        const auto range = host.auxReturnRange (0);
        ctx.expect (range.size() == 2 && range[0] <= -90.0 && std::abs (range[1] - 12.0) < 1.0e-6,
                    "the return fader does not run from off to +12 dB");
        ctx.expect (host.clickStripControl (GuiHost::StripKind::Aux, 0, "name", 2, false),
                    "the lane title did not take a double-click");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    {
        ctx.expect (host.pressPeerKey ("command + A"), "the title editor did not accept Select All");
        for (const char character : std::string ("verb"))
            ctx.expect (host.pressPeerKey (std::string (1, character), character), "the title editor rejected a character");
        ctx.expect (host.pressPeerKey ("return"), "the title editor did not accept Return");
    } });
    steps->push_back ({ 200, [&host, &ctx, &lane]
    {
        ctx.expect (lane.name == "verb", "double-clicking the title did not rename the lane");
        ctx.expect (host.clickStripControl (GuiHost::StripKind::Aux, 0, "mute", 1, false), "the mute button did not take a click");
    } });
    steps->push_back ({ 200, [&host, &ctx, &lane]
    {
        ctx.expect (lane.params.mute.load(), "the mute button did not mute the lane");
        ctx.expect (host.clickStripControl (GuiHost::StripKind::Aux, 0, "mute", 1, false), "the mute button did not take a click");
    } });
    steps->push_back ({ 200, [&host, &ctx, &lane]
    {
        ctx.expect (! lane.params.mute.load(), "a second click did not unmute the lane");
        ctx.expect (host.clickStripControl (GuiHost::StripKind::Aux, 0, "fader", 1, false),
                    "the return fader did not take a click");
    } });
    // Midway up the track, which the fader's skew puts near -12 dB. Held back
    // far enough that the double-click below does not count this click.
    steps->push_back ({ 700, [&host, &ctx, &lane]
    {
        const float level = lane.params.returnLevelDb.load();
        ctx.expect (level > -40.0f && level < 0.0f, "a click on the return fader did not set the return level");
        ctx.expect (host.clickStripControl (GuiHost::StripKind::Aux, 0, "fader", 2, false),
                    "the return fader did not take a double-click");
    } });
    steps->push_back ({ 200, [&ctx, &lane]
    {
        ctx.expect (std::abs (lane.params.returnLevelDb.load()) < 1.0e-3f,
                    "a double-click did not return the fader to 0 dB");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar auxReturnStrip { Scenario {
    "gui.aux_return_strip", { "gui", "aux" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAuxReturnStrip (host, ctx); }
} };

// The MIDI Bindings panel lists each binding with a Remove button, exports and
// imports the set, asks before Clear all, and reports an export or import it
// cannot complete.
std::optional<ScenarioResult> runMidiBindingsPanel (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("built without native settings UI");
   #else
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no modal");
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    const bool hardwareInputs = engine.setHardwareMidiInputsEnabledForScenario (false);
    ctx.cleanup ([&engine, hardwareInputs] { engine.setHardwareMidiInputsEnabledForScenario (hardwareInputs); });
    const auto publish = [&session] (std::vector<MidiBinding> binds)
    {
        session.midiBindings.mutate ([&binds] (std::vector<MidiBinding>& current) { current = binds; });
    };
    ctx.cleanup ([&host, publish, original = session.midiBindings.current()]
    {
        drainModals (host);
        host.closeAudioSettings();
        publish (original);
    });
    MidiBinding fader;
    fader.channel = 1; fader.dataNumber = 7; fader.target = MidiBindingTarget::TrackFader; fader.targetIndex = 0;
    MidiBinding pan;
    pan.channel = 2; pan.dataNumber = 10; pan.target = MidiBindingTarget::TrackPan; pan.targetIndex = 1;
    publish ({ fader, pan });
    const auto row = [&engine] (const MidiBinding& b)
    {
        return describeBindingTarget (b, &engine).toStdString() + " | " + describeBindingSource (b).toStdString();
    };
    const auto exported = ctx.tempDir() / "bindings.json";
    // Under a file, so it cannot be written. A folder's name would only open
    // that folder in the browser.
    const auto taken = ctx.tempDir() / "occupied" / "taken.json";
    const auto broken = ctx.tempDir() / "broken.json";
    std::ofstream (taken.parent_path()) << "occupied";
    std::ofstream (broken) << "not a bindings preset";
    const auto browse = [&host, &ctx] (const std::filesystem::path& path, const std::string& accept)
    {
        expectBrowserInHome (host, ctx, "the " + accept + " browser");
        if (! ctx.expect (host.focusFileName(), "the file browser has no name field")) return;
        host.pressPeerKey ("command + A");
        for (const char character : path.string())
            host.pressPeerKey (character == ' ' ? "Space" : std::string (1, character), character);
        ctx.expect (host.clickModalButton (accept), "the file browser has no " + accept + " button");
    };
    const auto button = [&host, &ctx] (const std::string& label)
    { ctx.expect (host.clickModalButton (label), "no " + label + " button on the top modal"); };
    const auto bound = [&session] { return session.midiBindings.current(); };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.openAudioSettings(), "native audio settings did not open"); } });
    pushSettingsTab (*steps, host, "midi-bindings");
    steps->push_back ({ 400, [&host, &ctx]
    { ctx.expect (host.clickAudioSettingsControl ("midi-bindings"), "MIDI Bindings button was not drawn"); } });
    steps->push_back ({ 300, [&host, &ctx, row, fader, pan]
    {
        ctx.expect (host.midiBindingRows() == std::vector<std::string> { row (fader), row (pan) },
                    "the panel does not list each binding's target and source");
        ctx.expect (host.clickMidiBindingRemove (0), "the first row has no Remove button");
    } });
    steps->push_back ({ 200, [&host, &ctx, row, pan, bound, button]
    {
        ctx.expect (bound().size() == 1 && bound()[0].target == MidiBindingTarget::TrackPan
                    && host.midiBindingRows() == std::vector<std::string> { row (pan) },
                    "Remove did not remove only its own binding");
        button ("Export...");
    } });
    steps->push_back ({ 300, [browse, exported] { browse (exported, "Save"); } });
    steps->push_back ({ 300, [&ctx, exported, bound, button]
    {
        std::ifstream input (exported);
        const std::string json ((std::istreambuf_iterator<char> (input)), std::istreambuf_iterator<char>());
        const auto parsed = deserializeBindingsPreset (json);
        ctx.expect (parsed.has_value() && parsed->size() == 1 && (*parsed)[0].target == MidiBindingTarget::TrackPan
                    && (*parsed)[0].dataNumber == 10 && bound().size() == 1,
                    "Export did not write the binding set");
        button ("Clear all");
    } });
    steps->push_back ({ 200, [&host, &ctx, button]
    {
        const auto text = host.confirmationText();
        ctx.expect (text == std::vector<std::string> { "Clear all MIDI bindings",
                                                       "Remove every MIDI binding? This cannot be undone." },
                    "Clear all did not ask first");
        button ("Cancel");
    } });
    steps->push_back ({ 200, [&ctx, bound, button]
    {
        ctx.expect (bound().size() == 1, "cancelling Clear all removed bindings");
        button ("Clear all");
    } });
    steps->push_back ({ 200, [button] { button ("Clear all"); } });
    steps->push_back ({ 200, [&host, &ctx, bound, button]
    {
        ctx.expect (bound().empty() && host.midiBindingRows().empty(), "confirming Clear all left bindings");
        button ("Import...");
    } });
    steps->push_back ({ 300, [browse, exported] { browse (exported, "Open"); } });
    steps->push_back ({ 300, [&host, &ctx, row, pan, bound, button]
    {
        ctx.expect (bound().size() == 1 && bound()[0].target == MidiBindingTarget::TrackPan
                    && host.midiBindingRows() == std::vector<std::string> { row (pan) },
                    "Import did not load the exported set");
        button ("Import...");
    } });
    steps->push_back ({ 300, [browse, broken] { browse (broken, "Open"); } });
    steps->push_back ({ 300, [&host, &ctx, broken, bound, button]
    {
        ctx.expect (host.modalText() == "Import failed\nCould not read bindings from " + broken.string()
                                        + ". File is missing, malformed, or written by a newer version of Dusk Studio.",
                    "an unreadable import was not reported");
        ctx.expect (bound().size() == 1, "a failed import changed the bindings");
        button ("OK");
    } });
    steps->push_back ({ 200, [button] { button ("Export..."); } });
    steps->push_back ({ 300, [browse, taken] { browse (taken, "Save"); } });
    steps->push_back ({ 300, [&host, &ctx, taken, button]
    {
        ctx.expect (host.modalText() == "Export failed\nCould not write to " + taken.string(),
                    "an export that could not be written was not reported");
        button ("OK");
    } });
    // Export onto the preset already written, named without its extension: the
    // file it would really write is asked about, Cancel keeps it and Replace
    // writes over it.
    const auto kept = std::make_shared<std::string>();
    steps->push_back ({ 200, [&host, &ctx, exported, kept, button]
    {
        ctx.expect (host.midiBindingsOpen(), "a failed export closed the panel");
        *kept = fileBytes (exported);
        ctx.expect (host.clickMidiBindingRemove (0), "the remaining row has no Remove button");
        button ("Export...");
    } });
    const auto replaceAsked = [&host, &ctx, button] (const std::string& answer)
    {
        ctx.expect (host.confirmationText() == std::vector<std::string> {
                        "Replace file?", "This file already exists and will be replaced:\n\nbindings.json\n\nContinue?" },
                    "Export onto an existing preset did not ask first; top modal '" + host.modalText() + "'");
        button (answer);
    };
    steps->push_back ({ 300, [browse, exported] { browse (exported.parent_path() / "bindings", "Save"); } });
    steps->push_back ({ 300, [replaceAsked] { replaceAsked ("Cancel"); } });
    steps->push_back ({ 300, [&host, &ctx, exported, kept, button]
    {
        ctx.expect (host.midiBindingsOpen() && fileBytes (exported) == *kept,
                    "Cancel in the replace prompt changed the exported preset");
        button ("Export...");
    } });
    steps->push_back ({ 300, [browse, exported] { browse (exported.parent_path() / "bindings", "Save"); } });
    steps->push_back ({ 300, [replaceAsked] { replaceAsked ("Replace"); } });
    steps->push_back ({ 300, [&ctx, exported]
    {
        const auto parsed = deserializeBindingsPreset (fileBytes (exported));
        ctx.expect (parsed.has_value() && parsed->empty(), "Replace did not export over the preset");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar midiBindingsPanel { Scenario {
    "gui.midi_bindings_panel", { "gui", "settings", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMidiBindingsPanel (host, ctx); }
} };

std::optional<ScenarioResult> runAutomationMenu (GuiHost& host, ScenarioContext& ctx)
{
    auto& track = ctx.session().track (0);
    ctx.keep (track.automationMode);
    ctx.cleanup ([&host, stage = ctx.engine().getStage()]
    {
        host.closeTopModal();
        switch (stage)
        {
            case AudioEngine::Stage::Recording: host.switchToStage (GuiHost::Stage::Recording); break;
            case AudioEngine::Stage::Mixing: host.switchToStage (GuiHost::Stage::Mixing); break;
            case AudioEngine::Stage::Aux: host.switchToStage (GuiHost::Stage::Aux); break;
            case AudioEngine::Stage::Mastering: host.switchToStage (GuiHost::Stage::Mastering); break;
        }
    });
    host.switchToStage (GuiHost::Stage::Mixing);
    auto* strip = host.strip (0);
    if (! ctx.expect (strip != nullptr, "the channel strip is unavailable")) return ctx.verdict();
    auto steps = std::make_shared<std::vector<Step>>();
    for (const int mode : { 1, 2, 3, 0 })
    {
        steps->push_back ({ 500, [strip] { strip->clickAutomationMode(); } });
        steps->push_back ({ 200, [&host, &ctx, mode]
        {
            ctx.expect (! host.modalStackEmpty(), "clicking the automation label did not open its menu");
            ctx.expect (host.clickModalAt (0.5f, (static_cast<float> (mode) + 0.5f) / 4.0f),
                        "the automation menu row was unavailable");
        } });
        steps->push_back ({ 200, [&host, &ctx, &track, mode]
        {
            static constexpr const char* labels[] { "OFF", "READ", "WRITE", "TOUCH" };
            ctx.expect (host.modalStackEmpty(), "choosing an automation mode left its menu open");
            ctx.expect (track.automationMode.load() == mode, "the menu selected the wrong automation mode");
            std::string label;
            bool faderEnabled = false;
            ctx.expect (host.automationView (GuiHost::StripKind::Channel, 0, label, faderEnabled)
                        && label == labels[mode] && faderEnabled == (mode != 1),
                        "the mode label or fader input state disagrees with the menu selection");
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar automationMenu { Scenario {
    "gui.automation_menu", { "gui", "automation" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAutomationMenu (host, ctx); }
} };

// A send knob runs OFF, then -59.9 to +6 dB, and reads OFF at the bottom of
// its travel rather than an audible level; a send is post-fader until its
// knob's right-click menu makes it pre, which the level text under the knob
// then says; and the compact strip's AUX light mutes all four sends and gives
// each its level back.
std::optional<ScenarioResult> runAuxSendKnobs (GuiHost& host, ScenarioContext& ctx)
{
    static constexpr auto kSends = ChannelStripParams::kNumAuxSends;
    auto& strip = ctx.session().track (kStripIndex).strip;
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no modal");
    // The right-click selects the track and focuses its strip.
    ctx.cleanup (host.preserveKeyboardFocus());
    for (int send = 0; send < kSends; ++send)
    {
        ctx.keep (strip.auxSendDb[(std::size_t) send]);
        ctx.keep (strip.auxSendPreFader[(std::size_t) send]);
    }
    ctx.keep (strip.auxSendsBypassed);
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Mixing);
    const bool compact = host.setStripCompact (kStripIndex, false);
    ctx.cleanup ([&host, compact]
    {
        drainModals (host);
        host.setStripCompact (kStripIndex, compact);
    });
    strip.auxSendsBypassed.store (false);
    for (int send = 0; send < kSends; ++send) strip.auxSendPreFader[(std::size_t) send].store (false);
    const auto title = [] (int send) { return "Track 1 aux " + std::to_string (send + 1) + " send"; };
    const std::string minus = "\xe2\x88\x92";

    auto steps = std::make_shared<std::vector<Step>>();
    struct Turn { const char* input; float stored; const char* spoken; std::string shown; };
    // Starts off the bottom so the move down to it changes the knob.
    const std::vector<Turn> turns {
        { "-12", -12.0f, "-12.0 dB", "-12" },
        { "-80", ChannelStripParams::kAuxSendOffDb, "OFF", minus },
        { "-59.9", -59.9f, "-59.9 dB", "-60" },
        { "OFF", ChannelStripParams::kAuxSendOffDb, "OFF", minus },
        { "10", ChannelStripParams::kAuxSendMaxDb, "6.0 dB", "6.0" },
        { "-12", -12.0f, "-12.0 dB", "-12" }
    };
    for (const auto& turn : turns)
    {
        steps->push_back ({ 150, [&host, &ctx, title, turn]
        {
            ctx.expect (host.setAccessibleValue (title (0), turn.input),
                        "the aux 1 send knob took no accessible value");
        } });
        steps->push_back ({ 150, [&host, &ctx, &strip, title, turn]
        {
            const auto label = std::string ("aux 1 set to ") + turn.input;
            std::string spoken, help;
            ctx.expect (std::abs (strip.auxSendDb[0].load() - turn.stored) < 0.01f,
                        label + " stored " + std::to_string (strip.auxSendDb[0].load()));
            ctx.expect (host.accessibleControl (title (0), spoken, help) && spoken == turn.spoken,
                        label + ": the knob reads '" + spoken + "', not '" + turn.spoken + "'");
            ctx.expect (host.stripSendLabel (kStripIndex, 0) == turn.shown,
                        label + ": the strip shows '" + host.stripSendLabel (kStripIndex, 0) + "', not '" + turn.shown + "'");
        } });
    }
    for (const bool pre : { true, false })
    {
        const std::string item = pre ? "Switch to PRE-fader" : "Switch to POST-fader";
        steps->push_back ({ 200, [&host, &ctx, title, item]
        { ctx.expect (host.clickTitledControl (title (0), true), item + ": the aux 1 send knob is not on screen"); } });
        steps->push_back ({ 300, [&host, &ctx, item]
        {
            const auto items = host.contextMenuItems();
            ctx.expect (std::find (items.begin(), items.end(), "Track 1 AUX 1 send") != items.end(),
                        item + ": the right-click menu reads '" + joinedLines (items) + "'");
            ctx.expect (host.clickContextMenuItem (item), "the right-click menu has no " + item);
        } });
        steps->push_back ({ 300, [&host, &ctx, &strip, pre, item]
        {
            ctx.expect (host.modalStackEmpty(), item + " left '" + host.modalText() + "' up");
            ctx.expect (strip.auxSendPreFader[0].load() == pre, item + " did not flip aux 1");
            for (int send = 1; send < kSends; ++send)
                ctx.expect (! strip.auxSendPreFader[(std::size_t) send].load(), item + " flipped another send");
            ctx.expect (std::abs (strip.auxSendDb[0].load() + 12.0f) < 0.01f, item + " changed the level");
            const std::string shown = pre ? "-12 PRE" : "-12";
            ctx.expect (host.stripSendLabel (kStripIndex, 0) == shown,
                        item + ": the strip shows '" + host.stripSendLabel (kStripIndex, 0) + "', not '" + shown + "'");
        } });
    }

    const std::array<const char*, kSends> levels { "-3", "-6", "-9", "-12" };
    steps->push_back ({ 150, [&host, &ctx, title, levels]
    {
        for (int send = 0; send < kSends; ++send)
            ctx.expect (host.setAccessibleValue (title (send), levels[(std::size_t) send]),
                        "the aux " + std::to_string (send + 1) + " send knob took no accessible value");
        host.setStripCompact (kStripIndex, true);
    } });
    const auto levelsKept = [&ctx, &strip] (const std::string& how)
    {
        for (int send = 0; send < kSends; ++send)
            ctx.expect (std::abs (strip.auxSendDb[(std::size_t) send].load() + 3.0f * (float) (send + 1)) < 0.01f,
                        how + " changed the aux " + std::to_string (send + 1) + " send level");
    };
    for (const bool bypassed : { true, false })
    {
        steps->push_back ({ 300, [&host, &ctx]
        {
            ctx.expect (host.stripCompact (kStripIndex), "the strip did not go compact");
            ctx.expect (host.clickStripModule (kStripIndex, 2, false, false), "the compact AUX light is not on screen");
        } });
        steps->push_back ({ 200, [&ctx, &strip, levelsKept, bypassed]
        {
            const std::string how = bypassed ? "the compact AUX light" : "the second click on the compact AUX light";
            ctx.expect (strip.auxSendsBypassed.load() == bypassed,
                        how + (bypassed ? " did not mute the sends" : " did not bring the sends back"));
            levelsKept (how);
        } });
    }
    steps->push_back ({ 100, [&host] { host.setStripCompact (kStripIndex, false); } });
    steps->push_back ({ 300, [&host, &ctx, title]
    {
        const std::array<const char*, kSends> shown { "-3.0", "-6.0", "-9.0", "-12" };
        for (int send = 0; send < kSends; ++send)
        {
            std::string spoken, help;
            const auto name = "aux " + std::to_string (send + 1);
            ctx.expect (host.accessibleControl (title (send), spoken, help)
                            && std::abs (std::strtof (spoken.c_str(), nullptr) + 3.0f * (float) (send + 1)) < 0.01f,
                        name + ": the knob reads '" + spoken + "' after the bypass");
            ctx.expect (host.stripSendLabel (kStripIndex, send) == shown[(std::size_t) send],
                        name + ": the strip shows '" + host.stripSendLabel (kStripIndex, send) + "' after the bypass");
        }
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar auxSendKnobs { Scenario {
    "gui.aux_send_knobs", { "gui", "strip", "aux" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAuxSendKnobs (host, ctx); }
} };

// Settings > Quickstart is offered when the quickstart document sits where the
// packages put it around the executable, and greyed out as "Quickstart  (not
// installed)" when it does not. Choosing it hands the file to the desktop,
// which a run under test must not do, so the case stops at the menu.
std::optional<ScenarioResult> runQuickstartEntry (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no modal");
    ctx.cleanup ([&host] { drainModals (host); });
    const auto exeDir = host.executableDirectory();
    if (exeDir.empty()) return ScenarioResult::skip ("the platform does not say where the executable is");
    const std::vector<fs::path> roots {
        exeDir.parent_path(), exeDir, exeDir.parent_path() / "Resources",
        exeDir.parent_path().parent_path().parent_path()
    };
    const auto document = host.quickstartDocument();

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickMenuBar ("Settings"), "the Settings menu is unavailable"); } });
    steps->push_back ({ 300, [&host, &ctx, roots, document]
    {
        const auto items = host.contextMenuItems();
        const auto has = [&items] (const char* text) { return std::find (items.begin(), items.end(), text) != items.end(); };
        if (! document.empty())
        {
            ctx.expect (has ("Quickstart") && ! has ("Quickstart  (not installed)"),
                        "the Settings menu reads '" + joinedLines (items) + "' with a quickstart installed");
            ctx.expect (host.contextMenuItemEnabled ("Quickstart"), "Quickstart is greyed out with the document installed");
            const auto name = document.filename().string();
            const auto root = document.parent_path().lexically_normal();
            ctx.expect (fs::is_regular_file (document) && (name == "QUICKSTART.md" || name == "QUICKSTART.txt")
                            && std::any_of (roots.begin(), roots.end(),
                                            [&root] (const fs::path& at) { return at.lexically_normal() == root; }),
                        "Quickstart would open '" + document.string() + "', which is not the shipped document");
            ctx.note ("Quickstart would open " + document.string());
        }
        else
        {
            ctx.expect (has ("Quickstart  (not installed)") && ! has ("Quickstart"),
                        "the Settings menu reads '" + joinedLines (items) + "' with no quickstart installed");
            ctx.expect (! host.contextMenuItemEnabled ("Quickstart  (not installed)"),
                        "Quickstart is offered with no document installed");
            for (const auto& at : roots)
                ctx.expect (! fs::is_regular_file (at / "QUICKSTART.md"),
                            "the entry is greyed out though " + (at / "QUICKSTART.md").string() + " is installed");
            ctx.note ("no quickstart beside this build; the entry is greyed out");
        }
        drainModals (host);
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar quickstartEntry { Scenario {
    "gui.settings_quickstart_entry", { "gui", "settings" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runQuickstartEntry (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
