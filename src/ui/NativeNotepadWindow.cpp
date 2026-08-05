#include "NativeNotepadWindow.h"
#include "NotepadDocument.h"
#include "NotepadEditor.h"
#include "NotepadChords.h"
#include "NotepadEditorCore.h"
#include "NotepadTheme.h"
#include "../foundation/MessageThread.h"

#include <Application.hpp>
#include <DearImGui.hpp>
#include <DearImGui/imgui_internal.h>
#ifndef DGL_NO_SHARED_RESOURCES
# include "src/Resources.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <optional>
#include <utility>

namespace duskstudio
{
namespace
{
// The measure both views share: wide enough for a long lyric line, narrow
// enough that the eye does not lose the line it is reading.
constexpr float kContentWidth = 760.0f;
constexpr float kHeaderHeight = 62.0f;
constexpr float kRibbonHeight = 62.0f;

// DejaVu Sans covers these ranges in the embedded fallback. Platform fonts can
// contribute additional glyphs, but the editor never intentionally truncates
// the atlas to Latin-1 as the first native prototype did.
static const ImWchar kDocumentGlyphRanges[] = {
    0x0020, 0x024f, // Latin + IPA
    0x0370, 0x052f, // Greek + Cyrillic
    0x0590, 0x06ff, // Hebrew + Arabic
    0x0900, 0x097f, // Devanagari
    0x2000, 0x206f, // punctuation
    0x20a0, 0x20cf, // currency
    0x2100, 0x27bf, // symbols, arrows, shapes
    0x3000, 0x30ff, // CJK punctuation + kana when the font provides it
    0xff00, 0xffef, // full-width forms
    0
};

ImFont* addPlatformFont (ImFontAtlas& atlas,
                         std::initializer_list<const char*> candidates,
                         float size)
{
    for (const auto* const path : candidates)
    {
        std::error_code error;
        if (path != nullptr && std::filesystem::is_regular_file (path, error))
            if (auto* const font = atlas.AddFontFromFileTTF (path, size, nullptr,
                                                            kDocumentGlyphRanges))
                return font;
    }
    return nullptr;
}

std::string clockLabel()
{
    const auto now = std::time (nullptr);
    std::tm local {};
   #if defined (_WIN32)
    localtime_s (&local, &now);
   #else
    localtime_r (&now, &local);
   #endif
    char buffer[6] {};
    std::strftime (buffer, sizeof (buffer), "%H:%M", &local);
    return buffer;
}

ImVec4 colour (unsigned int hex)
{
    return ImVec4 (((hex >> 24) & 0xff) / 255.0f,
                   ((hex >> 16) & 0xff) / 255.0f,
                   ((hex >> 8) & 0xff) / 255.0f,
                   (hex & 0xff) / 255.0f);
}

} // namespace

struct NativeNotepadWindow::Impl final : private dusk::Timer
{
    class EmbeddedApplication final : public DGL::Application
    {
    public:
        EmbeddedApplication() : DGL::Application (false)
        {
            setClassName ("dusk-studio-notepad");
        }
    };

    class EditorWidget final : public DGL::ImGuiTopLevelWidget
    {
    public:
        EditorWidget (DGL::Window& window, Impl& ownerRef)
            : DGL::ImGuiTopLevelWidget (window, notepad::kTypeScale.lyric),
              owner (ownerRef) {}

    protected:
        void onImGuiDisplay() override
        {
            owner.draw (static_cast<float> (getWidth()), static_cast<float> (getHeight()));
        }

        bool onKeyboard (const DGL::Widget::KeyboardEvent& event) override
        {
            if (owner.handleChordEntryNavigation (event))
                return true;
            return DGL::ImGuiTopLevelWidget::onKeyboard (event);
        }

    private:
        Impl& owner;
    };

    Impl()
    {
        editor.onDocumentChanged = [this] { notifyTextChanged(); };
        editor.onLinkActivated = [this] (const std::string& target)
        {
            if (onLinkOpened)
                onLinkOpened (target);
        };
        editor.onUndoRequested = [this] { restoreHistory (false); };
        editor.onRedoRequested = [this] { restoreHistory (true); };
    }

