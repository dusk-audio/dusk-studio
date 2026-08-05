#pragma once

#include "NotepadDocument.h"
#include "NotepadEditorCore.h"

#include <DearImGui.hpp>

#include <functional>
#include <string>

namespace duskstudio
{
// The notepad's editor, driving both views: the document projection and the
// Markdown source, which differ only in what NotepadDocument projects. It owns
// wrapping, caret, selection, input and painting; every text mutation goes
// through the projection so the Markdown source stays authoritative.
class NotepadEditor final
{
public:
    struct Fonts
    {
        ImFont* body = nullptr;
        ImFont* bold = nullptr;
        ImFont* italic = nullptr;
        ImFont* boldItalic = nullptr;
        ImFont* mono = nullptr;
    };

    NotepadEditor (NotepadDocument& documentToEdit, notepad::UndoStack& undoHistory);
    NotepadEditor (const NotepadEditor&) = delete;
    NotepadEditor& operator= (const NotepadEditor&) = delete;

    std::function<void()> onDocumentChanged;
    std::function<void (const std::string&)> onLinkActivated;
    std::function<void()> onUndoRequested;
    std::function<void()> onRedoRequested;

    void setFonts (const Fonts& value);
    void setDarkPage (bool dark) noexcept { darkPage = dark; }

    // Adopts the document as it stands now - used on open, mode switch and
    // history restore, and whenever an edit was refused by the model.
    void reset (NotepadDocument::Selection value);
    void setSelection (NotepadDocument::Selection value);
    NotepadDocument::Selection selection() const noexcept;
    void requestFocus() noexcept { focusRequested = true; }

    // Distance from the top of the viewport to the caret's row. Captured
    // before a mode switch and handed back after it, so the line under the
    // reader's eye stays where it was even though the two projections wrap at
    // different offsets.
    float caretViewportOffset() const noexcept;
    void keepCaretAtViewportOffset (float offset) noexcept;

    void draw (float bodySize);

    void applyInlineStyle (NotepadDocument::InlineStyle style);
    void applyBlockStyle (NotepadDocument::BlockStyle style);
    void insertLink (const std::string& url);

    // Opens the chord slot over the word at the caret, seeded with the chord
    // already anchored there. Enter commits, Esc cancels, an empty name
    // removes the chord.
    void beginChordEntry();
    bool chordEntryActive() const noexcept { return chordEditing; }
    // Sharps going up, flats coming down: the spelling a chart would use for
    // the direction the singer asked for.
    void transposeChords (int semitones);
    bool inlineStyleActive (NotepadDocument::InlineStyle style) const;
    NotepadDocument::BlockStyle blockStyle() const;

private:
    struct StickyStyle
    {
        bool active = false;
        bool bold = false;
        bool italic = false;
        bool code = false;
    };

    ImFont* fontForStyle (NotepadDocument::TextStyle style,
                          NotepadDocument::BlockStyle block) const noexcept;
    float measureRange (std::size_t begin, std::size_t end, float fontSize,
                        NotepadDocument::BlockStyle block) const;

    void ensureLayout (float width, float bodySize);
    // Closes a mutation: exactly one call per edit, after every model operation
    // that edit performs.
    void documentMutated();
    notepad::Snapshot snapshot() const;

    void moveCaret (std::size_t offset, bool extend, bool preferRowEnd = false);
    void moveVertical (int direction, bool extend, float bodySize);
    void movePage (int direction, bool extend, float viewportHeight, float bodySize);
    void selectAll();
    void keepCaretVisible (float viewportHeight, float bodySize);

    // Applies the edit and records it; the caller owns the closing
    // documentMutated() so post-edit model work joins the same notification.
    bool replaceRange (std::size_t start, std::size_t end, const std::string& insert,
                       notepad::EditKind kind);
    void insertText (const std::string& value);
    void insertNewline();
    void deleteBackward (bool wholeWord);
    void deleteForward (bool wholeWord);
    void toggleTaskLine (std::size_t lineStart);
    void toggleSticky (NotepadDocument::InlineStyle style);
    void applyStickyStyles (NotepadDocument::Selection inserted);
    bool& stickyValue (NotepadDocument::InlineStyle style) noexcept;

    void handleMouse (ImVec2 frameMin, ImVec2 frameMax, bool hovered, float bodySize);
    void handleKeyboard (float viewportHeight, float bodySize);
    // Consumes input while the chord slot is open. Returns false once it has
    // closed, so the same frame's remaining keys reach the text editor.
    bool handleChordEntry();
    void commitChordEntry();
    void render (ImVec2 frameMin, ImVec2 frameMax, float bodySize, bool active) const;

    std::size_t hitTest (ImVec2 position, ImVec2 frameMin, float bodySize) const;
    void resetBlink();

    NotepadDocument& document;
    notepad::UndoStack& history;
    notepad::MeasureFn measure;
    notepad::Layout layout;
    Fonts fonts;
    StickyStyle sticky;

    std::size_t caret = 0;
    std::size_t anchor = 0;
    std::size_t chordAnchor = 0;
    std::string chordDraft;
    bool chordEditing = false;
    float desiredX = -1.0f;
    float scrollY = 0.0f;
    float pendingCaretViewport = 0.0f;
    bool caretViewportPending = false;
    float layoutWidth = -1.0f;
    float layoutBodySize = -1.0f;
    double blinkStart = 0.0;
    bool layoutDirty = true;
    bool caretAtRowEnd = false;
    bool scrollToCaret = false;
    bool focusRequested = false;
    bool keepFocus = false;
    bool draggingSelection = false;
    bool draggingScrollbar = false;
    bool darkPage = true;
};
} // namespace duskstudio
