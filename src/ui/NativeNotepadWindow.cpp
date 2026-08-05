#include "NativeNotepadWindow.h"
#include "NotepadDocument.h"
#include "NotepadEditor.h"
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
#include <array>
#include <cmath>
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
        editor.setDarkPage (darkDocumentPage);
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
        markdownMode = false;
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

    const notepad::Palette& theme() const noexcept
    {
        return notepad::palette (darkDocumentPage);
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
    notepad::Snapshot currentSnapshot() const
    {
        return { document.markdown(), editor.selection(), markdownMode };
    }

    NotepadDocument::Selection selectionForCurrentMode (const notepad::Snapshot& state) const
    {
        if (state.selectionIsSource == markdownMode)
            return state.selection;
        return markdownMode ? document.sourceSelection (state.selection)
                            : document.documentSelection (state.selection);
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

    void setMode (bool useMarkdown)
    {
        if (markdownMode == useMarkdown)
            return;

        const auto viewport = editor.caretViewportOffset();
        const auto oldSelection = editor.selection();
        const auto mapped = useMarkdown ? document.sourceSelection (oldSelection)
                                        : document.documentSelection (oldSelection);
        markdownMode = useMarkdown;
        history.breakRun();
        document.setSourceMode (useMarkdown);
        editor.reset (mapped);
        editor.keepCaretAtViewportOffset (viewport);
        editor.requestFocus();
    }

    void applyInline (NotepadDocument::InlineStyle inlineStyle)
    {
        editor.applyInlineStyle (inlineStyle);
    }

    void applyBlock (NotepadDocument::BlockStyle style)
    {
        editor.applyBlockStyle (style);
    }

    void insertLink (const std::string& url)
    {
        editor.insertLink (url);
    }

    bool inlineStyleActive (NotepadDocument::InlineStyle style) const
    {
        return editor.inlineStyleActive (style);
    }

    NotepadDocument::BlockStyle selectedBlockStyle() const
    {
        return editor.blockStyle();
    }

    bool toolbarButton (const char* label, const char* tooltip, bool active,
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

    bool linkToolbarButton()
    {
        const bool clicked = ImGui::Button ("##insert-link", ImVec2 (34.0f, 32.0f));
        const auto min = ImGui::GetItemRectMin();
        const auto max = ImGui::GetItemRectMax();
        const auto centre = ImVec2 ((min.x + max.x) * 0.5f,
                                    (min.y + max.y) * 0.5f);
        const auto iconColour = ImGui::GetColorU32 (ImGuiCol_Text);
        auto* const drawList = ImGui::GetWindowDrawList();

        // Two overlapping outlined rings read as a chain link at toolbar size
        // and avoid relying on an emoji glyph that is absent from the bundled
        // cross-platform document font.
        drawList->AddCircle (ImVec2 (centre.x - 4.0f, centre.y), 5.5f,
                             iconColour, 16, 1.6f);
        drawList->AddCircle (ImVec2 (centre.x + 4.0f, centre.y), 5.5f,
                             iconColour, 16, 1.6f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip ("Link the selected text");
        return clicked;
    }

    enum class SegmentIcon { sun, moon, eye, code };

    // Vector icons: the bundled document font carries no symbol coverage that
    // survives every platform's fallback, and the segments are small enough
    // that hinted glyphs would read fuzzy next to the ribbon.
    void drawSegmentIcon (ImDrawList& drawList, SegmentIcon icon, ImVec2 centre,
                          ImU32 iconColour, ImU32 backdrop)
    {
        switch (icon)
        {
            case SegmentIcon::sun:
            {
                drawList.AddCircleFilled (centre, 3.3f, iconColour, 20);
                for (int i = 0; i < 8; ++i)
                {
                    const auto angle = (float) i * 0.785398f;
                    const auto dx = std::cos (angle);
                    const auto dy = std::sin (angle);
                    drawList.AddLine (ImVec2 (centre.x + dx * 5.4f, centre.y + dy * 5.4f),
                                      ImVec2 (centre.x + dx * 7.4f, centre.y + dy * 7.4f),
                                      iconColour, 1.5f);
                }
                break;
            }
            case SegmentIcon::moon:
                // Crescent by subtraction: the bite is the segment's own
                // background punched back over a filled disc.
                drawList.AddCircleFilled (ImVec2 (centre.x - 0.6f, centre.y), 6.3f,
                                          iconColour, 24);
                drawList.AddCircleFilled (ImVec2 (centre.x + 3.0f, centre.y - 2.0f), 5.8f,
                                          backdrop, 24);
                break;
            case SegmentIcon::eye:
            {
                // Lens: two arcs of radius R meeting at (+-a, 0), R^2 = a^2 + d^2.
                constexpr float a = 7.0f;
                constexpr float d = 5.0f;
                const auto radius = std::sqrt (a * a + d * d);
                const auto corner = std::atan2 (d, a);
                drawList.PathArcTo (ImVec2 (centre.x, centre.y + d), radius,
                                    -3.14159265f + corner, -corner);
                drawList.PathArcTo (ImVec2 (centre.x, centre.y - d), radius,
                                    corner, 3.14159265f - corner);
                drawList.PathStroke (iconColour, ImDrawFlags_Closed, 1.6f);
                drawList.AddCircleFilled (centre, 2.2f, iconColour, 16);
                break;
            }
            case SegmentIcon::code:
            {
                const auto size = ImGui::CalcTextSize ("</>");
                drawList.AddText (ImVec2 (centre.x - size.x * 0.5f,
                                          centre.y - size.y * 0.5f),
                                  iconColour, "</>");
                break;
            }
        }
    }

    // Two-segment icon switch: dark track, active half lifted in a lighter
    // rounded fill. Each half is its own hit region, so keyboard activation
    // picks a segment rather than reading the pointer, which is somewhere else
    // entirely. Returns the clicked segment (0 left, 1 right), or -1.
    int drawSegmentedToggle (const char* id, bool rightSelected,
                             SegmentIcon leftIcon, SegmentIcon rightIcon,
                             const char* leftTooltip, const char* rightTooltip)
    {
        constexpr float segmentWidth = 32.0f;
        constexpr float padding = 3.0f;
        const ImVec2 half (segmentWidth + padding, 30.0f);

        ImGui::PushID (id);
        const bool leftClicked = ImGui::InvisibleButton ("left", half);
        const auto leftMin = ImGui::GetItemRectMin();
        const auto leftMax = ImGui::GetItemRectMax();
        const bool leftHovered = ImGui::IsItemHovered();
        ImGui::SameLine (0.0f, 0.0f);
        const bool rightClicked = ImGui::InvisibleButton ("right", half);
        const auto rightMax = ImGui::GetItemRectMax();
        const bool rightHovered = ImGui::IsItemHovered();
        ImGui::PopID();

        const auto min = leftMin;
        const auto max = rightMax;
        const auto split = leftMax.x;
        const bool hovered = leftHovered || rightHovered;
        auto& drawList = *ImGui::GetWindowDrawList();

        const auto trackColour = ImGui::GetColorU32 (colour (notepad::kStagePalette.rule));
        const auto pillColour = ImGui::GetColorU32 (
            colour (hovered ? 0x494a5aff : 0x3d3e4cff));
        drawList.AddRectFilled (min, max, trackColour, 9.0f);
        drawList.AddRectFilled (
            ImVec2 (rightSelected ? split : min.x + padding, min.y + padding),
            ImVec2 (rightSelected ? max.x - padding : split, max.y - padding),
            pillColour, 7.0f);

        const auto activeColour = ImGui::GetColorU32 (colour (notepad::kStagePalette.lyric));
        const auto restingColour = ImGui::GetColorU32 (colour (notepad::kStagePalette.muted));
        const auto hoverColour = ImGui::GetColorU32 (colour (0xc3c4ceff));
        const auto centreY = (min.y + max.y) * 0.5f;

        const auto drawSegment = [&] (SegmentIcon icon, float centreX, bool selected,
                                      bool pointerOver)
        {
            drawSegmentIcon (drawList, icon, ImVec2 (centreX, centreY),
                             selected ? activeColour
                                      : pointerOver ? hoverColour
                                                    : restingColour,
                             selected ? pillColour : trackColour);
        };

        drawSegment (leftIcon, (min.x + padding + split) * 0.5f, ! rightSelected, leftHovered);
        drawSegment (rightIcon, (split + max.x - padding) * 0.5f, rightSelected, rightHovered);

        if (leftHovered)
            ImGui::SetTooltip ("%s", leftTooltip);
        else if (rightHovered)
            ImGui::SetTooltip ("%s", rightTooltip);
        return leftClicked ? 0 : rightClicked ? 1 : -1;
    }

    void drawRibbon()
    {
        ImGui::PushStyleVar (ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (22.0f, 13.0f));
        ImGui::PushStyleColor (ImGuiCol_ChildBg, colour (notepad::kStagePalette.shell));
        ImGui::BeginChild ("ribbon", ImVec2 (0.0f, 62.0f), false,
                           ImGuiWindowFlags_NoScrollbar);

        if (toolbarButton ("↶", "Undo the last edit (Ctrl+Z)", false))
            restoreHistory (false);
        ImGui::SameLine (0.0f, 5.0f);
        if (toolbarButton ("↷", "Redo the last edit (Ctrl+Y)", false))
            restoreHistory (true);
        ImGui::SameLine (0.0f, 10.0f);

        static const std::array<const char*, 4> styleNames {
            "Body text", "Heading 1", "Heading 2", "Heading 3"
        };
        const auto blockStyle = selectedBlockStyle();
        const auto paragraphStyle = blockStyle >= NotepadDocument::BlockStyle::body
                                 && blockStyle <= NotepadDocument::BlockStyle::heading3
                                  ? static_cast<int> (blockStyle)
                                  : 0;
        ImGui::SetNextItemWidth (118.0f);
        ImGui::PushStyleVar (ImGuiStyleVar_FramePadding, ImVec2 (8.0f, 8.5f));
        if (ImGui::BeginCombo ("##paragraph-style", styleNames[static_cast<std::size_t> (paragraphStyle)]))
        {
            for (int i = 0; i < static_cast<int> (styleNames.size()); ++i)
            {
                if (ImGui::Selectable (styleNames[static_cast<std::size_t> (i)], paragraphStyle == i))
                {
                    applyBlock (static_cast<NotepadDocument::BlockStyle> (i));
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip ("Set the style of the lines you have selected");
        ImGui::SameLine (0.0f, 10.0f);
        const bool bold = inlineStyleActive (NotepadDocument::InlineStyle::bold);
        const bool italic = inlineStyleActive (NotepadDocument::InlineStyle::italic);
        const bool code = inlineStyleActive (NotepadDocument::InlineStyle::code);
        if (toolbarButton ("B", "Bold the selection (Ctrl+B)", bold, 34.0f, boldFont))
            applyInline (NotepadDocument::InlineStyle::bold);
        ImGui::SameLine (0.0f, 5.0f);
        if (toolbarButton ("I", "Italicise the selection (Ctrl+I)", italic))
            applyInline (NotepadDocument::InlineStyle::italic);
        ImGui::SameLine (0.0f, 5.0f);
        if (toolbarButton ("</>", "Set the selection in a fixed-width face", code, 42.0f))
            applyInline (NotepadDocument::InlineStyle::code);

        ImGui::SameLine (0.0f, 16.0f);
        if (toolbarButton ("❝", "Quote the selected lines",
                           blockStyle == NotepadDocument::BlockStyle::quote))
            applyBlock (NotepadDocument::BlockStyle::quote);
        ImGui::SameLine (0.0f, 5.0f);
        if (toolbarButton ("•≡", "Bullet the selected lines",
                           blockStyle == NotepadDocument::BlockStyle::bullets, 42.0f))
            applyBlock (NotepadDocument::BlockStyle::bullets);
        ImGui::SameLine (0.0f, 5.0f);
        if (toolbarButton ("1≡", "Number the selected lines",
                           blockStyle == NotepadDocument::BlockStyle::numbers, 42.0f))
            applyBlock (NotepadDocument::BlockStyle::numbers);
        ImGui::SameLine (0.0f, 5.0f);
        if (toolbarButton ("☑", "Turn the selected lines into a checklist",
                           blockStyle == NotepadDocument::BlockStyle::tasks))
            applyBlock (NotepadDocument::BlockStyle::tasks);
        ImGui::SameLine (0.0f, 5.0f);
        if (linkToolbarButton())
            ImGui::OpenPopup ("Insert link");

        ImGui::SameLine (0.0f, 16.0f);
        if (toolbarButton ("♯", "Put a chord over the word at the caret (Ctrl+K)",
                           editor.chordEntryActive()))
        {
            editor.beginChordEntry();
            editor.requestFocus();
        }
        // Transpose only has meaning once the sheet carries chords.
        const bool canTranspose = document.hasChords();
        ImGui::BeginDisabled (! canTranspose);
        ImGui::SameLine (0.0f, 5.0f);
        if (toolbarButton ("−", "Transpose every chord down a semitone", false))
            editor.transposeChords (-1);
        ImGui::SameLine (0.0f, 5.0f);
        if (toolbarButton ("+", "Transpose every chord up a semitone", false))
            editor.transposeChords (1);
        ImGui::EndDisabled();

        if (ImGui::BeginPopupModal ("Insert link", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted ("Link destination");
            ImGui::SetNextItemWidth (360.0f);
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            ImGui::InputTextWithHint ("##link-url", "https://", linkUrl.data(), linkUrl.size());
            if (ImGui::Button ("Insert", ImVec2 (90.0f, 30.0f)))
            {
                const auto url = linkUrl[0] == '\0' ? std::string ("https://")
                                                   : notepad::encodeMarkdownLinkTarget (
                                                         linkUrl.data());
                insertLink (url);
                linkUrl.fill ('\0');
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button ("Cancel", ImVec2 (90.0f, 30.0f)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar (2);
    }

    void drawEditor (float height)
    {
        ImGui::PushStyleColor (ImGuiCol_ChildBg, colour (notepad::kStagePalette.shell));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (22.0f, 18.0f));
        ImGui::BeginChild ("workspace", ImVec2 (0.0f, height), false,
                           ImGuiWindowFlags_NoScrollbar);

        // Both views render into the same measure at the same metrics, so the
        // toggle reads as flipping one sheet over rather than opening another
        // document. No page card: the stage is the paper.
        const auto available = ImGui::GetContentRegionAvail();
        const float editorWidth = std::min (kContentWidth, std::max (0.0f, available.x));
        ImGui::SetCursorPosX (ImGui::GetCursorPosX() + (available.x - editorWidth) * 0.5f);

        ImGui::PushStyleVar (ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (34.0f, 26.0f));
        ImGui::PushStyleColor (ImGuiCol_ChildBg, colour (theme().stage));
        ImGui::BeginChild ("chart", ImVec2 (editorWidth, std::max (0.0f, available.y)), false);

        editor.draw (ImGui::GetFontSize());

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar (2);
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
                      | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);

        ImGui::SetCursorPos (ImVec2 (22.0f, 14.0f));
        ImGui::TextUnformatted ("SESSION NOTEPAD");
        ImGui::SetCursorPos (ImVec2 (22.0f, 34.0f));
        ImGui::PushStyleColor (ImGuiCol_Text, colour (notepad::kStagePalette.muted));
        ImGui::TextUnformatted ("Lyrics and session notes");
        ImGui::PopStyleColor();

        ImGui::SetCursorPos (ImVec2 (width - 170.0f, 16.0f));
        const int pageSegment = drawSegmentedToggle ("##page-theme", darkDocumentPage,
                                                     SegmentIcon::sun, SegmentIcon::moon,
                                                     "Show the chart on a light page", "Show the chart on a dark page");
        if (pageSegment >= 0 && (pageSegment == 1) != darkDocumentPage)
        {
            darkDocumentPage = pageSegment == 1;
            editor.setDarkPage (darkDocumentPage);
        }
        ImGui::SameLine (0.0f, 8.0f);
        const int modeSegment = drawSegmentedToggle ("##editor-mode", markdownMode,
                                                     SegmentIcon::eye, SegmentIcon::code,
                                                     "Read the chart", "Edit the Markdown source");
        if (modeSegment >= 0)
            setMode (modeSegment == 1);

        ImGui::SetCursorPosY (62.0f);
        drawRibbon();

        const float statusHeight = 28.0f;
        // Tiling WMs can ignore the minimum-size hint; never hand a negative
        // extent to the editor child.
        drawEditor (std::max (0.0f, height - 62.0f - 62.0f - statusHeight));

        ImGui::PushStyleColor (ImGuiCol_ChildBg, colour (notepad::kStagePalette.shell));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (18.0f, 6.0f));
        ImGui::BeginChild ("status", ImVec2 (0.0f, statusHeight), false,
                           ImGuiWindowFlags_NoScrollbar);
        ImGui::PushStyleColor (ImGuiCol_Text,
                              saveFailed ? colour (0xe48a8aff) : colour (notepad::kStagePalette.muted));
        const char* saveState = saveFailed ? "Save failed - changes are still unsaved"
                              : documentDirty && hasSessionFile
                                  ? "Unsaved changes - saved when closed"
                              : documentDirty
                                  ? "Unsaved changes - save the session to keep them"
                              : hasSessionFile ? "Saved with session  |  notepad.md"
                                               : "Untitled session - saves on first session save";
        ImGui::TextUnformatted (saveState);
        const auto summary = std::to_string (document.wordCount()) + " words  |  "
                           + std::to_string (document.characterCount()) + " characters";
        const auto summaryWidth = ImGui::CalcTextSize (summary.c_str()).x;
        ImGui::SameLine (std::max (300.0f, width - summaryWidth - 20.0f));
        ImGui::TextUnformatted (summary.c_str());
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

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
    std::array<char, 384> linkUrl {};
    std::optional<std::string> savedMarkdown;
    bool markdownMode = false;
    bool darkDocumentPage = true;
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
