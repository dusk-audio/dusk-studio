#include "../GuiScenarioSupport.h"
#include "../../../dsp/MultiCompStage.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// What a compressor editor draws and sends back, read from the host side: the
// face's own pixels as the display server holds them after the session moves a
// control, and the session after the harness works a control with the pointer
// through the display server. Both run on an X11 window; elsewhere the editor
// cannot be read back or reached that way, and the cases skip.
namespace duskstudio::scenario::gui
{
namespace
{
using Kind = GuiHost::StripKind;
using Pixels = GuiHost::FacePixels;

constexpr const char* kNoReadBack = "reads the editor window back through the X server";

// Multi-Comp 2's layout, multicompp::ui_detail in the donor's
// plugins/multi-comp/daf-plugin/MultiCompUI.cpp: design pixels on its 1120 x
// 380 canvas, whose 44 px header a host that frames the editor itself
// (MULTICOMP2_EMBEDDED_UI, as Dusk Studio builds it) does not show. A control
// the donor moves takes the region these cases read or press with it, and the
// case fails rather than passes.
namespace mc2
{
constexpr float kDesignWidth = 1120.0f, kDesignHeight = 380.0f, kCanvasTop = 44.0f;
// ModeRowLayout, and kFaceAreaTop under it.
constexpr float kModeRowY0 = 46.0f, kModeRowY1 = 70.0f;
constexpr float kTabLeft = 16.0f, kTabWidth = 92.0f, kTabGap = 4.0f;
constexpr float kBypassX0 = 918.0f, kBypassX1 = 1004.0f;
constexpr float kFaceTop = 72.0f;
// VcaFaceplateLayout.
constexpr float kVcaThresholdX = 178.0f, kVcaCompressionX = 385.0f, kVcaOutputX = 595.0f;
constexpr float kVcaKnobY = 212.0f, kVcaKnobRadius = 34.0f, kVcaCompressionRadius = 42.0f;
// BusFaceplateLayout.
constexpr float kBusRatioX = 536.0f, kBusMakeupX = 663.0f, kBusMixX = 949.0f;
constexpr float kBusRow1Y = 152.0f, kBusKnobRadius = 23.0f;

constexpr float tabCentreX (int slot) { return kTabLeft + (float) slot * (kTabWidth + kTabGap) + 0.5f * kTabWidth; }
constexpr float kModeRowCentreY = 0.5f * (kModeRowY0 + kModeRowY1);
constexpr float kBypassCentreX = 0.5f * (kBypassX0 + kBypassX1);
} // namespace mc2

// Where the canvas lands in the editor's window: at its design proportions,
// as large as the window allows and centred (MultiCompUI::onImGuiDisplay).
struct Face
{
    Face (int width, int height)
    {
        const float windowHeight = mc2::kDesignHeight - mc2::kCanvasTop;
        scale = std::min ((float) width / mc2::kDesignWidth, (float) height / windowHeight);
        left = 0.5f * ((float) width - mc2::kDesignWidth * scale);
        top = 0.5f * ((float) height - windowHeight * scale) - mc2::kCanvasTop * scale;
    }
    float x (float design) const { return left + design * scale; }
    float y (float design) const { return top + design * scale; }

