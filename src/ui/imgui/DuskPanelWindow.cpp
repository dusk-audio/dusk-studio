#include "DuskPanelWindow.h"
#include "DuskImGuiHost.h"
#include "DuskTheme.h"
#include "PanelText.h"
#include "PanelFontFallback.h"
#include "../../foundation/AppConfigDir.h"
#include "../../foundation/MessageThread.h"

#include <DearImGui.hpp>
#include <DearImGui/imgui_internal.h>
#include <OpenGL.hpp>
#ifndef DGL_NO_SHARED_RESOURCES
# include "src/Resources.hpp"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace duskstudio::imgui
{
namespace
{
namespace dw = DuskWidgets;

// EmbeddedModal's Backdrop, in the framework's colour order. A ported panel has to
// land in the same plate the JUCE modals draw, or the family stops reading as one.
constexpr float kPlateMargin = 6.0f;
constexpr float kPlateRounding = 8.0f;
constexpr unsigned int kPlateFill = 0x202024ffu;
constexpr unsigned int kPlateBorder = 0x3a3a42ffu;

// A scenario's input counts as seen once the view has drawn this many frames with
// nothing left in the queue: the frame that took the last event, and the one after
// it for whatever that event set in motion.
constexpr int kSettledFrames = 2;
constexpr float kFrameSeconds = 1.0f / 60.0f;
int panelsWithUnseenScenarioInput = 0;
int panelFramesDrawing = 0;

// A panel draws only when a frame is wanted: for kInputHold after input and at least
// kFramesAfterInput frames, while Dear ImGui is in the middle of something, when the
// view's liveStamp changes, and every kFrameFloor whatever happens, so a model change
// a view's stamp misses is late rather than lost.
constexpr auto kInputHold = std::chrono::milliseconds (750);
constexpr int kFramesAfterInput = 3;
constexpr auto kFrameFloor = std::chrono::milliseconds (250);

// The event pump and the geometry poll run at kActivePumpMs while frames are wanted
// and at kIdlePumpMs once kInputHold passes without one. Input a child reads from its
// own display connection waits for the next pump, so an idle panel answers the first
// event at most kIdlePumpMs late. Input and state the shell hands a panel from outside
// its own event handlers must call wake(), which asks for the frames input asks for
// and puts an idle pump back on kActivePumpMs, so the panel draws within that.
constexpr int kActivePumpMs = 16;
constexpr int kIdlePumpMs = 32;

bool scenarioFramesOnDemand = false;
bool scenarioFrameFloor = true;

// The scenario runner and the screenshot harness count frames and wait for them, so
// under either one the panels draw continuously unless a case asks otherwise.
bool harnessDrawsContinuously()
{
    static const bool continuous = std::getenv ("DUSKSTUDIO_RUN_SCENARIOS") != nullptr
                                || std::getenv ("DUSKSTUDIO_CAPTURE_DIR") != nullptr;
    return continuous && ! scenarioFramesOnDemand;
}

// A scenario run can hold the panels to the frame rate of a loaded software
// renderer, which is where a case's timing assumptions break. 0 leaves them alone.
int scenarioFrameIntervalMs()
{
    static const int interval = []
    {
        const char* const spec = std::getenv ("DUSKSTUDIO_SCENARIO_FRAME_MS");
        if (spec == nullptr || std::getenv ("DUSKSTUDIO_RUN_SCENARIOS") == nullptr)
            return 0;
        return std::clamp (std::atoi (spec), 0, 1000);
    }();
    return interval;
}

ImU32 rgba (unsigned int hex)
{
    return IM_COL32 ((hex >> 24) & 0xff, (hex >> 16) & 0xff, (hex >> 8) & 0xff, hex & 0xff);
}

struct ShortcutBinding
{
    ImGuiKey key;
    ShellShortcut shortcut;
};

// The bare keys EmbeddedModal forwards. A modifier chord is never one of them: the
// shell binds Ctrl+R and Cmd+. to entirely different things, and reading a chord as
// its unmodified key would fire the wrong one from behind a panel.
const std::array<ShortcutBinding, 9>& shortcutBindings()
{
    static const std::array<ShortcutBinding, 9> bindings { {
        { ImGuiKey_Space, ShellShortcut::playStop },
        { ImGuiKey_R, ShellShortcut::record },
        { ImGuiKey_Home, ShellShortcut::playheadToZero },
        { ImGuiKey_Period, ShellShortcut::stopAndRewind },
        { ImGuiKey_L, ShellShortcut::toggleLoop },
        { ImGuiKey_P, ShellShortcut::togglePunch },
        { ImGuiKey_LeftBracket, ShellShortcut::setLoopIn },
        { ImGuiKey_RightBracket, ShellShortcut::setLoopOut },
        { ImGuiKey_F11, ShellShortcut::toggleFullscreen },
    } };
    return bindings;
}

// Held with the command key, these are the session's own shortcuts.
constexpr std::array<ImGuiKey, 4> kSessionKeys { ImGuiKey_Z, ImGuiKey_Y, ImGuiKey_S, ImGuiKey_Q };

// A native panel renders into its own framework child, which the JUCE screenshot
// harness cannot reach through createComponentSnapshot. The application reads its
// own frame back instead, the way the gate spike's --capture does, so the manual's
// figures for a ported panel come from the same run as everything else.
//
// PPM because it needs no encoder; the capture script converts.
bool writePpm (const std::string& path, int width, int height,
               const std::vector<unsigned char>& rgb)
{
    std::FILE* const file = std::fopen (path.c_str(), "wb");
    if (file == nullptr)
        return false;
    std::fprintf (file, "P6\n%d %d\n255\n", width, height);
    const bool ok = std::fwrite (rgb.data(), 1, rgb.size(), file) == rgb.size();
    std::fclose (file);
    return ok;
}

std::filesystem::path firstFrameMarkerPath (const std::string& logTag)
{
    const auto cfg = dusk::fs::appConfigDir();
    if (cfg.empty())
        return {};
    return cfg / (logTag + "-first-frame");
}

struct NamedKey
{
    const char* name;
    ImGuiKey key;
    unsigned int code;
    bool forwardToShell = true;
};

const std::array<NamedKey, 34>& namedKeys()
{
    static const std::array<NamedKey, 34> keys { {
        { "spacebar", ImGuiKey_Space, DGL::kKeySpace },
        { "space", ImGuiKey_Space, DGL::kKeySpace },
        { "return", ImGuiKey_Enter, DGL::kKeyEnter },
        { "enter", ImGuiKey_Enter, DGL::kKeyEnter },
        { "numpad enter", ImGuiKey_KeypadEnter, DGL::kKeyPadEnter },
        { "escape", ImGuiKey_Escape, DGL::kKeyEscape },
        { "backspace", ImGuiKey_Backspace, DGL::kKeyBackspace },
        { "delete", ImGuiKey_Delete, DGL::kKeyDelete },
        { "tab", ImGuiKey_Tab, '\t' },
        { "home", ImGuiKey_Home, DGL::kKeyHome },
        { "end", ImGuiKey_End, DGL::kKeyEnd },
        { "page up", ImGuiKey_PageUp, DGL::kKeyPageUp },
        { "page down", ImGuiKey_PageDown, DGL::kKeyPageDown },
        { "cursor left", ImGuiKey_LeftArrow, DGL::kKeyLeft },
        { "cursor right", ImGuiKey_RightArrow, DGL::kKeyRight },
        { "cursor up", ImGuiKey_UpArrow, DGL::kKeyUp },
        { "cursor down", ImGuiKey_DownArrow, DGL::kKeyDown },
        { "insert", ImGuiKey_Insert, DGL::kKeyInsert },
        { "f11", ImGuiKey_F11, DGL::kKeyF11 },
        { "numpad +", ImGuiKey_KeypadAdd, DGL::kKeyPadAdd },
        { "numpad add", ImGuiKey_KeypadAdd, DGL::kKeyPadAdd },
        { "numpad -", ImGuiKey_KeypadSubtract, DGL::kKeyPadSubtract },
        { "numpad subtract", ImGuiKey_KeypadSubtract, DGL::kKeyPadSubtract },
        { "left ctrl", ImGuiKey_LeftCtrl, DGL::kKeyControlL, false },
        { "right ctrl", ImGuiKey_RightCtrl, DGL::kKeyControlR, false },
        { "left alt", ImGuiKey_LeftAlt, DGL::kKeyAltL, false },
        { "right alt", ImGuiKey_RightAlt, DGL::kKeyAltR, false },
        { "left shift", ImGuiKey_LeftShift, DGL::kKeyShiftL, false },
        { "right shift", ImGuiKey_RightShift, DGL::kKeyShiftR, false },
        { "left super", ImGuiKey_LeftSuper, DGL::kKeySuperL, false },
        { "right super", ImGuiKey_RightSuper, DGL::kKeySuperR, false },
        { "caps lock", ImGuiKey_CapsLock, DGL::kKeyCapsLock, false },
        { "num lock", ImGuiKey_NumLock, DGL::kKeyNumLock, false },
        { "scroll lock", ImGuiKey_ScrollLock, DGL::kKeyScrollLock, false },
    } };
    return keys;
}

// The printable keys, by the character the key carries unshifted.
struct PrintableKey
{
    char character;
    ImGuiKey key;
};

const std::array<PrintableKey, 11>& printableKeys()
{
    static const std::array<PrintableKey, 11> keys { {
        { '[', ImGuiKey_LeftBracket }, { ']', ImGuiKey_RightBracket }, { '=', ImGuiKey_Equal },
        { '-', ImGuiKey_Minus }, { '.', ImGuiKey_Period }, { ',', ImGuiKey_Comma },
        { '/', ImGuiKey_Slash }, { ';', ImGuiKey_Semicolon }, { '\'', ImGuiKey_Apostrophe },
        { '\\', ImGuiKey_Backslash }, { '`', ImGuiKey_GraveAccent },
    } };
    return keys;
}

// The framework's key code for a chord's key: the unshifted character for a printable
// key, the framework's own code otherwise. Zero for a key it has no code for.
unsigned int frameworkKeyCode (ImGuiKey key)
{
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z)
        return static_cast<unsigned int> ('a' + (key - ImGuiKey_A));
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9)
        return static_cast<unsigned int> ('0' + (key - ImGuiKey_0));
    for (const auto& printable : printableKeys())
        if (printable.key == key)
            return static_cast<unsigned char> (printable.character);
    for (const auto& named : namedKeys())
        if (named.key == key)
            return named.code;
    return 0;
}

