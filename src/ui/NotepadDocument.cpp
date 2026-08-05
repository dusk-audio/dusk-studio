#include "NotepadDocument.h"
#include "NotepadChords.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>

namespace duskstudio
{
namespace
{
struct ProjectionBuilder
{
    explicit ProjectionBuilder (const std::string& sourceText)
        : source (sourceText)
    {
        boundaries.push_back (0);
    }

    void skipTo (std::size_t sourceOffset)
    {
        boundaries.back() = std::min (sourceOffset, source.size());
    }

    void appendSourceRange (std::size_t start, std::size_t end,
                            NotepadDocument::TextStyle style = {},
                            const std::string& linkTarget = {})
    {
        style.block = blockStyle;
        end = std::min (end, source.size());
        for (auto i = start; i < end; ++i)
        {
            skipTo (i);
            text.push_back (source[i]);
            styles.push_back (style);
            links.push_back (linkTarget);
            boundaries.push_back (i + 1);
        }
    }

    const std::string& source;
    std::string text;
    std::vector<std::size_t> boundaries;
    std::vector<NotepadDocument::TextStyle> styles;
    std::vector<std::string> links;
    std::vector<NotepadDocument::Chord> chords;
    std::vector<std::pair<std::size_t, std::size_t>> chordSpans;
    NotepadDocument::BlockStyle blockStyle = NotepadDocument::BlockStyle::body;
};

std::size_t findUnescaped (const std::string& text,
                           const std::string& needle,
                           std::size_t from,
                           std::size_t limit)
{
    while (from < limit)
    {
        const auto found = text.find (needle, from);
        if (found == std::string::npos || found >= limit)
            return std::string::npos;
        if (found == 0 || text[found - 1] != '\\')
            return found;
        from = found + needle.size();
    }
    return std::string::npos;
}

void projectInline (ProjectionBuilder& out, std::size_t start, std::size_t end,
                    NotepadDocument::TextStyle style = {},
                    const std::string& linkTarget = {})
{
    const auto& source = out.source;
    auto i = start;

    while (i < end)
    {
        if (source[i] == '\\' && i + 1 < end)
        {
            out.skipTo (i + 1);
            out.appendSourceRange (i + 1, i + 2, style, linkTarget);
            i += 2;
            continue;
        }

        // CommonMark's compact combined form must be recognized before the
        // two-star bold delimiter. Otherwise ***Heading*** is consumed as a
        // bold run containing a literal leading star, leaving the final star
        // visible in Document mode.
        if (i + 2 < end && source.compare (i, 3, "***") == 0)
        {
            const auto close = findUnescaped (source, "***", i + 3, end);
            if (close != std::string::npos)
            {
                out.skipTo (i + 3);
                auto combinedStyle = style;
                combinedStyle.bold = true;
                combinedStyle.italic = true;
                projectInline (out, i + 3, close, combinedStyle, linkTarget);
                out.skipTo (close + 3);
                i = close + 3;
                continue;
            }
        }

        if (i + 1 < end && source.compare (i, 2, "**") == 0)
        {
            const auto close = findUnescaped (source, "**", i + 2, end);
            if (close != std::string::npos)
            {
                out.skipTo (i + 2);
                auto boldStyle = style;
                boldStyle.bold = true;
                projectInline (out, i + 2, close, boldStyle, linkTarget);
                out.skipTo (close + 2);
                i = close + 2;
                continue;
            }
        }

        if (source[i] == '*' && i + 1 < end)
        {
            const auto close = findUnescaped (source, "*", i + 1, end);
            if (close != std::string::npos)
            {
                out.skipTo (i + 1);
                auto italicStyle = style;
                italicStyle.italic = true;
                projectInline (out, i + 1, close, italicStyle, linkTarget);
                out.skipTo (close + 1);
                i = close + 1;
                continue;
            }
        }

        if (source[i] == '`' && i + 1 < end)
        {
            const auto close = findUnescaped (source, "`", i + 1, end);
            if (close != std::string::npos)
            {
                out.skipTo (i + 1);
                auto codeStyle = style;
                codeStyle.code = true;
                out.appendSourceRange (i + 1, close, codeStyle, linkTarget);
                out.skipTo (close + 1);
                i = close + 1;
                continue;
            }
        }

        if (source[i] == '[')
        {
            // ChordPro brackets are hidden like any other syntax; the chord
            // anchors on whatever character the projection appends next, so it
            // stays over its syllable as the lyric is edited around it.
            const auto chordEnd = findUnescaped (source, "]", i + 1, end);
            if (chordEnd != std::string::npos
                && (chordEnd + 1 >= end || source[chordEnd + 1] != '(')
                && notepad::chords::isChord (std::string_view (source).substr (i + 1, chordEnd - (i + 1))))
            {
                out.chords.push_back ({ out.text.size(),
                                        source.substr (i + 1, chordEnd - (i + 1)) });
                out.chordSpans.emplace_back (i, chordEnd + 1);
                out.skipTo (chordEnd + 1);
                i = chordEnd + 1;
                continue;
            }

            const auto labelEnd = findUnescaped (source, "](", i + 1, end);
            if (labelEnd != std::string::npos)
            {
                const auto urlEnd = findUnescaped (source, ")", labelEnd + 2, end);
                if (urlEnd != std::string::npos)
                {
                    out.skipTo (i + 1);
                    auto linkStyle = style;
                    linkStyle.link = true;
                    projectInline (out, i + 1, labelEnd, linkStyle,
                                   source.substr (labelEnd + 2,
                                                  urlEnd - (labelEnd + 2)));
                    out.skipTo (urlEnd + 1);
                    i = urlEnd + 1;
                    continue;
                }
            }
        }

        out.appendSourceRange (i, i + 1, style, linkTarget);
        ++i;
    }
}

std::size_t headingPrefixLength (const std::string& text,
                                 std::size_t lineStart,
                                 std::size_t lineEnd)
{
    std::size_t count = 0;
    while (lineStart + count < lineEnd && count < 6 && text[lineStart + count] == '#')
        ++count;
    return count > 0 && lineStart + count < lineEnd && text[lineStart + count] == ' '
             ? count + 1
             : 0;
}

std::size_t orderedPrefixLength (const std::string& text,
                                 std::size_t lineStart,
                                 std::size_t lineEnd)
{
    auto i = lineStart;
    while (i < lineEnd && std::isdigit (static_cast<unsigned char> (text[i])) != 0)
        ++i;
    return i > lineStart && i + 1 < lineEnd && text[i] == '.' && text[i + 1] == ' '
             ? i + 2 - lineStart
             : 0;
}

std::size_t blockPrefixLength (const std::string& text,
                               std::size_t lineStart,
                               std::size_t lineEnd)
{
    if (const auto heading = headingPrefixLength (text, lineStart, lineEnd); heading != 0)
        return heading;
    if (const auto ordered = orderedPrefixLength (text, lineStart, lineEnd); ordered != 0)
        return ordered;
    if (text.compare (lineStart, std::min<std::size_t> (6, lineEnd - lineStart), "- [ ] ") == 0
        || text.compare (lineStart, std::min<std::size_t> (6, lineEnd - lineStart), "- [x] ") == 0)
        return 6;
    if (lineEnd - lineStart >= 2
        && ((text[lineStart] == '-' || text[lineStart] == '*' || text[lineStart] == '+'
             || text[lineStart] == '>')
            && text[lineStart + 1] == ' '))
        return 2;
    return 0;
}

NotepadDocument::BlockStyle blockStyleAtLine (const std::string& text,
                                              std::size_t lineStart,
                                              std::size_t lineEnd)
{
    const auto heading = headingPrefixLength (text, lineStart, lineEnd);
    if (heading != 0)
    {
        switch (heading - 1)
        {
            case 1: return NotepadDocument::BlockStyle::heading1;
            case 2: return NotepadDocument::BlockStyle::heading2;
            default: return NotepadDocument::BlockStyle::heading3;
        }
    }
    if (orderedPrefixLength (text, lineStart, lineEnd) != 0)
        return NotepadDocument::BlockStyle::numbers;
    if (text.compare (lineStart, std::min<std::size_t> (6, lineEnd - lineStart), "- [ ] ") == 0
        || text.compare (lineStart, std::min<std::size_t> (6, lineEnd - lineStart), "- [x] ") == 0)
        return NotepadDocument::BlockStyle::tasks;
    if (lineEnd - lineStart >= 2 && text[lineStart + 1] == ' ')
    {
        if (text[lineStart] == '>')
            return NotepadDocument::BlockStyle::quote;
        if (text[lineStart] == '-' || text[lineStart] == '*'
            || text[lineStart] == '+')
            return NotepadDocument::BlockStyle::bullets;
    }
    return NotepadDocument::BlockStyle::body;
}

bool hasInlineStyle (const NotepadDocument::TextStyle& textStyle,
                     NotepadDocument::InlineStyle inlineStyle)
{
    switch (inlineStyle)
    {
        case NotepadDocument::InlineStyle::bold:   return textStyle.bold;
        case NotepadDocument::InlineStyle::italic: return textStyle.italic;
        case NotepadDocument::InlineStyle::code:   return textStyle.code;
    }
    return false;
}

std::string escapeDocumentInsertion (const std::string& text, bool atLineStart)
{
    std::string escaped;
    escaped.reserve (text.size());

    for (const auto ch : text)
    {
        const bool inlineSyntax = ch == '\\' || ch == '*' || ch == '_' || ch == '['
                               || ch == ']' || ch == '`';
        const bool blockSyntax = atLineStart && (ch == '#' || ch == '>' || ch == '-'
                                               || ch == '+');
        if (inlineSyntax || blockSyntax)
            escaped.push_back ('\\');
        escaped.push_back (ch);
        atLineStart = ch == '\n';
    }
    return escaped;
}

std::size_t lineStartAt (const std::string& text, std::size_t offset)
{
    offset = std::min (offset, text.size());
    const auto found = offset == 0 ? std::string::npos : text.rfind ('\n', offset - 1);
    return found == std::string::npos ? 0 : found + 1;
}

std::size_t lineEndAt (const std::string& text, std::size_t offset)
{
    const auto found = text.find ('\n', std::min (offset, text.size()));
    return found == std::string::npos ? text.size() : found;
}
std::string projectRendered (const std::string& markdown)
{
    ProjectionBuilder out (markdown);
    std::size_t lineStart = 0;
    while (lineStart <= markdown.size())
    {
        const auto lineEnd = lineEndAt (markdown, lineStart);
        auto contentStart = lineStart;
        out.blockStyle = blockStyleAtLine (markdown, lineStart, lineEnd);
        if (const auto prefix = blockPrefixLength (markdown, lineStart, lineEnd); prefix != 0)
        {
            contentStart += prefix;
            out.skipTo (contentStart);
        }
        projectInline (out, contentStart, lineEnd);
        if (lineEnd < markdown.size())
            out.appendSourceRange (lineEnd, lineEnd + 1);
        if (lineEnd == markdown.size())
            break;
        lineStart = lineEnd + 1;
    }
    return std::move (out.text);
}

} // namespace

void NotepadDocument::setMarkdown (std::string text)
{
    markdownText = std::move (text);
    rebuildProjection();
}

bool NotepadDocument::replaceDocumentText (const std::string& text)
{
    std::size_t prefix = 0;
    while (prefix < projectedText.size() && prefix < text.size()
           && projectedText[prefix] == text[prefix])
        ++prefix;

    std::size_t suffix = 0;
    while (suffix < projectedText.size() - prefix && suffix < text.size() - prefix
           && projectedText[projectedText.size() - 1 - suffix] == text[text.size() - 1 - suffix])
        ++suffix;

    const auto escapeAt = [this] (std::size_t sourceOffset, const std::string& value)
    {
        // Source mode edits the Markdown directly, so its syntax characters are
        // the point: escaping them there would turn a typed '*' into '\\*'.
        if (sourceMode)
            return value;
        return escapeDocumentInsertion (
            value, sourceOffset == 0 || markdownText[sourceOffset - 1] == '\n');
    };

    const auto oldEnd = projectedText.size() - suffix;
    // Use the same inward affinity as formatting commands. In particular, the
    // projected boundary after the final character of **bold** maps past the
    // hidden closing marker; deleting that character must stop before the
    // marker or the edit would create invalid Markdown and flatten the run.
    auto source = sourceSelection ({ prefix, oldEnd });
    if (prefix == oldEnd && prefix > 0)
    {
        // At the right edge of a styled run the projected boundary sits after
        // its hidden closing marker. Insert just inside that marker first; the
        // caller can then retain or clear the run's style without producing a
        // chain of adjacent wrappers for character-by-character typing.
        const auto previousCharacter = sourceSelection ({ prefix - 1, prefix });
        source.start = source.end = previousCharacter.end;
    }
    const auto sourceStart = source.start;
    const auto sourceEnd = source.end;

    auto previousMarkdown = markdownText;
    markdownText.replace (sourceStart, sourceEnd - sourceStart,
                          escapeAt (sourceStart,
                                    text.substr (prefix, text.size() - prefix - suffix)));
    rebuildProjection();
    if (projectedText == text)
        return true;

    // Never fall back to re-authoring an affected line as plain text. A dropped
    // keystroke is recoverable; silently stripping the paragraph's formatting
    // is not. The editor resynchronises its buffer when false is returned.
    markdownText = std::move (previousMarkdown);
    rebuildProjection();
    return false;
}

NotepadDocument::Selection NotepadDocument::sourceSelection (Selection selection) const noexcept
{
    selection.start = std::min (selection.start, projectedText.size());
    selection.end = std::min (selection.end, projectedText.size());
    if (selection.start > selection.end)
        std::swap (selection.start, selection.end);
    auto sourceStart = documentBoundaryToSource[selection.start];
    auto sourceEnd = documentBoundaryToSource[selection.end];

    // A projected boundary immediately after formatted text naturally maps
    // past the hidden closing marker. For selection edits and toolbar toggles,
    // the useful affinity is inside that marker: replacing "bold" in
    // **bold** should produce **replacement**, not an unmatched **replacement.
    if (! selection.empty() && selection.end > 0 && ! projectedStyles.empty())
    {
        const auto previous = projectedStyles[selection.end - 1];
        const auto next = selection.end < projectedStyles.size()
                        ? projectedStyles[selection.end]
                        : TextStyle {};
        if (previous.code != next.code && sourceEnd >= 1
            && markdownText[sourceEnd - 1] == '`')
            --sourceEnd;
        if (previous.bold != next.bold && sourceEnd >= 2
            && markdownText.compare (sourceEnd - 2, 2, "**") == 0)
            sourceEnd -= 2;
        if (previous.italic != next.italic && sourceEnd >= 1
            && markdownText[sourceEnd - 1] == '*')
            --sourceEnd;
    }

    return { sourceStart, sourceEnd };
}

NotepadDocument::Selection NotepadDocument::documentSelection (Selection selection) const noexcept
{
    selection.start = std::min (selection.start, markdownText.size());
    selection.end = std::min (selection.end, markdownText.size());
    if (selection.start > selection.end)
        std::swap (selection.start, selection.end);

    const auto boundaryFor = [this] (std::size_t sourceOffset)
    {
        const auto found = std::lower_bound (documentBoundaryToSource.begin(),
                                             documentBoundaryToSource.end(),
                                             sourceOffset);
        return static_cast<std::size_t> (std::distance (documentBoundaryToSource.begin(),
                                                       found));
    };
    return { std::min (boundaryFor (selection.start), projectedText.size()),
             std::min (boundaryFor (selection.end), projectedText.size()) };
}

void NotepadDocument::wrapDocumentSelection (Selection selection,
                                             const std::string& prefix,
                                             const std::string& suffix)
{
    const auto source = sourceSelection (selection);
    const bool hasPrefix = source.start >= prefix.size()
                        && markdownText.compare (source.start - prefix.size(), prefix.size(), prefix) == 0;
    const bool hasSuffixBeforeEnd = source.end >= suffix.size()
                                 && markdownText.compare (source.end - suffix.size(), suffix.size(), suffix) == 0;
    const bool hasSuffixAfterEnd = markdownText.compare (source.end, suffix.size(), suffix) == 0;

    if (hasPrefix && (hasSuffixBeforeEnd || hasSuffixAfterEnd))
    {
        const auto suffixStart = hasSuffixBeforeEnd ? source.end - suffix.size() : source.end;
        markdownText.erase (suffixStart, suffix.size());
        markdownText.erase (source.start - prefix.size(), prefix.size());
    }
    else
    {
        markdownText.insert (source.end, suffix);
        markdownText.insert (source.start, prefix);
    }
    rebuildProjection();
}

void NotepadDocument::setDocumentInlineStyle (Selection selection,
                                              InlineStyle inlineStyle,
                                              bool enabled)
{
    selection.start = std::min (selection.start, projectedText.size());
    selection.end = std::min (selection.end, projectedText.size());
    if (selection.start > selection.end)
        std::swap (selection.start, selection.end);
    if (selection.empty())
        return;

    std::string prefix;
    std::string suffix;
    switch (inlineStyle)
    {
        case InlineStyle::bold:   prefix = suffix = "**"; break;
        case InlineStyle::italic: prefix = suffix = "*";  break;
        case InlineStyle::code:   prefix = suffix = "`";  break;
    }

    struct Run { std::size_t start = 0, end = 0; };
    std::vector<Run> runs;
    for (auto i = selection.start; i < selection.end;)
    {
        const bool wanted = enabled ? ! hasInlineStyle (projectedStyles[i], inlineStyle)
                                    : hasInlineStyle (projectedStyles[i], inlineStyle);
        if (! wanted || projectedText[i] == '\n' || projectedText[i] == '\r')
        {
            ++i;
            continue;
        }
        const auto start = i++;
        while (i < selection.end && projectedText[i] != '\n' && projectedText[i] != '\r'
               && (enabled ? ! hasInlineStyle (projectedStyles[i], inlineStyle)
                           : hasInlineStyle (projectedStyles[i], inlineStyle)))
            ++i;
        runs.push_back ({ start, i });
    }

    // Work backwards so source offsets for earlier runs stay valid.
    for (auto runIt = runs.rbegin(); runIt != runs.rend(); ++runIt)
    {
        if (enabled)
        {
            const auto source = sourceSelection ({ runIt->start, runIt->end });
            markdownText.insert (source.end, suffix);
            markdownText.insert (source.start, prefix);
            rebuildProjection();
            continue;
        }

        // Removing a style from the middle of a run means splitting the outer
        // Markdown wrapper around the unstyled island. Preserve nested styles
        // verbatim inside each segment.
        auto fullStart = runIt->start;
        auto fullEnd = runIt->end;
        while (fullStart > 0 && projectedText[fullStart - 1] != '\n'
               && hasInlineStyle (projectedStyles[fullStart - 1], inlineStyle))
            --fullStart;
        while (fullEnd < projectedText.size() && projectedText[fullEnd] != '\n'
               && hasInlineStyle (projectedStyles[fullEnd], inlineStyle))
            ++fullEnd;

        const auto fullSource = sourceSelection ({ fullStart, fullEnd });
        const auto middleSource = sourceSelection ({ runIt->start, runIt->end });
        if (fullSource.start < prefix.size()
            || markdownText.compare (fullSource.start - prefix.size(), prefix.size(), prefix) != 0
            || markdownText.compare (fullSource.end, suffix.size(), suffix) != 0
            || middleSource.start < fullSource.start || middleSource.end > fullSource.end)
            continue;

        const auto left = markdownText.substr (fullSource.start,
                                               middleSource.start - fullSource.start);
        const auto middle = markdownText.substr (middleSource.start,
                                                 middleSource.end - middleSource.start);
        const auto right = markdownText.substr (middleSource.end,
                                                fullSource.end - middleSource.end);
        std::string replacement;
        if (! left.empty()) replacement += prefix + left + suffix;
        replacement += middle;
        if (! right.empty()) replacement += prefix + right + suffix;

        const auto outerStart = fullSource.start - prefix.size();
        const auto outerEnd = fullSource.end + suffix.size();
        markdownText.replace (outerStart, outerEnd - outerStart, replacement);
        rebuildProjection();
    }
}

void NotepadDocument::setDocumentBlockStyle (Selection selection, BlockStyle style)
{
    const auto source = sourceSelection (selection);
    const auto firstLine = lineStartAt (markdownText, source.start);
    const auto inclusiveEnd = source.end > firstLine && source.end <= markdownText.size()
                           && markdownText[source.end - 1] == '\n'
                         ? source.end - 1
                         : source.end;
    const auto lastLine = lineEndAt (markdownText, inclusiveEnd);

    std::vector<std::pair<std::size_t, std::size_t>> lines;
    for (auto start = firstLine; start <= lastLine;)
    {
        const auto end = lineEndAt (markdownText, start);
        lines.emplace_back (start, end);
        if (end == markdownText.size())
            break;
        start = end + 1;
    }

    for (std::size_t index = lines.size(); index-- > 0;)
    {
        const auto [start, end] = lines[index];
        const auto prefixLength = blockPrefixLength (markdownText, start, end);
        markdownText.erase (start, prefixLength);

        std::string prefix;
        switch (style)
        {
            case BlockStyle::heading1: prefix = "# "; break;
            case BlockStyle::heading2: prefix = "## "; break;
            case BlockStyle::heading3: prefix = "### "; break;
            case BlockStyle::quote:    prefix = "> "; break;
            case BlockStyle::bullets:  prefix = "- "; break;
            case BlockStyle::tasks:    prefix = "- [ ] "; break;
            case BlockStyle::body:
            case BlockStyle::numbers:  break;
        }
        if (style == BlockStyle::numbers)
            prefix = std::to_string (index + 1) + ". ";
        markdownText.insert (start, prefix);
    }
    rebuildProjection();
}

NotepadDocument::LineInfo NotepadDocument::lineInfoAt (std::size_t documentOffset) const noexcept
{
    // Source mode shows the syntax rather than acting on it, so every line is
    // body text: a heading keeps its metrics between the two views and the
    // toggle moves nothing.
    if (sourceMode)
        return {};

    documentOffset = std::min (documentOffset, projectedText.size());
    const auto sourceOffset = documentBoundaryToSource[documentOffset];
    const auto start = lineStartAt (markdownText, sourceOffset);
    const auto end = lineEndAt (markdownText, start);

    LineInfo info;
    info.block = blockStyleAtLine (markdownText, start, end);
    if (info.block == BlockStyle::numbers)
    {
        int number = 0;
        for (auto i = start; i < end && std::isdigit (static_cast<unsigned char> (markdownText[i])) != 0; ++i)
        {
            const int digit = markdownText[i] - '0';
            // orderedPrefixLength accepts any run of digits, so a hand-written
            // "99999999999. item" would overflow the accumulator.
            if (number > (std::numeric_limits<int>::max() - digit) / 10)
                break;
            number = number * 10 + digit;
        }
        info.orderedNumber = number;
    }
    else if (info.block == BlockStyle::tasks)
    {
        info.taskChecked = markdownText.compare (start, 6, "- [x] ") == 0;
    }
    return info;
}

void NotepadDocument::toggleTaskAt (std::size_t documentOffset)
{
    documentOffset = std::min (documentOffset, projectedText.size());
    const auto sourceOffset = documentBoundaryToSource[documentOffset];
    const auto start = lineStartAt (markdownText, sourceOffset);
    const auto end = lineEndAt (markdownText, start);

    if (markdownText.compare (start, std::min<std::size_t> (6, end - start), "- [ ] ") == 0)
        markdownText[start + 3] = 'x';
    else if (markdownText.compare (start, std::min<std::size_t> (6, end - start), "- [x] ") == 0)
        markdownText[start + 3] = ' ';
    else
        return;
    rebuildProjection();
}

void NotepadDocument::renumberOrderedRunAt (std::size_t documentOffset)
{
    documentOffset = std::min (documentOffset, projectedText.size());
    auto lineStart = lineStartAt (markdownText, documentBoundaryToSource[documentOffset]);
    if (orderedPrefixLength (markdownText, lineStart,
                             lineEndAt (markdownText, lineStart)) == 0)
        return;

    while (lineStart > 0)
    {
        const auto prevEnd = lineStart - 1;
        const auto prevStart = lineStartAt (markdownText, prevEnd);
        if (orderedPrefixLength (markdownText, prevStart, prevEnd) == 0)
            break;
        lineStart = prevStart;
    }

    int number = 1;
    auto start = lineStart;
    while (true)
    {
        const auto end = lineEndAt (markdownText, start);
        const auto prefix = orderedPrefixLength (markdownText, start, end);
        if (prefix == 0)
            break;
        const auto replacement = std::to_string (number++) + ". ";
        markdownText.replace (start, prefix, replacement);
        const auto newEnd = end - prefix + replacement.size();
        if (newEnd >= markdownText.size())
            break;
        start = newEnd + 1;
    }
    rebuildProjection();
}

NotepadDocument::TextStyle NotepadDocument::styleAt (std::size_t documentOffset) const noexcept
{
    if (projectedStyles.empty())
        return {};
    if (documentOffset >= projectedStyles.size())
        documentOffset = projectedStyles.size() - 1;
    return projectedStyles[documentOffset];
}

std::string NotepadDocument::chordAt (std::size_t documentOffset) const
{
    for (const auto& chord : projectedChords)
        if (chord.documentOffset == documentOffset)
            return chord.name;
    return {};
}

bool NotepadDocument::setChordAt (std::size_t documentOffset, const std::string& name)
{
    if (! name.empty() && ! notepad::chords::isChord (name))
        return false;

    for (std::size_t i = 0; i < projectedChords.size(); ++i)
    {
        if (projectedChords[i].documentOffset != documentOffset)
            continue;

        const auto& token = chordTokens[i];
        markdownText.replace (token.start, token.end - token.start,
                              name.empty() ? std::string() : "[" + name + "]");
        rebuildProjection();
        return true;
    }

    if (name.empty())
        return false;

    documentOffset = std::min (documentOffset, documentBoundaryToSource.size() - 1);
    markdownText.insert (documentBoundaryToSource[documentOffset], "[" + name + "]");
    rebuildProjection();
    return true;
}

void NotepadDocument::transposeChords (int semitones, bool preferFlats)
{
    if (chordTokens.empty())
        return;

    // Back to front: every rewrite can change the token's length, and a span
    // ahead of the cursor would shift under the next replace.
    for (auto i = chordTokens.size(); i-- > 0;)
    {
        const auto& token = chordTokens[i];
        const auto shifted = notepad::chords::transpose (token.name, semitones, preferFlats);
        markdownText.replace (token.start, token.end - token.start, "[" + shifted + "]");
    }
    rebuildProjection();
}

std::string NotepadDocument::linkTargetAt (std::size_t documentOffset) const
{
    if (projectedLinkTargets.empty())
        return {};
    if (documentOffset >= projectedLinkTargets.size())
        documentOffset = projectedLinkTargets.size() - 1;
    return projectedLinkTargets[documentOffset];
}

bool NotepadDocument::selectionHasInlineStyle (Selection selection,
                                               InlineStyle inlineStyle) const noexcept
{
    selection.start = std::min (selection.start, projectedStyles.size());
    selection.end = std::min (selection.end, projectedStyles.size());
    if (selection.start > selection.end)
        std::swap (selection.start, selection.end);

    if (selection.empty())
    {
        if (selection.start < projectedStyles.size() && projectedText[selection.start] != '\n')
            return hasInlineStyle (projectedStyles[selection.start], inlineStyle);
        if (selection.start > 0 && projectedText[selection.start - 1] != '\n')
            return hasInlineStyle (projectedStyles[selection.start - 1], inlineStyle);
        return false;
    }

    bool foundContent = false;
    for (auto i = selection.start; i < selection.end; ++i)
    {
        if (projectedText[i] == '\n' || projectedText[i] == '\r')
            continue;
        foundContent = true;
        if (! hasInlineStyle (projectedStyles[i], inlineStyle))
            return false;
    }
    return foundContent;
}

NotepadDocument::BlockStyle NotepadDocument::blockStyleForSelection (Selection selection) const noexcept
{
    const auto source = sourceSelection (selection);
    const auto firstLine = lineStartAt (markdownText, source.start);
    const auto inclusiveEnd = source.end > firstLine && source.end <= markdownText.size()
                           && markdownText[source.end - 1] == '\n'
                         ? source.end - 1
                         : source.end;
    const auto lastLine = lineEndAt (markdownText, inclusiveEnd);
    const auto initial = blockStyleAtLine (markdownText, firstLine,
                                           lineEndAt (markdownText, firstLine));

    for (auto start = firstLine; start <= lastLine;)
    {
        const auto end = lineEndAt (markdownText, start);
        if (blockStyleAtLine (markdownText, start, end) != initial)
            return BlockStyle::body;
        if (end == markdownText.size())
            break;
        start = end + 1;
    }
    return initial;
}

const std::string& NotepadDocument::renderedText() const
{
    if (! renderedTextCache.has_value())
    {
        if (! sourceMode)
            renderedTextCache = projectedText;
        else
            renderedTextCache = projectRendered (markdownText);
    }
    return *renderedTextCache;
}

std::size_t NotepadDocument::wordCount() const noexcept
{
    const auto& text = renderedText();
    std::size_t count = 0;
    std::size_t start = 0;
    while (start < text.size())
    {
        while (start < text.size()
               && std::isspace (static_cast<unsigned char> (text[start])) != 0)
            ++start;
        auto end = start;
        while (end < text.size()
               && std::isspace (static_cast<unsigned char> (text[end])) == 0)
            ++end;
        if (end > start)
            ++count;
        start = end;
    }
    return count;
}

std::size_t NotepadDocument::characterCount() const noexcept
{
    return renderedText().size();
}

void NotepadDocument::scanChordTokens()
{
    chordTokens.clear();
    for (std::size_t i = 0; i < markdownText.size(); ++i)
    {
        if (markdownText[i] != '[')
            continue;
        const auto close = findUnescaped (markdownText, "]", i + 1, markdownText.size());
        if (close == std::string::npos)
            break;
        if (close + 1 < markdownText.size() && markdownText[close + 1] == '(')
            continue;
        auto name = markdownText.substr (i + 1, close - (i + 1));
        if (! notepad::chords::isChord (name))
            continue;
        chordTokens.push_back ({ i, close + 1, std::move (name) });
        i = close;
    }

    std::size_t flats = 0;
    std::size_t sharps = 0;
    for (const auto& token : chordTokens)
    {
        flats += token.name.find ('b') != std::string::npos ? 1u : 0u;
        sharps += token.name.find ('#') != std::string::npos ? 1u : 0u;
    }
    if (flats != 0 || sharps != 0)
        flatSpelling = flats > sharps;
}

void NotepadDocument::setSourceMode (bool showSource)
{
    if (sourceMode == showSource)
        return;
    sourceMode = showSource;
    rebuildProjection();
}

void NotepadDocument::rebuildProjection()
{
    renderedTextCache.reset();
    scanChordTokens();
    if (sourceMode)
    {
        projectedText = markdownText;
        documentBoundaryToSource.resize (markdownText.size() + 1);
        for (std::size_t i = 0; i <= markdownText.size(); ++i)
            documentBoundaryToSource[i] = i;
        projectedStyles.assign (markdownText.size(), TextStyle {});
        projectedLinkTargets.assign (markdownText.size(), std::string {});
        projectedChords.clear();
        return;
    }

    ProjectionBuilder out (markdownText);
    std::size_t lineStart = 0;

    while (lineStart <= markdownText.size())
    {
        const auto lineEnd = lineEndAt (markdownText, lineStart);
        auto contentStart = lineStart;
        out.blockStyle = blockStyleAtLine (markdownText, lineStart, lineEnd);

        // Every block prefix is hidden wholesale; the renderer draws list
        // markers itself (LineInfo) so the projected text carries only what
        // the user can edit. Ordered prefixes count too.
        if (const auto prefix = blockPrefixLength (markdownText, lineStart, lineEnd); prefix != 0)
        {
            contentStart += prefix;
            out.skipTo (contentStart);
        }

        projectInline (out, contentStart, lineEnd);
        if (lineEnd < markdownText.size())
            out.appendSourceRange (lineEnd, lineEnd + 1);
        if (lineEnd == markdownText.size())
            break;
        lineStart = lineEnd + 1;
    }

    projectedText = std::move (out.text);
    documentBoundaryToSource = std::move (out.boundaries);
    projectedStyles = std::move (out.styles);
    projectedLinkTargets = std::move (out.links);
    projectedChords = std::move (out.chords);
}
} // namespace duskstudio
