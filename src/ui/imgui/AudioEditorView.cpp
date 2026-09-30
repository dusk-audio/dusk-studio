#include "AudioEditorView.h"
#include "PanelControls.h"
#include "../AppConfig.h"
#include "../../engine/AudioEngine.h"
#include "../../engine/Transport.h"
#include "../../engine/audiofile/FileReader.h"
#include "../../engine/audiofile/WaveformPeaks.h"
#include "../../foundation/Decibels.h"
#include "../../foundation/PlanarBuffer.h"
#include "../../foundation/Text.h"
#include "../../foundation/VectorOps.h"
#include "../../session/RegionEditActions.h"
#include "../../session/Session.h"
#include "../../session/SnapHelpers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace duskstudio::imgui
{
namespace
{
namespace dw = DuskWidgets;
using dusk::audio::WaveformDetails;
using dusk::audio::WaveformPeaks;
using dusk::audio::WaveformSource;

// The JUCE editor's bands, in design pixels.
constexpr float kIconRowH = 48.0f;
constexpr float kRulerH = 28.0f;
constexpr float kStatusH = 30.0f;
constexpr float kScrollH = 12.0f;
constexpr float kLaneInset = 4.0f;

constexpr float kMinPixelsPerSample = 1.0e-5f;
constexpr float kMaxPixelsPerSample = 1.0f;
constexpr float kZoomStep = 1.15f;
constexpr float kWheelPanPixels = 12.5f;

// Each source owns a worker thread, so the editor keeps only the files the view has
// shown most recently rather than one per region.
constexpr std::size_t kMaxSources = 8;

constexpr std::uint32_t kBackground = 0xff181820;
constexpr std::uint32_t kIconRowFill = 0xff20202c;
constexpr std::uint32_t kHeaderFill = 0xff202028;
constexpr std::uint32_t kHeaderText = 0xffb0b0b8;
constexpr std::uint32_t kBarLine = 0xff5a5a64;
constexpr std::uint32_t kBeatLine = 0xff3c3c46;
constexpr std::uint32_t kWaveformFill = 0xff80b0e0;
constexpr std::uint32_t kGainLine = 0xff80e070;
constexpr std::uint32_t kGainChip = 0xff80ff70;
constexpr std::uint32_t kGainChipInk = 0xff182018;
constexpr std::uint32_t kFadeStroke = 0xffe0c060;
constexpr std::uint32_t kFadeDisc = 0xffffd040;
constexpr std::uint32_t kTrimStrip = 0xff707078;
constexpr std::uint32_t kSliceBoundary = 0xff404048;
constexpr std::uint32_t kEditCursor = 0xffffd060;
constexpr std::uint32_t kPlayhead = 0xffffe8c0;
constexpr std::uint32_t kLoop = 0xff3aa860;
constexpr std::uint32_t kPunch = 0xffd05a5a;
constexpr std::uint32_t kIconDisc = 0xff262630;
constexpr std::uint32_t kIconRim = 0xff3a3a44;
constexpr std::uint32_t kIconGlyph = 0xffd0d0d0;
constexpr std::uint32_t kReadoutText = 0xffd0d0d8;
constexpr std::uint32_t kReadoutOutline = 0xff3a3a44;
constexpr std::uint32_t kChaseOff = 0xff222226;
constexpr std::uint32_t kChaseOn = 0xff406030;
constexpr std::uint32_t kChaseTextOff = 0xff909094;
constexpr std::uint32_t kChaseTextOn = 0xffe0e0e0;
constexpr std::uint32_t kAutoFill = 0xff282830;
constexpr std::uint32_t kScrollTrack = 0xff15151b;
constexpr std::uint32_t kScrollThumb = 0xff4a4a54;
constexpr std::uint32_t kRange = 0xffffd060;
constexpr std::uint32_t kSelection = 0xff80c0ff;

constexpr const char* kContextMenu = "##editor-context";
constexpr const char* kFadeMenu = "##editor-fade-shape";
constexpr const char* kPropertiesMenu = "##editor-properties";

using RegionColour = decltype (AudioRegion::customColour);
using RegionLabel = decltype (AudioRegion::label);

// The region colours every region surface offers, the tape strip's included.
struct PaletteEntry
{
    const char* label;
    std::uint32_t argb;
};

constexpr PaletteEntry kPalette[] = {
    { "Reset to track colour", 0x00000000 },
    { "Red", 0xffd05f5f }, { "Orange", 0xffd09060 }, { "Yellow", 0xffd0c060 },
    { "Green", 0xff60c070 }, { "Cyan", 0xff60c0c0 }, { "Blue", 0xff6090d0 },
    { "Purple", 0xff9070c0 }, { "Magenta", 0xffc060a0 },
};

struct FadeShapeEntry
{
    FadeShape shape;
    const char* label;
};

constexpr FadeShapeEntry kFadeShapes[] = {
    { FadeShape::Linear, "Linear" }, { FadeShape::EqualPower, "Equal-power" },
    { FadeShape::Sigmoid, "S-curve" }, { FadeShape::Exp, "Exponential" },
    { FadeShape::Log, "Logarithmic" },
};

// The keys the editor reads from Dear ImGui, and whether holding one repeats it.
struct EditorKey
{
    ImGuiKey key;
    bool repeats;
};

constexpr EditorKey kEditorKeys[] = {
    { ImGuiKey_Escape, false }, { ImGuiKey_LeftArrow, true }, { ImGuiKey_RightArrow, true },
    { ImGuiKey_Home, false }, { ImGuiKey_End, false }, { ImGuiKey_G, false }, { ImGuiKey_R, false },
    { ImGuiKey_C, false }, { ImGuiKey_Z, true }, { ImGuiKey_E, false }, { ImGuiKey_F, false },
    { ImGuiKey_X, false }, { ImGuiKey_V, false }, { ImGuiKey_LeftBracket, false },
    { ImGuiKey_RightBracket, false }, { ImGuiKey_L, false }, { ImGuiKey_P, false },
    { ImGuiKey_Delete, false }, { ImGuiKey_Backspace, false }, { ImGuiKey_Equal, true },
    { ImGuiKey_Minus, true }, { ImGuiKey_KeypadAdd, true }, { ImGuiKey_KeypadSubtract, true },
    { ImGuiKey_0, false },
};

// std::clamp, but tolerant of an upper bound below the lower one.
std::int64_t clampTo (std::int64_t value, std::int64_t lo, std::int64_t hi) noexcept
{
    return std::clamp (value, lo, std::max (lo, hi));
}

bool nonZero (float v) noexcept { return v < 0.0f || v > 0.0f; }

ImU32 argb (std::uint32_t c, float alpha = 1.0f) noexcept
{
    const auto a = static_cast<unsigned int> (std::lround (static_cast<float> ((c >> 24) & 0xffu)
                                                           * std::clamp (alpha, 0.0f, 1.0f)));
    return IM_COL32 ((c >> 16) & 0xffu, (c >> 8) & 0xffu, c & 0xffu, a);
}

// HSB brightness, the way the JUCE editor dimmed its neighbouring slices.
ImU32 withBrightness (ImU32 colour, float factor, float alpha) noexcept
{
    ImVec4 v = ImGui::ColorConvertU32ToFloat4 (colour);
    float h = 0.0f, s = 0.0f, b = 0.0f;
    ImGui::ColorConvertRGBtoHSV (v.x, v.y, v.z, h, s, b);
    ImGui::ColorConvertHSVtoRGB (h, s, std::min (1.0f, b * factor), v.x, v.y, v.z);
    v.w = alpha;
    return ImGui::ColorConvertFloat4ToU32 (v);
}

const char* const kSnapLabels[] = {
    "Bar", "1/2 Note", "1/4 Note", "1/8 Note", "1/16 Note", "1/32 Note", "1/64 Note",
    "1/128 Note", "1/2 Triplet", "1/4 Triplet", "1/8 Triplet", "1/16 Triplet",
    "1/32 Triplet", "1/2 Dotted", "1/4 Dotted", "1/8 Dotted", "1/16 Dotted",
    "Timecode", "MinSec", "CD Frames"
};
constexpr int kSnapLabelCount = static_cast<int> (std::size (kSnapLabels));

struct Box
{
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;

    float width() const noexcept { return x1 - x0; }
    float height() const noexcept { return y1 - y0; }
    ImVec2 tl() const noexcept { return { x0, y0 }; }
    ImVec2 br() const noexcept { return { x1, y1 }; }
    ImVec2 at (float fx, float fy) const noexcept { return { x0 + width() * fx, y0 + height() * fy }; }
    bool contains (ImVec2 p) const noexcept { return p.x >= x0 && p.x < x1 && p.y >= y0 && p.y < y1; }
    Box reduced (float dx, float dy) const noexcept { return { x0 + dx, y0 + dy, x1 - dx, y1 - dy }; }
    Box sizedKeepingCentre (float w, float h) const noexcept
    {
        const auto c = at (0.5f, 0.5f);
        return { c.x - w * 0.5f, c.y - h * 0.5f, c.x + w * 0.5f, c.y + h * 0.5f };
    }
    Box takeLeft (float w) noexcept
    {
        const Box taken { x0, y0, std::min (x1, x0 + w), y1 };
        x0 = taken.x1;
        return taken;
    }
    Box takeRight (float w) noexcept
    {
        const Box taken { std::max (x0, x1 - w), y0, x1, y1 };
        x1 = taken.x0;
        return taken;
    }
};

// Every band of the editor in window pixels, rebuilt each frame from the body the
// window grants.
struct Layout
{
    float scale = 1.0f;
    Box body, icons, ruler, wave, lanes, scroll, status;

    float s (float v) const noexcept { return v * scale; }
};

enum class Glyph { undo, redo, split, normalize, reverse, properties, zoomOut, zoomIn, zoomFit };

struct Control
{
    std::string name;
    Box box;
    bool enabled = true;
};

struct Source
{
    std::string path;
    std::unique_ptr<WaveformSource> source;
    std::vector<WaveformDetails::Window> requested;
    std::vector<WaveformDetails::Window> wanted;
    WaveformSource::Snapshot snapshot;
    std::uint64_t lastUsed = 0;
    bool detailsSent = false;
};

// One region's on-screen span, gathered before anything is drawn so the detail windows
// can be requested for this frame rather than the next.
struct Slice
{
    int region = -1;
    int xa = 0, xb = 0;     // the whole region, which may run off either edge
    int x0 = 0, x1 = 0;     // the part inside the lanes
    Source* source = nullptr;
    WaveformDetails::Window window;
    bool detail = false;
};

void vline (ImDrawList* dl, float x, float y0, float y1, ImU32 colour, float width)
{
    const float left = std::floor (x);
    dl->AddRectFilled (ImVec2 (left, y0), ImVec2 (left + std::max (1.0f, width), y1), colour);
}

void hline (ImDrawList* dl, float y, float x0, float x1, ImU32 colour, float width)
{
    const float top = std::floor (y);
    dl->AddRectFilled (ImVec2 (x0, top), ImVec2 (x1, top + std::max (1.0f, width)), colour);
}

WaveformDetails::Window windowFor (const AudioRegion& region, int xa, int xb, int clipX0, int clipX1)
{
    const int first = std::max (xa, clipX0);
    return { region.sourceOffset, region.lengthInSamples, xb - xa, first - xa,
             std::min (xb, clipX1) - first };
}

// The edit-mode glyphs the timeline shows at the pointer, drawn here because nothing
// JUCE paints can land on top of the editor's child. The hand's hotspot is the tip of
// the index finger and the scissors' the blades' crossing; both are authored at the
// JUCE glyphs' size and drawn at 0.85 of it, as the overlay drew them.
void drawHandGlyph (ImDrawList* dl, ImVec2 at, float scale)
{
    struct Part { float x, y, w, h, rounding; };
    static constexpr Part kParts[] = { { 6.0f, 11.0f, 12.0f, 10.0f, 3.0f }, { 7.0f, 2.0f, 3.2f, 13.0f, 1.6f },
                                       { 11.0f, 9.0f, 3.0f, 4.0f, 1.4f }, { 14.5f, 9.5f, 3.0f, 4.0f, 1.4f },
                                       { 4.0f, 13.0f, 3.0f, 6.0f, 1.4f } };
    const float g = 0.85f * scale;
    const auto fill = [dl, at, g] (const Part& part, float grow, ImU32 colour)
    {
        const float x = part.x - 8.6f, y = part.y - 2.0f;
        dl->AddRectFilled (ImVec2 (at.x + (x - grow) * g, at.y + (y - grow) * g),
                           ImVec2 (at.x + (x + part.w + grow) * g, at.y + (y + part.h + grow) * g),
                           colour, (part.rounding + grow) * g);
    };
    for (const auto& part : kParts)
        fill (part, 1.7f, IM_COL32 (0, 0, 0, 191));
    for (const auto& part : kParts)
        fill (part, 0.0f, IM_COL32_WHITE);
}

void drawScissorsGlyph (ImDrawList* dl, ImVec2 at, float scale)
{
    const float g = 0.85f * scale;
    const auto p = [at, g] (float x, float y) { return ImVec2 (at.x + x * g, at.y + y * g); };
    constexpr float a = 8.0f;
    constexpr float loop = 2.5f;
    for (const auto& [colour, width] : { std::pair<ImU32, float> { IM_COL32 (0, 0, 0, 217), 2.6f },
                                         std::pair<ImU32, float> { IM_COL32_WHITE, 1.4f } })
    {
        dl->AddLine (p (-a, -a), p (a, a), colour, width * g);
        dl->AddLine (p (a, -a), p (-a, a), colour, width * g);
        dl->AddCircle (p (-a, a), loop * g, colour, 12, width * g);
        dl->AddCircle (p (a, a), loop * g, colour, 12, width * g);
    }
}

// Where a Cut click would split: a dashed line down the waveform, dark under light so
// it reads over the audio.
void drawCutLine (ImDrawList* dl, float x, float y0, float y1, float scale)
{
    const float dash = 3.0f * scale;
    for (const auto& [colour, width] : { std::pair<ImU32, float> { IM_COL32 (0, 0, 0, 217), 2.2f },
                                         std::pair<ImU32, float> { IM_COL32_WHITE, 1.2f } })
        for (float y = y0; y < y1; y += dash * 2.0f)
            dl->AddLine (ImVec2 (x, y), ImVec2 (x, std::min (y + dash, y1)), colour, width * scale);
}

class AudioEditorViewImpl final : public AudioEditorView
{
public:
    AudioEditorViewImpl (Session& s, AudioEngine& e, int track, int regionIndex, AudioEditorHost h)
        : session (s), engine (e), trackIdx (track), regionIdx (regionIndex), host (std::move (h)),
          chase (appconfig::getFollowPlayheadDefault())
    {
        // The editor offers four of the five tools; Grid edits the tempo map, which is
        // the timeline's business.
        if (session.editMode == EditMode::Grid)
            session.editMode = EditMode::Grab;
    }

    void setAvailableSize (float width, float height) override
    {
        available = ImVec2 (std::max (200.0f, width), std::max (200.0f, height));
    }

    ImVec2 preferredSize() const override { return available; }
    float dimAlpha() const override { return 0.80f; }

    // Escape is the view's, so an open dropdown takes it before the editor closes.
    bool escapeDismisses() const override { return false; }

    bool takeDismissRequest() override { return std::exchange (dismissRequested, false); }

    // Home and the loop and punch keys act on the editor's own view and cursor, and R
    // picks the Range tool rather than recording; the other transport keys stay the
    // shell's.
    bool claimsShortcut (ShellShortcut shortcut) const override
    {
        return shortcut == ShellShortcut::playheadToZero || shortcut == ShellShortcut::toggleLoop
            || shortcut == ShellShortcut::togglePunch || shortcut == ShellShortcut::setLoopIn
            || shortcut == ShellShortcut::setLoopOut || shortcut == ShellShortcut::setPunchIn
            || shortcut == ShellShortcut::setPunchOut || shortcut == ShellShortcut::record;
    }

    int trackIndex() const override { return trackIdx; }
    int regionIndex() const override { return regionIdx; }
    bool chaseEnabled() const override { return chase; }

    bool handleShellKey (const std::string& description) override
    {
        const auto chord = parseKeyDescription (description);
        if (! chord)
            return false;
        if (keysAvailable)
            return handleKey (*chord);

        // A menu or a field open over the editor takes Escape, as it does when the
        // child has the keyboard, rather than letting the shell close the editor
        // under it. The key is replayed into Dear ImGui on the next frame.
        const bool bareEscape = chord->key == ImGuiKey_Escape && ! chord->command() && ! chord->shift && ! chord->alt;
        if (bareEscape && (popupWasOpen || editing != Field::none))
        {
            escapePending = true;
            return true;
        }
        return false;
    }

    void focusRegion (int index) override
    {
        regionIdx = index;
        if (const auto* r = region())
            editCursorSample = std::clamp (editCursorSample, r->sourceOffset,
                                           r->sourceOffset + r->lengthInSamples);
    }

    std::vector<double> viewForScenario() const override
    {
        return { static_cast<double> (pixelsPerSample), static_cast<double> (scrollSamples),
                 static_cast<double> (editCursorSample) };
    }

    bool samplePointForScenario (std::int64_t timelineSample, ImVec2& point) const override
    {
        if (! laidOut)
            return false;
        point = { xForTimeline (timelineSample), layout.wave.y0 + layout.wave.height() * 0.75f };
        return true;
    }

    bool gesturePointForScenario (const std::string& kind, std::int64_t timelineSample,
                                  ImVec2& point) const override
    {
        if (! samplePointForScenario (timelineSample, point) || region() == nullptr)
            return false;
        if (kind == "start") point.x = trimStart().at (0.5f, 0.5f).x;
        else if (kind == "end") point.x = trimEnd().at (0.5f, 0.5f).x;
        else if (kind == "gain") point.y = gainLineY();
        else if (kind != "wave") return false;
        point = { (point.x - layout.body.x0) / layout.scale, (point.y - layout.body.y0) / layout.scale };
        return true;
    }

    std::vector<std::int64_t> selectionForScenario() const override
    {
        return { regionIdx, rangeActive ? 1 : 0, rangeStartSample, rangeEndSample };
    }

    // Toolbar and status controls by the name the JUCE editor gave each button, plus
    // "waveform" (the point the JUCE editor's centre-low click used), "sample:<n>" for
    // a timeline sample, "at:<x>,<y>" for a body-relative design-pixel point and
    // "menu:<item>" for an item of the menu that is open.
    bool controlPointForScenario (const std::string& name, ImVec2& point, float position) const override
    {
        if (! laidOut)
            return false;
        if (name == "waveform")
        {
            point = layout.body.at (0.5f, 0.75f);
            return true;
        }
        if (name.rfind ("sample:", 0) == 0)
            return samplePointForScenario (std::strtoll (name.c_str() + 7, nullptr, 10), point);
        if (name.rfind ("at:", 0) == 0)
        {
            char* end = nullptr;
            const float x = std::strtof (name.c_str() + 3, &end);
            if (end == nullptr || *end != ',')
                return false;
            const float y = std::strtof (end + 1, nullptr);
            point = { layout.body.x0 + layout.s (x), layout.body.y0 + layout.s (y) };
            return true;
        }
        for (const auto& control : controls)
        {
            if (name != control.name)
                continue;
            if (! control.enabled)
                return false;
            point = { control.box.x0 + control.box.width() * std::clamp (position, 0.0f, 1.0f),
                      control.box.at (0.5f, 0.5f).y };
            return true;
        }
        return false;
    }

    void draw (dw::Context& ctx, ImVec2 origin, ImVec2 size) override
    {
        popupWasOpen = popupOpen;
        ++frame;
        if (std::exchange (escapePending, false))
        {
            // Queued as a press and a release, which Dear ImGui plays over two frames.
            auto& io = ImGui::GetIO();
            io.AddKeyEvent (ImGuiKey_Escape, true);
            io.AddKeyEvent (ImGuiKey_Escape, false);
        }
        controls.clear();
        layout = layoutFor (origin, size, ctx.scale);
        laidOut = true;

        const float waveWidth = layout.wave.width() / layout.scale;
        if (region() != nullptr && std::abs (waveWidth - fittedWidth) > 0.5f)
        {
            zoomFit();
            fittedWidth = waveWidth;
        }

        if (const auto* r = region())
            sourceFor (r->file.getFullPathName().toStdString());

        handleKeys (ctx);
        handleWheel();
        followPlayhead();

        auto* const dl = ctx.dl;
        dl->AddRectFilled (layout.body.tl(), layout.body.br(), argb (kBackground));

        drawToolbar (ctx);
        drawStatusBar (ctx);

        if (region() == nullptr)
        {
            const auto centre = layout.body.at (0.0f, 0.5f);
            dw::text (ctx, ctx.fonts->valueLarge, ctx.s (14.0f),
                      ImVec2 (centre.x, centre.y - ctx.s (7.0f)), layout.body.width(),
                      argb (kHeaderText), "region unavailable");
            drag = Drag::none;
            drawPopups (ctx);
            finishFrame();
            return;
        }

        collectSlices();
        requestDetails();

        dl->PushClipRect (layout.ruler.tl(), ImVec2 (layout.wave.x1, layout.wave.y1), true);
        drawRuler (ctx);
        drawWaveforms (ctx);
        drawBarGrid (ctx);
        drawFades (ctx);
        drawLoopPunch (ctx);
        drawHandles (ctx);
        drawRange (ctx);
        drawEditCursor (ctx);
        drawPlayhead (ctx);
        drawSnapGuide (ctx);
        dl->PopClipRect();

        drawScrollBar (ctx);
        handlePointer();
        drawPopups (ctx);
        drawPointerGlyph (ctx);
        finishFrame();
    }

private:
    Session& session;
    AudioEngine& engine;
    int trackIdx;
    int regionIdx;
    AudioEditorHost host;

    ImVec2 available { 1000.0f, 640.0f };
    Layout layout;
    bool laidOut = false;
    float fittedWidth = -1.0f;

    // The view spans the whole track, [anchorStart, anchorStart + anchorLength), in
    // timeline samples; the edit cursor is a file sample of the focused region.
    std::int64_t anchorStart = 0;
    std::int64_t anchorLength = 1;
    float pixelsPerSample = 0.0f;
    std::int64_t scrollSamples = 0;
    std::int64_t editCursorSample = 0;
    float panRemainder = 0.0f;

    bool chase = false;
    bool dismissRequested = false;
    bool popupOpen = false;
    bool popupWasOpen = false;
    bool keysAvailable = false;
    bool escapePending = false;

    // A pointer gesture from press to release. The press snapshots the region, the
    // drag edits it live so the view follows, and the release rolls it back and
    // commits the whole gesture as one undo step.
    enum class Drag { none, fadeIn, fadeOut, gain, trimStart, trimEnd, moveCursor, range, moveRegion,
                      pan, loopIn, loopOut, punchIn, punchOut };
    Drag drag = Drag::none;
    ImGuiMouseButton dragButton = ImGuiMouseButton_Left;
    ImVec2 dragDown;
    ImVec2 dragLast;
    bool gestureActive = false;
    AudioRegion regionAtDragStart;
    float dragOriginGainDb = 0.0f;
    std::int64_t dragOriginTimeline = 0;
    std::int64_t panStartScroll = 0;
    std::int64_t snapGuide = -1;

    // Regions selected alongside the focused one, which is always implicitly selected.
    // Delete, move and nudge act on the lot.
    std::vector<int> additional;
    std::vector<std::int64_t> additionalOrigins;

    // [start, end) in file samples of the focused region; active once it has width.
    bool rangeActive = false;
    std::int64_t rangeStartSample = 0;
    std::int64_t rangeEndSample = 0;

    bool fadeMenuIsIn = true;
    ImVec2 propertiesAnchor;

    enum class Field { none, title, label, gain, fade };
    Field editing = Field::none;
    bool fieldTakesFocus = false;
    bool fieldDrawn = false;
    std::array<char, 256> fieldText {};
    std::string fieldOriginal;

    bool scrollDragging = false;
    float scrollDragX = 0.0f;
    std::int64_t scrollDragOrigin = 0;

    std::vector<Control> controls;
    std::vector<std::unique_ptr<Source>> sources;
    std::vector<Slice> slices;
    std::uint64_t frame = 0;

    AudioRegion* region()
    {
        if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return nullptr;
        auto& regions = session.track (trackIdx).regions;
        if (regionIdx < 0 || regionIdx >= static_cast<int> (regions.size())) return nullptr;
        return &regions[static_cast<std::size_t> (regionIdx)];
    }

    const AudioRegion* region() const
    {
        if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return nullptr;
        const auto& regions = session.track (trackIdx).regions;
        if (regionIdx < 0 || regionIdx >= static_cast<int> (regions.size())) return nullptr;
        return &regions[static_cast<std::size_t> (regionIdx)];
    }

    const std::vector<AudioRegion>& trackRegions() const
    {
        return session.track (trackIdx).regions;
    }

    void commit (const char* name, const AudioRegion& before, const AudioRegion& after)
    {
        auto& undo = engine.getUndoManager();
        undo.beginNewTransaction (name);
        undo.perform (new RegionEditAction (session, engine, trackIdx, regionIdx, before, after));
    }

    static Layout layoutFor (ImVec2 origin, ImVec2 size, float scale)
    {
        Layout l;
        l.scale = scale;
        l.body = { origin.x, origin.y, origin.x + size.x, origin.y + size.y };
        l.icons = { l.body.x0, l.body.y0, l.body.x1, l.body.y0 + l.s (kIconRowH) };
        l.ruler = { l.body.x0, l.icons.y1, l.body.x1, l.icons.y1 + l.s (kRulerH) };
        l.status = { l.body.x0, l.body.y1 - l.s (kStatusH), l.body.x1, l.body.y1 };
        l.scroll = { l.body.x0, l.status.y0 - l.s (kScrollH), l.body.x1, l.status.y0 };
        l.wave = { l.body.x0, l.ruler.y1, l.body.x1, std::max (l.ruler.y1, l.scroll.y0) };
        l.lanes = l.wave.reduced (l.s (kLaneInset), l.s (kLaneInset));
        return l;
    }

    float pixelsPerSampleOnScreen() const noexcept { return pixelsPerSample * layout.scale; }

    float xForTimeline (std::int64_t timelineSample) const
    {
        const auto rel = static_cast<double> (timelineSample - anchorStart - scrollSamples);
        return layout.lanes.x0 + static_cast<float> (rel * static_cast<double> (pixelsPerSampleOnScreen()));
    }

    int columnForTimeline (std::int64_t timelineSample) const
    {
        return static_cast<int> (std::lround (xForTimeline (timelineSample)));
    }

    std::int64_t timelineForX (float x) const
    {
        if (pixelsPerSample <= 0.0f)
            return anchorStart;
        return anchorStart + scrollSamples
             + static_cast<std::int64_t> (std::llround ((x - layout.lanes.x0) / pixelsPerSampleOnScreen()));
    }

    float xForFileSample (std::int64_t fileSample) const
    {
        const auto* r = region();
        if (r == nullptr) return layout.lanes.x0;
        return xForTimeline (r->timelineStart + (fileSample - r->sourceOffset));
    }

    std::int64_t fileSampleForX (float x) const
    {
        const auto* r = region();
        if (r == nullptr) return 0;
        const auto fileSample = r->sourceOffset + (timelineForX (x) - r->timelineStart);
        return std::clamp (fileSample, r->sourceOffset, r->sourceOffset + r->lengthInSamples);
    }

    int regionIndexAtX (float x) const
    {
        const auto t = timelineForX (x);
        const auto& regions = trackRegions();
        for (int i = 0; i < static_cast<int> (regions.size()); ++i)
        {
            const auto& reg = regions[static_cast<std::size_t> (i)];
            if (t >= reg.timelineStart && t < reg.timelineStart + reg.lengthInSamples)
                return i;
        }
        return -1;
    }

    // The visible width in samples, measured across the whole editor the way the JUCE
    // editor measured it, so a pan step and the chase jump land where they did.
    std::int64_t viewSamples() const
    {
        return static_cast<std::int64_t> (std::llround (
            static_cast<double> (layout.body.width() / layout.scale)
            / std::max (1.0e-9, static_cast<double> (pixelsPerSample))));
    }

    std::int64_t maxScroll() const
    {
        return std::max<std::int64_t> (0, anchorLength - viewSamples());
    }

    Box fadeInDisc() const
    {
        const auto* r = region();
        const float x = xForFileSample (r->sourceOffset + r->fadeInSamples);
        return { x - layout.s (9.0f), layout.wave.y0 + layout.s (4.0f),
                 x + layout.s (9.0f), layout.wave.y0 + layout.s (22.0f) };
    }

    Box fadeOutDisc() const
    {
        const auto* r = region();
        const float x = xForFileSample (r->sourceOffset + r->lengthInSamples - r->fadeOutSamples);
        return { x - layout.s (9.0f), layout.wave.y0 + layout.s (4.0f),
                 x + layout.s (9.0f), layout.wave.y0 + layout.s (22.0f) };
    }

    Box trimStart() const
    {
        const float x = xForFileSample (region()->sourceOffset);
        return { x - layout.s (4.0f), layout.wave.y0 + layout.s (18.0f),
                 x + layout.s (4.0f), layout.wave.y1 - layout.s (4.0f) };
    }

    Box trimEnd() const
    {
        const auto* r = region();
        const float x = xForFileSample (r->sourceOffset + r->lengthInSamples);
        return { x - layout.s (4.0f), layout.wave.y0 + layout.s (18.0f),
                 x + layout.s (4.0f), layout.wave.y1 - layout.s (4.0f) };
    }

    // 0 dB sits mid-lane; +-24 dB spans half the lanes' height.
    float gainLineY() const
    {
        const float frac = std::clamp (region()->gainDb, -24.0f, 12.0f) / 24.0f;
        return layout.lanes.at (0.0f, 0.5f).y - std::round (frac * layout.lanes.height() * 0.5f);
    }

    Source* sourceFor (const std::string& path)
    {
        for (auto& source : sources)
            if (source->path == path)
            {
                source->lastUsed = frame;
                return source.get();
            }

        if (sources.size() >= kMaxSources)
        {
            auto oldest = sources.end();
            for (auto it = sources.begin(); it != sources.end(); ++it)
                if ((*it)->lastUsed != frame
                    && (oldest == sources.end() || (*it)->lastUsed < (*oldest)->lastUsed))
                    oldest = it;
            if (oldest == sources.end())
                return nullptr;
            sources.erase (oldest);
        }

        auto source = std::make_unique<Source>();
        source->path = path;
        source->source = std::make_unique<WaveformSource>();
        source->source->setFile (std::filesystem::u8path (path));
        source->lastUsed = frame;
        sources.push_back (std::move (source));
        return sources.back().get();
    }

    Source* focusedSource() const
    {
        const auto* r = region();
        if (r == nullptr)
            return nullptr;
        const auto path = r->file.getFullPathName().toStdString();
        for (const auto& source : sources)
            if (source->path == path)
                return source.get();
        return nullptr;
    }

    // Region positions were stored at the recording's rate and the waveform is drawn in
    // the file's own, so the grid and the readouts use the file's rate too - the device
    // rate would drift them against the audio after a hot-swap to another rate.
    double sampleRate() const
    {
        if (const auto* source = focusedSource(); source != nullptr && source->snapshot.info)
            if (source->snapshot.info->sampleRate > 0.0)
                return source->snapshot.info->sampleRate;
        return std::max (1.0, engine.getCurrentSampleRate());
    }

    void zoomFit()
    {
        const auto* r = region();
        if (r == nullptr || r->lengthInSamples <= 0)
            return;

        std::int64_t lo = r->timelineStart;
        std::int64_t hi = r->timelineStart + r->lengthInSamples;
        for (const auto& reg : trackRegions())
        {
            if (reg.lengthInSamples <= 0) continue;
            lo = std::min (lo, reg.timelineStart);
            hi = std::max (hi, reg.timelineStart + reg.lengthInSamples);
        }
        anchorStart = lo;
        anchorLength = std::max<std::int64_t> (1, hi - lo);

        const float width = std::max (1.0f, layout.wave.width() / layout.scale - 2.0f * kLaneInset);
        pixelsPerSample = width / static_cast<float> (std::max<std::int64_t> (1, r->lengthInSamples));
        const auto fitSamples = static_cast<std::int64_t> (std::llround (width / pixelsPerSample));
        scrollSamples = std::clamp<std::int64_t> (r->timelineStart - anchorStart, 0,
                                                  std::max<std::int64_t> (0, anchorLength - fitSamples));
        editCursorSample = r->sourceOffset;
    }

    void clampScroll()
    {
        scrollSamples = std::clamp<std::int64_t> (scrollSamples, 0,
                                                  std::max<std::int64_t> (0, anchorLength - 1));
    }

    void zoomAround (float x, float factor)
    {
        const auto before = timelineForX (x);
        pixelsPerSample = std::clamp (pixelsPerSample * factor, kMinPixelsPerSample, kMaxPixelsPerSample);
        scrollSamples += before - timelineForX (x);
        clampScroll();
    }

    void zoomOnCursor (float factor)
    {
        if (region() == nullptr)
            return;
        zoomAround (xForFileSample (editCursorSample), factor);
    }

    void panBy (std::int64_t samples)
    {
        scrollSamples = std::clamp<std::int64_t> (scrollSamples + samples, 0, maxScroll());
    }

    // Keys wait while a menu or a text field has the keyboard. The editor's own drag
    // holds an item active, which would otherwise read as "busy" too.
    void handleKeys (const dw::Context& ctx)
    {
        const auto& io = ImGui::GetIO();
        keysAvailable = editing == Field::none && ! popupWasOpen && ! io.WantTextInput
                     && (dw::shortcutsAvailable (ctx) || gestureActive);
        if (! keysAvailable)
            return;
        for (const auto& entry : kEditorKeys)
            if (ImGui::IsKeyPressed (entry.key, entry.repeats))
                handleKey ({ entry.key, io.KeyCtrl, io.KeySuper, io.KeyShift, io.KeyAlt });
    }

    bool handleKey (const KeyChord& k)
    {
        const bool command = k.command();
        const bool bare = ! command && ! k.shift && ! k.alt;
        const auto is = [&k] (ImGuiKey key) { return k.key == key; };

        if (is (ImGuiKey_Escape))
        {
            if (! bare)
                return false;
            if (rangeActive) rangeActive = false;
            else if (! additional.empty()) additional.clear();
            else dismissRequested = true;
            return true;
        }

        if (command && (is (ImGuiKey_LeftArrow) || is (ImGuiKey_RightArrow)))
        {
            const auto step = std::max<std::int64_t> (1, viewSamples() / 4);
            panBy (is (ImGuiKey_LeftArrow) ? -step : step);
            return true;
        }
        if (bare && is (ImGuiKey_Home))
        {
            scrollSamples = 0;
            return true;
        }
        if (bare && is (ImGuiKey_End))
        {
            scrollSamples = maxScroll();
            return true;
        }

        // R and C are the shell's record and click keys; inside the editor they pick
        // tools, as G does everywhere.
        if (bare && (is (ImGuiKey_G) || is (ImGuiKey_R) || is (ImGuiKey_C)))
        {
            session.editMode = is (ImGuiKey_G) ? EditMode::Grab
                             : is (ImGuiKey_R) ? EditMode::Range : EditMode::Cut;
            return true;
        }

        if (command && is (ImGuiKey_Z))
        {
            undoStep (k.shift);
            return true;
        }

        if (command && (is (ImGuiKey_RightBracket) || is (ImGuiKey_LeftBracket)))
        {
            if (host.navigateToRegion)
                if (const int index = neighbourRegion (is (ImGuiKey_RightBracket) ? 1 : -1); index >= 0)
                    host.navigateToRegion (trackIdx, index);
            return true;
        }

        auto& transport = engine.getTransport();
        if (! command && ! k.alt)
        {
            if (is (ImGuiKey_L) && ! k.shift)
            {
                transport.setLoopEnabled (! transport.isLoopEnabled());
                return true;
            }
            if (is (ImGuiKey_P) && ! k.shift)
            {
                transport.setPunchEnabled (! transport.isPunchEnabled());
                return true;
            }
            const auto* r = region();
            if (r != nullptr && (is (ImGuiKey_LeftBracket) || is (ImGuiKey_RightBracket)))
            {
                const bool punch = k.shift;
                const auto cursor = editCursorSample + (r->timelineStart - r->sourceOffset);
                if (is (ImGuiKey_LeftBracket))
                {
                    if (punch) transport.placePunchRange (cursor, std::max (transport.getPunchOut(), cursor));
                    else       transport.placeLoopRange (cursor, std::max (transport.getLoopEnd(), cursor));
                }
                else
                {
                    // An unset partner reads as 0, so ']' alone drops a zero-width marker
                    // at the cursor rather than making a range from the session start.
                    auto start = punch ? transport.getPunchIn() : transport.getLoopStart();
                    if (start == 0) start = cursor;
                    if (punch) transport.placePunchRange (std::min (start, cursor), cursor);
                    else       transport.placeLoopRange (std::min (start, cursor), cursor);
                }
                return true;
            }
        }

        if (command && is (ImGuiKey_E))
        {
            if (rangeActive) splitRange();
            else             splitAtCursor();
            return true;
        }

        if (! command && ! k.alt && is (ImGuiKey_F) && rangeActive)
        {
            fadeToSelection (k.shift);
            return true;
        }

        if (command && is (ImGuiKey_C))
        {
            copyToClipboard();
            return true;
        }
        if (command && is (ImGuiKey_X))
        {
            cutToClipboard();
            return true;
        }
        if (command && is (ImGuiKey_V))
        {
            pasteFromClipboard();
            return true;
        }

        if (bare && (is (ImGuiKey_Delete) || is (ImGuiKey_Backspace)))
        {
            deleteSelection();
            return true;
        }

        if (! command && ! k.alt && (is (ImGuiKey_LeftArrow) || is (ImGuiKey_RightArrow)))
        {
            nudge (is (ImGuiKey_LeftArrow) ? -1 : 1, k.shift);
            return true;
        }

        if (! command && ! k.alt)
        {
            if (is (ImGuiKey_Equal) || is (ImGuiKey_KeypadAdd))
            {
                zoomOnCursor (kZoomStep);
                return true;
            }
            if (is (ImGuiKey_Minus) || is (ImGuiKey_KeypadSubtract))
            {
                zoomOnCursor (1.0f / kZoomStep);
                return true;
            }
        }
        if (bare && is (ImGuiKey_0))
        {
            zoomFit();
            return true;
        }
        return false;
    }

    void undoStep (bool redo)
    {
        auto& undo = engine.getUndoManager();
        if (redo) undo.redo();
        else      undo.undo();
        rangeActive = false;
        additional.clear();
        // An undone split can leave the focused index past the end of the list.
        const auto count = static_cast<int> (trackRegions().size());
        if (count == 0)
            dismissRequested = true;
        else
            regionIdx = std::clamp (regionIdx, 0, count - 1);
    }

    bool trackFrozen() const
    {
        return session.track (trackIdx).frozen.load (std::memory_order_relaxed);
    }

    // Splits at both edges of the range, the right one first so the left edge's index
    // still names the same region.
    void splitRange()
    {
        const auto* r = region();
        if (r == nullptr) return;
        const auto a = std::min (rangeStartSample, rangeEndSample);
        const auto b = std::max (rangeStartSample, rangeEndSample);
        const auto tlA = r->timelineStart + (a - r->sourceOffset);
        const auto tlB = r->timelineStart + (b - r->sourceOffset);
        auto& undo = engine.getUndoManager();
        undo.beginNewTransaction ("Split range");
        undo.perform (new SplitRegionAction (session, engine, trackIdx, regionIdx, tlB));
        undo.perform (new SplitRegionAction (session, engine, trackIdx, regionIdx, tlA));
        rangeActive = false;
    }

    void fadeToSelection (bool fadeOut)
    {
        const auto* r = region();
        if (r == nullptr) return;
        const auto length = std::abs (rangeEndSample - rangeStartSample);
        const AudioRegion before = *r;
        AudioRegion after = before;
        if (fadeOut)
            after.fadeOutSamples = std::min (length, std::max<std::int64_t> (0, r->lengthInSamples - r->fadeInSamples));
        else
            after.fadeInSamples = std::min (length, std::max<std::int64_t> (0, r->lengthInSamples - r->fadeOutSamples));
        commit (fadeOut ? "Fade-out to selection" : "Fade-in to selection", before, after);
    }

    // The range as a free-standing slice of the same file: no fades of its own and no
    // takes, since a paste places it anew.
    AudioRegion rangeChunk (const AudioRegion& r) const
    {
        AudioRegion chunk = r;
        chunk.sourceOffset = std::min (rangeStartSample, rangeEndSample);
        chunk.lengthInSamples = std::abs (rangeEndSample - rangeStartSample);
        chunk.timelineStart = 0;
        chunk.fadeInSamples = 0;
        chunk.fadeOutSamples = 0;
        chunk.previousTakes.clear();
        return chunk;
    }

    void copyToClipboard()
    {
        const auto* r = region();
        if (r == nullptr) return;
        auto& clip = engine.getRegionClipboard();
        clip.region = rangeActive ? rangeChunk (*r) : *r;
        clip.sourceTrack = trackIdx;
        clip.hasContent = true;
    }

    void cutToClipboard()
    {
        const auto* r = region();
        if (r == nullptr) return;
        if (cutRange())
            return;
        // A range cutRange refused - locked or frozen - is a no-op, not a licence to
        // cut the whole region.
        if (rangeActive || r->locked || trackFrozen())
            return;

        auto& clip = engine.getRegionClipboard();
        clip.region = *r;
        clip.sourceTrack = trackIdx;
        clip.hasContent = true;
        auto& undo = engine.getUndoManager();
        undo.beginNewTransaction ("Cut region");
        undo.perform (new DeleteRegionAction (session, engine, trackIdx, regionIdx));
        dismissRequested = true;
    }

    void pasteFromClipboard()
    {
        auto& clip = engine.getRegionClipboard();
        if (! clip.hasContent) return;
        AudioRegion pasted = clip.region;
        if (const auto* r = region())
            pasted.timelineStart = r->timelineStart + (editCursorSample - r->sourceOffset);
        else
            pasted.timelineStart = engine.getTransport().getPlayhead();
        const int target = clip.sourceTrack >= 0 && clip.sourceTrack < Session::kNumTracks
                         ? clip.sourceTrack : trackIdx;
        auto& undo = engine.getUndoManager();
        undo.beginNewTransaction ("Paste region");
        undo.perform (new PasteRegionAction (session, engine, target, pasted));
    }

    // Removes the range from the focused region: split at whichever edges are inside
    // it, since a split on the region's own edge does nothing, then delete the middle.
    void deleteRange (const char* transaction)
    {
        const auto* r = region();
        const auto a = std::min (rangeStartSample, rangeEndSample);
        const auto b = std::max (rangeStartSample, rangeEndSample);
        const auto regionStart = r->timelineStart;
        const auto regionEnd = r->timelineStart + r->lengthInSamples;
        const auto tlA = r->timelineStart + (a - r->sourceOffset);
        const auto tlB = r->timelineStart + (b - r->sourceOffset);
        const bool needLeft = tlA > regionStart;
        const bool needRight = tlB < regionEnd;

        auto& undo = engine.getUndoManager();
        undo.beginNewTransaction (transaction);
        if (needRight)
            undo.perform (new SplitRegionAction (session, engine, trackIdx, regionIdx, tlB));
        int doomed = regionIdx;
        if (needLeft)
        {
            undo.perform (new SplitRegionAction (session, engine, trackIdx, regionIdx, tlA));
            doomed = regionIdx + 1;
        }
        undo.perform (new DeleteRegionAction (session, engine, trackIdx, doomed));
        rangeActive = false;
        reanchorOrClose();
    }

    bool cutRange()
    {
        const auto* r = region();
        if (r == nullptr || ! rangeActive || r->locked || trackFrozen())
            return false;
        if (std::max (rangeStartSample, rangeEndSample) <= std::min (rangeStartSample, rangeEndSample))
            return false;
        auto& clip = engine.getRegionClipboard();
        clip.region = rangeChunk (*r);
        clip.sourceTrack = trackIdx;
        clip.hasContent = true;
        deleteRange ("Cut chunk");
        return true;
    }

    void deleteSelection()
    {
        const auto* r = region();
        if (rangeActive && r != nullptr)
        {
            if (! r->locked && ! trackFrozen()
                && std::max (rangeStartSample, rangeEndSample) > std::min (rangeStartSample, rangeEndSample))
                deleteRange ("Delete chunk");
            return;
        }

        // Highest index first, so each delete leaves the indices still to go intact.
        std::vector<int> doomed = additional;
        doomed.push_back (regionIdx);
        std::sort (doomed.begin(), doomed.end(), std::greater<int>());
        doomed.erase (std::unique (doomed.begin(), doomed.end()), doomed.end());
        auto& undo = engine.getUndoManager();
        undo.beginNewTransaction (doomed.size() > 1 ? "Delete regions" : "Delete region");
        for (const int index : doomed)
            undo.perform (new DeleteRegionAction (session, engine, trackIdx, index));
        reanchorOrClose();
    }

    // After a delete, stay on a surviving region with the cursor inside it, or close
    // when the track has none left.
    void reanchorOrClose()
    {
        additional.clear();
        const int total = static_cast<int> (trackRegions().size());
        if (total <= 0)
        {
            dismissRequested = true;
            return;
        }
        regionIdx = std::clamp (regionIdx, 0, total - 1);
        if (const auto* r = region())
            editCursorSample = std::clamp (editCursorSample, r->sourceOffset, r->sourceOffset + r->lengthInSamples);
    }

    // One beat, or one bar with Shift, for the focused region and every selected one.
    void nudge (int direction, bool bar)
    {
        if (region() == nullptr) return;
        const double bpm = std::max (1.0, static_cast<double> (session.tempoBpm.load (std::memory_order_relaxed)));
        const int beatsPerBar = std::max (1, session.beatsPerBar.load (std::memory_order_relaxed));
        const auto beat = static_cast<std::int64_t> (std::llround (sampleRate() * 60.0 / bpm));
        const auto delta = direction * (bar ? beat * beatsPerBar : beat);

        auto& undo = engine.getUndoManager();
        undo.beginNewTransaction (delta < 0 ? "Nudge region left" : "Nudge region right");
        const auto nudgeOne = [&] (int index)
        {
            auto& regions = session.track (trackIdx).regions;
            if (index < 0 || index >= static_cast<int> (regions.size())) return;
            const AudioRegion before = regions[static_cast<std::size_t> (index)];
            if (before.locked) return;
            AudioRegion after = before;
            after.timelineStart = std::max<std::int64_t> (0, before.timelineStart + delta);
            if (after.timelineStart == before.timelineStart) return;
            undo.perform (new RegionEditAction (session, engine, trackIdx, index, before, after));
        };
        nudgeOne (regionIdx);
        for (const int index : additional)
            nudgeOne (index);
    }

    void joinSelected()
    {
        if (additional.empty()) return;
        std::vector<int> indices = additional;
        indices.push_back (regionIdx);
        auto& undo = engine.getUndoManager();
        undo.beginNewTransaction (("Join " + std::to_string (indices.size()) + " regions").c_str());
        undo.perform (new JoinRegionsAction (session, engine, trackIdx, indices));
        additional.clear();
        // The joined region takes the lowest of the indices it replaced.
        const int lowest = *std::min_element (indices.begin(), indices.end());
        const int total = static_cast<int> (trackRegions().size());
        regionIdx = std::clamp (lowest, 0, std::max (0, total - 1));
    }

    void reverseRegion()
    {
        const auto* r = region();
        if (r == nullptr || r->locked || trackFrozen()) return;
        auto& undo = engine.getUndoManager();
        undo.beginNewTransaction ("Reverse region");
        undo.perform (new ReverseRegionAction (session, engine, trackIdx, regionIdx));
    }

    template <typename Edit>
    void editFocused (const char* transaction, Edit&& edit)
    {
        const auto* r = region();
        if (r == nullptr) return;
        const AudioRegion before = *r;
        AudioRegion after = before;
        edit (after);
        commit (transaction, before, after);
    }

    void setColour (std::uint32_t argb)
    {
        const auto* r = region();
        const RegionColour colour (argb);
        if (r == nullptr || r->customColour == colour) return;
        editFocused ("Set region colour", [&colour] (AudioRegion& a) { a.customColour = colour; });
    }

    void deleteFocused()
    {
        if (region() == nullptr) return;
        auto& undo = engine.getUndoManager();
        undo.beginNewTransaction ("Delete region");
        undo.perform (new DeleteRegionAction (session, engine, trackIdx, regionIdx));
        dismissRequested = true;
    }

    // Brings every overlap on the track to a matching pair of auto-fades, and retracts
    // the auto-fades whose overlap is gone. A fade the user set is left alone. Each
    // change joins the caller's undo transaction.
    void syncAutoCrossfades()
    {
        auto& regions = session.track (trackIdx).regions;
        std::vector<int> order (regions.size());
        for (int i = 0; i < static_cast<int> (order.size()); ++i)
            order[static_cast<std::size_t> (i)] = i;
        std::sort (order.begin(), order.end(), [&regions] (int a, int b)
        {
            return regions[static_cast<std::size_t> (a)].timelineStart < regions[static_cast<std::size_t> (b)].timelineStart;
        });
        const auto overlap = [] (const AudioRegion& a, const AudioRegion& b) -> std::int64_t
        {
            const auto aEnd = a.timelineStart + a.lengthInSamples;
            if (aEnd <= b.timelineStart) return 0;
            return std::min (aEnd - b.timelineStart, std::min (a.lengthInSamples, b.lengthInSamples));
        };
        const auto fit = [] (bool& automatic, std::int64_t& samples, FadeShape& shape, std::int64_t against)
        {
            if (! automatic && samples != 0)
                return;
            if (against > 0)
            {
                samples = against;
                if (shape == FadeShape::Linear) shape = FadeShape::EqualPower;
                automatic = true;
            }
            else if (automatic)
            {
                samples = 0;
                shape = FadeShape::Linear;
                automatic = false;
            }
        };

        auto& undo = engine.getUndoManager();
        for (std::size_t pos = 0; pos < order.size(); ++pos)
        {
            const int index = order[pos];
            auto& self = regions[static_cast<std::size_t> (index)];
            if (self.locked) continue;
            const auto overlapPrev = pos > 0 ? overlap (regions[static_cast<std::size_t> (order[pos - 1])], self) : 0;
            const auto overlapNext = pos + 1 < order.size()
                                   ? overlap (self, regions[static_cast<std::size_t> (order[pos + 1])]) : 0;

            const AudioRegion before = self;
            AudioRegion after = before;
            fit (after.fadeInAuto, after.fadeInSamples, after.fadeInShape, overlapPrev);
            fit (after.fadeOutAuto, after.fadeOutSamples, after.fadeOutShape, overlapNext);
            after.fadeInSamples = clampTo (after.fadeInSamples, 0, after.lengthInSamples);
            after.fadeOutSamples = clampTo (after.fadeOutSamples, 0, after.lengthInSamples - after.fadeInSamples);

            if (after.fadeInSamples == before.fadeInSamples && after.fadeOutSamples == before.fadeOutSamples
                && after.fadeInShape == before.fadeInShape && after.fadeOutShape == before.fadeOutShape
                && after.fadeInAuto == before.fadeInAuto && after.fadeOutAuto == before.fadeOutAuto)
                continue;
            undo.perform (new RegionEditAction (session, engine, trackIdx, index, before, after));
        }
    }

    // The region after (+1) or before (-1) the focused one in time, ties broken by
    // storage order; -1 at either end, which does not wrap.
    int neighbourRegion (int direction) const
    {
        const auto& regions = trackRegions();
        if (regionIdx < 0 || regionIdx >= static_cast<int> (regions.size())) return -1;
        using Key = std::pair<std::int64_t, int>;
        const Key current { regions[static_cast<std::size_t> (regionIdx)].timelineStart, regionIdx };
        int found = -1;
        Key best {};
        for (int i = 0; i < static_cast<int> (regions.size()); ++i)
        {
            const Key key { regions[static_cast<std::size_t> (i)].timelineStart, i };
            const bool beyond = direction > 0 ? key > current : key < current;
            const bool closer = found < 0 || (direction > 0 ? key < best : key > best);
            if (beyond && closer)
            {
                best = key;
                found = i;
            }
        }
        return found;
    }

    void handleWheel()
    {
        const auto& io = ImGui::GetIO();
        if (popupWasOpen || region() == nullptr || pixelsPerSample <= 0.0f
            || ! layout.body.contains (io.MousePos) || ! ImGui::IsWindowHovered())
            return;
        if (! nonZero (io.MouseWheel) && ! nonZero (io.MouseWheelH))
            return;

        if (io.KeyCtrl || io.KeySuper)
        {
            zoomAround (io.MousePos.x, std::pow (kZoomStep, io.MouseWheel));
            return;
        }

        const float wheel = nonZero (io.MouseWheelH) ? io.MouseWheelH : io.MouseWheel;
        panRemainder += -wheel * kWheelPanPixels;
        const float whole = std::trunc (panRemainder);
        panRemainder -= whole;
        scrollSamples = std::clamp<std::int64_t> (
            scrollSamples + static_cast<std::int64_t> (std::llround (whole / std::max (1.0e-5f, pixelsPerSample))),
            0, std::max<std::int64_t> (0, anchorLength - 1));
    }

    // Keeps a playing playhead in view by jumping it to the left quarter once it leaves.
    void followPlayhead()
    {
        auto& transport = engine.getTransport();
        if (! chase || ! transport.isPlaying() || pixelsPerSample <= 0.0f || anchorLength <= 0)
            return;
        const auto rel = transport.getPlayhead() - anchorStart;
        if (rel < 0 || rel >= anchorLength || playheadX() >= 0.0f)
            return;
        scrollSamples = std::clamp<std::int64_t> (rel - viewSamples() / 4, 0, maxScroll());
    }

    // The ruler and the waveform are one gesture surface. Dear ImGui tracks the press
    // so the toolbar cannot take a drag that wanders over it; which gesture a press
    // starts is decided here, in the order the JUCE editor's mouseDown decided it.
    void handlePointer()
    {
        const Box area { layout.ruler.x0, layout.ruler.y0, layout.wave.x1, layout.wave.y1 };
        ImGui::SetCursorScreenPos (area.tl());
        ImGui::InvisibleButton ("##gesture", ImVec2 (std::max (1.0f, area.width()), std::max (1.0f, area.height())),
                                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight
                                    | ImGuiButtonFlags_MouseButtonMiddle);
        gestureActive = ImGui::IsItemActive();

        const auto& io = ImGui::GetIO();
        // A press that closes an open menu only closes it.
        if (ImGui::IsItemActivated() && ! popupWasOpen)
            for (const auto button : { ImGuiMouseButton_Left, ImGuiMouseButton_Right, ImGuiMouseButton_Middle })
                if (io.MouseClicked[button])
                {
                    pointerDown (button, io.MousePos);
                    break;
                }

        if (drag == Drag::none)
            return;
        if (ImGui::IsMousePosValid() && (nonZero (io.MousePos.x - dragLast.x) || nonZero (io.MousePos.y - dragLast.y)))
        {
            dragLast = io.MousePos;
            pointerDrag (io.MousePos);
        }
        if (! io.MouseDown[dragButton])
            pointerUp();
    }

    Drag loopPunchEdgeAt (float x) const
    {
        const auto& transport = engine.getTransport();
        const float tolerance = layout.s (6.0f);
        const auto near = [&] (std::int64_t sample) { return std::abs (x - xForTimeline (sample)) <= tolerance; };
        if (transport.getLoopEnd() > transport.getLoopStart())
        {
            if (near (transport.getLoopStart())) return Drag::loopIn;
            if (near (transport.getLoopEnd())) return Drag::loopOut;
        }
        if (transport.getPunchOut() > transport.getPunchIn())
        {
            if (near (transport.getPunchIn())) return Drag::punchIn;
            if (near (transport.getPunchOut())) return Drag::punchOut;
        }
        return Drag::none;
    }

    bool overGainLine (ImVec2 p) const
    {
        return layout.wave.contains (p) && std::abs (p.y - gainLineY()) <= layout.s (4.0f);
    }

    void pointerDown (ImGuiMouseButton button, ImVec2 p)
    {
        auto* r = region();
        if (r == nullptr)
            return;
        const auto& io = ImGui::GetIO();
        const bool command = io.KeyCtrl || io.KeySuper;
        const bool inWave = layout.wave.contains (p);
        const bool inRuler = layout.ruler.contains (p);
        dragButton = button;
        dragDown = dragLast = p;

        if (button == ImGuiMouseButton_Middle)
        {
            if (inWave)
            {
                drag = Drag::pan;
                panStartScroll = scrollSamples;
            }
            return;
        }

        if (button == ImGuiMouseButton_Left && inRuler)
            if (const auto edge = loopPunchEdgeAt (p.x); edge != Drag::none)
            {
                drag = edge;
                return;
            }

        if (button == ImGuiMouseButton_Right)
        {
            if (! r->locked && (fadeInDisc().contains (p) || fadeOutDisc().contains (p)))
            {
                fadeMenuIsIn = fadeInDisc().contains (p);
                ImGui::OpenPopup (kFadeMenu);
                return;
            }
            if (inWave)
                editCursorSample = snapFileSample (fileSampleForX (p.x), command);
            ImGui::OpenPopup (kContextMenu);
            return;
        }

        // A ruler click seeks, and dragging on from it selects a range.
        if (inRuler)
        {
            engine.getTransport().locate (snapTimelineSample (timelineForX (p.x), command));
            rangeStartSample = rangeEndSample = snapFileSample (fileSampleForX (p.x), command);
            rangeActive = false;
            drag = Drag::range;
            return;
        }

        regionAtDragStart = *r;
        dragOriginGainDb = r->gainDb;
        if (! r->locked)
        {
            if (fadeInDisc().contains (p)) drag = Drag::fadeIn;
            else if (fadeOutDisc().contains (p)) drag = Drag::fadeOut;
            else if (trimStart().contains (p)) drag = Drag::trimStart;
            else if (trimEnd().contains (p)) drag = Drag::trimEnd;
            else if (overGainLine (p)) drag = Drag::gain;
            if (drag != Drag::none)
                return;
        }

        // Draw is the automation pencil and never moves the region.
        if (session.editMode == EditMode::Draw || ! inWave)
            return;
        bodyDown (p, command, io.KeyShift);
    }

    void bodyDown (ImVec2 p, bool command, bool shift)
    {
        const int hit = regionIndexAtX (p.x);
        const auto& regions = trackRegions();

        if (command && hit >= 0)
        {
            if (hit != regionIdx)
            {
                const auto it = std::find (additional.begin(), additional.end(), hit);
                if (it != additional.end()) additional.erase (it);
                else                        additional.push_back (hit);
            }
            return;
        }

        // Shift-click selects every region between the focused one and the one clicked.
        if (shift && hit >= 0 && hit != regionIdx)
        {
            const auto anchor = regions[static_cast<std::size_t> (regionIdx)].timelineStart;
            const auto clicked = regions[static_cast<std::size_t> (hit)].timelineStart;
            additional.clear();
            for (int i = 0; i < static_cast<int> (regions.size()); ++i)
            {
                const auto t = regions[static_cast<std::size_t> (i)].timelineStart;
                if (i != regionIdx && t >= std::min (anchor, clicked) && t <= std::max (anchor, clicked))
                    additional.push_back (i);
            }
            return;
        }

        const auto mode = session.editMode;
        // Cut splits the region under the click, which after one cut need not be the
        // focused one.
        if (mode == EditMode::Cut && hit >= 0)
        {
            if (! regions[static_cast<std::size_t> (hit)].locked)
            {
                const auto at = snapTimelineSample (timelineForX (p.x), command);
                auto& undo = engine.getUndoManager();
                undo.beginNewTransaction ("Split region");
                undo.perform (new SplitRegionAction (session, engine, trackIdx, hit, at));
                regionIdx = hit;
            }
            return;
        }

        if (mode == EditMode::Range || shift || hit < 0)
        {
            // The range belongs to the slice it was taken on, so focus that one first.
            if (mode == EditMode::Range && hit >= 0 && hit != regionIdx)
            {
                additional.clear();
                regionIdx = hit;
                regionAtDragStart = *region();
                editCursorSample = snapFileSample (fileSampleForX (p.x), command);
            }
            rangeStartSample = rangeEndSample = fileSampleForX (p.x);
            rangeActive = false;
            drag = Drag::range;
            return;
        }

        const bool clickedSelected = hit == regionIdx
                                  || std::find (additional.begin(), additional.end(), hit) != additional.end();
        if (! clickedSelected)
            additional.clear();

        // A press on another slice focuses it and picks it up to move; on the focused
        // slice it drops the cursor and only becomes a move once the pointer travels.
        if (hit != regionIdx)
        {
            regionIdx = hit;
            const auto* r = region();
            regionAtDragStart = *r;
            dragOriginGainDb = r->gainDb;
            editCursorSample = r->sourceOffset
                             + std::clamp<std::int64_t> (timelineForX (p.x) - r->timelineStart, 0, r->lengthInSamples);
            drag = Drag::moveRegion;
        }
        else
        {
            editCursorSample = snapFileSample (fileSampleForX (p.x), command);
            drag = Drag::moveCursor;
        }
        dragOriginTimeline = region()->timelineStart;
        additionalOrigins.clear();
        for (const int index : additional)
            additionalOrigins.push_back (index >= 0 && index < static_cast<int> (regions.size())
                                         ? regions[static_cast<std::size_t> (index)].timelineStart : 0);
    }

    void pointerDrag (ImVec2 p)
    {
        auto* r = region();
        if (r == nullptr)
            return;
        const auto& io = ImGui::GetIO();
        const bool bypass = io.KeyCtrl || io.KeySuper;
        const bool snapping = ! bypass && session.audioEditorSnap;

        if (drag == Drag::loopIn || drag == Drag::loopOut || drag == Drag::punchIn || drag == Drag::punchOut)
        {
            auto& transport = engine.getTransport();
            const auto t = std::max<std::int64_t> (0, snapTimelineSample (timelineForX (p.x), bypass));
            if (drag == Drag::loopIn)       transport.setLoopRange (std::min (t, transport.getLoopEnd()), transport.getLoopEnd());
            else if (drag == Drag::loopOut) transport.setLoopRange (transport.getLoopStart(), std::max (t, transport.getLoopStart()));
            else if (drag == Drag::punchIn) transport.setPunchRange (std::min (t, transport.getPunchOut()), transport.getPunchOut());
            else                            transport.setPunchRange (transport.getPunchIn(), std::max (t, transport.getPunchIn()));
            return;
        }

        if (drag == Drag::pan)
        {
            const auto samples = static_cast<std::int64_t> (std::llround (
                -(p.x - dragDown.x) / std::max (1.0e-5f, pixelsPerSampleOnScreen())));
            scrollSamples = std::clamp<std::int64_t> (panStartScroll + samples, 0,
                                                      std::max<std::int64_t> (0, anchorLength - 1));
            return;
        }

        const double sr = sampleRate();
        // Snaps in timeline samples, where the ruler is read, and leaves the guide on
        // the grid line it landed on.
        const auto snapToGrid = [&] (std::int64_t fileSample)
        {
            snapGuide = -1;
            if (! snapping)
                return fileSample;
            const auto fileToTimeline = r->timelineStart - r->sourceOffset;
            const auto t = fileSample + fileToTimeline;
            const auto snapped = snap::snapAbsoluteToGridUnchecked (t, session, sr);
            if (snapped != t)
                snapGuide = snapped;
            return snapped - fileToTimeline;
        };

        if (drag == Drag::moveCursor)
        {
            if (r->locked || std::abs (p.x - dragDown.x) <= layout.s (3.0f))
            {
                editCursorSample = fileSampleForX (p.x);
                return;
            }
            drag = Drag::moveRegion;
        }

        if (drag == Drag::moveRegion)
        {
            if (r->locked)
                return;
            const auto rawDelta = timelineForX (p.x) - timelineForX (dragDown.x);
            const auto delta = snapping ? snap::snapDeltaToGridUnchecked (rawDelta, session, sr) : rawDelta;
            r->timelineStart = std::max<std::int64_t> (0, dragOriginTimeline + delta);
            snapGuide = snapping && delta != rawDelta ? r->timelineStart : -1;
            auto& regions = session.track (trackIdx).regions;
            for (std::size_t i = 0; i < additional.size() && i < additionalOrigins.size(); ++i)
            {
                const int index = additional[i];
                if (index < 0 || index >= static_cast<int> (regions.size())) continue;
                auto& other = regions[static_cast<std::size_t> (index)];
                if (! other.locked)
                    other.timelineStart = std::max<std::int64_t> (0, additionalOrigins[i] + delta);
            }
            return;
        }

        if (drag == Drag::range)
        {
            rangeEndSample = snapToGrid (fileSampleForX (p.x));
            rangeActive = rangeEndSample != rangeStartSample;
            return;
        }

        switch (drag)
        {
            case Drag::fadeIn:
                r->fadeInSamples = clampTo (snapToGrid (fileSampleForX (p.x)) - r->sourceOffset, 0,
                                            std::max<std::int64_t> (0, r->lengthInSamples - r->fadeOutSamples));
                snapGuide = r->timelineStart + r->fadeInSamples;
                break;
            case Drag::fadeOut:
                r->fadeOutSamples = clampTo (r->sourceOffset + r->lengthInSamples - snapToGrid (fileSampleForX (p.x)), 0,
                                             std::max<std::int64_t> (0, r->lengthInSamples - r->fadeInSamples));
                snapGuide = r->timelineStart + r->lengthInSamples - r->fadeOutSamples;
                break;
            case Drag::gain:
                // 0.1 dB a pixel, the tape strip's Alt-drag rate.
                snapGuide = -1;
                r->gainDb = std::clamp (dragOriginGainDb + (dragDown.y - p.y) / layout.scale * 0.1f, -24.0f, 12.0f);
                break;
            case Drag::trimStart:
            {
                // The start slides along the file, so the audio under it stays put.
                const auto& origin = regionAtDragStart;
                const auto offset = clampTo (snapToGrid (fileSampleForX (p.x)), 0,
                                             origin.sourceOffset + origin.lengthInSamples - 1);
                const auto delta = offset - origin.sourceOffset;
                r->sourceOffset = offset;
                r->lengthInSamples = origin.lengthInSamples - delta;
                r->timelineStart = origin.timelineStart + delta;
                r->fadeInSamples = clampTo (r->fadeInSamples, 0, r->lengthInSamples);
                r->fadeOutSamples = clampTo (r->fadeOutSamples, 0, r->lengthInSamples - r->fadeInSamples);
                break;
            }
            case Drag::trimEnd:
            {
                const auto length = clampTo (snapToGrid (fileSampleForX (p.x)) - regionAtDragStart.sourceOffset, 1,
                                             std::numeric_limits<std::int64_t>::max());
                r->lengthInSamples = length;
                r->fadeInSamples = clampTo (r->fadeInSamples, 0, length);
                r->fadeOutSamples = clampTo (r->fadeOutSamples, 0, length - r->fadeInSamples);
                break;
            }
            case Drag::none: case Drag::moveCursor: case Drag::range: case Drag::moveRegion: case Drag::pan:
            case Drag::loopIn: case Drag::loopOut: case Drag::punchIn: case Drag::punchOut:
                break;
        }
    }

    void pointerUp()
    {
        const auto finished = std::exchange (drag, Drag::none);
        snapGuide = -1;

        if (finished == Drag::range)
        {
            if (rangeEndSample < rangeStartSample)
                std::swap (rangeStartSample, rangeEndSample);
            rangeActive = rangeEndSample > rangeStartSample;
            return;
        }

        auto* r = region();
        if (r == nullptr || finished == Drag::pan || finished == Drag::moveCursor
            || finished == Drag::loopIn || finished == Drag::loopOut
            || finished == Drag::punchIn || finished == Drag::punchOut)
            return;

        // A fade the user dragged is theirs; the crossfade pass leaves it alone.
        AudioRegion after = *r;
        if (finished == Drag::fadeIn) after.fadeInAuto = false;
        if (finished == Drag::fadeOut) after.fadeOutAuto = false;

        const auto& before = regionAtDragStart;
        if (after.sourceOffset == before.sourceOffset && after.timelineStart == before.timelineStart
            && after.lengthInSamples == before.lengthInSamples && after.fadeInSamples == before.fadeInSamples
            && after.fadeOutSamples == before.fadeOutSamples && after.fadeInAuto == before.fadeInAuto
            && after.fadeOutAuto == before.fadeOutAuto && ! nonZero (after.gainDb - before.gainDb))
            return;

        // The action's perform applies the after state, so the live edit is rolled
        // back first and the stored before is the authoritative one.
        *r = before;
        auto& undo = engine.getUndoManager();
        undo.beginNewTransaction (finished == Drag::fadeIn    ? "Fade-in"
                                : finished == Drag::fadeOut   ? "Fade-out"
                                : finished == Drag::gain      ? "Region gain"
                                : finished == Drag::trimStart ? "Trim start"
                                : finished == Drag::trimEnd   ? "Trim end"
                                                              : "Move region");
        undo.perform (new RegionEditAction (session, engine, trackIdx, regionIdx, before, after));

        if (finished == Drag::moveRegion)
        {
            auto& regions = session.track (trackIdx).regions;
            for (std::size_t i = 0; i < additional.size() && i < additionalOrigins.size(); ++i)
            {
                const int index = additional[i];
                if (index < 0 || index >= static_cast<int> (regions.size())) continue;
                auto& other = regions[static_cast<std::size_t> (index)];
                if (other.locked || other.timelineStart == additionalOrigins[i]) continue;
                const AudioRegion otherAfter = other;
                AudioRegion otherBefore = otherAfter;
                otherBefore.timelineStart = additionalOrigins[i];
                other = otherBefore;
                undo.perform (new RegionEditAction (session, engine, trackIdx, index, otherBefore, otherAfter));
            }
        }
        if (finished == Drag::moveRegion || finished == Drag::trimStart || finished == Drag::trimEnd)
            syncAutoCrossfades();
    }

    std::int64_t snapFileSample (std::int64_t fileSample, bool bypass) const
    {
        const auto* r = region();
        if (bypass || ! session.audioEditorSnap || r == nullptr)
            return fileSample;
        const auto fileToTimeline = r->timelineStart - r->sourceOffset;
        return snap::snapAbsoluteToGridUnchecked (fileSample + fileToTimeline, session, sampleRate())
             - fileToTimeline;
    }

    std::int64_t snapTimelineSample (std::int64_t timelineSample, bool bypass) const
    {
        if (bypass || ! session.audioEditorSnap)
            return timelineSample;
        return snap::snapAbsoluteToGridUnchecked (timelineSample, session, sampleRate());
    }

    void splitAtCursor()
    {
        const auto* r = region();
        if (r == nullptr) return;
        const auto at = r->timelineStart + (editCursorSample - r->sourceOffset);
        auto& undo = engine.getUndoManager();
        undo.beginNewTransaction ("Split region");
        undo.perform (new SplitRegionAction (session, engine, trackIdx, regionIdx, at));
    }

    // Non-destructive: raises the region gain so the slice's peak lands at 0.99.
    void normalize()
    {
        const auto* r = region();
        if (r == nullptr || ! r->file.existsAsFile() || r->lengthInSamples <= 0) return;

        auto reader = dusk::audio::FileReader::open (
            std::filesystem::u8path (r->file.getFullPathName().toStdString()));
        if (reader == nullptr) return;

        const int channels = std::max (1, reader->info().numChannels);
        constexpr int kChunk = 1 << 16;
        dusk::audio::PlanarBuffer buffer;
        if (! buffer.setSize (channels, kChunk)) return;

        float peak = 0.0f;
        std::int64_t done = 0;
        while (done < r->lengthInSamples)
        {
            const auto want = std::min<std::int64_t> (kChunk, r->lengthInSamples - done);
            const auto got = static_cast<int> (reader->read (buffer.data(), channels,
                                                             r->sourceOffset + done, want));
            if (got <= 0) break;
            for (int c = 0; c < channels; ++c)
            {
                const auto range = dusk::audio::findSignedMinMax (buffer.channel (c), got);
                peak = std::max ({ peak, std::abs (range.min), std::abs (range.max) });
            }
            done += got;
        }
        if (peak <= 1.0e-6f) return;

        const AudioRegion before = *r;
        AudioRegion after = before;
        after.gainDb = std::clamp (before.gainDb + dusk::audio::gainToDecibels (0.99f / peak),
                                   -24.0f, 12.0f);
        commit ("Normalize", before, after);
    }

    void toggleMute()
    {
        const auto* r = region();
        if (r == nullptr) return;
        const AudioRegion before = *r;
        AudioRegion after = before;
        after.muted = ! before.muted;
        commit (after.muted ? "Mute region" : "Unmute region", before, after);
    }

    void toggleLock()
    {
        const auto* r = region();
        if (r == nullptr) return;
        const AudioRegion before = *r;
        AudioRegion after = before;
        after.locked = ! before.locked;
        commit (after.locked ? "Lock region" : "Unlock region", before, after);
    }

    void addControl (std::string name, const Box& box, bool enabled)
    {
        controls.push_back ({ std::move (name), box, enabled });
    }

    // A release over the control that the press also landed on.
    static bool releasedOn (bool hovered)
    {
        return hovered && ImGui::IsItemDeactivated();
    }

    static bool doubleClicked (dw::Context& ctx, const char* id, const Box& box)
    {
        return dw::hitArea (ctx, id, box.tl(), box.br()) && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left);
    }

    bool iconButton (dw::Context& ctx, const char* name, const Box& box, Glyph glyph, bool enabled)
    {
        addControl (name, box, enabled);
        char id[48];
        std::snprintf (id, sizeof (id), "##icon-%s", name);
        const bool hovered = dw::hitArea (ctx, id, box.tl(), box.br());
        const bool down = hovered && ImGui::IsItemActive();
        const bool clicked = enabled && releasedOn (hovered);

        auto* const dl = ctx.dl;
        const auto disc = box.reduced (ctx.s (2.0f), ctx.s (2.0f));
        const auto centre = disc.at (0.5f, 0.5f);
        const float radius = disc.width() * 0.5f;
        auto fill = argb (kIconDisc);
        if (enabled && down) fill = dw::brighter (fill, 0.14f);
        else if (enabled && hovered) fill = dw::brighter (fill, 0.08f);
        dl->AddCircleFilled (centre, radius, fill, 32);
        dl->AddCircle (centre, radius, argb (kIconRim), 32, ctx.s (down ? 2.0f : 1.4f));
        drawGlyph (dl, glyph, centre, disc.width() / ctx.scale * 0.30f, ctx.scale,
                   argb (kIconGlyph, enabled ? 1.0f : 0.35f));
        return clicked;
    }

    // The JUCE IconButton glyphs, in design units about the disc centre.
    static void drawGlyph (ImDrawList* dl, Glyph glyph, ImVec2 c, float r, float scale, ImU32 colour)
    {
        const auto p = [c, scale] (float dx, float dy) { return ImVec2 (c.x + dx * scale, c.y + dy * scale); };
        const auto line = [dl, &p, scale, colour] (float x0, float y0, float x1, float y1, float w)
        { dl->AddLine (p (x0, y0), p (x1, y1), colour, w * scale); };
        const auto arrowhead = [&line] (float tx, float ty, float fx, float fy, float size)
        {
            const float dx = tx - fx, dy = ty - fy;
            const float len = std::max (0.001f, std::sqrt (dx * dx + dy * dy));
            const float ux = dx / len, uy = dy / len;
            const float px = -uy, py = ux;
            line (tx - ux * size + px * size * 0.6f, ty - uy * size + py * size * 0.6f, tx, ty, 1.6f);
            line (tx - ux * size - px * size * 0.6f, ty - uy * size - py * size * 0.6f, tx, ty, 1.6f);
        };

        switch (glyph)
        {
            case Glyph::undo:
            case Glyph::redo:
            {
                // Clockwise from twelve o'clock, as the JUCE arc was specified.
                const float radius = r * 0.95f;
                const float mirror = glyph == Glyph::redo ? -1.0f : 1.0f;
                constexpr float kPi = 3.14159265f;
                std::array<ImVec2, 25> arc {};
                for (std::size_t i = 0; i < arc.size(); ++i)
                {
                    const float a = kPi * (0.30f + 1.55f * static_cast<float> (i) / static_cast<float> (arc.size() - 1));
                    arc[i] = p (mirror * std::sin (a) * radius, -std::cos (a) * radius);
                }
                dl->AddPolyline (arc.data(), static_cast<int> (arc.size()), colour, ImDrawFlags_None, 1.6f * scale);
                const float sx = mirror * std::sin (kPi * 0.30f) * radius;
                const float sy = -std::cos (kPi * 0.30f) * radius;
                arrowhead (sx, sy, sx + sx * 0.001f, sy + sy * 0.001f, 4.0f);
                break;
            }
            case Glyph::split:
            {
                const float h = r * 1.6f;
                dl->AddRectFilled (p (-0.8f, -h * 0.5f), p (0.8f, h * 0.5f), colour);
                line (-r * 0.95f, 0.0f, -2.0f, 0.0f, 1.4f);
                line (2.0f, 0.0f, r * 0.95f, 0.0f, 1.4f);
                arrowhead (-r * 0.95f, 0.0f, -2.0f, 0.0f, 3.5f);
                arrowhead (r * 0.95f, 0.0f, 2.0f, 0.0f, 3.5f);
                break;
            }
            case Glyph::normalize:
            {
                line (-r, -r * 0.85f, r, -r * 0.85f, 1.2f);
                const float xs[5] = { -r * 0.7f, -r * 0.35f, 0.0f, r * 0.35f, r * 0.7f };
                const float hs[5] = { r * 0.4f, r * 0.7f, r * 0.85f, r * 0.55f, r * 0.3f };
                for (int i = 0; i < 5; ++i)
                    dl->AddRectFilled (p (xs[i] - 0.8f, -hs[i]), p (xs[i] + 0.8f, hs[i]), colour);
                break;
            }
            case Glyph::reverse:
            {
                const float xs[4] = { -r * 0.6f, -r * 0.2f, r * 0.2f, r * 0.6f };
                const float hs[4] = { r * 0.25f, r * 0.45f, r * 0.65f, r * 0.85f };
                for (int i = 0; i < 4; ++i)
                    dl->AddRectFilled (p (xs[i] - 0.8f, -r * 0.2f - hs[i] * 0.6f),
                                       p (xs[i] + 0.8f, -r * 0.2f + hs[i] * 0.2f), colour);
                line (r * 0.9f, r * 0.55f, -r * 0.9f, r * 0.55f, 1.4f);
                arrowhead (-r * 0.9f, r * 0.55f, 0.0f, r * 0.55f, 3.5f);
                break;
            }
            case Glyph::zoomFit:
            {
                const float w = r * 0.95f;
                const float half = r * 1.10f * 0.9f * 0.5f;
                const float arm = r * 0.45f;
                for (const float side : { -1.0f, 1.0f })
                {
                    line (side * w, -half, side * w, half, 1.4f);
                    line (side * w, -half, side * (w - arm), -half, 1.4f);
                    line (side * w, half, side * (w - arm), half, 1.4f);
                }
                dl->AddCircleFilled (c, 1.5f * scale, colour, 12);
                break;
            }
            case Glyph::zoomIn:
            case Glyph::zoomOut:
            {
                const float rad = r * 0.62f;
                const float cx = -r * 0.18f, cy = -r * 0.18f;
                dl->AddCircle (p (cx, cy), rad * scale, colour, 24, 1.4f * scale);
                line (cx + rad * 0.55f, cy + rad * 0.55f, r * 0.85f, r * 0.85f, 1.8f);
                line (cx - rad * 0.5f, cy, cx + rad * 0.5f, cy, 1.4f);
                if (glyph == Glyph::zoomIn)
                    line (cx, cy - rad * 0.5f, cx, cy + rad * 0.5f, 1.4f);
                break;
            }
            case Glyph::properties:
            {
                dl->AddCircle (c, r * 0.55f * scale, colour, 24, 1.4f * scale);
                for (int i = 0; i < 6; ++i)
                {
                    const float a = 6.2831853f * static_cast<float> (i) / 6.0f;
                    line (std::cos (a) * r * 0.65f, std::sin (a) * r * 0.65f,
                          std::cos (a) * r * 0.95f, std::sin (a) * r * 0.95f, 1.4f);
                }
                dl->AddCircleFilled (c, 1.5f * scale, colour, 12);
                break;
            }
        }
    }

    bool toggleButton (dw::Context& ctx, const char* name, const Box& box, const char* label,
                       bool on, const dw::ButtonStyle& style)
    {
        addControl (name, box, true);
        char id[48];
        std::snprintf (id, sizeof (id), "##button-%s", name);
        return dw::textButton (ctx, id, box.tl(), box.br(), label, on, style).clicked;
    }

    void drawToolbar (dw::Context& ctx)
    {
        auto* const dl = ctx.dl;
        const auto& icons = layout.icons;
        dl->AddRectFilled (icons.tl(), icons.br(), argb (kIconRowFill));
        hline (dl, icons.y1 - ctx.s (1.0f), icons.x0, icons.x1, argb (kBarLine), ctx.s (1.0f));

        const bool haveRegion = region() != nullptr;
        auto& undo = engine.getUndoManager();

        auto inner = icons.reduced (ctx.s (8.0f), ctx.s (6.0f));
        const float dia = std::min (inner.height(), ctx.s (36.0f));
        const float gap = ctx.s (8.0f);
        const auto left = [&] (float w) { auto b = inner.takeLeft (w); inner.takeLeft (gap); return b; };
        const auto right = [&] (float w) { auto b = inner.takeRight (w); inner.takeRight (gap); return b; };
        const auto square = [dia] (const Box& b) { return b.sizedKeepingCentre (dia, dia); };

        if (iconButton (ctx, "Zoom fit", square (right (dia)), Glyph::zoomFit, haveRegion))
            zoomFit();
        if (iconButton (ctx, "Zoom in", square (right (dia)), Glyph::zoomIn, haveRegion))
            zoomOnCursor (kZoomStep);
        if (iconButton (ctx, "Zoom out", square (right (dia)), Glyph::zoomOut, haveRegion))
            zoomOnCursor (1.0f / kZoomStep);
        inner.takeRight (ctx.s (8.0f));

        dw::ButtonStyle chaseStyle;
        chaseStyle.offFill = argb (kChaseOff);
        chaseStyle.onFill = argb (kChaseOn);
        chaseStyle.offText = argb (kChaseTextOff);
        chaseStyle.onText = argb (kChaseTextOn);
        chaseStyle.fontSize = 11.0f;
        if (toggleButton (ctx, "Chase", right (ctx.s (56.0f)).sizedKeepingCentre (ctx.s (56.0f), dia - ctx.s (8.0f)),
                          "Chase", chase, chaseStyle))
            chase = ! chase;

        if (iconButton (ctx, "Undo", square (left (dia)), Glyph::undo, undo.canUndo()))
            undoStep (false);
        if (iconButton (ctx, "Redo", square (left (dia)), Glyph::redo, undo.canRedo()))
            undoStep (true);
        inner.takeLeft (gap);
        if (iconButton (ctx, "Split", square (left (dia)), Glyph::split, haveRegion))
            splitAtCursor();
        if (iconButton (ctx, "Normalize", square (left (dia)), Glyph::normalize, haveRegion))
            normalize();
        const auto* focused = region();
        if (iconButton (ctx, "Reverse", square (left (dia)), Glyph::reverse,
                        focused != nullptr && ! focused->locked && ! trackFrozen()))
            reverseRegion();
        const auto propertiesBox = square (left (dia));
        if (iconButton (ctx, "Properties", propertiesBox, Glyph::properties, haveRegion))
        {
            propertiesAnchor = ImVec2 (propertiesBox.x0, propertiesBox.y1);
            ImGui::OpenPopup (kPropertiesMenu);
        }
        inner.takeLeft (ctx.s (4.0f));

        drawModeGroup (ctx, inner.takeLeft (std::min (ctx.s (360.0f), std::max (0.0f, inner.width()))), dia);
        inner.takeLeft (gap);

        if (inner.width() > 0.0f)
        {
            const float w = std::min (ctx.s (130.0f), inner.width());
            dw::ButtonStyle pill;
            pill.offFill = argb (kAutoFill);
            pill.offText = argb (kReadoutText);
            pill.fontSize = 11.0f;
            const auto box = inner.takeLeft (w).sizedKeepingCentre (w, dia - ctx.s (6.0f));
            addControl ("Auto", box, true);
            toggleButton (ctx, "Auto: Off", box, "Auto: Off", false, pill);
            inner.takeLeft (gap);
        }

        if (inner.width() > 0.0f && trackIdx >= 0 && trackIdx < Session::kNumTracks)
        {
            const auto& track = session.track (trackIdx);
            const auto name = track.name.toStdString();
            const auto nameBox = inner.takeLeft (std::min (ctx.s (130.0f), inner.width() * 0.5f))
                                     .reduced (ctx.s (8.0f), ctx.s (2.0f));
            clippedText (ctx, nameBox, ctx.fonts->title, 12.5f,
                         dw::brighter (argb (track.colour.getARGB()), 0.3f), name.c_str(), dw::Align::left);

            // Read again: the buttons above may have split or undone the region.
            const auto titleBox = inner.reduced (ctx.s (8.0f), ctx.s (2.0f));
            if (! drawField (ctx, Field::title, titleBox) && ! drawField (ctx, Field::label, titleBox))
                if (const auto* r = region())
                {
                    const auto title = r->label.isNotEmpty() ? r->label.toStdString()
                                                             : r->file.getFileName().toStdString();
                    clippedText (ctx, titleBox, ctx.fonts->title, 12.5f, argb (kReadoutText), title.c_str(),
                                 dw::Align::left);
                    addControl ("Title", titleBox, true);
                    if (doubleClicked (ctx, "##title", titleBox))
                        beginEdit (Field::title, title);
                }
        }
    }

    void beginEdit (Field field, const std::string& text)
    {
        editing = field;
        fieldTakesFocus = true;
        // Opened after the fields were laid out, so it shows from the next frame.
        fieldDrawn = true;
        std::snprintf (fieldText.data(), fieldText.size(), "%s", text.c_str());
        fieldOriginal = fieldText.data();
    }

    // The inline editor over `box` while `field` is the one being edited. True if it
    // drew, so the caller skips the readout underneath.
    bool drawField (dw::Context& ctx, Field field, const Box& box)
    {
        if (editing != field)
            return false;
        fieldDrawn = true;
        const auto result = dw::textField (ctx, "##editor-field", box.tl(), box.br(), fieldText.data(),
                                           fieldText.size(), fieldTakesFocus);
        fieldTakesFocus = false;
        if (result.committed || result.cancelled)
        {
            editing = Field::none;
            // Enter on the text as it opened is not an edit.
            if (result.committed && fieldOriginal != fieldText.data())
                commitField (field, fieldText.data());
        }
        return true;
    }

    void commitField (Field field, const std::string& text)
    {
        const auto* r = region();
        if (r == nullptr)
            return;
        switch (field)
        {
            case Field::title:
            case Field::label:
            {
                // Accepting the file name as shown leaves the region unlabelled, so the
                // title keeps following the file.
                auto label = field == Field::title ? dusk::text::trim (text) : text;
                if (field == Field::title && label == r->file.getFileName().toStdString())
                    label.clear();
                const auto value = RegionLabel::fromUTF8 (label.c_str());
                editFocused ("Rename region", [&value] (AudioRegion& a) { a.label = value; });
                break;
            }
            case Field::gain:
            {
                const auto digits = dusk::text::retainCharacters (dusk::text::trim (text), "0123456789.-+");
                if (digits.empty())
                    return;
                const auto db = std::clamp (dusk::text::getFloatValue (digits), -24.0f, 12.0f);
                editFocused ("Set region gain", [db] (AudioRegion& a) { a.gainDb = db; });
                break;
            }
            case Field::fade:
            {
                // "IN / OUT", or one value for the fade-in alone, and the readout's own
                // "fade ... ms" dressing is tolerated so it can be retyped as shown.
                auto raw = dusk::text::toLowerCase (dusk::text::trim (text));
                raw = dusk::text::replace (dusk::text::replace (raw, "fade", ""), "ms", "");
                const auto slash = raw.find ('/');
                const auto inMs = std::max (0.0, dusk::text::getDoubleValue (dusk::text::trim (raw.substr (0, slash))));
                const auto outMs = slash == std::string::npos
                                 ? 0.0 : std::max (0.0, dusk::text::getDoubleValue (dusk::text::trim (raw.substr (slash + 1))));
                const double sr = sampleRate();
                editFocused ("Set region fades", [inMs, outMs, sr] (AudioRegion& a)
                {
                    const auto length = std::max<std::int64_t> (0, a.lengthInSamples);
                    a.fadeInSamples = clampTo (static_cast<std::int64_t> (std::llround (inMs * sr / 1000.0)), 0, length);
                    a.fadeOutSamples = clampTo (static_cast<std::int64_t> (std::llround (outMs * sr / 1000.0)), 0,
                                                length - a.fadeInSamples);
                });
                break;
            }
            case Field::none:
                break;
        }
    }

    void drawModeGroup (dw::Context& ctx, Box area, float dia)
    {
        struct Mode { const char* name; EditMode mode; };
        static constexpr Mode kModes[] = { { "Grab", EditMode::Grab }, { "Range", EditMode::Range },
                                           { "Cut", EditMode::Cut }, { "Draw", EditMode::Draw } };
        const float h = dia - ctx.s (8.0f);
        dw::ButtonStyle style;
        style.fontSize = 11.0f;
        for (const auto& mode : kModes)
        {
            if (area.width() < ctx.s (44.0f)) return;
            const auto box = area.takeLeft (ctx.s (44.0f)).sizedKeepingCentre (ctx.s (44.0f), h);
            area.takeLeft (ctx.s (2.0f));
            if (toggleButton (ctx, mode.name, box, mode.name, session.editMode == mode.mode, style))
                session.editMode = mode.mode;
        }
        area.takeLeft (ctx.s (6.0f));

        if (area.width() < ctx.s (44.0f)) return;
        const auto snapBox = area.takeLeft (ctx.s (44.0f)).sizedKeepingCentre (ctx.s (44.0f), h);
        area.takeLeft (ctx.s (4.0f));
        if (toggleButton (ctx, "Snap", snapBox, "Snap", session.audioEditorSnap, style))
            session.audioEditorSnap = ! session.audioEditorSnap;

        if (area.width() < ctx.s (60.0f)) return;
        const float comboWidth = std::min (ctx.s (112.0f), area.width());
        const auto comboBox = area.takeLeft (comboWidth).sizedKeepingCentre (comboWidth, h);
        addControl ("Snap resolution", comboBox, true);
        int selected = std::clamp (static_cast<int> (session.snapResolution), 0, kSnapLabelCount - 1);
        const ScopedFormStyle form (ctx);
        if (formCombo (ctx, "##snap-resolution", comboBox.tl(), comboBox.br(), kSnapLabels,
                       kSnapLabelCount, selected))
            session.snapResolution = static_cast<SnapResolution> (selected);
    }

    void clippedText (const dw::Context& ctx, const Box& box, ImFont* font, float size, ImU32 colour,
                      const char* str, dw::Align align) const
    {
        if (box.width() <= 0.0f)
            return;
        ctx.dl->PushClipRect (box.tl(), box.br(), true);
        dw::text (ctx, font, ctx.s (size), ImVec2 (box.x0, box.at (0.0f, 0.5f).y - ctx.s (size) * 0.5f),
                  box.width(), colour, str, align);
        ctx.dl->PopClipRect();
    }

    void readout (dw::Context& ctx, const Box& box, const char* str, dw::Align align)
    {
        ctx.dl->AddRectFilled (box.tl(), box.br(), argb (kBackground));
        ctx.dl->AddRect (box.tl(), box.br(), argb (kReadoutOutline), 0.0f, 0, ctx.s (1.0f));
        clippedText (ctx, box.reduced (ctx.s (4.0f), 0.0f), ctx.fonts->band, 12.0f, argb (kReadoutText),
                     str, align);
    }

    bool tickBox (dw::Context& ctx, const char* name, const Box& box, bool on, bool enabled)
    {
        addControl (name, box, enabled);
        char id[32];
        std::snprintf (id, sizeof (id), "##toggle-%s", name);
        const bool hovered = dw::hitArea (ctx, id, box.tl(), box.br());
        const float side = ctx.s (14.0f);
        const Box tick { box.x0 + ctx.s (2.0f), box.at (0.0f, 0.5f).y - side * 0.5f,
                         box.x0 + ctx.s (2.0f) + side, box.at (0.0f, 0.5f).y + side * 0.5f };
        const auto ink = argb (kReadoutText, enabled ? 1.0f : 0.4f);
        ctx.dl->AddRect (tick.tl(), tick.br(), ink, ctx.s (3.0f), 0, ctx.s (1.2f));
        if (on)
        {
            const ImVec2 points[] = { tick.at (0.22f, 0.52f), tick.at (0.42f, 0.74f), tick.at (0.8f, 0.26f) };
            ctx.dl->AddPolyline (points, 3, ink, ImDrawFlags_None, ctx.s (1.8f));
        }
        clippedText (ctx, Box { tick.x1 + ctx.s (5.0f), box.y0, box.x1, box.y1 }, ctx.fonts->band, 12.0f,
                     ink, name, dw::Align::left);
        return enabled && releasedOn (hovered);
    }

    void drawStatusBar (dw::Context& ctx)
    {
        auto* const dl = ctx.dl;
        const auto& status = layout.status;
        dl->AddRectFilled (status.tl(), status.br(), argb (kBackground));
        hline (dl, status.y0, status.x0, status.x1, argb (kBarLine), ctx.s (1.0f));

        const auto* r = region();
        auto inner = status.reduced (ctx.s (8.0f), ctx.s (4.0f));
        const auto pos = inner.takeLeft (ctx.s (110.0f));
        inner.takeLeft (ctx.s (4.0f));
        const auto gain = inner.takeLeft (ctx.s (84.0f));
        inner.takeLeft (ctx.s (4.0f));
        const auto fade = inner.takeLeft (ctx.s (162.0f));
        inner.takeLeft (ctx.s (4.0f));
        const auto samples = inner.takeLeft (ctx.s (220.0f));
        inner.takeLeft (ctx.s (10.0f));
        const auto lock = inner.takeRight (ctx.s (60.0f));
        inner.takeRight (ctx.s (4.0f));
        const auto mute = inner.takeRight (ctx.s (60.0f));
        inner.takeRight (ctx.s (10.0f));

        if (tickBox (ctx, "Mute", mute, r != nullptr && r->muted, r != nullptr))
            toggleMute();
        if (tickBox (ctx, "Lock", lock, r != nullptr && r->locked, r != nullptr))
            toggleLock();

        r = region();
        if (r == nullptr)
            return;

        const double sr = sampleRate();
        const float bpm = std::max (1.0f, session.tempoBpm.load (std::memory_order_relaxed));
        const int bpb = std::max (1, session.beatsPerBar.load (std::memory_order_relaxed));
        const auto mode = static_cast<TimeDisplayMode> (session.timeDisplayMode.load (std::memory_order_relaxed));
        const auto cursor = r->timelineStart + (editCursorSample - r->sourceOffset);

        const auto position = "pos " + formatSamplePosition (cursor, sr, session.tempoMap, bpm, bpb, mode)
                                           .toStdString();
        readout (ctx, pos, position.c_str(), dw::Align::left);

        char text[96];
        std::snprintf (text, sizeof (text), "%.1f dB", static_cast<double> (r->gainDb));
        if (! drawField (ctx, Field::gain, gain))
        {
            readout (ctx, gain, text, dw::Align::left);
            if (doubleClicked (ctx, "##gain-readout", gain))
                beginEdit (Field::gain, text);
        }

        r = region();
        if (r == nullptr)
            return;
        std::snprintf (text, sizeof (text), "fade %.0f / %.0f ms",
                       static_cast<double> (r->fadeInSamples) * 1000.0 / sr,
                       static_cast<double> (r->fadeOutSamples) * 1000.0 / sr);
        if (! drawField (ctx, Field::fade, fade))
        {
            readout (ctx, fade, text, dw::Align::left);
            if (doubleClicked (ctx, "##fade-readout", fade))
                beginEdit (Field::fade, text);
        }

        r = region();
        if (r == nullptr)
            return;
        // Raw sample counts, for working out latency by hand.
        if (rangeActive)
            std::snprintf (text, sizeof (text), "smp %lld +%lld",
                           static_cast<long long> (r->timelineStart + (std::min (rangeStartSample, rangeEndSample) - r->sourceOffset)),
                           static_cast<long long> (std::abs (rangeEndSample - rangeStartSample)));
        else
            std::snprintf (text, sizeof (text), "smp %lld", static_cast<long long> (cursor));
        readout (ctx, samples, text, dw::Align::left);

        const double seconds = static_cast<double> (r->lengthInSamples) / sr;
        const int minutes = static_cast<int> (seconds / 60.0);
        std::snprintf (text, sizeof (text), "%dch - %d kHz - %d:%.3f", std::max (1, r->numChannels),
                       static_cast<int> (std::lround (sr / 1000.0)), minutes, seconds - minutes * 60.0);
        readout (ctx, inner, text, dw::Align::right);
    }

    void collectSlices()
    {
        slices.clear();
        for (auto& source : sources)
            source->wanted.clear();

        const auto& lanes = layout.lanes;
        const int clipX0 = static_cast<int> (std::floor (lanes.x0));
        const int clipX1 = static_cast<int> (std::ceil (lanes.x1));
        const auto anchorEnd = anchorStart + anchorLength;
        const auto& regions = trackRegions();
        std::size_t columns = 0;

        for (int i = 0; i < static_cast<int> (regions.size()); ++i)
        {
            const auto& reg = regions[static_cast<std::size_t> (i)];
            if (reg.lengthInSamples <= 0) continue;
            const auto end = reg.timelineStart + reg.lengthInSamples;
            if (end <= anchorStart || reg.timelineStart >= anchorEnd) continue;

            Slice slice;
            slice.region = i;
            slice.xa = columnForTimeline (reg.timelineStart);
            slice.xb = columnForTimeline (end);
            slice.x0 = std::max (slice.xa, clipX0);
            slice.x1 = std::min (slice.xb, clipX1);
            if (slice.xb <= slice.xa || slice.x1 <= slice.x0) continue;

            slice.source = sourceFor (reg.file.getFullPathName().toStdString());
            slice.window = windowFor (reg, slice.xa, slice.xb, clipX0, clipX1);
            if (slice.source != nullptr && slice.window.numColumns > 0
                && slice.window.sourceLength
                       <= static_cast<std::int64_t> (slice.window.fullWidth) * WaveformPeaks::kMaxFramesPerPeak
                && static_cast<std::size_t> (slice.window.numColumns) <= WaveformDetails::kMaxColumns - columns)
            {
                columns += static_cast<std::size_t> (slice.window.numColumns);
                slice.detail = true;
                slice.source->wanted.push_back (slice.window);
            }
            slices.push_back (slice);
        }
    }

    void requestDetails()
    {
        for (auto& source : sources)
        {
            if (source->lastUsed != frame)
                continue;
            if (! source->detailsSent || source->wanted != source->requested)
            {
                auto windows = source->wanted;
                const auto before = source->source->snapshot();
                if (! WaveformDetails::validRequest (windows, before.info ? before.info->numChannels : 1))
                    windows.clear();
                source->source->setDetailWindows (windows);
                source->requested = source->wanted;
                source->detailsSent = true;
            }
            source->snapshot = source->source->snapshot();
        }
    }

    void drawWaveforms (dw::Context& ctx)
    {
        auto* const dl = ctx.dl;
        const auto& lanes = layout.lanes;
        dl->AddRectFilled (lanes.tl(), lanes.br(), IM_COL32 (0, 0, 0, 64));

        const auto& regions = trackRegions();
        for (const auto& slice : slices)
        {
            const auto& reg = regions[static_cast<std::size_t> (slice.region)];
            const bool focused = slice.region == regionIdx;
            const ImVec2 tl (static_cast<float> (slice.x0), lanes.y0);
            const ImVec2 br (static_cast<float> (slice.x1), lanes.y1);
            if (! focused)
                dl->AddRectFilled (tl, br, argb (kBackground, 0.6f));

            const auto base = reg.customColour.isTransparent() ? argb (kWaveformFill)
                                                               : argb (reg.customColour.getARGB());
            const auto* snapshot = slice.source != nullptr ? &slice.source->snapshot : nullptr;
            if (snapshot == nullptr || snapshot->state == WaveformSource::State::Failed)
            {
                dl->AddRectFilled (tl, br, withBrightness (base, 0.5f, 0.7f));
                continue;
            }
            if (! snapshot->info)
                continue;
            drawColumns (dl, slice, *snapshot, withBrightness (base, focused ? 1.05f : 0.55f, focused ? 1.0f : 0.85f));
        }

        for (const auto& reg : regions)
        {
            if (reg.lengthInSamples <= 0) continue;
            for (const auto edge : { reg.timelineStart, reg.timelineStart + reg.lengthInSamples })
                if (edge > anchorStart && edge < anchorStart + anchorLength)
                    vline (dl, xForTimeline (edge), lanes.y0, lanes.y1, argb (kSliceBoundary, 0.55f), ctx.s (1.0f));
        }

        hline (dl, lanes.at (0.0f, 0.5f).y, lanes.x0, lanes.x1, argb (kBeatLine, 0.6f), ctx.s (1.0f));

        // The regions a group edit would also touch.
        for (const int index : additional)
        {
            if (index < 0 || index >= static_cast<int> (regions.size())) continue;
            const auto& reg = regions[static_cast<std::size_t> (index)];
            const auto a = std::max (reg.timelineStart, anchorStart);
            const auto b = std::min (reg.timelineStart + reg.lengthInSamples, anchorStart + anchorLength);
            if (b > a)
                dl->AddRectFilled (ImVec2 (xForTimeline (a), lanes.y0), ImVec2 (xForTimeline (b), lanes.y1),
                                   argb (kSelection, 0.18f));
        }
    }

    void drawRange (const dw::Context& ctx) const
    {
        if (! rangeActive) return;
        const float xa = std::floor (xForFileSample (std::min (rangeStartSample, rangeEndSample)));
        const float xb = std::floor (xForFileSample (std::max (rangeStartSample, rangeEndSample)));
        if (xb <= xa) return;
        const auto& wave = layout.wave;
        ctx.dl->AddRectFilled (ImVec2 (xa, wave.y0), ImVec2 (xb, wave.y1), argb (kRange, 0.18f));
        vline (ctx.dl, xa, wave.y0, wave.y1, argb (kRange, 0.6f), ctx.s (1.0f));
        vline (ctx.dl, xb, wave.y0, wave.y1, argb (kRange, 0.6f), ctx.s (1.0f));
    }

    void drawSnapGuide (const dw::Context& ctx) const
    {
        if (snapGuide < 0) return;
        const float x = xForTimeline (snapGuide);
        if (x >= layout.wave.x0 && x < layout.wave.x1)
            vline (ctx.dl, x, layout.wave.y0, layout.wave.y1, argb (kSelection, 0.85f), ctx.s (1.0f));
    }

    void drawColumns (ImDrawList* dl, const Slice& slice, const WaveformSource::Snapshot& snapshot, ImU32 colour) const
    {
        const auto& lanes = layout.lanes;
        const auto& info = *snapshot.info;
        std::size_t detailIndex = 0;
        bool haveDetail = false;
        if (slice.detail && snapshot.details)
        {
            const auto& cached = snapshot.details->windows();
            const auto found = std::find (cached.begin(), cached.end(), slice.window);
            haveDetail = found != cached.end();
            detailIndex = static_cast<std::size_t> (std::distance (cached.begin(), found));
        }

        const int channels = std::max (1, info.numChannels);
        const float laneHeight = lanes.height() / static_cast<float> (channels);
        const float pad = 0.3f * layout.scale;
        for (int channel = 0; channel < channels; ++channel)
        {
            const float top = lanes.y0 + laneHeight * static_cast<float> (channel);
            const float bottom = top + laneHeight;
            const float centre = (top + bottom) * 0.5f;
            const float span = laneHeight * 0.475f;
            for (int column = 0; column < slice.window.numColumns; ++column)
            {
                std::optional<WaveformPeaks::Peak> peak;
                if (haveDetail)
                    peak = snapshot.details->column (detailIndex, channel, column);
                if (! peak && snapshot.peaks)
                    if (const auto range = slice.window.columnRange (column, info.numFrames))
                        peak = snapshot.peaks->query (channel, range->first, range->second);
                if (! peak || (peak->minimum >= 0.0f && peak->maximum <= 0.0f))
                    continue;
                const float y1 = std::clamp (centre - std::clamp (peak->maximum, -1.0f, 1.0f) * span - pad, top, bottom);
                const float y2 = std::clamp (centre - std::clamp (peak->minimum, -1.0f, 1.0f) * span + pad, top, bottom);
                const float x = static_cast<float> (slice.x0 + column);
                dl->AddRectFilled (ImVec2 (x, y1), ImVec2 (x + 1.0f, std::max (y2, y1 + 1.0f)), colour);
            }
        }
    }

    void drawRuler (dw::Context& ctx)
    {
        auto* const dl = ctx.dl;
        const auto& ruler = layout.ruler;
        dl->AddRectFilled (ruler.tl(), ruler.br(), argb (kHeaderFill));
        hline (dl, ruler.y1 - ctx.s (1.0f), ruler.x0, ruler.x1, argb (kBarLine), ctx.s (1.0f));

        const double sr = sampleRate();
        const auto anchorEnd = anchorStart + anchorLength;
        const float fontSize = ctx.s (12.0f);
        const float textY = ruler.at (0.0f, 0.5f).y - fontSize * 0.5f;
        const auto label = [&] (float x, const char* str)
        {
            dw::text (ctx, ctx.fonts->band, fontSize, ImVec2 (x + ctx.s (3.0f), textY), ctx.s (60.0f),
                      argb (kHeaderText), str, dw::Align::left);
        };

        const auto mode = static_cast<TimeDisplayMode> (session.timeDisplayMode.load (std::memory_order_relaxed));
        if (mode == TimeDisplayMode::Bars)
        {
            const int beatsPerBar = std::max (1, session.beatsPerBar.load (std::memory_order_relaxed));
            const std::int64_t ticksPerBeat = kMidiTicksPerQuarter;
            const std::int64_t ticksPerBar = beatsPerBar * ticksPerBeat;
            // Padded past both edges so a bar just off-screen whose number spills into
            // view still labels.
            const auto from = std::max (anchorStart, timelineForX (ruler.x0 - ctx.s (48.0f)));
            const auto to = std::min (anchorEnd, timelineForX (ruler.x1 + ctx.s (48.0f)));
            const auto firstBar = session.samplesToTicks (from, sr) / ticksPerBar;
            const auto lastBar = session.samplesToTicks (to, sr) / ticksPerBar + 1;

            const float bpm = session.tempoBpm.load (std::memory_order_relaxed);
            const double pxPerBeat = bpm > 0.0f ? pixelsPerSample * (sr * 60.0 / bpm) : 0.0;
            const bool showBeats = pxPerBeat >= 9.0;
            const int subdivisions = pxPerBeat >= 80.0 ? 4 : (pxPerBeat >= 36.0 ? 2 : 1);
            const float beatTop = ruler.y1 - ruler.height() * 0.45f;
            const float subTop = ruler.y1 - ruler.height() * 0.28f;

            const auto tickAt = [&] (std::int64_t tick, float top, ImU32 colour)
            {
                const auto t = session.ticksToSamples (tick, sr);
                if (t < anchorStart || t > anchorEnd) return;
                const float x = xForTimeline (t);
                if (x < ruler.x0 || x > ruler.x1) return;
                vline (dl, x, top, ruler.y1, colour, ctx.s (1.0f));
            };

            for (auto bar = firstBar; bar <= lastBar; ++bar)
            {
                const auto barTick = bar * ticksPerBar;
                const auto t = session.ticksToSamples (barTick, sr);
                if (t >= anchorStart && t <= anchorEnd)
                {
                    const float x = xForTimeline (t);
                    if (x >= ruler.x0 && x <= ruler.x1)
                    {
                        vline (dl, x, ruler.y0, ruler.y1, argb (kBarLine), ctx.s (1.0f));
                        char number[24];
                        std::snprintf (number, sizeof (number), "%lld", static_cast<long long> (bar + 1));
                        label (x, number);
                    }
                }
                if (! showBeats) continue;
                for (int beat = 0; beat < beatsPerBar; ++beat)
                {
                    if (beat > 0)
                        tickAt (barTick + beat * ticksPerBeat, beatTop, argb (kBarLine, 0.55f));
                    for (int sub = 1; sub < subdivisions; ++sub)
                        tickAt (barTick + beat * ticksPerBeat + sub * ticksPerBeat / subdivisions, subTop,
                                argb (kBarLine, 0.30f));
                }
            }
            return;
        }

        const double pxPerSecond = pixelsPerSample * sr;
        double every = 1.0;
        if (pxPerSecond < 6.0) every = 30.0;
        else if (pxPerSecond < 16.0) every = 10.0;
        else if (pxPerSecond < 40.0) every = 5.0;

        const double from = static_cast<double> (std::max (anchorStart, timelineForX (ruler.x0 - ctx.s (60.0f)))) / sr;
        const double to = static_cast<double> (std::min (anchorEnd, timelineForX (ruler.x1))) / sr;
        for (double second = std::floor (from / every) * every; second <= to; second += every)
        {
            const auto t = static_cast<std::int64_t> (std::llround (second * sr));
            if (t < anchorStart || t > anchorEnd) continue;
            const float x = xForTimeline (t);
            if (x < ruler.x0 || x > ruler.x1) continue;
            vline (dl, x, ruler.y0, ruler.y1, argb (kBarLine), ctx.s (1.0f));
            char stamp[24];
            std::snprintf (stamp, sizeof (stamp), "%d:%02d", static_cast<int> (second / 60.0),
                           static_cast<int> (second) % 60);
            label (x, stamp);
        }
    }

    void drawBarGrid (dw::Context& ctx)
    {
        if (static_cast<TimeDisplayMode> (session.timeDisplayMode.load (std::memory_order_relaxed))
                != TimeDisplayMode::Bars)
            return;
        const double sr = sampleRate();
        const float bpm = session.tempoBpm.load (std::memory_order_relaxed);
        if (bpm <= 0.0f) return;
        if (pixelsPerSample * (sr * 60.0 / bpm) < 6.0) return;

        const auto& wave = layout.wave;
        const int beatsPerBar = std::max (1, session.beatsPerBar.load (std::memory_order_relaxed));
        const std::int64_t ticksPerBeat = kMidiTicksPerQuarter;
        const std::int64_t ticksPerBar = beatsPerBar * ticksPerBeat;
        const auto anchorEnd = anchorStart + anchorLength;
        const auto from = std::max (anchorStart, timelineForX (wave.x0));
        const auto to = std::min (anchorEnd, timelineForX (wave.x1));
        const auto firstBar = session.samplesToTicks (from, sr) / ticksPerBar;
        const auto lastBar = session.samplesToTicks (to, sr) / ticksPerBar + 1;
        for (auto bar = firstBar; bar <= lastBar; ++bar)
            for (int beat = 0; beat < beatsPerBar; ++beat)
            {
                const auto t = session.ticksToSamples (bar * ticksPerBar + beat * ticksPerBeat, sr);
                if (t < anchorStart || t > anchorEnd) continue;
                const float x = xForTimeline (t);
                if (x < wave.x0 || x > wave.x1) continue;
                vline (ctx.dl, x, wave.y0, wave.y1, argb (kBarLine, beat == 0 ? 0.28f : 0.12f), ctx.s (1.0f));
            }
    }

    // How far the focused region overlaps its neighbours in time, and the neighbours'
    // facing fade shapes, so a crossfade draws as the X the two regions make together.
    void overlapNeighbours (std::int64_t& overlapPrev, std::int64_t& overlapNext,
                            FadeShape& prevOut, FadeShape& nextIn) const
    {
        overlapPrev = overlapNext = 0;
        prevOut = nextIn = FadeShape::EqualPower;
        const auto& regions = trackRegions();
        if (regionIdx < 0 || regionIdx >= static_cast<int> (regions.size())) return;

        std::vector<int> order (regions.size());
        for (int i = 0; i < static_cast<int> (order.size()); ++i) order[static_cast<std::size_t> (i)] = i;
        std::sort (order.begin(), order.end(), [&regions] (int a, int b)
        {
            return regions[static_cast<std::size_t> (a)].timelineStart < regions[static_cast<std::size_t> (b)].timelineStart;
        });
        const auto overlap = [] (const AudioRegion& a, const AudioRegion& b) -> std::int64_t
        {
            const auto aEnd = a.timelineStart + a.lengthInSamples;
            if (aEnd <= b.timelineStart) return 0;
            return std::min (aEnd - b.timelineStart, std::min (a.lengthInSamples, b.lengthInSamples));
        };

        const auto at = std::find (order.begin(), order.end(), regionIdx);
        const auto pos = static_cast<std::size_t> (std::distance (order.begin(), at));
        const auto& self = regions[static_cast<std::size_t> (regionIdx)];
        if (pos > 0)
        {
            const auto& prev = regions[static_cast<std::size_t> (order[pos - 1])];
            overlapPrev = overlap (prev, self);
            prevOut = prev.fadeOutShape;
        }
        if (pos + 1 < order.size())
        {
            const auto& next = regions[static_cast<std::size_t> (order[pos + 1])];
            overlapNext = overlap (self, next);
            nextIn = next.fadeInShape;
        }
    }

    void strokeFade (const dw::Context& ctx, float x0, float x1, FadeShape shape, bool rising, ImU32 colour) const
    {
        const auto& lanes = layout.lanes;
        const int span = std::clamp (static_cast<int> (std::round (x1 - x0)), 1, 4096);
        std::vector<ImVec2> points (static_cast<std::size_t> (span + 1));
        for (int i = 0; i <= span; ++i)
        {
            const float t = static_cast<float> (i) / static_cast<float> (span);
            const float gain = applyFadeShape (rising ? t : 1.0f - t, shape);
            points[static_cast<std::size_t> (i)] = ImVec2 (x0 + t * (x1 - x0), lanes.y1 - gain * lanes.height());
        }
        ctx.dl->AddPolyline (points.data(), static_cast<int> (points.size()), colour, ImDrawFlags_None, ctx.s (1.4f));
    }

    void drawFades (const dw::Context& ctx) const
    {
        const auto* r = region();
        if (r->lengthInSamples <= 0) return;
        std::int64_t overlapPrev = 0, overlapNext = 0;
        FadeShape prevOut = FadeShape::EqualPower, nextIn = FadeShape::EqualPower;
        overlapNeighbours (overlapPrev, overlapNext, prevOut, nextIn);

        const auto curve = argb (kFadeStroke, 0.9f);
        const auto crossfadeFill = IM_COL32 (255, 255, 255, 0x18);
        const auto zone = [&] (float x0, float x1, FadeShape first, bool firstRising,
                               bool crossfade, FadeShape second)
        {
            if (x1 <= x0) return;
            if (crossfade)
            {
                ctx.dl->AddRectFilled (ImVec2 (x0, layout.lanes.y0), ImVec2 (x1, layout.lanes.y1), crossfadeFill);
                strokeFade (ctx, x0, x1, second, false, curve);
                strokeFade (ctx, x0, x1, first, true, curve);
                return;
            }
            strokeFade (ctx, x0, x1, first, firstRising, curve);
        };

        if (r->fadeInSamples > 0)
            zone (xForFileSample (r->sourceOffset), xForFileSample (r->sourceOffset + r->fadeInSamples),
                  r->fadeInShape, true, overlapPrev > 0, prevOut);
        if (r->fadeOutSamples > 0)
        {
            const float x0 = xForFileSample (r->sourceOffset + r->lengthInSamples - r->fadeOutSamples);
            const float x1 = xForFileSample (r->sourceOffset + r->lengthInSamples);
            if (overlapNext > 0)
                zone (x0, x1, nextIn, true, true, r->fadeOutShape);
            else
                zone (x0, x1, r->fadeOutShape, false, false, r->fadeOutShape);
        }
    }

    void drawLoopPunch (const dw::Context& ctx) const
    {
        const auto& transport = engine.getTransport();
        const auto& ruler = layout.ruler;
        const auto& wave = layout.wave;
        const float cap = ctx.s (6.0f), capH = ctx.s (3.0f), w = ctx.s (1.0f);
        const auto pair = [&] (std::int64_t a, std::int64_t b, std::uint32_t colour, bool enabled)
        {
            if (b < a) return;
            const float alpha = enabled ? 1.0f : 0.4f;
            const float xa = xForTimeline (a);
            const float xb = xForTimeline (b);
            const auto edge = argb (colour, 0.9f * alpha);
            const auto caps = [&] (float x)
            {
                ctx.dl->AddRectFilled (ImVec2 (x, ruler.y0), ImVec2 (x + cap, ruler.y0 + capH), edge);
                ctx.dl->AddRectFilled (ImVec2 (x, ruler.y1 - capH), ImVec2 (x + cap, ruler.y1), edge);
            };
            // One point set with its partner still unplaced: a single edge marker.
            if (b == a)
            {
                if (a <= 0 || xa < wave.x0 || xa > wave.x1) return;
                vline (ctx.dl, xa, ruler.y0, wave.y1, edge, w);
                caps (xa);
                return;
            }
            if (xb < wave.x0 || xa > wave.x1) return;
            ctx.dl->AddRectFilled (ImVec2 (xa, wave.y0), ImVec2 (std::max (xa + 1.0f, xb), wave.y1),
                                   argb (colour, 0.08f * alpha));
            vline (ctx.dl, xa, ruler.y0, wave.y1, edge, w);
            vline (ctx.dl, xb, ruler.y0, wave.y1, edge, w);
            caps (xa);
            caps (xb - cap + w);
        };
        pair (transport.getLoopStart(), transport.getLoopEnd(), kLoop, transport.isLoopEnabled());
        pair (transport.getPunchIn(), transport.getPunchOut(), kPunch, transport.isPunchEnabled());
    }

    void drawHandles (const dw::Context& ctx) const
    {
        const auto* r = region();
        auto* const dl = ctx.dl;
        const auto& lanes = layout.lanes;

        const float gy = gainLineY();
        dl->AddRectFilled (ImVec2 (lanes.x0, gy - ctx.s (1.0f)), ImVec2 (lanes.x1, gy + ctx.s (1.0f)), argb (kGainLine));
        const float cx = layout.wave.at (0.5f, 0.0f).x;
        const ImVec2 chipTl (cx - ctx.s (22.0f), gy - ctx.s (5.0f));
        const ImVec2 chipBr (cx + ctx.s (22.0f), gy + ctx.s (5.0f));
        dl->AddRectFilled (chipTl, chipBr, argb (kGainChip), ctx.s (2.0f));
        dl->AddRect (chipTl, chipBr, argb (kGainChipInk), ctx.s (2.0f), 0, ctx.s (1.2f));
        char db[24];
        std::snprintf (db, sizeof (db), "%.1f dB", static_cast<double> (r->gainDb));
        dw::text (ctx, ctx.fonts->label, ctx.s (9.5f), ImVec2 (chipTl.x, gy - ctx.s (5.5f)), ctx.s (44.0f),
                  argb (kGainChipInk), db);

        for (const auto& disc : { fadeInDisc(), fadeOutDisc() })
        {
            const auto centre = disc.at (0.5f, 0.5f);
            dl->AddCircleFilled (centre, disc.width() * 0.5f, argb (kFadeDisc), 24);
            dl->AddCircle (centre, disc.width() * 0.5f - ctx.s (1.0f), argb (kBackground), 24, ctx.s (2.0f));
        }

        // Barely-there strips at the focused slice's edges; the grab target is the
        // wider trim rect, not the painted strip.
        const auto strip = argb (kTrimStrip, 0.5f);
        const auto start = trimStart();
        const auto end = trimEnd();
        dl->AddRectFilled (ImVec2 (start.x0 + ctx.s (3.0f), start.y0), ImVec2 (start.x0 + ctx.s (6.0f), start.y1), strip);
        dl->AddRectFilled (ImVec2 (end.x0 + ctx.s (2.0f), end.y0), ImVec2 (end.x0 + ctx.s (5.0f), end.y1), strip);
    }

    void drawEditCursor (const dw::Context& ctx) const
    {
        const auto& wave = layout.wave;
        const float x = std::floor (xForFileSample (editCursorSample));
        if (x < wave.x0 - ctx.s (1.0f) || x > wave.x1 + ctx.s (1.0f)) return;
        vline (ctx.dl, x, wave.y0, wave.y1, argb (kEditCursor, 0.7f), ctx.s (1.0f));
        const float mid = x + ctx.s (0.5f);
        ctx.dl->AddTriangleFilled (ImVec2 (mid - ctx.s (4.0f), wave.y0), ImVec2 (mid + ctx.s (4.0f), wave.y0),
                                   ImVec2 (mid, wave.y0 + ctx.s (5.0f)), argb (kEditCursor));
    }

    // -1 when the playhead is outside the track's span or off the visible lanes.
    float playheadX() const
    {
        const auto playhead = engine.getTransport().getPlayhead();
        if (anchorLength <= 0 || playhead < anchorStart || playhead >= anchorStart + anchorLength)
            return -1.0f;
        const float x = xForTimeline (playhead);
        return x < layout.wave.x0 || x > layout.wave.x1 ? -1.0f : x;
    }

    void drawPlayhead (const dw::Context& ctx) const
    {
        const float x = playheadX();
        if (x < 0.0f) return;
        const auto& wave = layout.wave;
        const float w = ctx.s (1.0f);
        // Dark flanks hold the line's contrast over the bright waveform.
        vline (ctx.dl, x - w, wave.y0, wave.y1, IM_COL32 (0, 0, 0, 128), w);
        vline (ctx.dl, x + w, wave.y0, wave.y1, IM_COL32 (0, 0, 0, 128), w);
        vline (ctx.dl, x, wave.y0, wave.y1, argb (kPlayhead), w);
    }

    void drawScrollBar (dw::Context& ctx)
    {
        const auto& bar = layout.scroll;
        ctx.dl->AddRectFilled (bar.tl(), bar.br(), argb (kScrollTrack));
        if (anchorLength <= 0 || pixelsPerSample <= 0.0f) return;

        const float total = static_cast<float> (anchorLength);
        const float visible = std::min (total, static_cast<float> (viewSamples()));
        const float thumbW = std::max (ctx.s (16.0f), bar.width() * visible / total);
        const float travel = std::max (1.0f, bar.width() - thumbW);
        const float maxStart = std::max (1.0f, total - visible);
        const float thumbX = bar.x0 + travel * std::clamp (static_cast<float> (scrollSamples) / maxStart, 0.0f, 1.0f);
        const Box thumb { thumbX, bar.y0 + ctx.s (2.0f), thumbX + thumbW, bar.y1 - ctx.s (2.0f) };

        dw::hitArea (ctx, "##scroll", bar.tl(), bar.br());
        const auto& io = ImGui::GetIO();
        if (ImGui::IsItemActivated())
        {
            if (io.MousePos.x >= thumb.x0 && io.MousePos.x < thumb.x1)
            {
                scrollDragging = true;
                scrollDragX = io.MousePos.x;
                scrollDragOrigin = scrollSamples;
            }
            else
            {
                panBy (io.MousePos.x < thumb.x0 ? -viewSamples() : viewSamples());
            }
        }
        if (! ImGui::IsItemActive())
            scrollDragging = false;
        else if (scrollDragging)
            scrollSamples = std::clamp<std::int64_t> (
                scrollDragOrigin + static_cast<std::int64_t> (std::llround ((io.MousePos.x - scrollDragX) / travel * maxStart)),
                0, maxScroll());

        const bool hot = ImGui::IsItemHovered() || scrollDragging;
        ctx.dl->AddRectFilled (thumb.tl(), thumb.br(), argb (kScrollThumb, hot ? 1.0f : 0.8f), ctx.s (3.0f));
    }

    bool menuItem (const char* label, bool enabled = true, bool checked = false)
    {
        const bool picked = ImGui::MenuItem (label, nullptr, checked, enabled);
        addControl (std::string ("menu:") + label, Box { ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                                                         ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y },
                    enabled);
        return picked;
    }

    // The editor's menus are Dear ImGui popups, which the window keeps inside the
    // child and so inside the plate.
    void drawPopups (dw::Context& ctx)
    {
        const ScopedFormStyle form (ctx);
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (ctx.s (6.0f), ctx.s (6.0f)));
        ImGui::PushStyleVar (ImGuiStyleVar_ItemSpacing, ImVec2 (ctx.s (8.0f), ctx.s (5.0f)));
        drawContextMenu();
        drawFadeMenu();
        drawPropertiesMenu();
        ImGui::PopStyleVar (2);
    }

    // True when the popup is up and its region still exists; closes it otherwise.
    bool beginMenu (const char* id)
    {
        if (! ImGui::BeginPopup (id))
            return false;
        if (region() != nullptr)
            return true;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return false;
    }

    void drawContextMenu()
    {
        if (! beginMenu (kContextMenu))
            return;
        const auto* r = region();
        const bool editLocked = r->locked || trackFrozen();
        const bool muted = r->muted;
        const bool locked = r->locked;
        if (menuItem ("Split at edit cursor")) splitAtCursor();
        if (menuItem ("Cut range", rangeActive && ! editLocked)) cutRange();
        if (menuItem ("Join selected regions", ! additional.empty())) joinSelected();
        ImGui::Separator();
        if (menuItem ("Reset gain (0 dB)"))
            editFocused ("Reset gain", [] (AudioRegion& a) { a.gainDb = 0.0f; });
        if (menuItem ("Reset fades"))
            editFocused ("Reset fades", [] (AudioRegion& a)
            {
                a.fadeInSamples = a.fadeOutSamples = 0;
                a.fadeInAuto = a.fadeOutAuto = false;
            });
        ImGui::Separator();
        if (menuItem (muted ? "Unmute" : "Mute"))
            editFocused (muted ? "Unmute" : "Mute", [] (AudioRegion& a) { a.muted = ! a.muted; });
        if (menuItem (locked ? "Unlock" : "Lock"))
            editFocused (locked ? "Unlock" : "Lock", [] (AudioRegion& a) { a.locked = ! a.locked; });
        ImGui::Separator();
        if (menuItem ("Reverse", ! editLocked)) reverseRegion();
        ImGui::EndPopup();
    }

    void drawFadeMenu()
    {
        if (! beginMenu (kFadeMenu))
            return;
        const auto* r = region();
        const auto current = fadeMenuIsIn ? r->fadeInShape : r->fadeOutShape;
        for (const auto& entry : kFadeShapes)
        {
            if (! menuItem (entry.label, true, entry.shape == current))
                continue;
            // Picking the shape already in place still makes the fade the user's.
            const AudioRegion before = *region();
            AudioRegion after = before;
            if (fadeMenuIsIn) { after.fadeInShape = entry.shape; after.fadeInAuto = false; }
            else              { after.fadeOutShape = entry.shape; after.fadeOutAuto = false; }
            if (after.fadeInShape != before.fadeInShape || after.fadeOutShape != before.fadeOutShape
                || after.fadeInAuto != before.fadeInAuto || after.fadeOutAuto != before.fadeOutAuto)
                commit (fadeMenuIsIn ? "Fade-in shape" : "Fade-out shape", before, after);
        }
        ImGui::EndPopup();
    }

    void drawPropertiesMenu()
    {
        // Hung from the button, as the menu it replaces was.
        ImGui::SetNextWindowPos (propertiesAnchor, ImGuiCond_Appearing);
        if (! beginMenu (kPropertiesMenu))
            return;
        const auto* r = region();
        const bool muted = r->muted;
        const bool locked = r->locked;
        const auto label = r->label.toStdString();
        const auto customColour = r->customColour;

        char text[160];
        std::snprintf (text, sizeof (text), "Track %d  region %d", trackIdx + 1, regionIdx + 1);
        ImGui::TextDisabled ("%s", text);
        ImGui::Separator();
        if (menuItem (label.empty() ? "Add label..." : "Rename label..."))
            beginEdit (Field::label, label);
        ImGui::Separator();
        if (menuItem (muted ? "Unmute region" : "Mute region")) toggleMute();
        if (menuItem (locked ? "Unlock region" : "Lock region")) toggleLock();
        ImGui::Separator();
        const bool colourOpen = ImGui::BeginMenu ("Color");
        addControl ("menu:Color", Box { ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                                        ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y }, true);
        if (colourOpen)
        {
            for (const auto& entry : kPalette)
            {
                const bool current = entry.argb == 0 ? customColour.isTransparent()
                                                     : customColour.getARGB() == entry.argb;
                if (menuItem (entry.label, true, current))
                    setColour (entry.argb);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (menuItem ("Delete region")) deleteFocused();

        if (const auto* shown = region())
        {
            ImGui::Separator();
            const double seconds = static_cast<double> (shown->lengthInSamples) / sampleRate();
            const int minutes = static_cast<int> (seconds / 60.0);
            std::snprintf (text, sizeof (text), "%s  -  %d kHz  -  %dch  -  %d:%06.3f",
                           shown->file.getFileName().toStdString().c_str(),
                           static_cast<int> (std::lround (sampleRate() / 1000.0)), shown->numChannels, minutes,
                           seconds - 60.0 * minutes);
            ImGui::TextDisabled ("%s", text);
        }
        ImGui::EndPopup();
    }

    // Grab and Cut show their glyph over a region, where there is something to take
    // hold of or to split; the system pointer is hidden there so only one shows. The
    // handles and Range keep system cursors, which draw correctly on their own.
    void drawPointerGlyph (const dw::Context& ctx)
    {
        const auto* r = region();
        if (r == nullptr || ImGui::IsPopupOpen (nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)
            || ! ImGui::IsMousePosValid()
            || ! ImGui::IsWindowHovered (ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
            return;
        const auto p = ImGui::GetIO().MousePos;

        switch (drag)
        {
            case Drag::fadeIn: case Drag::fadeOut: case Drag::trimStart: case Drag::trimEnd:
            case Drag::loopIn: case Drag::loopOut: case Drag::punchIn: case Drag::punchOut:
                ImGui::SetMouseCursor (ImGuiMouseCursor_ResizeEW);
                return;
            case Drag::gain:  ImGui::SetMouseCursor (ImGuiMouseCursor_ResizeNS); return;
            case Drag::pan:   ImGui::SetMouseCursor (ImGuiMouseCursor_Hand); return;
            case Drag::range: ImGui::SetMouseCursor (ImGuiMouseCursor_TextInput); return;
            case Drag::none: case Drag::moveCursor: case Drag::moveRegion: break;
        }

        if (layout.ruler.contains (p))
        {
            if (loopPunchEdgeAt (p.x) != Drag::none)
                ImGui::SetMouseCursor (ImGuiMouseCursor_ResizeEW);
            return;
        }
        if (! layout.wave.contains (p))
            return;
        if (! r->locked)
        {
            if (fadeInDisc().contains (p) || fadeOutDisc().contains (p) || trimStart().contains (p)
                || trimEnd().contains (p))
            {
                ImGui::SetMouseCursor (ImGuiMouseCursor_ResizeEW);
                return;
            }
            if (overGainLine (p))
            {
                ImGui::SetMouseCursor (ImGuiMouseCursor_ResizeNS);
                return;
            }
        }

        const auto mode = session.editMode;
        if (mode == EditMode::Range)
        {
            ImGui::SetMouseCursor (ImGuiMouseCursor_TextInput);
            return;
        }
        // Draw is the automation pencil, which has nothing to draw on until a lane is up.
        if ((mode != EditMode::Grab && mode != EditMode::Cut)
            || (regionIndexAtX (p.x) < 0 && drag != Drag::moveRegion))
            return;

        ImGui::SetMouseCursor (ImGuiMouseCursor_None);
        ctx.dl->PushClipRect (layout.body.tl(), layout.body.br(), false);
        if (mode == EditMode::Cut)
        {
            // Drawn where the split will land, which Snap may move off the pointer.
            const float x = std::floor (xForTimeline (snapTimelineSample (timelineForX (p.x), false)));
            drawCutLine (ctx.dl, x, layout.wave.y0, layout.wave.y1, ctx.scale);
            drawScissorsGlyph (ctx.dl, ImVec2 (x, p.y), ctx.scale);
        }
        else
        {
            drawHandGlyph (ctx.dl, p, ctx.scale);
        }
        ctx.dl->PopClipRect();
    }

    void finishFrame()
    {
        // A field whose box was not laid out this frame could never be committed
        // or cancelled, and would hold the keys forever.
        if (! std::exchange (fieldDrawn, false))
            editing = Field::none;
        popupOpen = ImGui::IsPopupOpen (nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    }
};
} // namespace

std::unique_ptr<AudioEditorView> makeAudioEditorView (Session& session, AudioEngine& engine,
                                                      int trackIndex, int regionIndex,
                                                      AudioEditorHost host)
{
    return std::make_unique<AudioEditorViewImpl> (session, engine, trackIndex, regionIndex,
                                                  std::move (host));
}
} // namespace duskstudio::imgui
