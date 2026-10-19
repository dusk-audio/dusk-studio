#include "PianoRollView.h"
#include "PanelControls.h"
#include "RulerDensity.h"
#include "../AppConfig.h"
#include "../PianoRollEdit.h"
#include "../WheelScroll.h"
#include "../../engine/AudioEngine.h"
#include "../../engine/Transport.h"
#include "../../session/RegionEditActions.h"
#include "../../session/Session.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <utility>

namespace duskstudio::imgui
{
namespace
{
namespace dw = DuskWidgets;
constexpr int kKeyboardWidth = 76;
constexpr int kHeaderHeight = 28;
constexpr int kNoteHeight = 16;
constexpr int kNumKeys = 128;
constexpr int kFullGridHeight = kNumKeys * kNoteHeight;
constexpr int kVelocityStripHMin = 56;
constexpr int kVelocityStripHMax = 320;
constexpr int kCcStripHMin = 48;
constexpr int kCcStripHMax = 320;
constexpr int kScrollBarH = 12;
constexpr std::uint32_t kDisabledInk = 0xff606068;

template <typename T>
T limited (T lo, T hi, T value) noexcept { return std::clamp (value, lo, std::max (lo, hi)); }

std::int64_t snapTick (std::int64_t tick, std::int64_t step)
{
    return step > 0 ? static_cast<std::int64_t> (std::llround (static_cast<double> (tick) / static_cast<double> (step))) * step : tick;
}

ImU32 argb (std::uint32_t colour, float alpha = 1.0f)
{
    return IM_COL32 ((colour >> 16) & 255, (colour >> 8) & 255, colour & 255,
                     static_cast<int> (static_cast<float> ((colour >> 24) & 255) * alpha));
}

struct Box
{
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool contains (ImVec2 p) const { return p.x >= x && p.x < right() && p.y >= y && p.y < bottom(); }
    bool intersects (const Box& b) const
    { return right() > b.x && x < b.right() && bottom() > b.y && y < b.bottom(); }
};

enum class Drag { none, move, resize, noteVelocity, velocity, cc, box, range, pan,
                  velocityHeight, ccHeight, loopIn, loopOut, punchIn, punchOut, dropped };
enum class Popup { none, quantize, scale, velocity, note, region, rename };
enum class Glyph { undo, redo, split, glue, quantize, properties, cc, zoomOut, zoomIn, zoomFit };

struct PaletteEntry { const char* label; std::uint32_t argb; };
constexpr PaletteEntry kPalette[] {
    { "Reset to track colour", 0 }, { "Red", 0xffd05f5f }, { "Orange", 0xffd09060 },
    { "Yellow", 0xffd0c060 }, { "Green", 0xff60c070 }, { "Cyan", 0xff60c0c0 },
    { "Blue", 0xff6090d0 }, { "Purple", 0xff9070c0 }, { "Magenta", 0xffc060a0 }
};
constexpr std::uint32_t kPitchColours[] {
    0xffe0464a, 0xffe07f2a, 0xffe0c050, 0xffb0d050, 0xff60c060, 0xff40c0a8,
    0xff50b0d0, 0xff5090e0, 0xff7068d8, 0xff9858c8, 0xffc060c0, 0xffe06090
};
constexpr const char* kGridNames[] { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "Off" };
constexpr std::int64_t kGridTicks[] { 1920, 960, 480, 240, 120, 60, 0 };
constexpr const char* kScaleNames[] { "Off", "Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian" };
constexpr const char* kRootNames[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

class PianoRollViewImpl final : public PianoRollView
{
public:
    PianoRollViewImpl (Session& s, AudioEngine& e, int track, int index, PianoRollHost h)
        : session (s), engine (e), trackIdx (track), regionIdx (index), host (std::move (h)),
          keySnap (s.pianoRollKeySnap), chase (appconfig::getFollowPlayheadDefault()) { rememberRegion(); }
    ~PianoRollViewImpl() override { cancelGesture(); }

    void setAvailableSize (float w, float h) override { available = { std::max (320.0f, w), std::max (240.0f, h) }; }
    ImVec2 preferredSize() const override { return available; }
    float dimAlpha() const override { return 0.80f; }
    bool escapeDismisses() const override { return false; }
    bool capturesKeyboard() const override { return ! covered() && (popupOpen || popupRequested != Popup::none); }
    bool takeDismissRequest() override { return std::exchange (dismissRequested, false); }
    bool claimsShortcut (ShellShortcut key) const override
    {
        if (covered() && host.keyboardClaimsShortcut && host.keyboardClaimsShortcut (key)) return true;
        return key == ShellShortcut::undo || key == ShellShortcut::redo || key == ShellShortcut::playheadToZero
            || key == ShellShortcut::setLoopIn || key == ShellShortcut::setLoopOut
            || key == ShellShortcut::setPunchIn || key == ShellShortcut::setPunchOut
            || key == ShellShortcut::toggleLoop || key == ShellShortcut::togglePunch;
    }
    int trackIndex() const override { return trackIdx; }
    int regionIndex() const override { return regionIdx; }
    bool chaseEnabled() const override { return chase; }
    void focusRegion (int index) override { cancelGesture(); showRegion (index); }
    void followTrack (int index) override { cancelGesture(); trackIdx = index; rememberRegion(); }
    void yieldDragToRecordCommit() override
    {
        const bool held = drag != Drag::none;
        cancelGesture();
        if (held) drag = Drag::dropped;
        popupRequested = Popup::none;
        cancelPopups = true;
        rememberRegion();
    }

    void followRegionEdits() override;
    void showCommittedRegion (int index) override;
    void showSwappedTake() override;
    void stepRecordNoteOn (int noteNumber, int velocity, int channel) override;
    void stepRecordNoteOff (int noteNumber, int channel) override;
    void stepRecordKeyboardClosed() noexcept override;

    std::array<double, 4> viewportForScenario() const override
    { return { pixelsPerTick, static_cast<double> (scrollX), static_cast<double> (scrollY), grid.w }; }
    std::array<int, 4> optionsForScenario() const override
    { return { static_cast<int> (scale), scaleRoot, activeCcController, colorMode }; }
    bool notePointForScenario (std::int64_t tick, int pitch, ImVec2& p) const override
    {
        if (! laidOut) return false;
        p = screen ({ xForTick (tick), yForNoteNumber (pitch) + kNoteHeight * 0.5f });
        return true;
    }
    std::array<double, 4> laneBoundsForScenario (bool cc) const override
    {
        const auto& box = cc ? ccArea : velocityArea;
        const auto point = screen ({ box.x, box.y });
        return { point.x, point.y, box.w * scaleFactor, box.h * scaleFactor };
    }
    std::array<double, 4> gridBoundsForScenario() const override
    {
        const auto p = screen ({ grid.x, grid.y });
        return { p.x, p.y, grid.w * scaleFactor, grid.h * scaleFactor };
    }
    ImVec2 bodyPointForScenario (ImVec2 p) const override { return local (p); }
    float scaleForScenario() const override { return scaleFactor; }
    std::array<int, 2> mergeStateForScenario() const override { return drawnMerge; }
    std::array<double, 4> controlBoundsForScenario (const std::string& name) const override
    {
        for (const auto& control : controls)
            if (name == control.name)
            {
                const auto p = screen ({ control.box.x, control.box.y });
                return { p.x, p.y, control.box.w * scaleFactor, control.box.h * scaleFactor };
            }
        return {};
    }
    bool popupForScenario() const override { return popupOpen || popupRequested != Popup::none; }
    std::vector<std::string> menuItemsForScenario() const override { return menuLabels; }
    bool wheelForScenario (float delta, bool command, bool shift, bool smooth, bool lane) override
    {
        if (! laidOut || covered() || popupForScenario()) return false;
        const auto& area = lane ? velocityArea : grid;
        wheelInputs.push_back ({ { area.x + area.w * 0.5f, area.y + area.h * 0.5f }, delta, 0, command, shift, smooth });
        return true;
    }
    std::vector<int> selectionForScenario() const override { return edit.selectedNotes; }
    std::int64_t editCursorForScenario() const override { return editCursorTick; }
    bool controlPointForScenario (const std::string& name, ImVec2& point, float position) const override
    {
        if (! laidOut) return false;
        if (name.rfind ("at:", 0) == 0)
        {
            char* end = nullptr;
            const float x = std::strtof (name.c_str() + 3, &end);
            if (end == nullptr || *end != ',') return false;
            point = screen ({ x, std::strtof (end + 1, nullptr) });
            return true;
        }
        for (const auto& control : controls)
            if (name == control.name && control.enabled)
            {
                point = screen ({ control.box.x + control.box.w * std::clamp (position, 0.0f, 1.0f),
                                  control.box.y + control.box.h * 0.5f });
                return true;
            }
        return false;
    }

    bool handleShellKey (const std::string& description, bool repeat) override
    {
        if (popupOpen) return false;
        const auto chord = parseKeyDescription (description);
        if (! chord) return false;
        if (repeat && ! repeats (chord->key)) return true;
        followRegionEdits();
        return handleKey (*chord);
    }

    void draw (dw::Context& ctx, ImVec2 at, ImVec2 size) override
    {
        const bool isCovered = covered();
        if (isCovered && ! wasCovered)
        {
            cancelGesture();
            popupRequested = Popup::none;
            cancelPopups = true;
        }
        wasCovered = isCovered;
        const bool popupWasOpen = popupOpen;
        origin = at;
        scaleFactor = std::max (0.01f, ctx.scale);
        width = std::max (1, static_cast<int> (size.x / scaleFactor));
        height = std::max (1, static_cast<int> (size.y / scaleFactor));
        layout();
        laidOut = true;
        controls.clear();
        menuLabels.clear();
        if (! isCovered) followRegionEdits();
        followPlayhead();
        const ScopedFormStyle style (ctx);
        fill (ctx, { 0, 0, static_cast<float> (width), static_cast<float> (height) }, 0xff181820);
        drawGrid (ctx);
        ImGui::BeginDisabled (isCovered);
        drawToolbar (ctx);
        if (! isCovered && covered())
        {
            ImGui::EndDisabled();
            return;
        }
        if (! isCovered)
        {
            handlePointer (popupWasOpen);
            handleWheel (popupWasOpen);
            if (! popupWasOpen)
                for (const auto& input : wheelInputs) applyWheel (input);
        }
        wheelInputs.clear();
        drawNotes (ctx);
        drawLanes (ctx);
        drawStatus (ctx);
        drawScroll (ctx);
        ImGui::EndDisabled();
        if ((isCovered || ! popupWasOpen) && ! ImGui::GetIO().WantTextInput && (dw::shortcutsAvailable (ctx) || gestureActive))
        {
            const auto& io = ImGui::GetIO();
            for (int code = ImGuiKey_NamedKey_BEGIN; code < ImGuiKey_GamepadStart; ++code)
            {
                const auto key = static_cast<ImGuiKey> (code);
                if (ImGui::IsKeyPressed (key, repeats (key)))
                   #if defined (__APPLE__)
                    dispatchKey ({ key, io.KeySuper, io.KeyCtrl, io.KeyShift, io.KeyAlt });
                   #else
                    dispatchKey ({ key, io.KeyCtrl, io.KeySuper, io.KeyShift, io.KeyAlt });
                   #endif
            }
        }
        drawPopups (ctx);
        popupOpen = popupRequested != Popup::none
            || ImGui::IsPopupOpen (nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        if (isCovered)
        {
            ctx.dl->AddRectFilled ({ 0, 0 }, { size.x + at.x * 2, size.y + at.y * 2 }, IM_COL32 (0, 0, 0, 140));
            if (ImGui::IsWindowHovered (ImGuiHoveredFlags_AllowWhenBlockedByPopup)
                && ImGui::IsMouseClicked (ImGuiMouseButton_Left) && host.dismissCover)
                host.dismissCover();
        }
        else drawCursor (ctx);
    }

private:
    Session& session;
    AudioEngine& engine;
    int trackIdx, regionIdx;
    PianoRollHost host;
    PianoRollEdit edit;
    ImVec2 available { 1120, 680 }, origin;
    float scaleFactor = 1.0f;
    int width = 1120, height = 680, statusHeight = 30, toolbarHeight = 48;
    bool laidOut = false, dismissRequested = false, popupOpen = false, gestureActive = false;
    float pixelsPerTick = 0.05f;
    int scrollX = 0, scrollY = (kNumKeys - 24) * kNoteHeight / 2;
    wheel::Accumulator wheelX, wheelY, wheelLane;
    struct WheelInput { ImVec2 point; float vertical, horizontal; bool command, shift, smooth; };
    std::vector<WheelInput> wheelInputs;
    bool wasCovered = false;
    std::array<int, 2> drawnMerge { -1, -1 };
    std::vector<std::string> menuLabels;
    bool covered() const { return host.covered && host.covered(); }
    std::int64_t snapTicks = 120, editCursorTick = 0;
    int velocityStripH = 110, ccStripH = 0, activeCcController = 1, colorMode = 0, noteEntryMode = 0;
    enum class Scale { Off, Major, Minor, Dorian, Phrygian, Lydian, Mixolydian, Locrian };
    Scale scale = Scale::Off;
    int scaleRoot = 0;
    bool keySnap = true, chase = false;
    std::array<int, kNumKeys * 16> stepRecordHeld {};
    bool stepRecordChordHad = false, stepRecordReopened = false;
    std::int64_t stepRecordAt = 0;
    Box grid, rulerArea, velocityArea, ccArea, rubberBand;
    struct Control { std::string name; Box box; bool enabled; };
    std::vector<Control> controls;
    Drag drag = Drag::none;
    ImVec2 dragStart, dragLast;
    int dragButton = ImGuiMouseButton_Left, draggedCcIdx = -1, dragOriginNoteNum = 0;
    int panStartScrollX = 0, panStartScrollY = 0, resizeStartStripH = 0;
    std::int64_t dragOriginTick = 0, dragNoteStartTick = 0;
    bool rangeActive = false, noteEditActive = false;
    std::int64_t rangeStartTick = 0, rangeEndTick = 0;
    MidiRegion noteEditBefore;
    std::uint32_t noteEditRevision = 0;
    struct RegionAnchor { std::int64_t start = 0, end = 0, startTick = 0; bool valid = false; };
    std::uint32_t seenRegionEdits = 0;
    int shownRegionCount = 0;
    RegionAnchor shown, leftForTake;
    std::vector<MidiNote> shownNotes;
    std::vector<MidiCc> shownCcs;
    std::int64_t shownTicks = 0;
    Popup popupRequested = Popup::none;
    std::uint32_t popupRevision = 0;
    int popupRegion = -1;
    bool cancelPopups = false, renameFocus = false;
    std::vector<char> nameBuffer;
    std::mt19937 random { std::random_device{}() };

    MidiRegion* region();
    const MidiRegion* region() const;
    void rememberRegion();
    int regionOverlapping (const RegionAnchor&) const;
    void showRegion (int index);
    bool isInScale (int noteNumber) const noexcept;
    int snapPitchToScale (int noteNumber) const noexcept;
    int nextRegionIndex (int delta) const;
    void splitSelectedAtCursor();
    void zoomFit();
    void zoomByFactor (float factor);
    void clearSelection() { edit.clearSelection(); }

    ImVec2 screen (ImVec2 p) const { return { origin.x + p.x * scaleFactor, origin.y + p.y * scaleFactor }; }
    ImVec2 local (ImVec2 p) const { return { (p.x - origin.x) / scaleFactor, (p.y - origin.y) / scaleFactor }; }
    void fill (dw::Context& ctx, Box b, std::uint32_t colour, float alpha = 1.0f) const
    { ctx.dl->AddRectFilled (screen ({ b.x, b.y }), screen ({ b.right(), b.bottom() }), argb (colour, alpha)); }
    void line (dw::Context& ctx, ImVec2 a, ImVec2 b, std::uint32_t colour, float thickness = 1.0f) const
    { ctx.dl->AddLine (screen (a), screen (b), argb (colour), ctx.s (thickness)); }
    void text (dw::Context& ctx, Box b, const std::string& value, std::uint32_t colour = 0xffb0b0b8, float fontSize = 12) const
    {
        if (b.w <= 0 || b.h <= 0) return;
        ctx.dl->PushClipRect (screen ({ b.x, b.y }), screen ({ b.right(), b.bottom() }), true);
        dw::text (ctx, ctx.fonts->band, ctx.s (fontSize), screen ({ b.x, b.y + (b.h - fontSize) * 0.5f }),
                  ctx.s (b.w), argb (colour), value.c_str(), dw::Align::left);
        ctx.dl->PopClipRect();
    }
    void remember (std::string name, Box b, bool enabled = true) { controls.push_back ({ std::move (name), b, enabled }); }
    void rememberMenu (const std::string& name, bool enabled = true)
    {
        const auto a = local (ImGui::GetItemRectMin()), b = local (ImGui::GetItemRectMax());
        remember (name, { a.x, a.y, b.x - a.x, b.y - a.y }, enabled);
    }
    float xForTick (std::int64_t tick) const
    { return static_cast<float> (kKeyboardWidth - static_cast<double> (scrollX) + static_cast<double> (tick) * pixelsPerTick); }
    float yForNoteNumber (int note) const
    { return static_cast<float> (toolbarHeight + kHeaderHeight + (127 - note) * kNoteHeight - scrollY); }
    int noteNumberForY (float y) const
    { return std::clamp (127 - static_cast<int> ((y - toolbarHeight - kHeaderHeight + static_cast<float> (scrollY)) / kNoteHeight), 0, 127); }
    std::int64_t tickForX (float x) const
    { return std::max<std::int64_t> (0, static_cast<std::int64_t> (std::llround ((x - kKeyboardWidth + static_cast<double> (scrollX)) / pixelsPerTick))); }
    std::int64_t effectiveSnapTicks() const { return session.midiEditorSnap ? snapTicks : 0; }
    bool frozen() const { return trackIdx < 0 || trackIdx >= Session::kNumTracks || session.track (trackIdx).frozen.load (std::memory_order_relaxed); }
    bool editable() const { const auto* r = region(); return r != nullptr && ! frozen() && ! r->locked; }

    std::array<Box, 15> toolbarBoxes() const
    {
        constexpr float widths[] { 28, 28, 28, 28, 28, 28, 34, 34, 42, 90, 52, 28, 28, 28, 28 };
        std::array<Box, 15> boxes;
        float x = 8, y = 10;
        for (std::size_t i = 0; i < boxes.size(); ++i)
        {
            if (x > 8 && x + widths[i] > width - 8) { x = 8; y += 38; }
            boxes[i] = { x, y, widths[i], 28 };
            x += widths[i] + (i == 6 || i == 7 || i == 8 ? 4 : i == 9 || i == 10 ? 8 : 6);
        }
        return boxes;
    }
    void layout()
    {
        toolbarHeight = static_cast<int> (toolbarBoxes().back().bottom()) + 10;
        statusHeight = width < 920 ? 56 : 30;
        const float bottom = static_cast<float> (height - statusHeight - kScrollBarH);
        const float top = static_cast<float> (toolbarHeight + kHeaderHeight);
        const float laneRoom = std::max (0.0f, bottom - top - 40.0f);
        const float requested = static_cast<float> (velocityStripH + ccStripH);
        const float shrink = requested > 0 ? std::min (1.0f, laneRoom / requested) : 1.0f;
        const float cc = static_cast<float> (ccStripH) * shrink, vel = static_cast<float> (velocityStripH) * shrink;
        const float w = std::max (1.0f, static_cast<float> (width - kKeyboardWidth));
        rulerArea = { kKeyboardWidth, static_cast<float> (toolbarHeight), w, kHeaderHeight };
        ccArea = { kKeyboardWidth, bottom - cc, w, cc };
        velocityArea = { kKeyboardWidth, bottom - cc - vel, w, vel };
        grid = { kKeyboardWidth, top, w, std::max (1.0f, velocityArea.y - top) };
        scrollY = std::clamp (scrollY, 0, std::max (0, kFullGridHeight - static_cast<int> (grid.h)));
    }

    void commit (const MidiRegion& before, const MidiRegion& after, const char* name)
    {
        auto& undo = engine.getUndoManager();
        undo.beginNewTransaction (name);
        undo.perform (new MidiRegionEditAction (session, engine, trackIdx, regionIdx, before, after));
        rememberRegion();
    }
    void pushNoteEdit (const MidiRegion& before, const char* name)
    {
        const auto* r = region();
        if (r == nullptr || (r->notes == before.notes && r->ccs == before.ccs
            && r->lengthInTicks == before.lengthInTicks && r->lengthInSamples == before.lengthInSamples)) return;
        const MidiRegion after = *r;
        commit (before, after, name);
    }
    template <typename Change>
    void changeNotes (const char* name, Change&& change)
    {
        auto* r = region();
        if (! editable()) return;
        const MidiRegion before = *r;
        change (*r);
        pushNoteEdit (before, name);
    }
    template <typename Change>
    void changeRegion (const char* name, Change&& change)
    {
        const auto* r = region();
        if (r == nullptr || frozen()) return;
        const MidiRegion before = *r;
        MidiRegion after = before;
        change (after);
        commit (before, after, name);
    }
    void beginNoteEdit()
    {
        if (const auto* r = region())
        {
            noteEditBefore = *r;
            noteEditRevision = session.track (trackIdx).midiRegions.edits();
            noteEditActive = true;
        }
    }
    void cancelGesture()
    {
        if (noteEditActive && trackIdx >= 0 && trackIdx < Session::kNumTracks
            && session.track (trackIdx).midiRegions.edits() == noteEditRevision)
            if (auto* r = region()) *r = noteEditBefore;
        noteEditActive = false;
        drag = Drag::none;
        edit.dragAnchor = -1;
        edit.dragSnapshots.clear();
        draggedCcIdx = -1;
        rubberBand = {};
    }
    void undoOrRedo (bool redo)
    {
        cancelGesture();
        clearSelection();
        rangeActive = false;
        if (host.undo) host.undo (redo);
        else if (redo) engine.getUndoManager().redo();
        else engine.getUndoManager().undo();
    }
    void showPopup (Popup which)
    {
        popupRequested = which;
        popupRegion = regionIdx;
        popupRevision = trackIdx >= 0 && trackIdx < Session::kNumTracks ? session.track (trackIdx).midiRegions.edits() : 0;
    }
    void quantize (std::int64_t ticks, float strength)
    { changeNotes ("Quantize notes", [&] (MidiRegion& r) { edit.quantizeSelected (r.notes, ticks, strength); }); }
    void setVelocity (int value)
    { changeNotes ("Set velocity", [&] (MidiRegion& r) { edit.setVelocityFor (r.notes, value); }); }
    void humanize (int percent)
    {
        changeNotes ("Humanise velocity", [&] (MidiRegion& r)
        {
            edit.humanizeVelocity (r.notes, percent, [this] (int lo, int hi)
            { return std::uniform_int_distribution<int> (lo, hi - 1) (random); });
        });
    }
    void glue()
    { changeNotes ("Glue notes", [&] (MidiRegion& r) { edit.glueSelectedNotes (r.notes); }); }
    void navigate (int delta)
    {
        const int index = nextRegionIndex (delta);
        if (index >= 0)
        {
            if (host.navigateToRegion) host.navigateToRegion (trackIdx, index);
            else focusRegion (index);
        }
    }
    static bool repeats (ImGuiKey key)
    {
        return key == ImGuiKey_LeftArrow || key == ImGuiKey_RightArrow || key == ImGuiKey_UpArrow
            || key == ImGuiKey_DownArrow || key == ImGuiKey_Equal || key == ImGuiKey_Minus;
    }
    bool dispatchKey (const KeyChord& key)
    {
        if (covered() && host.keyboardOwnsKey && host.keyboardOwnsKey (key))
            return host.unhandledKey && host.unhandledKey (key);
        if (covered() && (key.key == ImGuiKey_Escape || key.key == ImGuiKey_K))
            return host.unhandledKey && host.unhandledKey (key);
        if (handleKey (key)) return true;
        return host.unhandledKey && ! shellShortcutFor (key) && host.unhandledKey (key);
    }
    bool handleKey (const KeyChord& k)
    {
        const bool command = k.command(), bare = ! command && ! k.shift && ! k.alt;
        if (drag != Drag::none)
        {
            if (k.key == ImGuiKey_Escape) cancelGesture();
            return true;
        }
        if (k.key == ImGuiKey_Escape)
        {
            if (rangeActive) { rangeActive = false; clearSelection(); }
            else if (! edit.selectedNotes.empty()) clearSelection();
            else dismissRequested = true;
            return true;
        }
        if (bare && (k.key == ImGuiKey_G || k.key == ImGuiKey_D))
        { session.editMode = k.key == ImGuiKey_G ? EditMode::Grab : EditMode::Draw; return true; }
        if (command)
        {
            if (k.key == ImGuiKey_Z || k.key == ImGuiKey_Y)
            { undoOrRedo (k.key == ImGuiKey_Y || k.shift); return true; }
            if (k.key == ImGuiKey_LeftBracket || k.key == ImGuiKey_RightBracket)
            { navigate (k.key == ImGuiKey_RightBracket ? 1 : -1); return true; }
            if (k.key == ImGuiKey_L)
            { auto& t = engine.getTransport(); t.setLoopEnabled (! t.isLoopEnabled()); return true; }
            if (k.key == ImGuiKey_E) { splitSelectedAtCursor(); return true; }
            if (k.key == ImGuiKey_LeftArrow || k.key == ImGuiKey_RightArrow)
            {
                const auto* r = region();
                const auto maxScroll = r != nullptr ? std::max (0.0, static_cast<double> (r->lengthInTicks) * pixelsPerTick - grid.w) : 0.0;
                const int limit = static_cast<int> (std::min (maxScroll, static_cast<double> (std::numeric_limits<int>::max())));
                scrollX = std::clamp (scrollX + (k.key == ImGuiKey_LeftArrow ? -1 : 1) * std::max (1, static_cast<int> (grid.w / 4)), 0, limit);
                return true;
            }
            if (k.key == ImGuiKey_A)
            {
                clearSelection(); rangeActive = false;
                if (const auto* r = region()) for (int i = 0; i < static_cast<int> (r->notes.size()); ++i) edit.addToSelection (i);
                return true;
            }
            if (k.key == ImGuiKey_C)
            { if (const auto* r = region(); r != nullptr && ! edit.selectedNotes.empty()) edit.copySelected (r->notes); return true; }
            if (k.key == ImGuiKey_X)
            {
                if (! edit.selectedNotes.empty())
                    changeNotes ("Cut notes", [&] (MidiRegion& r) { edit.copySelected (r.notes); edit.deleteSelected (r.notes); });
                return true;
            }
            if (k.key == ImGuiKey_V)
            {
                if (! PianoRollEdit::clipboard.empty())
                    changeNotes ("Paste notes", [&] (MidiRegion& r) { edit.paste (r.notes, r.lengthInTicks, editCursorTick); });
                return true;
            }
            if (k.key == ImGuiKey_D)
            { changeNotes ("Duplicate notes", [&] (MidiRegion& r) { edit.duplicateSelectedNotes (r.notes, kMidiTicksPerQuarter); }); return true; }
            if (k.key == ImGuiKey_G) { glue(); return true; }
            return false;
        }
        if (! k.alt && (k.key == ImGuiKey_LeftBracket || k.key == ImGuiKey_RightBracket || (k.key == ImGuiKey_P && ! k.shift)))
        {
            auto& t = engine.getTransport();
            if (k.key == ImGuiKey_P) { t.setPunchEnabled (! t.isPunchEnabled()); return true; }
            if (const auto* r = region())
            {
                const auto sr = engine.getTimelineSampleRate();
                const auto sample = session.ticksToSamples (session.samplesToTicks (r->timelineStart, sr) + editCursorTick, sr);
                if (k.key == ImGuiKey_LeftBracket)
                {
                    if (k.shift) t.placePunchRange (sample, std::max (sample, t.getPunchOut()));
                    else t.placeLoopRange (sample, std::max (sample, t.getLoopEnd()));
                }
                else
                {
                    if (k.shift) t.placePunchRange (std::min (sample, t.getPunchIn()), sample);
                    else t.placeLoopRange (std::min (sample, t.getLoopStart()), sample);
                }
            }
            return true;
        }
        if (bare && k.key == ImGuiKey_Home) { scrollX = 0; return true; }
        if (bare && k.key == ImGuiKey_End)
        {
            if (const auto* r = region()) scrollX = static_cast<int> (std::clamp (static_cast<double> (r->lengthInTicks) * pixelsPerTick - grid.w,
                    0.0, static_cast<double> (std::numeric_limits<int>::max())));
            return true;
        }
        if (bare && (k.key == ImGuiKey_Delete || k.key == ImGuiKey_Backspace))
        { changeNotes ("Delete notes", [&] (MidiRegion& r) { edit.deleteSelected (r.notes); }); return true; }
        if (! k.alt && (k.key == ImGuiKey_UpArrow || k.key == ImGuiKey_DownArrow))
        {
            changeNotes ("Transpose notes", [&] (MidiRegion& r)
            { edit.transposeSelected (r.notes, (k.key == ImGuiKey_UpArrow ? 1 : -1) * (k.shift ? 12 : 1)); });
            return true;
        }
        if (! k.alt && (k.key == ImGuiKey_LeftArrow || k.key == ImGuiKey_RightArrow))
        {
            const auto step = k.shift ? kMidiTicksPerQuarter : (effectiveSnapTicks() > 0 ? effectiveSnapTicks() : 30);
            changeNotes ("Nudge notes", [&] (MidiRegion& r)
            { edit.nudgeSelectedTicks (r.notes, r.lengthInTicks, k.key == ImGuiKey_LeftArrow ? -step : step); });
            return true;
        }
        if (! k.alt && (k.key == ImGuiKey_Equal || k.key == ImGuiKey_KeypadAdd)) { zoomByFactor (1.15f); return true; }
        if (bare && (k.key == ImGuiKey_Minus || k.key == ImGuiKey_KeypadSubtract)) { zoomByFactor (1.0f / 1.15f); return true; }
        if (k.shift && ! k.alt && k.key == ImGuiKey_G) { glue(); return true; }
        if (! bare) return false;
        if (k.key >= ImGuiKey_1 && k.key <= ImGuiKey_6)
        { snapTicks = kGridTicks[k.key - ImGuiKey_1]; return true; }
        if (k.key == ImGuiKey_0) { snapTicks = 0; return true; }
        if (k.key == ImGuiKey_C) { colorMode = (colorMode + 1) % 3; return true; }
        if (k.key == ImGuiKey_Q) { showPopup (Popup::quantize); return true; }
        if (k.key == ImGuiKey_S) { showPopup (Popup::scale); return true; }
        if (k.key == ImGuiKey_V) { showPopup (Popup::velocity); return true; }
        if (k.key == ImGuiKey_L)
        {
            constexpr int controllers[] { 1, 7, 11, 64, 74 };
            const auto* found = std::find (std::begin (controllers), std::end (controllers), activeCcController);
            activeCcController = found == std::end (controllers) || found + 1 == std::end (controllers) ? 1 : *(found + 1);
            return true;
        }
        return false;
    }

    int hitNote (ImVec2 p, bool& edge) const
    {
        const auto* r = region();
        if (r == nullptr || ! grid.contains (p)) return -1;
        for (int i = static_cast<int> (r->notes.size()); --i >= 0;)
        {
            const auto& n = r->notes[static_cast<std::size_t> (i)];
            const float end = xForTick (n.startTick + n.lengthInTicks), y = yForNoteNumber (n.noteNumber);
            if (p.x >= xForTick (n.startTick) && p.x <= end && p.y >= y && p.y < y + kNoteHeight)
            { edge = end - p.x <= 5; return i; }
        }
        return -1;
    }
    int hitBar (float x, bool cc) const
    {
        const auto* r = region();
        if (r == nullptr) return -1;
        if (! cc)
        {
            for (int i = static_cast<int> (r->notes.size()); --i >= 0;)
            {
                const auto& note = r->notes[static_cast<std::size_t> (i)];
                if (x >= xForTick (note.startTick) && x <= xForTick (note.startTick + note.lengthInTicks)) return i;
            }
        }
        const auto count = cc ? r->ccs.size() : r->notes.size();
        float closest = cc ? 6.0f : 18.0f;
        int best = -1;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (cc && (r->ccs[i].kind != MidiCc::Kind::ControlChange || r->ccs[i].controller != activeCcController)) continue;
            const float delta = std::abs (x - xForTick (cc ? r->ccs[i].atTick : r->notes[i].startTick));
            if (delta <= closest) { closest = delta; best = static_cast<int> (i); }
        }
        return best;
    }
    const MidiCapturePreview* recordingPreview (std::int64_t& offset) const
    {
        const auto* r = region();
        auto& recorder = engine.getRecordManager();
        if (r == nullptr || ! recorder.isActive()) return nullptr;
        const auto* preview = recorder.midiCapturePreview (trackIdx);
        if (preview == nullptr) return nullptr;
        const auto sr = engine.getTimelineSampleRate();
        offset = session.samplesToTicks (recorder.midiCaptureTimelineStart(), sr) - session.samplesToTicks (r->timelineStart, sr);
        return preview;
    }
    Box laneBar (std::int64_t tick, int value, const Box& area) const
    {
        const float h = static_cast<float> (value) / 127 * area.h;
        return { xForTick (tick) - 4, area.bottom() - h, 8, std::max (h, 3.0f) };
    }
    bool hitsRecordingBar (ImVec2 p, bool cc) const
    {
        std::int64_t offset = 0;
        const auto* preview = recordingPreview (offset);
        if (preview == nullptr) return false;
        if (cc)
        {
            for (const auto& control : preview->ccs)
                if (control.kind == MidiCc::Kind::ControlChange && control.controller == activeCcController
                    && laneBar (control.atTick + offset, control.value, ccArea).contains (p)) return true;
        }
        else
            for (const auto& note : preview->notes)
                if (laneBar (note.startTick + offset, note.velocity, velocityArea).contains (p)) return true;
        return false;
    }
    int valueAt (float y, const Box& b, int minimum) const
    { return std::clamp (static_cast<int> (std::lround ((b.bottom() - y) * 127.0f / std::max (1.0f, b.h))), minimum, 127); }
    void pointerDown (ImVec2 p, int button)
    {
        auto* r = region();
        if (r == nullptr) return;
        const auto& io = ImGui::GetIO();
        dragStart = dragLast = p;
        dragButton = button;
        if (button == ImGuiMouseButton_Middle && grid.contains (p))
        { drag = Drag::pan; panStartScrollX = scrollX; panStartScrollY = scrollY; return; }
        bool edge = false;
        const int hit = hitNote (p, edge);
        if (button == ImGuiMouseButton_Right)
        {
            if (hit >= 0) { if (! edit.isNoteSelected (hit)) edit.selectOnly (hit); showPopup (Popup::note); }
            return;
        }
        if (button != ImGuiMouseButton_Left) return;
        if (rulerArea.contains (p))
        {
            auto& t = engine.getTransport();
            const auto sr = engine.getTimelineSampleRate(), startTick = static_cast<double> (session.samplesToTicks (r->timelineStart, sr));
            struct Edge { std::int64_t sample; Drag mode; bool valid; };
            const Edge edges[] {
                { t.getLoopStart(), Drag::loopIn, t.getLoopEnd() > t.getLoopStart() },
                { t.getLoopEnd(), Drag::loopOut, t.getLoopEnd() > t.getLoopStart() },
                { t.getPunchIn(), Drag::punchIn, t.getPunchOut() > t.getPunchIn() },
                { t.getPunchOut(), Drag::punchOut, t.getPunchOut() > t.getPunchIn() }
            };
            for (const auto& bracket : edges)
                if (bracket.valid && std::abs (xForTick (session.samplesToTicks (bracket.sample, sr) - static_cast<std::int64_t> (startTick)) - p.x) <= 6)
                { drag = bracket.mode; return; }
            editCursorTick = tickForX (p.x);
            t.locate (session.ticksToSamples (static_cast<std::int64_t> (startTick) + editCursorTick, sr));
            rangeStartTick = rangeEndTick = editCursorTick;
            rangeActive = false; clearSelection(); drag = Drag::range; return;
        }
        if (p.x >= kKeyboardWidth && p.y >= velocityArea.y - 5 && p.y < velocityArea.y)
        { drag = Drag::velocityHeight; resizeStartStripH = velocityStripH; return; }
        if (p.x >= kKeyboardWidth && p.y >= ccArea.y - 5 && p.y < ccArea.y)
        { drag = Drag::ccHeight; resizeStartStripH = ccStripH; return; }
        if (ccArea.h > 0 && ccArea.contains (p) && editable())
        {
            if (hitsRecordingBar (p, true)) return;
            beginNoteEdit();
            draggedCcIdx = hitBar (p.x, true);
            if (draggedCcIdx < 0)
            {
                MidiCc cc;
                cc.channel = 1; cc.controller = activeCcController;
                cc.atTick = limited<std::int64_t> (0, r->lengthInTicks - 1, snapTick (tickForX (p.x), effectiveSnapTicks()));
                r->ccs.push_back (cc);
                draggedCcIdx = static_cast<int> (r->ccs.size()) - 1;
            }
            r->ccs[static_cast<std::size_t> (draggedCcIdx)].value = valueAt (p.y, ccArea, 0);
            drag = Drag::cc; return;
        }
        if (velocityArea.contains (p) && editable())
        {
            if (hitsRecordingBar (p, false)) return;
            const int index = hitBar (p.x, false);
            if (index >= 0)
            {
                beginNoteEdit(); edit.selectOnly (index); edit.dragAnchor = index;
                r->notes[static_cast<std::size_t> (index)].velocity = valueAt (p.y, velocityArea, 1);
                drag = Drag::velocity;
            }
            return;
        }
        const bool extend = io.KeyShift || io.KeyCtrl || io.KeySuper;
        if (hit >= 0)
        {
            if (extend) { edit.toggleSelected (hit); return; }
            if (! edit.isNoteSelected (hit)) edit.selectOnly (hit);
            const auto& note = r->notes[static_cast<std::size_t> (hit)];
            editCursorTick = note.startTick;
            if (! editable()) return;
            beginNoteEdit(); edit.beginGroupDrag (&r->notes, hit);
            dragOriginTick = tickForX (p.x) - note.startTick;
            dragOriginNoteNum = note.noteNumber; dragNoteStartTick = note.startTick;
            drag = edge ? Drag::resize : io.KeyAlt ? Drag::noteVelocity : Drag::move;
            return;
        }
        if (! grid.contains (p)) return;
        editCursorTick = limited<std::int64_t> (0, r->lengthInTicks, snapTick (tickForX (p.x), effectiveSnapTicks()));
        if (extend || session.editMode == EditMode::Grab)
        { if (! extend) clearSelection(); rubberBand = { p.x, p.y, 0, 0 }; drag = Drag::box; return; }
        if (session.editMode == EditMode::Range)
        { clearSelection(); rangeStartTick = rangeEndTick = tickForX (p.x); rangeActive = false; drag = Drag::range; return; }
        if (session.editMode != EditMode::Draw || ! editable()) return;
        auto step = effectiveSnapTicks();
        if (noteEntryMode == 1) step = 0;
        if (noteEntryMode == 2) step = step * 2 / 3;
        if (noteEntryMode == 3) step = step * 3 / 2;
        MidiNote note;
        note.noteNumber = snapPitchToScale (noteNumberForY (p.y));
        const auto raw = limited<std::int64_t> (0, r->lengthInTicks - 1, tickForX (p.x));
        note.startTick = limited<std::int64_t> (0, r->lengthInTicks - 1, snapTick (raw, step));
        note.lengthInTicks = std::min<std::int64_t> (step > 0 ? step : kMidiTicksPerQuarter, r->lengthInTicks - note.startTick);
        if (note.lengthInTicks <= 0) return;
        beginNoteEdit(); r->notes.push_back (note);
        edit.selectOnly (static_cast<int> (r->notes.size()) - 1); edit.dragAnchor = edit.selectedNotes.front();
        drag = Drag::resize;
    }

    void pointerDrag (ImVec2 p)
    {
        auto* r = region();
        if (r == nullptr) return;
        if (drag == Drag::pan)
        {
            scrollX = std::max (0, panStartScrollX - static_cast<int> (p.x - dragStart.x));
            scrollY = std::clamp (panStartScrollY - static_cast<int> (p.y - dragStart.y), 0,
                                 std::max (0, kFullGridHeight - static_cast<int> (grid.h)));
            return;
        }
        if (drag == Drag::velocityHeight || drag == Drag::ccHeight)
        {
            const int value = resizeStartStripH + static_cast<int> (dragStart.y - p.y);
            const int top = toolbarHeight + kHeaderHeight + statusHeight + kScrollBarH + 80;
            if (drag == Drag::velocityHeight) velocityStripH = limited (kVelocityStripHMin, std::min (320, height - top - ccStripH), value);
            else ccStripH = limited (kCcStripHMin, std::min (320, height - top - velocityStripH), value);
            return;
        }
        if (drag == Drag::box)
        { rubberBand = { std::min (p.x, dragStart.x), std::min (p.y, dragStart.y), std::abs (p.x - dragStart.x), std::abs (p.y - dragStart.y) }; return; }
        if (drag == Drag::range)
        {
            rangeEndTick = tickForX (p.x); rangeActive = rangeEndTick != rangeStartTick; clearSelection();
            const auto a = std::min (rangeStartTick, rangeEndTick), b = std::max (rangeStartTick, rangeEndTick);
            for (int i = 0; i < static_cast<int> (r->notes.size()); ++i)
                if (r->notes[static_cast<std::size_t> (i)].startTick >= a && r->notes[static_cast<std::size_t> (i)].startTick < b) edit.addToSelection (i);
            return;
        }
        if (drag == Drag::loopIn || drag == Drag::loopOut || drag == Drag::punchIn || drag == Drag::punchOut)
        {
            const auto sr = engine.getTimelineSampleRate();
            const auto sample = session.ticksToSamples (session.samplesToTicks (r->timelineStart, sr) + tickForX (p.x), sr);
            auto& t = engine.getTransport();
            if (drag == Drag::loopIn) t.setLoopRange (std::min (sample, t.getLoopEnd()), t.getLoopEnd());
            if (drag == Drag::loopOut) t.setLoopRange (t.getLoopStart(), std::max (sample, t.getLoopStart()));
            if (drag == Drag::punchIn) t.setPunchRange (std::min (sample, t.getPunchOut()), t.getPunchOut());
            if (drag == Drag::punchOut) t.setPunchRange (t.getPunchIn(), std::max (sample, t.getPunchIn()));
            return;
        }
        if (drag == Drag::cc)
        {
            if (draggedCcIdx >= 0 && draggedCcIdx < static_cast<int> (r->ccs.size()))
                r->ccs[static_cast<std::size_t> (draggedCcIdx)].value = valueAt (p.y, ccArea, 0);
            return;
        }
        if (edit.dragAnchor < 0 || edit.dragAnchor >= static_cast<int> (r->notes.size())) return;
        auto& anchor = r->notes[static_cast<std::size_t> (edit.dragAnchor)];
        if (drag == Drag::move)
            edit.applyGroupMove (r->notes, r->lengthInTicks, snapTick (tickForX (p.x) - dragOriginTick, effectiveSnapTicks()) - dragNoteStartTick,
                                 snapPitchToScale (noteNumberForY (p.y)) - dragOriginNoteNum);
        if (drag == Drag::resize)
            anchor.lengthInTicks = limited<std::int64_t> (anchor.startTick + 1, r->lengthInTicks,
                snapTick (tickForX (p.x), effectiveSnapTicks())) - anchor.startTick;
        if (drag == Drag::velocity) anchor.velocity = valueAt (p.y, velocityArea, 1);
        if (drag == Drag::noteVelocity)
            for (std::size_t i = 0; i < edit.selectedNotes.size() && i < edit.dragSnapshots.size(); ++i)
            {
                const auto index = static_cast<std::size_t> (edit.selectedNotes[i]);
                if (index < r->notes.size()) r->notes[index].velocity = std::clamp (edit.dragSnapshots[i].velocity
                    + static_cast<int> (std::lround ((dragStart.y - p.y) * 0.5f)), 1, 127);
            }
    }
    void pointerUp()
    {
        if (drag == Drag::box)
            if (const auto* r = region(); r != nullptr && rubberBand.w > 0 && rubberBand.h > 0)
                for (int i = 0; i < static_cast<int> (r->notes.size()); ++i)
                {
                    const auto& n = r->notes[static_cast<std::size_t> (i)];
                    const Box box { xForTick (n.startTick), yForNoteNumber (n.noteNumber) + 1,
                        std::max (2.0f, xForTick (n.startTick + n.lengthInTicks) - xForTick (n.startTick)), kNoteHeight - 2 };
                    if (rubberBand.intersects (box)) edit.addToSelection (i);
                }
        if (noteEditActive)
        {
            noteEditActive = false;
            const char* name = drag == Drag::move ? "Move notes" : drag == Drag::resize ? "Resize note"
                : drag == Drag::cc ? "Edit CC" : "Edit velocity";
            if (session.track (trackIdx).midiRegions.edits() == noteEditRevision) pushNoteEdit (noteEditBefore, name);
        }
        drag = Drag::none; rubberBand = {}; draggedCcIdx = -1; edit.dragAnchor = -1; edit.dragSnapshots.clear();
    }
    void handlePointer (bool popupWasOpen)
    {
        const Box surface { 0, static_cast<float> (toolbarHeight), static_cast<float> (width), std::max (1.0f, ccArea.bottom() - toolbarHeight) };
        ImGui::SetCursorScreenPos (screen ({ surface.x, surface.y }));
        ImGui::InvisibleButton ("##piano-gesture", { surface.w * scaleFactor, surface.h * scaleFactor },
            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        gestureActive = ImGui::IsItemActive();
        const auto& io = ImGui::GetIO();
        if (ImGui::IsItemActivated() && ! popupWasOpen)
            for (const int button : { ImGuiMouseButton_Left, ImGuiMouseButton_Right, ImGuiMouseButton_Middle })
                if (io.MouseClicked[button]) { pointerDown (local (io.MousePos), button); break; }
        if (noteEditActive && session.track (trackIdx).midiRegions.edits() != noteEditRevision)
        { noteEditActive = false; drag = Drag::dropped; }
        if (drag != Drag::none)
        {
            if (ImGui::IsMousePosValid())
            {
                const auto p = local (io.MousePos);
                if (p.x != dragLast.x || p.y != dragLast.y) { pointerDrag (p); dragLast = p; }
            }
            if (! io.MouseDown[dragButton]) pointerUp();
        }
        if (! popupWasOpen && surface.contains (local (io.MousePos)) && ImGui::IsWindowHovered())
        {
            bool edge = false;
            const auto p = local (io.MousePos);
            if (drag == Drag::pan) ImGui::SetMouseCursor (ImGuiMouseCursor_ResizeAll);
            else if (drag == Drag::resize || (hitNote (p, edge) >= 0 && edge)) ImGui::SetMouseCursor (ImGuiMouseCursor_ResizeEW);
            else if (p.x >= kKeyboardWidth && (std::abs (p.y - velocityArea.y) <= 5 || std::abs (p.y - ccArea.y) <= 5))
                ImGui::SetMouseCursor (ImGuiMouseCursor_ResizeNS);
            else if (grid.contains (p)) ImGui::SetMouseCursor (ImGuiMouseCursor_Arrow);
        }
    }

    void drawCursor (dw::Context& ctx)
    {
        const auto& io = ImGui::GetIO();
        const auto point = local (io.MousePos);
        bool edge = false;
        if (popupOpen || ! ImGui::IsMousePosValid() || ! ImGui::IsWindowHovered() || ! grid.contains (point)
            || drag == Drag::resize || drag == Drag::pan || (hitNote (point, edge) >= 0 && edge)) return;
        if (session.editMode != EditMode::Grab && session.editMode != EditMode::Draw) return;
        ImGui::SetMouseCursor (ImGuiMouseCursor_None);
        const auto at = io.MousePos;
        if (session.editMode == EditMode::Grab)
        {
            struct Part { float x, y, w, h, rounding; };
            constexpr Part parts[] { { 6, 11, 12, 10, 3 }, { 7, 2, 3.2f, 13, 1.6f },
                { 11, 9, 3, 4, 1.4f }, { 14.5f, 9.5f, 3, 4, 1.4f }, { 4, 13, 3, 6, 1.4f } };
            const float size = 0.85f * ctx.scale;
            for (const float grow : { 1.7f, 0.0f })
                for (const auto& part : parts)
                {
                    const float x = part.x - 8.6f, y = part.y - 2;
                    ctx.dl->AddRectFilled ({ at.x + (x - grow) * size, at.y + (y - grow) * size },
                        { at.x + (x + part.w + grow) * size, at.y + (y + part.h + grow) * size },
                        grow > 0 ? IM_COL32 (0, 0, 0, 191) : IM_COL32_WHITE, (part.rounding + grow) * size);
                }
        }
        else
        {
            const float size = 0.85f * ctx.scale * 34 / 24;
            const auto p = [at, size] (float t, float w)
            { return ImVec2 (at.x + (0.70710678f * t + 0.70710678f * w) * size,
                             at.y + (-0.70710678f * t + 0.70710678f * w) * size); };
            const ImVec2 outline[] { p (0, 0), p (5.8f, -3), p (21, -3), p (21, 3), p (5.8f, 3) };
            ctx.dl->AddPolyline (outline, 5, IM_COL32 (0, 0, 0, 217), ImDrawFlags_Closed, ctx.s (2.55f));
            ctx.dl->AddConvexPolyFilled (outline, 5, IM_COL32_WHITE);
            ctx.dl->AddTriangleFilled (p (0, 0), p (2.4f, -1.2f), p (2.4f, 1.2f), IM_COL32 (0, 0, 0, 217));
            for (const float t : { 5.8f, 16.0f, 18.6f })
                ctx.dl->AddLine (p (t, -3), p (t, 3), IM_COL32 (0, 0, 0, 217), ctx.s (1.1f));
        }
    }
    void applyWheel (const WheelInput& input)
    {
        const auto p = input.point;
        if (velocityArea.contains (p) || (ccArea.h > 0 && ccArea.contains (p)))
        {
            const int delta = wheelLane.pixels (input.vertical, input.smooth, 8);
            if (velocityArea.contains (p)) velocityStripH = limited (kVelocityStripHMin,
                std::min (kVelocityStripHMax, height - toolbarHeight - kHeaderHeight - ccStripH - statusHeight - kScrollBarH - 80), velocityStripH + delta);
            else ccStripH = limited (kCcStripHMin,
                std::min (kCcStripHMax, height - toolbarHeight - kHeaderHeight - velocityStripH - statusHeight - kScrollBarH - 80), ccStripH + delta);
        }
        else if (input.command)
        {
            const auto tick = tickForX (p.x);
            const auto* r = region();
            const float minimum = r != nullptr && r->lengthInTicks > 0
                ? std::min (0.005f, grid.w / (float) r->lengthInTicks) : 0.005f;
            pixelsPerTick = std::clamp (pixelsPerTick * wheel::zoomFactor (input.vertical, input.smooth, 1.15f),
                                       std::min (pixelsPerTick, minimum), 1.0f);
            scrollX = static_cast<int> (std::clamp (static_cast<double> (tick) * pixelsPerTick - (p.x - kKeyboardWidth),
                       0.0, static_cast<double> (std::numeric_limits<int>::max())));
        }
        else if (input.horizontal != 0 || input.shift)
            scrollX = std::max (0, scrollX + wheelX.pixels (-(input.horizontal != 0 ? input.horizontal : input.vertical), input.smooth, 24));
        else scrollY = std::clamp (scrollY + wheelY.pixels (-input.vertical, input.smooth, kNoteHeight), 0,
                                  std::max (0, kFullGridHeight - static_cast<int> (grid.h)));
    }
    void handleWheel (bool popupWasOpen)
    {
        const auto& io = ImGui::GetIO();
        const auto p = local (io.MousePos);
        if (popupWasOpen || ! ImGui::IsWindowHovered() || p.x < 0 || p.x > width || p.y < toolbarHeight
            || p.y >= height - statusHeight || (io.MouseWheel == 0 && io.MouseWheelH == 0)) return;
        const auto notch = wheel::profile().lineNotchDelta;
        applyWheel ({ p, io.MouseWheel * notch, io.MouseWheelH * notch, io.KeyCtrl || io.KeySuper, io.KeyShift, false });
    }

    ImU32 noteColour (const MidiNote& note) const
    {
        const float brightness = 0.55f + 0.45f * static_cast<float> (note.velocity) / 127.0f;
        ImVec4 rgba = ImGui::ColorConvertU32ToFloat4 (argb (colorMode == 0 ? kPitchColours[std::clamp (note.noteNumber, 0, 127) % 12] : 0xff70b0e0));
        if (colorMode == 2) ImGui::ColorConvertHSVtoRGB (static_cast<float> (std::clamp (note.channel, 1, 16) - 1) / 16, 0.65f, 1, rgba.x, rgba.y, rgba.z);
        rgba.x *= brightness; rgba.y *= brightness; rgba.z *= brightness;
        return ImGui::ColorConvertFloat4ToU32 (rgba);
    }
    void drawGrid (dw::Context& ctx)
    {
        fill (ctx, { 0, static_cast<float> (toolbarHeight), static_cast<float> (width), kHeaderHeight }, 0xff202028);
        ctx.dl->PushClipRect (screen ({ 0, grid.y }), screen ({ static_cast<float> (width), grid.bottom() }), true);
        for (int note = 0; note < 128; ++note)
        {
            const float y = yForNoteNumber (note);
            if (y + kNoteHeight < grid.y || y >= grid.bottom()) continue;
            const int pitch = note % 12;
            const bool black = pitch == 1 || pitch == 3 || pitch == 6 || pitch == 8 || pitch == 10;
            fill (ctx, { 0, y, kKeyboardWidth, kNoteHeight }, black ? 0xff242428 : 0xffe0e0e6);
            fill (ctx, { grid.x, y, grid.w, kNoteHeight }, black ? 0xff141418 : 0xff1c1c24);
            if (! isInScale (note)) fill (ctx, { grid.x, y, grid.w, kNoteHeight }, 0xff000000, 0.30f);
            line (ctx, { 0, y + kNoteHeight }, { static_cast<float> (width), y + kNoteHeight }, note % 12 == 0 ? 0xff55555f : 0xff2a2a32);
            if (note % 12 == 0) text (ctx, { 44, y, 30, kNoteHeight }, "C" + std::to_string (note / 12 - 1), 0xff404048, 11);
        }
        ctx.dl->PopClipRect();
        const auto* r = region();
        if (r == nullptr) { text (ctx, grid, "MIDI region unavailable"); return; }
        ctx.dl->PushClipRect (screen ({ grid.x, rulerArea.y }), screen ({ grid.right(), grid.bottom() }), true);
        const auto ticksPerBar = static_cast<std::int64_t> (kMidiTicksPerQuarter) * std::max (1, session.beatsPerBar.load (std::memory_order_relaxed));
        const auto first = tickForX (grid.x), last = tickForX (grid.right());
        const auto bars = (last - first) / ticksPerBar + 2;
        const auto step = ruler::barStep (static_cast<double> (ticksPerBar) * pixelsPerTick, bars);
        const bool barMode = static_cast<TimeDisplayMode> (session.timeDisplayMode.load (std::memory_order_relaxed)) == TimeDisplayMode::Bars;
        for (auto bar = (first / ticksPerBar / step) * step; bar <= last / ticksPerBar + 1; bar += step)
        {
            const float x = xForTick (bar * ticksPerBar);
            line (ctx, { x, rulerArea.y }, { x, grid.bottom() }, 0xff5a5a64);
            if (barMode) text (ctx, { x + 3, rulerArea.y, 60, rulerArea.h }, std::to_string (bar + 1));
        }
        const auto beatStep = pixelsPerTick * kMidiTicksPerQuarter >= 9 ? kMidiTicksPerQuarter : ticksPerBar * step;
        for (auto tick = first / beatStep * beatStep; tick <= last; tick += beatStep)
        {
            const float x = xForTick (tick);
            if (tick % ticksPerBar != 0) line (ctx, { x, rulerArea.bottom() - 9 }, { x, grid.bottom() }, 0xff3c3c46);
        }
        const auto snap = effectiveSnapTicks();
        if (snap > 0 && pixelsPerTick * static_cast<float> (snap) >= 6)
            for (auto tick = first / snap * snap; tick <= last; tick += snap)
                line (ctx, { xForTick (tick), grid.y }, { xForTick (tick), grid.bottom() }, 0xff2a2a32);
        if (! barMode)
        {
            const auto sr = engine.getTimelineSampleRate();
            const auto startTick = session.samplesToTicks (r->timelineStart, sr);
            const double from = static_cast<double> (session.ticksToSamples (startTick + first, sr)) / sr;
            const double to = static_cast<double> (session.ticksToSamples (startTick + last, sr)) / sr;
            const double spacing = ruler::stampSeconds (grid.w / std::max (0.0001, to - from), to - from);
            for (double sec = std::floor (from / spacing) * spacing; sec <= to; sec += spacing)
            {
                const auto tick = session.samplesToTicks (static_cast<std::int64_t> (std::llround (sec * sr)), sr) - startTick;
                const float x = xForTick (tick);
                char stamp[40]; std::snprintf (stamp, sizeof (stamp), "%lld:%02d", static_cast<long long> (sec / 60), static_cast<int> (sec) % 60);
                text (ctx, { x + 3, rulerArea.y, 64, rulerArea.h }, stamp);
                line (ctx, { x, rulerArea.y }, { x, rulerArea.bottom() }, 0xff5a5a64);
            }
        }
        const float end = xForTick (r->lengthInTicks);
        if (end < grid.right()) fill (ctx, { std::max (end, grid.x), grid.y, grid.right() - std::max (end, grid.x), grid.h }, 0xff000000, 0.35f);
        if (rangeActive)
        {
            const float a = xForTick (std::min (rangeStartTick, rangeEndTick)), b = xForTick (std::max (rangeStartTick, rangeEndTick));
            fill (ctx, { a, grid.y, b - a, grid.h }, 0xffffd060, 0.14f);
        }
        ctx.dl->PopClipRect();
        remember ("grid", grid);
    }
    void drawNotes (dw::Context& ctx)
    {
        const auto* r = region();
        if (r == nullptr) return;
        ctx.dl->PushClipRect (screen ({ grid.x, grid.y }), screen ({ grid.right(), grid.bottom() }), true);
        const auto note = [&] (const MidiNote& n, bool selected, bool preview)
        {
            const Box box { xForTick (n.startTick), yForNoteNumber (n.noteNumber) + 1,
                std::max (2.0f, xForTick (n.startTick + n.lengthInTicks) - xForTick (n.startTick)), kNoteHeight - 2 };
            if (! box.intersects (grid)) return;
            ctx.dl->AddRectFilled (screen ({ box.x, box.y }), screen ({ box.right(), box.bottom() }),
                preview ? argb (0xffffd060, 0.65f) : selected ? argb (0xffffd060) : noteColour (n));
            ctx.dl->AddRect (screen ({ box.x, box.y }), screen ({ box.right(), box.bottom() }), argb (0xff141418));
        };
        for (int i = 0; i < static_cast<int> (r->notes.size()); ++i) note (r->notes[static_cast<std::size_t> (i)], edit.isNoteSelected (i), false);
        std::int64_t offset = 0;
        if (const auto* preview = recordingPreview (offset))
            for (auto n : preview->notes) { n.startTick += offset; note (n, false, true); }
        line (ctx, { xForTick (editCursorTick), grid.y }, { xForTick (editCursorTick), grid.bottom() }, 0xffffd060);
        const auto sr = engine.getTimelineSampleRate();
        const auto playhead = engine.getTransport().getPlayhead();
        const double tick = session.samplesToTicksFractional (playhead, sr) - session.samplesToTicksFractional (r->timelineStart, sr);
        if (playhead >= r->timelineStart && (tick <= r->lengthInTicks || engine.getRecordManager().isActive()))
        {
            const float x = static_cast<float> (kKeyboardWidth - scrollX + tick * pixelsPerTick);
            line (ctx, { x, grid.y }, { x, grid.bottom() }, 0xffffe8c0);
        }
        if (drag == Drag::box && rubberBand.w > 0 && rubberBand.h > 0)
        {
            fill (ctx, rubberBand, 0xffffd060, 0.18f);
            ctx.dl->AddRect (screen ({ rubberBand.x, rubberBand.y }), screen ({ rubberBand.right(), rubberBand.bottom() }), argb (0xffffd060));
        }
        ctx.dl->PopClipRect();
        drawBrackets (ctx);
    }
    void drawBrackets (dw::Context& ctx)
    {
        const auto* r = region();
        if (r == nullptr) return;
        const auto sr = engine.getTimelineSampleRate();
        const auto start = session.samplesToTicks (r->timelineStart, sr);
        auto& t = engine.getTransport();
        ctx.dl->PushClipRect (screen ({ rulerArea.x, rulerArea.y }), screen ({ grid.right(), grid.bottom() }), true);
        const auto bracket = [&] (std::int64_t a, std::int64_t b, bool enabled, std::uint32_t colour, float y)
        {
            if (b <= a) return;
            const float x0 = xForTick (session.samplesToTicks (a, sr) - start), x1 = xForTick (session.samplesToTicks (b, sr) - start);
            if (enabled) fill (ctx, { x0, grid.y, x1 - x0, grid.h }, colour, 0.07f);
            line (ctx, { x0, y }, { x1, y }, colour, 2);
            line (ctx, { x0, y }, { x0, rulerArea.bottom() }, colour, 2);
            line (ctx, { x1, y }, { x1, rulerArea.bottom() }, colour, 2);
        };
        bracket (t.getLoopStart(), t.getLoopEnd(), t.isLoopEnabled(), 0xff3aa860, rulerArea.y + 3);
        bracket (t.getPunchIn(), t.getPunchOut(), t.isPunchEnabled(), 0xffd05a5a, rulerArea.y + 8);
        ctx.dl->PopClipRect();
    }
    void drawLanes (dw::Context& ctx)
    {
        const auto* r = region();
        for (const bool cc : { false, true })
        {
            const auto area = cc ? ccArea : velocityArea;
            if (area.h <= 0) continue;
            fill (ctx, area, 0xff121218);
            fill (ctx, { area.x, area.y - 5, area.w, 5 }, 0xff30303c);
            text (ctx, { 4, area.y, kKeyboardWidth - 8, 20 }, cc ? "CC " + std::to_string (activeCcController) : "Velocity");
            remember (cc ? "cc-strip" : "velocity-strip", area);
            if (r == nullptr) continue;
            ctx.dl->PushClipRect (screen ({ area.x, area.y }), screen ({ area.right(), area.bottom() }), true);
            const auto bar = [&] (std::int64_t tick, int value, ImU32 colour)
            {
                const auto bounds = laneBar (tick, value, area);
                if (! bounds.intersects (area)) return;
                ctx.dl->AddRectFilled (screen ({ bounds.x + 1, bounds.y }), screen ({ bounds.right() - 1, area.bottom() }), colour);
                ctx.dl->AddRectFilled (screen ({ bounds.x, bounds.y }), screen ({ bounds.right(), bounds.y + 3 }), colour);
            };
            const auto count = cc ? r->ccs.size() : r->notes.size();
            for (std::size_t i = 0; i < count; ++i)
            {
                if (cc && (r->ccs[i].kind != MidiCc::Kind::ControlChange || r->ccs[i].controller != activeCcController)) continue;
                const int value = cc ? r->ccs[i].value : r->notes[i].velocity;
                const auto colour = cc ? argb (0xff70b0e0) : edit.isNoteSelected (static_cast<int> (i)) ? argb (0xffffd060) : noteColour (r->notes[i]);
                bar (cc ? r->ccs[i].atTick : r->notes[i].startTick, value, colour);
            }
            std::int64_t offset = 0;
            if (const auto* preview = recordingPreview (offset))
            {
                const auto colour = argb (0xffffd060, 0.65f);
                if (cc)
                {
                    for (const auto& control : preview->ccs)
                        if (control.kind == MidiCc::Kind::ControlChange && control.controller == activeCcController)
                            bar (control.atTick + offset, control.value, colour);
                }
                else
                    for (const auto& note : preview->notes) bar (note.startTick + offset, note.velocity, colour);
            }
            ctx.dl->PopClipRect();
        }
    }

    bool button (dw::Context& ctx, const char* id, Box b, const char* label, bool on = false, bool enabled = true)
    {
        ImGui::BeginDisabled (! enabled);
        dw::ButtonStyle style; style.fontSize = 11;
        if (! enabled) { style.offText = argb (kDisabledInk); style.onFill = argb (kDisabledInk); }
        const bool clicked = dw::textButton (ctx, id, screen ({ b.x, b.y }), screen ({ b.right(), b.bottom() }), label, on, style).clicked;
        ImGui::EndDisabled();
        remember (id, b, enabled);
        return enabled && clicked;
    }
    bool icon (dw::Context& ctx, const char* id, Box b, Glyph glyph, const char* tooltip, bool enabled = true)
    {
        ImGui::BeginDisabled (! enabled);
        dw::hitArea (ctx, id, screen ({ b.x, b.y }), screen ({ b.right(), b.bottom() }));
        const bool click = ImGui::IsItemClicked();
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetTooltip ("%s", tooltip);
        ImGui::EndDisabled();
        remember (id, b, enabled);
        const auto centre = screen ({ b.x + b.w * 0.5f, b.y + b.h * 0.5f });
        ctx.dl->AddCircleFilled (centre, ctx.s (13), argb (hovered ? 0xff353544 : 0xff262630));
        ctx.dl->AddCircle (centre, ctx.s (13), argb (0xff3a3a44));
        const auto colour = argb (enabled ? 0xffd0d0d0 : kDisabledInk);
        const auto p = [&] (float x, float y) { return ImVec2 (centre.x + ctx.s (x), centre.y + ctx.s (y)); };
        const auto stroke = [&] (float x, float y, float x1, float y1) { ctx.dl->AddLine (p (x, y), p (x1, y1), colour, ctx.s (1.5f)); };
        if (glyph == Glyph::undo || glyph == Glyph::redo)
        {
            const float sign = glyph == Glyph::undo ? 1.0f : -1.0f;
            ctx.dl->PathArcTo (p (0, 1), ctx.s (6), -2.8f, 1.5f, 18); ctx.dl->PathStroke (colour, 0, ctx.s (1.5f));
            stroke (-6 * sign, -6, -6 * sign, 0); stroke (-6 * sign, 0, 0, -1);
        }
        else if (glyph == Glyph::zoomIn || glyph == Glyph::zoomOut)
        {
            ctx.dl->AddCircle (p (-2, -2), ctx.s (5), colour, 16, ctx.s (1.5f)); stroke (2, 2, 7, 7); stroke (-5, -2, 1, -2);
            if (glyph == Glyph::zoomIn) stroke (-2, -5, -2, 1);
        }
        else if (glyph == Glyph::zoomFit)
        {
            for (const int sign : { -1, 1 }) { stroke (-7, 6.0f * sign, 7, 6.0f * sign); stroke (7.0f * sign, -6, 7.0f * sign, 6); }
            stroke (-3, 0, 3, 0);
        }
        else if (glyph == Glyph::split) { stroke (-6, -5, 6, 5); stroke (6, -5, -6, 5); stroke (0, -8, 0, 8); }
        else if (glyph == Glyph::glue) { stroke (-7, -5, -2, -5); stroke (-2, -5, -2, 5); stroke (2, -5, 2, 5); stroke (2, 5, 7, 5); stroke (-7, 0, 7, 0); }
        else text (ctx, { b.x + 5, b.y + 3, b.w - 8, b.h - 6 }, glyph == Glyph::cc ? "CC" : glyph == Glyph::quantize ? "Q" : "i", enabled ? 0xffd0d0d0 : kDisabledInk, 12);
        return enabled && click;
    }
    void drawToolbar (dw::Context& ctx)
    {
        fill (ctx, { 0, 0, static_cast<float> (width), static_cast<float> (toolbarHeight) }, 0xff20202c);
        const auto boxes = toolbarBoxes();
        std::size_t index = 0;
        const auto next = [&] { return boxes[index++]; };
        if (icon (ctx, "undo", next(), Glyph::undo, "Undo", engine.getUndoManager().canUndo())) undoOrRedo (false);
        if (icon (ctx, "redo", next(), Glyph::redo, "Redo", engine.getUndoManager().canRedo())) undoOrRedo (true);
        if (icon (ctx, "split", next(), Glyph::split, "Split at edit cursor", editable())) splitSelectedAtCursor();
        if (icon (ctx, "glue", next(), Glyph::glue, "Glue selection", editable() && edit.selectedNotes.size() >= 2)) glue();
        if (icon (ctx, "quantize", next(), Glyph::quantize, "Quantize...", editable())) showPopup (Popup::quantize);
        if (icon (ctx, "properties", next(), Glyph::properties, "Region properties...", region() != nullptr)) showPopup (Popup::region);
        if (button (ctx, "grab", next(), "Grab", session.editMode == EditMode::Grab)) session.editMode = EditMode::Grab;
        if (button (ctx, "draw", next(), "Draw", session.editMode == EditMode::Draw)) session.editMode = EditMode::Draw;
        if (button (ctx, "snap", next(), "Snap", session.midiEditorSnap)) session.midiEditorSnap = ! session.midiEditorSnap;
        const bool merge = session.midiMergeEnabled.load (std::memory_order_relaxed);
        const bool mergeEnabled = ! engine.getRecordManager().isActive() && ! covered();
        const auto mergeBox = next();
        drawnMerge = mergeBox.right() <= width ? std::array<int, 2> { merge ? 1 : 0, mergeEnabled ? 1 : 0 }
                                               : std::array<int, 2> { -1, -1 };
        if (button (ctx, "midi-merge", mergeBox, "MIDI Merge", merge, mergeEnabled))
            session.midiMergeEnabled.store (! session.midiMergeEnabled.load (std::memory_order_relaxed), std::memory_order_relaxed);
        if (button (ctx, "chase", next(), "Chase", chase)) chase = ! chase;
        if (icon (ctx, "toggle-cc", next(), Glyph::cc, "Show / hide CC lane")) ccStripH = ccStripH > 0 ? 0 : 100;
        if (icon (ctx, "zoom-out", next(), Glyph::zoomOut, "Zoom out (-)")) zoomByFactor (1 / 1.15f);
        if (icon (ctx, "zoom-in", next(), Glyph::zoomIn, "Zoom in (=)")) zoomByFactor (1.15f);
        if (icon (ctx, "zoom-fit", next(), Glyph::zoomFit, "Zoom fit")) zoomFit();
    }

    void drawStatus (dw::Context& ctx)
    {
        const float top = static_cast<float> (height - statusHeight);
        const auto* r = region();
        const auto sr = engine.getTimelineSampleRate();
        const auto position = r != nullptr ? session.ticksToSamples (session.samplesToTicks (r->timelineStart, sr) + editCursorTick, sr) : 0;
        const auto mode = static_cast<TimeDisplayMode> (session.timeDisplayMode.load (std::memory_order_relaxed));
        text (ctx, { 8, top, 108, 28 }, "pos " + formatSamplePosition (position, sr, session.tempoMap,
            session.tempoBpm.load (std::memory_order_relaxed), session.beatsPerBar.load (std::memory_order_relaxed), mode));
        int velocity = -1;
        if (r != nullptr && ! edit.selectedNotes.empty() && edit.selectedNotes.front() < static_cast<int> (r->notes.size()))
            velocity = r->notes[static_cast<std::size_t> (edit.selectedNotes.front())].velocity;
        text (ctx, { 120, top, 64, 28 }, velocity < 0 ? "Vel: --" : "Vel: " + std::to_string (velocity));
        const float y = statusHeight > 30 ? top + 28 : top;
        float x = statusHeight > 30 ? 8.0f : 190.0f;
        const auto combo = [&] (const char* id, float w, const char* const* labels, int count, int& selected)
        {
            const Box b { x, y + 3, w, 22 }; x += w + 6;
            const bool changed = formCombo (ctx, id, screen ({ b.x, b.y }), screen ({ b.right(), b.bottom() }), labels, count, selected);
            remember (id, b); return changed;
        };
        int gridIndex = 6;
        for (int i = 0; i < 7; ++i) if (snapTicks == kGridTicks[i]) gridIndex = i;
        if (combo ("grid", 72, kGridNames, 7, gridIndex)) snapTicks = kGridTicks[gridIndex];
        constexpr const char* entries[] { "Grid", "Free", "Triplet", "Dotted" };
        constexpr const char* colours[] { "Pitch", "Velocity", "Channel" };
        combo ("notes", 82, entries, 4, noteEntryMode);
        combo ("color", 88, colours, 3, colorMode);
        if (button (ctx, "key-snap", { x, y + 3, 78, 22 }, "Key snap", keySnap)) { keySnap = ! keySnap; session.pianoRollKeySnap = keySnap; }
        x += 84;
        if (button (ctx, "mute", { x, y + 3, 44, 22 }, "Mute", r != nullptr && r->muted, r != nullptr && ! frozen()))
            changeRegion (r->muted ? "Unmute MIDI region" : "Mute MIDI region", [] (MidiRegion& a) { a.muted = ! a.muted; });
        x += 50;
        r = region();
        if (button (ctx, "lock", { x, y + 3, 44, 22 }, "Lock", r != nullptr && r->locked, r != nullptr && ! frozen()))
            changeRegion (r->locked ? "Unlock MIDI region" : "Lock MIDI region", [] (MidiRegion& a) { a.locked = ! a.locked; });
        const float captionX = statusHeight > 30 ? 190.0f : x + 54;
        if (trackIdx >= 0 && trackIdx < Session::kNumTracks)
        {
            const auto& track = session.track (trackIdx);
            const auto name = track.name.toStdString();
            const auto caption = name == std::to_string (trackIdx + 1) ? "Track " + name : name;
            const int channel = region() != nullptr && ! region()->notes.empty() ? region()->notes.front().channel : 1;
            text (ctx, { captionX, top, std::max (0.0f, width - captionX - 8), 28 }, caption + "  -  ch " + std::to_string (channel), track.colour.getARGB());
        }
    }
    void drawScroll (dw::Context& ctx)
    {
        const auto* r = region();
        const double regionPixels = r != nullptr ? static_cast<double> (r->lengthInTicks) * pixelsPerTick : 0;
        const double total = std::max (regionPixels, static_cast<double> (scrollX) + grid.w);
        const Box bar { kKeyboardWidth, ccArea.bottom(), grid.w, kScrollBarH };
        fill (ctx, bar, 0xff101018);
        const float thumbWidth = std::clamp (static_cast<float> (grid.w * grid.w / std::max (1.0, total)), 20.0f, std::max (20.0f, bar.w));
        const double maxScroll = std::max (0.0, total - grid.w);
        const float thumbX = bar.x + static_cast<float> (maxScroll > 0 ? scrollX / maxScroll : 0) * (bar.w - thumbWidth);
        fill (ctx, { thumbX, bar.y + 2, thumbWidth, bar.h - 4 }, 0xff606070);
        ImGui::SetCursorScreenPos (screen ({ bar.x, bar.y }));
        ImGui::InvisibleButton ("##piano-scroll", { bar.w * scaleFactor, bar.h * scaleFactor });
        if (ImGui::IsItemActive() && ImGui::IsMousePosValid())
        {
            const auto mouse = local (ImGui::GetIO().MousePos);
            const auto fraction = std::clamp ((mouse.x - bar.x - thumbWidth * 0.5f) / std::max (1.0f, bar.w - thumbWidth), 0.0f, 1.0f);
            scrollX = static_cast<int> (std::min (maxScroll * fraction, static_cast<double> (std::numeric_limits<int>::max())));
        }
        remember ("scroll", bar);
    }
    void followPlayhead()
    {
        const auto* r = region();
        auto& t = engine.getTransport();
        if (! chase || r == nullptr || (! t.isPlaying() && ! t.isRecording())) return;
        const auto playhead = t.getPlayhead();
        if (playhead < r->timelineStart || (playhead > r->timelineStart + r->lengthInSamples && ! engine.getRecordManager().isActive())) return;
        const auto sr = engine.getTimelineSampleRate();
        const auto tick = session.samplesToTicks (playhead, sr) - session.samplesToTicks (r->timelineStart, sr);
        const double pixel = static_cast<double> (tick) * pixelsPerTick;
        if (pixel - scrollX < 0 || pixel - scrollX > grid.w)
            scrollX = static_cast<int> (std::clamp (pixel - grid.w * 0.25, 0.0, static_cast<double> (std::numeric_limits<int>::max())));
    }

    const char* popupId (Popup p) const
    {
        switch (p)
        {
            case Popup::quantize: return "Quantize";
            case Popup::scale: return "Scale";
            case Popup::velocity: return "Velocity";
            case Popup::note: return "Note Properties";
            case Popup::region: return "Region properties";
            case Popup::rename: return "MIDI region label";
            case Popup::none: return "";
        }
        return "";
    }
    bool menu (const std::string& title, const std::string& id, bool enabled = true, bool selected = false)
    {
        const bool clicked = ImGui::MenuItem (title.c_str(), nullptr, selected, enabled);
        rememberMenu (id, enabled);
        rememberMenu ("menu:" + title, enabled);
        menuLabels.push_back (title);
        return clicked;
    }
    bool beginMenu (const char* title, bool enabled = true)
    {
        const bool open = ImGui::BeginMenu (title, enabled);
        rememberMenu (std::string ("menu:") + title, enabled);
        menuLabels.push_back (title);
        return open;
    }
    void drawPopups (dw::Context& ctx)
    {
        if (popupRequested != Popup::none)
        {
            ImGui::OpenPopup (popupId (popupRequested));
            popupRequested = Popup::none;
        }
        for (const auto kind : { Popup::quantize, Popup::scale, Popup::velocity, Popup::note, Popup::region, Popup::rename })
        {
            if (! ImGui::BeginPopup (popupId (kind))) continue;
            if (cancelPopups || regionIdx != popupRegion || trackIdx < 0 || trackIdx >= Session::kNumTracks
                || session.track (trackIdx).midiRegions.edits() != popupRevision)
            { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); continue; }
            if (kind == Popup::quantize)
            {
                struct Choice { const char* label; std::int64_t ticks; float strength; };
                constexpr Choice choices[] { { "1/4 @ 100%", 480, 1 }, { "1/8 @ 100%", 240, 1 },
                    { "1/16 @ 100%", 120, 1 }, { "1/32 @ 100%", 60, 1 }, { "1/8T @ 100%", 160, 1 },
                    { "1/16T @ 100%", 80, 1 }, { "1/32T @ 100%", 40, 1 },
                    { "1/16 @ 75%", 120, 0.75f }, { "1/16 @ 50%", 120, 0.5f }, { "1/8 @ 50%", 240, 0.5f } };
                for (const auto& c : choices) if (menu (c.label, std::string ("quantize:") + c.label, editable())) quantize (c.ticks, c.strength);
            }
            if (kind == Popup::scale)
            {
                if (menu ("Off", "scale:off", true, scale == Scale::Off)) scale = Scale::Off;
                for (int s = 1; s < 8; ++s)
                    if (beginMenu (kScaleNames[s]))
                    {
                        for (int root = 0; root < 12; ++root)
                            if (menu (kRootNames[root], std::string ("scale:") + kScaleNames[s] + ":" + kRootNames[root], true,
                                      static_cast<int> (scale) == s && scaleRoot == root))
                            { scale = static_cast<Scale> (s); scaleRoot = root; }
                        ImGui::EndMenu();
                    }
            }
            if (kind == Popup::velocity)
            {
                ImGui::TextUnformatted ("Humanise (random +/- of 127)");
                for (const int percent : { 5, 10, 20, 30 })
                    if (menu ("+/- " + std::to_string (percent) + " %", "humanize:" + std::to_string (percent), editable())) humanize (percent);
                ImGui::Separator();
                ImGui::TextUnformatted ("Set velocity");
                for (const int value : { 127, 100, 80, 64, 32 })
                    if (menu ("Set to " + std::to_string (value) + (value == 127 ? " (max)" : value == 64 ? " (mid)" : ""), "velocity:" + std::to_string (value), editable())) setVelocity (value);
            }
            if (kind == Popup::note)
            {
                ImGui::Text ("Note Properties (%zu %s)", edit.selectedNotes.size(), edit.selectedNotes.size() == 1 ? "note" : "notes");
                if (beginMenu ("Channel", editable()))
                {
                    for (int ch = 1; ch <= 16; ++ch)
                        if (menu ("Channel " + std::to_string (ch), "channel:" + std::to_string (ch)))
                            changeNotes ("Set note channel", [&] (MidiRegion& r) { edit.setChannelForSelected (r.notes, ch); });
                    ImGui::EndMenu();
                }
                if (beginMenu ("Length", editable()))
                {
                    constexpr const char* labels[] { "Whole", "Half", "Quarter", "Eighth", "Sixteenth", "Thirty-second" };
                    for (int i = 0; i < 6; ++i)
                        if (menu (labels[i], std::string ("length:") + labels[i]))
                            changeNotes ("Set note length", [&] (MidiRegion& r) { edit.setLengthTicksForSelected (r.notes, r.lengthInTicks, kGridTicks[i]); });
                    ImGui::EndMenu();
                }
                if (beginMenu ("Velocity", editable()))
                {
                    for (const int value : { 127, 100, 80, 64, 32 })
                        if (menu (std::to_string (value) + (value == 127 ? " (max)" : value == 64 ? " (mid)" : ""), "velocity:" + std::to_string (value))) setVelocity (value);
                    ImGui::EndMenu();
                }
            }
            if (kind == Popup::region) drawRegionMenu();
            if (kind == Popup::rename)
            {
                ImGui::TextUnformatted ("Type a label for this region:");
                const auto a = ImGui::GetCursorScreenPos();
                const float w = ctx.s (340), h = ctx.s (28);
                const auto result = dw::textField (ctx, "##piano-label", a, { a.x + w, a.y + h }, nameBuffer.data(), nameBuffer.size(), renameFocus);
                renameFocus = false;
                ImGui::SetCursorScreenPos ({ a.x, a.y + h + ctx.s (8) });
                const bool entered = ImGui::IsKeyPressed (ImGuiKey_Enter, false) || ImGui::IsKeyPressed (ImGuiKey_KeypadEnter, false);
                const bool accept = ImGui::Button ("OK") || (result.committed && entered);
                rememberMenu ("rename:ok");
                ImGui::SameLine();
                const bool cancel = ImGui::Button ("Cancel") || result.cancelled;
                rememberMenu ("rename:cancel");
                if (accept && ! cancel)
                {
                    const std::string label (nameBuffer.data());
                    changeRegion ("Rename MIDI region", [&] (MidiRegion& r) { r.label = label.c_str(); });
                }
                if (accept || cancel) ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        cancelPopups = false;
    }
    void drawRegionMenu()
    {
        const auto* r = region();
        if (r == nullptr) return;
        ImGui::Text ("Track %d  MIDI region %d", trackIdx + 1, regionIdx + 1);
        if (menu (r->label.isEmpty() ? "Add label..." : "Rename label...", "region:rename", ! frozen()))
        {
            const auto label = r->label.toStdString();
            nameBuffer.assign (std::max<std::size_t> (4096, label.size() + 1024), 0);
            std::copy (label.begin(), label.end(), nameBuffer.begin());
            renameFocus = true;
            showPopup (Popup::rename);
        }
        if (menu (r->muted ? "Unmute region" : "Mute region", "region:mute", ! frozen()))
            changeRegion (r->muted ? "Unmute MIDI region" : "Mute MIDI region", [] (MidiRegion& a) { a.muted = ! a.muted; });
        r = region();
        if (r == nullptr) return;
        if (menu (r->locked ? "Unlock region" : "Lock region", "region:lock", ! frozen()))
            changeRegion (r->locked ? "Unlock MIDI region" : "Lock MIDI region", [] (MidiRegion& a) { a.locked = ! a.locked; });
        if (beginMenu ("Color", ! frozen()))
        {
            for (const auto& colour : kPalette)
                if (menu (colour.label, std::string ("region:color:") + colour.label))
                    changeRegion ("Set MIDI region colour", [&] (MidiRegion& a) { a.customColour = decltype (a.customColour) (colour.argb); });
            ImGui::EndMenu();
        }
        if (menu ("Delete region", "region:delete", editable()))
        {
            auto& undo = engine.getUndoManager(); undo.beginNewTransaction ("Delete MIDI region");
            undo.perform (new DeleteMidiRegionAction (session, engine, trackIdx, regionIdx));
            dismissRequested = true;
        }
        if (const auto* current = region())
            ImGui::TextDisabled ("%zu notes  -  %lld bars  -  ch %d", current->notes.size(),
                static_cast<long long> (current->lengthInTicks / (kMidiTicksPerQuarter * std::max (1, session.beatsPerBar.load (std::memory_order_relaxed)))),
                current->notes.empty() ? 1 : current->notes.front().channel);
    }
};

MidiRegion* PianoRollViewImpl::region()
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return nullptr;
    auto& v = session.track (trackIdx).midiRegions.currentMutable();
    if (regionIdx < 0 || regionIdx >= (int) v.size()) return nullptr;
    return &v[(size_t) regionIdx];
}

const MidiRegion* PianoRollViewImpl::region() const
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return nullptr;
    const auto& v = session.track (trackIdx).midiRegions.current();
    if (regionIdx < 0 || regionIdx >= (int) v.size()) return nullptr;
    return &v[(size_t) regionIdx];
}

void PianoRollViewImpl::rememberRegion()
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return;
    const auto& regions = session.track (trackIdx).midiRegions;
    seenRegionEdits = regions.edits();
    shownRegionCount = (int) regions.current().size();
    if (const auto* r = region())
    {
        shown = { r->timelineStart, r->timelineStart + r->lengthInSamples,
                  session.samplesToTicks (r->timelineStart, engine.getTimelineSampleRate()), true };
        shownNotes = r->notes;
        shownCcs   = r->ccs;
        shownTicks = r->lengthInTicks;
    }
}

int PianoRollViewImpl::regionOverlapping (const RegionAnchor& anchor) const
{
    if (! anchor.valid) return -1;
    const auto& regions = session.track (trackIdx).midiRegions.current();
    int best = -1;
    std::int64_t bestOverlap = 0;
    for (int i = 0; i < (int) regions.size(); ++i)
    {
        const auto& candidate = regions[(size_t) i];
        const auto overlap = std::min (anchor.end, candidate.timelineStart + candidate.lengthInSamples)
                           - std::max (anchor.start, candidate.timelineStart);
        if (overlap > bestOverlap) { best = i; bestOverlap = overlap; }
    }
    return best;
}

void PianoRollViewImpl::followRegionEdits()
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return;
    const auto& snapshot = session.track (trackIdx).midiRegions;
    if (snapshot.edits() == seenRegionEdits) return;
    if (noteEditActive) { noteEditActive = false; drag = Drag::dropped; }
    cancelPopups = true;
    const auto& regions = snapshot.current();
    if (regions.empty())
    {
        seenRegionEdits = snapshot.edits();
        dismissRequested = true;
        if (host.regionsGone) host.regionsGone();
        return;
    }

