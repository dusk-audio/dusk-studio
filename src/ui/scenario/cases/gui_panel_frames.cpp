#include "../GuiScenarioSupport.h"
#include "../../../session/SessionSerializer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

namespace duskstudio::scenario::gui
{
namespace
{
constexpr int kQuietMs = 1000;
constexpr int kHoldMs = 2000;
constexpr int kPollGapMs = 200;
constexpr int kDrawBoundMs = 10000;
constexpr int kFloorWindowMs = 1500;
constexpr int kFloorMs = 250;
// Dear ImGui's tooltip delay is 400 ms, and a panel that stopped drawing before it ran
// out would leave the tooltip to the safety frame.
constexpr int kDrawsOnForMs = 500;
constexpr float kClockAfterRestS = 0.05f;

using Clock = std::chrono::steady_clock;
using Panels = std::vector<std::string>;

// The panels' frames summed; empty while any of them is closed.
std::optional<std::uint64_t> framesOf (const GuiHost& host, const Panels& panels)
{
    std::uint64_t total = 0;
    for (const auto& tag : panels)
    {
        const auto frames = host.panelFrames (tag);
        if (! frames) return std::nullopt;
        total += *frames;
    }
    return total;
}

struct Quiet
{
    std::uint64_t seen = 0;
    Clock::time_point since {}, lastPoll {};
};

// True once the panels' frame count has held still for kQuietMs of polls no more
// than kPollGapMs apart. The panels draw on the message thread this polls from, so
// time that thread spent stalled shows nothing either way.
std::function<bool()> framesQuiet (GuiHost& host, Panels panels)
{
    auto quiet = std::make_shared<Quiet>();
    return [&host, panels, quiet]
    {
        const auto frames = framesOf (host, panels);
        const auto at = Clock::now();
        const bool stalled = at - quiet->lastPoll > std::chrono::milliseconds (kPollGapMs);
        quiet->lastPoll = at;
        if (! frames || *frames != quiet->seen || stalled)
        {
            quiet->seen = frames.value_or (0);
            quiet->since = at;
            return false;
        }
        return at - quiet->since >= std::chrono::milliseconds (kQuietMs);
    };
}

// With the safety frame off, panels at rest draw nothing at all for kHoldMs.
void pushStaysStill (GuiHost& host, ScenarioContext& ctx, std::vector<Step>& steps,
                     const Panels& panels, const std::string& what)
{
    auto base = std::make_shared<std::uint64_t> (0);
    steps.push_back ({ 0, [] {}, framesQuiet (host, panels),
        what + " never stopped drawing while nothing changed", {}, kDrawBoundMs });
    steps.push_back ({ 0, [&host, panels, base] { *base = framesOf (host, panels).value_or (0); } });
    steps.push_back ({ kHoldMs, [&host, &ctx, panels, base, what]
    {
        const auto frames = framesOf (host, panels);
        if (! ctx.expect (frames.has_value(), what + " closed while it was watched")) return;
        const auto drawn = *frames - *base;
        ctx.expect (drawn == 0, what + " drew " + std::to_string (drawn) + " frames in "
                                    + std::to_string (kHoldMs) + " ms with nothing changing");
    } });
}

enum class Cause { model, input };

struct Woken
{
    std::uint64_t base = 0, seen = 0;
    Clock::time_point at {}, lastFrame {};
};

// Panels at rest draw again once `change` has run, and the first frame after the
// rest moves Dear ImGui's clock by a frame, not by the rest. After input they keep
// drawing for long enough that a hover delay can run out.
void pushDrawsOn (GuiHost& host, ScenarioContext& ctx, std::vector<Step>& steps,
                  const Panels& panels, const std::string& what, std::function<bool()> change,
                  Cause cause = Cause::model)
{
    auto woken = std::make_shared<Woken>();
    steps.push_back ({ 0, [] {}, framesQuiet (host, panels),
        "the panel never came to rest before " + what, {}, kDrawBoundMs });
    steps.push_back ({ 0, [&host, &ctx, panels, woken, what, change]
    {
        woken->base = woken->seen = framesOf (host, panels).value_or (0);
        for (const auto& tag : panels)
            host.panelClockAfterRest (tag);
        woken->at = woken->lastFrame = Clock::now();
        ctx.expect (change(), what + " could not be made");
    } });
    steps.push_back ({ 0, [&host, &ctx, panels, what]
    {
        bool drawn = false;
        for (const auto& tag : panels)
        {
            const auto clock = host.panelClockAfterRest (tag);
            if (! clock) continue;
            drawn = true;
            ctx.expect (*clock <= kClockAfterRestS, what + ": the first frame " + tag + " drew after its rest moved its clock "
                                                       + std::to_string (*clock) + " s");
        }
        ctx.expect (drawn, what + " drew no frame after a rest");
    }, [&host, panels, woken]
    {
        const auto frames = framesOf (host, panels);
        return frames && *frames > woken->base;
    }, what + " drew no frame", {}, kDrawBoundMs });
    if (cause != Cause::input) return;
    steps.push_back ({ 0, [&ctx, woken, what]
    {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (woken->lastFrame - woken->at).count();
        ctx.expect (ms >= kDrawsOnForMs, what + ": the last frame came " + std::to_string (ms) + " ms after the input, under "
                                             + std::to_string (kDrawsOnForMs) + " ms");
    }, [&host, panels, woken]
    {
        const auto now = Clock::now();
        if (const auto frames = framesOf (host, panels); frames && *frames != woken->seen)
        {
            woken->seen = *frames;
            woken->lastFrame = now;
        }
        return now - woken->at >= std::chrono::milliseconds (kDrawsOnForMs + 1000);
    }, what + " was not watched to its end", {}, kDrawBoundMs });
}

struct FloorStart
{
    std::vector<std::uint64_t> frames;
    Clock::time_point at {};
};

// With the safety frame back on, each panel at rest draws at least once and never
// more often than the floor allows.
void pushFloor (GuiHost& host, ScenarioContext& ctx, std::vector<Step>& steps,
                const Panels& panels, const std::string& what)
{
    auto start = std::make_shared<FloorStart>();
    steps.push_back ({ 0, [&host, panels, start]
    {
        host.panelFramesOnDemand (true, true);
        start->frames.clear();
        for (const auto& tag : panels)
            start->frames.push_back (host.panelFrames (tag).value_or (0));
        start->at = Clock::now();
    }, framesQuiet (host, panels), "the panel never came to rest before the safety frame", {}, kDrawBoundMs });
    steps.push_back ({ kFloorWindowMs, [&host, &ctx, panels, start, what]
    {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (Clock::now() - start->at).count();
        const auto most = static_cast<std::uint64_t> (ms / kFloorMs + 1);
        const auto in = " in " + std::to_string (ms) + " ms";
        for (std::size_t i = 0; i < panels.size(); ++i)
        {
            const auto frames = host.panelFrames (panels[i]);
            if (! ctx.expect (frames.has_value(), panels[i] + " closed while " + what + " was watched")) continue;
            const auto drawn = *frames - start->frames[i];
            ctx.expect (drawn >= 1, panels[i] + " drew no safety frame" + in);
            ctx.expect (drawn <= most, panels[i] + " drew " + std::to_string (drawn) + " frames" + in
                                           + " with nothing changing, more than the safety frame's "
                                           + std::to_string (most));
        }
        host.panelFramesOnDemand (false, true);
    } });
}

void pushOpened (GuiHost& host, std::vector<Step>& steps, const Panels& panels,
                 std::function<bool()> ready, const std::string& what)
{
    steps.push_back ({ 0, [] {}, [&host, panels, ready] { return ready() && framesOf (host, panels).has_value(); },
        what + " never opened", {}, kDrawBoundMs });
    steps.push_back ({ 0, [&host] { host.panelFramesOnDemand (true, false); } });
}

struct DrawnPlayhead
{
    double x = 0.0;
    int width = 0;
};

// The middle of the playhead's red line along the row through a capture's middle;
// empty when the capture cannot be read or shows no playhead.
std::optional<DrawnPlayhead> playheadIn (const std::filesystem::path& ppm)
{
    std::ifstream in (ppm, std::ios::binary);
    std::string magic;
    int width = 0, height = 0, maxValue = 0;
    in >> magic >> width >> height >> maxValue;
    if (! in || magic != "P6" || width <= 0 || height <= 0 || maxValue != 255) return std::nullopt;
    in.get();
    std::vector<unsigned char> rgb (static_cast<std::size_t> (width) * static_cast<std::size_t> (height) * 3u);
    if (! in.read (reinterpret_cast<char*> (rgb.data()), static_cast<std::streamsize> (rgb.size()))) return std::nullopt;
    const auto row = static_cast<std::size_t> (height / 2) * static_cast<std::size_t> (width);
    double sum = 0.0;
    int count = 0;
    for (int x = 0; x < width; ++x)
    {
        const auto* pixel = &rgb[(row + static_cast<std::size_t> (x)) * 3u];
        if (pixel[0] > 0xc0 && pixel[1] < 0x70 && pixel[2] < 0x70)
        {
            sum += x;
            ++count;
        }
    }
    if (count == 0) return std::nullopt;
    return DrawnPlayhead { sum / count, width };
}

// Where the mini timeline puts a sample: the song runs to the last thing on the
// timeline, a minute at least, and the playhead when that is further.
double miniTimelineFraction (ScenarioContext& ctx, std::int64_t sample)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto end = static_cast<std::int64_t> (engine.getTimelineSampleRate() * 60.0);
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        for (const auto& r : session.track (t).regions) end = std::max (end, r.timelineStart + r.lengthInSamples);
        for (const auto& m : session.track (t).midiRegions.current()) end = std::max (end, m.timelineStart + m.lengthInSamples);
    }
    for (const auto& marker : session.getMarkers()) end = std::max (end, marker.timelineSamples);
    end = std::max (end, sample);
    return static_cast<double> (sample) / static_cast<double> (end);
}

