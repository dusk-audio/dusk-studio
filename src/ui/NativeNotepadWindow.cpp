#include "NativeNotepadWindow.h"
#include "NotepadDocument.h"
#include "NotepadEditor.h"
#include "NotepadChords.h"
#include "NotepadEditorCore.h"
#include "NotepadGraphicsCompatibility.h"
#include "NotepadTheme.h"
#include "../foundation/Fs.h"
#include "imgui/DuskImGuiHost.h"

#include <DearImGui.hpp>
#include <DearImGui/imgui_internal.h>
#ifndef DGL_NO_SHARED_RESOURCES
# include "src/Resources.hpp"
#endif

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <optional>
#include <utility>

namespace duskstudio
{
namespace
{
// The chart's measure: wide enough for a long lyric line, narrow enough that
// the eye does not lose the line it is reading.
constexpr float kContentWidth = 760.0f;
constexpr float kHeaderHeight = 62.0f;
constexpr float kRibbonHeight = 62.0f;
constexpr float kToolbarButtonLabelPadding = 18.0f;
constexpr float kToolbarButtonMinWidth = 34.0f;
// Below this the ribbon cannot hold the full row at its roomy spacing.
constexpr float kRibbonRoomyWidth = 780.0f;

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

std::filesystem::path firstFrameMarkerPath()
{
    const auto cfg = dusk::fs::userConfigDir();
    if (cfg.empty())
        return {};
    return cfg / "Dusk Studio" / "notepad-first-frame";
}

ImVec4 colour (unsigned int hex)
{
    return ImVec4 (((hex >> 24) & 0xff) / 255.0f,
                   ((hex >> 16) & 0xff) / 255.0f,
                   ((hex >> 8) & 0xff) / 255.0f,
                   (hex & 0xff) / 255.0f);
}

float toolbarButtonWidth (const char* label, float labelPadding)
{
    return std::max (kToolbarButtonMinWidth,
                     ImGui::CalcTextSize (label, nullptr, true).x + labelPadding);
}

int resizeMarkdownBuffer (ImGuiInputTextCallbackData* data)
{
    if (data == nullptr || data->EventFlag != ImGuiInputTextFlags_CallbackResize
        || data->UserData == nullptr)
        return 0;

    auto& buffer = *static_cast<std::string*> (data->UserData);
    buffer.resize (static_cast<std::size_t> (std::max (0, data->BufTextLen)));
    data->Buf = buffer.data();
    return 0;
}

} // namespace

struct NativeNotepadWindow::Impl final
{
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
            // ImGui's multiline input treats Escape as "revert to activation",
            // which can discard a complete Markdown-source editing pass. In
            // source view Escape closes through the normal deferred save path.
            if (owner.handleMarkdownEscape (event))
                return true;
            if (owner.handleChordEntryNavigation (event))
                return true;
            return DGL::ImGuiTopLevelWidget::onKeyboard (event);
        }

