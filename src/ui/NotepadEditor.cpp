#include "NotepadEditor.h"
#include "NotepadTheme.h"

#include <DearImGui/imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace duskstudio
{
namespace
{
constexpr float kScrollbarWidth = 10.0f;
constexpr float kWheelStep = 42.0f;
constexpr char kLinkPlaceholder[] = "Link text";
// The chord grammar refuses a token longer than this, so the draft cannot grow
// past what a commit would accept.
constexpr std::size_t kMaxChordDraftBytes = 16;

ImU32 colourOf (unsigned int hex)
{
    return ImGui::GetColorU32 (ImVec4 (((hex >> 24) & 0xff) / 255.0f,
                                       ((hex >> 16) & 0xff) / 255.0f,
                                       ((hex >> 8) & 0xff) / 255.0f,
                                       (hex & 0xff) / 255.0f));
}

float scrollThumbHeight (float viewport, float content)
{
    return std::max (28.0f, viewport * viewport / std::max (1.0f, content));
}
} // namespace

NotepadEditor::NotepadEditor (NotepadDocument& documentToEdit, notepad::UndoStack& undoHistory)
    : document (documentToEdit), history (undoHistory)
{
    measure = [this] (std::size_t begin, std::size_t end, float fontSize,
                      NotepadDocument::BlockStyle block)
    {
        return measureRange (begin, end, fontSize, block);
    };
}

void NotepadEditor::setFonts (const Fonts& value)
{
    fonts = value;
    layoutDirty = true;
}

void NotepadEditor::reset (NotepadDocument::Selection value)
{
    setSelection (value);
    sticky = {};
    desiredX = -1.0f;
    caretAtRowEnd = false;
    draggingSelection = false;
    draggingScrollbar = false;
    layoutDirty = true;
    scrollToCaret = true;
    resetBlink();
}

void NotepadEditor::setSelection (NotepadDocument::Selection value)
{
    // A restored selection can carry offsets from a different projection, so it
    // is snapped: a caret inside a codepoint splits the sequence on the next
    // edit and truncates a copy.
    const auto& text = document.documentText();
    anchor = notepad::snapToBoundary (text, value.start);
    caret = notepad::snapToBoundary (text, value.end);
}

NotepadDocument::Selection NotepadEditor::selection() const noexcept
{
    return { std::min (anchor, caret), std::max (anchor, caret) };
}

ImFont* NotepadEditor::fontForStyle (NotepadDocument::TextStyle style,
                                     NotepadDocument::BlockStyle block) const noexcept
{
    if (style.code)
        return fonts.mono;
    const bool bold = style.bold || notepad::isHeading (block);
    if (bold && style.italic)
        return fonts.boldItalic;
    if (bold)
        return fonts.bold;
    if (style.italic)
        return fonts.italic;
    return fonts.body;
}

float NotepadEditor::measureRange (std::size_t begin, std::size_t end, float fontSize,
                                   NotepadDocument::BlockStyle block) const
{
    const auto& text = document.documentText();
    begin = std::min (begin, text.size());
    end = std::min (end, text.size());

    float width = 0.0f;
    while (begin < end)
    {
        const auto style = document.styleAt (begin);
        auto runEnd = notepad::nextOffset (text, begin);
        while (runEnd < end && document.styleAt (runEnd) == style)
            runEnd = notepad::nextOffset (text, runEnd);
        runEnd = std::min (runEnd, end);
        if (auto* const font = fontForStyle (style, block))
            width += font->CalcTextSizeA (fontSize, FLT_MAX, 0.0f,
                                          text.data() + begin, text.data() + runEnd).x;
        begin = runEnd;
    }
    return width;
}

void NotepadEditor::ensureLayout (float width, float bodySize)
{
    if (! layoutDirty && std::abs (width - layoutWidth) < 0.5f
        && std::abs (bodySize - layoutBodySize) < 0.01f)
        return;

    layout = notepad::buildLayout (document, width, bodySize, measure,
                                   chordEditing ? chordAnchor : std::string::npos);
    layoutWidth = width;
    layoutBodySize = bodySize;
    layoutDirty = false;
}

// Every mutation path ends here, once, after all of its model operations: the
// host caches the Markdown this hands out, so a notification that races a
// follow-up edit (a sticky wrapper, a continued list marker) would strand it.
void NotepadEditor::documentMutated()
{
    layoutDirty = true;
    scrollToCaret = true;
    resetBlink();
    const auto& text = document.documentText();
    caret = notepad::snapToBoundary (text, caret);
    anchor = notepad::snapToBoundary (text, anchor);
    if (onDocumentChanged)
        onDocumentChanged();
}

notepad::Snapshot NotepadEditor::snapshot() const
{
    return { document.markdown(), selection(), false };
}

void NotepadEditor::resetBlink()
{
    if (ImGui::GetCurrentContext() != nullptr)
        blinkStart = ImGui::GetTime();
}

void NotepadEditor::moveCaret (std::size_t offset, bool extend, bool preferRowEnd)
{
    caret = std::min (offset, document.documentText().size());
    if (! extend)
        anchor = caret;
    caretAtRowEnd = preferRowEnd;
    desiredX = -1.0f;
    sticky = {};
    scrollToCaret = true;
    resetBlink();
    history.breakRun();
}

void NotepadEditor::moveVertical (int direction, bool extend, float bodySize)
{
    ensureLayout (layoutWidth, bodySize);
    if (layout.rows.empty())
        return;

    const auto rowIndex = layout.rowForOffset (caret, caretAtRowEnd);
    const auto x = desiredX >= 0.0f
                 ? desiredX
                 : notepad::offsetX (document, layout.rows[rowIndex], caret, bodySize, measure);

    if ((direction < 0 && rowIndex == 0)
        || (direction > 0 && rowIndex + 1 == layout.rows.size()))
    {
        moveCaret (direction < 0 ? 0 : document.documentText().size(), extend);
        return;
    }

    const auto targetIndex = static_cast<std::size_t> (static_cast<int> (rowIndex) + direction);
    const auto& row = layout.rows[targetIndex];
    const auto offset = notepad::offsetAtX (document, row, x, bodySize, measure);
    moveCaret (offset, extend, offset == row.end && ! row.lastRowOfLine);
    desiredX = x;
}

void NotepadEditor::movePage (int direction, bool extend, float viewportHeight, float bodySize)
{
    ensureLayout (layoutWidth, bodySize);
    if (layout.rows.empty())
        return;

    const auto rowIndex = layout.rowForOffset (caret, caretAtRowEnd);
    const auto x = desiredX >= 0.0f
                 ? desiredX
                 : notepad::offsetX (document, layout.rows[rowIndex], caret, bodySize, measure);
    const auto targetY = layout.rows[rowIndex].y
                       + static_cast<float> (direction) * std::max (40.0f, viewportHeight - 24.0f);
    const auto& row = layout.rows[layout.rowAtY (std::max (0.0f, targetY))];
    const auto offset = notepad::offsetAtX (document, row, x, bodySize, measure);
    moveCaret (offset, extend, offset == row.end && ! row.lastRowOfLine);
    desiredX = x;
    scrollY += static_cast<float> (direction) * std::max (40.0f, viewportHeight - 24.0f);
}

void NotepadEditor::selectAll()
{
    anchor = 0;
    caret = document.documentText().size();
    caretAtRowEnd = false;
    desiredX = -1.0f;
    sticky = {};
    scrollToCaret = true;
    history.breakRun();
}

float NotepadEditor::caretViewportOffset() const noexcept
{
    if (layout.rows.empty())
        return 0.0f;
    return layout.rows[layout.rowForOffset (caret, caretAtRowEnd)].y - scrollY;
}

void NotepadEditor::keepCaretAtViewportOffset (float offset) noexcept
{
    pendingCaretViewport = offset;
    caretViewportPending = true;
}

void NotepadEditor::keepCaretVisible (float viewportHeight, float bodySize)
{
    ensureLayout (layoutWidth, bodySize);
    if (layout.rows.empty())
        return;

    const auto& row = layout.rows[layout.rowForOffset (caret, caretAtRowEnd)];
    if (row.y < scrollY)
        scrollY = row.y;
    else if (row.y + row.height > scrollY + viewportHeight)
        scrollY = row.y + row.height - viewportHeight;
}

bool NotepadEditor::replaceRange (std::size_t start, std::size_t end, const std::string& insert,
                                  notepad::EditKind kind)
{
    if (start > end)
        std::swap (start, end);

    auto text = document.documentText();
    start = std::min (start, text.size());
    end = std::min (end, text.size());
    if (start == end && insert.empty())
        return false;

    auto before = snapshot();
    text.replace (start, end - start, insert);
    if (! document.replaceDocumentText (text))
    {
        // The model refused the edit to keep the Markdown intact; the editor
        // must fall back onto the projection it still holds.
        reset (selection());
        return false;
    }

    const auto caretAfter = start + insert.size();
    history.record (kind, std::move (before), caretAfter);
    caret = anchor = std::min (caretAfter, document.documentText().size());
    caretAtRowEnd = false;
    desiredX = -1.0f;
    return true;
}

void NotepadEditor::insertText (const std::string& value)
{
    if (value.empty())
        return;

    const auto target = selection();
    if (! replaceRange (target.start, target.end, value, notepad::EditKind::typing))
        return;

    if (sticky.active)
        applyStickyStyles ({ target.start, target.start + value.size() });
    documentMutated();
}

void NotepadEditor::applyStickyStyles (NotepadDocument::Selection inserted)
{
    document.setDocumentInlineStyle (inserted, NotepadDocument::InlineStyle::bold, sticky.bold);
    document.setDocumentInlineStyle (inserted, NotepadDocument::InlineStyle::italic, sticky.italic);
    document.setDocumentInlineStyle (inserted, NotepadDocument::InlineStyle::code, sticky.code);
}

void NotepadEditor::insertNewline()
{
    const auto target = selection();
    const auto& text = document.documentText();
    const auto info = document.lineInfoAt (target.start);
    const bool emptyLine = notepad::lineStartOffset (text, target.start)
                        == notepad::lineEndOffset (text, target.start);

    if (target.empty() && emptyLine && notepad::isList (info.block))
    {
        // Enter on an empty list item leaves the list instead of stacking
        // another marker onto a line the user did not fill in.
        auto before = snapshot();
        document.setDocumentBlockStyle (target, NotepadDocument::BlockStyle::body);
        history.record (notepad::EditKind::structural, std::move (before), target.start);
        sticky = {};
        documentMutated();
        return;
    }

    if (! replaceRange (target.start, target.end, "\n", notepad::EditKind::structural))
        return;

    const auto newLine = caret;
    // The line that the new break ends, not the byte before it: on a blank line
    // that byte is the terminator of the line above, whose style is not ours.
    const auto previous = document.lineInfoAt (notepad::lineStartOffset (text, newLine - 1));
    if (notepad::isList (previous.block))
    {
        document.setDocumentBlockStyle ({ newLine, newLine }, previous.block);
        if (previous.block == NotepadDocument::BlockStyle::numbers)
            document.renumberOrderedRunAt (newLine);
    }
    sticky = {};
    documentMutated();
}

void NotepadEditor::deleteBackward (bool wholeWord)
{
    const auto target = selection();
    if (! target.empty())
    {
        if (replaceRange (target.start, target.end, {}, notepad::EditKind::deleting))
            documentMutated();
        return;
    }
    if (caret == 0)
        return;

    const auto& text = document.documentText();
    const auto from = wholeWord ? notepad::wordLeft (text, caret)
                                : notepad::previousOffset (text, caret);
    if (replaceRange (from, caret, {}, notepad::EditKind::deleting))
        documentMutated();
}

void NotepadEditor::deleteForward (bool wholeWord)
{
    const auto target = selection();
    if (! target.empty())
    {
        if (replaceRange (target.start, target.end, {}, notepad::EditKind::deleting))
            documentMutated();
        return;
    }

    const auto& text = document.documentText();
    if (caret >= text.size())
        return;

    const auto to = wholeWord ? notepad::wordRight (text, caret)
                              : notepad::nextOffset (text, caret);
    if (replaceRange (caret, to, {}, notepad::EditKind::deleting))
        documentMutated();
}

void NotepadEditor::toggleTaskLine (std::size_t lineStart)
{
    auto before = snapshot();
    document.toggleTaskAt (lineStart);
    history.record (notepad::EditKind::structural, std::move (before), caret);
    documentMutated();
}

bool& NotepadEditor::stickyValue (NotepadDocument::InlineStyle style) noexcept
{
    switch (style)
    {
        case NotepadDocument::InlineStyle::italic: return sticky.italic;
        case NotepadDocument::InlineStyle::code:   return sticky.code;
        case NotepadDocument::InlineStyle::bold:   break;
    }
    return sticky.bold;
}

void NotepadEditor::toggleSticky (NotepadDocument::InlineStyle style)
{
    if (! sticky.active)
    {
        const auto current = document.styleAt (caret);
        sticky = { true, current.bold, current.italic, current.code };
    }
    auto& value = stickyValue (style);
    value = ! value;
}

void NotepadEditor::applyInlineStyle (NotepadDocument::InlineStyle style)
{
    const auto target = selection();
    if (target.empty())
    {
        toggleSticky (style);
        focusRequested = true;
        return;
    }

    auto before = snapshot();
    const bool enable = ! document.selectionHasInlineStyle (target, style);
    document.setDocumentInlineStyle (target, style, enable);
    history.record (notepad::EditKind::structural, std::move (before), target.end);
    focusRequested = true;
    documentMutated();
}

void NotepadEditor::applyBlockStyle (NotepadDocument::BlockStyle style)
{
    const auto target = selection();
    if (blockStyle() == style)
        style = NotepadDocument::BlockStyle::body;

    auto before = snapshot();
    document.setDocumentBlockStyle (target, style);
    if (style == NotepadDocument::BlockStyle::numbers)
        document.renumberOrderedRunAt (target.start);
    history.record (notepad::EditKind::structural, std::move (before), target.end);
    sticky = {};
    focusRequested = true;
    documentMutated();
}

void NotepadEditor::insertLink (const std::string& url)
{
    auto target = selection();
    auto before = snapshot();

    if (target.empty())
    {
        auto text = document.documentText();
        text.insert (target.start, kLinkPlaceholder);
        if (! document.replaceDocumentText (text))
        {
            reset (target);
            return;
        }
        target.end = target.start + sizeof (kLinkPlaceholder) - 1;
    }

    document.wrapDocumentSelection (
        target, "[", "](" + notepad::encodeMarkdownLinkTarget (url) + ")");
    history.record (notepad::EditKind::structural, std::move (before), target.end);
    setSelection (target);
    sticky = {};
    focusRequested = true;
    documentMutated();
}

bool NotepadEditor::inlineStyleActive (NotepadDocument::InlineStyle style) const
{
    if (sticky.active && selection().empty())
    {
        switch (style)
        {
            case NotepadDocument::InlineStyle::bold:   return sticky.bold;
            case NotepadDocument::InlineStyle::italic: return sticky.italic;
            case NotepadDocument::InlineStyle::code:   return sticky.code;
        }
    }
    return document.selectionHasInlineStyle (selection(), style);
}

NotepadDocument::BlockStyle NotepadEditor::blockStyle() const
{
    return document.blockStyleForSelection (selection());
}

std::size_t NotepadEditor::hitTest (ImVec2 position, ImVec2 frameMin, float bodySize) const
{
    if (layout.rows.empty())
        return 0;
    const auto& row = layout.rows[layout.rowAtY (std::max (0.0f, position.y - frameMin.y + scrollY))];
    return notepad::offsetAtX (document, row, position.x - frameMin.x - row.indent,
                               bodySize, measure);
}

void NotepadEditor::handleMouse (ImVec2 frameMin, ImVec2 frameMax, bool hovered, float bodySize)
{
    auto& io = ImGui::GetIO();
    const auto viewport = std::max (1.0f, frameMax.y - frameMin.y);
    const auto maxScroll = std::max (0.0f, layout.contentHeight - viewport);

    if (hovered && io.MouseWheel != 0.0f)
        scrollY = std::clamp (scrollY - io.MouseWheel * kWheelStep, 0.0f, maxScroll);

    if (draggingScrollbar || (hovered && maxScroll > 0.0f
                              && ImGui::IsMouseClicked (ImGuiMouseButton_Left)
                              && io.MousePos.x >= frameMax.x - kScrollbarWidth))
    {
        if (ImGui::IsMouseDown (ImGuiMouseButton_Left))
        {
            draggingScrollbar = true;
            const auto thumb = scrollThumbHeight (viewport, layout.contentHeight);
            const auto travel = std::max (1.0f, viewport - thumb);
            scrollY = std::clamp ((io.MousePos.y - frameMin.y - thumb * 0.5f) / travel * maxScroll,
                                  0.0f, maxScroll);
        }
        else
        {
            draggingScrollbar = false;
        }
        return;
    }

    if (layout.rows.empty())
        return;

    const auto& text = document.documentText();
    if (hovered)
    {
        const auto over = hitTest (io.MousePos, frameMin, bodySize);
        const bool onLink = over < text.size() && ! document.linkTargetAt (over).empty();
        ImGui::SetMouseCursor (onLink ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_TextInput);
    }

    if (hovered && ImGui::IsMouseClicked (ImGuiMouseButton_Left))
    {
        const auto clicks = io.MouseClickedCount[ImGuiMouseButton_Left];
        const auto& row = layout.rows[layout.rowAtY (
            std::max (0.0f, io.MousePos.y - frameMin.y + scrollY))];

        if (clicks == 1 && row.firstRowOfLine
            && row.block == NotepadDocument::BlockStyle::tasks
            && io.MousePos.x >= frameMin.x && io.MousePos.x <= frameMin.x + row.indent - 4.0f)
        {
            toggleTaskLine (row.lineStart);
            return;
        }

        // A click in the chord band edits the chord it landed on instead of
        // moving the caret into the lyric underneath.
        const auto rowTop = frameMin.y + row.y - scrollY;
        if (clicks == 1 && row.chordTop > 0.0f && io.MousePos.y < rowTop + row.chordTop)
        {
            const auto over = notepad::offsetAtX (document, row,
                                                  io.MousePos.x - frameMin.x - row.indent,
                                                  bodySize, measure);
            for (const auto& chord : document.chords())
            {
                if (chord.documentOffset < row.start
                    || chord.documentOffset > row.end
                    || (chord.documentOffset == row.end && ! row.lastRowOfLine))
                    continue;
                const auto reach = chord.documentOffset + chord.name.size();
                if (over >= chord.documentOffset && over <= reach)
                {
                    moveCaret (chord.documentOffset, false);
                    beginChordEntry();
                    return;
                }
            }
        }

        const auto offset = hitTest (io.MousePos, frameMin, bodySize);
        if (clicks == 1 && (io.KeyCtrl || io.KeySuper) && offset < text.size())
        {
            const auto target = document.linkTargetAt (offset);
            if (! target.empty())
            {
                if (onLinkActivated)
                    onLinkActivated (target);
                return;
            }
        }

        if (clicks >= 3)
        {
            anchor = row.lineStart;
            moveCaret (row.lineEnd, true, true);
        }
        else if (clicks == 2)
        {
            const auto word = notepad::wordAt (text, offset);
            anchor = word.start;
            moveCaret (word.end, true);
        }
        else if (io.KeyShift)
        {
            moveCaret (offset, true, offset == row.end && ! row.lastRowOfLine);
            draggingSelection = true;
        }
        else
        {
            moveCaret (offset, false, offset == row.end && ! row.lastRowOfLine);
            draggingSelection = true;
        }
    }
    else if (draggingSelection && ImGui::IsMouseDown (ImGuiMouseButton_Left))
    {
        const auto offset = hitTest (io.MousePos, frameMin, bodySize);
        if (offset != caret)
            moveCaret (offset, true);

        if (io.MousePos.y < frameMin.y)
            scrollY = std::max (0.0f, scrollY - kWheelStep * 0.5f);
        else if (io.MousePos.y > frameMax.y)
            scrollY = std::min (maxScroll, scrollY + kWheelStep * 0.5f);
    }

    if (ImGui::IsMouseReleased (ImGuiMouseButton_Left))
        draggingSelection = false;
}

void NotepadEditor::handleKeyboard (float viewportHeight, float bodySize)
{
    if (handleChordEntry())
        return;

    auto& io = ImGui::GetIO();
    const auto& text = document.documentText();
    const bool acceptsText = notepad::acceptsTextInput (io.KeyCtrl, io.KeyAlt, io.KeySuper);
    const bool command = ! acceptsText;
    const bool shift = io.KeyShift;
    const bool byWord = io.KeyCtrl || io.KeyAlt;

    if (command && ImGui::IsKeyPressed (ImGuiKey_Z, false))
    {
        if (shift)
        {
            if (onRedoRequested) onRedoRequested();
        }
        else if (onUndoRequested)
        {
            onUndoRequested();
        }
        return;
    }
    if (command && ImGui::IsKeyPressed (ImGuiKey_Y, false))
    {
        if (onRedoRequested)
            onRedoRequested();
        return;
    }
    if (command && ImGui::IsKeyPressed (ImGuiKey_A, false))
    {
        selectAll();
        return;
    }
    if (command && ImGui::IsKeyPressed (ImGuiKey_K, false))
    {
        beginChordEntry();
        return;
    }
    if (command && ImGui::IsKeyPressed (ImGuiKey_B, false))
    {
        applyInlineStyle (NotepadDocument::InlineStyle::bold);
        return;
    }
    if (command && ImGui::IsKeyPressed (ImGuiKey_I, false))
    {
        applyInlineStyle (NotepadDocument::InlineStyle::italic);
        return;
    }
    if (command && (ImGui::IsKeyPressed (ImGuiKey_C, false) || ImGui::IsKeyPressed (ImGuiKey_X, false)))
    {
        const auto target = selection();
        if (! target.empty())
        {
            ImGui::SetClipboardText (text.substr (target.start, target.end - target.start).c_str());
            if (ImGui::IsKeyPressed (ImGuiKey_X, false)
                && replaceRange (target.start, target.end, {}, notepad::EditKind::structural))
                documentMutated();
        }
        return;
    }
    if (command && ImGui::IsKeyPressed (ImGuiKey_V, false))
    {
        if (const auto* const clipboard = ImGui::GetClipboardText())
        {
            std::string pasted;
            for (const auto* character = clipboard; *character != '\0'; ++character)
                if (*character != '\r')
                    pasted.push_back (*character);
            if (! pasted.empty())
            {
                const auto target = selection();
                if (replaceRange (target.start, target.end, pasted,
                                  notepad::EditKind::structural))
                    documentMutated();
            }
        }
        return;
    }

    if (ImGui::IsKeyPressed (ImGuiKey_LeftArrow))
    {
        const auto target = selection();
        if (! shift && ! target.empty() && ! byWord)
            moveCaret (target.start, false);
        else
            moveCaret (byWord ? notepad::wordLeft (text, caret)
                              : notepad::previousOffset (text, caret), shift);
    }
    else if (ImGui::IsKeyPressed (ImGuiKey_RightArrow))
    {
        const auto target = selection();
        if (! shift && ! target.empty() && ! byWord)
            moveCaret (target.end, false);
        else
            moveCaret (byWord ? notepad::wordRight (text, caret)
                              : notepad::nextOffset (text, caret), shift);
    }
    else if (ImGui::IsKeyPressed (ImGuiKey_UpArrow))
    {
        moveVertical (-1, shift, bodySize);
    }
    else if (ImGui::IsKeyPressed (ImGuiKey_DownArrow))
    {
        moveVertical (1, shift, bodySize);
    }
    else if (ImGui::IsKeyPressed (ImGuiKey_PageUp))
    {
        movePage (-1, shift, viewportHeight, bodySize);
    }
    else if (ImGui::IsKeyPressed (ImGuiKey_PageDown))
    {
        movePage (1, shift, viewportHeight, bodySize);
    }
    else if (ImGui::IsKeyPressed (ImGuiKey_Home))
    {
        ensureLayout (layoutWidth, bodySize);
        const auto& row = layout.rows[layout.rowForOffset (caret, caretAtRowEnd)];
        moveCaret (command ? 0 : row.start, shift);
    }
    else if (ImGui::IsKeyPressed (ImGuiKey_End))
    {
        ensureLayout (layoutWidth, bodySize);
        const auto& row = layout.rows[layout.rowForOffset (caret, caretAtRowEnd)];
        moveCaret (command ? text.size() : row.end, shift, true);
    }

    if (ImGui::IsKeyPressed (ImGuiKey_Backspace))
        deleteBackward (byWord);
    if (ImGui::IsKeyPressed (ImGuiKey_Delete))
        deleteForward (byWord);
    if (ImGui::IsKeyPressed (ImGuiKey_Enter) || ImGui::IsKeyPressed (ImGuiKey_KeypadEnter))
        insertNewline();
    // Match Markdown mode's ImGuiInputTextFlags_AllowTabInput semantics.
    if (! io.KeyCtrl && ! io.KeyAlt && ! io.KeySuper && ! shift
        && ImGui::IsKeyPressed (ImGuiKey_Tab))
        insertText ("\t");

    if (io.InputQueueCharacters.Size > 0)
    {
        std::string typed;
        if (acceptsText)
        {
            for (int i = 0; i < io.InputQueueCharacters.Size; ++i)
            {
                const auto character = static_cast<unsigned int> (io.InputQueueCharacters[i]);
                if (character < 0x20u || character == 0x7fu)
                    continue;
                notepad::appendUtf8 (typed, character);
            }
        }
        io.InputQueueCharacters.resize (0);
        insertText (typed);
    }
}

void NotepadEditor::beginChordEntry()
{
    // Chords land on syllables, so the slot anchors on the word under the
    // caret rather than the caret itself: click anywhere in "morning" and the
    // chord sits over its first letter.
    const auto& text = document.documentText();
    const auto word = notepad::wordAt (text, caret);
    chordAnchor = word.start < word.end ? word.start : caret;
    chordDraft = document.chordAt (chordAnchor);
    chordEditing = true;
    layoutDirty = true;
    resetBlink();
}

void NotepadEditor::insertSectionMarker (const std::string& label)
{
    auto before = snapshot();
    if (! document.insertSectionMarker (selection().start, label))
        return;

    history.breakRun();
    history.record (notepad::EditKind::structural, std::move (before), caret);
    focusRequested = true;
    documentMutated();
}

void NotepadEditor::commitChordEntry()
{
    const auto before = snapshot();
    if (! document.setChordAt (chordAnchor, chordDraft))
        return;

    history.breakRun();
    history.record (notepad::EditKind::structural, before, caret);
    documentMutated();
    chordEditing = false;
    chordDraft.clear();
    layoutDirty = true;
}

bool NotepadEditor::handleChordEntry()
{
    if (! chordEditing)
        return false;

    auto& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed (ImGuiKey_Escape))
    {
        chordEditing = false;
        chordDraft.clear();
        layoutDirty = true;
        io.InputQueueCharacters.resize (0);
        return true;
    }
    if (ImGui::IsKeyPressed (ImGuiKey_Enter) || ImGui::IsKeyPressed (ImGuiKey_KeypadEnter))
    {
        commitChordEntry();
        io.InputQueueCharacters.resize (0);
        return true;
    }
    if (ImGui::IsKeyPressed (ImGuiKey_Backspace) && ! chordDraft.empty())
        chordDraft.pop_back();

    for (int i = 0; i < io.InputQueueCharacters.Size; ++i)
    {
        const auto character = static_cast<unsigned int> (io.InputQueueCharacters[i]);
        if (character < 0x20u || character == 0x7fu)
            continue;
        std::string candidate;
        notepad::appendUtf8 (candidate, character);
        if (chordDraft.size() + candidate.size() <= kMaxChordDraftBytes)
            chordDraft += candidate;
    }
    io.InputQueueCharacters.resize (0);
    resetBlink();
    return true;
}