    void buildFontAtlas (float size)
    {
        auto& io = ImGui::GetIO();
       #ifndef DGL_NO_SHARED_RESOURCES
        ImFontConfig embeddedConfig;
        embeddedConfig.FontDataOwnedByAtlas = false;
        embeddedConfig.OversampleH = 2;
        embeddedConfig.OversampleV = 2;
        embeddedConfig.PixelSnapH = false;

        io.Fonts->Clear();
        bodyFont = io.Fonts->AddFontFromMemoryTTF (
            (void*) dpf_resources::dejavusans_ttf,
            dpf_resources::dejavusans_ttf_size,
            size, &embeddedConfig, kDocumentGlyphRanges);

        boldFont = addPlatformFont (*io.Fonts, {
            "/usr/share/fonts/truetype/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
            "C:/Windows/Fonts/arialbd.ttf" }, size);
        italicFont = addPlatformFont (*io.Fonts, {
            "/usr/share/fonts/truetype/DejaVuSans-Oblique.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf",
            "/System/Library/Fonts/Supplemental/Arial Italic.ttf",
            "C:/Windows/Fonts/ariali.ttf" }, size);
        boldItalicFont = addPlatformFont (*io.Fonts, {
            "/usr/share/fonts/truetype/DejaVuSans-BoldOblique.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-BoldOblique.ttf",
            "/System/Library/Fonts/Supplemental/Arial Bold Italic.ttf",
            "C:/Windows/Fonts/arialbi.ttf" }, size);
        monoFont = addPlatformFont (*io.Fonts, {
            "/usr/share/fonts/truetype/DejaVuSansMono.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/System/Library/Fonts/Menlo.ttc",
            "C:/Windows/Fonts/consola.ttf" }, size);

        io.Fonts->Build();
        io.FontDefault = bodyFont;
       #else
        bodyFont = io.FontDefault != nullptr ? io.FontDefault
                                             : (io.Fonts->Fonts.empty() ? nullptr
                                                                       : io.Fonts->Fonts.front());
       #endif
        if (boldFont == nullptr) boldFont = bodyFont;
        if (italicFont == nullptr) italicFont = bodyFont;
        if (boldItalicFont == nullptr) boldItalicFont = boldFont;
        if (monoFont == nullptr) monoFont = bodyFont;
        editor.setFonts ({ bodyFont, boldFont, italicFont, boldItalicFont, monoFont });
    }

    ~Impl()
    {
        stopTimer();
        destroyEmbeddedWindow();
    }

    void setCallbacks (TextChangedCallback changed, ClosedCallback closed,
                       LinkOpenedCallback linkOpened)
    {
        onTextChanged = std::move (changed);
        onClosed = std::move (closed);
        onLinkOpened = std::move (linkOpened);
    }

    bool open (std::uintptr_t nativeParent, EmbeddedGeometry geometry,
               const std::string& markdown,
               bool sessionExists, bool unsavedChanges)
    {
        stopTimer();
        destroyEmbeddedWindow();
        if (nativeParent == 0 || geometry.width < 2 || geometry.height < 2)
            return false;

        document.setMarkdown (markdown);
        document.setSourceMode (false);
        editor.reset ({ 0, 0 });
        editor.requestFocus();
        closeRequested = false;
        closeWasPumped = false;
        hasSessionFile = sessionExists;
        documentDirty = unsavedChanges;
        saveFailed = false;
        savedMarkdown = unsavedChanges ? std::optional<std::string> {}
                                       : std::optional<std::string> { markdown };
        history.clear();

        window = std::make_unique<DGL::Window> (
            app, nativeParent, geometry.width, geometry.height,
            geometry.scaleFactor, false);
        // A display without a usable GL configuration leaves Pugl with an
        // unrealised view: no native handle, and a size hint that never took.
        if (window->getNativeWindowHandle() == 0
            || window->getWidth() < 2 || window->getHeight() < 2)
        {
            window.reset();
            return false;
        }

        // Dusk Studio owns both native windows, so it also owns the child's
        // in-parent placement. The explicit DPF embed API leaves normal plugin
        // windows under host control.
        // Native Wayland cannot embed a surface owned by DPF's display
        // connection into the legacy shell's surface. Refuse that backend
        // instead of silently falling back to the separate top-level window
        // that Pugl otherwise creates for an unsupported parent.
        if (! window->setEmbeddedOffset (geometry.x, geometry.y))
        {
            window.reset();
            return false;
        }
        {
            DGL::Window::ScopedGraphicsContext context (*window);
            editorWidget = std::make_unique<EditorWidget> (*window, *this);
            buildFontAtlas (static_cast<float> (notepad::kTypeScale.lyric
                                                * window->getScaleFactor()));
        }
        window->focus();
        startTimer (16);
        return true;
    }

