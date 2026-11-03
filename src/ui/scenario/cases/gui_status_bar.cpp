#include "../GuiScenarioSupport.h"
#include "../../DspReadout.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <regex>
#include <string>

#if defined (__linux__)
 #include "../../../engine/RealtimeKit.h"
 #include <algorithm>
 #include <csignal>
 #include <sys/resource.h>
#endif

namespace duskstudio::scenario::gui
{
namespace
{
const char* severityName (DspSeverity severity)
{
    switch (severity)
    {
        case DspSeverity::disruption: return "red";
        case DspSeverity::risk:       return "amber";
        case DspSeverity::normal:     break;
    }
    return "the normal colour";
}

// Text is antialiased, so the strongest pixel is a blend of the text colour
// into the background. Each palette colour is tried as the text colour and the
// one whose blend leaves the smallest remainder wins.
std::optional<DspSeverity> severityOfPaint (std::uint32_t argb)
{
    const auto offset = [] (std::uint32_t colour)
    {
        std::array<double, 3> rgb {};
        for (std::size_t i = 0; i < 3; ++i)
        {
            const auto shift = (unsigned) (16 - 8 * (int) i);
            rgb[i] = (double) ((colour >> shift) & 0xffu) - (double) ((kStatusBarBackground >> shift) & 0xffu);
        }
        return rgb;
    };
    const auto painted = offset (argb);
    double paintedNorm = 0.0;
    for (const double c : painted) paintedNorm += c * c;
    if (paintedNorm < 60.0 * 60.0) return std::nullopt;

    std::optional<DspSeverity> best;
    double bestRemainder = std::numeric_limits<double>::infinity();
    for (const auto severity : { DspSeverity::normal, DspSeverity::risk, DspSeverity::disruption })
    {
        const auto text = offset (dspSeverityColour (severity));
        double dot = 0.0, norm = 0.0;
        for (std::size_t i = 0; i < 3; ++i) { dot += painted[i] * text[i]; norm += text[i] * text[i]; }
        const double coverage = dot / norm;
        double remainder = 0.0;
        for (std::size_t i = 0; i < 3; ++i)
        {
            const double d = painted[i] - coverage * text[i];
            remainder += d * d;
        }
        if (remainder < bestRemainder) { bestRemainder = remainder; best = severity; }
    }
    return best;
}

void expectPainted (GuiHost& host, ScenarioContext& ctx, DspSeverity want, const std::string& what)
{
    const auto text = host.dspReadout();
    const auto painted = severityOfPaint (host.dspReadoutPaint());
    if (! ctx.expect (painted.has_value(), what + ": the DSP readout painted no text: " + text)) return;
    ctx.expect (*painted == want, what + " painted the DSP readout " + severityName (*painted)
                                      + " instead of " + severityName (want) + ": " + text);
}

int backendCountShown (const std::string& text)
{
    std::smatch match;
    return std::regex_search (text, match, std::regex (R"(\(\d+/(\d+)\))")) ? std::stoi (match[1].str()) : -1;
}

// Red belongs to backend xruns alone. An engine overrun, load above 85% and a
// realtime warning are amber however many of them stack up, and a backend xrun
// beside any of them is still red.
void checkSeverities (GuiHost& host, ScenarioContext& ctx)
{
    struct Case
    {
        const char* what;
        DspReadings readings;
        const char* prefix;
        const char* suffix;
        DspSeverity want;
    };
    const Case cases[] = {
        { "an engine overrun alone",         { 0.10, 3, 0, 0 }, "DSP: 10% (3/0)", "",      DspSeverity::risk },
        { "a backend xrun alone",            { 0.10, 0, 2, 0 }, "DSP: 10% (0/2)", "",      DspSeverity::disruption },
        { "engine overruns and backend xruns", { 0.10, 3, 2, 0 }, "DSP: 10% (3/2)", "",    DspSeverity::disruption },
        { "load above 85%",                  { 0.90, 0, 0, 0 }, "DSP: 90% (0/0)", "",      DspSeverity::risk },
        { "load at 85%",                     { 0.85, 0, 0, 0 }, "DSP: 85% (0/0)", "",      DspSeverity::normal },
        { "a realtime warning alone",        { 0.10, 0, 0, 1 }, "DSP: 10% (0/0)", " RT 1", DspSeverity::risk },
        { "a backend xrun beside every risk", { 0.95, 3, 1, 2 }, "DSP: 95% (3/1)", " RT 2", DspSeverity::disruption },
        { "quiet readings",                  { 0.10, 0, 0, 0 }, "DSP: 10% (0/0)", "",      DspSeverity::normal },
    };
    for (const auto& c : cases)
    {
        if (! ctx.expect (host.pinDspReadings (c.readings), "the status bar would not take readings")) return;
        const auto text = host.dspReadout();
        const std::string prefix = c.prefix, suffix = c.suffix;
        const bool endsRight = suffix.empty() ? text.find ("RT") == std::string::npos
                                              : text.size() >= suffix.size()
                                                    && text.compare (text.size() - suffix.size(), suffix.size(), suffix) == 0;
        ctx.expect (text.rfind (prefix, 0) == 0 && endsRight,
                    std::string (c.what) + " read \"" + text + "\" instead of \"" + prefix + "\" ending \"" + suffix + "\"");
        expectPainted (host, ctx, c.want, c.what);
    }
    host.pinDspReadings (std::nullopt);
}

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
    ctx.cleanup ([&host] { host.pinDspReadings (std::nullopt); });