// With frames on demand, what is on screen once the panel comes to rest is the
// last frame it drew: the playhead must be where the transport is, not where it was.
// With DUSKSTUDIO_SCENARIO_CAPTURE_DIR set the capture is also kept there as
// `capture`, for a reviewer to look at.
void pushPlayheadShown (GuiHost& host, ScenarioContext& ctx, std::vector<Step>& steps, const std::string& what,
                        const std::string& capture)
{
    steps.push_back ({ 0, [] {}, framesQuiet (host, { "mini-timeline" }),
        "the mini timeline never came to rest " + what, {}, kDrawBoundMs });
    steps.push_back ({ 0, [&host, &ctx, what, capture]
    {
        const auto ppm = ctx.tempDir() / capture;
        if (const auto* destination = std::getenv ("DUSKSTUDIO_SCENARIO_CAPTURE_DIR");
            destination != nullptr && *destination != '\0')
        {
            const auto directory = std::filesystem::u8path (destination);
            std::error_code error;
            std::filesystem::create_directories (directory, error);
            host.capturePanel ("mini-timeline", directory / capture);
        }
        if (! host.capturePanel ("mini-timeline", ppm))
        {
            ctx.note ("this platform cannot read the mini timeline back, so the playhead " + what + " is not checked");
            return;
        }
        const auto drawn = playheadIn (ppm);
        if (! ctx.expect (drawn.has_value(), "the mini timeline read back " + what + " shows no playhead")) return;
        const double scale = host.uiScale();
        const double expected = 6.0 * scale + std::round (miniTimelineFraction (ctx, ctx.engine().getTransport().getPlayhead())
                                                          * std::max (1.0, drawn->width - 12.0 * scale));
        ctx.expect (std::abs (drawn->x - expected) <= 3.0 + scale,
                    "the mini timeline read back " + what + " has its playhead at x " + std::to_string (drawn->x)
                        + ", not at " + std::to_string (expected));
    } });
}