    private:
        Impl& owner;
    };

    Impl()
    {
        imgui::DuskImGuiHost::Callbacks callbacks;
        callbacks.createWidget = [this] (DGL::Window& window)
        {
            auto widget = std::make_unique<EditorWidget> (window, *this);
            buildFontAtlas (static_cast<float> (notepad::kTypeScale.lyric
                                                * window.getScaleFactor()));
            return std::unique_ptr<DGL::TopLevelWidget> (widget.release());
        };
        callbacks.checkGraphics = [this] (const char* version, const char* renderer)
        {
            const auto compatibility = notepad::assessGraphicsCompatibility (version, renderer);
            if (compatibility == notepad::GraphicsCompatibility::supported)
                return std::string();

            if (compatibility == notepad::GraphicsCompatibility::unsafeMesaD3D12)
            {
                host.log ("Mesa D3D12 (OpenGL Compatibility Pack) is known "
                          "to terminate the host; notepad unavailable");
                return std::string ("Notepad unavailable: the OpenGL Compatibility Pack renderer "
                                    "ends the application. Install your graphics vendor's driver.");
            }

            host.log ("display provides no OpenGL 3 context; notepad unavailable");
            return std::string ("Notepad unavailable: this display provides no OpenGL 3 context.");
        };
        callbacks.widgetReleased = [this]
        {
            bodyFont = boldFont = italicFont = boldItalicFont = monoFont = nullptr;
            editor.setFonts ({});
        };
        callbacks.closed = [this]
        {
            if (onClosed)
                onClosed();
        };
        host.setCallbacks (std::move (callbacks));

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
            (void*) daf_resources::dejavusans_ttf,
            daf_resources::dejavusans_ttf_size,
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

    void setCallbacks (TextChangedCallback changed, ClosedCallback closed,
                       LinkOpenedCallback linkOpened)
    {
        onTextChanged = std::move (changed);
        onClosed = std::move (closed);
        onLinkOpened = std::move (linkOpened);
    }

    bool open (std::uintptr_t nativeParent, EmbeddedGeometry geometry,
               const std::string& markdown, bool sessionExists, bool unsavedChanges)
    {
        document.setMarkdown (markdown);
        editor.reset ({ 0, 0 });
        editor.requestFocus();
        markdownView = false;
        markdownEntrySnapshot.reset();
        markdownFocusRequested = false;
        hasSessionFile = sessionExists;
        documentDirty = unsavedChanges;
        saveFailed = false;
        savedAtLabel.clear();
        savedMarkdown = unsavedChanges ? std::optional<std::string> {}
                                       : std::optional<std::string> { markdown };
        history.clear();

        return host.open (nativeParent, { geometry.x, geometry.y, geometry.width,
                                          geometry.height, geometry.scaleFactor });
    }

    void close()
    {
        // Dismissal can arrive from the DAW side (the dim backdrop) without
        // ever reaching the editor's own mouse handling, and the sidecar save
        // follows immediately. Settle an open chord slot here so every route
        // out of the notepad keeps the chord that was being typed.
        editor.closeChordEntry();
        host.close();
    }

    bool isOpen() const noexcept { return host.isOpen(); }

    const std::string& lastOpenFailure() const noexcept { return host.lastOpenFailure(); }

    void setEmbeddedGeometry (EmbeddedGeometry geometry)
    {
        host.setGeometry ({ geometry.x, geometry.y, geometry.width, geometry.height,
                            geometry.scaleFactor });
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
    bool handleMarkdownEscape (const DGL::Widget::KeyboardEvent& event)
    {
        if (! markdownView || event.key != DGL::kKeyEscape)
            return false;
        if (event.press)
            close();
        return true;
    }

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
        return { document.markdown(), editor.selection() };
    }

    void restoreHistory (bool redo)
    {
        notepad::Snapshot restored;
        if (! (redo ? history.redo (currentSnapshot(), restored)
                    : history.undo (currentSnapshot(), restored)))
            return;

        document.restoreMarkdown (restored.markdown);
        editor.reset (restored.selection);
        editor.requestFocus();
        notifyTextChanged();
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

    void setMarkdownView (bool shouldShowMarkdown)
    {
        if (markdownView == shouldShowMarkdown)
            return;

        if (shouldShowMarkdown)
        {
            editor.closeChordEntry();
            markdownBuffer = document.markdown();
            markdownEntrySnapshot = currentSnapshot();
            history.breakRun();
            markdownView = true;
            markdownFocusRequested = true;
            return;
        }

        const auto documentEnd = document.documentText().size();
        if (markdownEntrySnapshot.has_value()
            && markdownEntrySnapshot->markdown != document.markdown())
            history.record (notepad::EditKind::structural,
                            std::move (*markdownEntrySnapshot), documentEnd);
        markdownEntrySnapshot.reset();
        markdownView = false;
        editor.reset ({ documentEnd, documentEnd });
        editor.requestFocus();
    }

    bool toolbarButton (const char* label, const char* tooltip, bool active = false,
                        ImFont* labelFont = nullptr)
    {
        // The atlas is built at the display scale, so a fixed width clips its
        // own label the moment the font grows. Measure what is about to be
        // drawn instead - in the label's own font, and past the "##" id tail.
        if (active)
        {
            ImGui::PushStyleColor (ImGuiCol_Button, colour (notepad::kStagePalette.rule));
            ImGui::PushStyleColor (ImGuiCol_ButtonHovered,
                                   colour (notepad::kStagePalette.shell));
        }
        if (labelFont != nullptr)
            ImGui::PushFont (labelFont);
        const auto width = toolbarButtonWidth (label, toolbarLabelPadding);
        const bool clicked = ImGui::Button (label, ImVec2 (width, 32.0f));
        if (labelFont != nullptr)
            ImGui::PopFont();
        if (active)
            ImGui::PopStyleColor (2);
        // A disabled button is exactly the one whose tooltip explains why it is
        // disabled, so hovering has to register through BeginDisabled - and the
        // tooltip has to escape the dimming that BeginDisabled folds into the
        // global alpha, or the explanation is the hardest thing to read.
        if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::PushStyleVar (ImGuiStyleVar_Alpha, 1.0f);
            ImGui::SetTooltip ("%s", tooltip);
            ImGui::PopStyleVar();
        }
        return clicked;
    }

    void drawSourceToggle()
    {
        // The ribbon is the only chrome that survives into source view, so it
        // carries the way back out as well as the way in. Escape closes the
        // notepad rather than returning to the chart.
        if (toolbarButton ("Source", markdownView
                                       ? "Return to the chord chart"
                                       : "Edit the notepad.md text directly",
                           markdownView))
            setMarkdownView (! markdownView);
    }

    void drawRibbon (float height)
    {
        ImGui::PushStyleVar (ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (22.0f, 13.0f));
        ImGui::PushStyleColor (ImGuiCol_ChildBg, colour (notepad::kStagePalette.shell));
        ImGui::BeginChild ("ribbon", ImVec2 (0.0f, height),
                           ImGuiChildFlags_AlwaysUseWindowPadding,
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        // The row is sized for the notepad at its preferred width. A narrower
        // host window has to give something up, and the least useful thing to
        // lose is the breathing room around the buttons.
        const bool compact = ImGui::GetContentRegionAvail().x < kRibbonRoomyWidth;
        toolbarLabelPadding = compact ? 10.0f : kToolbarButtonLabelPadding;
        toolbarGap          = compact ? 3.0f : 5.0f;
        toolbarTightGap     = compact ? 3.0f : 4.0f;
        toolbarGroupGap     = compact ? 8.0f : 16.0f;

        // Source view replaces the chart with a plain text field, so the chart
        // controls have nothing to act on while it is up.
        if (! markdownView)
        {
            // The toggle's width is taken out of the row before the chart
            // controls are laid out, and they draw inside what is left. The way
            // back out of source view is then reachable at any ribbon width: a
            // row too long for the space loses its last controls rather than
            // pushing the toggle off the edge.
            const auto available = ImGui::GetContentRegionAvail();
            const auto controlsWidth = available.x
                                     - toolbarButtonWidth ("Source", toolbarLabelPadding)
                                     - toolbarGroupGap;
            // A zero width means "the rest of the row" to BeginChild, which is
            // the opposite of reserving space, so a ribbon with no room for the
            // controls carries the toggle alone.
            if (controlsWidth > 0.0f)
            {
                ImGui::BeginChild ("ribbon-controls", ImVec2 (controlsWidth, available.y),
                                   ImGuiChildFlags_None,
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                                   | ImGuiWindowFlags_NoBackground);
                drawChartControls();
                ImGui::EndChild();
                ImGui::SameLine (0.0f, toolbarGroupGap);
            }
        }
        drawSourceToggle();

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar (2);
    }

    void drawChartControls()
    {
        ImGui::BeginDisabled (! history.canUndo());
        if (toolbarButton ("↶", "Undo the last edit (Ctrl+Z)", false))
            restoreHistory (false);
        ImGui::EndDisabled();
        ImGui::SameLine (0.0f, toolbarGap);
        ImGui::BeginDisabled (! history.canRedo());
        if (toolbarButton ("↷", "Redo the last edit (Ctrl+Y)", false))
            restoreHistory (true);
        ImGui::EndDisabled();

        // The ribbon stays centred on a songwriter's working loop: structure,
        // chords and just enough lyric styling to distinguish the title and
        // add emphasis. General-purpose Markdown authoring belongs in the
        // compatible notepad.md file rather than competing with those actions.
        ImGui::SameLine (0.0f, toolbarGroupGap);
        if (toolbarButton ("Chord", "Add or edit a chord (Ctrl+K); repeat the previous chord (Ctrl+Shift+K)",
                           editor.chordEntryActive()))
        {
            editor.beginChordEntry();
            editor.requestFocus();
        }
        ImGui::SameLine (0.0f, toolbarGap);
        if (toolbarButton ("Section", "Add or remove a song section"))
            ImGui::OpenPopup ("section-menu");

        const bool canTranspose = document.hasChords();
        ImGui::BeginDisabled (! canTranspose);
        ImGui::SameLine (0.0f, toolbarGap);
        if (toolbarButton ("−1", "Transpose every chord down one semitone"))
            editor.transposeChords (-1);
        ImGui::SameLine (0.0f, toolbarGap);
        if (toolbarButton ("+1", "Transpose every chord up one semitone"))
            editor.transposeChords (1);
        ImGui::EndDisabled();

        const auto blockStyle = selectedBlockStyle();
        ImGui::SameLine (0.0f, toolbarGroupGap);
        if (toolbarButton ("Lyrics", "Use lyric or note text",
                           blockStyle == NotepadDocument::BlockStyle::body))
            applyBlock (NotepadDocument::BlockStyle::body);
        ImGui::SameLine (0.0f, toolbarGap);
        if (toolbarButton ("Title", "Make this line the song title",
                           blockStyle == NotepadDocument::BlockStyle::heading1, boldFont))
            applyBlock (NotepadDocument::BlockStyle::heading1);

        ImGui::SameLine (0.0f, toolbarGroupGap);
        if (toolbarButton ("B", "Bold the selection (Ctrl+B)",
                           inlineStyleActive (NotepadDocument::InlineStyle::bold), boldFont))
            applyInline (NotepadDocument::InlineStyle::bold);
        ImGui::SameLine (0.0f, toolbarGap);
        if (toolbarButton ("I", "Italicise the selection (Ctrl+I)",
                           inlineStyleActive (NotepadDocument::InlineStyle::italic)))
            applyInline (NotepadDocument::InlineStyle::italic);

        const auto spelling = document.spellingMode();
        ImGui::SameLine (0.0f, toolbarGroupGap);
        if (toolbarButton ("Auto##spell-auto", "Match the song's existing sharps or flats",
                           spelling == NotepadDocument::Spelling::followDocument))
            document.setSpelling (NotepadDocument::Spelling::followDocument);
        ImGui::SameLine (0.0f, toolbarTightGap);
        if (toolbarButton ("♯##spell-sharps", "Spell chords with sharps",
                           spelling == NotepadDocument::Spelling::sharps))
            document.setSpelling (NotepadDocument::Spelling::sharps);
        ImGui::SameLine (0.0f, toolbarTightGap);
        if (toolbarButton ("♭##spell-flats", "Spell chords with flats",
                           spelling == NotepadDocument::Spelling::flats))
            document.setSpelling (NotepadDocument::Spelling::flats);

        drawSectionMenu();
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

    void drawMarkdownEditor (float height)
    {
        ImGui::PushStyleColor (ImGuiCol_ChildBg, colour (notepad::kStagePalette.shell));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (22.0f, 18.0f));
        ImGui::BeginChild ("markdown-workspace", ImVec2 (0.0f, height),
                           ImGuiChildFlags_AlwaysUseWindowPadding,
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        const auto available = ImGui::GetContentRegionAvail();
        const float editorWidth = std::min (kContentWidth, std::max (0.0f, available.x));
        ImGui::SetCursorPosX (ImGui::GetCursorPosX() + (available.x - editorWidth) * 0.5f);

        if (editorWidth > 0.0f && available.y > 0.0f)
        {
            ImGui::PushStyleColor (ImGuiCol_FrameBg, colour (notepad::kStagePalette.stage));
            ImGui::PushStyleColor (ImGuiCol_FrameBgHovered,
                                   colour (notepad::kStagePalette.stage));
            ImGui::PushStyleColor (ImGuiCol_FrameBgActive,
                                   colour (notepad::kStagePalette.stage));
            ImGui::PushStyleVar (ImGuiStyleVar_FramePadding, ImVec2 (34.0f, 26.0f));
            if (monoFont != nullptr)
                ImGui::PushFont (monoFont);
            if (markdownFocusRequested)
            {
                ImGui::SetKeyboardFocusHere();
                markdownFocusRequested = false;
            }

            const auto flags = ImGuiInputTextFlags_AllowTabInput
                             | ImGuiInputTextFlags_CallbackResize;
            if (ImGui::InputTextMultiline ("##markdown-source", markdownBuffer.data(),
                                           markdownBuffer.capacity() + 1,
                                           ImVec2 (editorWidth, available.y), flags,
                                           resizeMarkdownBuffer, &markdownBuffer))
            {
                document.restoreMarkdown (markdownBuffer);
                notifyTextChanged();
            }

            if (monoFont != nullptr)
                ImGui::PopFont();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor (3);
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
            if (markdownView)
                drawMarkdownEditor (bands.chart);
            else
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

    float toolbarLabelPadding = kToolbarButtonLabelPadding;
    float toolbarGap = 5.0f;
    float toolbarTightGap = 4.0f;
    float toolbarGroupGap = 16.0f;
    imgui::DuskImGuiHost host { { "dusk-studio-notepad", "notepad", "Notepad" },
                                firstFrameMarkerPath() };
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
    std::optional<notepad::Snapshot> markdownEntrySnapshot;
    std::string markdownBuffer;
    std::string savedAtLabel;
    bool markdownView = false;
    bool markdownFocusRequested = false;
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

const std::string& NativeNotepadWindow::lastOpenFailure() const noexcept
{
    return impl->lastOpenFailure();
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
