#pragma once

#include "NotepadDocument.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace duskstudio
{
namespace notepad
{
std::size_t nextOffset (const std::string& text, std::size_t offset) noexcept;
std::size_t previousOffset (const std::string& text, std::size_t offset) noexcept;
// Rounds an offset down onto the start of the codepoint that contains it.
std::size_t snapToBoundary (const std::string& text, std::size_t offset) noexcept;
std::size_t lineStartOffset (const std::string& text, std::size_t offset) noexcept;
std::size_t lineEndOffset (const std::string& text, std::size_t offset) noexcept;
std::size_t wordLeft (const std::string& text, std::size_t offset) noexcept;
std::size_t wordRight (const std::string& text, std::size_t offset) noexcept;

struct Range
{
    std::size_t start = 0;
    std::size_t end = 0;
};

Range wordAt (const std::string& text, std::size_t offset) noexcept;
void appendUtf8 (std::string& text, unsigned int codepoint);

// AltGr is reported by ImGui as Ctrl+Alt on platforms that use it. Treat that
// pair as text input while keeping Ctrl-only and Super combinations available
// for editor commands.
bool acceptsTextInput (bool control, bool alt, bool super) noexcept;

struct MarkdownTransform
{
    std::string markdown;
    NotepadDocument::Selection selection;
};

bool markdownInlineActive (const std::string& markdown,
                           NotepadDocument::Selection selection,
                           const std::string& prefix,
                           const std::string& suffix);
MarkdownTransform toggleMarkdownInline (const std::string& markdown,
                                        NotepadDocument::Selection selection,
                                        const std::string& prefix,
                                        const std::string& suffix);
MarkdownTransform setMarkdownBlockStyle (const std::string& markdown,
                                         NotepadDocument::Selection selection,
                                         NotepadDocument::BlockStyle style);

bool isHeading (NotepadDocument::BlockStyle block) noexcept;
bool isList (NotepadDocument::BlockStyle block) noexcept;
float blockFontSize (NotepadDocument::BlockStyle block, float bodySize) noexcept;
float blockRowHeight (NotepadDocument::BlockStyle block, float bodySize) noexcept;
float blockIndent (NotepadDocument::BlockStyle block) noexcept;
// Height of the chord band drawn above a row that carries chords.
float chordBandHeight (float bodySize) noexcept;
// Point size the chord lane is drawn at above lyrics of the given size.
float chordFontSize (float bodySize) noexcept;

// Vertical bands of the panel. The chrome keeps its stated heights and the
// chart absorbs whatever is left, so a band can never be handed a fraction of
// a line and clip its own text mid-glyph. When even the chrome cannot fit, the
// bands shrink from the bottom rather than overflowing the window.
struct ChromeBands
{
    float header = 0.0f;
    float ribbon = 0.0f;
    float chart = 0.0f;
    float status = 0.0f;

    float total() const noexcept { return header + ribbon + chart + status; }
};

ChromeBands layoutChrome (float windowHeight, float headerHeight, float ribbonHeight,
                          float statusHeight) noexcept;

// Width of documentText()[begin, end) drawn at fontSize inside a block of the
// given style. The renderer supplies the font metrics; the layout math stays
// free of any toolkit so it can be unit tested against a synthetic advance.
using MeasureFn = std::function<float (std::size_t begin, std::size_t end, float fontSize,
                                       NotepadDocument::BlockStyle block)>;

struct Row
{
    std::size_t start = 0;
    std::size_t end = 0;
    std::size_t lineStart = 0;
    std::size_t lineEnd = 0;
    NotepadDocument::BlockStyle block = NotepadDocument::BlockStyle::body;
    NotepadDocument::LineInfo lineInfo;
    float y = 0.0f;
    float height = 0.0f;
    float indent = 0.0f;
    bool firstRowOfLine = false;
    bool lastRowOfLine = false;
    // Band reserved above the text for the row's chords, part of height. Zero
    // on rows without chords, so a lyric sheet only pays for the lines it uses.
    float chordTop = 0.0f;
};

struct Layout
{
    std::vector<Row> rows;
    float contentHeight = 0.0f;

    // A caret at a soft-wrap boundary belongs to two rows. preferRowEnd picks
    // the row that ends there (End, or a click past the last glyph); otherwise
    // the caret rides the following row, which is where typing continues.
    std::size_t rowForOffset (std::size_t offset, bool preferRowEnd) const noexcept;
    std::size_t rowAtY (float y) const noexcept;
};

// pendingChordAnchor reserves the chord band on the row that is about to
// receive a chord, so the open slot pushes the lyric down exactly like a
// committed chord does. npos when no slot is open.
Layout buildLayout (const NotepadDocument& document, float width, float bodySize,
                    const MeasureFn& measure,
                    std::size_t pendingChordAnchor = std::string::npos);
float offsetX (const NotepadDocument& document, const Row& row, std::size_t offset,
               float bodySize, const MeasureFn& measure);
std::size_t offsetAtX (const NotepadDocument& document, const Row& row, float localX,
                       float bodySize, const MeasureFn& measure);

// Width of a chord label drawn in the chord lane. Supplied by the renderer for
// the same reason MeasureFn is: the geometry stays free of any toolkit.
using LabelWidthFn = std::function<float (const std::string&)>;

struct ChordPlacement
{
    std::size_t chordIndex = 0;   // into NotepadDocument::chords()
    float x = 0.0f;               // from the row's indent
    float width = 0.0f;
};

// The chords anchored on a row, left to right, each with the span its label
// occupies. A label is truncated where the next chord's anchor starts, and two
// chords sharing an anchor are set side by side, so the spans never overlap and
// every label is both visible and reachable by exactly one click.
std::vector<ChordPlacement> rowChordPlacements (const NotepadDocument& document, const Row& row,
                                                float bodySize, const MeasureFn& measure,
                                                const LabelWidthFn& labelWidth);
// Index into NotepadDocument::chords() for the label covering localX, or npos
// when the click landed on bare band.
std::size_t chordAtX (const std::vector<ChordPlacement>& placements, float localX) noexcept;

enum class EditKind
{
    typing,
    deleting,
    structural
};

struct Snapshot
{
    std::string markdown;
    NotepadDocument::Selection selection;
};

class UndoStack final
{
public:
    static constexpr std::size_t kMaxEntries = 256;

    void clear() noexcept;
    // Ends the current coalescing run, so the next edit starts a fresh step.
    void breakRun() noexcept;

    void record (EditKind kind, Snapshot before, std::size_t caretAfter);

    bool canUndo() const noexcept { return ! undoEntries.empty(); }
    bool canRedo() const noexcept { return ! redoEntries.empty(); }
    bool undo (const Snapshot& current, Snapshot& restored);
    bool redo (const Snapshot& current, Snapshot& restored);

    std::size_t undoDepth() const noexcept { return undoEntries.size(); }
    std::size_t redoDepth() const noexcept { return redoEntries.size(); }

private:
    struct Entry
    {
        EditKind kind = EditKind::structural;
        Snapshot state;
        std::size_t caretAfter = 0;
    };

    std::vector<Entry> undoEntries;
    std::vector<Entry> redoEntries;
    bool runBroken = true;
};
} // namespace notepad
} // namespace duskstudio
