#include <catch2/catch_test_macros.hpp>

#include "ui/NotepadDocument.h"

#include <string>

using duskstudio::NotepadDocument;

TEST_CASE ("Notepad document projection preserves formatting state",
           "[notepad][document]")
{
    NotepadDocument document;
    document.setMarkdown ("Body **bold chunk** and *italic* with `code`\n"
                          "## Existing heading\n"
                          "Plain paragraph");

    const auto boldStart = document.documentText().find ("bold chunk");
    REQUIRE (boldStart != std::string::npos);
    const NotepadDocument::Selection boldSelection { boldStart, boldStart + 10 };

    CHECK (document.selectionHasInlineStyle (
        boldSelection, NotepadDocument::InlineStyle::bold));
    CHECK_FALSE (document.selectionHasInlineStyle (
        boldSelection, NotepadDocument::InlineStyle::italic));
    CHECK (document.styleAt (boldStart).bold);

    const auto headingStart = document.documentText().find ("Existing heading");
    REQUIRE (headingStart != std::string::npos);
    CHECK (document.blockStyleForSelection ({ headingStart, headingStart + 8 })
           == NotepadDocument::BlockStyle::heading2);
}

TEST_CASE ("Notepad document formatting mutates Markdown and remains selectable",
           "[notepad][document]")
{
    NotepadDocument document;
    document.setMarkdown ("Make this bold\nPromote this line\nLeave this alone");

    const auto boldStart = document.documentText().find ("this bold");
    REQUIRE (boldStart != std::string::npos);
    const NotepadDocument::Selection boldSelection { boldStart, boldStart + 9 };
    document.wrapDocumentSelection (boldSelection, "**", "**");
    CHECK (document.markdown().find ("**this bold**") != std::string::npos);
    CHECK (document.selectionHasInlineStyle (
        boldSelection, NotepadDocument::InlineStyle::bold));

    auto editedText = document.documentText();
    editedText.replace (boldSelection.start, boldSelection.end - boldSelection.start,
                        "still bold");
    REQUIRE (document.replaceDocumentText (editedText));
    CHECK (document.markdown().find ("**still bold**") != std::string::npos);

    const NotepadDocument::Selection editedBold {
        boldSelection.start, boldSelection.start + std::string ("still bold").size()
    };
    CHECK (document.selectionHasInlineStyle (
        editedBold, NotepadDocument::InlineStyle::bold));

    document.wrapDocumentSelection (editedBold, "**", "**");
    CHECK (document.markdown().find ("**still bold**") == std::string::npos);

    const auto headingStart = document.documentText().find ("Promote this line");
    REQUIRE (headingStart != std::string::npos);
    const NotepadDocument::Selection headingSelection { headingStart, headingStart + 17 };

    document.setDocumentBlockStyle (headingSelection, NotepadDocument::BlockStyle::heading1);
    CHECK (document.markdown().find ("# Promote this line") != std::string::npos);
    CHECK (document.blockStyleForSelection (headingSelection)
           == NotepadDocument::BlockStyle::heading1);

    document.setDocumentBlockStyle (headingSelection, NotepadDocument::BlockStyle::heading2);
    CHECK (document.markdown().find ("## Promote this line") != std::string::npos);
    CHECK (document.blockStyleForSelection (headingSelection)
           == NotepadDocument::BlockStyle::heading2);

    document.setDocumentBlockStyle (headingSelection, NotepadDocument::BlockStyle::heading3);
    CHECK (document.markdown().find ("### Promote this line") != std::string::npos);
    CHECK (document.blockStyleForSelection (headingSelection)
           == NotepadDocument::BlockStyle::heading3);
}

TEST_CASE ("Notepad document edits preserve wrappers at formatting boundaries",
           "[notepad][document]")
{
    NotepadDocument document;
    document.setMarkdown ("**bold** and *italic*");

    auto edited = document.documentText();
    edited.erase (3, 1); // Backspace at the right edge of the bold run.
    REQUIRE (document.replaceDocumentText (edited));
    CHECK (document.markdown() == "**bol** and *italic*");
    CHECK (document.documentText() == "bol and italic");
    CHECK (document.selectionHasInlineStyle ({ 0, 3 },
                                             NotepadDocument::InlineStyle::bold));
    CHECK (document.selectionHasInlineStyle ({ 8, 14 },
                                             NotepadDocument::InlineStyle::italic));
}

