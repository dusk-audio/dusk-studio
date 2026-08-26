#include "VirtualKeyboardView.h"
#include "DuskTheme.h"
#include "../AppConfig.h"
#include "../../engine/AudioEngine.h"
#include "../../foundation/MidiNoteName.h"
#if defined (__linux__)
# include "../KeyboardStateLinux.h"
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace duskstudio::imgui
{
namespace
{
namespace dw = DuskWidgets;

constexpr float kPanelW = 720.0f;
constexpr float kPanelH = 220.0f;
constexpr float kHeaderH = 24.0f;
constexpr float kButtonW = 56.0f;
constexpr float kButtonGap = 4.0f;

// The typed range: one octave of bass context below the centre, then up to the top
// typed key - centre + 28 is the 'P' of the I9O0P row - so every typeable note is
// on screen and labelled.
constexpr int kContextBelow = 12;
constexpr int kTopTypedOffset = 28;

// ~100 ms at 60 frames a second. On Linux the physical-key query is ground truth and
// one frame would do; the extra frames absorb a single stale read on the platforms
// that fall back to the frame's own key state, where an auto-repeat pair can read
// not-down for one frame.
constexpr int kReleaseFrames = 6;

ImU32 rgba (unsigned int hex)
{
    return IM_COL32 ((hex >> 24) & 0xff, (hex >> 16) & 0xff, (hex >> 8) & 0xff, hex & 0xff);
}

bool isBlackKey (int midiNote) noexcept
{
    const int pc = ((midiNote % 12) + 12) % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

struct LayoutKey
{
    const char* label;  // also the shell's key code for it, as uppercase ASCII
    ImGuiKey key;
    int offset;         // semitones above the centre note
};

// Reaper's layout: three rows, each an octave above the last.
const std::array<LayoutKey, 29>& layout()
{
    static const std::array<LayoutKey, 29> keys { {
        { "Z", ImGuiKey_Z, 0 },  { "S", ImGuiKey_S, 1 },  { "X", ImGuiKey_X, 2 },
        { "D", ImGuiKey_D, 3 },  { "C", ImGuiKey_C, 4 },  { "V", ImGuiKey_V, 5 },
        { "G", ImGuiKey_G, 6 },  { "B", ImGuiKey_B, 7 },  { "H", ImGuiKey_H, 8 },
        { "N", ImGuiKey_N, 9 },  { "J", ImGuiKey_J, 10 }, { "M", ImGuiKey_M, 11 },
        { "Q", ImGuiKey_Q, 12 }, { "2", ImGuiKey_2, 13 }, { "W", ImGuiKey_W, 14 },
        { "3", ImGuiKey_3, 15 }, { "E", ImGuiKey_E, 16 }, { "R", ImGuiKey_R, 17 },
        { "5", ImGuiKey_5, 18 }, { "T", ImGuiKey_T, 19 }, { "6", ImGuiKey_6, 20 },
        { "Y", ImGuiKey_Y, 21 }, { "7", ImGuiKey_7, 22 }, { "U", ImGuiKey_U, 23 },
        { "I", ImGuiKey_I, 24 }, { "9", ImGuiKey_9, 25 }, { "O", ImGuiKey_O, 26 },
        { "0", ImGuiKey_0, 27 }, { "P", ImGuiKey_P, 28 }
    } };
    return keys;
}

// Ground truth on Linux, where the frame's own key state can read not-down between
// the event pairs an auto-repeating key produces. Elsewhere the frame's state is what
// there is.
bool keyIsDown (const LayoutKey& entry)
{
   #if defined (__linux__)
    const int physical = isKeyPhysicallyDown (entry.label[0]);
    if (physical >= 0)
        return physical == 1;
   #endif
    return ImGui::IsKeyDown (entry.key);
}

// The key silhouette: square shoulders, rounded toe.
void drawKey (ImDrawList& dl, ImVec2 tl, ImVec2 br, float rounding, ImU32 top, ImU32 mid,
              ImU32 bottom, ImU32 edge, ImU32 toe, float toeHeight, float thickness)
{
    const float breakY = tl.y + (br.y - tl.y) * 0.62f;
    dl.AddRectFilled (tl, br, bottom, rounding, ImDrawFlags_RoundCornersBottom);
    dl.AddRectFilledMultiColor (tl, ImVec2 (br.x, breakY), top, top, mid, mid);
    dl.AddRectFilledMultiColor (ImVec2 (tl.x, breakY), ImVec2 (br.x, br.y - toeHeight),
                                mid, mid, bottom, bottom);
    dl.AddRectFilled (ImVec2 (tl.x, br.y - toeHeight), br, toe, rounding,
                      ImDrawFlags_RoundCornersBottom);
    dl.AddRect (tl, br, edge, rounding, ImDrawFlags_RoundCornersBottom, thickness);
}

class VirtualKeyboardView final : public DuskPanelView
{
public:
    VirtualKeyboardView (AudioEngine& engineRef,
                         std::function<void (int, int, int)> onNoteOn,
                         std::function<void (int, int)> onNoteOff)
        : engine (engineRef), noteOn (std::move (onNoteOn)), noteOff (std::move (onNoteOff))
    {
        // Clamp to the range shiftCentre enforces, so a hand-edited config cannot
        // seed a centre the steppers can never leave.
        centreNote = std::clamp (appconfig::getVkbCentreNote(), 0, 120);
    }

    ~VirtualKeyboardView() override { releaseAll(); }

    ImVec2 preferredSize() const override { return ImVec2 (kPanelW, kPanelH); }

    // The layout's letters overlap the shell's single-key shortcuts, so the panel
    // owns them for as long as it is up. Claim is layout membership rather than the
    // note the current octave resolves to: a key the centre pushed past MIDI 127
    // sounds nothing but must still not drop a punch point behind the panel.
    bool claimsShortcut (ShellShortcut shortcut) const override
    {
        return shortcut == ShellShortcut::record        // R
            || shortcut == ShellShortcut::togglePunch;  // P
    }

    bool takeDismissRequest() override
    {
        const bool wanted = dismissRequested;
        dismissRequested = false;
        return wanted;
    }

    void draw (dw::Context& ctx, ImVec2 origin, ImVec2 size) override
    {
        const float scale = ctx.scale;
        ctx.dl->AddRectFilled (origin, ImVec2 (origin.x + size.x, origin.y + size.y),
                               rgba (0x1a1a20ff));

        drawHeader (ctx, origin, size);

        const ImVec2 kbTl (origin.x, origin.y + scale * kHeaderH);
        const ImVec2 kbBr (origin.x + size.x, origin.y + size.y);

        // K opened the keyboard and closes it, which under JUCE happened because an
        // unconsumed key walked up to the shell. It is not a layout key, so taking
        // it here costs the panel nothing.
        if (dw::shortcutsAvailable (ctx) && ImGui::IsKeyPressed (ImGuiKey_K, false))
            dismissRequested = true;

        handleTyping();
        handleMouse (ctx, kbTl, kbBr);
        drawKeyboard (ctx, kbTl, kbBr);
    }

private:
    struct HeldNote
    {
        int note = -1;
        int channel = -1;
        int silentFrames = 0;
    };

    void drawHeader (dw::Context& ctx, ImVec2 origin, ImVec2 size)
    {
        const float scale = ctx.scale;
        const float top = origin.y;
        const float bottom = top + scale * kHeaderH;

        dw::text (ctx, ctx.fonts->title, scale * 13.0f,
                  ImVec2 (origin.x + scale * 10.0f, top + scale * 4.0f), size.x,
                  rgba (0xd0d0d0ff), "VIRTUAL MIDI KEYBOARD", dw::Align::left);

        dw::ButtonStyle style;
        style.offFill = rgba (0x262630ff);
        style.offText = rgba (0xd0d0d4ff);
        style.fontSize = 10.0f * scale;

        float right = origin.x + size.x;
        const auto stepper = [&] (const char* id, const char* label, int delta, bool octave)
        {
            const ImVec2 br (right, bottom - scale * 2.0f);
            const ImVec2 tl (right - scale * kButtonW, top + scale * 2.0f);
            if (dw::textButton (ctx, id, tl, br, label, false, style).clicked)
            {
                if (octave) shiftCentre (delta);
                else        shiftChannel (delta);
            }
            right = tl.x;
        };

        stepper ("##ch-up", "Ch +", 1, false);
        right -= scale * kButtonGap;
        stepper ("##ch-down", "Ch -", -1, false);
        right -= scale * (kButtonGap + 12.0f);
        stepper ("##oct-up", "Oct +", 12, true);
        right -= scale * kButtonGap;
        stepper ("##oct-down", "Oct -", -12, true);

        char status[64];
        std::snprintf (status, sizeof status, "CH %d   Centre: %s", channel,
                       dusk::midiNoteName (centreNote).c_str());
        dw::text (ctx, ctx.fonts->band, scale * 12.0f,
                  ImVec2 (origin.x + scale * 10.0f, top + scale * 5.0f),
                  (right - scale * 4.0f) - (origin.x + scale * 10.0f),
                  rgba (0x8090a0ff), status, dw::Align::right);
    }

    void handleTyping()
    {
        // Arrow keys move the centre and the channel, and never reach the shell:
        // nothing behind the panel binds them, and the panel is where they belong
        // while it is up.
        if (ImGui::IsKeyPressed (ImGuiKey_UpArrow, false))    shiftCentre (12);
        if (ImGui::IsKeyPressed (ImGuiKey_DownArrow, false))  shiftCentre (-12);
        if (ImGui::IsKeyPressed (ImGuiKey_LeftArrow, false))  shiftChannel (-1);
        if (ImGui::IsKeyPressed (ImGuiKey_RightArrow, false)) shiftChannel (1);

        for (std::size_t i = 0; i < layout().size(); ++i)
        {
            const auto& entry = layout()[i];
            auto& slot = held[i];
            // Start a note from this panel's own key state, sustain it from the X
            // server's. XQueryKeymap reports physical state for the whole server, so
            // taking the start edge from it would sound whatever the user types in
            // another window while the panel happens to be open - the panel keeps
            // drawing, and so polling, whether or not it holds the focus. Once a note
            // is sounding the server's state is the point: it is what stops the note
            // dropping between the event pairs an auto-repeating key produces.
            const bool down = slot.note >= 0 ? keyIsDown (entry)
                                             : ImGui::IsKeyDown (entry.key);

            if (down)
            {
                slot.silentFrames = 0;
                if (slot.note < 0)
                {
                    const int note = centreNote + entry.offset;
                    // A layout key the octave pushed past MIDI 127 sounds nothing,
                    // and takes no slot, but claimsShortcut still owns it.
                    if (note >= 0 && note <= 127)
                    {
                        slot.note = note;
                        slot.channel = channel;
                        sendNoteOn (note, channel);
                    }
                }
                continue;
            }

            if (slot.note < 0)
                continue;
            if (++slot.silentFrames >= kReleaseFrames)
            {
                sendNoteOff (slot.note, slot.channel);
                slot = {};
            }
        }
    }

    void handleMouse (dw::Context& ctx, ImVec2 tl, ImVec2 br)
    {
        dw::hitArea (ctx, "##vkb-keys", tl, br);

        if (ImGui::IsItemActive())
        {
            const int note = noteAtPoint (ImGui::GetIO().MousePos, tl, br);
            if (note >= 0 && note != mouseHeld.note)
            {
                // Glissando: the previous note releases before the new one sounds.
                if (mouseHeld.note >= 0)
                    sendNoteOff (mouseHeld.note, mouseHeld.channel);
                mouseHeld = { note, channel, 0 };
                sendNoteOn (note, channel);
            }
            return;
        }

        if (mouseHeld.note >= 0)
        {
            sendNoteOff (mouseHeld.note, mouseHeld.channel);
            mouseHeld = {};
        }
    }

    int firstNote() const noexcept { return std::max (0, centreNote - kContextBelow); }
    int lastNote() const noexcept { return std::min (127, centreNote + kTopTypedOffset); }

    int whiteKeyCount() const noexcept
    {
        int count = 0;
        for (int m = firstNote(); m <= lastNote(); ++m)
            if (! isBlackKey (m))
                ++count;
        return std::max (1, count);
    }

    int noteAtPoint (ImVec2 at, ImVec2 tl, ImVec2 br) const
    {
        if (at.x < tl.x || at.x >= br.x || at.y < tl.y || at.y >= br.y)
            return -1;

        const float whiteW = (br.x - tl.x) / static_cast<float> (whiteKeyCount());
        const float blackW = whiteW * 0.62f;
        const float blackH = (br.y - tl.y) * 0.62f;

        // Black keys first: they sit on top of the whites.
        float x = tl.x;
        for (int m = firstNote(); m <= lastNote(); ++m)
        {
            if (! isBlackKey (m))
            {
                x += whiteW;
                continue;
            }
            const float bx = x - blackW * 0.5f;
            if (at.x >= bx && at.x < bx + blackW && at.y < tl.y + blackH)
                return m;
        }

        x = tl.x;
        for (int m = firstNote(); m <= lastNote(); ++m)
        {
            if (isBlackKey (m))
                continue;
            if (at.x >= x && at.x < x + whiteW)
                return m;
            x += whiteW;
        }
        return -1;
    }

    const char* letterForNote (int note) const
    {
        const int offset = note - centreNote;
        for (const auto& entry : layout())
            if (entry.offset == offset)
                return entry.label;
        return nullptr;
    }

    void drawKeyboard (dw::Context& ctx, ImVec2 tl, ImVec2 br)
    {
        const float scale = ctx.scale;
        auto& dl = *ctx.dl;

        std::array<bool, 128> sounding {};
        for (const auto& slot : held)
            if (slot.note >= 0)
                sounding[static_cast<std::size_t> (slot.note)] = true;
        if (mouseHeld.note >= 0)
            sounding[static_cast<std::size_t> (mouseHeld.note)] = true;

        const float whiteW = (br.x - tl.x) / static_cast<float> (whiteKeyCount());
        const float whiteH = br.y - tl.y;
        const float blackW = whiteW * 0.62f;
        const float blackH = whiteH * 0.62f;

        float x = tl.x;
        for (int m = firstNote(); m <= lastNote(); ++m)
        {
            if (isBlackKey (m))
                continue;

            const bool active = sounding[static_cast<std::size_t> (m)];
            const ImVec2 keyTl (x, tl.y);
            const ImVec2 keyBr (x + whiteW - scale, br.y);
            drawKey (dl, keyTl, keyBr, scale * 4.0f,
                     active ? rgba (0x4a90d8ff) : rgba (0xe6e7eaff),
                     active ? rgba (0x5fa0e0ff) : rgba (0xffffffff),
                     active ? rgba (0x3a78b8ff) : rgba (0xfdfdfeff),
                     rgba (0x303036ff),
                     active ? rgba (0x3a78b8ff) : rgba (0xcccdd2ff),
                     scale * 3.0f, scale * 0.8f);

            if (((m % 12) + 12) % 12 == 0)
                dw::text (ctx, ctx.fonts->value, scale * 10.0f,
                          ImVec2 (keyTl.x + scale * 4.0f, keyBr.y - scale * 15.0f),
                          whiteW - scale * 8.0f,
                          active ? rgba (0xe8f0faff) : rgba (0x6a6a72ff),
                          dusk::midiNoteName (m).c_str(), dw::Align::left);

            if (const char* const letter = letterForNote (m))
                dw::text (ctx, ctx.fonts->title, scale * 13.0f,
                          ImVec2 (keyTl.x, keyBr.y - scale * 32.0f), whiteW,
                          active ? rgba (0x10283eff) : rgba (0x707078ff), letter);
            x += whiteW;
        }

        x = tl.x;
        for (int m = firstNote(); m <= lastNote(); ++m)
        {
            if (! isBlackKey (m))
            {
                x += whiteW;
                continue;
            }

            const bool active = sounding[static_cast<std::size_t> (m)];
            const ImVec2 keyTl (x - blackW * 0.5f, tl.y);
            const ImVec2 keyBr (keyTl.x + blackW, tl.y + blackH);
            drawKey (dl, keyTl, keyBr, scale * 3.0f,
                     active ? rgba (0x3a78b8ff) : rgba (0x45454dff),
                     active ? rgba (0x4a90d8ff) : rgba (0x2a2a30ff),
                     active ? rgba (0x2c5a8aff) : rgba (0x0b0b0fff),
                     rgba (0x000000ff),
                     active ? rgba (0x2c5a8aff) : rgba (0x0b0b0fff),
                     scale * 2.0f, scale * 0.8f);

            // The glossy cap near the top edge is what reads as a raised, bevelled
            // key rather than a painted rectangle.
            const float capInset = blackW * 0.18f;
            const ImVec2 capTl (keyTl.x + capInset, keyTl.y + blackH * 0.06f);
            const ImVec2 capBr (keyBr.x - capInset, capTl.y + blackH * 0.16f);
            dl.AddRectFilled (capTl, capBr,
                              dw::withAlpha (active ? rgba (0x8fc0f0ff) : rgba (0x6a6a74ff),
                                             0.55f),
                              scale * 1.5f);

            if (const char* const letter = letterForNote (m))
                dw::text (ctx, ctx.fonts->pill, scale * 11.0f,
                          ImVec2 (keyTl.x, keyBr.y - scale * 14.0f), blackW,
                          active ? rgba (0xeaf2fcff) : rgba (0xc0c0c8ff), letter);
        }
    }

    void shiftCentre (int semitones)
    {
        const int previous = centreNote;
        centreNote = std::clamp (centreNote + semitones, 0, 120);
        if (centreNote == previous)
            return;
        // Held slots still name the old centre's notes, so they go before the shift
        // can orphan their note-offs.
        releaseAll();
        appconfig::setVkbCentreNote (centreNote);
    }

    void shiftChannel (int delta)
    {
        channel = std::clamp (channel + delta, 1, 16);
    }

    // Typing and the mouse can hold the same key at once, so a note counts its
    // sources: the synth hears one note-on when the first arrives and one note-off
    // only when the last lets go, rather than a release from either cutting the
    // note the other is still holding.
    std::uint8_t& sourceCount (int note, int chan) noexcept
    {
        return sources[static_cast<std::size_t> (note & 0x7f) * 16u
                       + static_cast<std::size_t> (std::clamp (chan, 1, 16) - 1)];
    }

    void sendNoteOn (int note, int chan)
    {
        if (++sourceCount (note, chan) > 1)
            return;

        // Clamp rather than mask: masking wraps an out-of-range channel onto a
        // different one instead of the nearest valid one.
        const std::uint8_t bytes[3] {
            static_cast<std::uint8_t> (0x90 | (std::clamp (chan, 1, 16) - 1)),
            static_cast<std::uint8_t> (note & 0x7f),
            static_cast<std::uint8_t> (kVelocity & 0x7f) };
        engine.postVirtualKeyboardMidi (bytes, 3);
        if (noteOn)
            noteOn (note, kVelocity, chan);
    }

    void sendNoteOff (int note, int chan)
    {
        auto& count = sourceCount (note, chan);
        if (count == 0 || --count > 0)
            return;

        const std::uint8_t bytes[3] {
            static_cast<std::uint8_t> (0x80 | (std::clamp (chan, 1, 16) - 1)),
            static_cast<std::uint8_t> (note & 0x7f), 0 };
        engine.postVirtualKeyboardMidi (bytes, 3);
        if (noteOff)
            noteOff (note, chan);
    }

    void releaseAll()
    {
        for (auto& slot : held)
        {
            if (slot.note < 0)
                continue;
            sendNoteOff (slot.note, slot.channel);
            slot = {};
        }
        if (mouseHeld.note >= 0)
        {
            sendNoteOff (mouseHeld.note, mouseHeld.channel);
            mouseHeld = {};
        }
    }

    static constexpr int kVelocity = 100;

    AudioEngine& engine;
    std::function<void (int, int, int)> noteOn;
    std::function<void (int, int)> noteOff;
    std::array<HeldNote, 29> held {};
    HeldNote mouseHeld {};
    std::array<std::uint8_t, 128 * 16> sources {};
    int centreNote = 36;
    int channel = 1;
    bool dismissRequested = false;
};
} // namespace

std::unique_ptr<DuskPanelView> makeVirtualKeyboardView (
    AudioEngine& engine,
    std::function<void (int, int, int)> noteOn,
    std::function<void (int, int)> noteOff)
{
    return std::unique_ptr<DuskPanelView> (
        new VirtualKeyboardView (engine, std::move (noteOn), std::move (noteOff)));
}
} // namespace duskstudio::imgui