    float scale = 1.0f, left = 0.0f, top = 0.0f;
};

// A rectangle of the canvas, in design pixels.
struct Area
{
    float x0, y0, x1, y1;
    std::string name;
};

Area around (float x, float y, float radius, std::string name)
{
    return { x - radius, y - radius, x + radius, y + radius, std::move (name) };
}

// How many of the area's pixels changed colour by more than a shade between
// two reads of one face, and how many it holds; -1 when the reads differ in
// size.
std::array<int, 2> changedIn (const Pixels& before, const Pixels& after, const Area& area)
{
    static constexpr int kShade = 24;
    if (before.width != after.width || before.height != after.height || before.width <= 0)
        return { -1, -1 };
    const Face face (before.width, before.height);
    const int x0 = std::clamp ((int) std::floor (face.x (area.x0)), 0, before.width);
    const int x1 = std::clamp ((int) std::ceil (face.x (area.x1)), 0, before.width);
    const int y0 = std::clamp ((int) std::floor (face.y (area.y0)), 0, before.height);
    const int y1 = std::clamp ((int) std::ceil (face.y (area.y1)), 0, before.height);
    int changed = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
        {
            const auto at = ((std::size_t) y * (std::size_t) before.width + (std::size_t) x) * 3;
            for (std::size_t c = 0; c < 3; ++c)
                if (std::abs ((int) before.rgb[at + c] - (int) after.rgb[at + c]) > kShade)
                {
                    ++changed;
                    break;
                }
        }
    return { changed, std::max (0, (x1 - x0) * (y1 - y0)) };
}

// The control the session moved redrew, by at least a stroke's worth of
// pixels, and each other control stayed as it was.
void expectRedrawn (ScenarioContext& ctx, const Pixels& before, const Pixels& after, const std::string& what,
                    const Area& moved, const std::vector<Area>& kept)
{
    static constexpr int kStroke = 20;
    const auto redrawn = changedIn (before, after, moved);
    std::string counts = what + ": " + moved.name + " " + std::to_string (redrawn[0]) + " of "
                       + std::to_string (redrawn[1]) + " pixels changed";
    ctx.expect (redrawn[0] >= kStroke, what + " redrew " + std::to_string (redrawn[0]) + " of the "
                                           + std::to_string (redrawn[1]) + " pixels of " + moved.name
                                           + " on the face");
    for (const auto& area : kept)
    {
        const auto same = changedIn (before, after, area);
        counts += ", " + area.name + " " + std::to_string (same[0]) + " of " + std::to_string (same[1]);
        ctx.expect (same[0] >= 0 && same[0] * 100 <= same[1], what + " changed " + std::to_string (same[0])
                                                              + " of the " + std::to_string (same[1])
                                                              + " pixels of " + area.name);
    }
    ctx.note (counts);
}

void keepComp (ScenarioContext& ctx, CompParams& comp)
{
    for (std::size_t i = 0; i < kCompParamCount; ++i)
        ctx.keep (comp.atom ((CompParamId) i));
}

// Every value of the kind but `moved` is still what `before` holds.
void expectOnly (ScenarioContext& ctx, CompStage kind, const CompParams& comp, const CompParams& before,
                 CompParamId moved, const std::string& what)
{
    ctx.expect (std::abs (comp.get (moved) - before.get (moved)) > 1.0e-4f,
                what + " left " + compParamSpec (moved).symbol + " at " + std::to_string (comp.get (moved)));
    for (const auto id : compStageParams (kind))
        if (id != moved)
            ctx.expect (std::abs (comp.get (id) - before.get (id)) < 1.0e-6f,
                        what + " moved " + compParamSpec (id).symbol + " from " + std::to_string (before.get (id))
                            + " to " + std::to_string (comp.get (id)));
}

bool faceReadable (GuiHost& host)
{
   #if defined (__linux__)
    return ! host.nativeWaylandWindow();
   #else
    (void) host;
    return false;
   #endif
}

// The reads and the pointer of one compressor editor.
struct Editor
{
    GuiHost& host;
    ScenarioContext& ctx;
    Kind kind;
    int index;
    std::string name;

    Pixels read() const
    {
        const auto ppm = ctx.tempDir() / "face.ppm";
        auto pixels = host.compEditorPixels (kind, index, ppm);
        ctx.expect (pixels.status == Pixels::Status::read,
                    "the " + name + " editor's face was not read back (status "
                        + std::to_string ((int) pixels.status) + ")");
        return pixels;
    }