TEST_CASE ("Notepad inline commands split and rejoin styled runs safely",
           "[notepad][document]")
{
    NotepadDocument document;
    document.setMarkdown ("**bold**");

    document.setDocumentInlineStyle ({ 2, 3 }, NotepadDocument::InlineStyle::bold, false);
    CHECK (document.markdown() == "**bo**l**d**");
    CHECK (document.styleAt (0).bold);
    CHECK_FALSE (document.styleAt (2).bold);
    CHECK (document.styleAt (3).bold);

    document.setDocumentInlineStyle ({ 2, 3 }, NotepadDocument::InlineStyle::bold, true);
    CHECK (document.documentText() == "bold");
    CHECK (document.selectionHasInlineStyle ({ 0, 4 },
                                             NotepadDocument::InlineStyle::bold));
}

TEST_CASE ("Notepad headings support combined bold and italic formatting",
           "[notepad][document]")
{
    NotepadDocument document;
    document.setMarkdown ("# Heading");
    const NotepadDocument::Selection heading { 0, 7 };

    document.setDocumentInlineStyle (heading, NotepadDocument::InlineStyle::bold, true);
    CHECK (document.documentText() == "Heading");
    CHECK (document.markdown() == "# **Heading**");
    CHECK (document.selectionHasInlineStyle (heading,
                                             NotepadDocument::InlineStyle::bold));

    document.setDocumentInlineStyle (heading, NotepadDocument::InlineStyle::italic, true);
    CHECK (document.documentText() == "Heading");
    CHECK (document.markdown() == "# ***Heading***");
    CHECK (document.selectionHasInlineStyle (heading,
                                             NotepadDocument::InlineStyle::bold));
    CHECK (document.selectionHasInlineStyle (heading,
                                             NotepadDocument::InlineStyle::italic));

    document.setDocumentInlineStyle (heading, NotepadDocument::InlineStyle::bold, false);
    CHECK (document.documentText() == "Heading");
    CHECK (document.markdown() == "# *Heading*");
    CHECK_FALSE (document.selectionHasInlineStyle (heading,
                                                   NotepadDocument::InlineStyle::bold));
    CHECK (document.selectionHasInlineStyle (heading,
                                             NotepadDocument::InlineStyle::italic));

    document.setDocumentInlineStyle (heading, NotepadDocument::InlineStyle::bold, true);
    document.setDocumentInlineStyle (heading, NotepadDocument::InlineStyle::italic, false);
    CHECK (document.documentText() == "Heading");
    CHECK (document.markdown() == "# **Heading**");
    CHECK (document.selectionHasInlineStyle (heading,
                                             NotepadDocument::InlineStyle::bold));
    CHECK_FALSE (document.selectionHasInlineStyle (heading,
                                                   NotepadDocument::InlineStyle::italic));

    NotepadDocument reverseOrder;
    reverseOrder.setMarkdown ("## Another heading");
    const NotepadDocument::Selection anotherHeading { 0, 15 };
    reverseOrder.setDocumentInlineStyle (anotherHeading,
                                         NotepadDocument::InlineStyle::italic, true);
    reverseOrder.setDocumentInlineStyle (anotherHeading,
                                         NotepadDocument::InlineStyle::bold, true);
    CHECK (reverseOrder.documentText() == "Another heading");
    CHECK (reverseOrder.markdown() == "## ***Another heading***");
    CHECK (reverseOrder.selectionHasInlineStyle (anotherHeading,
                                                 NotepadDocument::InlineStyle::bold));
    CHECK (reverseOrder.selectionHasInlineStyle (anotherHeading,
                                                 NotepadDocument::InlineStyle::italic));
}

