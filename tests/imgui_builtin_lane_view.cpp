#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/builtin/NativeBuiltinSlot.h"
#include "ui/imgui/BuiltinLaneView.h"
#include "ui/imgui/DuskTheme.h"

#include <DuskWidgets.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// A built-in unit's inline aux-lane view drawn with no window, no GL and no compositor,
// at the lane sizes a user meets and at display scales 1 and 2. What is asserted is what
// a screenshot reader would check: every parameter has a control, the controls do not
// sit on top of each other, nothing is drawn outside the lane, and the controls answer
// the pointer.

namespace dw = DuskWidgets;
using namespace duskstudio;

namespace
{
class HeadlessLane
{
public:
    // The atlas is baked at the display scale, as DuskPanelWindow bakes it.
    explicit HeadlessLane (float displayScale) : scale (displayScale)
    {
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext (context);

        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2 (1600.0f * scale, 900.0f * scale);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.Fonts->Flags |= ImFontAtlasFlags_NoBakedLines;
        ImFontConfig config;
        config.SizePixels = 13.0f * scale;
        io.Fonts->AddFontDefault (&config);
        io.Fonts->Build();
        io.Fonts->SetTexID (static_cast<ImTextureID> (1));

        ImFont* const font = io.Fonts->Fonts.front();
        fonts.caption = fonts.label = fonts.pill = fonts.band = font;
        fonts.title = fonts.value = fonts.valueLarge = fonts.textEntry = font;
    }

    ~HeadlessLane() { ImGui::DestroyContext (context); }

    struct Frame
    {
        ImVec2 inkMin { 1.0e9f, 1.0e9f };
        ImVec2 inkMax { -1.0e9f, -1.0e9f };
        ImVec2 textMax {};
    };

    Frame frame (imgui::DuskPanelView& view, ImVec2 designSize)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos (ImVec2 (0.0f, 0.0f));
        ImGui::SetNextWindowSize (ImGui::GetIO().DisplaySize);
        ImGui::Begin ("##lane", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                      | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

        dw::Context ctx;
        ctx.dl = ImGui::GetWindowDrawList();
        ctx.theme = &imgui::consolePalette().widgets;
        ctx.fonts = &fonts;
        ctx.drag = &drag;
        ctx.scale = scale;

        const int before = ctx.dl->VtxBuffer.Size;
        view.draw (ctx, ImVec2 (0.0f, 0.0f), size (designSize));

        Frame out;
        const ImVec2 white = ImGui::GetIO().Fonts->TexUvWhitePixel;
        for (int i = before; i < ctx.dl->VtxBuffer.Size; ++i)
        {
            const auto& v = ctx.dl->VtxBuffer[i];
            out.inkMin = ImVec2 (std::min (out.inkMin.x, v.pos.x), std::min (out.inkMin.y, v.pos.y));
            out.inkMax = ImVec2 (std::max (out.inkMax.x, v.pos.x), std::max (out.inkMax.y, v.pos.y));
            if (v.uv.x < white.x || white.x < v.uv.x || v.uv.y < white.y || white.y < v.uv.y)
                out.textMax = ImVec2 (std::max (out.textMax.x, v.pos.x),
                                      std::max (out.textMax.y, v.pos.y));
        }

        ImGui::End();
        ImGui::Render();
        return out;
    }

    ImVec2 size (ImVec2 design) const { return ImVec2 (design.x * scale, design.y * scale); }

    void movePointer (ImVec2 to) { ImGui::GetIO().AddMousePosEvent (to.x, to.y); }
    void pressPointer (bool down) { ImGui::GetIO().AddMouseButtonEvent (ImGuiMouseButton_Left, down); }
    void wheel (float notches) { ImGui::GetIO().AddMouseWheelEvent (0.0f, notches); }

