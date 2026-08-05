#include "NativeNotepadWindow.h"
#include "NotepadDocument.h"
#include "NotepadEditor.h"
#include "NotepadEditorCore.h"
#include "../foundation/MessageThread.h"

#include <Application.hpp>
#include <DearImGui.hpp>
#include <DearImGui/imgui_internal.h>
#ifndef DGL_NO_SHARED_RESOURCES
# include "src/Resources.hpp"
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <optional>
#include <utility>

namespace duskstudio
{
namespace
{
constexpr auto kLightDocumentPaperColour = 0xf7f6f3ff;
constexpr auto kDarkDocumentPaperColour = 0x22242cff;

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

struct EditorBuffer
{
    std::string text;
    int selectionStart = 0;
    int selectionEnd = 0;
    int pendingSelectionStart = -1;
    int pendingSelectionEnd = -1;

    void assign (const std::string& value)
    {
        text = value;
        text.reserve (std::max<std::size_t> (4096, text.size() + 1024));
        selectionStart = selectionEnd = std::min<int> (selectionEnd,
                                                        static_cast<int> (text.size()));
    }

    static int callback (ImGuiInputTextCallbackData* data)
    {
        auto& self = *static_cast<EditorBuffer*> (data->UserData);
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
        {
            self.text.resize (static_cast<std::size_t> (data->BufTextLen));
            data->Buf = self.text.data();
        }
        else if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways)
        {
            if (self.pendingSelectionStart >= 0)
            {
                data->SelectionStart = self.pendingSelectionStart;
                data->SelectionEnd = self.pendingSelectionEnd;
                data->CursorPos = self.pendingSelectionEnd;
                self.pendingSelectionStart = self.pendingSelectionEnd = -1;
            }
            if (data->SelectionStart == data->SelectionEnd)
                self.selectionStart = self.selectionEnd = data->CursorPos;
            else
            {
                self.selectionStart = data->SelectionStart;
                self.selectionEnd = data->SelectionEnd;
            }
        }
        return 0;
    }

    NotepadDocument::Selection selection() const noexcept
    {
        const auto start = static_cast<std::size_t> (std::max (0, selectionStart));
        const auto end = static_cast<std::size_t> (std::max (0, selectionEnd));
        return { std::min (start, end), std::max (start, end) };
    }