    const bool inRange = regionIdx >= 0 && regionIdx < (int) regions.size();
    bool sameRegion = false;
    if (inRange)
    {
        const auto& candidate = regions[(size_t) regionIdx];
        const auto tick = session.samplesToTicks (candidate.timelineStart, engine.getTimelineSampleRate());
        sameRegion = ((int) regions.size() == shownRegionCount && candidate.lengthInTicks == shownTicks
                         && candidate.notes == shownNotes && candidate.ccs == shownCcs)
                  || candidate.timelineStart == shown.start
                  || std::abs (tick - shown.startTick) <= 1;
    }
    if (! sameRegion)
    {

        int best = regionOverlapping (shown);
        if (best < 0) best = regionOverlapping (leftForTake);
        if (best < 0)
        {
            best = 0;
            for (int i = 1; i < (int) regions.size(); ++i)
                if (std::abs (regions[(size_t) i].timelineStart - shown.start)
                    < std::abs (regions[(size_t) best].timelineStart - shown.start))
                    best = i;
        }
        if (best != regionIdx)
        {
            showRegion (best);
            return;
        }
        clearSelection();
    }

    const auto* r = region();
    const int noteCount = r != nullptr ? (int) r->notes.size() : 0;
    edit.selectedNotes.erase (std::lower_bound (edit.selectedNotes.begin(), edit.selectedNotes.end(), noteCount),
                         edit.selectedNotes.end());
    if (edit.dragAnchor >= noteCount) edit.dragAnchor = -1;
    rememberRegion();
}

void PianoRollViewImpl::showSwappedTake()
{
    followRegionEdits();
    clearSelection();
}

void PianoRollViewImpl::showRegion (int newRegionIdx)
{
    if (newRegionIdx != regionIdx)
        stepRecordChordHad = false;
    regionIdx = newRegionIdx;
    cancelPopups = true;
    leftForTake.valid = false;
    clearSelection();
    drag = Drag::none;
    edit.dragSnapshots.clear();
    noteEditActive = false;
    rangeActive = false;
    draggedCcIdx = -1;
    rememberRegion();
    if (host.regionChanged) host.regionChanged (regionIdx);
}

void PianoRollViewImpl::showCommittedRegion (int committedIndex)
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return;
    const auto count = (int) session.track (trackIdx).midiRegions.current().size();
    if (count == 0) return;
    const auto left = shown;
    const double absoluteLeftTick = (double) shown.startTick + (double) scrollX / pixelsPerTick;
    showRegion (committedIndex >= 0 && committedIndex < count ? committedIndex : count - 1);
    scrollX = std::max (0, (int) std::round ((absoluteLeftTick - (double) shown.startTick) * pixelsPerTick));
    leftForTake = left;
}