    const float scale;

private:
    ImGuiContext* context = nullptr;
    dw::Fonts fonts;
    dw::DragState drag;
};

// The effects the aux lane's picker lists.
const char* const kEffects[] = {
    "dusk.builtin.utility", "dusk.builtin.reverb", "dusk.builtin.delay", "dusk.builtin.tape",
};

// The effects and the instrument, which reaches this view only through its section
// fallback: twenty-six parameters is the worst case the layout meets.
const char* const kUnits[] = {
    "dusk.builtin.utility", "dusk.builtin.reverb", "dusk.builtin.delay",
    "dusk.builtin.tape", "dusk.builtin.synth",
};

// The lane's editor rectangle, in design pixels: at the smallest window Dusk Studio
// allows, on a 14-inch laptop's full window, and a lane smaller than the window can
// make it, where the layout has to shrink.
constexpr ImVec2 kCompactLane { 690.0f, 560.0f };
constexpr ImVec2 kRoomyLane { 1030.0f, 730.0f };
constexpr ImVec2 kCrampedLane { 560.0f, 340.0f };

std::vector<float> snapshot (const builtin::NativeBuiltinSlot& slot)
{
    std::vector<float> values;
    for (int i = 0; i < slot.paramCount(); ++i)
        values.push_back (slot.getParamValue (i));
    return values;
}

int indexOf (const builtin::NativeBuiltinSlot& slot, const char* id)
{
    for (int i = 0; i < slot.paramCount(); ++i)
        if (std::strcmp (slot.paramInfo (i)->id, id) == 0)
            return i;
    return -1;
}

const imgui::BuiltinLaneView::Placement* placementOf (const imgui::BuiltinLaneView& view,
                                                      int index)
{
    for (const auto& p : view.placements())
        if (p.paramIndex == index)
            return &p;
    return nullptr;
}

// Neighbouring cells share an edge, which float rounding can turn into a sliver of
// overlap; half a pixel is not a control drawn on top of another.
bool overlaps (const imgui::BuiltinLaneView::Placement& a,
               const imgui::BuiltinLaneView::Placement& b)
{
    constexpr float kSliver = 0.5f;
    return a.tl.x + kSliver < b.br.x && b.tl.x + kSliver < a.br.x
        && a.tl.y + kSliver < b.br.y && b.tl.y + kSliver < a.br.y;
}

// A knob cell's caption and readout sit either side of the dial, so the cell's centre
// is on it.
ImVec2 centreOf (const imgui::BuiltinLaneView::Placement& p)
{
    return ImVec2 ((p.tl.x + p.br.x) * 0.5f, (p.tl.y + p.br.y) * 0.5f);
}

// Two settling frames first: Dear ImGui decides what the pointer is over from the
// window it hovered on the previous frame.
void clickAt (HeadlessLane& lane, imgui::DuskPanelView& view, ImVec2 designSize, ImVec2 at)
{
    lane.movePointer (at);
    lane.frame (view, designSize);
    lane.frame (view, designSize);
    lane.pressPointer (true);
    lane.frame (view, designSize);
    lane.pressPointer (false);
    lane.frame (view, designSize);
}
} // namespace