    // One pointer edge at a design point of the face, which `size` says where
    // it lands: button 1 presses, -1 releases, 0 moves.
    bool pointer (const Pixels& size, float x, float y, int button) const
    {
        const Face face (size.width, size.height);
        const bool sent = host.compEditorPointer (kind, index, face.x (x), face.y (y), button);
        return ctx.expect (sent, "the pointer did not reach the " + name + " editor");
    }
};

// A press and release at one point of the face, a step apart, as a click.
void click (std::vector<Step>& steps, std::shared_ptr<Editor> editor, std::shared_ptr<Pixels> size,
            float x, float y, std::function<void()> then)
{
    steps.push_back ({ 300, [editor, size, x, y] { editor->pointer (*size, x, y, 1); } });
    steps.push_back ({ 100, [editor, size, x, y] { editor->pointer (*size, x, y, -1); } });
    steps.push_back ({ 300, std::move (then) });
}

// A press at a point, moves up `rise` design pixels in four steps, and a
// release, as a drag.
void drag (std::vector<Step>& steps, std::shared_ptr<Editor> editor, std::shared_ptr<Pixels> size,
           float x, float y, float rise, std::function<void()> then)
{
    steps.push_back ({ 300, [editor, size, x, y] { editor->pointer (*size, x, y, 1); } });
    for (int i = 1; i <= 4; ++i)
        steps.push_back ({ 60, [editor, size, x, y, rise, i] { editor->pointer (*size, x, y - rise * (float) i / 4.0f, 0); } });
    steps.push_back ({ 100, [editor, size, x, y, rise] { editor->pointer (*size, x, y - rise, -1); } });
    steps.push_back ({ 300, std::move (then) });
}

// A channel strip's compressor editor, the VCA face of a strip switched out,
// against the session both ways. Session to editor: a threshold change redraws
// the THRESHOLD knob and leaves the other knobs alone, the status light
// redraws BYPASS, and a mode change redraws the face. Editor to session: a
// click on the VCA tab sets the strip's mode, picked, without engaging; a drag
// on THRESHOLD moves that value alone and engages, as a face knob does; a
// click on BYPASS switches it out again.
std::optional<ScenarioResult> runCompEditorStripFace (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native UI");
   #else
    if (! faceReadable (host)) return ScenarioResult::skip (kNoReadBack);
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (kStripIndex);
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    if (! host.canEmbedPluginEditors())
        return ScenarioResult::skip ("requires a window the compressor editor can embed into");
    if (track.frozen.load()) return ScenarioResult::skip ("requires an unfrozen first track");
    auto* strip = readyStrip (host, ctx);
    if (strip == nullptr) return ScenarioResult::fail ("channel strip is unavailable");

    auto& comp = track.strip.comp;
    keepComp (ctx, comp);
    ctx.keep (track.strip.compEnabled);
    ctx.keep (track.strip.compMode);
    ctx.keep (track.strip.compModePicked);
    ctx.cleanup ([&host, strip, mode = track.mode.load()]
    {
        host.closeCompEditor (Kind::Channel, kStripIndex);
        drainModals (host);
        strip->restoreTrackMode (mode);
    });
    strip->restoreTrackMode ((int) Track::Mode::Mono);
    track.strip.compEnabled.store (false);
    track.strip.compMode.store (2);
    track.strip.compModePicked.store (false);
    comp.set (CompParamId::VcaThreshold, -45.0f);

    auto editor = std::make_shared<Editor> (Editor { host, ctx, Kind::Channel, kStripIndex, "strip" });
    auto before = std::make_shared<Pixels>();
    auto values = std::make_shared<CompParams>();
    const auto threshold = around (mc2::kVcaThresholdX, mc2::kVcaKnobY, mc2::kVcaKnobRadius + 8.0f, "THRESHOLD");
    const auto compression = around (mc2::kVcaCompressionX, mc2::kVcaKnobY, mc2::kVcaCompressionRadius + 8.0f, "COMPRESSION");
    const auto output = around (mc2::kVcaOutputX, mc2::kVcaKnobY, mc2::kVcaKnobRadius + 8.0f, "OUTPUT GAIN");
    const Area bypass { mc2::kBypassX0, mc2::kModeRowY0, mc2::kBypassX1, mc2::kModeRowY1, "BYPASS" };
    const Area tabs { mc2::kTabLeft, mc2::kModeRowY0, mc2::tabCentreX (2) + 0.5f * mc2::kTabWidth, mc2::kModeRowY1, "the mode tabs" };
    const Area faceArea { 0.0f, mc2::kFaceTop, mc2::kDesignWidth, mc2::kDesignHeight, "the face" };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host] { host.openCompEditor (Kind::Channel, kStripIndex); } });
    steps->push_back ({ 0, [] {}, [&host] { return host.compEditorDrawn (Kind::Channel, kStripIndex); },
                        "the strip's compressor editor never drew a frame" });
    steps->push_back ({ 300, [editor, before, &comp]
    {
        *before = editor->read();
        comp.set (CompParamId::VcaThreshold, -5.0f);
    } });
    steps->push_back ({ 300, [editor, before, &ctx, &track, threshold, compression, output]
    {
        const auto after = editor->read();
        expectRedrawn (ctx, *before, after, "THRESHOLD set in the session", threshold, { compression, output });
        *before = after;
        track.strip.compEnabled.store (true);
    } });
    steps->push_back ({ 300, [editor, before, &ctx, &track, bypass, tabs, compression]
    {
        const auto after = editor->read();
        expectRedrawn (ctx, *before, after, "the status light switching the compressor in", bypass, { tabs, compression });
        *before = after;
        track.strip.compEnabled.store (false);
        track.strip.compMode.store (1);
    } });
    steps->push_back ({ 300, [editor, before, &ctx, faceArea]
    {
        const auto after = editor->read();
        const auto face = changedIn (*before, after, faceArea);
        const auto counts = std::to_string (face[0]) + " of the face's " + std::to_string (face[1]) + " pixels";
        ctx.note ("the FET mode set in the session: " + counts + " changed");
        ctx.expect (face[0] * 3 >= face[1], "the FET mode set in the session redrew " + counts);
        *before = after;
    } });

    // From here the pointer works the face.
    click (*steps, editor, before, mc2::tabCentreX (2), mc2::kModeRowCentreY, [&ctx, &track, values, &comp]
    {
        ctx.expect (track.strip.compMode.load() == 2, "a click on the VCA tab left the strip's mode at "
                                                          + std::to_string (track.strip.compMode.load()));
        ctx.expect (track.strip.compModePicked.load(), "a click on the VCA tab did not mark the mode picked");
        ctx.expect (! track.strip.compEnabled.load(), "a click on a mode tab engaged the compressor");
        values->copyFrom (comp);
    });
    drag (*steps, editor, before, mc2::kVcaThresholdX, mc2::kVcaKnobY, 60.0f, [&ctx, &track, values, &comp]
    {
        expectOnly (ctx, CompStage::Strip, comp, *values, CompParamId::VcaThreshold, "a drag on THRESHOLD in the editor");
        ctx.expect (track.strip.compEnabled.load(), "a drag on THRESHOLD in the editor did not engage the compressor");
        values->copyFrom (comp);
    });
    click (*steps, editor, before, mc2::kBypassCentreX, mc2::kModeRowCentreY, [&ctx, &track, values, &comp]
    {
        ctx.expect (! track.strip.compEnabled.load(), "a click on BYPASS in the editor left the compressor engaged");
        for (const auto id : compStageParams (CompStage::Strip))
            ctx.expect (std::abs (comp.get (id) - values->get (id)) < 1.0e-6f,
                        std::string ("a click on BYPASS moved ") + compParamSpec (id).symbol);
    });
    runSteps (ctx, steps, [&host, &ctx]
    {
        host.closeCompEditor (Kind::Channel, kStripIndex);
        ctx.waitUntil ([&host] { return ! host.compEditorOpen (Kind::Channel, kStripIndex); }, kSettleBoundMs,
                       [&ctx] { ctx.complete (ctx.verdict()); }, "the strip compressor editor did not close");
    });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar compEditorStripFace { Scenario {
    "gui.comp_editor_strip_face", { "gui", "strip", "comp" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runCompEditorStripFace (host, ctx); }
} };