void NotepadEditor::transposeChords (int semitones)
{
    if (! document.hasChords() || semitones == 0)
        return;

    const auto before = snapshot();
    document.transposeChords (semitones, document.prefersFlats());
    history.breakRun();
    history.record (notepad::EditKind::structural, before, caret);
    documentMutated();
}

void NotepadEditor::render (ImVec2 frameMin, ImVec2 frameMax, float bodySize, bool active) const
{
    auto* const drawList = ImGui::GetWindowDrawList();
    const auto& text = document.documentText();
    const auto target = selection();

    const auto& theme = notepad::kStagePalette;
    const auto textColour = colourOf (theme.lyric);
    const auto mutedColour = colourOf (theme.muted);
    // Links and code spans are demoted furniture: they read through weight and
    // underline rather than a colour of their own, so the panel keeps exactly
    // one accent and it belongs to the chord lane.
    const auto linkColour = colourOf (theme.lyric);
    const auto codeColour = colourOf (theme.lyric);
    const auto selectionColour = colourOf (notepad::withAlpha (theme.muted, 0x66));
    const auto codeBackground = colourOf (notepad::withAlpha (theme.rule, 0xcc));
    const auto accentColour = colourOf (theme.muted);
    const auto chordColour = colourOf (theme.chord);

    const bool caretVisible = active
        && std::fmod (ImGui::GetTime() - blinkStart, 1.06) < 0.56;
    const auto caretRow = layout.rowForOffset (caret, caretAtRowEnd);

    drawList->PushClipRect (frameMin, frameMax, true);
    for (std::size_t rowIndex = 0; rowIndex < layout.rows.size(); ++rowIndex)
    {
        const auto& row = layout.rows[rowIndex];
        const float rowY = frameMin.y + row.y - scrollY;
        if (rowY + row.height < frameMin.y || rowY > frameMax.y)
            continue;

        const auto fontSize = notepad::blockFontSize (row.block, bodySize);
        // The chord band is part of the row: everything below it - text,
        // selection, markers, caret - hangs off textTop, not the row origin.
        const float textTop = rowY + row.chordTop;
        const float textHeight = row.height - row.chordTop;
        // Inset from the row top by a share of the glyph size, not of the row:
        // centring inside the row makes a heading's baseline depend on its row
        // height, so the title jumped when the source view drew the same line
        // at body metrics.
        const float textY = textTop + fontSize * 0.17f;
        const float originX = frameMin.x + row.indent;

        const auto selectedStart = std::max (target.start, row.start);
        const auto selectedEnd = std::min (target.end, row.end);
        // A selection that runs past a hard line break also covers the newline,
        // which has no glyph - show it as a stub past the last character.
        const bool newlineSelected = row.lastRowOfLine && target.start <= row.end
                                  && target.end > row.end;
        if (selectedStart < selectedEnd || newlineSelected)
        {
            const auto x1 = originX + notepad::offsetX (document, row, selectedStart,
                                                        bodySize, measure);
            const auto x2 = originX + notepad::offsetX (document, row, selectedEnd,
                                                        bodySize, measure)
                          + (newlineSelected ? 6.0f : 0.0f);
            drawList->AddRectFilled (ImVec2 (x1, textTop + 1.0f),
                                     ImVec2 (std::max (x1 + 2.0f, x2), rowY + row.height - 1.0f),
                                     selectionColour, 2.0f);
        }

        // A section reads as a quiet label against an accent rule: the chart's
        // structure should be findable without competing with the lyric.
        if (row.lineInfo.section && row.firstRowOfLine)
            drawList->AddRectFilled (ImVec2 (originX - 12.0f, textTop + 1.0f),
                                     ImVec2 (originX - 9.0f, rowY + row.height - 1.0f),
                                     chordColour, 1.0f);

        if (row.firstRowOfLine)
        {
            const float markerRight = originX - 9.0f;
            const float markerY = textTop + textHeight * 0.53f;
            if (row.block == NotepadDocument::BlockStyle::quote)
                drawList->AddRectFilled (ImVec2 (frameMin.x + 3.0f, textTop + 1.0f),
                                         ImVec2 (frameMin.x + 6.0f, rowY + row.height - 1.0f),
                                         accentColour, 1.0f);
            else if (row.block == NotepadDocument::BlockStyle::bullets)
                drawList->AddCircleFilled (ImVec2 (markerRight - 4.0f, markerY), 3.0f, textColour);
            else if (row.block == NotepadDocument::BlockStyle::numbers && fonts.body != nullptr)
            {
                const auto marker = std::to_string (row.lineInfo.orderedNumber) + ".";
                const auto width = fonts.body->CalcTextSizeA (bodySize, FLT_MAX, 0.0f,
                                                              marker.c_str()).x;
                drawList->AddText (fonts.body, bodySize,
                                   ImVec2 (markerRight - width, textY), mutedColour,
                                   marker.c_str());
            }
            else if (row.block == NotepadDocument::BlockStyle::tasks)
            {
                const ImVec2 boxMin (markerRight - 14.0f, markerY - 7.0f);
                const ImVec2 boxMax (markerRight, markerY + 7.0f);
                drawList->AddRect (boxMin, boxMax, mutedColour, 2.0f, 0, 1.5f);
                if (row.lineInfo.taskChecked)
                {
                    drawList->AddLine (ImVec2 (boxMin.x + 3.0f, markerY),
                                       ImVec2 (boxMin.x + 6.0f, boxMax.y - 3.0f),
                                       accentColour, 2.0f);
                    drawList->AddLine (ImVec2 (boxMin.x + 6.0f, boxMax.y - 3.0f),
                                       ImVec2 (boxMax.x - 2.0f, boxMin.y + 3.0f),
                                       accentColour, 2.0f);
                }
            }
        }

        float x = originX;
        for (auto runStart = row.start; runStart < row.end;)
        {
            const auto style = document.styleAt (runStart);
            auto runEnd = notepad::nextOffset (text, runStart);
            while (runEnd < row.end && document.styleAt (runEnd) == style)
                runEnd = notepad::nextOffset (text, runEnd);
            runEnd = std::min (runEnd, row.end);

            auto* const font = fontForStyle (style, row.block);
            const auto runWidth = measureRange (runStart, runEnd, fontSize, row.block);
            if (style.code)
                drawList->AddRectFilled (ImVec2 (x - 2.0f, textTop + 2.0f),
                                         ImVec2 (x + runWidth + 2.0f, rowY + row.height - 2.0f),
                                         codeBackground, 2.0f);
            if (font != nullptr)
                drawList->AddText (font, fontSize, ImVec2 (x, textY),
                                   row.lineInfo.section ? mutedColour
                                   : style.link ? linkColour
                                   : style.code ? codeColour : textColour,
                                   text.data() + runStart, text.data() + runEnd);
            if (style.link)
                drawList->AddLine (ImVec2 (x, textY + fontSize + 1.0f),
                                   ImVec2 (x + runWidth, textY + fontSize + 1.0f),
                                   linkColour, 1.0f);
            x += runWidth;
            runStart = runEnd;
        }

        const bool slotOnThisRow = chordEditing && chordAnchor >= row.start
                                && (chordAnchor < row.end
                                    || (chordAnchor == row.end && row.lastRowOfLine));
        if ((row.chordTop > 0.0f || slotOnThisRow) && fonts.bold != nullptr)
        {
            const auto chordSize = std::max (10.0f, bodySize
                * (notepad::kTypeScale.chord / notepad::kTypeScale.lyric));
            const float bandTop = rowY + std::max (row.chordTop,
                                                   notepad::chordBandHeight (bodySize))
                                - chordSize - 1.0f;
            if (slotOnThisRow)
            {
                const auto slotX = originX
                    + notepad::offsetX (document, row, std::min (chordAnchor, row.end),
                                        bodySize, measure);
                const auto width = std::max (26.0f,
                                             fonts.bold->CalcTextSizeA (chordSize, FLT_MAX, 0.0f,
                                                                        chordDraft.c_str()).x + 8.0f);
                drawList->AddRectFilled (ImVec2 (slotX - 3.0f, bandTop - 2.0f),
                                         ImVec2 (slotX + width, bandTop + chordSize + 2.0f),
                                         codeBackground, 3.0f);
                drawList->AddRect (ImVec2 (slotX - 3.0f, bandTop - 2.0f),
                                   ImVec2 (slotX + width, bandTop + chordSize + 2.0f),
                                   chordColour, 3.0f, 0, 1.2f);
                if (! chordDraft.empty())
                    drawList->AddText (fonts.bold, chordSize, ImVec2 (slotX, bandTop),
                                       chordColour, chordDraft.c_str());
                if (caretVisible)
                {
                    const auto caretX = slotX
                        + fonts.bold->CalcTextSizeA (chordSize, FLT_MAX, 0.0f,
                                                     chordDraft.c_str()).x + 1.0f;
                    drawList->AddLine (ImVec2 (caretX, bandTop),
                                       ImVec2 (caretX, bandTop + chordSize), chordColour, 1.2f);
                }
            }
            for (const auto& chord : document.chords())
            {
                if (chordEditing && chord.documentOffset == chordAnchor)
                    continue;
                if (chord.documentOffset < row.start
                    || (chord.documentOffset > row.end
                        || (chord.documentOffset == row.end && ! row.lastRowOfLine)))
                    continue;

                const auto anchorX = originX
                    + notepad::offsetX (document, row,
                                        std::min (chord.documentOffset, row.end),
                                        bodySize, measure);
                drawList->AddText (fonts.bold, chordSize, ImVec2 (anchorX, bandTop),
                                   chordColour, chord.name.c_str());
            }
        }

        if (caretVisible && rowIndex == caretRow)
        {
            const auto caretX = originX + notepad::offsetX (document, row, caret, bodySize, measure);
            drawList->AddLine (ImVec2 (caretX, textTop + 2.0f),
                               ImVec2 (caretX, rowY + row.height - 2.0f), textColour, 1.4f);
        }
    }

    const auto viewport = frameMax.y - frameMin.y;
    if (layout.contentHeight > viewport)
    {
        const auto thumb = scrollThumbHeight (viewport, layout.contentHeight);
        const auto travel = viewport - thumb;
        const auto thumbY = frameMin.y + travel * scrollY
                          / std::max (1.0f, layout.contentHeight - viewport);
        drawList->AddRectFilled (ImVec2 (frameMax.x - 4.0f, thumbY),
                                 ImVec2 (frameMax.x - 1.0f, thumbY + thumb),
                                 colourOf (0xaaa7b0aa), 2.0f);
    }
    drawList->PopClipRect();
}