// Pointer and key modifiers in the framework's bits. Cmd is the command key on macOS
// and Ctrl everywhere else, as JUCE reads "command".
unsigned int frameworkModifiers (bool shift, bool ctrl, bool super, bool alt)
{
    return (shift ? DGL::kModifierShift : 0u) | (ctrl ? DGL::kModifierControl : 0u)
         | (super ? DGL::kModifierSuper : 0u) | (alt ? DGL::kModifierAlt : 0u);
}

void encodeUtf8 (std::uint32_t c, char (&out)[8])
{
    std::fill (std::begin (out), std::end (out), '\0');
    const auto byte = [] (std::uint32_t v) { return static_cast<char> (static_cast<unsigned char> (v)); };
    if (c < 0x80)
    {
        out[0] = byte (c);
    }
    else if (c < 0x800)
    {
        out[0] = byte (0xc0 | (c >> 6));
        out[1] = byte (0x80 | (c & 0x3f));
    }
    else if (c < 0x10000)
    {
        out[0] = byte (0xe0 | (c >> 12));
        out[1] = byte (0x80 | ((c >> 6) & 0x3f));
        out[2] = byte (0x80 | (c & 0x3f));
    }
    else
    {
        out[0] = byte (0xf0 | (c >> 18));
        out[1] = byte (0x80 | ((c >> 12) & 0x3f));
        out[2] = byte (0x80 | ((c >> 6) & 0x3f));
        out[3] = byte (0x80 | (c & 0x3f));
    }
}

// The code point starting at `at`, advancing past it. A malformed sequence yields
// U+FFFD and consumes one byte.
std::uint32_t decodeUtf8 (const std::string& text, std::size_t& at)
{
    const auto lead = static_cast<unsigned char> (text[at++]);
    if (lead < 0x80)
        return lead;
    const int extra = lead >= 0xf0 && lead < 0xf8 ? 3 : lead >= 0xe0 ? 2 : lead >= 0xc0 ? 1 : -1;
    if (extra < 0 || at + static_cast<std::size_t> (extra) > text.size())
        return 0xfffd;
    std::uint32_t c = lead & (0x3fu >> extra);
    for (int i = 0; i < extra; ++i)
    {
        const auto next = static_cast<unsigned char> (text[at + static_cast<std::size_t> (i)]);
        if ((next & 0xc0) != 0x80)
            return 0xfffd;
        c = (c << 6) | (next & 0x3fu);
    }
    at += static_cast<std::size_t> (extra);
    return c;
}

bool commandIsSuper()
{
   #if defined (__APPLE__)
    return true;
   #else
    return false;
   #endif
}

DGL::MouseCursor frameworkCursor (ImGuiMouseCursor cursor)
{
    switch (cursor)
    {
        case ImGuiMouseCursor_None:       return DGL::kMouseCursorNone;
        case ImGuiMouseCursor_TextInput:  return DGL::kMouseCursorCaret;
        case ImGuiMouseCursor_ResizeAll:  return DGL::kMouseCursorAllScroll;
        case ImGuiMouseCursor_ResizeNS:   return DGL::kMouseCursorUpDown;
        case ImGuiMouseCursor_ResizeEW:   return DGL::kMouseCursorLeftRight;
        case ImGuiMouseCursor_ResizeNESW: return DGL::kMouseCursorUpRightDownLeft;
        case ImGuiMouseCursor_ResizeNWSE: return DGL::kMouseCursorUpLeftDownRight;
        case ImGuiMouseCursor_Hand:       return DGL::kMouseCursorHand;
        case ImGuiMouseCursor_NotAllowed: return DGL::kMouseCursorNotAllowed;
        default:                          return DGL::kMouseCursorArrow;
    }
}
} // namespace

std::string shellKeyName (ImGuiKey key)
{
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z)
        return std::string (1, static_cast<char> ('a' + key - ImGuiKey_A));
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9)
        return std::string (1, static_cast<char> ('0' + key - ImGuiKey_0));
    if (key == ImGuiKey_KeypadEnter) return "return";
    for (const auto& printable : printableKeys())
        if (printable.key == key)
            return std::string (1, printable.character);
    for (const auto& named : namedKeys())
        if (named.key == key && named.forwardToShell)
            return named.name;
    return {};
}