    void close()
    {
        if (window == nullptr)
            return;
        closeRequested = true;
        closeWasPumped = false;
    }

    bool isOpen() const noexcept { return window != nullptr; }

    void setEmbeddedGeometry (EmbeddedGeometry geometry)
    {
        if (window == nullptr || geometry.width < 2 || geometry.height < 2)
            return;
        window->setSize (geometry.width, geometry.height);
        window->setEmbeddedOffset (geometry.x, geometry.y);
    }

    void markSaved()
    {
        savedAtLabel = "at " + clockLabel();
        documentDirty = false;
        saveFailed = false;
        hasSessionFile = true;
        savedMarkdown = document.markdown();
    }

    void markSaveFailed()
    {
        documentDirty = true;
        saveFailed = true;
    }

private:
    bool handleChordEntryNavigation (const DGL::Widget::KeyboardEvent& event)
    {
        if (! editor.chordEntryActive())
            return false;

        if (event.key == DGL::kKeyUp || event.key == DGL::kKeyDown)
        {
            if (event.press)
                editor.cycleChordCandidate (event.key == DGL::kKeyUp ? -1 : 1);
            return true;
        }
        if (event.key == DGL::kKeyTab)
        {
            if (event.press)
            {
                if ((event.mod & DGL::kModifierShift) != 0)
                    editor.cycleChordCandidate (-1);
                else
                    editor.acceptChordCandidate();
            }
            return true;
        }
        return false;
    }

    notepad::Snapshot currentSnapshot() const
    {
        return { document.markdown(), editor.selection(), false };
    }

    NotepadDocument::Selection selectionForCurrentMode (const notepad::Snapshot& state) const
    {
        return state.selectionIsSource ? document.documentSelection (state.selection)
                                       : state.selection;
    }

    void restoreHistory (bool redo)
    {
        notepad::Snapshot restored;
        if (! (redo ? history.redo (currentSnapshot(), restored)
                    : history.undo (currentSnapshot(), restored)))
            return;

        document.setMarkdown (restored.markdown);
        const auto selection = selectionForCurrentMode (restored);
        editor.reset (selection);
        editor.requestFocus();
        notifyTextChanged();
    }

    void timerCallback() override
    {
        app.idle();
        // Close requests come from the native host boundary. Wait until DPF
        // returns from the event pump before destroying its embedded widget
        // and native child.
        if (closeRequested && window != nullptr)
        {
            destroyEmbeddedWindow();
            closeWasPumped = true;
            return;
        }

        if (closeRequested && closeWasPumped)
        {
            // Give the platform event queue one tick to finish unmapping the
            // child before focus and input are restored to the DAW.
            app.idle();
            closeRequested = false;
            closeWasPumped = false;
            stopTimer();
            if (onClosed)
                onClosed();
        }
    }

    void destroyEmbeddedWindow()
    {
        if (window == nullptr)
            return;

        if (editorWidget != nullptr)
        {
            DGL::Window::ScopedGraphicsContext context (*window);
            editorWidget.reset();
        }
        window.reset();
        bodyFont = boldFont = italicFont = boldItalicFont = monoFont = nullptr;
        editor.setFonts ({});
    }