void PianoRollViewImpl::stepRecordNoteOn (int noteNumber, int velocity, int channel)
{

    if (engine.getTransport().isRecording() || ! editable()) return;
    auto* r = region();
    if (r == nullptr) return;
    const double sr  = engine.getTimelineSampleRate();
    const float  bpm = session.tempoBpm.load (std::memory_order_relaxed);
    if (bpm <= 0.0f) return;

    const std::int64_t stepTicks = snapTicks > 0
        ? snapTicks
        : (kMidiTicksPerQuarter / 4);

    // After the keyboard closes and reopens, a playhead the user moved meanwhile is
    // the new entry point rather than a place to step on from.
    const auto playhead = engine.getTransport().getPlayhead();
    const bool chordHeld = std::any_of (stepRecordHeld.begin(), stepRecordHeld.end(),
                                       [] (int count) { return count > 0; });
    if (! chordHeld && stepRecordChordHad
        && (! stepRecordReopened || playhead == stepRecordAt))
    {
        const auto playheadTick = session.samplesToTicks (playhead, sr);
        const std::int64_t advanceSamples =
            session.ticksToSamples (playheadTick + stepTicks, sr) - playhead;
        engine.getTransport().setPlayhead (playhead + advanceSamples);
    }
    if (! chordHeld)
        stepRecordChordHad = false;
    stepRecordReopened = false;

    const auto nowPlayhead = engine.getTransport().getPlayhead();
    if (nowPlayhead - r->timelineStart < 0) return;
    const auto regStartTick = session.samplesToTicks (r->timelineStart, sr);
    const std::int64_t startTick = session.samplesToTicks (nowPlayhead, sr) - regStartTick;
    const MidiRegion before = *r;

    if (startTick + stepTicks > r->lengthInTicks)
    {
        r->lengthInTicks = startTick + stepTicks;
        r->lengthInSamples = session.ticksToSamples (regStartTick + r->lengthInTicks, sr)
                                 - r->timelineStart;
    }

    MidiNote n;
    n.channel       = 1;
    n.noteNumber    = limited (0, kNumKeys - 1, noteNumber);
    n.velocity      = limited (1, 127, velocity);
    n.startTick     = startTick;
    n.lengthInTicks = stepTicks;
    r->notes.push_back (n);

    pushNoteEdit (before, "Step record note");
    const auto heldIndex = (std::size_t) (std::clamp (channel, 1, 16) - 1) * kNumKeys
                            + (std::size_t) n.noteNumber;
    ++stepRecordHeld[heldIndex];
    stepRecordChordHad = true;
    stepRecordAt = nowPlayhead;
}