std::uint32_t shellKeyText (const KeyChord& chord)
{
    if (chord.ctrl || chord.super || chord.alt)
        return 0;
    if (chord.key == ImGuiKey_Space)
        return ' ';
    if (chord.key >= ImGuiKey_A && chord.key <= ImGuiKey_Z)
        return static_cast<std::uint32_t> ((chord.shift ? 'A' : 'a') + (chord.key - ImGuiKey_A));
    static constexpr std::string_view unshifted = "0123456789[]=-.,/;'\\`";
    static constexpr std::string_view shifted   = ")!@#$%^&*({}+_><?:\"|~";
    static_assert (unshifted.size() == shifted.size());
    const auto name = shellKeyName (chord.key);
    const auto at = name.size() == 1 ? unshifted.find (name.front()) : std::string_view::npos;
    if (at == std::string_view::npos)
        return 0;
    return static_cast<unsigned char> ((chord.shift ? shifted : unshifted)[at]);
}

std::optional<KeyChord> parseKeyDescription (const std::string& description)
{
    std::vector<std::string> parts;
    for (std::size_t start = 0;;)
    {
        const auto at = description.find (" + ", start);
        parts.push_back (description.substr (start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos)
            break;
        start = at + 3;
    }

    const auto lower = [] (std::string text)
    {
        for (auto& c : text)
            c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
        return text;
    };

    KeyChord chord;
    auto name = parts.back();
    parts.pop_back();
    for (const auto& part : parts)
    {
        const auto modifier = lower (part);
        if (modifier == "shift") chord.shift = true;
        else if (modifier == "ctrl" || modifier == "control") chord.ctrl = true;
        else if (modifier == "alt" || modifier == "option") chord.alt = true;
        else if (modifier == "command" || modifier == "cmd")
            (commandIsSuper() ? chord.super : chord.ctrl) = true;
        else return std::nullopt;
    }

    if (name.size() > 1 && name.front() == '#')
    {
        unsigned int code = 0;
        const auto* end = name.data() + name.size();
        const auto parsed = std::from_chars (name.data() + 1, end, code, 16);
        if (parsed.ec != std::errc {} || parsed.ptr != end || code > 0x7f)
            return std::nullopt;
        name.assign (1, static_cast<char> (code));
    }

    if (name.size() == 1)
    {
        const char c = name.front();
        const auto folded = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
        if (folded >= 'a' && folded <= 'z')
            chord.key = static_cast<ImGuiKey> (ImGuiKey_A + (folded - 'a'));
        else if (c >= '0' && c <= '9')
            chord.key = static_cast<ImGuiKey> (ImGuiKey_0 + (c - '0'));
        else if (c == ' ')
            chord.key = ImGuiKey_Space;
        else
        {
            // Some platforms report the shifted glyph rather than the key under it.
            static constexpr std::array<std::pair<char, char>, 4> shifted { {
                { '{', '[' }, { '}', ']' }, { '+', '=' }, { '_', '-' } } };
            char unshifted = c;
            for (const auto& [glyph, base] : shifted)
                if (c == glyph)
                {
                    unshifted = base;
                    chord.shift = true;
                }
            for (const auto& printable : printableKeys())
                if (printable.character == unshifted)
                    chord.key = printable.key;
        }
    }
    else
    {
        const auto lowered = lower (name);
        for (const auto& named : namedKeys())
            if (lowered == named.name)
                chord.key = named.key;
    }

    if (chord.key == ImGuiKey_None)
        return std::nullopt;
    return chord;
}

std::optional<ShellShortcut> shellShortcutFor (const KeyChord& chord)
{
    if (chord.alt)
        return std::nullopt;
    const bool command = commandIsSuper() ? chord.super : chord.ctrl;
    const bool otherModifier = commandIsSuper() ? chord.ctrl : chord.super;
    if (command && ! otherModifier)
    {
        switch (chord.key)
        {
            case ImGuiKey_Z: return chord.shift ? ShellShortcut::redo : ShellShortcut::undo;
            case ImGuiKey_S: return chord.shift ? ShellShortcut::saveAs : ShellShortcut::save;
            case ImGuiKey_Y: return chord.shift ? std::nullopt : std::optional<ShellShortcut> (ShellShortcut::redo);
            case ImGuiKey_Q: return chord.shift ? std::nullopt : std::optional<ShellShortcut> (ShellShortcut::quit);
            default:         return std::nullopt;
        }
    }
    if (chord.ctrl || chord.super)
        return std::nullopt;
    for (const auto& binding : shortcutBindings())
    {
        if (binding.key != chord.key)
            continue;
        if (! chord.shift)
            return binding.shortcut;
        // Shift turns the bracket keys into the punch pair, the way the shell's own
        // bindings read the shifted glyph.
        if (binding.shortcut == ShellShortcut::setLoopIn)
            return ShellShortcut::setPunchIn;
        if (binding.shortcut == ShellShortcut::setLoopOut)
            return ShellShortcut::setPunchOut;
        return std::nullopt;
    }
    return std::nullopt;
}

bool operator!= (const DuskPanelWindow::Geometry& a, const DuskPanelWindow::Geometry& b)
{
    return a.x != b.x || a.y != b.y || a.width != b.width || a.height != b.height
        || a.scaleFactor < b.scaleFactor || b.scaleFactor < a.scaleFactor;
}

struct DuskPanelWindow::Impl final : private dusk::Timer
{
    class PanelWidget final : public DGL::ImGuiTopLevelWidget
    {
    public:
        PanelWidget (DGL::Window& window, Impl& ownerRef)
            : DGL::ImGuiTopLevelWidget (window, 13.0f), owner (ownerRef)
        {
            lastInputAt = std::chrono::steady_clock::now();
            lastActivity = "the panel opening";
            framesOwed = kFramesAfterInput;
            owner.pace (true);
        }

        ~PanelWidget() override
        {
            if (quietFrames < kSettledFrames)
                --panelsWithUnseenScenarioInput;
        }

        void clickForScenario (ImVec2 point)
        {
            noteScenarioInput();
            MotionEvent motion;
            motion.pos = { point.x, point.y };
            motion.absolutePos = motion.pos;
            onMotion (motion);
            MouseEvent click;
            click.button = DGL::kMouseButtonLeft;
            click.pos = motion.pos;
            click.absolutePos = motion.pos;
            click.press = true;
            onMouse (click);
            releasePending = point;
        }

        // Both clicks are queued at once: the view takes one button event a frame and
        // the clock is held while any wait, so the two presses land two short frames
        // apart however long a frame takes to draw and whenever the runner's timers fire.
        void doubleClickForScenario (ImVec2 point)
        {
            noteScenarioInput();
            MotionEvent motion;
            motion.pos = { point.x, point.y };
            motion.absolutePos = motion.pos;
            onMotion (motion);
            MouseEvent click;
            click.button = DGL::kMouseButtonLeft;
            click.pos = motion.pos;
            click.absolutePos = motion.pos;
            for (const bool press : { true, false, true, false })
            {
                click.press = press;
                onMouse (click);
            }
        }

        void pointerForScenario (ImVec2 point, bool pressed, int modifiers)
        {
            noteScenarioInput();
            const bool command = (modifiers & scenarioCommand) != 0;
            const auto mods = frameworkModifiers ((modifiers & scenarioShift) != 0,
                                                  command && ! commandIsSuper(),
                                                  command && commandIsSuper(), false);
            MotionEvent motion;
            motion.mod = mods;
            motion.pos = { point.x, point.y };
            motion.absolutePos = motion.pos;
            onMotion (motion);
            MouseEvent button;
            if (pressed)
                heldButton = (modifiers & scenarioRightButton) != 0 ? DGL::kMouseButtonRight
                                                                     : DGL::kMouseButtonLeft;
            button.button = heldButton;
            button.mod = mods;
            button.pos = motion.pos;
            button.absolutePos = motion.pos;
            button.press = pressed;
            onMouse (button);
        }

        void scrollForScenario (double wheel)
        {
            noteScenarioInput();
            MotionEvent motion;
            motion.pos = { getWidth() * 0.5, getHeight() * 0.5 };
            motion.absolutePos = motion.pos;
            onMotion (motion);
            ScrollEvent scroll;
            scroll.pos = motion.pos;
            scroll.absolutePos = motion.pos;
            scroll.delta = { 0.0, wheel };
            onScroll (scroll);
        }

        void typeForScenario (const std::string& text)
        {
            noteScenarioInput();
            for (std::size_t at = 0; at < text.size();)
                type (decodeUtf8 (text, at));
        }

        bool inputForScenario (const std::string& input)
        {
            if (input == "focus-out")
            {
                focusForScenario (false);
                noteScenarioInput();
                return true;
            }
            if (input == "scroll-down")
            {
                scrollForScenario (-20.0);
                return true;
            }
            if (input == "hover" || input == "shift + hover")
            {
                noteScenarioInput();
                MotionEvent motion;
                if (input != "hover") motion.mod = DGL::kModifierShift;
                motion.pos = { getWidth() * 0.5, getHeight() * 0.5 };
                motion.absolutePos = motion.pos;
                onMotion (motion);
                return true;
            }
            const auto chord = parseKeyDescription (input);
            if (! chord || ! press (*chord))
                return false;
            noteScenarioInput();
            return true;
        }

        // The key as the platform would have delivered it to a focused child: the press
        // now, its release after the next frame, and the character it types.
        void replayShellKey (const std::string& description, std::uint32_t character)
        {
            const auto chord = parseKeyDescription (description);
            if (chord)
                press (*chord);
            const bool typesText = character >= 0x20 && character != 0x7f && character <= 0x10ffff;
            if (typesText && ! (chord && chord->command()))
                type (character);
        }

        bool hasKeyboard() const noexcept { return keyboardFocused; }

        // `what` names the event, for a scenario that finds a panel drawing when
        // nothing should have reached it.
        void noteActivity (const char* what)
        {
            const auto now = std::chrono::steady_clock::now();
            lastInputAt = now;
            lastActivity = what;
            framesOwed = kFramesAfterInput;
            owner.pace (true);
            // The first event after a rest draws at once. Later ones are left to the
            // idle callback, so a pointer reporting at 1 kHz cannot drive more than a
            // frame per tick on a platform that paints on every invalidate.
            if (now - drawnAt >= std::chrono::milliseconds (kActivePumpMs))
            {
                askedFor = what;
                repaint();
            }
        }

        void noteScenarioInput()
        {
            if (quietFrames >= kSettledFrames)
            {
                ++panelsWithUnseenScenarioInput;
                unseenSince = std::chrono::steady_clock::now();
            }
            quietFrames = 0;
        }

        void focusForScenario (bool focused)
        {
            FocusEvent event;
            event.focus = focused;
            event.mode = DGL::kCrossingNormal;
            onFocusChanged (event);
        }

    protected:
        void idleCallback() override
        {
            const auto now = std::chrono::steady_clock::now();
            if (frameWanted (now))
            {
                lastWantedAt = now;
                askedFor = wantedFor;
                DGL::ImGuiTopLevelWidget::idleCallback();
            }
            else
            {
                afterGap = true;
                if ((! scenarioFramesOnDemand || scenarioFrameFloor) && now - drawnAt >= kFrameFloor)
                {
                    askedFor = "the periodic frame";
                    DGL::ImGuiTopLevelWidget::idleCallback();
                }
            }
            owner.pace (now - lastWantedAt < kInputHold);
        }

        bool onMouse (const MouseEvent& event) override
        {
            noteActivity ("a mouse button");
            if (event.press && owner.callbacks.pressed)
                owner.callbacks.pressed();
            return DGL::ImGuiTopLevelWidget::onMouse (event);
        }

        bool onMotion (const MotionEvent& event) override
        {
            noteActivity ("pointer motion");
            return DGL::ImGuiTopLevelWidget::onMotion (event);
        }

        bool onScroll (const ScrollEvent& event) override
        {
            noteActivity ("a scroll");
            return DGL::ImGuiTopLevelWidget::onScroll (event);
        }

        bool onCharacterInput (const CharacterInputEvent& event) override
        {
            noteActivity ("typed text");
            return DGL::ImGuiTopLevelWidget::onCharacterInput (event);
        }

        void onResize (const ResizeEvent& event) override
        {
            noteActivity ("a resize");
            DGL::ImGuiTopLevelWidget::onResize (event);
        }

        void onFocusChanged (const FocusEvent& event) override
        {
            noteActivity ("a focus change");
            keyboardFocused = event.focus;
            DGL::ImGuiTopLevelWidget::onFocusChanged (event);
            // A grab crossing lends the focus to a grab and gets it back; it does
            // not mean the user has left the window.
            if (! event.focus && event.mode == DGL::kCrossingNormal && owner.view != nullptr)
                owner.view->focusLost();
        }

        // A key the widget reports as unused is handed to the host window, where the
        // shell's own bindings claim it - so a note letter typed at the panel would
        // also toggle mute behind it. The panel names the shortcuts it wants the shell
        // to keep through forwardShortcuts; every other key stays here.
        bool onKeyboard (const DGL::Widget::KeyboardEvent& event) override
        {
            noteActivity ("a key");
            auto key = event;
            // The framework's ImGui adapter does not map keypad Enter.
            if (key.key == DGL::kKeyPadEnter) key.key = DGL::kKeyEnter;
            DGL::ImGuiTopLevelWidget::onKeyboard (key);
            return true;
        }

        // Dear ImGui times a double-click on the wall clock, and the scenario runner
        // waits for each input to be drawn before it sends the next, so a slow
        // renderer would age two clicks apart. While scenario input is waiting to be
        // drawn the clock moves no faster than a 60 Hz frame; the time between one
        // input being drawn and the next being sent still counts in full.
        //
        // The frame after the input was drawn is held too. A slow renderer can start
        // that frame before the runner's timer for its next step fires, however short
        // the step's delay, so it is time no step asked to wait.
        void onImGuiPrepareFrame() override
        {
            imguiContext = ImGui::GetCurrentContext();
            // The clock ran on through the frames not drawn, and a hover delay or a
            // fade would otherwise jump by all of it.
            drawingAfterGap = std::exchange (afterGap, false);
            if (drawingAfterGap)
                ImGui::GetIO().DeltaTime = std::min (ImGui::GetIO().DeltaTime, kFrameSeconds);
            const auto scale = static_cast<float> (getWindow().getScaleFactor());
            bool glyphsChanged = false;
            if (owner.view != nullptr && owner.view->marksMissingGlyphs())
            {
                if (auto* input = ImGui::GetInputTextState (GImGui->ActiveId))
                    if (input->TextA.Data != nullptr && input->TextLen > 0)
                        owner.fontFallback.request ({ input->TextA.Data, static_cast<std::size_t> (input->TextLen) });
                glyphsChanged = owner.fontFallback.update (dw::FontSizes {}.textEntry * scale);
            }
            if (glyphsChanged || std::abs (owner.fontScale - scale) > 0.0001f)
            {
                owner.buildFonts (scale);
                rebuildFontTexture();
            }
            if (quietFrames >= kSettledFrames && ! std::exchange (holdNextFrame, false))
                return;
            const auto now = std::chrono::steady_clock::now();
            const float waited = std::chrono::duration<float> (now - unseenSince).count();
            unseenSince = now;
            auto& io = ImGui::GetIO();
            io.DeltaTime = std::max (io.DeltaTime - std::max (0.0f, waited - kFrameSeconds),
                                     std::min (io.DeltaTime, kFrameSeconds));
        }

        void onImGuiDisplay() override
        {
            if (std::exchange (drawingAfterGap, false))
                clockAfterGap = ImGui::GetIO().DeltaTime;
            owner.draw (static_cast<float> (getWidth()), static_cast<float> (getHeight()),
                        static_cast<float> (getWindow().getScaleFactor()));
            // The framework does not act on the cursor a view asks Dear ImGui for.
            if (const auto cursor = ImGui::GetMouseCursor(); cursor != appliedCursor)
            {
                appliedCursor = cursor;
                setCursor (frameworkCursor (cursor));
            }
            for (const auto code : std::exchange (keyReleases, {}))
            {
                KeyboardEvent key;
                key.key = code;
                onKeyboard (key);
            }
            if (const auto point = std::exchange (releasePending, std::nullopt))
            {
                // Native focus can move the pointer between a scenario's press and release.
                MotionEvent motion;
                motion.pos = { point->x, point->y };
                motion.absolutePos = motion.pos;
                onMotion (motion);
                MouseEvent release;
                release.button = DGL::kMouseButtonLeft;
                release.pos = motion.pos;
                release.absolutePos = motion.pos;
                onMouse (release);
            }
            if (quietFrames < kSettledFrames)
            {
                if (ImGui::GetCurrentContext()->InputEventsQueue.Size > 0
                    || (owner.view != nullptr && ! owner.view->inputSettledForScenario()))
                    quietFrames = 0;
                else if (++quietFrames == kSettledFrames)
                {
                    --panelsWithUnseenScenarioInput;
                    holdNextFrame = true;
                }
            }
        }

        void onDisplay() override
        {
            if (owner.framesPausedForScenario) return;
            if (const int interval = scenarioFrameIntervalMs(); interval > 0)
            {
                // Held on the message thread, as a software renderer's frame is: no
                // timer can fire until the frame is out.
                std::this_thread::sleep_until (lastFrameAt + std::chrono::milliseconds (interval));
                lastFrameAt = std::chrono::steady_clock::now();
            }
            drawnStamp = owner.view != nullptr ? owner.view->liveStamp() : std::nullopt;
            drawnWidth = getWidth();
            drawnHeight = getHeight();
            drawnScale = getWindow().getScaleFactor();
            {
                struct Drawing
                {
                    Drawing() { ++panelFramesDrawing; }
                    ~Drawing() { --panelFramesDrawing; }
                } drawing;
                DGL::ImGuiTopLevelWidget::onDisplay();
            }
            drawnAt = std::chrono::steady_clock::now();
            framesOwed = std::max (0, framesOwed - 1);
            // A frame nothing here asked for is the system's: an expose, a resize.
            const char* const asked = std::exchange (askedFor, nullptr);
            frameCause = asked != nullptr ? asked : "a repaint the system asked for";
            ++owner.completedFrames;
            ++framesShown;
            owner.captureFrameIfAsked (static_cast<int> (getWidth()),
                                       static_cast<int> (getHeight()));
        }

    private:
        // Asked between frames, so Dear ImGui's state is what the last frame left.
        bool frameWanted (std::chrono::steady_clock::time_point now)
        {
            const auto wanted = [this] (const char* why) { wantedFor = why; return true; };
            if (harnessDrawsContinuously()) return wanted ("the harness draws every frame");
            if (! owner.capturePath.empty()) return wanted ("a capture");
            if (owner.view == nullptr || imguiContext == nullptr) return wanted ("no view yet");
            if (framesOwed > 0 || now - lastInputAt < kInputHold) return wanted (lastActivity);
            if (getWidth() != drawnWidth || getHeight() != drawnHeight
                || std::abs (getWindow().getScaleFactor() - drawnScale) > 1.0e-6)
                return wanted ("a new size or scale");
            const auto& g = *imguiContext;
            if (g.InputEventsQueue.Size > 0) return wanted ("queued input");
            if (g.ActiveId != 0) return wanted ("an active control");
            if (g.IO.WantTextInput) return wanted ("a text field");
            if (g.OpenPopupStack.Size > 0) return wanted ("an open list");
            if (g.DragDropActive || owner.drag.active != 0) return wanted ("a drag");
            for (const bool down : g.IO.MouseDown)
                if (down) return wanted ("a mouse button held");
            // Not the modifier entries: the framework sets those from every pointer
            // event, and a pointer that leaves the window takes their release with it.
            for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_ReservedForModCtrl; ++key)
                if (g.IO.KeysData[key - ImGuiKey_NamedKey_BEGIN].Down) return wanted ("a key held");
            if (owner.fontFallback.waiting())
                return wanted ("a fallback font loading");
            const auto stamp = owner.view->liveStamp();
            if (! stamp) return wanted ("a view with no stamp");
            return stamp != drawnStamp && wanted ("what the view shows changing");
        }

        bool press (const KeyChord& chord)
        {
            KeyboardEvent key;
            key.key = frameworkKeyCode (chord.key);
            if (key.key == 0)
                return false;
            key.mod = frameworkModifiers (chord.shift, chord.ctrl, chord.super, chord.alt);
            switch (chord.key)
            {
                case ImGuiKey_LeftCtrl: case ImGuiKey_RightCtrl: key.mod |= DGL::kModifierControl; break;
                case ImGuiKey_LeftAlt: case ImGuiKey_RightAlt: key.mod |= DGL::kModifierAlt; break;
                case ImGuiKey_LeftShift: case ImGuiKey_RightShift: key.mod |= DGL::kModifierShift; break;
                case ImGuiKey_LeftSuper: case ImGuiKey_RightSuper: key.mod |= DGL::kModifierSuper; break;
                default: break;
            }
            key.press = true;
            onKeyboard (key);
            keyReleases.push_back (key.key);
            return true;
        }

        void type (std::uint32_t character)
        {
            CharacterInputEvent input;
            input.character = character;
            encodeUtf8 (character, input.string);
            onCharacterInput (input);
        }

        Impl& owner;
        bool keyboardFocused = false;
        std::optional<ImVec2> releasePending;
        int quietFrames = kSettledFrames;
        bool holdNextFrame = false;
        std::chrono::steady_clock::time_point unseenSince;
        std::vector<unsigned int> keyReleases;
        DGL::MouseButton heldButton = DGL::kMouseButtonLeft;
        ImGuiMouseCursor appliedCursor = ImGuiMouseCursor_Arrow;
        std::chrono::steady_clock::time_point lastFrameAt;
        ImGuiContext* imguiContext = nullptr;
        std::chrono::steady_clock::time_point lastInputAt, lastWantedAt, drawnAt;
        std::optional<std::uint64_t> drawnStamp;
        unsigned int drawnWidth = 0, drawnHeight = 0;
        double drawnScale = 0.0;
        int framesOwed = 0;
        bool afterGap = false, drawingAfterGap = false;
        const char* lastActivity = "";
        const char* wantedFor = "";
        const char* askedFor = nullptr;   // why the frame now on its way was asked for

    public:
        // Why the newest frame was drawn: the input that reached the panel, what
        // else called for it, or the system. Empty before the first.
        const char* frameCause = "";
        // Dear ImGui's frame time on the last frame drawn after a gap.
        std::optional<float> clockAfterGap;
        // Frames this widget has drawn since it opened.
        std::uint64_t framesShown = 0;
    };

    Impl (std::string className, std::string logTagToUse, std::string displayName)
        : logTag (logTagToUse),
          host ({ std::move (className), logTagToUse, std::move (displayName) },
                firstFrameMarkerPath (logTagToUse))
    {
    }

    ~Impl() override { stopTimer(); }

    void startGeometryPolling() { startTimer (paced ? kActivePumpMs : kIdlePumpMs); }
    void stopGeometryPolling() { stopTimer(); }

    void pace (bool active)
    {
        if (active == paced)
            return;
        paced = active;
        const int interval = active ? kActivePumpMs : kIdlePumpMs;
        host.setPumpIntervalMs (interval);
        if (isTimerRunning())
            startTimer (interval);
    }

    void timerCallback() override
    {
        if (! callbacks.geometry || ! host.isOpen())
            return;
        const auto wanted = callbacks.geometry();
        if (wanted != lastGeometry)
        {
            lastGeometry = wanted;
            // A move resizes nothing, and a scale change on a surface that keeps its
            // size reports no resize either. Before the call, which can close the window.
            if (panelWidget != nullptr)
                panelWidget->noteActivity ("a geometry change");
            host.setGeometry ({ wanted.x, wanted.y, wanted.width, wanted.height,
                                wanted.scaleFactor });
        }
    }

    // Frame 30 rather than the first: the meter ballistics and any smoother have
    // settled by then, so two runs of the same panel produce the same picture.
    void captureFrameIfAsked (int width, int height)
    {
        if (capturePath.empty() || width < 1 || height < 1)
            return;
        if (++framesDrawn < 30)
            return;

        std::vector<unsigned char> rgb (static_cast<std::size_t> (width)
                                        * static_cast<std::size_t> (height) * 3u);

        // The rows below are read back tightly packed. GL pads them to four bytes by
        // default, which skews every row whose width is not a multiple of four - the
        // plates happen to be today, but a scale factor decides that, not the layout.
        GLint previousAlignment = 4;
        glGetIntegerv (GL_PACK_ALIGNMENT, &previousAlignment);
        glPixelStorei (GL_PACK_ALIGNMENT, 1);
        glReadPixels (0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
        glPixelStorei (GL_PACK_ALIGNMENT, previousAlignment);

        // GL reads bottom-up.
        std::vector<unsigned char> flipped (rgb.size());
        const std::size_t stride = static_cast<std::size_t> (width) * 3;
        for (int row = 0; row < height; ++row)
            std::copy (rgb.begin() + static_cast<std::ptrdiff_t> (stride * static_cast<std::size_t> (height - 1 - row)),
                       rgb.begin() + static_cast<std::ptrdiff_t> (stride * static_cast<std::size_t> (height - row)),
                       flipped.begin() + static_cast<std::ptrdiff_t> (stride * static_cast<std::size_t> (row)));

        if (! writePpm (capturePath, width, height, flipped))
            host.log ("could not write the capture to %s", capturePath.c_str());
        capturePath.clear();
    }

    void buildFonts (float scale)
    {
       #ifndef DGL_NO_SHARED_RESOURCES
        auto& io = ImGui::GetIO();
        io.Fonts->Clear();
        fonts = buildPanelFonts (*io.Fonts, daf_resources::dejavusans_ttf,
                                 static_cast<int> (daf_resources::dejavusans_ttf_size), scale,
                                 view != nullptr && view->marksMissingGlyphs());
        fontFallback.reserve (*io.Fonts);
        // Reserved before the pack and filled in after, so a knob costs three quads
        // rather than the thousand vertices a drawn dome does.
        knobAtlas.reserve (*io.Fonts, static_cast<int> (128.0f * std::max (1.0f, scale)));
        if (view != nullptr)
            view->reserveAtlasImages (*io.Fonts);
        io.FontDefault = fonts.band;
        io.Fonts->Build();
        if (fonts.textEntry != nullptr) fontFallback.rasterise (*io.Fonts, *fonts.textEntry);
        knobAtlas.rasterise (*io.Fonts);
        if (view != nullptr)
            view->rasteriseAtlasImages (*io.Fonts);
       #endif
        fontScale = scale;
    }

    void draw (float width, float height, float scale)
    {
        if (view == nullptr)
            return;

        ImGui::SetNextWindowPos (ImVec2 (0.0f, 0.0f));
        ImGui::SetNextWindowSize (ImVec2 (width, height));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (0.0f, 0.0f));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor (ImGuiCol_WindowBg, ImVec4 (0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::Begin ("##panel", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                      | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
                      | ImGuiWindowFlags_NoScrollWithMouse
                      | ImGuiWindowFlags_NoBringToFrontOnFocus
                      | ImGuiWindowFlags_NoSavedSettings);

        dw::Context ctx;
        ctx.dl = ImGui::GetWindowDrawList();
        ctx.theme = &consolePalette().widgets;
        ctx.fonts = &fonts;
        ctx.knobAtlas = &knobAtlas;
        ctx.drag = &drag;
        ctx.scale = scale;

        // The child is the plate: the frame runs along its own edges and the body
        // sits inside the margin. The shadow the JUCE backdrop cast has nowhere to
        // fall here - it would land on the plate itself - so the frame carries the
        // panel on its own. An inline view takes no plate and gets the whole child.
        const float margin = view->wantsPlate() ? kPlateMargin * scale : 0.0f;
        if (view->wantsPlate())
        {
            const float rounding = kPlateRounding * scale;
            ctx.dl->AddRectFilled (ImVec2 (0.0f, 0.0f), ImVec2 (width, height),
                                   rgba (kPlateFill), rounding);
            ctx.dl->AddRect (ImVec2 (0.5f, 0.5f), ImVec2 (width - 0.5f, height - 0.5f),
                             rgba (kPlateBorder), rounding, 0, scale);
        }

        const ImVec2 bodyTl (margin, margin);
        const ImVec2 body (std::max (1.0f, width - margin * 2.0f),
                           std::max (1.0f, height - margin * 2.0f));

        const ScopedPanelFontFallback fallback (view->marksMissingGlyphs() ? &fontFallback : nullptr);
        view->draw (ctx, bodyTl, body);
        dw::drawDragBubble (ctx);

        // A field or menu that opens asks for the keyboard, which a child the click
        // did not focus (Windows) might then get and keep for itself. Asked even when
        // the child believes it has it: on X11 a focus event the pointer caused marks
        // it focused while the keys still go to the parent.
        const bool capturing = view->capturesKeyboard();
        if (capturing && ! wasCapturing && panelWidget != nullptr)
            if (auto* window = host.window())
            {
                window->focus();
                ++keyboardRequests;
            }
        wasCapturing = capturing;

        // A click outside the panel lands on the host's dim overlay rather than
        // here, because the child is exactly the plate.
        if (view->takeDismissRequest())
            requestDismiss();
        else if (view->escapeDismisses() && dw::shortcutsAvailable (ctx)
                 && ImGui::IsKeyPressed (ImGuiKey_Escape, false))
            requestDismiss();

        forwardShortcuts (ctx);

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar (2);
    }

    void forwardShortcuts (const dw::Context& ctx)
    {
        if (! callbacks.shortcut || ! dw::shortcutsAvailable (ctx)
            || (view != nullptr && view->capturesKeyboard()))
            return;

        const auto& io = ImGui::GetIO();
        const auto forward = [&] (ImGuiKey key)
        {
            if (! ImGui::IsKeyPressed (key, false))
                return;
            // Dear ImGui reports Cmd as Ctrl on macOS (ConfigMacOSXBehaviors), and
            // the chord keeps the platform's own names.
           #if defined (__APPLE__)
            const auto shortcut = shellShortcutFor ({ key, io.KeySuper, io.KeyCtrl, io.KeyShift, io.KeyAlt });
           #else
            const auto shortcut = shellShortcutFor ({ key, io.KeyCtrl, io.KeySuper, io.KeyShift, io.KeyAlt });
           #endif
            if (shortcut && (view == nullptr || ! view->claimsShortcut (*shortcut)))
                callbacks.shortcut (*shortcut);
        };
        for (const auto& binding : shortcutBindings())
            forward (binding.key);
        for (const auto key : kSessionKeys)
            forward (key);

        if (view == nullptr || view->wantsPlate() || shellKeys != PanelShellKeys::all
            || ! io.KeyCtrl || io.KeySuper || io.KeyShift || io.KeyAlt)
            return;
        static constexpr std::array<ShortcutBinding, 4> stages { {
            { ImGuiKey_1, ShellShortcut::recordingStage },
            { ImGuiKey_2, ShellShortcut::mixingStage },
            { ImGuiKey_3, ShellShortcut::masteringStage },
            { ImGuiKey_4, ShellShortcut::auxStage }
        } };
        for (const auto& binding : stages)
            if (ImGui::IsKeyPressed (binding.key, false) && ! view->claimsShortcut (binding.shortcut))
                callbacks.shortcut (binding.shortcut);
    }

    void requestDismiss()
    {
        if (auto callback = callbacks.dismissed)
            callback();
    }

    // Declared last so the host - whose teardown reaches back through the callbacks
    // below - is destroyed before the state those callbacks touch.
    Callbacks callbacks;
    std::unique_ptr<DuskPanelView> view;
    PanelWidget* panelWidget = nullptr;
    bool wasCapturing = false;
    int keyboardRequests = 0;
    dw::Fonts fonts;
    float fontScale = 0.0f;
    PanelFontFallback fontFallback;
    dw::KnobAtlas knobAtlas;
    dw::DragState drag;
    Geometry lastGeometry;
    PanelShellKeys shellKeys = PanelShellKeys::all;
    std::string capturePath;
    int framesDrawn = 0;
    std::uint64_t completedFrames = 0;
    bool framesPausedForScenario = false;
    bool paced = true;
    std::string logTag;
    DuskImGuiHost host;
};

namespace
{
// Never freed, so a window destroyed during static teardown still finds it.
std::vector<DuskPanelWindow*>& liveWindows()
{
    static auto* windows = new std::vector<DuskPanelWindow*>();
    return *windows;
}
} // namespace

DuskPanelWindow::DuskPanelWindow (std::string className, std::string logTag,
                                  std::string displayName)
    : impl (new Impl (std::move (className), std::move (logTag), std::move (displayName)))
{
    liveWindows().push_back (this);
    DuskImGuiHost::Callbacks callbacks;
    callbacks.createWidget = [this] (DGL::Window& window) -> std::unique_ptr<DGL::TopLevelWidget>
    {
        // X11 delivers a held key's auto-repeat as release/press pairs, and Dear ImGui
        // applies one transition per key per frame, so on slow frames the pairs queue
        // up and a released key keeps reading down: a held virtual keyboard key sounds
        // its note again after release. Dear ImGui repeats held keys itself. The
        // notepad keeps the system repeat, which is what repeats a held letter there.
        window.setIgnoringKeyRepeat (true);
        auto widget = std::unique_ptr<Impl::PanelWidget> (new Impl::PanelWidget (window, *impl));
        impl->panelWidget = widget.get();
        impl->buildFonts (static_cast<float> (window.getScaleFactor()));
        return std::unique_ptr<DGL::TopLevelWidget> (widget.release());
    };
    callbacks.checkGraphics = [] (const char*, const char*) { return std::string(); };
    callbacks.widgetReleased = [this]
    {
        impl->panelWidget = nullptr;
        impl->wasCapturing = false;
        // The fonts and the baked dome live in the atlas the widget owned.
        impl->fonts = {};
        impl->fontScale = 0.0f;
        impl->fontFallback = PanelFontFallback {};
        impl->knobAtlas = {};
        impl->drag = {};
    };
    callbacks.closed = [this]
    {
        impl->stopGeometryPolling();
        impl->view.reset();
        if (auto callback = impl->callbacks.closed)
            callback();
    };
    impl->host.setCallbacks (std::move (callbacks));
}

DuskPanelWindow::~DuskPanelWindow()
{
    auto& windows = liveWindows();
    windows.erase (std::remove (windows.begin(), windows.end(), this), windows.end());
}

void DuskPanelWindow::setCallbacks (Callbacks callbacks)
{
    impl->callbacks = std::move (callbacks);
}

void DuskPanelWindow::setShellKeys (PanelShellKeys keys) noexcept
{
    impl->shellKeys = keys;
}

void DuskPanelWindow::setTakesKeyboardOnOpen (bool takes) noexcept
{
    impl->host.setTakesKeyboardOnOpen (takes);
}

PanelShellKeys DuskPanelWindow::openShellKeys() noexcept
{
    auto strictest = PanelShellKeys::all;
    for (const auto* window : liveWindows())
        if (window->isOpen())
            strictest = std::max (strictest, window->impl->shellKeys);
    return strictest;
}

bool DuskPanelWindow::hasOpenModal() noexcept
{
    for (const auto* window : liveWindows())
        if (window->isOpen() && window->impl->view != nullptr && window->impl->view->wantsPlate())
            return true;
    return false;
}

void DuskPanelWindow::setView (std::unique_ptr<DuskPanelView> view)
{
    impl->view = std::move (view);
}

DuskPanelWindow::PlateSize DuskPanelWindow::plateSize() const
{
    if (impl->view == nullptr)
        return {};
    const auto body = impl->view->preferredSize();
    const auto frame = impl->view->wantsPlate() ? static_cast<int> (kPlateMargin) * 2 : 0;
    return { static_cast<int> (body.x) + frame, static_cast<int> (body.y) + frame };
}

float DuskPanelWindow::dimAlpha() const
{
    return impl->view != nullptr ? impl->view->dimAlpha() : 0.55f;
}

void DuskPanelWindow::captureNextFrameTo (std::string path)
{
    impl->capturePath = std::move (path);
    impl->framesDrawn = 0;
}

bool DuskPanelWindow::open (std::uintptr_t nativeParent, Geometry geometry)
{
    impl->lastGeometry = geometry;
    impl->framesDrawn = 0;
    if (! impl->host.open (nativeParent, { geometry.x, geometry.y, geometry.width,
                                           geometry.height, geometry.scaleFactor }))
        return false;

    impl->startGeometryPolling();
    return true;
}

const std::string& DuskPanelWindow::lastOpenFailure() const noexcept
{
    return impl->host.lastOpenFailure();
}

void DuskPanelWindow::setGeometry (Geometry geometry)
{
    impl->host.setGeometry ({ geometry.x, geometry.y, geometry.width, geometry.height,
                              geometry.scaleFactor });
}

void DuskPanelWindow::close()
{
    impl->host.close();
}

bool DuskPanelWindow::isOpen() const noexcept
{
    return impl->host.isOpen();
}

bool DuskPanelWindow::offerShellKey (const std::string& description, std::uint32_t character)
{
    if (! isOpen() || impl->view == nullptr || impl->panelWidget == nullptr || ! impl->view->capturesKeyboard())
        return false;
   #if ! defined (__APPLE__)
    if (impl->panelWidget->hasKeyboard()) return true;
   #endif
    impl->panelWidget->replayShellKey (description, character);
    return true;
}

bool DuskPanelWindow::takeKeptKey (const std::string& description, std::uint32_t character)
{
    if (! isOpen() || impl->view == nullptr || impl->panelWidget == nullptr)
        return false;
    const auto chord = parseKeyDescription (description);
    if (! chord || ! impl->view->keepsKey (*chord))
        return false;
    if (chord->command() || ! impl->panelWidget->hasKeyboard())
        impl->panelWidget->replayShellKey (description, character);
    return true;
}

// Input queued now lands in the next frame, so two drawn frames put it in the third.
bool DuskPanelWindow::takesPointerForScenario() const noexcept
{
    return isOpen() && impl->panelWidget != nullptr && impl->panelWidget->framesShown >= 2;
}
bool DuskPanelWindow::clickControlForScenario (const std::string& control)
{
    ImVec2 point;
    if (! isOpen() || impl->view == nullptr || impl->panelWidget == nullptr
        || ! impl->view->controlPointForScenario (control, point)) return false;
    if (auto* window = impl->host.window()) window->focus();
    impl->panelWidget->clickForScenario (point);
    return true;
}
std::optional<ImVec2> DuskPanelWindow::controlAtForScenario (const std::string& control, float position) const
{
    ImVec2 point;
    if (! isOpen() || impl->view == nullptr || ! impl->view->controlPointForScenario (control, point, position))
        return std::nullopt;
    return point;
}
bool DuskPanelWindow::doubleClickControlForScenario (const std::string& control)
{
    ImVec2 point;
    if (! isOpen() || impl->view == nullptr || impl->panelWidget == nullptr
        || ! impl->view->controlPointForScenario (control, point)) return false;
    if (auto* window = impl->host.window()) window->focus();
    impl->panelWidget->doubleClickForScenario (point);
    return true;
}
bool DuskPanelWindow::inputForScenario (const std::string& input)
{
    if (! isOpen() || impl->panelWidget == nullptr) return false;
    return impl->panelWidget->inputForScenario (input);
}
bool DuskPanelWindow::typeForScenario (const std::string& text)
{
    if (! isOpen() || impl->panelWidget == nullptr) return false;
    impl->panelWidget->typeForScenario (text);
    return true;
}
bool DuskPanelWindow::scrollForScenario (float wheel)
{
    if (! isOpen() || impl->panelWidget == nullptr) return false;
    impl->panelWidget->scrollForScenario (wheel);
    return true;
}
bool DuskPanelWindow::pointerControlForScenario (const std::string& control, float position, bool pressed,
                                                 int modifiers)
{
    ImVec2 point;
    if (! isOpen() || impl->view == nullptr || impl->panelWidget == nullptr
        || ! impl->view->controlPointForScenario (control, point, position)) return false;
    impl->panelWidget->pointerForScenario (point, pressed, modifiers);
    return true;
}
bool DuskPanelWindow::keyboardFocusForScenario (bool focused)
{
    if (! isOpen() || impl->panelWidget == nullptr) return false;
    impl->panelWidget->focusForScenario (focused);
    return true;
}
bool DuskPanelWindow::focusForScenario()
{
    if (! isOpen() || impl->panelWidget == nullptr || impl->host.window() == nullptr) return false;
    impl->host.window()->focus();
    impl->panelWidget->focusForScenario (true);
    impl->panelWidget->noteScenarioInput();
    return true;
}
bool DuskPanelWindow::hasKeyboardForScenario() const noexcept
{
    return isOpen() && impl->panelWidget != nullptr && impl->panelWidget->hasKeyboard();
}
std::uint64_t DuskPanelWindow::completedFramesForScenario() const noexcept
{
    return impl->completedFrames;
}
std::string DuskPanelWindow::frameCauseForScenario() const
{
    return isOpen() && impl->panelWidget != nullptr ? std::string (impl->panelWidget->frameCause) : std::string();
}
void DuskPanelWindow::pauseFramesForScenario (bool paused) noexcept
{
    impl->framesPausedForScenario = paused;
}
bool DuskPanelWindow::capturesKeyboardForScenario() const noexcept
{
    return isOpen() && impl->view != nullptr && impl->view->capturesKeyboard();
}
int DuskPanelWindow::keyboardRequestsForScenario() const noexcept
{
    return impl->keyboardRequests;
}
bool DuskPanelWindow::ignoresKeyRepeatForScenario() const noexcept
{
    const auto* window = impl->host.window();
    return window != nullptr && window->isIgnoringKeyRepeat();
}
std::uintptr_t DuskPanelWindow::nativeWindowHandleForScenario() const noexcept
{
    const auto* window = impl->host.window();
    return window != nullptr ? window->getNativeWindowHandle() : 0;
}
std::optional<GlyphTally> DuskPanelWindow::nameGlyphsForScenario() const
{
    if (impl->view == nullptr) return std::nullopt;
    return impl->view->nameGlyphsForScenario();
}
void DuskPanelWindow::expectInputForScenario()
{
    if (isOpen() && impl->panelWidget != nullptr)
    {
        impl->panelWidget->noteScenarioInput();
        impl->panelWidget->noteActivity ("scenario input");
    }
}
void DuskPanelWindow::wake()
{
    if (isOpen() && impl->panelWidget != nullptr)
        impl->panelWidget->noteActivity ("a wake from its owner");
}
void DuskPanelWindow::closeLists()
{
    if (! isOpen() || impl->view == nullptr) return;
    impl->view->closeLists();
    wake();
}
void DuskPanelWindow::sessionReplaced()
{
    if (! isOpen() || impl->view == nullptr) return;
    impl->view->sessionReplaced();
    wake();
}
bool DuskPanelWindow::inputSeenForScenario() noexcept
{
    return panelsWithUnseenScenarioInput == 0;
}
bool DuskPanelWindow::drawingForScenario() noexcept
{
    return panelFramesDrawing > 0;
}
std::optional<float> DuskPanelWindow::takeClockAfterRestForScenario()
{
    if (! isOpen() || impl->panelWidget == nullptr)
        return std::nullopt;
    return std::exchange (impl->panelWidget->clockAfterGap, std::nullopt);
}
void DuskPanelWindow::framesOnDemandForScenario (bool onDemand, bool floor) noexcept
{
    scenarioFramesOnDemand = onDemand;
    scenarioFrameFloor = floor;
}
DuskPanelWindow* DuskPanelWindow::openForScenario (const std::string& logTag) noexcept
{
    for (auto* window : liveWindows())
        if (window->isOpen() && window->impl->logTag == logTag)
            return window;
    return nullptr;
}
} // namespace duskstudio::imgui
