#include "../GuiScenarioSupport.h"
#include "../../../dsp/AuxLaneStrip.h"
#include "../../../engine/audiofile/FileWriter.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
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
constexpr int kQuietMs   = 1000;
constexpr int kSettleMs  = 15000;
constexpr int kTailMs    = 25000;
constexpr int kIdleMs    = 3000;
constexpr int kPollGapMs = 200;
// A dozen meter ticks.
constexpr int kHiddenMs  = 400;

using Clock = std::chrono::steady_clock;

struct Quiet
{
    std::uint64_t seen = 0;
    Clock::time_point start {}, since {}, lastPoll {};
    bool judged = false;
};

// True once the counter has held still for kQuietMs of polls no more than
// kPollGapMs apart. The meters tick on the message thread this polls from, so
// time the thread spent stalled ran no meter ticks and proves nothing. When no
// two polls inside boundMs come that close, the case fails as too slow to
// judge rather than as a console that never came to rest.
std::function<bool()> quietFor (GuiHost& host, ScenarioContext& ctx, int boundMs, std::string what)
{
    auto quiet = std::make_shared<Quiet>();
    return [&host, &ctx, quiet, boundMs, what]
    {
        const auto count = host.meterRepaintRequests();
        const auto at = Clock::now();
        if (quiet->start == Clock::time_point {}) quiet->start = at;
        const bool stalled = quiet->lastPoll == Clock::time_point {}
                          || at - quiet->lastPoll > std::chrono::milliseconds (kPollGapMs);
        quiet->judged = quiet->judged || ! stalled;
        quiet->lastPoll = at;
        if (! quiet->judged && at - quiet->start >= std::chrono::milliseconds (boundMs - 1000))
        {
            ctx.expect (false, "the message thread never polled twice within "
                                   + std::to_string (kPollGapMs) + " ms, too slow to judge " + what);
            return true;
        }
        if (count != quiet->seen || stalled || quiet->since == Clock::time_point {})
        {
            quiet->seen = count;
            quiet->since = at;
            return false;
        }
        return at - quiet->since >= std::chrono::milliseconds (kQuietMs);
    };
}

// The meter as the display server holds it, which a repaint that was never
// asked for leaves stale; a component snapshot would paint it afresh.
void expectMeterAtRest (GuiHost& host, ScenarioContext& ctx, GuiHost::StripKind kind,
                        int index, const std::string& name)
{
    using Status = GuiHost::MeterRest::Status;
    const auto ppm = ctx.tempDir() / (name + ".ppm");
    const auto rest = host.meterRestPixels (kind, index, ppm);
    switch (rest.status)
    {
        case Status::noCapture:
            ctx.note (name + ": this platform cannot read the window back");
            return;
        case Status::notShown:
            ctx.expect (false, name + ": the meter is not showing, or only "
                                   + std::to_string (rest.height) + " px tall, so nothing was read");
            return;
        case Status::captureFailed:
            ctx.expect (false, name + ": the display server would not give the window back");
            return;
        case Status::read:
            break;
    }
    const auto summary = std::to_string (rest.lit) + " lit pixels on a "
                       + std::to_string (rest.height) + " px meter";
    ctx.note (name + ": " + summary);
    ctx.expect (rest.lit == 0, name + ": " + summary + " at rest");

    const auto* destination = std::getenv ("DUSKSTUDIO_SCENARIO_CAPTURE_DIR");
    if (destination == nullptr || *destination == '\0') return;
    const auto directory = std::filesystem::u8path (destination);
    std::error_code error;
    std::filesystem::create_directories (directory, error);
    std::filesystem::copy_file (ppm, directory / (name + ".ppm"),
                                std::filesystem::copy_options::overwrite_existing, error);
    std::ofstream (directory / (name + ".txt")) << summary << "\n";
}

bool writeTone (ScenarioContext& ctx, double rate, std::int64_t frames, const std::filesystem::path& path)
{
    std::vector<float> samples ((size_t) frames);
    for (std::int64_t i = 0; i < frames; ++i)
        samples[(size_t) i] = 0.5f * static_cast<float> (std::sin (6.283185307179586 * 440.0 * static_cast<double> (i) / rate));
    dusk::audio::WriteSpec spec;
    spec.sampleRate = rate;
    spec.numChannels = 1;
    spec.bitsPerSample = 32;
    auto writer = dusk::audio::FileWriter::create (path, spec);
    const float* channels[] = { samples.data() };
    return ctx.expect (writer != nullptr && writer->write (channels, 1, frames) && writer->flush(),
                       "could not write the playback tone");
}

