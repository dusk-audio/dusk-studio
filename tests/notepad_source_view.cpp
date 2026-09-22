#include <catch2/catch_test_macros.hpp>

#include "ui/NotepadEditorCore.h"

#include <string>

using duskstudio::NotepadDocument;
namespace notepad = duskstudio::notepad;

namespace
{
notepad::Snapshot snapshot (const std::string& markdown, std::size_t caret)
{
    return { markdown, NotepadDocument::Selection { caret, caret } };
}
} // namespace

// Source view stands in for the raw-text editor: the visit is entered, the
// markdown is mutated as often as the typist likes, and the visit is closed.

TEST_CASE ("Source view records one undo step for a whole visit", "[notepad][undo]")
{
    notepad::UndoStack history;
    notepad::SourceViewSession source;

    std::string markdown = "[C]Verse one\n";
    history.record (notepad::EditKind::typing, snapshot (markdown, 0), markdown.size());
    const auto before = history.undoDepth();

    source.enter (history, snapshot (markdown, markdown.size()));
    markdown += "[G]Verse two\n";
    markdown += "[Am]Verse three\n";
    markdown = "# Chart\n" + markdown;
    REQUIRE (source.exit (history, markdown, markdown.size()));

    REQUIRE (history.undoDepth() == before + 1);
    REQUIRE_FALSE (source.active());
}

TEST_CASE ("One undo after source view restores the text it was entered with",
           "[notepad][undo]")
{
    notepad::UndoStack history;
    notepad::SourceViewSession source;

    const std::string entered = "[C]Verse one\n";
    std::string markdown = entered;

    source.enter (history, snapshot (markdown, markdown.size()));
    markdown = "# Chart\n[G]Rewritten in source\n";
    source.exit (history, markdown, markdown.size());

    notepad::Snapshot restored;
    REQUIRE (history.undo (snapshot (markdown, markdown.size()), restored));
    REQUIRE (restored.markdown == entered);
    REQUIRE (history.undoDepth() == 0);
}

TEST_CASE ("A source view visit that changed nothing records nothing", "[notepad][undo]")
{
    notepad::UndoStack history;
    notepad::SourceViewSession source;

    const std::string markdown = "[C]Verse one\n";
    source.enter (history, snapshot (markdown, markdown.size()));
    REQUIRE (source.active());
    REQUIRE_FALSE (source.exit (history, markdown, markdown.size()));
    REQUIRE (history.undoDepth() == 0);
    REQUIRE_FALSE (history.canUndo());
}

TEST_CASE ("Source view ends the typing run it interrupted", "[notepad][undo]")
{
    notepad::UndoStack history;
    notepad::SourceViewSession source;

    // A typing run that keeps coalescing while the caret carries on.
    history.record (notepad::EditKind::typing, snapshot ("", 0), 1);
    history.record (notepad::EditKind::typing, snapshot ("a", 1), 2);
    REQUIRE (history.undoDepth() == 1);

    // Looking at the source and changing nothing still separates what follows
    // from what came before.
    source.enter (history, snapshot ("ab", 2));
    REQUIRE_FALSE (source.exit (history, "ab", 2));

    history.record (notepad::EditKind::typing, snapshot ("ab", 2), 3);
    REQUIRE (history.undoDepth() == 2);
}

TEST_CASE ("Escape in source view closes the notepad instead of reverting",
           "[notepad][keys]")
{
    using notepad::EscapeAction;
    using notepad::sourceViewEscape;

    REQUIRE (sourceViewEscape (true, true, true) == EscapeAction::closeNotepad);
    // The release is claimed too, so the text field never sees the key at all.
    REQUIRE (sourceViewEscape (true, true, false) == EscapeAction::consume);
    // Other keys and the chart view are none of its business.
    REQUIRE (sourceViewEscape (true, false, true) == EscapeAction::pass);
    REQUIRE (sourceViewEscape (false, true, true) == EscapeAction::pass);
}
