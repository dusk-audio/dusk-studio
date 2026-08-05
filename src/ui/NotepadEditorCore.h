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

bool isHeading (NotepadDocument::BlockStyle block) noexcept;
bool isList (NotepadDocument::BlockStyle block) noexcept;
float blockFontSize (NotepadDocument::BlockStyle block, float bodySize) noexcept;
float blockRowHeight (NotepadDocument::BlockStyle block, float bodySize) noexcept;
float blockIndent (NotepadDocument::BlockStyle block) noexcept;

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

Layout buildLayout (const NotepadDocument& document, float width, float bodySize,
                    const MeasureFn& measure);
float offsetX (const NotepadDocument& document, const Row& row, std::size_t offset,
               float bodySize, const MeasureFn& measure);
std::size_t offsetAtX (const NotepadDocument& document, const Row& row, float localX,
                       float bodySize, const MeasureFn& measure);

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
    // Selections are recorded in the coordinate space of the mode that made the
    // edit; the restoring side maps them when the mode has changed since.
    bool selectionIsSource = false;
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