void NotepadEditor::draw (float bodySize)
{
    auto& g = *ImGui::GetCurrentContext();
    auto* const host = ImGui::GetCurrentWindow();
    if (host->SkipItems)
        return;

    const auto origin = ImGui::GetCursorScreenPos();
    const auto available = ImGui::GetContentRegionAvail();
    const ImVec2 size (std::max (32.0f, available.x), std::max (32.0f, available.y));
    const ImVec2 frameMin = origin;
    const ImVec2 frameMax (origin.x + size.x, origin.y + size.y);
    const ImRect frame (frameMin, frameMax);

    const auto id = host->GetID ("##notepad-document");
    ImGui::ItemSize (size);
    if (! ImGui::ItemAdd (frame, id, &frame, ImGuiItemFlags_Inputable))
        return;

    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsMouseClicked (ImGuiMouseButton_Left);

    // A modal (the link dialog) owns the keyboard while it is up; hand the
    // focus back only once it has closed and asked for it.
    if (ImGui::GetTopMostPopupModal() != nullptr)
    {
        if (g.ActiveId == id)
            ImGui::ClearActiveID();
        ensureLayout (size.x, bodySize);
        scrollY = std::clamp (scrollY, 0.0f, std::max (0.0f, layout.contentHeight - size.y));
        render (frameMin, frameMax, bodySize, false);
        return;
    }

    // The page is the only text surface in the window, so it takes the caret
    // back once a toolbar button has finished with the active id - otherwise
    // every format button would cost the user a click to resume typing.
    // Reclaiming focus refocuses the host window, which dismisses any popup
    // over it: a toolbar menu would open and shut on the same frame.
    const bool popupOpen = ImGui::IsPopupOpen (nullptr, ImGuiPopupFlags_AnyPopupId
                                                        | ImGuiPopupFlags_AnyPopupLevel);
    const bool reclaim = keepFocus && g.ActiveId == 0 && ! clicked && ! popupOpen;

    if (focusRequested || reclaim || (hovered && clicked))
    {
        if (g.ActiveId != id)
        {
            ImGui::SetActiveID (id, host);
            ImGui::SetFocusID (id, host);
            ImGui::FocusWindow (host);
        }
        focusRequested = false;
        keepFocus = true;
    }
    else if (g.ActiveId == id && clicked && ! hovered)
    {
        ImGui::ClearActiveID();
        keepFocus = false;
    }

    const bool active = g.ActiveId == id;
    if (active)
    {
        // Same ownership set as ImGui's own multiline text input: without it
        // keyboard navigation eats the arrows and Tab moves focus away.
        static const ImGuiKey ownedKeys[] = {
            ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow,
            ImGuiKey_Home, ImGuiKey_End, ImGuiKey_PageUp, ImGuiKey_PageDown,
            ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Delete, ImGuiKey_Backspace,
            ImGuiKey_Tab
        };
        for (const auto key : ownedKeys)
            ImGui::SetKeyOwner (key, id);
        // The page holds the active id so typing survives a toolbar click, but
        // ItemHoverable refuses to hover any other item while a foreign id is
        // active. Without this the ribbon needed one click to drop the page's
        // id and a second to actually press the button, and tooltips never
        // appeared at all.
        g.ActiveIdAllowOverlap = true;
        g.ActiveIdUsingNavDirMask |= (1u << ImGuiDir_Left) | (1u << ImGuiDir_Right)
                                   | (1u << ImGuiDir_Up) | (1u << ImGuiDir_Down);
        g.WantTextInputNextFrame = 1;
    }
    // The page scrolls itself, so the wheel must not also scroll the child.
    ImGui::SetItemKeyOwner (ImGuiKey_MouseWheelY, ImGuiInputFlags_None);

    ensureLayout (size.x, bodySize);
    handleMouse (frameMin, frameMax, hovered, bodySize);
    if (active)
        handleKeyboard (size.y, bodySize);
    ensureLayout (size.x, bodySize);

    if (caretViewportPending)
    {
        if (! layout.rows.empty())
            scrollY = layout.rows[layout.rowForOffset (caret, caretAtRowEnd)].y
                    - pendingCaretViewport;
        caretViewportPending = false;
        scrollToCaret = false;
    }
    if (scrollToCaret)
    {
        keepCaretVisible (size.y, bodySize);
        scrollToCaret = false;
    }
    scrollY = std::clamp (scrollY, 0.0f, std::max (0.0f, layout.contentHeight - size.y));

    render (frameMin, frameMax, bodySize, active);
}
} // namespace duskstudio