TEST_CASE ("Notepad inline formatting stays within each selected line",
           "[notepad][document]")
{
    NotepadDocument document;
    document.setMarkdown ("# **Heading**\n\nBody");

    document.setDocumentInlineStyle ({ 0, document.documentText().size() },
                                     NotepadDocument::InlineStyle::italic, true);

    CHECK (document.documentText() == "Heading\n\nBody");
    CHECK (document.markdown() == "# ***Heading***\n\n*Body*");
    CHECK (document.selectionHasInlineStyle ({ 0, 7 },
                                             NotepadDocument::InlineStyle::italic));
    CHECK (document.selectionHasInlineStyle ({ 9, 13 },
                                             NotepadDocument::InlineStyle::italic));
}

TEST_CASE ("Notepad caret formatting preserves character order while typing",
           "[notepad][document]")
{
    NotepadDocument document;
    document.setMarkdown ("- Second item");

    auto caret = document.documentText().find ("item") + 2;
    for (const auto ch : std::string (" strong"))
    {
        auto edited = document.documentText();
        edited.insert (caret, 1, ch);
        REQUIRE (document.replaceDocumentText (edited));
        document.setDocumentInlineStyle ({ caret, caret + 1 },
                                         NotepadDocument::InlineStyle::bold, true);
        ++caret;
    }

    CHECK (document.documentText() == "Second it strongem");
    CHECK (document.markdown() == "- Second it** strong**em");
    CHECK (document.selectionHasInlineStyle ({ 9, 16 },
                                             NotepadDocument::InlineStyle::bold));

    for (const auto ch : std::string (" plain"))
    {
        auto edited = document.documentText();
        edited.insert (caret, 1, ch);
        REQUIRE (document.replaceDocumentText (edited));
        document.setDocumentInlineStyle ({ caret, caret + 1 },
                                         NotepadDocument::InlineStyle::bold, false);
        ++caret;
    }

    CHECK (document.documentText() == "Second it strong plainem");
    CHECK (document.markdown() == "- Second it** strong** plainem");
}

TEST_CASE ("Notepad document exposes list, task, and link presentation metadata",
           "[notepad][document]")
{
    NotepadDocument document;
    document.setMarkdown ("- bullet\n7. numbered\n- [ ] task\n"
                          "[Dusk Studio](https://dusk.audio)");

    const auto bullet = document.documentText().find ("bullet");
    const auto numbered = document.documentText().find ("numbered");
    const auto task = document.documentText().find ("task");
    const auto link = document.documentText().find ("Dusk Studio");
    REQUIRE (bullet != std::string::npos);
    REQUIRE (numbered != std::string::npos);
    REQUIRE (task != std::string::npos);
    REQUIRE (link != std::string::npos);

    CHECK (document.lineInfoAt (bullet).block == NotepadDocument::BlockStyle::bullets);
    CHECK (document.lineInfoAt (numbered).orderedNumber == 7);
    CHECK_FALSE (document.lineInfoAt (task).taskChecked);
    document.toggleTaskAt (task);
    CHECK (document.lineInfoAt (task).taskChecked);
    CHECK (document.linkTargetAt (link) == "https://dusk.audio");
    CHECK (document.styleAt (link).link);

    document.renumberOrderedRunAt (numbered);
    CHECK (document.lineInfoAt (numbered).orderedNumber == 1);
}

TEST_CASE ("Notepad source and document selections round-trip across hidden syntax",
           "[notepad][document]")
{
    NotepadDocument document;
    document.setMarkdown ("# A **bold** [link](https://example.com)");
    const auto start = document.documentText().find ("bold");
    REQUIRE (start != std::string::npos);
    const NotepadDocument::Selection projected { start, start + 4 };
    const auto source = document.sourceSelection (projected);
    const auto roundTrip = document.documentSelection (source);
    CHECK (roundTrip.start == projected.start);
    CHECK (roundTrip.end == projected.end);
}

// orderedPrefixLength accepts any run of digits, so the ordered-number
// accumulator has to stop before it overflows a signed int.
TEST_CASE ("Notepad ordered-list numbers survive an absurd digit run",
           "[notepad][document]")
{
    NotepadDocument document;
    document.setMarkdown ("99999999999999999999. item");

    const auto info = document.lineInfoAt (0);
    CHECK (info.block == NotepadDocument::BlockStyle::numbers);
    // The accumulator stops before the multiply-add that would overflow:
    // nine 9s land, the tenth trips the guard.
    CHECK (info.orderedNumber == 999999999);
}