    checkSeverities (host, ctx);

    engine.resetXRunCounts();
    for (int overrun = 0; overrun < 3; ++overrun) engine.noteEngineXRun();

    // The readout is timer-driven, so the count appears on a later tick.
    ctx.waitUntil ([&host] { return host.dspReadout().find (" (3/") != std::string::npos; }, 3000,
        [&host, &ctx, &engine]
        {
            const auto text = host.dspReadout();
            ctx.expect (std::regex_match (text, std::regex (R"(DSP: \d+% \(3/\d+\)( @\dx)?)")),
                        "the DSP readout does not carry a load percentage beside the dropout pair: " + text);
            ctx.expect (engine.getXRunCount() == 3, "the readout reported a count the engine does not hold");
            // The device is live under the gate, so a real backend xrun can
            // land here; the readout's own pair says which colour is right.
            expectPainted (host, ctx, backendCountShown (text) > 0 ? DspSeverity::disruption : DspSeverity::risk,
                           "the engine's own overruns");
            if (! ctx.expect (host.doubleClickDspReadout(), "the DSP readout did not take a double-click"))
            { ctx.complete (ctx.verdict()); return; }
            ctx.expect (engine.getXRunCount() == 0, "the double-click did not zero the engine dropout count");
            ctx.expect (engine.getBackendXRunCount() == 0, "the double-click did not zero the backend dropout count");
            // mouseDoubleClick refreshes the readout itself rather than
            // leaving a stale warning up until the next tick.
            ctx.expect (host.dspReadout().find (" (0/0)") != std::string::npos,
                        "the readout still showed the old counters after the reset: " + host.dspReadout());
            expectPainted (host, ctx, engine.getCpuUsage() > 0.85f ? DspSeverity::risk : DspSeverity::normal,
                           "the reset");
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
            expectPainted (host, ctx, backendCountShown (text) > 0 ? DspSeverity::disruption : DspSeverity::risk,
                           "the realtime warning");
            if (! ctx.expect (host.doubleClickDspReadout(), "the DSP readout did not take a double-click"))
            { ctx.complete (ctx.verdict()); return; }
            ctx.expect (engine.getRealtimeDemotionCount() == 0, "the double-click did not zero the realtime count");
            ctx.expect (host.dspReadout().find ("RT") == std::string::npos,
                        "the readout still showed the realtime count after the reset: " + host.dspReadout());
            expectPainted (host, ctx, engine.getCpuUsage() > 0.85f ? DspSeverity::risk : DspSeverity::normal,
                           "the reset");
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
} // namespace
} // namespace duskstudio::scenario::gui
