#include "NotepadEditorCore.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace duskstudio
{
namespace notepad
{
namespace
{
bool isContinuation (const std::string& text, std::size_t offset) noexcept
{
    return (static_cast<unsigned char> (text[offset]) & 0xc0u) == 0x80u;
}

enum class CharClass
{
    word,
    space,
    punctuation,
    newline
};

CharClass classify (const std::string& text, std::size_t offset) noexcept
{
    const auto byte = static_cast<unsigned char> (text[offset]);
    if (byte == '\n' || byte == '\r')
        return CharClass::newline;
    if (byte == ' ' || byte == '\t')
        return CharClass::space;
    if (byte >= 0x80 || byte == '_' || (byte >= '0' && byte <= '9')
        || (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z'))
        return CharClass::word;
    return CharClass::punctuation;
}

NotepadDocument::Selection normaliseSelection (
    const std::string& text, NotepadDocument::Selection selection) noexcept
{
    selection.start = std::min (selection.start, text.size());
    selection.end = std::min (selection.end, text.size());
    if (selection.start > selection.end)
        std::swap (selection.start, selection.end);
    return selection;
}

std::size_t markdownBlockPrefixLength (const std::string& text,
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

std::string markdownBlockPrefix (NotepadDocument::BlockStyle style, std::size_t number)
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
} // namespace

std::size_t nextOffset (const std::string& text, std::size_t offset) noexcept
{
    if (offset >= text.size())
        return text.size();
    ++offset;
    while (offset < text.size() && isContinuation (text, offset))
        ++offset;
    return offset;
}

std::size_t previousOffset (const std::string& text, std::size_t offset) noexcept
{
    offset = std::min (offset, text.size());
    if (offset == 0)
        return 0;
    --offset;
    while (offset > 0 && isContinuation (text, offset))
        --offset;
    return offset;
}

std::size_t snapToBoundary (const std::string& text, std::size_t offset) noexcept
{
    offset = std::min (offset, text.size());
    while (offset > 0 && offset < text.size() && isContinuation (text, offset))
        --offset;
    return offset;
}

std::size_t lineStartOffset (const std::string& text, std::size_t offset) noexcept
{
    offset = std::min (offset, text.size());
    const auto found = offset == 0 ? std::string::npos : text.rfind ('\n', offset - 1);
    return found == std::string::npos ? 0 : found + 1;
}

std::size_t lineEndOffset (const std::string& text, std::size_t offset) noexcept
{
    const auto found = text.find ('\n', std::min (offset, text.size()));
    return found == std::string::npos ? text.size() : found;
}

std::size_t wordLeft (const std::string& text, std::size_t offset) noexcept
{
    offset = std::min (offset, text.size());
    while (offset > 0)
    {
        const auto previous = previousOffset (text, offset);
        const auto category = classify (text, previous);
        if (category != CharClass::space && category != CharClass::newline)
            break;
        offset = previous;
        if (category == CharClass::newline)
            return offset;
    }
    if (offset == 0)
        return 0;
    const auto category = classify (text, previousOffset (text, offset));
    while (offset > 0)
    {
        const auto previous = previousOffset (text, offset);
        if (classify (text, previous) != category)
            break;
        offset = previous;
    }
    return offset;
}

std::size_t wordRight (const std::string& text, std::size_t offset) noexcept
{
    offset = std::min (offset, text.size());
    if (offset < text.size() && classify (text, offset) == CharClass::newline)
        return nextOffset (text, offset);

    const auto category = offset < text.size() ? classify (text, offset) : CharClass::space;
    while (offset < text.size() && classify (text, offset) == category
           && category != CharClass::space)
        offset = nextOffset (text, offset);
    while (offset < text.size() && classify (text, offset) == CharClass::space)
        offset = nextOffset (text, offset);
    return offset;
}

Range wordAt (const std::string& text, std::size_t offset) noexcept
{
    offset = std::min (offset, text.size());
    if (text.empty())
        return { 0, 0 };

    auto probe = offset;
    if (probe == text.size() || classify (text, probe) == CharClass::newline)
    {
        if (probe == 0)
            return { probe, probe };
        const auto previous = previousOffset (text, probe);
        if (classify (text, previous) == CharClass::newline)
            return { probe, probe };
        probe = previous;
    }

    const auto category = classify (text, probe);
    auto start = probe;
    while (start > 0)
    {
        const auto previous = previousOffset (text, start);
        if (classify (text, previous) != category)
            break;
        start = previous;
    }
    auto end = probe;
    while (end < text.size() && classify (text, end) == category)
        end = nextOffset (text, end);
    return { start, end };
}

void appendUtf8 (std::string& text, unsigned int codepoint)
{
    if (codepoint < 0x80u)
    {
        text.push_back (static_cast<char> (codepoint));
    }
    else if (codepoint < 0x800u)
    {
        text.push_back (static_cast<char> (0xc0u | (codepoint >> 6)));
        text.push_back (static_cast<char> (0x80u | (codepoint & 0x3fu)));
    }
    else if (codepoint < 0x10000u)
    {
        text.push_back (static_cast<char> (0xe0u | (codepoint >> 12)));
        text.push_back (static_cast<char> (0x80u | ((codepoint >> 6) & 0x3fu)));
        text.push_back (static_cast<char> (0x80u | (codepoint & 0x3fu)));
    }
    else if (codepoint <= 0x10ffffu)
    {
        text.push_back (static_cast<char> (0xf0u | (codepoint >> 18)));
        text.push_back (static_cast<char> (0x80u | ((codepoint >> 12) & 0x3fu)));
        text.push_back (static_cast<char> (0x80u | ((codepoint >> 6) & 0x3fu)));
        text.push_back (static_cast<char> (0x80u | (codepoint & 0x3fu)));
    }
}

bool acceptsTextInput (bool control, bool alt, bool super) noexcept
{
    return ! super && (! control || alt);
}

std::string encodeMarkdownLinkTarget (const std::string& target)
{
    static constexpr char hexDigits[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve (target.size());
    for (const auto ch : target)
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

bool markdownInlineActive (const std::string& markdown,
                           NotepadDocument::Selection selection,
                           const std::string& prefix,
                           const std::string& suffix)
{
    if (prefix.empty() && suffix.empty())
        return false;

    selection = normaliseSelection (markdown, selection);
    const bool prefixOutside = selection.start >= prefix.size()
                            && markdown.compare (selection.start - prefix.size(),
                                                 prefix.size(), prefix) == 0;
    const bool suffixOutside = markdown.compare (selection.end, suffix.size(), suffix) == 0;
    const bool includesMarkers = selection.end >= selection.start + prefix.size() + suffix.size()
                              && markdown.compare (selection.start, prefix.size(), prefix) == 0
                              && markdown.compare (selection.end - suffix.size(),
                                                   suffix.size(), suffix) == 0;
    return (prefixOutside && suffixOutside) || includesMarkers;
}

MarkdownTransform toggleMarkdownInline (const std::string& markdown,
                                        NotepadDocument::Selection selection,
                                        const std::string& prefix,
                                        const std::string& suffix)
{
    selection = normaliseSelection (markdown, selection);
    MarkdownTransform result { markdown, selection };
    if (prefix.empty() && suffix.empty())
        return result;

    const bool wrappersOutside = selection.start >= prefix.size()
                              && markdown.compare (selection.start - prefix.size(),
                                                   prefix.size(), prefix) == 0
                              && markdown.compare (selection.end, suffix.size(), suffix) == 0;
    const bool markersIncluded = selection.end >= selection.start + prefix.size() + suffix.size()
                              && markdown.compare (selection.start, prefix.size(), prefix) == 0
                              && markdown.compare (selection.end - suffix.size(),
                                                   suffix.size(), suffix) == 0;

    if (wrappersOutside)
    {
        result.markdown.erase (selection.end, suffix.size());
        result.markdown.erase (selection.start - prefix.size(), prefix.size());
        result.selection = { selection.start - prefix.size(),
                             selection.end - prefix.size() };
    }
    else if (markersIncluded)
    {
        result.markdown.erase (selection.end - suffix.size(), suffix.size());
        result.markdown.erase (selection.start, prefix.size());
        result.selection = { selection.start,
                             selection.end - prefix.size() - suffix.size() };
    }
    else
    {
        result.markdown.insert (selection.end, suffix);
        result.markdown.insert (selection.start, prefix);
        result.selection = { selection.start + prefix.size(),
                             selection.end + prefix.size() };
    }
    return result;
}

MarkdownTransform setMarkdownBlockStyle (const std::string& markdown,
                                         NotepadDocument::Selection selection,
                                         NotepadDocument::BlockStyle style)
{
    selection = normaliseSelection (markdown, selection);

    struct PrefixEdit
    {
        std::size_t start = 0;
        std::size_t oldLength = 0;
        std::string replacement;
    };

    std::vector<PrefixEdit> edits;
    const auto firstLine = lineStartOffset (markdown, selection.start);
    const auto finalProbe = selection.end > selection.start ? selection.end - 1
                                                            : selection.end;
    const auto lastLine = lineStartOffset (markdown, finalProbe);
    for (auto start = firstLine, number = std::size_t { 1 };; ++number)
    {
        const auto end = lineEndOffset (markdown, start);
        edits.push_back ({ start, markdownBlockPrefixLength (markdown, start, end),
                           markdownBlockPrefix (style, number) });
        if (start == lastLine || end == markdown.size())
            break;
        start = end + 1;
    }

    const auto mapOffset = [&edits] (std::size_t offset)
    {
        std::ptrdiff_t delta = 0;
        for (const auto& edit : edits)
        {
            if (offset < edit.start)
                break;

            const auto oldEnd = edit.start + edit.oldLength;
            const auto replacementLength = edit.replacement.size();
            if (edit.oldLength == 0 && offset == edit.start)
                return static_cast<std::size_t> (
                    static_cast<std::ptrdiff_t> (edit.start + replacementLength) + delta);
            if (offset <= oldEnd)
            {
                const auto within = offset == oldEnd
                                  ? replacementLength
                                  : std::min (offset - edit.start, replacementLength);
                return static_cast<std::size_t> (
                    static_cast<std::ptrdiff_t> (edit.start + within) + delta);
            }
            delta += static_cast<std::ptrdiff_t> (replacementLength)
                   - static_cast<std::ptrdiff_t> (edit.oldLength);
        }
        return static_cast<std::size_t> (static_cast<std::ptrdiff_t> (offset) + delta);
    };

    MarkdownTransform result { markdown,
                               { mapOffset (selection.start), mapOffset (selection.end) } };
    for (auto edit = edits.rbegin(); edit != edits.rend(); ++edit)
        result.markdown.replace (edit->start, edit->oldLength, edit->replacement);
    return result;
}

bool isHeading (NotepadDocument::BlockStyle block) noexcept
{
    return block == NotepadDocument::BlockStyle::heading1
        || block == NotepadDocument::BlockStyle::heading2
        || block == NotepadDocument::BlockStyle::heading3;
}

bool isList (NotepadDocument::BlockStyle block) noexcept
{
    return block == NotepadDocument::BlockStyle::bullets
        || block == NotepadDocument::BlockStyle::numbers
        || block == NotepadDocument::BlockStyle::tasks;
}

float blockFontSize (NotepadDocument::BlockStyle block, float bodySize) noexcept
{
    switch (block)
    {
        case NotepadDocument::BlockStyle::heading1: return bodySize + 11.0f;
        case NotepadDocument::BlockStyle::heading2: return bodySize + 7.0f;
        case NotepadDocument::BlockStyle::heading3: return bodySize + 3.5f;
        default: return bodySize;
    }
}

float blockRowHeight (NotepadDocument::BlockStyle block, float bodySize) noexcept
{
    const auto fontSize = blockFontSize (block, bodySize);
    return std::ceil (fontSize * (isHeading (block) ? 1.32f : 1.48f));
}

float blockIndent (NotepadDocument::BlockStyle block) noexcept
{
    if (isList (block))
        return 30.0f;
    return block == NotepadDocument::BlockStyle::quote ? 18.0f : 0.0f;
}

float chordBandHeight (float bodySize) noexcept
{
    return std::max (12.0f, bodySize * 1.05f);
}

namespace
{
// A chord anchored exactly on a row's end belongs to that row only when the
// row closes its line: at a soft wrap the anchor is the following row's first
// character, and drawing it twice would double the chord.
bool inRow (std::size_t offset, std::size_t start, std::size_t end, bool lastRowOfLine) noexcept
{
    return offset >= start && (offset < end || (lastRowOfLine && offset == end));
}

bool rowCarriesChord (const NotepadDocument& document, std::size_t start, std::size_t end,
                      bool lastRowOfLine, std::size_t pendingChordAnchor) noexcept
{
    if (pendingChordAnchor != std::string::npos
        && inRow (pendingChordAnchor, start, end, lastRowOfLine))
        return true;
    for (const auto& chord : document.chords())
        if (inRow (chord.documentOffset, start, end, lastRowOfLine))
            return true;
    return false;
}
} // namespace

ChromeBands layoutChrome (float windowHeight, float headerHeight, float ribbonHeight,
                          float statusHeight) noexcept
{
    ChromeBands bands;
    windowHeight = std::max (0.0f, windowHeight);
    bands.header = std::min (headerHeight, windowHeight);
    bands.ribbon = std::min (ribbonHeight, windowHeight - bands.header);
    bands.status = std::min (statusHeight,
                             windowHeight - bands.header - bands.ribbon);
    bands.chart = windowHeight - bands.header - bands.ribbon - bands.status;
    return bands;
}

Layout buildLayout (const NotepadDocument& document, float width, float bodySize,
                    const MeasureFn& measure, std::size_t pendingChordAnchor)
{
    Layout layout;
    const auto& text = document.documentText();
    const bool anyChords = document.hasChords() || pendingChordAnchor != std::string::npos;
    const auto chordBand = chordBandHeight (bodySize);
    float y = 0.0f;
    std::size_t lineStart = 0;

    while (lineStart <= text.size())
    {
        const auto found = text.find ('\n', lineStart);
        const auto lineEnd = found == std::string::npos ? text.size() : found;
        const auto info = document.lineInfoAt (lineStart);
        const auto fontSize = blockFontSize (info.block, bodySize);
        const auto rowHeight = blockRowHeight (info.block, bodySize);
        const auto indent = blockIndent (info.block);
        const auto wrapWidth = std::max (60.0f, width - indent - 8.0f);

        if (isHeading (info.block) && ! layout.rows.empty())
            y += 8.0f;

        const auto firstRowIndex = layout.rows.size();
        auto rowStart = lineStart;
        if (rowStart == lineEnd)
        {
            const auto band = anyChords
                           && rowCarriesChord (document, rowStart, rowStart, true,
                                               pendingChordAnchor)
                            ? chordBand : 0.0f;
            layout.rows.push_back ({ rowStart, rowStart, lineStart, lineEnd, info.block,
                                     info, y, rowHeight + band, indent, true, true, band });
            y += rowHeight + band;
        }
        while (rowStart < lineEnd)
        {
            auto offset = rowStart;
            auto rowEnd = rowStart;
            auto lastBreak = std::string::npos;
            float used = 0.0f;
            while (offset < lineEnd)
            {
                const auto next = nextOffset (text, offset);
                const auto advance = measure (offset, std::min (next, lineEnd), fontSize,
                                              info.block);
                if (used + advance > wrapWidth && offset > rowStart)
                {
                    rowEnd = lastBreak != std::string::npos && lastBreak > rowStart
                           ? lastBreak : offset;
                    break;
                }
                used += advance;
                rowEnd = std::min (next, lineEnd);
                if (text[offset] == ' ' || text[offset] == '\t')
                    lastBreak = rowEnd;
                offset = rowEnd;
            }
            if (rowEnd == rowStart)
                rowEnd = std::min (nextOffset (text, rowStart), lineEnd);
            const bool lastRow = rowEnd == lineEnd;
            const auto band = anyChords
                           && rowCarriesChord (document, rowStart, rowEnd, lastRow,
                                               pendingChordAnchor)
                            ? chordBand : 0.0f;
            layout.rows.push_back ({ rowStart, rowEnd, lineStart, lineEnd, info.block, info,
                                     y, rowHeight + band, indent,
                                     layout.rows.size() == firstRowIndex, lastRow, band });
            y += rowHeight + band;
            rowStart = rowEnd;
        }

        if (isHeading (info.block))
            y += 4.0f;
        else if (info.block == NotepadDocument::BlockStyle::quote)
            y += 2.0f;

        if (found == std::string::npos)
            break;
        lineStart = found + 1;
    }

    layout.contentHeight = std::max (y, bodySize);
    return layout;
}

std::size_t Layout::rowForOffset (std::size_t offset, bool preferRowEnd) const noexcept
{
    if (rows.empty())
        return 0;

    std::size_t match = 0;
    bool found = false;
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        if (rows[i].start > offset)
            break;
        if (offset > rows[i].end)
            continue;
        match = i;
        found = true;
        if (offset < rows[i].end || preferRowEnd)
            break;
    }
    return found ? match : rows.size() - 1;
}

std::size_t Layout::rowAtY (float y) const noexcept
{
    if (rows.empty())
        return 0;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (y < rows[i].y + rows[i].height)
            return i;
    return rows.size() - 1;
}

float offsetX (const NotepadDocument& document, const Row& row, std::size_t offset,
               float bodySize, const MeasureFn& measure)
{
    const auto& text = document.documentText();
    const auto fontSize = blockFontSize (row.block, bodySize);
    offset = std::clamp (offset, row.start, std::min (row.end, text.size()));

    float x = 0.0f;
    for (auto runStart = row.start; runStart < offset;)
    {
        const auto style = document.styleAt (runStart);
        auto runEnd = nextOffset (text, runStart);
        while (runEnd < offset && document.styleAt (runEnd) == style)
            runEnd = nextOffset (text, runEnd);
        runEnd = std::min (runEnd, offset);
        x += measure (runStart, runEnd, fontSize, row.block);
        runStart = runEnd;
    }
    return x;
}

std::size_t offsetAtX (const NotepadDocument& document, const Row& row, float localX,
                       float bodySize, const MeasureFn& measure)
{
    const auto& text = document.documentText();
    const auto fontSize = blockFontSize (row.block, bodySize);
    float x = 0.0f;
    for (auto offset = row.start; offset < row.end;)
    {
        const auto next = std::min (nextOffset (text, offset), row.end);
        const auto advance = measure (offset, next, fontSize, row.block);
        if (localX < x + advance * 0.5f)
            return offset;
        x += advance;
        offset = next;
    }
    return row.end;
}

void UndoStack::clear() noexcept
{
    undoEntries.clear();
    redoEntries.clear();
    runBroken = true;
}

void UndoStack::breakRun() noexcept
{
    runBroken = true;
}

void UndoStack::record (EditKind kind, Snapshot before, std::size_t caretAfter)
{
    redoEntries.clear();

    const bool coalesce = ! runBroken
                       && kind != EditKind::structural
                       && ! undoEntries.empty()
                       && undoEntries.back().kind == kind
                       && undoEntries.back().state.selectionIsSource == before.selectionIsSource
                       && undoEntries.back().caretAfter == before.selection.start
                       && before.selection.empty();
    runBroken = kind == EditKind::structural;

    if (coalesce)
    {
        undoEntries.back().caretAfter = caretAfter;
        return;
    }

    undoEntries.push_back ({ kind, std::move (before), caretAfter });
    if (undoEntries.size() > kMaxEntries)
        undoEntries.erase (undoEntries.begin());
}

bool UndoStack::undo (const Snapshot& current, Snapshot& restored)
{
    if (undoEntries.empty())
        return false;

    redoEntries.push_back ({ EditKind::structural, current, current.selection.end });
    if (redoEntries.size() > kMaxEntries)
        redoEntries.erase (redoEntries.begin());
    restored = std::move (undoEntries.back().state);
    undoEntries.pop_back();
    runBroken = true;
    return true;
}

bool UndoStack::redo (const Snapshot& current, Snapshot& restored)
{
    if (redoEntries.empty())
        return false;

    undoEntries.push_back ({ EditKind::structural, current, current.selection.end });
    if (undoEntries.size() > kMaxEntries)
        undoEntries.erase (undoEntries.begin());
    restored = std::move (redoEntries.back().state);
    redoEntries.pop_back();
    runBroken = true;
    return true;
}
} // namespace notepad
} // namespace duskstudio