void PianoRollViewImpl::stepRecordNoteOff (int noteNumber, int channel)
{
    if (noteNumber < 0 || noteNumber >= kNumKeys) return;
    const auto heldIndex = (std::size_t) (std::clamp (channel, 1, 16) - 1) * kNumKeys
                            + (std::size_t) noteNumber;
    auto& held = stepRecordHeld[heldIndex];
    if (held > 0)
        --held;
}

void PianoRollViewImpl::stepRecordKeyboardClosed() noexcept
{
    stepRecordHeld.fill (0);
    stepRecordReopened = stepRecordChordHad;
}

bool PianoRollViewImpl::isInScale (int noteNumber) const noexcept
{
    if (scale == Scale::Off) return true;
    static constexpr int kMajorIntervals[]      = { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr int kMinorIntervals[]      = { 0, 2, 3, 5, 7, 8, 10 };
    static constexpr int kDorianIntervals[]     = { 0, 2, 3, 5, 7, 9, 10 };
    static constexpr int kPhrygianIntervals[]   = { 0, 1, 3, 5, 7, 8, 10 };
    static constexpr int kLydianIntervals[]     = { 0, 2, 4, 6, 7, 9, 11 };
    static constexpr int kMixolydianIntervals[] = { 0, 2, 4, 5, 7, 9, 10 };
    static constexpr int kLocrianIntervals[]    = { 0, 1, 3, 5, 6, 8, 10 };
    const int* table = nullptr;
    int len = 7;
    switch (scale)
    {
        case Scale::Major:      table = kMajorIntervals;      break;
        case Scale::Minor:      table = kMinorIntervals;      break;
        case Scale::Dorian:     table = kDorianIntervals;     break;
        case Scale::Phrygian:   table = kPhrygianIntervals;   break;
        case Scale::Lydian:     table = kLydianIntervals;     break;
        case Scale::Mixolydian: table = kMixolydianIntervals; break;
        case Scale::Locrian:    table = kLocrianIntervals;    break;
        case Scale::Off: default: return true;
    }
    const int rel = ((noteNumber - scaleRoot) % 12 + 12) % 12;
    for (int i = 0; i < len; ++i) if (table[i] == rel) return true;
    return false;
}

int PianoRollViewImpl::snapPitchToScale (int noteNumber) const noexcept
{

    if (! keySnap || scale == Scale::Off) return noteNumber;
    noteNumber = limited (0, kNumKeys - 1, noteNumber);
    if (isInScale (noteNumber)) return noteNumber;
    for (int d = 1; d < 12; ++d)
    {
        if (noteNumber - d >= 0            && isInScale (noteNumber - d)) return noteNumber - d;
        if (noteNumber + d <= kNumKeys - 1 && isInScale (noteNumber + d)) return noteNumber + d;
    }
    return noteNumber;
}

int PianoRollViewImpl::nextRegionIndex (int delta) const
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return -1;
    const auto& regs = session.track (trackIdx).midiRegions.current();
    if (regionIdx < 0 || regionIdx >= (int) regs.size()) return -1;

    using Key = std::pair<std::int64_t, int>;
    const Key cur { regs[(size_t) regionIdx].timelineStart, regionIdx };
    int bestIdx = -1;
    Key best = (delta > 0)
        ? Key { std::numeric_limits<std::int64_t>::max(), std::numeric_limits<int>::max() }
        : Key { std::numeric_limits<std::int64_t>::min(), std::numeric_limits<int>::min() };
    for (int i = 0; i < (int) regs.size(); ++i)
    {
        if (i == regionIdx) continue;
        const Key k { regs[(size_t) i].timelineStart, i };
        if (delta > 0 ? (k > cur && k < best) : (k < cur && k > best))
        {
            best = k;
            bestIdx = i;
        }
    }
    return bestIdx;
}

