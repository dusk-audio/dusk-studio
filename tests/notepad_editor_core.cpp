#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ui/NotepadEditorCore.h"

#include <string>

using duskstudio::NotepadDocument;
namespace notepad = duskstudio::notepad;

namespace
{
// One unit of width per byte, scaled by the block's font size, so wrap points
// are exact multiples of the content width in the assertions below.
notepad::MeasureFn fixedAdvance (float perByteAtBodySize, float bodySize)
{
    return [perByteAtBodySize, bodySize] (std::size_t begin, std::size_t end, float fontSize,
                                          NotepadDocument::BlockStyle)
    {
        return static_cast<float> (end - begin) * perByteAtBodySize * (fontSize / bodySize);
    };
}

std::string rowText (const NotepadDocument& document, const notepad::Row& row)
{
    return document.documentText().substr (row.start, row.end - row.start);
}
} // namespace

TEST_CASE ("Notepad editor UTF-8 caret movement steps whole codepoints",
           "[notepad][editor]")
{
    const std::string text = "aé漢z";

    CHECK (notepad::nextOffset (text, 0) == 1);
    CHECK (notepad::nextOffset (text, 1) == 3);
    CHECK (notepad::nextOffset (text, 3) == 6);
    CHECK (notepad::nextOffset (text, 6) == 7);
    CHECK (notepad::nextOffset (text, 7) == 7);

    CHECK (notepad::previousOffset (text, 7) == 6);
    CHECK (notepad::previousOffset (text, 6) == 3);
    CHECK (notepad::previousOffset (text, 3) == 1);
    CHECK (notepad::previousOffset (text, 1) == 0);
    CHECK (notepad::previousOffset (text, 0) == 0);
}

TEST_CASE ("Notepad editor word jumps stop at word and line boundaries",
           "[notepad][editor]")
{
    const std::string text = "alpha beta\ngamma";

    CHECK (notepad::wordRight (text, 0) == 6);
    CHECK (notepad::wordRight (text, 6) == 10);
    // The line break is a stop of its own so a word jump never skips a line.
    CHECK (notepad::wordRight (text, 10) == 11);

    CHECK (notepad::wordLeft (text, 10) == 6);
    CHECK (notepad::wordLeft (text, 6) == 0);
    CHECK (notepad::wordLeft (text, 11) == 10);

    const auto word = notepad::wordAt (text, 8);
    CHECK (word.start == 6);
    CHECK (word.end == 10);

    const auto atLineEnd = notepad::wordAt (text, 10);
    CHECK (atLineEnd.start == 6);
    CHECK (atLineEnd.end == 10);
}

TEST_CASE ("Notepad editor layout wraps body text at the content width",
           "[notepad][editor][layout]")
{
    NotepadDocument document;
    document.setMarkdown ("alpha beta gamma delta");

    // 10 units per byte, 68 units of content: 6 bytes fit per row after the
    // layout's 8 unit right gutter.
    const auto layout = notepad::buildLayout (document, 68.0f, 10.0f,
                                              fixedAdvance (10.0f, 10.0f));

    REQUIRE (layout.rows.size() == 4);
    CHECK (rowText (document, layout.rows[0]) == "alpha ");
    CHECK (rowText (document, layout.rows[1]) == "beta ");
    CHECK (rowText (document, layout.rows[2]) == "gamma ");
    CHECK (rowText (document, layout.rows[3]) == "delta");
    CHECK (layout.rows[0].firstRowOfLine);
    CHECK_FALSE (layout.rows[1].firstRowOfLine);
    CHECK (layout.rows[3].lastRowOfLine);
    CHECK (layout.contentHeight > 0.0f);

    // Rows tile the line without gaps or overlap.
    for (std::size_t i = 1; i < layout.rows.size(); ++i)
        CHECK (layout.rows[i].start == layout.rows[i - 1].end);
}

TEST_CASE ("Notepad editor layout gives headings their own metrics",
           "[notepad][editor][layout]")
{
    NotepadDocument document;
    document.setMarkdown ("# Heading here\nbody");

    const auto layout = notepad::buildLayout (document, 400.0f, 10.0f,
                                              fixedAdvance (2.0f, 10.0f));

    REQUIRE (layout.rows.size() >= 2);
    CHECK (layout.rows.front().block == NotepadDocument::BlockStyle::heading1);
    // The projection hides "# ", so the row text is the heading content only.
    CHECK (rowText (document, layout.rows.front()) == "Heading here");
    CHECK (layout.rows.front().height > layout.rows.back().height);
    CHECK (layout.rows.back().block == NotepadDocument::BlockStyle::body);
}

TEST_CASE ("Notepad editor layout indents list lines and keeps marker data",
           "[notepad][editor][layout]")
{
    NotepadDocument document;
    document.setMarkdown ("- first\n- second\n\n1. one\n2. two\n- [x] done");

    const auto layout = notepad::buildLayout (document, 400.0f, 10.0f,
                                              fixedAdvance (2.0f, 10.0f));

    REQUIRE (layout.rows.size() == 6);
    CHECK (rowText (document, layout.rows[0]) == "first");
    CHECK (layout.rows[0].indent > 0.0f);
    CHECK (layout.rows[2].start == layout.rows[2].end); // the blank line
    CHECK (layout.rows[3].block == NotepadDocument::BlockStyle::numbers);
    CHECK (layout.rows[3].lineInfo.orderedNumber == 1);
    CHECK (layout.rows[4].lineInfo.orderedNumber == 2);
    CHECK (layout.rows[5].block == NotepadDocument::BlockStyle::tasks);
    CHECK (layout.rows[5].lineInfo.taskChecked);
    CHECK (rowText (document, layout.rows[5]) == "done");
}