TEST_CASE ("the aux lane view gives every parameter one control inside the lane",
           "[builtin][imgui][lane]")
{
    const float scale = GENERATE (1.0f, 2.0f);
    const ImVec2 lane = GENERATE (kCompactLane, kRoomyLane, kCrampedLane);

    for (const char* id : kUnits)
    {
        INFO ("unit " << id << " at scale " << scale << ", lane " << lane.x << "x" << lane.y);
        builtin::NativeBuiltinSlot slot;
        std::string error;
        REQUIRE (slot.loadUnit (id, 48000.0, 256, error));

        auto view = imgui::makeBuiltinLaneView (slot, {});
        REQUIRE_FALSE (view->wantsPlate());

        HeadlessLane headless (scale);
        const auto before = snapshot (slot);
        const auto drawn = headless.frame (*view, lane);
        REQUIRE (snapshot (slot) == before);

        const auto size = headless.size (lane);
        const float stroke = 1.0f * scale;
        REQUIRE (drawn.inkMin.x >= -stroke);
        REQUIRE (drawn.inkMin.y >= -stroke);
        REQUIRE (drawn.inkMax.x <= size.x + stroke);
        REQUIRE (drawn.inkMax.y <= size.y + stroke);

        const auto& placements = view->placements();
        REQUIRE (static_cast<int> (placements.size()) == slot.paramCount());
        for (int i = 0; i < slot.paramCount(); ++i)
        {
            INFO ("parameter " << slot.paramInfo (i)->id);
            REQUIRE (placementOf (*view, i) != nullptr);
        }
        for (std::size_t a = 0; a < placements.size(); ++a)
        {
            const auto& p = placements[a];
            INFO ("parameter " << slot.paramInfo (p.paramIndex)->id);
            REQUIRE (p.tl.x >= 0.0f);
            REQUIRE (p.tl.y >= 0.0f);
            REQUIRE (p.br.x <= size.x);
            REQUIRE (p.br.y <= size.y);
            for (std::size_t b = a + 1; b < placements.size(); ++b)
                REQUIRE_FALSE (overlaps (p, placements[b]));
        }
    }
}

TEST_CASE ("the aux lane view at a display scale of 2 is the same picture doubled",
           "[builtin][imgui][lane]")
{
    const ImVec2 lane = GENERATE (kCompactLane, kRoomyLane, kCrampedLane);
    for (const char* id : kUnits)
    {
        INFO ("unit " << id << ", lane " << lane.x << "x" << lane.y);
        builtin::NativeBuiltinSlot slot;
        std::string error;
        REQUIRE (slot.loadUnit (id, 48000.0, 256, error));
        auto view = imgui::makeBuiltinLaneView (slot, {});

        HeadlessLane one (1.0f);
        const auto atOne = one.frame (*view, lane);
        const auto placedAtOne = view->placements();
        HeadlessLane two (2.0f);
        const auto atTwo = two.frame (*view, lane);

        REQUIRE_THAT (atTwo.textMax.x, Catch::Matchers::WithinRel (atOne.textMax.x * 2.0f, 0.05f));
        REQUIRE_THAT (atTwo.textMax.y, Catch::Matchers::WithinRel (atOne.textMax.y * 2.0f, 0.05f));
        REQUIRE (view->placements().size() == placedAtOne.size());
        for (std::size_t i = 0; i < placedAtOne.size(); ++i)
        {
            REQUIRE_THAT (view->placements()[i].br.x,
                          Catch::Matchers::WithinAbs (placedAtOne[i].br.x * 2.0f, 1.0));
            REQUIRE_THAT (view->placements()[i].br.y,
                          Catch::Matchers::WithinAbs (placedAtOne[i].br.y * 2.0f, 1.0));
        }
    }
}

TEST_CASE ("the aux lane view's knobs stay full size down to the smallest window",
           "[builtin][imgui][lane]")
{
    // A knob cell is its caption, the dial and its readout; 78 design pixels is a dial of
    // radius 20 drawn unshrunk, the smallest this layout calls readable.
    const ImVec2 lane = GENERATE (kCompactLane, kRoomyLane);
    for (const char* id : kEffects)
    {
        INFO ("unit " << id << ", lane " << lane.x << "x" << lane.y);
        builtin::NativeBuiltinSlot slot;
        std::string error;
        REQUIRE (slot.loadUnit (id, 48000.0, 256, error));
        auto view = imgui::makeBuiltinLaneView (slot, {});

        HeadlessLane headless (1.0f);
        headless.frame (*view, lane);
        for (const auto& p : view->placements())
        {
            const auto kind = slot.paramInfo (p.paramIndex)->kind;
            if (kind != builtin::ParamKind::Continuous)
                continue;
            INFO ("parameter " << slot.paramInfo (p.paramIndex)->id);
            REQUIRE (p.br.y - p.tl.y >= 78.0f);
        }
    }
}

