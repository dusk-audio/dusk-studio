#include "../GuiScenarioSupport.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace duskstudio::scenario::gui
{
namespace
{
constexpr int kSettleMs = 1500;
constexpr int kQuietWindowMs = 2000;
// A one-off repaint, such as a status message timing out, spoils one window;
// a control that repaints on a timer spoils every one.
constexpr int kQuietWindows = 5;

// An idle window paints nothing, so the display's frame timer can rest.
std::optional<ScenarioResult> runIdleWindowPaintsNothing (GuiHost& host, ScenarioContext& ctx)
{
    if (! ctx.engine().getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    if (! host.hoverForTooltip (false))
        return ScenarioResult::skip ("no point in the window is free of tooltips");

    // A change in the displayed CPU load legitimately repaints the status bar.
    if (host.pinDspReadings (DspReadings {}))
        ctx.cleanup ([&host] { host.pinDspReadings (std::nullopt); });

    auto window = std::make_shared<std::function<void (int, std::string)>>();
    std::weak_ptr<std::function<void (int, std::string)>> weakWindow = window;
    *window = [&host, &ctx, weakWindow] (int attempt, std::string counts)
    {
        const auto before = host.windowFramesPainted();
        ctx.later (kQuietWindowMs, [&host, &ctx, next = weakWindow.lock(), attempt, before, counts]
        {
            const auto painted = host.windowFramesPainted() - before;
            if (painted == 0)
            {
                ctx.complete (ctx.verdict());
                return;
            }
            const auto seen = counts + (counts.empty() ? "" : ", ") + std::to_string (painted);
            if (attempt + 1 < kQuietWindows)
            {
                (*next) (attempt + 1, seen);
                return;
            }
            ctx.expect (false, "the idle window painted in every " + std::to_string (kQuietWindowMs)
                                   + " ms window; frames per window: " + seen);
            ctx.complete (ctx.verdict());
        });
    };
    ctx.later (kSettleMs, [window] { (*window) (0, {}); });
    return std::nullopt;
}

// The tooltip window stops polling the pointer while no tip waits to show, and
// a pointer move over a control with a tip starts it again.
std::optional<ScenarioResult> runTooltipShowsAfterRest (GuiHost& host, ScenarioContext& ctx)
{
    if (! host.modalStackEmpty())
        return ScenarioResult::skip ("requires no modal");
    if (! host.hoverForTooltip (false))
        return ScenarioResult::skip ("no point in the window is free of tooltips");
    ctx.cleanup ([&host] { host.hoverForTooltip (false); });

    ctx.later (kSettleMs, [&host, &ctx]
    {
        ctx.expect (! host.tooltipShowing(), "a tooltip showed over a control that has none");
        if (! host.hoverForTooltip (true))
        {
            ctx.complete (ScenarioResult::skip (
                "no control's tooltip can show; the framework hides tips while the app is not in front"));
            return;
        }
        ctx.waitUntil ([&host] { return host.tooltipShowing(); }, 3000, [&host, &ctx]
        {
            host.hoverForTooltip (false);
            ctx.waitUntil ([&host] { return ! host.tooltipShowing(); }, 3000,
                           [&ctx] { ctx.complete (ctx.verdict()); },
                           "the tooltip stayed up after the pointer left its control");
        }, "no tooltip showed over a control that has one after the tooltip poll had rested");
    });
    return std::nullopt;
}

const ScenarioRegistrar idleWindowPaintsNothing { Scenario {
    "gui.idle_window_paints_nothing", { "gui", "idle" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runIdleWindowPaintsNothing (host, ctx); }
} };

const ScenarioRegistrar tooltipShowsAfterRest { Scenario {
    "gui.tooltip_shows_after_rest", { "gui", "idle" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTooltipShowsAfterRest (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