TEST_CASE ("Notepad editor hit testing round-trips against caret positions",
           "[notepad][editor][layout]")
{
    NotepadDocument document;
    document.setMarkdown ("alpha beta gamma delta");

    const auto measure = fixedAdvance (10.0f, 10.0f);
    const auto layout = notepad::buildLayout (document, 68.0f, 10.0f, measure);
    REQUIRE (layout.rows.size() == 4);

    const auto& row = layout.rows[1];
    for (auto offset = row.start; offset <= row.end; ++offset)
    {
        const auto x = notepad::offsetX (document, row, offset, 10.0f, measure);
        CHECK (notepad::offsetAtX (document, row, x, 10.0f, measure) == offset);
    }
    // Past the end of a row the caret lands on the row's last boundary.
    CHECK (notepad::offsetAtX (document, row, 1000.0f, 10.0f, measure) == row.end);
}

TEST_CASE ("Notepad editor rows resolve caret rows at wrap boundaries",
           "[notepad][editor][layout]")
{
    NotepadDocument document;
    document.setMarkdown ("alpha beta gamma delta");

    const auto layout = notepad::buildLayout (document, 68.0f, 10.0f,
                                              fixedAdvance (10.0f, 10.0f));
    REQUIRE (layout.rows.size() == 4);
    const auto boundary = layout.rows[0].end;
    REQUIRE (boundary == layout.rows[1].start);

    CHECK (layout.rowForOffset (boundary, false) == 1);
    CHECK (layout.rowForOffset (boundary, true) == 0);
    CHECK (layout.rowForOffset (0, false) == 0);
    CHECK (layout.rowForOffset (document.documentText().size(), false)
           == layout.rows.size() - 1);
}

TEST_CASE ("Notepad editor undo coalesces a typing run into one step",
           "[notepad][editor][undo]")
{
    notepad::UndoStack history;

    const auto typed = [&history] (const std::string& before, std::size_t caret)
    {
        history.record (notepad::EditKind::typing, { before, { caret, caret }, false },
                        caret + 1);
    };

    typed ("", 0);
    typed ("a", 1);
    typed ("ab", 2);
    CHECK (history.undoDepth() == 1);

    notepad::Snapshot restored;
    REQUIRE (history.undo ({ "abc", { 3, 3 }, false }, restored));
    CHECK (restored.markdown.empty());
    CHECK (restored.selection.start == 0);
    CHECK (history.undoDepth() == 0);
    CHECK (history.canRedo());

    notepad::Snapshot redone;
    REQUIRE (history.redo ({ "", { 0, 0 }, false }, redone));
    CHECK (redone.markdown == "abc");
}

TEST_CASE ("Notepad editor undo breaks runs on caret moves and structural edits",
           "[notepad][editor][undo]")
{
    notepad::UndoStack history;

    history.record (notepad::EditKind::typing, { "", { 0, 0 }, false }, 1);
    history.record (notepad::EditKind::typing, { "a", { 1, 1 }, false }, 2);
    CHECK (history.undoDepth() == 1);

    // A caret move between keystrokes starts a new step even though the kind
    // and the offsets still line up.
    history.breakRun();
    history.record (notepad::EditKind::typing, { "ab", { 2, 2 }, false }, 3);
    CHECK (history.undoDepth() == 2);

    // Deletions never join a typing run, and every toolbar edit stands alone.
    history.record (notepad::EditKind::deleting, { "abc", { 3, 3 }, false }, 2);
    CHECK (history.undoDepth() == 3);
    history.record (notepad::EditKind::structural, { "ab", { 2, 2 }, false }, 2);
    history.record (notepad::EditKind::structural, { "**ab**", { 2, 2 }, false }, 2);
    CHECK (history.undoDepth() == 5);

    // Typing after an undo starts a fresh step rather than extending the one
    // that was just restored.
    notepad::Snapshot restored;
    REQUIRE (history.undo ({ "***ab***", { 2, 2 }, false }, restored));
    const auto depthAfterUndo = history.undoDepth();
    history.record (notepad::EditKind::typing, { "**ab**", { 2, 2 }, false }, 3);
    CHECK (history.undoDepth() == depthAfterUndo + 1);
    CHECK_FALSE (history.canRedo());
}

TEST_CASE ("Notepad editor undo replacing a selection is its own step",
           "[notepad][editor][undo]")
{
    notepad::UndoStack history;

    history.record (notepad::EditKind::typing, { "", { 0, 0 }, false }, 1);
    // Typing over a selection starts at the same offset the previous run ended
    // at, but it must not be folded into it.
    history.record (notepad::EditKind::typing, { "a", { 1, 4 }, false }, 2);
    CHECK (history.undoDepth() == 2);
}

TEST_CASE ("Notepad editor undo history is bounded", "[notepad][editor][undo]")
{
    notepad::UndoStack history;

    for (std::size_t i = 0; i < notepad::UndoStack::kMaxEntries + 20; ++i)
        history.record (notepad::EditKind::structural,
                        { std::string (i, 'x'), { i, i }, false }, i);

    CHECK (history.undoDepth() == notepad::UndoStack::kMaxEntries);

    notepad::Snapshot restored;
    REQUIRE (history.undo ({ "current", { 0, 0 }, false }, restored));
    CHECK (restored.markdown.size() == notepad::UndoStack::kMaxEntries + 19);
}
