#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
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

    // A ChordPro bracket hidden from the projection. documentOffset is the
    // character the chord sits over; a chord parked at the end of a line
    // anchors on that line's newline.
    struct Chord
    {
        std::size_t documentOffset = 0;
        std::string name;
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
    // Source mode projects the Markdown 1:1, with no hidden syntax, styles or
    // chords, so the same editor drives both views: one set of layout metrics,
    // one caret, one undo stack.
    void setSourceMode (bool showSource);
    bool isSourceMode() const noexcept { return sourceMode; }
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

    const std::vector<Chord>& chords() const noexcept { return projectedChords; }
    // The spelling a transpose should write: whichever accidental the document
    // already uses, so a round trip returns the notation the user typed. Ties
    // and chord-free documents take sharps.
    bool prefersFlats() const noexcept { return flatSpelling; }
    // Empty when no chord is anchored at that exact offset.
    std::string chordAt (std::size_t documentOffset) const;
    // Inserts, replaces, or (empty name) removes the chord anchored at the
    // offset. Names that don't parse as a chord are rejected, so a stray
    // bracket can never be minted from the document view.
    bool setChordAt (std::size_t documentOffset, const std::string& name);
    void transposeChords (int semitones, bool preferFlats);
    // Writes "[Label]" on its own line above documentOffset's line, straight
    // into the source: a section marker is plain text the user can edit by
    // hand, so it must not go through the document view's escaping.
    bool insertSectionMarker (std::size_t documentOffset, const std::string& label);
    bool hasChords() const noexcept { return ! chordTokens.empty(); }

    TextStyle styleAt (std::size_t documentOffset) const noexcept;
    std::string linkTargetAt (std::size_t documentOffset) const;
    bool selectionHasInlineStyle (Selection selection, InlineStyle style) const noexcept;
    BlockStyle blockStyleForSelection (Selection selection) const noexcept;

    std::size_t wordCount() const noexcept;
    std::size_t characterCount() const noexcept;
    // A line whose whole content is one bracketed non-chord word, which is how
    // section markers are written.
    std::size_t sectionCount() const noexcept;
    std::vector<std::string> uniqueChordNames() const;

private:
    struct ChordToken
    {
        std::size_t start = 0;   // the '[' in markdownText
        std::size_t end = 0;     // one past the ']'
        std::string name;
        // Where the rendered projection anchors it; meaningless in source mode,
        // which resolves a chord by the caret's position inside the token.
        std::size_t documentOffset = 0;
    };

    void rebuildProjection();
    const ChordToken* chordTokenAt (std::size_t documentOffset) const noexcept;
    const std::string& renderedText() const;

    std::string markdownText;
    std::string projectedText;
    // One boundary even before the first rebuildProjection, so sourceSelection()
    // on a default-constructed document resolves offset 0 instead of indexing an
    // empty vector.
    std::vector<std::size_t> documentBoundaryToSource { 0 };
    std::vector<TextStyle> projectedStyles;
    std::vector<std::string> projectedLinkTargets;
    bool sourceMode = false;
    std::vector<Chord> projectedChords;
    // The source span of every chord the projection renders, kept in both modes:
    // chord commands act on the source, so they keep working while the source
    // view is up and projectedChords is empty.
    std::vector<ChordToken> chordTokens;
    // Word and character counts describe the lyric, so they are measured on the
    // rendered projection in both modes: never the syntax the source shows.
    // Only source mode fills this; the rendered view counts projectedText.
    std::optional<std::string> renderedTextCache;
    // Sticky, because a transpose can land every chord on a natural and erase
    // the evidence: re-deriving it there would spell the way back in sharps
    // and hand a flat writer a document they did not write.
    bool flatSpelling = false;
};
} // namespace duskstudio