void PianoRollViewImpl::splitSelectedAtCursor()
{
    auto* r = region();
    if (! editable()) return;
    const MidiRegion before = *r;
    const auto cursor = editCursorTick;

    auto candidate = [&] (int idx)
    {
        return ! edit.selectedNotes.empty()
            ? std::binary_search (edit.selectedNotes.begin(), edit.selectedNotes.end(), idx)
            : true;
    };

    const int initialCount = (int) r->notes.size();
    for (int i = initialCount - 1; i >= 0; --i)
    {
        if (! candidate (i)) continue;
        auto& n = r->notes[(size_t) i];
        if (cursor <= n.startTick || cursor >= n.startTick + n.lengthInTicks) continue;

        MidiNote tail = n;
        tail.startTick     = cursor;
        tail.lengthInTicks = (n.startTick + n.lengthInTicks) - cursor;
        n.lengthInTicks    = cursor - n.startTick;
        r->notes.push_back (tail);
    }
    pushNoteEdit (before, "Split notes");
}

void PianoRollViewImpl::zoomFit()
{
    const auto* r = region();
    if (r == nullptr || r->lengthInTicks <= 0) return;
    pixelsPerTick = std::min (1.0f, (float) grid.w / (float) r->lengthInTicks);
    scrollX = 0;
}

void PianoRollViewImpl::zoomByFactor (float factor)
{

    const int gridLeft = kKeyboardWidth;
    const int viewportW = std::max (1, width - gridLeft);
    const int centreX = gridLeft + viewportW / 2;
    const auto centreTickBefore = tickForX (centreX);
    const auto* r = region();
    const float minimum = r != nullptr && r->lengthInTicks > 0
        ? std::min (0.005f, grid.w / (float) r->lengthInTicks) : 0.005f;
    pixelsPerTick = limited (std::min (pixelsPerTick, minimum), 1.0f, pixelsPerTick * factor);
    const int newCentrePx = (int) std::round ((double) centreTickBefore * pixelsPerTick);
    scrollX = std::max (0, newCentrePx - (centreX - gridLeft));
}

} // namespace

std::unique_ptr<PianoRollView> makePianoRollView (Session& session, AudioEngine& engine, int track, int region,
                                               PianoRollHost host)
{
    return std::make_unique<PianoRollViewImpl> (session, engine, track, region, std::move (host));
}
} // namespace duskstudio::imgui