    void restoreSelection (NotepadDocument::Selection selection)
    {
        const auto limit = text.size();
        selectionStart = pendingSelectionStart
            = static_cast<int> (std::min (selection.start, limit));
        selectionEnd = pendingSelectionEnd
            = static_cast<int> (std::min (selection.end, limit));
    }
};

std::size_t sourceLineStart (const std::string& text, std::size_t offset)
{
    offset = std::min (offset, text.size());
    const auto found = offset == 0 ? std::string::npos : text.rfind ('\n', offset - 1);
    return found == std::string::npos ? 0 : found + 1;
}

std::size_t sourceLineEnd (const std::string& text, std::size_t offset)
{
    const auto found = text.find ('\n', std::min (offset, text.size()));
    return found == std::string::npos ? text.size() : found;
}

std::size_t sourceBlockPrefixLength (const std::string& text,
                                     std::size_t start,
                                     std::size_t end)
{
    auto i = start;
    while (i < end && text[i] == '#' && i - start < 6)
        ++i;
    if (i > start && i < end && text[i] == ' ')
        return i + 1 - start;

    i = start;
    while (i < end && text[i] >= '0' && text[i] <= '9')
        ++i;
    if (i > start && i + 1 < end && text[i] == '.' && text[i + 1] == ' ')
        return i + 2 - start;

    if (text.compare (start, std::min<std::size_t> (6, end - start), "- [ ] ") == 0
        || text.compare (start, std::min<std::size_t> (6, end - start), "- [x] ") == 0)
        return 6;
    if (end - start >= 2 && text[start + 1] == ' '
        && (text[start] == '-' || text[start] == '*' || text[start] == '+' || text[start] == '>'))
        return 2;
    return 0;
}

NotepadDocument::BlockStyle sourceBlockStyle (const std::string& text,
                                              NotepadDocument::Selection selection)
{
    const auto lineStart = sourceLineStart (text, selection.start);
    const auto lineEnd = sourceLineEnd (text, selection.start);
    const auto prefixLength = sourceBlockPrefixLength (text, lineStart, lineEnd);
    if (prefixLength == 0)
        return NotepadDocument::BlockStyle::body;

    if (text[lineStart] == '#')
    {
        const auto count = prefixLength - 1;
        if (count == 1) return NotepadDocument::BlockStyle::heading1;
        if (count == 2) return NotepadDocument::BlockStyle::heading2;
        return NotepadDocument::BlockStyle::heading3;
    }
    if (text[lineStart] == '>')
        return NotepadDocument::BlockStyle::quote;
    if (text.compare (lineStart, std::min<std::size_t> (6, lineEnd - lineStart), "- [ ] ") == 0
        || text.compare (lineStart, std::min<std::size_t> (6, lineEnd - lineStart), "- [x] ") == 0)
        return NotepadDocument::BlockStyle::tasks;
    if (text[lineStart] >= '0' && text[lineStart] <= '9')
        return NotepadDocument::BlockStyle::numbers;
    return NotepadDocument::BlockStyle::bullets;
}

// The destination is emitted inside "](...)", so a bracket or parenthesis the
// user typed would close it early and leave raw syntax in the document.
std::string percentEncodeUrl (const std::string& url)
{
    static constexpr char hexDigits[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve (url.size());
    for (const auto ch : url)
    {
        const auto byte = static_cast<unsigned char> (ch);
        if (byte <= 0x20 || byte == 0x7f || ch == '(' || ch == ')' || ch == '['
            || ch == ']' || ch == '<' || ch == '>' || ch == '"' || ch == '\\')
        {
            encoded.push_back ('%');
            encoded.push_back (hexDigits[byte >> 4]);
            encoded.push_back (hexDigits[byte & 0x0f]);
        }
        else
        {
            encoded.push_back (ch);
        }
    }
    return encoded;
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
            : DGL::ImGuiTopLevelWidget (window, 15.0f), owner (ownerRef) {}

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
        buffer.assign (document.markdown());
        buffer.restoreSelection ({ 0, 0 });
        editor.reset ({ 0, 0 });
        editor.requestFocus();
        closeRequested = false;
        closeWasPumped = false;
        focusEditorNextFrame = false;
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
            buildFontAtlas (static_cast<float> (15.0 * window->getScaleFactor()));
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
        return { document.markdown(),
                 markdownMode ? buffer.selection() : editor.selection(),
                 markdownMode };
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
        if (markdownMode)
        {
            buffer.assign (document.markdown());
            buffer.restoreSelection (selection);
            focusEditorNextFrame = true;
        }
        else
        {
            editor.reset (selection);
            editor.requestFocus();
        }
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
        const auto oldSelection = markdownMode ? buffer.selection() : editor.selection();
        markdownMode = useMarkdown;
        history.breakRun();
        if (markdownMode)
        {
            buffer.assign (document.markdown());
            buffer.restoreSelection (document.sourceSelection (oldSelection));
            focusEditorNextFrame = true;
        }
        else
        {
            editor.reset (document.documentSelection (oldSelection));
            editor.requestFocus();
        }
    }

    void applyInline (NotepadDocument::InlineStyle inlineStyle,
                      const std::string& prefix, const std::string& suffix)
    {
        if (! markdownMode)
        {
            editor.applyInlineStyle (inlineStyle);
            return;
        }

        const auto selection = buffer.selection();
        auto before = currentSnapshot();
        auto transformed = notepad::toggleMarkdownInline (
            document.markdown(), selection, prefix, suffix);
        document.setMarkdown (std::move (transformed.markdown));
        buffer.assign (document.markdown());
        buffer.restoreSelection (transformed.selection);
        history.record (notepad::EditKind::structural, std::move (before),
                        transformed.selection.end);
        focusEditorNextFrame = true;
        notifyTextChanged();
    }

    void applyBlock (NotepadDocument::BlockStyle style)
    {
        if (! markdownMode)
        {
            editor.applyBlockStyle (style);
            return;
        }

        const auto selection = buffer.selection();
        if (selectedBlockStyle() == style)
            style = NotepadDocument::BlockStyle::body;
        auto before = currentSnapshot();
        auto transformed = notepad::setMarkdownBlockStyle (
            document.markdown(), selection, style);
        document.setMarkdown (std::move (transformed.markdown));
        buffer.assign (document.markdown());
        buffer.restoreSelection (transformed.selection);
        history.record (notepad::EditKind::structural, std::move (before),
                        transformed.selection.end);
        focusEditorNextFrame = true;
        notifyTextChanged();
    }

    void insertLink (const std::string& url)
    {
        if (! markdownMode)
        {
            editor.insertLink (url);
            return;
        }

        const auto selection = buffer.selection();
        auto before = currentSnapshot();
        auto source = document.markdown();
        const auto label = selection.empty()
                         ? std::string ("Link text")
                         : source.substr (selection.start, selection.end - selection.start);
        const auto replacement = "[" + label + "](" + url + ")";
        source.replace (selection.start, selection.end - selection.start, replacement);
        document.setMarkdown (std::move (source));
        buffer.assign (document.markdown());
        buffer.restoreSelection ({ selection.start + 1, selection.start + 1 + label.size() });
        history.record (notepad::EditKind::structural, std::move (before),
                        selection.start + 1 + label.size());
        focusEditorNextFrame = true;
        notifyTextChanged();
    }

    bool inlineStyleActive (NotepadDocument::InlineStyle style,
                            const std::string& prefix,
                            const std::string& suffix) const
    {
        return markdownMode
             ? notepad::markdownInlineActive (document.markdown(), buffer.selection(),
                                               prefix, suffix)
             : editor.inlineStyleActive (style);
    }

    NotepadDocument::BlockStyle selectedBlockStyle() const
    {
        return markdownMode
             ? sourceBlockStyle (document.markdown(), buffer.selection())
             : editor.blockStyle();
    }

    bool toolbarButton (const char* label, const char* tooltip, bool active,
                        float width = 34.0f, ImFont* labelFont = nullptr)
    {
        if (active)
        {
            ImGui::PushStyleColor (ImGuiCol_Button, colour (0x5a4880ff));
            ImGui::PushStyleColor (ImGuiCol_ButtonHovered, colour (0x6b5796ff));
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
            ImGui::SetTooltip ("Insert link");
        return clicked;
    }

    bool drawGlyphToggle (const char* id, bool rightSelected,
                          const char* leftGlyph, const char* rightGlyph,
                          const char* tooltip, float width)
    {
        const bool clicked = ImGui::InvisibleButton (id, ImVec2 (width, 30.0f));
        const auto min = ImGui::GetItemRectMin();
        const auto max = ImGui::GetItemRectMax();
        const auto centre = ImVec2 ((min.x + max.x) * 0.5f,
                                    (min.y + max.y) * 0.5f);
        const bool hovered = ImGui::IsItemHovered();
        auto* const drawList = ImGui::GetWindowDrawList();

        const auto selectedColour = ImGui::GetColorU32 (colour (0xe7e8edff));
        const auto mutedColour = ImGui::GetColorU32 (
            colour (hovered ? 0xb5b6c0ff : 0x868892ff));
        const auto trackColour = ImGui::GetColorU32 (
            colour (hovered ? 0x6b5796ff : 0x5a4880ff));
        const auto knobColour = ImGui::GetColorU32 (colour (0xf0eff4ff));

        const auto drawCentredGlyph = [&] (const char* glyph, float x, bool selected)
        {
            const auto size = ImGui::CalcTextSize (glyph);
            drawList->AddText (ImVec2 (x - size.x * 0.5f,
                                       centre.y - size.y * 0.5f),
                               selected ? selectedColour : mutedColour, glyph);
        };

        drawCentredGlyph (leftGlyph, min.x + 11.0f, ! rightSelected);
        drawCentredGlyph (rightGlyph, max.x - 13.0f, rightSelected);

        constexpr float halfTrackWidth = 20.0f;
        constexpr float halfTrackHeight = 7.0f;
        drawList->AddRectFilled (
            ImVec2 (centre.x - halfTrackWidth, centre.y - halfTrackHeight),
            ImVec2 (centre.x + halfTrackWidth, centre.y + halfTrackHeight),
            trackColour, halfTrackHeight);
        drawList->AddCircleFilled (
            ImVec2 (centre.x + (rightSelected ? 12.0f : -12.0f), centre.y),
            5.0f, knobColour);

        if (hovered)
            ImGui::SetTooltip ("%s", tooltip);
        return clicked;
    }

    void drawRibbon()
    {
        ImGui::PushStyleVar (ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (22.0f, 13.0f));
        ImGui::PushStyleColor (ImGuiCol_ChildBg, colour (0x20212aff));
        ImGui::BeginChild ("ribbon", ImVec2 (0.0f, 62.0f), false,
                           ImGuiWindowFlags_NoScrollbar);

        if (toolbarButton ("↶", "Undo (Ctrl+Z)", false))
            restoreHistory (false);
        ImGui::SameLine (0.0f, 5.0f);
        if (toolbarButton ("↷", "Redo (Ctrl+Y)", false))
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
        ImGui::SameLine (0.0f, 10.0f);
        const bool bold = inlineStyleActive (NotepadDocument::InlineStyle::bold, "**", "**");
        const bool italic = inlineStyleActive (NotepadDocument::InlineStyle::italic, "*", "*");
        const bool code = inlineStyleActive (NotepadDocument::InlineStyle::code, "`", "`");
        if (toolbarButton ("B", "Bold (Ctrl+B)", bold, 34.0f, boldFont))
            applyInline (NotepadDocument::InlineStyle::bold, "**", "**");
        ImGui::SameLine (0.0f, 5.0f);
        if (toolbarButton ("I", "Italic (Ctrl+I)", italic))
            applyInline (NotepadDocument::InlineStyle::italic, "*", "*");
        ImGui::SameLine (0.0f, 5.0f);
        if (toolbarButton ("</>", "Inline code", code, 42.0f))
            applyInline (NotepadDocument::InlineStyle::code, "`", "`");

        ImGui::SameLine (0.0f, 16.0f);
        if (toolbarButton ("❝", "Quote selected paragraphs",
                           blockStyle == NotepadDocument::BlockStyle::quote))
            applyBlock (NotepadDocument::BlockStyle::quote);
        ImGui::SameLine (0.0f, 5.0f);
        if (toolbarButton ("•≡", "Bulleted list",
                           blockStyle == NotepadDocument::BlockStyle::bullets, 42.0f))
            applyBlock (NotepadDocument::BlockStyle::bullets);
        ImGui::SameLine (0.0f, 5.0f);
        if (toolbarButton ("1≡", "Numbered list",
                           blockStyle == NotepadDocument::BlockStyle::numbers, 42.0f))
            applyBlock (NotepadDocument::BlockStyle::numbers);
        ImGui::SameLine (0.0f, 5.0f);
        if (toolbarButton ("☑", "Checklist",
                           blockStyle == NotepadDocument::BlockStyle::tasks))
            applyBlock (NotepadDocument::BlockStyle::tasks);
        ImGui::SameLine (0.0f, 5.0f);
        if (linkToolbarButton())
            ImGui::OpenPopup ("Insert link");

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
                                                   : percentEncodeUrl (linkUrl.data());
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
        ImGui::PushStyleColor (ImGuiCol_ChildBg, colour (0x111218ff));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (22.0f, 18.0f));
        ImGui::BeginChild ("workspace", ImVec2 (0.0f, height), false,
                           ImGuiWindowFlags_NoScrollbar);

        const auto available = ImGui::GetContentRegionAvail();
        const float editorWidth = markdownMode ? available.x : std::min (760.0f, available.x);
        ImGui::SetCursorPosX (ImGui::GetCursorPosX() + (available.x - editorWidth) * 0.5f);

        ImGui::PushStyleVar (ImGuiStyleVar_ChildRounding, 3.0f);
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding,
                             markdownMode ? ImVec2 (20.0f, 18.0f) : ImVec2 (58.0f, 46.0f));
        ImGui::PushStyleColor (ImGuiCol_ChildBg,
                              markdownMode ? colour (0x181922ff)
                                           : colour (darkDocumentPage
                                                        ? kDarkDocumentPaperColour
                                                        : kLightDocumentPaperColour));
        ImGui::BeginChild (markdownMode ? "markdown-source" : "document-page",
                           ImVec2 (editorWidth, available.y), true);

        if (markdownMode)
            drawMarkdownSource();
        else
            editor.draw (ImGui::GetFontSize());

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar (2);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    void drawMarkdownSource()
    {
        ImGui::PushStyleColor (ImGuiCol_FrameBg, colour (0x181922ff));
        ImGui::PushStyleColor (ImGuiCol_Text, colour (0xd7d9e0ff));
        ImGui::PushStyleColor (ImGuiCol_TextSelectedBg, colour (0x70599aaa));
        ImGui::PushStyleVar (ImGuiStyleVar_FramePadding, ImVec2 (0.0f, 0.0f));

        if (focusEditorNextFrame)
        {
            ImGui::SetKeyboardFocusHere();
            focusEditorNextFrame = false;
        }

        const auto flags = ImGuiInputTextFlags_AllowTabInput
                         | ImGuiInputTextFlags_CallbackResize
                         | ImGuiInputTextFlags_CallbackAlways
                         | ImGuiInputTextFlags_NoUndoRedo;
        const auto selectionBefore = buffer.selection();
        const bool changed = ImGui::InputTextMultiline (
            "##notepad-editor", buffer.text.data(), buffer.text.capacity() + 1,
            ImGui::GetContentRegionAvail(), flags, EditorBuffer::callback, &buffer);
        const bool editorActive = ImGui::IsItemActive();
        if (auto* const state = ImGui::GetInputTextState (ImGui::GetItemID()))
        {
            // CallbackAlways can observe the selection before the current
            // character edit is committed. The live input state is the
            // authority after InputTextMultiline returns.
            if (state->HasSelection())
            {
                buffer.selectionStart = state->GetSelectionStart();
                buffer.selectionEnd = state->GetSelectionEnd();
            }
            else
            {
                buffer.selectionStart = buffer.selectionEnd = state->GetCursorPos();
            }
        }

        if (changed)
        {
            auto before = currentSnapshot();
            before.selection = selectionBefore;
            buffer.text.resize (std::strlen (buffer.text.c_str()));
            // Deletions must not join a typing run, or a backspace burst would
            // undo the text it was correcting along with itself.
            const auto kind = buffer.text.size() < before.markdown.size()
                            ? notepad::EditKind::deleting
                            : notepad::EditKind::typing;
            document.setMarkdown (buffer.text);
            history.record (kind, std::move (before), buffer.selection().end);
            notifyTextChanged();
        }
        else if (selectionBefore.start != buffer.selection().start
                 || selectionBefore.end != buffer.selection().end)
        {
            history.breakRun();
        }

        const auto& io = ImGui::GetIO();
        const bool shortcut = ! notepad::acceptsTextInput (
            io.KeyCtrl, io.KeyAlt, io.KeySuper);
        if (editorActive && shortcut && ImGui::IsKeyPressed (ImGuiKey_Z, false))
            restoreHistory (io.KeyShift);
        else if (editorActive && shortcut && ImGui::IsKeyPressed (ImGuiKey_Y, false))
            restoreHistory (true);
        else if (editorActive && shortcut && ImGui::IsKeyPressed (ImGuiKey_B, false))
            applyInline (NotepadDocument::InlineStyle::bold, "**", "**");
        else if (editorActive && shortcut && ImGui::IsKeyPressed (ImGuiKey_I, false))
            applyInline (NotepadDocument::InlineStyle::italic, "*", "*");

        ImGui::PopStyleVar();
        ImGui::PopStyleColor (3);
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
        style.Colors[ImGuiCol_ButtonActive] = colour (0x4b3f66ff);
        style.Colors[ImGuiCol_FrameBg] = colour (0x292a33ff);
        style.Colors[ImGuiCol_FrameBgHovered] = colour (0x32343eff);
        style.Colors[ImGuiCol_FrameBgActive] = colour (0x373944ff);
        style.Colors[ImGuiCol_Header] = colour (0x4b3f66ff);
        style.Colors[ImGuiCol_HeaderHovered] = colour (0x5a4880ff);
        style.Colors[ImGuiCol_PopupBg] = colour (0x20212aff);
        style.Colors[ImGuiCol_Border] = colour (0x3a3b46ff);
        style.Colors[ImGuiCol_Text] = colour (0xe6e5eaff);
        style.Colors[ImGuiCol_TextDisabled] = colour (0x8f909aff);

        ImGui::SetNextWindowPos (ImVec2 (0.0f, 0.0f));
        ImGui::SetNextWindowSize (ImVec2 (width, height));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (0.0f, 0.0f));
        ImGui::PushStyleColor (ImGuiCol_WindowBg, colour (0x181920ff));
        ImGui::Begin ("Session Notepad", nullptr,
                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                      | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);

        ImGui::SetCursorPos (ImVec2 (22.0f, 14.0f));
        ImGui::TextUnformatted ("SESSION NOTEPAD");
        ImGui::SetCursorPos (ImVec2 (22.0f, 34.0f));
        ImGui::PushStyleColor (ImGuiCol_Text, colour (0x92939dff));
        ImGui::TextUnformatted ("Lyrics and session notes");
        ImGui::PopStyleColor();

        ImGui::SetCursorPos (ImVec2 (width - 210.0f, 16.0f));
        if (drawGlyphToggle ("##page-theme", darkDocumentPage, "☀", "☾",
                             "Light page / Dark page", 82.0f))
        {
            darkDocumentPage = ! darkDocumentPage;
            editor.setDarkPage (darkDocumentPage);
        }
        ImGui::SameLine (0.0f, 5.0f);
        if (drawGlyphToggle ("##editor-mode", markdownMode, "▤", "M↓",
                             "Document / Markdown", 100.0f))
            setMode (! markdownMode);

        ImGui::SetCursorPosY (62.0f);
        drawRibbon();

        const float statusHeight = 28.0f;
        // Tiling WMs can ignore the minimum-size hint; never hand a negative
        // extent to the editor child.
        drawEditor (std::max (0.0f, height - 62.0f - 62.0f - statusHeight));

        ImGui::PushStyleColor (ImGuiCol_ChildBg, colour (0x181920ff));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (18.0f, 6.0f));
        ImGui::BeginChild ("status", ImVec2 (0.0f, statusHeight), false,
                           ImGuiWindowFlags_NoScrollbar);
        ImGui::PushStyleColor (ImGuiCol_Text,
                              saveFailed ? colour (0xe48a8aff) : colour (0x8f909aff));
        const char* saveState = saveFailed ? "Save failed - changes are still unsaved"
                              : documentDirty && hasSessionFile
                                  ? "Unsaved changes - saved when closed"
                              : documentDirty
                                  ? "Unsaved changes - save the session to keep them"
                              : hasSessionFile ? "Saved with session  |  notepad.md"
                                               : "Untitled session - saves on first session save";
        ImGui::TextUnformatted (saveState);
        const auto summary = std::to_string (document.wordCount()) + " words  |  "
                           + std::to_string (document.documentText().size()) + " characters  |  "
                           + (markdownMode ? "Markdown" : "Document");
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
    EditorBuffer buffer;
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
    bool focusEditorNextFrame = false;
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