// The session comes back from a saved copy, in a folder of the case's own so the
// autosave cannot make the reload a recovery prompt.
std::optional<ScenarioResult> keepSession (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, originalDir, restore]
    {
        drainModals (host);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
    });
    const auto folder = ctx.tempDir() / "session";
    std::error_code error;
    if (! std::filesystem::create_directories (folder, error) && error)
        return ScenarioResult::fail ("could not create the session folder");
    applySessionDirectory (session, folder);
    return std::nullopt;
}

std::optional<ScenarioResult> runMiniTimelineOnDemand (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    if (! transport.isStopped()) return ScenarioResult::skip ("requires stopped transport");
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no open modal");
    if (auto failed = keepSession (host, ctx)) return failed;
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Recording);
    const bool expanded = host.setTimelineShown (false);
    const auto size = host.mainWindowSize();
    ctx.cleanup ([&host, &transport, expanded, size, playhead = transport.getPlayhead()]
    {
        host.panelFramesOnDemand (false, true);
        if (size.size() >= 2) host.resizeMainWindow (size[0], size[1]);
        transport.setPlayhead (playhead);
        host.setTimelineShown (expanded);
    });

    const double rate = engine.getTimelineSampleRate();
    const auto seconds = [rate] (double s) { return static_cast<std::int64_t> (rate * s); };
    const auto ramp = ctx.tempDir() / "session" / "ramp.wav";
    if (! ctx.expect (writeRampFixture (ramp, rate, seconds (1.0)), "could not write the region's audio"))
        return ctx.verdict();
    session.getMarkers().clear();

    const Panels mini { "mini-timeline" };
    auto steps = std::make_shared<std::vector<Step>>();
    pushOpened (host, *steps, mini, [] { return true; }, "the mini timeline");
    pushStaysStill (host, ctx, *steps, mini, "the idle mini timeline");
    pushPlayheadShown (host, ctx, *steps, "at rest", "mini-timeline-rest.ppm");
    pushDrawsOn (host, ctx, *steps, mini, "moving the playhead", [&transport, seconds]
    {
        transport.setPlayhead (seconds (20.0));
        return true;
    });
    pushPlayheadShown (host, ctx, *steps, "after the playhead moved", "mini-timeline-playhead.ppm");
    pushDrawsOn (host, ctx, *steps, mini, "adding a marker", [&session, seconds]
    {
        session.addMarker (seconds (10.0));
        return true;
    });
    pushDrawsOn (host, ctx, *steps, mini, "adding a region past the end", [&session, ramp, seconds]
    {
        AudioRegion region;
        using File = std::decay_t<decltype (region.file)>;
        region.file = File (ramp.u8string().c_str());
        region.timelineStart = seconds (120.0);
        region.lengthInSamples = seconds (1.0);
        session.track (0).regions.push_back (region);
        return true;
    });
    pushPlayheadShown (host, ctx, *steps, "after a region lengthened the song", "mini-timeline-region.ppm");
    pushDrawsOn (host, ctx, *steps, mini, "moving the pointer over it",
                 [&host] { return host.panelInput ("mini-timeline", "hover"); }, Cause::input);
    pushDrawsOn (host, ctx, *steps, mini, "a click on it",
                 [&host, seconds] { return host.clickMiniSample (seconds (30.0)); }, Cause::input);
    // The framework takes the modifiers from every pointer event, and the pointer that
    // leaves the panel takes their release with it.
    pushDrawsOn (host, ctx, *steps, mini, "moving the pointer over it with Shift held",
                 [&host] { return host.panelInput ("mini-timeline", "shift + hover"); }, Cause::input);
    pushStaysStill (host, ctx, *steps, mini, "the mini timeline after a pointer that held Shift");
    pushDrawsOn (host, ctx, *steps, mini, "a key at it",
                 [&host] { return host.pressMiniKey ("left shift"); }, Cause::input);
    pushDrawsOn (host, ctx, *steps, mini, "resizing the window", [&host, size]
    {
        if (size.size() < 4) return false;
        const int narrower = size[0] - 60;
        return host.resizeMainWindow (narrower >= size[2] ? narrower : size[0] + 60, size[1]);
    }, Cause::input);
    pushStaysStill (host, ctx, *steps, mini, "the mini timeline after the changes");
    pushFloor (host, ctx, *steps, mini, "the idle mini timeline");
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