// A bus compressor's editor, the Bus face of a bus switched out, against the
// session both ways. Session to editor: a make-up change redraws MAKE-UP and
// leaves RATIO and MIX alone, and the status light redraws BYPASS. Editor to
// session: a drag on MAKE-UP moves that value alone and leaves the compressor
// out, as only the threshold engages a bus; a click on BYPASS switches it in.
std::optional<ScenarioResult> runCompEditorBusFace (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native UI");
   #else
    static constexpr int kBus = 0;
    if (! faceReadable (host)) return ScenarioResult::skip (kNoReadBack);
    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    if (! host.canEmbedPluginEditors())
        return ScenarioResult::skip ("requires a window the compressor editor can embed into");
    if (readyStrip (host, ctx) == nullptr) return ScenarioResult::fail ("channel strip is unavailable");

    auto& bus = ctx.session().bus (kBus).strip;
    auto& comp = bus.comp;
    keepComp (ctx, comp);
    ctx.keep (bus.compEnabled);
    ctx.cleanup ([&host]
    {
        host.closeCompEditor (Kind::Bus, kBus);
        drainModals (host);
    });
    bus.compEnabled.store (false);
    comp.set (CompParamId::BusMakeup, 0.0f);

    auto editor = std::make_shared<Editor> (Editor { host, ctx, Kind::Bus, kBus, "bus" });
    auto before = std::make_shared<Pixels>();
    auto values = std::make_shared<CompParams>();
    const auto makeup = around (mc2::kBusMakeupX, mc2::kBusRow1Y, mc2::kBusKnobRadius + 8.0f, "MAKE-UP");
    const auto ratio = around (mc2::kBusRatioX, mc2::kBusRow1Y, mc2::kBusKnobRadius + 8.0f, "RATIO");
    const auto mix = around (mc2::kBusMixX, mc2::kBusRow1Y, mc2::kBusKnobRadius + 8.0f, "MIX");
    const Area bypass { mc2::kBypassX0, mc2::kModeRowY0, mc2::kBypassX1, mc2::kModeRowY1, "BYPASS" };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host] { host.openCompEditor (Kind::Bus, kBus); } });
    steps->push_back ({ 0, [] {}, [&host] { return host.compEditorDrawn (Kind::Bus, kBus); },
                        "the bus compressor editor never drew a frame" });
    steps->push_back ({ 300, [editor, before, &comp]
    {
        *before = editor->read();
        comp.set (CompParamId::BusMakeup, 12.0f);
    } });
    steps->push_back ({ 300, [editor, before, &ctx, &bus, makeup, ratio, mix]
    {
        const auto after = editor->read();
        expectRedrawn (ctx, *before, after, "MAKE-UP set in the session", makeup, { ratio, mix });
        *before = after;
        bus.compEnabled.store (true);
    } });
    steps->push_back ({ 300, [editor, before, &ctx, &bus, &comp, values, bypass, ratio, mix]
    {
        const auto after = editor->read();
        expectRedrawn (ctx, *before, after, "the status light switching the compressor in", bypass, { ratio, mix });
        *before = after;
        bus.compEnabled.store (false);
        values->copyFrom (comp);
    } });
    drag (*steps, editor, before, mc2::kBusMakeupX, mc2::kBusRow1Y, -40.0f, [&ctx, &bus, values, &comp]
    {
        expectOnly (ctx, CompStage::Bus, comp, *values, CompParamId::BusMakeup, "a drag on MAKE-UP in the editor");
        ctx.expect (! bus.compEnabled.load(), "a drag on MAKE-UP in the editor engaged the compressor");
        values->copyFrom (comp);
    });
    click (*steps, editor, before, mc2::kBypassCentreX, mc2::kModeRowCentreY, [&ctx, &bus, values, &comp]
    {
        ctx.expect (bus.compEnabled.load(), "a click on BYPASS in the editor left the compressor out");
        for (const auto id : compStageParams (CompStage::Bus))
            ctx.expect (std::abs (comp.get (id) - values->get (id)) < 1.0e-6f,
                        std::string ("a click on BYPASS moved ") + compParamSpec (id).symbol);
    });
    runSteps (ctx, steps, [&host, &ctx]
    {
        host.closeCompEditor (Kind::Bus, kBus);
        ctx.waitUntil ([&host] { return ! host.compEditorOpen (Kind::Bus, kBus); }, kSettleBoundMs,
                       [&ctx] { ctx.complete (ctx.verdict()); }, "the bus compressor editor did not close");
    });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar compEditorBusFace { Scenario {
    "gui.comp_editor_bus_face", { "gui", "comp" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runCompEditorBusFace (host, ctx); }
} };

} // namespace
} // namespace duskstudio::scenario::gui