TEST_CASE ("the aux lane view's controls answer the pointer", "[builtin][imgui][lane]")
{
    const float scale = GENERATE (1.0f, 2.0f);
    std::string error;

    SECTION ("dragging a knob up raises its parameter")
    {
        builtin::NativeBuiltinSlot slot;
        REQUIRE (slot.loadUnit ("dusk.builtin.reverb", 48000.0, 256, error));
        int touched = -1;
        auto view = imgui::makeBuiltinLaneView (slot, [&touched] (int index) { touched = index; });
        HeadlessLane lane (scale);
        lane.frame (*view, kRoomyLane);

        const int mix = indexOf (slot, "mix");
        const auto* at = placementOf (*view, mix);
        REQUIRE (at != nullptr);
        const ImVec2 dial = centreOf (*at);

        const float before = slot.getParamValue (mix);
        lane.movePointer (dial);
        lane.frame (*view, kRoomyLane);
        lane.frame (*view, kRoomyLane);
        lane.pressPointer (true);
        lane.frame (*view, kRoomyLane);
        lane.movePointer (ImVec2 (dial.x, dial.y - 60.0f * scale));
        lane.frame (*view, kRoomyLane);
        lane.pressPointer (false);
        lane.frame (*view, kRoomyLane);

        REQUIRE (slot.getParamValue (mix) > before + 0.2f);
        REQUIRE (touched == mix);
    }

    SECTION ("clicking a toggle flips it")
    {
        builtin::NativeBuiltinSlot slot;
        REQUIRE (slot.loadUnit ("dusk.builtin.utility", 48000.0, 256, error));
        auto view = imgui::makeBuiltinLaneView (slot, {});
        HeadlessLane lane (scale);
        lane.frame (*view, kRoomyLane);

        const int polarity = indexOf (slot, "polarity");
        const auto* at = placementOf (*view, polarity);
        REQUIRE (at != nullptr);
        REQUIRE (slot.getParamValue (polarity) < 0.5f);
        clickAt (lane, *view, kRoomyLane, centreOf (*at));
        REQUIRE (slot.getParamValue (polarity) > 0.5f);
    }

    SECTION ("clicking a bank button selects that position")
    {
        builtin::NativeBuiltinSlot slot;
        REQUIRE (slot.loadUnit ("dusk.builtin.tape", 48000.0, 256, error));
        auto view = imgui::makeBuiltinLaneView (slot, {});
        HeadlessLane lane (scale);
        lane.frame (*view, kRoomyLane);

        // Swiss, American: the right-hand button, below the caption.
        const int machine = indexOf (slot, "machine");
        const auto* at = placementOf (*view, machine);
        REQUIRE (at != nullptr);
        REQUIRE (slot.getParamValue (machine) < 0.5f);
        clickAt (lane, *view, kRoomyLane,
                 ImVec2 (at->tl.x + (at->br.x - at->tl.x) * 0.75f, at->br.y - 6.0f * scale));
        REQUIRE_THAT (slot.getParamValue (machine), Catch::Matchers::WithinAbs (1.0, 1.0e-6));
    }

    SECTION ("the wheel steps a rotary switch one position")
    {
        builtin::NativeBuiltinSlot slot;
        REQUIRE (slot.loadUnit ("dusk.builtin.delay", 48000.0, 256, error));
        auto view = imgui::makeBuiltinLaneView (slot, {});
        HeadlessLane lane (scale);
        lane.frame (*view, kRoomyLane);

        const int mode = indexOf (slot, "mode");
        const auto* at = placementOf (*view, mode);
        REQUIRE (at != nullptr);
        const float before = slot.getParamValue (mode);

        lane.movePointer (centreOf (*at));
        lane.frame (*view, kRoomyLane);
        lane.frame (*view, kRoomyLane);
        lane.wheel (1.0f);
        lane.frame (*view, kRoomyLane);

        REQUIRE_THAT (slot.getParamValue (mode), Catch::Matchers::WithinAbs (before + 1.0f, 1.0e-6));
    }
}
