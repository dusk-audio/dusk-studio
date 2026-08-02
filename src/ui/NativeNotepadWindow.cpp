#include "NativeNotepadWindow.h"
#include "NotepadDocument.h"
#include "../foundation/MessageThread.h"

#include <Application.hpp>
#include <DearImGui.hpp>
#include <DearImGui/imgui_internal.h>
#ifndef DGL_NO_SHARED_RESOURCES
# include "src/Resources.hpp"
#endif

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
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

std::string blockPrefix (NotepadDocument::BlockStyle style, std::size_t number)
{
    switch (style)
    {
        case NotepadDocument::BlockStyle::heading1: return "# ";
        case NotepadDocument::BlockStyle::heading2: return "## ";
        case NotepadDocument::BlockStyle::heading3: return "### ";
        case NotepadDocument::BlockStyle::quote:    return "> ";
        case NotepadDocument::BlockStyle::bullets:  return "- ";
        case NotepadDocument::BlockStyle::numbers:  return std::to_string (number) + ". ";
        case NotepadDocument::BlockStyle::tasks:    return "- [ ] ";
        case NotepadDocument::BlockStyle::body:     return {};
    }
    return {};
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

bool sourceSelectionHasWrapper (const std::string& text,
                                NotepadDocument::Selection selection,
                                const std::string& prefix,
                                const std::string& suffix)
{
    selection.start = std::min (selection.start, text.size());
    selection.end = std::min (selection.end, text.size());
    if (selection.start > selection.end)
        std::swap (selection.start, selection.end);

    const bool prefixOutside = selection.start >= prefix.size()
                            && text.compare (selection.start - prefix.size(), prefix.size(), prefix) == 0;
    const bool suffixOutside = text.compare (selection.end, suffix.size(), suffix) == 0;
    const bool includesMarkers = selection.end >= selection.start + prefix.size() + suffix.size()
                              && text.compare (selection.start, prefix.size(), prefix) == 0
                              && text.compare (selection.end - suffix.size(), suffix.size(), suffix) == 0;
    return (prefixOutside && suffixOutside) || includesMarkers;
}

std::size_t nextUtf8Offset (const std::string& text, std::size_t offset,
                            std::size_t limit)
{
    offset = std::min (offset + 1, limit);
    while (offset < limit
           && (static_cast<unsigned char> (text[offset]) & 0xc0u) == 0x80u)
        ++offset;
    return offset;
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

    Impl() = default;

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

    void open (std::uintptr_t nativeParent, EmbeddedGeometry geometry,
               const std::string& markdown,
               bool sessionExists, bool unsavedChanges)
    {
        if (nativeParent == 0 || geometry.width < 2 || geometry.height < 2)
            return;

        destroyEmbeddedWindow();
        document.setMarkdown (markdown);
        markdownMode = false;
        buffer.assign (document.documentText());
        buffer.restoreSelection ({ 0, 0 });
        selectionAnchor = 0;
        selectingWithMouse = false;
        documentScrollY = 0.0f;
        closeRequested = false;
        closeWasPumped = false;
        focusEditorNextFrame = true;
        hasSessionFile = sessionExists;
        documentDirty = unsavedChanges;
        saveFailed = false;
        savedMarkdown = unsavedChanges ? std::optional<std::string> {}
                                       : std::optional<std::string> { markdown };
        typingStyleOverride = false;
        undoHistory.clear();
        redoHistory.clear();

        window = std::make_unique<DGL::Window> (
            app, nativeParent, geometry.width, geometry.height,
            geometry.scaleFactor, false);
        if (window->getNativeWindowHandle() == 0)
        {
            window.reset();
            return;
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
            return;
        }
        {
            DGL::Window::ScopedGraphicsContext context (*window);
            editorWidget = std::make_unique<EditorWidget> (*window, *this);
            buildFontAtlas (static_cast<float> (15.0 * window->getScaleFactor()));
        }
        window->focus();
        startTimer (16);
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
    struct HistoryState
    {
        std::string markdown;
        NotepadDocument::Selection selection;
        bool selectionIsMarkdown = false;
    };

    HistoryState historyState (NotepadDocument::Selection selection) const
    {
        return { document.markdown(), selection, markdownMode };
    }

    void pushUndo (NotepadDocument::Selection selection)
    {
        undoHistory.push_back (historyState (selection));
        if (undoHistory.size() > 256)
            undoHistory.erase (undoHistory.begin());
        redoHistory.clear();
    }

    NotepadDocument::Selection selectionForCurrentMode (const HistoryState& state) const
    {
        if (state.selectionIsMarkdown == markdownMode)
            return state.selection;
        return markdownMode ? document.sourceSelection (state.selection)
                            : document.documentSelection (state.selection);
    }

    void restoreHistory (std::vector<HistoryState>& from,
                         std::vector<HistoryState>& to)
    {
        if (from.empty())
            return;
        to.push_back (historyState (buffer.selection()));
        const auto state = std::move (from.back());
        from.pop_back();
        document.setMarkdown (state.markdown);
        buffer.assign (markdownMode ? document.markdown() : document.documentText());
        buffer.restoreSelection (selectionForCurrentMode (state));
        typingStyleOverride = false;
        focusEditorNextFrame = true;
        notifyTextChanged();
    }

    void timerCallback() override
    {
        app.idle();
        // The Done button is handled inside the ImGui display callback. Wait
        // until DPF returns from the event pump before destroying its embedded
        // widget and native child.
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
        const auto oldSelection = buffer.selection();
        markdownMode = useMarkdown;
        buffer.assign (markdownMode ? document.markdown() : document.documentText());
        const auto converted = markdownMode ? document.sourceSelection (oldSelection)
                                            : document.documentSelection (oldSelection);
        buffer.restoreSelection (converted);
        selectionAnchor = converted.start;
        selectingWithMouse = false;
        documentScrollY = 0.0f;
        typingStyleOverride = false;
        focusEditorNextFrame = true;
    }

    bool& typingStyleValue (NotepadDocument::InlineStyle style)
    {
        switch (style)
        {
            case NotepadDocument::InlineStyle::bold:   return typingBold;
            case NotepadDocument::InlineStyle::italic: return typingItalic;
            case NotepadDocument::InlineStyle::code:   return typingCode;
        }
        return typingBold;
    }

    void initialiseTypingStyle()
    {
        if (typingStyleOverride)
            return;
        const auto caret = buffer.selection().start;
        const auto style = document.styleAt (caret);
        typingBold = style.bold;
        typingItalic = style.italic;
        typingCode = style.code;
        typingStyleOverride = true;
    }

    void applyInline (NotepadDocument::InlineStyle inlineStyle,
                      const std::string& prefix, const std::string& suffix)
    {
        const auto selection = buffer.selection();
        if (markdownMode)
        {
            pushUndo (selection);
            auto source = document.markdown();
            source.insert (selection.end, suffix);
            source.insert (selection.start, prefix);
            document.setMarkdown (std::move (source));
            buffer.assign (document.markdown());
            buffer.restoreSelection ({ selection.start + prefix.size(),
                                       selection.end + prefix.size() });
        }
        else if (selection.empty())
        {
            initialiseTypingStyle();
            auto& value = typingStyleValue (inlineStyle);
            value = ! value;
        }
        else
        {
            pushUndo (selection);
            const bool enable = ! document.selectionHasInlineStyle (selection, inlineStyle);
            document.setDocumentInlineStyle (selection, inlineStyle, enable);
            buffer.assign (document.documentText());
            buffer.restoreSelection (selection);
            notifyTextChanged();
        }
        focusEditorNextFrame = true;
        if (markdownMode)
            notifyTextChanged();
    }

    void applyBlock (NotepadDocument::BlockStyle style)
    {
        const auto selection = buffer.selection();
        if (selectedBlockStyle() == style)
            style = NotepadDocument::BlockStyle::body;
        pushUndo (selection);
        if (! markdownMode)
        {
            document.setDocumentBlockStyle (selection, style);
            buffer.assign (document.documentText());
            buffer.restoreSelection (selection);
        }
        else
        {
            auto source = document.markdown();
            const auto first = sourceLineStart (source, selection.start);
            const auto last = sourceLineEnd (source, selection.end);
            std::vector<std::pair<std::size_t, std::size_t>> lines;
            for (auto start = first; start <= last;)
            {
                const auto end = sourceLineEnd (source, start);
                lines.emplace_back (start, end);
                if (end == source.size())
                    break;
                start = end + 1;
            }
            for (std::size_t index = lines.size(); index-- > 0;)
            {
                const auto [start, end] = lines[index];
                source.erase (start, sourceBlockPrefixLength (source, start, end));
                source.insert (start, blockPrefix (style, index + 1));
            }
            document.setMarkdown (std::move (source));
            buffer.assign (document.markdown());
            buffer.restoreSelection (selection);
        }
        focusEditorNextFrame = true;
        typingStyleOverride = false;
        notifyTextChanged();
    }

    void insertLink (const std::string& url)
    {
        auto selection = buffer.selection();
        pushUndo (selection);
        constexpr auto placeholder = "Link text";

        if (markdownMode)
        {
            auto source = document.markdown();
            const auto label = selection.empty()
                             ? std::string (placeholder)
                             : source.substr (selection.start, selection.end - selection.start);
            const auto replacement = "[" + label + "](" + url + ")";
            source.replace (selection.start, selection.end - selection.start, replacement);
            document.setMarkdown (std::move (source));
            buffer.assign (document.markdown());
            buffer.restoreSelection ({ selection.start + 1,
                                       selection.start + 1 + label.size() });
        }
        else
        {
            if (selection.empty())
            {
                auto text = document.documentText();
                text.insert (selection.start, placeholder);
                if (! document.replaceDocumentText (text))
                {
                    undoHistory.pop_back();
                    return;
                }
                selection.end = selection.start + std::strlen (placeholder);
            }
            document.wrapDocumentSelection (selection, "[", "](" + url + ")");
            buffer.assign (document.documentText());
            buffer.restoreSelection (selection);
        }

        typingStyleOverride = false;
        focusEditorNextFrame = true;
        notifyTextChanged();
    }

    bool inlineStyleActive (NotepadDocument::InlineStyle style,
                            const std::string& prefix,
                            const std::string& suffix) const
    {
        if (! markdownMode && buffer.selection().empty() && typingStyleOverride)
        {
            switch (style)
            {
                case NotepadDocument::InlineStyle::bold:   return typingBold;
                case NotepadDocument::InlineStyle::italic: return typingItalic;
                case NotepadDocument::InlineStyle::code:   return typingCode;
            }
        }
        return markdownMode
             ? sourceSelectionHasWrapper (document.markdown(), buffer.selection(), prefix, suffix)
             : document.selectionHasInlineStyle (buffer.selection(), style);
    }

    NotepadDocument::BlockStyle selectedBlockStyle() const
    {
        return markdownMode
             ? sourceBlockStyle (document.markdown(), buffer.selection())
             : document.blockStyleForSelection (buffer.selection());
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
            restoreHistory (undoHistory, redoHistory);
        ImGui::SameLine (0.0f, 5.0f);
        if (toolbarButton ("↷", "Redo (Ctrl+Y)", false))
            restoreHistory (redoHistory, undoHistory);
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

    struct VisualRow
    {
        std::size_t start = 0;
        std::size_t end = 0;
        std::size_t lineStart = 0;
        std::size_t lineEnd = 0;
        NotepadDocument::BlockStyle block = NotepadDocument::BlockStyle::body;
        NotepadDocument::LineInfo lineInfo;
        float y = 0.0f;
        float height = 0.0f;
        float textIndent = 0.0f;
        bool firstVisualRow = false;
    };

    static bool isHeading (NotepadDocument::BlockStyle style) noexcept
    {
        return style == NotepadDocument::BlockStyle::heading1
            || style == NotepadDocument::BlockStyle::heading2
            || style == NotepadDocument::BlockStyle::heading3;
    }

    float documentFontSize (NotepadDocument::BlockStyle style, float bodySize) const noexcept
    {
        switch (style)
        {
            case NotepadDocument::BlockStyle::heading1: return bodySize + 11.0f;
            case NotepadDocument::BlockStyle::heading2: return bodySize + 7.0f;
            case NotepadDocument::BlockStyle::heading3: return bodySize + 3.5f;
            default: return bodySize;
        }
    }

    ImFont* fontForStyle (NotepadDocument::TextStyle style,
                          NotepadDocument::BlockStyle block) const noexcept
    {
        if (style.code)
            return monoFont;
        const bool bold = style.bold || isHeading (block);
        if (bold && style.italic)
            return boldItalicFont;
        if (bold)
            return boldFont;
        if (style.italic)
            return italicFont;
        return bodyFont;
    }

    float documentTextWidth (std::size_t start, std::size_t end, float fontSize,
                             NotepadDocument::BlockStyle block) const
    {
        const auto& text = document.documentText();
        start = std::min (start, text.size());
        end = std::min (end, text.size());
        float width = 0.0f;
        while (start < end)
        {
            const auto style = document.styleAt (start);
            auto runEnd = nextUtf8Offset (text, start, end);
            while (runEnd < end && document.styleAt (runEnd) == style)
                runEnd = nextUtf8Offset (text, runEnd, end);
            if (auto* const font = fontForStyle (style, block))
                width += font->CalcTextSizeA (fontSize, FLT_MAX, 0.0f,
                                              text.data() + start,
                                              text.data() + runEnd).x;
            start = runEnd;
        }
        return width;
    }

    NotepadDocument::BlockStyle documentLineStyle (std::size_t lineStart) const
    {
        const auto& text = document.documentText();
        if (lineStart < text.size() && text[lineStart] != '\n')
            return document.styleAt (lineStart).block;
        return document.blockStyleForSelection ({ lineStart, lineStart });
    }

    void buildDocumentLayout (float width, float bodySize)
    {
        documentRows.clear();
        const auto& text = document.documentText();
        float y = 0.0f;
        std::size_t lineStart = 0;

        while (lineStart <= text.size())
        {
            const auto foundEnd = text.find ('\n', lineStart);
            const auto lineEnd = foundEnd == std::string::npos ? text.size() : foundEnd;
            const auto block = documentLineStyle (lineStart);
            const auto info = document.lineInfoAt (lineStart);
            const auto fontSize = documentFontSize (block, bodySize);
            const auto rowHeight = std::ceil (fontSize * (isHeading (block) ? 1.32f : 1.48f));
            const bool marked = block == NotepadDocument::BlockStyle::bullets
                             || block == NotepadDocument::BlockStyle::numbers
                             || block == NotepadDocument::BlockStyle::tasks;
            const float indent = marked ? 30.0f
                               : block == NotepadDocument::BlockStyle::quote ? 18.0f : 0.0f;
            const float wrapWidth = std::max (60.0f, width - indent - 8.0f);
            if (isHeading (block) && ! documentRows.empty())
                y += 8.0f;

            auto rowStart = lineStart;
            bool first = true;
            if (rowStart == lineEnd)
            {
                documentRows.push_back ({ rowStart, rowStart, lineStart, lineEnd,
                                          block, info, y, rowHeight, indent, true });
                y += rowHeight;
            }
            while (rowStart < lineEnd)
            {
                auto offset = rowStart;
                auto rowEnd = rowStart;
                auto lastBreak = std::string::npos;
                float used = 0.0f;
                while (offset < lineEnd)
                {
                    const auto next = nextUtf8Offset (text, offset, lineEnd);
                    const auto advance = documentTextWidth (offset, next, fontSize, block);
                    if (used + advance > wrapWidth && offset > rowStart)
                    {
                        rowEnd = lastBreak != std::string::npos && lastBreak > rowStart
                               ? lastBreak : offset;
                        break;
                    }
                    used += advance;
                    rowEnd = next;
                    if (text[offset] == ' ' || text[offset] == '\t')
                        lastBreak = next;
                    offset = next;
                }
                if (rowEnd == rowStart)
                    rowEnd = nextUtf8Offset (text, rowStart, lineEnd);
                documentRows.push_back ({ rowStart, rowEnd, lineStart, lineEnd,
                                          block, info, y, rowHeight, indent, first });
                y += rowHeight;
                rowStart = rowEnd;
                first = false;
            }

            if (isHeading (block)) y += 4.0f;
            else if (block == NotepadDocument::BlockStyle::quote) y += 2.0f;
            if (foundEnd == std::string::npos)
                break;
            lineStart = foundEnd + 1;
        }
        documentContentHeight = std::max (y, bodySize);
    }

    std::size_t rowIndexAtY (float localY) const noexcept
    {
        if (documentRows.empty())
            return 0;
        for (std::size_t i = 0; i < documentRows.size(); ++i)
            if (localY < documentRows[i].y + documentRows[i].height)
                return i;
        return documentRows.size() - 1;
    }

    std::size_t offsetAtRowX (const VisualRow& row, float localX,
                              float bodySize) const
    {
        const auto& text = document.documentText();
        const auto fontSize = documentFontSize (row.block, bodySize);
        float x = 0.0f;
        for (auto offset = row.start; offset < row.end;)
        {
            const auto next = nextUtf8Offset (text, offset, row.end);
            const auto advance = documentTextWidth (offset, next, fontSize, row.block);
            if (localX < x + advance * 0.5f)
                return offset;
            x += advance;
            offset = next;
        }
        return row.end;
    }

    std::size_t documentHitTest (ImVec2 mouse, ImVec2 rectMin,
                                 float bodySize) const
    {
        if (documentRows.empty())
            return 0;
        const auto rowIndex = rowIndexAtY (std::max (0.0f,
            mouse.y - rectMin.y + documentScrollY));
        const auto& row = documentRows[rowIndex];
        return offsetAtRowX (row, mouse.x - rectMin.x - row.textIndent, bodySize);
    }

    void handleDocumentMouse (bool hovered, ImVec2 rectMin, ImVec2 rectMax,
                              float bodySize)
    {
        auto& io = ImGui::GetIO();
        const auto viewportHeight = std::max (1.0f, rectMax.y - rectMin.y);
        if (hovered && io.MouseWheel != 0.0f)
            documentScrollY = std::clamp (documentScrollY - io.MouseWheel * 42.0f,
                                          0.0f,
                                          std::max (0.0f, documentContentHeight - viewportHeight));

        const auto hit = hovered ? documentHitTest (io.MousePos, rectMin, bodySize) : 0;
        if (hovered && hit < document.documentText().size()
            && ! document.linkTargetAt (hit).empty())
            ImGui::SetMouseCursor (ImGuiMouseCursor_Hand);

        if (hovered && ImGui::IsMouseClicked (ImGuiMouseButton_Left)
            && io.MouseClickedCount[ImGuiMouseButton_Left] == 1)
        {
            const auto rowIndex = rowIndexAtY (std::max (0.0f,
                io.MousePos.y - rectMin.y + documentScrollY));
            const auto& row = documentRows[rowIndex];

            if (row.firstVisualRow && row.block == NotepadDocument::BlockStyle::tasks
                && io.MousePos.x >= rectMin.x + 3.0f
                && io.MousePos.x <= rectMin.x + row.textIndent - 4.0f)
            {
                const auto selection = buffer.selection();
                pushUndo (selection);
                document.toggleTaskAt (row.lineStart);
                buffer.assign (document.documentText());
                buffer.restoreSelection (selection);
                notifyTextChanged();
                return;
            }

            const auto target = hit < document.documentText().size()
                              ? document.linkTargetAt (hit)
                              : std::string {};
            if ((io.KeyCtrl || io.KeySuper) && ! target.empty())
            {
                if (onLinkOpened)
                    onLinkOpened (target);
                return;
            }

            if (io.KeyShift)
            {
                if (! selectingWithMouse)
                    selectionAnchor = buffer.selection().start;
            }
            else
            {
                selectionAnchor = hit;
            }
            buffer.restoreSelection ({ selectionAnchor, hit });
            typingStyleOverride = false;
            selectingWithMouse = true;
        }
        else if (selectingWithMouse && ImGui::IsMouseDown (ImGuiMouseButton_Left))
        {
            const auto dragHit = documentHitTest (io.MousePos, rectMin, bodySize);
            buffer.restoreSelection ({ selectionAnchor, dragHit });
        }

        if (ImGui::IsMouseReleased (ImGuiMouseButton_Left))
            selectingWithMouse = false;
    }

    void moveDocumentCaretVertically (NotepadDocument::Selection before,
                                      int direction, bool extend, float bodySize)
    {
        if (documentRows.empty())
            return;
        std::size_t current = 0;
        for (std::size_t i = 0; i < documentRows.size(); ++i)
        {
            if (before.end >= documentRows[i].start && before.end <= documentRows[i].end)
            {
                current = i;
                break;
            }
        }
        const auto target = static_cast<std::size_t> (std::clamp (
            static_cast<int> (current) + direction, 0,
            static_cast<int> (documentRows.size() - 1)));
        const auto& sourceRow = documentRows[current];
        const auto& targetRow = documentRows[target];
        const auto sourceSize = documentFontSize (sourceRow.block, bodySize);
        const auto x = documentTextWidth (sourceRow.start, before.end,
                                          sourceSize, sourceRow.block);
        const auto caret = offsetAtRowX (targetRow, x, bodySize);
        if (! extend)
            selectionAnchor = caret;
        else if (before.empty())
            selectionAnchor = before.start;
        buffer.restoreSelection ({ selectionAnchor, caret });
        typingStyleOverride = false;
    }

    void keepCaretVisible (ImVec2 rectMin, ImVec2 rectMax)
    {
        const auto caret = buffer.selection().end;
        for (const auto& row : documentRows)
        {
            if (caret < row.start || caret > row.end)
                continue;
            const auto viewport = rectMax.y - rectMin.y;
            if (row.y < documentScrollY)
                documentScrollY = row.y;
            else if (row.y + row.height > documentScrollY + viewport)
                documentScrollY = row.y + row.height - viewport;
            documentScrollY = std::clamp (documentScrollY, 0.0f,
                                          std::max (0.0f, documentContentHeight - viewport));
            break;
        }
    }

    void drawRichDocument (ImVec2 rectMin, ImVec2 rectMax,
                           float bodySize, bool editorActive)
    {
        const auto& text = document.documentText();
        auto* const drawList = ImGui::GetWindowDrawList();
        const auto selection = buffer.selection();
        const auto textColour = ImGui::GetColorU32 (
            colour (darkDocumentPage ? 0xe7e8edff : 0x24242aff));
        const auto mutedColour = ImGui::GetColorU32 (
            colour (darkDocumentPage ? 0xa9abb5ff : 0x67656dff));
        const auto linkColour = ImGui::GetColorU32 (
            colour (darkDocumentPage ? 0x8eb4ffff : 0x315fbdff));
        const auto codeColour = ImGui::GetColorU32 (
            colour (darkDocumentPage ? 0xd5a4f0ff : 0x5e367cff));
        const auto selectionColour = ImGui::GetColorU32 (
            colour (darkDocumentPage ? 0x516aa6cc : 0xb9c8f0cc));
        const auto codeBackground = ImGui::GetColorU32 (
            colour (darkDocumentPage ? 0x34313fff : 0xebe6f1ff));
        const auto quoteColour = ImGui::GetColorU32 (
            colour (darkDocumentPage ? 0xb28bd4ff : 0x8061a4ff));

        drawList->PushClipRect (rectMin, rectMax, true);
        for (const auto& row : documentRows)
        {
            const float rowY = rectMin.y + row.y - documentScrollY;
            if (rowY + row.height < rectMin.y || rowY > rectMax.y)
                continue;
            const auto fontSize = documentFontSize (row.block, bodySize);
            const float textY = rowY + std::max (0.0f, (row.height - fontSize) * 0.38f);
            const float originX = rectMin.x + row.textIndent;

            const auto selectedStart = std::max (selection.start, row.start);
            const auto selectedEnd = std::min (selection.end, row.end);
            if (selectedStart < selectedEnd)
            {
                const auto x1 = originX + documentTextWidth (row.start, selectedStart,
                                                             fontSize, row.block);
                const auto x2 = originX + documentTextWidth (row.start, selectedEnd,
                                                             fontSize, row.block);
                drawList->AddRectFilled (ImVec2 (x1, rowY + 1.0f),
                                         ImVec2 (std::max (x1 + 2.0f, x2),
                                                 rowY + row.height - 1.0f),
                                         selectionColour, 2.0f);
            }

            if (row.firstVisualRow)
            {
                const float markerRight = originX - 9.0f;
                const float markerY = rowY + row.height * 0.53f;
                if (row.block == NotepadDocument::BlockStyle::quote)
                    drawList->AddRectFilled (ImVec2 (rectMin.x + 3.0f, rowY + 1.0f),
                                             ImVec2 (rectMin.x + 6.0f,
                                                     rowY + row.height - 1.0f),
                                             quoteColour, 1.0f);
                else if (row.block == NotepadDocument::BlockStyle::bullets)
                    drawList->AddCircleFilled (ImVec2 (markerRight - 4.0f, markerY),
                                               3.0f, textColour);
                else if (row.block == NotepadDocument::BlockStyle::numbers
                         && bodyFont != nullptr)
                {
                    const auto marker = std::to_string (row.lineInfo.orderedNumber) + ".";
                    const auto size = bodyFont->CalcTextSizeA (bodySize, FLT_MAX, 0.0f,
                                                               marker.c_str()).x;
                    drawList->AddText (bodyFont, bodySize,
                                       ImVec2 (markerRight - size, textY), mutedColour,
                                       marker.c_str());
                }
                else if (row.block == NotepadDocument::BlockStyle::tasks)
                {
                    const ImVec2 a (markerRight - 14.0f, markerY - 7.0f);
                    const ImVec2 b (markerRight, markerY + 7.0f);
                    drawList->AddRect (a, b, mutedColour, 2.0f, 0, 1.5f);
                    if (row.lineInfo.taskChecked)
                    {
                        drawList->AddLine (ImVec2 (a.x + 3.0f, markerY),
                                           ImVec2 (a.x + 6.0f, b.y - 3.0f),
                                           quoteColour, 2.0f);
                        drawList->AddLine (ImVec2 (a.x + 6.0f, b.y - 3.0f),
                                           ImVec2 (b.x - 2.0f, a.y + 3.0f),
                                           quoteColour, 2.0f);
                    }
                }
            }

            float x = originX;
            for (auto runStart = row.start; runStart < row.end;)
            {
                const auto runStyle = document.styleAt (runStart);
                auto runEnd = nextUtf8Offset (text, runStart, row.end);
                while (runEnd < row.end && document.styleAt (runEnd) == runStyle)
                    runEnd = nextUtf8Offset (text, runEnd, row.end);
                auto* const font = fontForStyle (runStyle, row.block);
                const auto runWidth = documentTextWidth (runStart, runEnd, fontSize, row.block);
                if (runStyle.code)
                    drawList->AddRectFilled (ImVec2 (x - 2.0f, rowY + 2.0f),
                                             ImVec2 (x + runWidth + 2.0f,
                                                     rowY + row.height - 2.0f),
                                             codeBackground, 2.0f);
                const auto colourValue = runStyle.link ? linkColour
                                       : runStyle.code ? codeColour : textColour;
                if (font != nullptr)
                    drawList->AddText (font, fontSize, ImVec2 (x, textY), colourValue,
                                       text.data() + runStart, text.data() + runEnd);
                if (runStyle.link)
                    drawList->AddLine (ImVec2 (x, textY + fontSize + 1.0f),
                                       ImVec2 (x + runWidth, textY + fontSize + 1.0f),
                                       linkColour, 1.0f);
                x += runWidth;
                runStart = runEnd;
            }

            if (editorActive && selection.empty()
                && selection.end >= row.start && selection.end <= row.end)
            {
                const auto cursorX = originX + documentTextWidth (
                    row.start, selection.end, fontSize, row.block);
                drawList->AddLine (ImVec2 (cursorX, rowY + 2.0f),
                                   ImVec2 (cursorX, rowY + row.height - 2.0f),
                                   textColour, 1.2f);
            }
        }

        const auto viewport = rectMax.y - rectMin.y;
        if (documentContentHeight > viewport)
        {
            const auto trackX = rectMax.x - 4.0f;
            const auto thumbHeight = std::max (28.0f, viewport * viewport / documentContentHeight);
            const auto travel = viewport - thumbHeight;
            const auto thumbY = rectMin.y + travel * documentScrollY
                              / std::max (1.0f, documentContentHeight - viewport);
            drawList->AddRectFilled (ImVec2 (trackX, thumbY),
                                     ImVec2 (rectMax.x - 1.0f, thumbY + thumbHeight),
                                     ImGui::GetColorU32 (colour (0xaaa7b0aa)), 2.0f);
        }
        drawList->PopClipRect();
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

        ImGui::PushStyleColor (ImGuiCol_FrameBg,
                              markdownMode ? colour (0x181922ff)
                                           : colour (darkDocumentPage
                                                        ? kDarkDocumentPaperColour
                                                        : kLightDocumentPaperColour));
        ImGui::PushStyleColor (ImGuiCol_Text,
                              markdownMode ? colour (0xd7d9e0ff) : colour (0x00000000));
        ImGui::PushStyleColor (ImGuiCol_TextSelectedBg,
                              markdownMode ? colour (0x70599aaa) : colour (0x00000000));
        ImGui::PushStyleVar (ImGuiStyleVar_FramePadding, ImVec2 (0.0f, 0.0f));

        if (focusEditorNextFrame)
        {
            ImGui::SetKeyboardFocusHere();
            focusEditorNextFrame = false;
        }

        const auto flags = ImGuiInputTextFlags_AllowTabInput
                         | ImGuiInputTextFlags_CallbackResize
                         | ImGuiInputTextFlags_CallbackAlways
                         | ImGuiInputTextFlags_NoUndoRedo
                         | (markdownMode ? 0 : ImGuiInputTextFlags_NoHorizontalScroll);
        const auto inputSize = ImGui::GetContentRegionAvail();
        const auto selectionBeforeInput = buffer.selection();
        const auto oldDocumentText = document.documentText();
        const bool changed = ImGui::InputTextMultiline (
            "##notepad-editor", buffer.text.data(), buffer.text.capacity() + 1,
            inputSize, flags, EditorBuffer::callback, &buffer);
        bool acceptedChange = changed;
        const auto editorRectMin = ImGui::GetItemRectMin();
        const auto editorRectMax = ImGui::GetItemRectMax();
        const bool editorActive = ImGui::IsItemActive();
        const bool editorHovered = ImGui::IsItemHovered();
        if (auto* const state = ImGui::GetInputTextState (ImGui::GetItemID()))
        {
            // CallbackAlways can observe the selection before the current
            // character edit is committed. The live input state is the
            // authority after InputTextMultiline returns; keeping this in sync
            // prevents the next frame from restoring a stale caret and typing
            // successive formatted characters in reverse order.
            if (state->HasSelection())
            {
                buffer.selectionStart = state->GetSelectionStart();
                buffer.selectionEnd = state->GetSelectionEnd();
            }
            else
            {
                buffer.selectionStart = buffer.selectionEnd = state->GetCursorPos();
            }
            if (! markdownMode)
                state->Scroll = {};
        }
        if (changed)
        {
            pushUndo (selectionBeforeInput);
            const auto editedSelection = buffer.selection();
            buffer.text.resize (std::strlen (buffer.text.c_str()));
            if (markdownMode)
            {
                document.setMarkdown (buffer.text);
            }
            else
            {
                std::size_t prefix = 0;
                while (prefix < oldDocumentText.size() && prefix < buffer.text.size()
                       && oldDocumentText[prefix] == buffer.text[prefix])
                    ++prefix;
                std::size_t suffix = 0;
                while (suffix < oldDocumentText.size() - prefix
                       && suffix < buffer.text.size() - prefix
                       && oldDocumentText[oldDocumentText.size() - 1 - suffix]
                          == buffer.text[buffer.text.size() - 1 - suffix])
                    ++suffix;
                const auto insertedEnd = buffer.text.size() - suffix;
                const auto inserted = buffer.text.substr (prefix, insertedEnd - prefix);

                const auto oldLineStart = selectionBeforeInput.start == 0
                                        ? std::string::npos
                                        : oldDocumentText.rfind (
                                              '\n', selectionBeforeInput.start - 1);
                const auto lineStart = oldLineStart == std::string::npos ? 0 : oldLineStart + 1;
                const auto oldLineEnd = oldDocumentText.find ('\n', selectionBeforeInput.start);
                const auto lineEnd = oldLineEnd == std::string::npos
                                   ? oldDocumentText.size() : oldLineEnd;
                const auto oldBlock = document.lineInfoAt (selectionBeforeInput.start).block;
                const bool listBlock = oldBlock == NotepadDocument::BlockStyle::bullets
                                    || oldBlock == NotepadDocument::BlockStyle::numbers
                                    || oldBlock == NotepadDocument::BlockStyle::tasks;
                const bool exitEmptyList = selectionBeforeInput.empty()
                                        && inserted == "\n" && lineStart == lineEnd
                                        && listBlock;

                if (exitEmptyList)
                {
                    document.setDocumentBlockStyle (selectionBeforeInput,
                                                    NotepadDocument::BlockStyle::body);
                    buffer.assign (document.documentText());
                    buffer.restoreSelection (selectionBeforeInput);
                }
                else if (! document.replaceDocumentText (buffer.text))
                {
                    undoHistory.pop_back();
                    buffer.assign (document.documentText());
                    buffer.restoreSelection (selectionBeforeInput);
                    focusEditorNextFrame = true;
                    acceptedChange = false;
                }
                else
                {
                    if (typingStyleOverride && insertedEnd > prefix)
                    {
                        const NotepadDocument::Selection insertedSelection { prefix, insertedEnd };
                        document.setDocumentInlineStyle (
                            insertedSelection, NotepadDocument::InlineStyle::bold, typingBold);
                        document.setDocumentInlineStyle (
                            insertedSelection, NotepadDocument::InlineStyle::italic, typingItalic);
                        document.setDocumentInlineStyle (
                            insertedSelection, NotepadDocument::InlineStyle::code, typingCode);
                    }

                    for (std::size_t i = 0; i < inserted.size(); ++i)
                    {
                        if (inserted[i] != '\n')
                            continue;
                        const auto newLine = prefix + i + 1;
                        const auto previousOffset = newLine > 1 ? newLine - 2 : 0;
                        const auto previous = document.lineInfoAt (previousOffset);
                        if (previous.block == NotepadDocument::BlockStyle::bullets
                            || previous.block == NotepadDocument::BlockStyle::numbers
                            || previous.block == NotepadDocument::BlockStyle::tasks)
                        {
                            document.setDocumentBlockStyle ({ newLine, newLine }, previous.block);
                            if (previous.block == NotepadDocument::BlockStyle::numbers)
                                document.renumberOrderedRunAt (newLine);
                        }
                    }
                    buffer.assign (document.documentText());
                    buffer.restoreSelection (editedSelection);
                }
            }
            if (acceptedChange)
                notifyTextChanged();
        }
        else if (selectionBeforeInput.start != buffer.selection().start
                 || selectionBeforeInput.end != buffer.selection().end)
        {
            typingStyleOverride = false;
        }

        const auto& io = ImGui::GetIO();
        const bool shortcut = io.KeyCtrl || io.KeySuper;
        if (editorActive && shortcut && ImGui::IsKeyPressed (ImGuiKey_Z, false))
        {
            if (io.KeyShift)
                restoreHistory (redoHistory, undoHistory);
            else
                restoreHistory (undoHistory, redoHistory);
        }
        else if (editorActive && shortcut && ImGui::IsKeyPressed (ImGuiKey_Y, false))
        {
            restoreHistory (redoHistory, undoHistory);
        }
        else if (editorActive && shortcut && ImGui::IsKeyPressed (ImGuiKey_B, false))
        {
            applyInline (NotepadDocument::InlineStyle::bold, "**", "**");
        }
        else if (editorActive && shortcut && ImGui::IsKeyPressed (ImGuiKey_I, false))
        {
            applyInline (NotepadDocument::InlineStyle::italic, "*", "*");
        }

        if (! markdownMode)
        {
            buildDocumentLayout (editorRectMax.x - editorRectMin.x,
                                 ImGui::GetFontSize());
            if (editorActive && ImGui::IsKeyPressed (ImGuiKey_UpArrow, false))
                moveDocumentCaretVertically (selectionBeforeInput, -1, io.KeyShift,
                                             ImGui::GetFontSize());
            else if (editorActive && ImGui::IsKeyPressed (ImGuiKey_DownArrow, false))
                moveDocumentCaretVertically (selectionBeforeInput, 1, io.KeyShift,
                                             ImGui::GetFontSize());

            handleDocumentMouse (editorHovered, editorRectMin, editorRectMax,
                                 ImGui::GetFontSize());
            if (changed || ImGui::IsKeyPressed (ImGuiKey_UpArrow, false)
                        || ImGui::IsKeyPressed (ImGuiKey_DownArrow, false))
                keepCaretVisible (editorRectMin, editorRectMax);

            drawRichDocument (editorRectMin, editorRectMax,
                              ImGui::GetFontSize(), editorActive);
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor (3);
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

        ImGui::SetCursorPos (ImVec2 (width - 300.0f, 16.0f));
        if (drawGlyphToggle ("##page-theme", darkDocumentPage, "☀", "☾",
                             "Light page / Dark page", 82.0f))
            darkDocumentPage = ! darkDocumentPage;
        ImGui::SameLine (0.0f, 5.0f);
        if (drawGlyphToggle ("##editor-mode", markdownMode, "▤", "M↓",
                             "Document / Markdown", 100.0f))
            setMode (! markdownMode);
        ImGui::SameLine (0.0f, 12.0f);
        ImGui::PushStyleColor (ImGuiCol_Button, colour (0x305a82ff));
        if (ImGui::Button ("Done", ImVec2 (76.0f, 30.0f)))
            close();
        ImGui::PopStyleColor();

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
                                  ? "Unsaved changes - saved when Done"
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
    std::vector<HistoryState> undoHistory;
    std::vector<HistoryState> redoHistory;
    std::vector<VisualRow> documentRows;
    std::optional<std::string> savedMarkdown;
    bool markdownMode = false;
    bool darkDocumentPage = true;
    bool closeRequested = false;
    bool closeWasPumped = false;
    bool focusEditorNextFrame = false;
    bool selectingWithMouse = false;
    bool typingStyleOverride = false;
    bool typingBold = false;
    bool typingItalic = false;
    bool typingCode = false;
    bool hasSessionFile = false;
    bool documentDirty = false;
    bool saveFailed = false;
    std::size_t selectionAnchor = 0;
    float documentScrollY = 0.0f;
    float documentContentHeight = 0.0f;
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

void NativeNotepadWindow::open (std::uintptr_t nativeParent, EmbeddedGeometry geometry,
                                const std::string& markdown,
                                bool hasSessionFile, bool hasUnsavedChanges)
{
    impl->open (nativeParent, geometry, markdown, hasSessionFile, hasUnsavedChanges);
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