// The longest settle is a channel strip's peak hold falling from the top of
// the fader scale to its floor, about three seconds. The window is held at its
// smallest height with the timeline open, where a channel meter is short
// enough that a gate missing a draw threshold leaves its foot lit.
std::optional<ScenarioResult> runIdleMetersStayStill (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (0);
    const auto originalPosition = engine.getTransport().getPlayhead();
    const auto originalRegions = track.regions;
    keepStage (host, ctx);
    ctx.cleanup ([&engine, &track, originalPosition, originalRegions]
    {
        engine.stop();
        track.regions = originalRegions;
        engine.getPlaybackEngine().preparePlayback();
        engine.getTransport().setPlayhead (originalPosition);
    });
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& each = session.track (t);
        ctx.keep (each.recordArmed);
        ctx.keep (each.inputMonitor);
        each.recordArmed.store (false);
        each.inputMonitor.store (false);
    }
    ctx.keep (track.mode);
    ctx.keep (track.strip.faderDb);
    ctx.keep (track.strip.mute);
    ctx.keep (track.automationMode);
    ctx.keep (track.strip.auxSendDb[(size_t) kAuxLane]);
    ctx.keep (session.master().mute);
    track.automationMode.store ((int) AutomationMode::Off);
    session.master().mute.store (true);
    engine.stop();
    host.switchToStage (GuiHost::Stage::Recording);
    const auto size = host.mainWindowSize();
    if (size.size() >= 4)
    {
        ctx.cleanup ([&host, size] { host.resizeMainWindow (size[0], size[1]); });
        if (! host.resizeMainWindow (size[0], size[3]))
            ctx.note ("the window would not take its smallest height");
    }
    const bool timelineShown = host.setTimelineShown (true);
    ctx.cleanup ([&host, timelineShown] { host.setTimelineShown (timelineShown); });

    const auto rate = engine.getCurrentSampleRate();
    const auto frames = static_cast<std::int64_t> (rate * 5.0);
    const auto tone = ctx.tempDir() / "idle-meters-tone.wav";
    if (! writeTone (ctx, rate, frames, tone))
        return ctx.verdict();
    auto playTone = [&engine, &track, tone, frames]
    {
        AudioRegion region;
        using File = std::decay_t<decltype (region.file)>;
        region.file = File (tone.u8string().c_str());
        region.lengthInSamples = frames;
        region.numChannels = 1;
        track.regions = { region };
        track.mode.store ((int) Track::Mode::Mono);
        track.strip.faderDb.store (0.0f);
        track.strip.mute.store (false);
        engine.getTransport().setPlayhead (0);
        engine.play();
    };

    auto idleStart = std::make_shared<std::uint64_t> (0);
    auto auxLeg = std::make_shared<bool> (false);
    auto auxLoaded = std::make_shared<bool> (false);
    auto steps = std::make_shared<std::vector<Step>>();
    // The console's meters rest while it is hidden, and what follows must hold
    // for meters that started again.
    steps->push_back ({ 0, [&host] { host.switchToStage (GuiHost::Stage::Aux); } });
    steps->push_back ({ kHiddenMs, [&host, &ctx]
    {
        const int running = host.meterTimersRunning();
        ctx.expect (running == 0, std::to_string (running) + " meter timers still run with the console hidden");
        host.switchToStage (GuiHost::Stage::Recording);
    } });
    steps->push_back ({ 0, [&host, idleStart] { *idleStart = host.meterRepaintRequests(); },
        quietFor (host, ctx, kSettleMs, "whether the stopped console came to rest"),
        "the stopped console never stopped asking for repaints", {}, kSettleMs });
    steps->push_back ({ kIdleMs, [&host, &ctx, idleStart, playTone]
    {
        const auto asked = host.meterRepaintRequests() - *idleStart;
        ctx.expect (asked == 0, "the stopped console asked for " + std::to_string (asked)
                                    + " repaints in " + std::to_string (kIdleMs) + " ms");
        *idleStart = host.meterRepaintRequests();
        playTone();
    } });
    steps->push_back ({ 0, [] {},
        [&host, idleStart] { return host.meterRepaintRequests() > *idleStart; },
        "the meters asked for no repaint while a tone played", {}, kSettleMs });
    steps->push_back ({ 1000, [&engine] { engine.stop(); } });
    steps->push_back ({ 0, [&host, &ctx]
    {
        expectMeterAtRest (host, ctx, GuiHost::StripKind::Channel, 0, "rest-channel");
        host.switchToStage (GuiHost::Stage::Aux);
    }, quietFor (host, ctx, kSettleMs, "whether the channel meters came to rest after Stop"),
       "the channel meters never came to rest after Stop", {}, kSettleMs });
    steps->push_back ({ 300, [&host, &ctx, &engine, &track, playTone, auxLeg, auxLoaded]
    {
        const int originalLane = host.activeAuxLane();
        if (originalLane >= 0 && originalLane != kAuxLane)
        {
            ctx.cleanup ([&host, originalLane] { host.clickAuxSelector (originalLane); });
            host.clickAuxSelector (kAuxLane);
        }
        auto* lane = host.auxLane (kAuxLane);
        if (! ctx.expect (lane != nullptr && host.activeAuxLane() == kAuxLane,
                          "the aux stage does not show the first lane"))
            return;
        track.strip.auxSendDb[(size_t) kAuxLane].store (0.0f);

        // A paint that read the meters again would draw the silence stored
        // after the sample, not the sample.
        std::array<int, 2> drawn { -1, -1 };
        for (int attempt = 0; attempt < 3 && (drawn[0] != 1 || drawn[1] != 1); ++attempt)
        {
            const auto now = lane->metersDrawSample (0, -6.0f);
            drawn[0] = std::max (drawn[0], now[0]);
            drawn[1] = std::max (drawn[1], now[1]);
        }
        ctx.expect (drawn[0] == 1, drawn[0] < 0 ? "the aux strip meter is not laid out to check"
                                                : "the aux strip meter did not draw the level it sampled");
        ctx.expect (drawn[1] == 1, drawn[1] < 0 ? "the first Sources row is not laid out to check"
                                                : "the first Sources row did not draw the level it sampled");

        const auto& laneStrip = engine.getAuxLaneStrip (kAuxLane);
        if (laneStrip.insertMode[(size_t) kAuxSlot].load() != AuxLaneStrip::kInsertEmpty
            || laneStrip.isBuiltinLoaded (kAuxSlot))
        {
            ctx.note ("the first aux slot is in use, so the reverb tail is not checked");
            return;
        }
        if (! lane->loadBuiltin (kAuxSlot, "dusk.builtin.reverb"))
        {
            ctx.expect (false, "the reverb would not load on the first aux lane");
            return;
        }
        *auxLoaded = true;
        ctx.cleanup ([&host, auxLoaded]
        {
            if (! *auxLoaded) return;
            if (auto* l = host.auxLane (kAuxLane)) l->unloadSlot (kAuxSlot);
        });
        *auxLeg = true;
        playTone();
    } });
    steps->push_back ({ 1500, [&engine] { engine.stop(); } });
    auto auxQuiet = quietFor (host, ctx, kTailMs, "whether the aux meter came to rest after Stop");
    steps->push_back ({ 0, [&host, &ctx, auxLeg, auxLoaded]
    {
        if (! *auxLeg) return;
        expectMeterAtRest (host, ctx, GuiHost::StripKind::Aux, kAuxLane, "rest-aux");
        if (auto* lane = host.auxLane (kAuxLane)) lane->unloadSlot (kAuxSlot);
        *auxLoaded = false;
    }, [auxLeg, auxQuiet] { return ! *auxLeg || auxQuiet(); },
       "the aux meter never came to rest after Stop", {}, kTailMs });
    steps->push_back ({ 300, [] {} });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

