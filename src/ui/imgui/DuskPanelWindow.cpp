#include "DuskPanelWindow.h"
#include "DuskImGuiHost.h"
#include "DuskTheme.h"
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
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
constexpr float kScenarioFrameSeconds = 1.0f / 60.0f;
int panelsWithUnseenScenarioInput = 0;

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
};

const std::array<NamedKey, 22>& namedKeys()
{
    static const std::array<NamedKey, 22> keys { {
        { "spacebar", ImGuiKey_Space, DGL::kKeySpace },
        { "space", ImGuiKey_Space, DGL::kKeySpace },
        { "return", ImGuiKey_Enter, DGL::kKeyEnter },
        { "enter", ImGuiKey_Enter, DGL::kKeyEnter },
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
    const auto name = parts.back();
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
    if (chord.ctrl || chord.super || chord.alt)
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
            : DGL::ImGuiTopLevelWidget (window, 13.0f), owner (ownerRef) {}

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
            releasePending = true;
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
            if (input == "scroll-down")
            {
                scrollForScenario (-20.0);
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
        void onFocusChanged (const FocusEvent& event) override
        {
            keyboardFocused = event.focus;
            DGL::ImGuiTopLevelWidget::onFocusChanged (event);
        }

        // A key the widget reports as unused is handed to the host window, where the
        // shell's own bindings claim it - so a note letter typed at the panel would
        // also toggle mute behind it. The panel names the shortcuts it wants the shell
        // to keep through forwardShortcuts; every other key stays here.
        bool onKeyboard (const DGL::Widget::KeyboardEvent& event) override
        {
            DGL::ImGuiTopLevelWidget::onKeyboard (event);
            return true;
        }

        // Dear ImGui times a double-click on the wall clock, and the scenario runner
        // waits for each input to be drawn before it sends the next, so a slow
        // renderer would age two clicks apart. While scenario input is waiting to be
        // drawn the clock moves no faster than a 60 Hz frame; the time between one
        // input being drawn and the next being sent still counts in full.
        void onImGuiPrepareFrame() override
        {
            if (quietFrames >= kSettledFrames)
                return;
            const auto now = std::chrono::steady_clock::now();
            const float waited = std::chrono::duration<float> (now - unseenSince).count();
            unseenSince = now;
            auto& io = ImGui::GetIO();
            io.DeltaTime = std::max (io.DeltaTime - std::max (0.0f, waited - kScenarioFrameSeconds),
                                     std::min (io.DeltaTime, kScenarioFrameSeconds));
        }

        void onImGuiDisplay() override
        {
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
            if (releasePending)
            {
                releasePending = false;
                MouseEvent release;
                release.button = DGL::kMouseButtonLeft;
                onMouse (release);
            }
            if (quietFrames < kSettledFrames)
            {
                if (ImGui::GetCurrentContext()->InputEventsQueue.Size > 0)
                    quietFrames = 0;
                else if (++quietFrames == kSettledFrames)
                    --panelsWithUnseenScenarioInput;
            }
        }

        void onDisplay() override
        {
            if (const int interval = scenarioFrameIntervalMs(); interval > 0)
            {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastFrameAt < std::chrono::milliseconds (interval))
                    return;
                lastFrameAt = now;
            }
            DGL::ImGuiTopLevelWidget::onDisplay();
            owner.captureFrameIfAsked (static_cast<int> (getWidth()),
                                       static_cast<int> (getHeight()));
        }

    private:
        bool press (const KeyChord& chord)
        {
            KeyboardEvent key;
            key.key = frameworkKeyCode (chord.key);
            if (key.key == 0)
                return false;
            key.mod = frameworkModifiers (chord.shift, chord.ctrl, chord.super, chord.alt);
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
        bool releasePending = false;
        int quietFrames = kSettledFrames;
        std::chrono::steady_clock::time_point unseenSince;
        std::vector<unsigned int> keyReleases;
        DGL::MouseButton heldButton = DGL::kMouseButtonLeft;
        ImGuiMouseCursor appliedCursor = ImGuiMouseCursor_Arrow;
        std::chrono::steady_clock::time_point lastFrameAt;
    };

    Impl (std::string className, std::string logTag, std::string displayName)
        : host ({ std::move (className), logTag, std::move (displayName) },
                firstFrameMarkerPath (logTag))
    {
    }

    ~Impl() override { stopTimer(); }

    void startGeometryPolling() { startTimer (16); }
    void stopGeometryPolling() { stopTimer(); }

    void timerCallback() override
    {
        if (! callbacks.geometry || ! host.isOpen())
            return;
        const auto wanted = callbacks.geometry();
        if (wanted != lastGeometry)
        {
            lastGeometry = wanted;
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
        fonts = dw::buildFonts (*io.Fonts, daf_resources::dejavusans_ttf,
                                static_cast<int> (daf_resources::dejavusans_ttf_size), scale);
        // Reserved before the pack and filled in after, so a knob costs three quads
        // rather than the thousand vertices a drawn dome does.
        knobAtlas.reserve (*io.Fonts, static_cast<int> (128.0f * std::max (1.0f, scale)));
        if (view != nullptr)
            view->reserveAtlasImages (*io.Fonts);
        io.FontDefault = fonts.band;
        io.Fonts->Build();
        knobAtlas.rasterise (*io.Fonts);
        if (view != nullptr)
            view->rasteriseAtlasImages (*io.Fonts);
       #endif
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
        if (! callbacks.shortcut || ! dw::shortcutsAvailable (ctx))
            return;

        const auto& io = ImGui::GetIO();
        for (const auto& binding : shortcutBindings())
        {
            if (! ImGui::IsKeyPressed (binding.key, false))
                continue;
            const auto shortcut = shellShortcutFor ({ binding.key, io.KeyCtrl, io.KeySuper, io.KeyShift, io.KeyAlt });
            if (! shortcut || (view != nullptr && view->claimsShortcut (*shortcut)))
                continue;
            callbacks.shortcut (*shortcut);
        }
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
    dw::KnobAtlas knobAtlas;
    dw::DragState drag;
    Geometry lastGeometry;
    std::string capturePath;
    int framesDrawn = 0;
    DuskImGuiHost host;
};

DuskPanelWindow::DuskPanelWindow (std::string className, std::string logTag,
                                  std::string displayName)
    : impl (new Impl (std::move (className), std::move (logTag), std::move (displayName)))
{
    DuskImGuiHost::Callbacks callbacks;
    callbacks.createWidget = [this] (DGL::Window& window) -> std::unique_ptr<DGL::TopLevelWidget>
    {
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

DuskPanelWindow::~DuskPanelWindow() = default;

void DuskPanelWindow::setCallbacks (Callbacks callbacks)
{
    impl->callbacks = std::move (callbacks);
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
    if (! impl->panelWidget->hasKeyboard())
        impl->panelWidget->replayShellKey (description, character);
    return true;
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
int DuskPanelWindow::keyboardRequestsForScenario() const noexcept
{
    return impl->keyboardRequests;
}
void DuskPanelWindow::expectInputForScenario()
{
    if (isOpen() && impl->panelWidget != nullptr)
        impl->panelWidget->noteScenarioInput();
}
bool DuskPanelWindow::inputSeenForScenario() noexcept
{
    return panelsWithUnseenScenarioInput == 0;
}
} // namespace duskstudio::imgui