    void notifyTextChanged()
    {
        documentDirty = ! savedMarkdown.has_value()
                     || document.markdown() != *savedMarkdown;
        saveFailed = false;
        if (onTextChanged)
            onTextChanged (document.markdown(), documentDirty);
    }

    void applyInline (NotepadDocument::InlineStyle inlineStyle)
    {
        editor.applyInlineStyle (inlineStyle);
    }

    void applyBlock (NotepadDocument::BlockStyle style)
    {
        editor.applyBlockStyle (style);
    }

    bool inlineStyleActive (NotepadDocument::InlineStyle style) const
    {
        return editor.inlineStyleActive (style);
    }

    NotepadDocument::BlockStyle selectedBlockStyle() const
    {
        return editor.blockStyle();
    }

    bool toolbarButton (const char* label, const char* tooltip, bool active = false,
                        float width = 34.0f, ImFont* labelFont = nullptr)
    {
        if (active)
        {
            ImGui::PushStyleColor (ImGuiCol_Button, colour (notepad::kStagePalette.rule));
            ImGui::PushStyleColor (ImGuiCol_ButtonHovered,
                                   colour (notepad::kStagePalette.shell));
        }
        if (labelFont != nullptr)
            ImGui::PushFont (labelFont);
        const bool clicked = ImGui::Button (label, ImVec2 (width, 32.0f));
        if (labelFont != nullptr)
            ImGui::PopFont();
        if (active)
            ImGui::PopStyleColor (2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip ("%s", tooltip);
        return clicked;
    }

    void drawRibbon (float height)
    {
        ImGui::PushStyleVar (ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (22.0f, 13.0f));
        ImGui::PushStyleColor (ImGuiCol_ChildBg, colour (notepad::kStagePalette.shell));
        ImGui::BeginChild ("ribbon", ImVec2 (0.0f, height),
                           ImGuiChildFlags_AlwaysUseWindowPadding,
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        ImGui::BeginDisabled (! history.canUndo());
        if (toolbarButton ("↶", "Undo the last edit (Ctrl+Z)", false))
            restoreHistory (false);
        ImGui::EndDisabled();
        ImGui::SameLine (0.0f, 5.0f);
        ImGui::BeginDisabled (! history.canRedo());
        if (toolbarButton ("↷", "Redo the last edit (Ctrl+Y)", false))
            restoreHistory (true);
        ImGui::EndDisabled();

        // The ribbon stays centred on a songwriter's working loop: structure,
        // chords and just enough lyric styling to distinguish the title and
        // add emphasis. General-purpose Markdown authoring belongs in the
        // compatible notepad.md file rather than competing with those actions.
        ImGui::SameLine (0.0f, 16.0f);
        if (toolbarButton ("Chord", "Add or edit a chord (Ctrl+K); repeat the previous chord (Ctrl+Shift+K)",
                           editor.chordEntryActive(), 58.0f))
        {
            editor.beginChordEntry();
            editor.requestFocus();
        }
        ImGui::SameLine (0.0f, 5.0f);
        if (toolbarButton ("Section", "Add or remove a song section", false, 64.0f))
            ImGui::OpenPopup ("section-menu");

        const bool canTranspose = document.hasChords();
        ImGui::BeginDisabled (! canTranspose);
        ImGui::SameLine (0.0f, 5.0f);
        if (toolbarButton ("−1", "Transpose every chord down one semitone", false, 38.0f))
            editor.transposeChords (-1);
        ImGui::SameLine (0.0f, 5.0f);
        if (toolbarButton ("+1", "Transpose every chord up one semitone", false, 38.0f))
            editor.transposeChords (1);
        ImGui::EndDisabled();

        const auto blockStyle = selectedBlockStyle();
        ImGui::SameLine (0.0f, 16.0f);
        if (toolbarButton ("Lyrics", "Use lyric or note text",
                           blockStyle == NotepadDocument::BlockStyle::body, 54.0f))
            applyBlock (NotepadDocument::BlockStyle::body);
        ImGui::SameLine (0.0f, 5.0f);
        if (toolbarButton ("Title", "Make this line the song title",
                           blockStyle == NotepadDocument::BlockStyle::heading1,
                           48.0f, boldFont))
            applyBlock (NotepadDocument::BlockStyle::heading1);

        ImGui::SameLine (0.0f, 16.0f);
        if (toolbarButton ("B", "Bold the selection (Ctrl+B)",
                           inlineStyleActive (NotepadDocument::InlineStyle::bold),
                           34.0f, boldFont))
            applyInline (NotepadDocument::InlineStyle::bold);
        ImGui::SameLine (0.0f, 5.0f);
        if (toolbarButton ("I", "Italicise the selection (Ctrl+I)",
                           inlineStyleActive (NotepadDocument::InlineStyle::italic)))
            applyInline (NotepadDocument::InlineStyle::italic);

        const auto spelling = document.spellingMode();
        ImGui::SameLine (0.0f, 16.0f);
        if (toolbarButton ("Auto##spell-auto", "Match the song's existing sharps or flats",
                           spelling == NotepadDocument::Spelling::followDocument, 46.0f))
            document.setSpelling (NotepadDocument::Spelling::followDocument);
        ImGui::SameLine (0.0f, 4.0f);
        if (toolbarButton ("♯##spell-sharps", "Spell chords with sharps",
                           spelling == NotepadDocument::Spelling::sharps))
            document.setSpelling (NotepadDocument::Spelling::sharps);
        ImGui::SameLine (0.0f, 4.0f);
        if (toolbarButton ("♭##spell-flats", "Spell chords with flats",
                           spelling == NotepadDocument::Spelling::flats))
            document.setSpelling (NotepadDocument::Spelling::flats);

        drawSectionMenu();

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar (2);
    }

    void drawSectionMenu()
    {
        if (! ImGui::BeginPopup ("section-menu"))
            return;

        if (document.lineInfoAt (editor.selection().start).section)
        {
            if (ImGui::Selectable ("Remove current section"))
                editor.removeSectionMarker();
            ImGui::Separator();
        }

        // Markers are plain "[Label]" lines, so a hand-typed section is the
        // same thing this writes.
        for (const auto* const label : { "Intro", "Verse", "Pre-chorus", "Chorus",
                                         "Bridge", "Solo", "Outro" })
            if (ImGui::Selectable (label))
                editor.insertSectionMarker (label);
        ImGui::EndPopup();
    }

    void drawEditor (float height)
    {
        ImGui::PushStyleColor (ImGuiCol_ChildBg, colour (notepad::kStagePalette.shell));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (22.0f, 18.0f));
        ImGui::BeginChild ("workspace", ImVec2 (0.0f, height),
                           ImGuiChildFlags_AlwaysUseWindowPadding,
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        // No page card: the stage is the paper, with a readable measure centred
        // inside the surrounding workspace.
        const auto available = ImGui::GetContentRegionAvail();
        const float editorWidth = std::min (kContentWidth, std::max (0.0f, available.x));
        ImGui::SetCursorPosX (ImGui::GetCursorPosX() + (available.x - editorWidth) * 0.5f);

        if (editorWidth > 0.0f && available.y > 0.0f)
        {
            ImGui::PushStyleVar (ImGuiStyleVar_ChildRounding, 0.0f);
            ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (34.0f, 26.0f));
            ImGui::PushStyleColor (ImGuiCol_ChildBg, colour (notepad::kStagePalette.stage));
            // A borderless child drops WindowPadding unless asked, which left the
            // lyric running edge to edge with no gutter.
            ImGui::BeginChild ("chart", ImVec2 (editorWidth, available.y),
                               ImGuiChildFlags_AlwaysUseWindowPadding,
                               ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

            editor.draw (ImGui::GetFontSize());

            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar (2);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    void draw (float width, float height)
    {
        auto& style = ImGui::GetStyle();
        style.WindowBorderSize = 0.0f;
        style.FrameBorderSize = 0.0f;
        style.FrameRounding = 4.0f;
        style.PopupRounding = 5.0f;
        style.ScrollbarRounding = 4.0f;
        style.ItemSpacing = ImVec2 (8.0f, 6.0f);
        style.Colors[ImGuiCol_Button] = colour (0x2b2c35ff);
        style.Colors[ImGuiCol_ButtonHovered] = colour (0x393b47ff);
        style.Colors[ImGuiCol_ButtonActive] = colour (0x4a4c5aff);
        style.Colors[ImGuiCol_FrameBg] = colour (0x292a33ff);
        style.Colors[ImGuiCol_FrameBgHovered] = colour (0x32343eff);
        style.Colors[ImGuiCol_FrameBgActive] = colour (0x373944ff);
        style.Colors[ImGuiCol_Header] = colour (0x393b47ff);
        style.Colors[ImGuiCol_HeaderHovered] = colour (0x4a4c5aff);
        style.Colors[ImGuiCol_PopupBg] = colour (notepad::kStagePalette.shell);
        style.Colors[ImGuiCol_Border] = colour (notepad::kStagePalette.rule);
        style.Colors[ImGuiCol_Text] = colour (notepad::kStagePalette.lyric);
        style.Colors[ImGuiCol_TextDisabled] = colour (notepad::kStagePalette.muted);

        ImGui::SetNextWindowPos (ImVec2 (0.0f, 0.0f));
        ImGui::SetNextWindowSize (ImVec2 (width, height));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (0.0f, 0.0f));
        ImGui::PushStyleColor (ImGuiCol_WindowBg, colour (notepad::kStagePalette.shell));
        ImGui::Begin ("Session Notepad", nullptr,
                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                      | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar
                      | ImGuiWindowFlags_NoScrollWithMouse);

        ImGui::SetCursorPos (ImVec2 (22.0f, 14.0f));
        ImGui::TextUnformatted ("SESSION NOTEPAD");
        ImGui::SetCursorPos (ImVec2 (22.0f, 34.0f));
        ImGui::PushStyleColor (ImGuiCol_Text, colour (notepad::kStagePalette.muted));
        ImGui::TextUnformatted ("Lyrics and session notes");
        ImGui::PopStyleColor();

        ImGui::SetCursorPos (ImVec2 (width - 90.0f, 16.0f));
        if (ImGui::Button ("Done", ImVec2 (68.0f, 30.0f)))
            close();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip ("Close the notepad; changes stay with this session");

        // The status bar is a line of text plus its own padding: sizing it by a
        // constant left it a few pixels short of a full line once the child
        // asked for that padding, and it clipped its own glyphs.
        const auto statusPadding = 6.0f;
        const auto statusHeight = ImGui::GetTextLineHeight() + statusPadding * 2.0f;
        const auto bands = notepad::layoutChrome (height, kHeaderHeight, kRibbonHeight,
                                                  statusHeight);

        const auto ribbonTop = bands.header;
        const auto chartTop = ribbonTop + bands.ribbon;
        const auto statusTop = chartTop + bands.chart;
        if (bands.ribbon > 0.0f)
        {
            ImGui::SetCursorPos (ImVec2 (0.0f, ribbonTop));
            drawRibbon (bands.ribbon);
        }
        if (bands.chart > 0.0f)
        {
            ImGui::SetCursorPos (ImVec2 (0.0f, chartTop));
            drawEditor (bands.chart);
        }

        if (bands.status > 0.0f)
        {
            ImGui::SetCursorPos (ImVec2 (0.0f, statusTop));
            ImGui::PushStyleColor (ImGuiCol_ChildBg, colour (notepad::kStagePalette.shell));
            ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (18.0f, statusPadding));
            ImGui::BeginChild ("status", ImVec2 (0.0f, bands.status),
                               ImGuiChildFlags_AlwaysUseWindowPadding,
                               ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PushStyleColor (ImGuiCol_Text,
                                  saveFailed ? colour (0xe48a8aff) : colour (notepad::kStagePalette.muted));
            const char* saveState = saveFailed
                                  ? "Save failed, the notes are still unsaved"
                                  : documentDirty && hasSessionFile
                                      ? "Unsaved changes, saved when this closes"
                                  : documentDirty
                                      ? "Unsaved changes, save the session to keep them"
                                  : hasSessionFile ? "Saved with the session"
                                                   : "Untitled session, saves on first save";
            ImGui::TextUnformatted (saveState);
            if (! savedAtLabel.empty() && ! documentDirty && ! saveFailed)
            {
                ImGui::SameLine (0.0f, 6.0f);
                ImGui::TextUnformatted (savedAtLabel.c_str());
            }
            ImGui::SameLine (0.0f, 8.0f);
            ImGui::TextUnformatted ("notepad.md");
            ImGui::PopStyleColor();

            // What a songwriter wants from a glance: how the song is built, what it
            // is built from, and what it is in.
            const auto chordNames = document.uniqueChordNames();
            const auto key = notepad::chords::detectKey (chordNames);
            const auto sections = document.sectionCount();
            std::string summary = std::to_string (sections)
                                + (sections == 1 ? " section" : " sections") + "  |  ";
            summary += std::to_string (chordNames.size())
                     + (chordNames.size() == 1 ? " chord" : " chords");
            if (! key.empty())
                summary += "  |  key of " + key;
            ImGui::PushStyleColor (ImGuiCol_Text, colour (notepad::kStagePalette.muted));
            const auto summaryWidth = ImGui::CalcTextSize (summary.c_str()).x;
            ImGui::SameLine (std::max (300.0f, width - summaryWidth - 20.0f));
            ImGui::TextUnformatted (summary.c_str());
            ImGui::PopStyleColor();
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    EmbeddedApplication app;
    std::unique_ptr<DGL::Window> window;
    std::unique_ptr<EditorWidget> editorWidget;
    NotepadDocument document;
    notepad::UndoStack history;
    NotepadEditor editor { document, history };
    TextChangedCallback onTextChanged;
    ClosedCallback onClosed;
    LinkOpenedCallback onLinkOpened;
    ImFont* bodyFont = nullptr;
    ImFont* boldFont = nullptr;
    ImFont* italicFont = nullptr;
    ImFont* boldItalicFont = nullptr;
    ImFont* monoFont = nullptr;
    std::optional<std::string> savedMarkdown;
    std::string savedAtLabel;
    bool closeRequested = false;
    bool closeWasPumped = false;
    bool hasSessionFile = false;
    bool documentDirty = false;
    bool saveFailed = false;
};

NativeNotepadWindow::NativeNotepadWindow()
    : impl (std::make_unique<Impl>())
{
}

NativeNotepadWindow::~NativeNotepadWindow() = default;

void NativeNotepadWindow::setCallbacks (TextChangedCallback textChanged, ClosedCallback closed,
                                        LinkOpenedCallback linkOpened)
{
    impl->setCallbacks (std::move (textChanged), std::move (closed),
                        std::move (linkOpened));
}

bool NativeNotepadWindow::open (std::uintptr_t nativeParent, EmbeddedGeometry geometry,
                                const std::string& markdown,
                                bool hasSessionFile, bool hasUnsavedChanges)
{
    return impl->open (nativeParent, geometry, markdown, hasSessionFile, hasUnsavedChanges);
}

void NativeNotepadWindow::close()
{
    impl->close();
}

void NativeNotepadWindow::setEmbeddedGeometry (EmbeddedGeometry geometry)
{
    impl->setEmbeddedGeometry (geometry);
}

bool NativeNotepadWindow::isOpen() const noexcept
{
    return impl->isOpen();
}

void NativeNotepadWindow::markSaved()
{
    impl->markSaved();
}

void NativeNotepadWindow::markSaveFailed()
{
    impl->markSaveFailed();
}
} // namespace duskstudio