// A hidden lane or strip skips its refresh, so what changed while it was hidden
// has to be on its first paint once it shows. Each check reads that paint in the
// step that shows it, before any meter tick can run.
std::optional<ScenarioResult> runHiddenViewsShowCurrentState (GuiHost& host, ScenarioContext& ctx)
{
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no open modal");
    auto& track = ctx.session().track (0);
    auto& send = track.strip.auxSendDb[(size_t) kAuxLane];
    auto& liveSend = track.strip.liveAuxSendDb[(size_t) kAuxLane];
    keepStage (host, ctx);
    ctx.keep (track.strip.hpfEnabled);
    ctx.keep (track.strip.hpfFreq);
    ctx.keep (send);
    ctx.keep (liveSend);
    track.strip.hpfEnabled.store (false);
    send.store (-20.0f);
    liveSend.store (-20.0f);

    host.switchToStage (GuiHost::Stage::Aux);
    const int originalLane = host.activeAuxLane();
    if (originalLane >= 0 && originalLane != kAuxLane)
    {
        ctx.cleanup ([&host, originalLane] { host.clickAuxSelector (originalLane); });
        host.clickAuxSelector (kAuxLane);
    }
    if (host.activeAuxLane() != kAuxLane || host.auxLane (kAuxLane) == nullptr)
        return ScenarioResult::fail ("the aux stage does not show the first lane");
    host.switchToStage (GuiHost::Stage::Mixing);

    // Each change lands while its view is hidden, and must ask for no repaint then.
    auto asked = std::make_shared<std::uint64_t> (0);
    const auto expectNoneAsked = [&host, &ctx, asked] (const std::string& what)
    {
        const auto more = host.meterRepaintRequests() - *asked;
        ctx.expect (more == 0, what + " asked for " + std::to_string (more) + " repaints while hidden");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ kHiddenMs, [&host, &send, &liveSend, asked]
    {
        *asked = host.meterRepaintRequests();
        send.store (-7.5f);
        liveSend.store (-7.5f);
    } });
    steps->push_back ({ kHiddenMs, [&host, &ctx, expectNoneAsked]
    {
        expectNoneAsked ("the aux lane");
        host.switchToStage (GuiHost::Stage::Aux);
        auto* lane = host.auxLane (kAuxLane);
        const auto rows = lane != nullptr ? lane->drawnSourceRows() : std::vector<std::string>();
        const auto row = std::find_if (rows.begin(), rows.end(),
                                       [] (const std::string& drawn) { return drawn.rfind ("1\t", 0) == 0; });
        if (ctx.expect (row != rows.end(), "the lane's first paint after it showed drew no first Sources row"))
            ctx.expect (row->find ("\t-7.5 dB\t") != std::string::npos,
                        "the lane's first paint after it showed drew the first Sources row as \"" + *row
                            + "\", not at the -7.5 dB it was set to while hidden");
    } });
    steps->push_back ({ kHiddenMs, [&host, &track, asked]
    {
        *asked = host.meterRepaintRequests();
        track.strip.hpfFreq.store (120.0f);
        track.strip.hpfEnabled.store (true);
    } });
    steps->push_back ({ kHiddenMs, [&host, &ctx, expectNoneAsked]
    {
        expectNoneAsked ("the channel strips");
        host.switchToStage (GuiHost::Stage::Mixing);
        const auto text = host.stripFilterText (0, true);
        if (! text)
        {
            ctx.note ("the first channel strip is on another console page, so its first paint is not checked");
            return;
        }
        ctx.expect (! text->empty() && *text != "OFF",
                    "the strip's first paint after it showed has its high-pass at \"" + *text
                        + "\", though the filter was switched on while it was hidden");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

// The gain-reduction and VU meters and the mini timeline only draw, so their
// timers stop while a console page or the whole console is hidden, and start
// again, with what changed meanwhile, the moment it shows.
std::optional<ScenarioResult> runHiddenMeterTimersRest (GuiHost& host, ScenarioContext& ctx)
{
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no open modal");
    auto& strip = ctx.session().track (0).strip;
    keepStage (host, ctx);
    ctx.keep (strip.hpfEnabled);
    ctx.keep (strip.hpfFreq);
    strip.hpfEnabled.store (false);
    host.switchToStage (GuiHost::Stage::Mixing);
    int originalPage = -1;
    for (int page = 0; page < host.consolePageCount(); ++page)
        if (host.consolePageMatches (page)) originalPage = page;
    const auto size = host.mainWindowSize();
    if (size.size() >= 4)
    {
        ctx.cleanup ([&host, size] { host.resizeMainWindow (size[0], size[1]); });
        if (! host.resizeMainWindow (size[2], size[1]))
            ctx.note ("the window would not take its smallest width");
    }
    ctx.cleanup ([&host, originalPage]
    {
        if (originalPage >= 0 && originalPage < host.consolePageCount())
            host.pressKey (std::to_string (originalPage + 1), (char) ('1' + originalPage));
    });

    auto paged = std::make_shared<bool> (false);
    auto shownText = std::make_shared<std::string>();
    const auto showPage = [&host, &ctx] (int page)
    {
        return ctx.expect (host.pressKey (std::to_string (page + 1), (char) ('1' + page))
                               && host.consolePageMatches (page),
                           "the console would not show page " + std::to_string (page + 1));
    };
    const auto expectTimer = [&host, &ctx] (bool running, const std::string& what)
    {
        const auto state = host.stripMeterTimerRunning (0);
        ctx.expect (state.has_value() && *state == running,
                    ! state ? "the first channel strip is missing"
                            : "the first channel strip's gain-reduction meter " + what);
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ kHiddenMs, [&host, &ctx, paged, showPage]
    {
        *paged = host.consolePageCount() > 1;
        if (! *paged)
            ctx.note ("the console shows every strip at its narrowest width, so no page is hidden");
        else
            showPage (0);
    } });
    steps->push_back ({ kHiddenMs, [&ctx, paged, showPage, expectTimer]
    {
        expectTimer (true, "is stopped while its page shows");
        if (*paged && showPage (1))
            ctx.note ("page 2 shows, so the first strip is hidden");
    } });
    steps->push_back ({ kHiddenMs, [&host, &ctx, &strip, paged, shownText, showPage, expectTimer]
    {
        if (*paged)
        {
            expectTimer (false, "still runs while its page is hidden");
            strip.hpfFreq.store (120.0f);
            strip.hpfEnabled.store (true);
            if (showPage (0))
            {
                expectTimer (true, "did not start again when its page showed");
                const auto text = host.stripFilterText (0, true);
                ctx.expect (text && ! text->empty() && *text != "OFF",
                            "the strip's first paint after its page showed has its high-pass at \""
                                + text.value_or ("") + "\", though the filter was switched on while it was hidden");
                *shownText = text.value_or ("");
            }
        }
        host.switchToStage (GuiHost::Stage::Aux);
    } });
    steps->push_back ({ kHiddenMs, [&host, &ctx, &strip, shownText, expectTimer]
    {
        const int running = host.meterTimersRunning();
        ctx.expect (running == 0, std::to_string (running) + " meter timers still run with the console hidden");
        strip.hpfFreq.store (240.0f);
        strip.hpfEnabled.store (true);
        host.switchToStage (GuiHost::Stage::Mixing);
        ctx.expect (host.meterTimersRunning() > 0, "no meter timer started again when the console showed");
        expectTimer (true, "did not start again when the console showed");
        const auto text = host.stripFilterText (0, true);
        if (! text)
        {
            ctx.note ("the first channel strip is on another console page, so its first paint is not checked");
            return;
        }
        ctx.expect (! text->empty() && *text != "OFF" && *text != *shownText,
                    "the strip's first paint after the console showed has its high-pass at \"" + *text
                        + "\", not the frequency set while it was hidden");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar hiddenViews { Scenario {
    "gui.hidden_meters_show_current_state", { "gui", "meter" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runHiddenViewsShowCurrentState (host, ctx); }
} };

const ScenarioRegistrar hiddenMeterTimers { Scenario {
    "gui.hidden_meter_timers_rest", { "gui", "meter" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runHiddenMeterTimersRest (host, ctx); }
} };

const ScenarioRegistrar idleMeters { Scenario {
    "gui.idle_meters_stay_still", { "gui", "meter" }, Needs::Engine | Needs::Gui,
    {}, {}, 90000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runIdleMetersStayStill (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