std::optional<ScenarioResult> runMasteringPanelsOnDemand (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& mastering = ctx.session().mastering();
    if (! engine.getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no open modal");
    if (host.masteringPanelsOpen() < 0) return ScenarioResult::skip ("this build has no native mastering panels");
    keepStage (host, ctx);
    ctx.cleanup ([&host] { host.panelFramesOnDemand (false, true); });
    ctx.keep (mastering.eqBandGainDb[2]);
    ctx.keep (mastering.limiterCeilingDb);
    ctx.keep (mastering.phase.bands);
    host.switchToStage (GuiHost::Stage::Mastering);

    const Panels all { "mastering-eq", "mastering-limiter", "mastering-phase" };
    auto steps = std::make_shared<std::vector<Step>>();
    pushOpened (host, *steps, all, [&host] { return host.masteringPanelsOpen() == 3; }, "the mastering panels");
    pushStaysStill (host, ctx, *steps, all, "the idle mastering panels");
    pushDrawsOn (host, ctx, *steps, { "mastering-eq" }, "an EQ band's gain changing", [&mastering]
    {
        mastering.eqBandGainDb[2].store (mastering.eqBandGainDb[2].load() + 3.0f);
        return true;
    });
    pushDrawsOn (host, ctx, *steps, { "mastering-limiter" }, "the limiter ceiling changing", [&mastering]
    {
        mastering.limiterCeilingDb.store (mastering.limiterCeilingDb.load() - 0.5f);
        return true;
    });
    pushDrawsOn (host, ctx, *steps, { "mastering-phase" }, "the phase band count changing", [&mastering]
    {
        mastering.phase.bands.store (mastering.phase.bands.load() == 16 ? 24 : 16);
        return true;
    });
    for (const auto& tag : all)
        pushDrawsOn (host, ctx, *steps, { tag }, "moving the pointer over " + tag,
                     [&host, tag] { return host.panelInput (tag, "hover"); }, Cause::input);
    pushStaysStill (host, ctx, *steps, all, "the mastering panels after the changes");
    pushFloor (host, ctx, *steps, all, "the idle mastering panels");
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

std::optional<ScenarioResult> runVirtualKeyboardOnDemand (GuiHost& host, ScenarioContext& ctx)
{
    if (! ctx.engine().getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    if (! host.modalStackEmpty() || host.virtualKeyboardOpen())
        return ScenarioResult::skip ("requires no open modal");
    ctx.cleanup ([&host]
    {
        host.panelFramesOnDemand (false, true);
        host.closeVirtualKeyboard();
    });
    if (! host.clickVirtualKeyboardButton()) return ScenarioResult::fail ("the keyboard button could not be pressed");

    const Panels keyboard { "virtual-keyboard" };
    auto steps = std::make_shared<std::vector<Step>>();
    pushOpened (host, *steps, keyboard, [&host] { return host.virtualKeyboardOpen(); }, "the virtual keyboard");
    pushStaysStill (host, ctx, *steps, keyboard, "the idle virtual keyboard");
    pushDrawsOn (host, ctx, *steps, keyboard, "moving the pointer over it",
                 [&host] { return host.panelInput ("virtual-keyboard", "hover"); }, Cause::input);
    pushDrawsOn (host, ctx, *steps, keyboard, "a key at it",
                 [&host] { return host.panelInput ("virtual-keyboard", "left shift"); }, Cause::input);
    pushStaysStill (host, ctx, *steps, keyboard, "the virtual keyboard after the input");
    pushFloor (host, ctx, *steps, keyboard, "the idle virtual keyboard");
    // The window takes two pump ticks to go, and the next case must not find it.
    steps->push_back ({ 0, [&host] { host.closeVirtualKeyboard(); } });
    steps->push_back ({ 0, [] {}, [&host] { return ! host.virtualKeyboardOpen(); },
        "the virtual keyboard did not close", {}, kDrawBoundMs });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

std::optional<ScenarioResult> runPianoRollOnDemand (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    if (! transport.isStopped()) return ScenarioResult::skip ("requires stopped transport");
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no open modal");
    if (auto failed = keepSession (host, ctx)) return failed;
    ctx.cleanup ([&host, &transport, playhead = transport.getPlayhead()]
    {
        host.panelFramesOnDemand (false, true);
        host.closePiano();
        transport.setPlayhead (playhead);
    });
    MidiRegion region;
    region.lengthInTicks = 7680;
    region.lengthInSamples = session.ticksToSamples (region.lengthInTicks, engine.getCurrentSampleRate());
    region.notes = { { 1, 60, 100, 1200, 480 }, { 1, 64, 90, 2400, 480 } };
    session.track (0).mode.store ((int) Track::Mode::Midi);
    session.track (0).midiRegions.publish (
        std::make_unique<std::vector<MidiRegion>> (std::initializer_list<MidiRegion> { region }));
    transport.setPlayhead (0);
    if (! host.openPiano (0, 0)) return ScenarioResult::fail ("piano roll did not open");

    const Panels roll { "piano-roll" };
    const auto middle = region.lengthInSamples / 2;
    auto steps = std::make_shared<std::vector<Step>>();
    pushOpened (host, *steps, roll, [&host] { return host.pianoRollOpen(); }, "the piano roll");
    pushStaysStill (host, ctx, *steps, roll, "the idle piano roll");
    pushDrawsOn (host, ctx, *steps, roll, "moving the playhead into the region", [&transport, middle]
    {
        transport.setPlayhead (middle);
        return true;
    });
    pushDrawsOn (host, ctx, *steps, roll, "moving the pointer over it",
                 [&host] { return host.panelInput ("piano-roll", "hover"); }, Cause::input);
    // The shell hands the piano roll every key its child does not hold, which on
    // Windows is all of them, and select all changes nothing the view's stamp reads.
    pushDrawsOn (host, ctx, *steps, roll, "select all passed on by the shell",
                 [&host] { return host.pressPeerKey (commandChord ('a'), controlCharacter ('a')); }, Cause::input);
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.pianoSelection().size() == 2, "select all did not select the region's two notes"); } });
    pushStaysStill (host, ctx, *steps, roll, "the piano roll after the changes");
    pushFloor (host, ctx, *steps, roll, "the idle piano roll");
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar miniTimeline { Scenario {
    "gui.idle_mini_timeline_draws_on_demand", { "gui", "panel" }, Needs::Engine | Needs::Gui,
    {}, {}, 120000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMiniTimelineOnDemand (host, ctx); }
} };

const ScenarioRegistrar masteringPanels { Scenario {
    "gui.idle_mastering_panels_draw_on_demand", { "gui", "panel", "mastering" }, Needs::Engine | Needs::Gui,
    {}, {}, 120000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringPanelsOnDemand (host, ctx); }
} };

const ScenarioRegistrar virtualKeyboard { Scenario {
    "gui.idle_virtual_keyboard_draws_on_demand", { "gui", "panel" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runVirtualKeyboardOnDemand (host, ctx); }
} };

const ScenarioRegistrar pianoRoll { Scenario {
    "gui.idle_piano_roll_draws_on_demand", { "gui", "panel", "piano" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoRollOnDemand (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
