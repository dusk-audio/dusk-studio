#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace duskstudio
{
class NotepadDocument final
{
public:
    struct Selection
    {
        std::size_t start = 0;
        std::size_t end = 0;

        bool empty() const noexcept { return start == end; }
    };

    enum class BlockStyle
    {
        body,
        heading1,
        heading2,
        heading3,
        quote,
        bullets,
        numbers,
        tasks
    };

    enum class InlineStyle
    {
        bold,
        italic,
        code
    };

    struct TextStyle
    {
        BlockStyle block = BlockStyle::body;
        bool bold = false;
        bool italic = false;
        bool code = false;
        bool link = false;

        bool operator== (const TextStyle& other) const noexcept
        {
            return block == other.block && bold == other.bold
                && italic == other.italic && code == other.code
                && link == other.link;
        }
        bool operator!= (const TextStyle& other) const noexcept { return ! (*this == other); }
    };

    // Per-line block metadata for the document renderer: list markers are NOT
    // part of the projected text (the source prefixes are hidden wholesale),
    // so the editor draws bullets / numbers / checkboxes itself from this.
    struct LineInfo
    {
        BlockStyle block = BlockStyle::body;
        int  orderedNumber = 0;      // 1-based, numbers lines only
        bool taskChecked = false;    // tasks lines only
    };

    void setMarkdown (std::string text);
    // False when the edit could not be mapped back onto the Markdown source and
    // was dropped to keep it intact - the caller must resync its editor buffer
    // from documentText().
    bool replaceDocumentText (const std::string& text);

    const std::string& markdown() const noexcept { return markdownText; }
    const std::string& documentText() const noexcept { return projectedText; }

    LineInfo lineInfoAt (std::size_t documentOffset) const noexcept;
    // Flips "- [ ] " <-> "- [x] " on the line containing documentOffset.
    // No-op on non-task lines.
    void toggleTaskAt (std::size_t documentOffset);
    // Renumbers the contiguous run of ordered-list lines containing the line
    // at documentOffset (1-based from the top of the run). No-op outside one.
    void renumberOrderedRunAt (std::size_t documentOffset);

    Selection sourceSelection (Selection documentSelection) const noexcept;
    Selection documentSelection (Selection sourceSelection) const noexcept;
    void wrapDocumentSelection (Selection selection,
                                const std::string& prefix,
                                const std::string& suffix);
    void setDocumentInlineStyle (Selection selection, InlineStyle style, bool enabled);
    void setDocumentBlockStyle (Selection selection, BlockStyle style);

    TextStyle styleAt (std::size_t documentOffset) const noexcept;
    std::string linkTargetAt (std::size_t documentOffset) const;
    bool selectionHasInlineStyle (Selection selection, InlineStyle style) const noexcept;
    BlockStyle blockStyleForSelection (Selection selection) const noexcept;

    std::size_t wordCount() const noexcept;

private:
    void rebuildProjection();

    std::string markdownText;
    std::string projectedText;
    // One boundary even before the first rebuildProjection, so sourceSelection()
    // on a default-constructed document resolves offset 0 instead of indexing an
    // empty vector.
    std::vector<std::size_t> documentBoundaryToSource { 0 };
    std::vector<TextStyle> projectedStyles;
    std::vector<std::string> projectedLinkTargets;
};
} // namespace duskstudio
