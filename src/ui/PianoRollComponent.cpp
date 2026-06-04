#include "PianoRollComponent.h"
#include "DuskContextMenu.h"
#include "EditCursors.h"
#include "EditModeToolbar.h"
#include "EmbeddedModal.h"
#include "../engine/AudioEngine.h"
#include "../engine/Transport.h"
#include "../session/RegionEditActions.h"
#include <algorithm>
#include <limits>
#include <map>

namespace duskstudio
{
namespace
{
// Process-wide static modal for the piano roll's text-input dialogs
// (region label rename). Mirrors the sharedAlertModal pattern in
// PluginPickerHelpers — one in-window EmbeddedModal at a time, no
// per-call instance tracking, idempotent close.
EmbeddedModal& sharedPianoRollTextInputModal()
{
    static EmbeddedModal m;
    return m;
}

// Dusk-styled text-input panel — title + prompt + TextEditor + OK /
// Cancel. Replaces juce::AlertWindow + addTextEditor for the piano
// roll's rename flow so the dialog renders inside the main window's
// component tree and doesn't drop a native popup onto the X11 /
// Wayland stack.
class TextInputPanel final : public juce::Component
{
public:
    TextInputPanel (juce::String title, juce::String prompt,
                      juce::String initial,
                      std::function<void (juce::String)> onAccept)
        : titleStr (std::move (title)),
          promptStr (std::move (prompt)),
          onAcceptFn (std::move (onAccept))
    {
        setOpaque (true);
        editor.setText (initial, false);
        editor.setSelectAllWhenFocused (true);
        editor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff15151c));
        editor.setColour (juce::TextEditor::outlineColourId,    juce::Colour (0xff3a3a42));
        editor.setColour (juce::TextEditor::textColourId,       juce::Colours::white);
        editor.setColour (juce::TextEditor::highlightColourId,  juce::Colour (0xff5a4880));
        editor.onReturnKey = [this] { commit(); };
        editor.onEscapeKey = [this] { dismiss(); };
        addAndMakeVisible (editor);

        auto style = [] (juce::TextButton& b, juce::Colour fill)
        {
            b.setColour (juce::TextButton::buttonColourId,   fill);
            b.setColour (juce::TextButton::buttonOnColourId, fill.brighter (0.15f));
            b.setColour (juce::TextButton::textColourOffId,  juce::Colours::white);
            b.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
            b.setMouseClickGrabsKeyboardFocus (false);
        };
        style (okBtn,     juce::Colour (0xff5a4880));
        style (cancelBtn, juce::Colour (0xff262630));
        okBtn.onClick     = [this] { commit(); };
        cancelBtn.onClick = [this] { dismiss(); };
        addAndMakeVisible (okBtn);
        addAndMakeVisible (cancelBtn);

        setSize (420, 160);
        setWantsKeyboardFocus (true);
    }

    // Set BEFORE EmbeddedModal::show so the panel can close the modal
    // on accept/cancel without holding a back-reference to the modal.
    std::function<void()> onDismissFn;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1a1a22));
        auto r = getLocalBounds().reduced (18);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (14.5f, juce::Font::bold)));
        g.drawText (titleStr, r.removeFromTop (22),
                     juce::Justification::topLeft, false);
        r.removeFromTop (6);
        g.setColour (juce::Colour (0xffc0c0c8));
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText (promptStr, r.removeFromTop (18),
                     juce::Justification::topLeft, false);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (18);
        r.removeFromTop (22 + 6 + 18 + 8);  // title + prompt + padding
        editor.setBounds (r.removeFromTop (26));
        r.removeFromTop (14);
        auto buttons = r.removeFromTop (28);
        cancelBtn.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        okBtn    .setBounds (buttons.removeFromRight (90));
    }

    void visibilityChanged() override
    {
        if (isVisible())
            editor.grabKeyboardFocus();
    }

private:
    void commit()
    {
        if (onAcceptFn) onAcceptFn (editor.getText());
        dismiss();
    }
    void dismiss() { if (onDismissFn) onDismissFn(); }

    juce::String titleStr, promptStr;
    juce::TextEditor editor;
    juce::TextButton okBtn     { "OK" };
    juce::TextButton cancelBtn { "Cancel" };
    std::function<void (juce::String)> onAcceptFn;
};

void showEmbeddedTextInput (juce::Component& parent,
                              juce::String title, juce::String prompt,
                              juce::String initialValue,
                              std::function<void (juce::String)> onAccept)
{
    auto panel = std::make_unique<TextInputPanel> (std::move (title),
                                                      std::move (prompt),
                                                      std::move (initialValue),
                                                      std::move (onAccept));
    panel->onDismissFn = [] { sharedPianoRollTextInputModal().close(); };
    sharedPianoRollTextInputModal().show (parent, std::move (panel),
        /*onDismiss*/ [] { sharedPianoRollTextInputModal().close(); });
}
const juce::Colour kBgDark        { 0xff181820 };
const juce::Colour kRowWhite      { 0xff1c1c24 };
const juce::Colour kRowBlack      { 0xff141418 };
const juce::Colour kGridLine      { 0xff2a2a32 };
const juce::Colour kBeatLine      { 0xff3c3c46 };
const juce::Colour kBarLine       { 0xff5a5a64 };
const juce::Colour kHeaderBg      { 0xff202028 };
const juce::Colour kHeaderText    { 0xffb0b0b8 };
const juce::Colour kKeyWhite      { 0xffe0e0e6 };
const juce::Colour kKeyBlack      { 0xff242428 };
const juce::Colour kKeyText       { 0xff404048 };
const juce::Colour kKeyOctaveLine { 0xff8a8a94 };
const juce::Colour kNoteFill      { 0xff70b0e0 };
const juce::Colour kNoteSelected  { 0xffffd060 };
const juce::Colour kNoteEdge      { 0xff141418 };
// Soft cream for the live transport playhead - distinct enough from
// the gold edit cursor (kNoteSelected) that both can coexist on the
// grid without competing.
const juce::Colour kTransportPlayhead { 0xffffe8c0 };

// Same palette TapeStrip + AudioRegionEditor use - keep all three
// surfaces in sync when colours change.
struct PaletteEntry { const char* label; juce::uint32 argb; };
constexpr PaletteEntry kPalette[] = {
    { "Reset to track colour", 0x00000000 },
    { "Red",     0xffd05f5f }, { "Orange",  0xffd09060 },
    { "Yellow",  0xffd0c060 }, { "Green",   0xff60c070 },
    { "Cyan",    0xff60c0c0 }, { "Blue",    0xff6090d0 },
    { "Purple",  0xff9070c0 }, { "Magenta", 0xffc060a0 },
};
constexpr int kPaletteCount = (int) (sizeof (kPalette) / sizeof (kPalette[0]));
} // namespace

// Storage for the static note clipboard. Initially empty.
std::vector<MidiNote> PianoRollComponent::sNoteClipboard;

namespace
{

// Pitch-class palette. Twelve distinct hues, manually picked rather
// than a pure hue-cycle so the chromatic neighbours stay visually
// distinct (a pure cycle puts e.g. F#/G next to each other at almost
// identical hues, which defeats the purpose). Indexed by note % 12;
// C = 0, B = 11.
const juce::Colour kPitchClassPalette[12] = {
    juce::Colour (0xffe0464a),  // C   - red
    juce::Colour (0xffe07f2a),  // C#  - orange
    juce::Colour (0xffe0c050),  // D   - amber
    juce::Colour (0xffb0d050),  // D#  - lime
    juce::Colour (0xff60c060),  // E   - green
    juce::Colour (0xff40c0a8),  // F   - teal
    juce::Colour (0xff50b0d0),  // F#  - cyan
    juce::Colour (0xff5090e0),  // G   - sky blue
    juce::Colour (0xff7068d8),  // G#  - indigo
    juce::Colour (0xff9858c8),  // A   - violet
    juce::Colour (0xffc060c0),  // A#  - magenta
    juce::Colour (0xffe06090),  // B   - rose
};

bool isBlackKey (int noteNumber) noexcept
{
    static constexpr bool blacks[12] =
        { false, true, false, true, false, false, true, false, true, false, true, false };
    return blacks[(noteNumber + 1200) % 12];
}

const char* noteNameForC (int noteNumber)
{
    // Returns labels only for C of each octave (e.g. "C4"); empty for others.
    // Pre-built static buffer is fine because the caller copies into a juce::String.
    if (noteNumber % 12 != 0) return "";
    static char buf[16];
    const int octave = (noteNumber / 12) - 1;     // MIDI 60 = C4
    std::snprintf (buf, sizeof (buf), "C%d", octave);
    return buf;
}
} // namespace

PianoRollComponent::PianoRollComponent (Session& s, AudioEngine& e, int t, int r)
    : session (s), engine (e), trackIdx (t), regionIdx (r)
{
    setOpaque (true);
    setWantsKeyboardFocus (true);

    // Status-bar readouts. Read-only labels styled to match Reaper's
    // small-grey-on-dark aesthetic. setInterceptsMouseClicks(false,
    // false) keeps the label transparent to the rest of the routing.
    auto styleReadout = [] (juce::Label& l, juce::Justification just)
    {
        l.setJustificationType (just);
        l.setColour (juce::Label::textColourId, juce::Colour (0xffd0d0d8));
        l.setColour (juce::Label::backgroundColourId, juce::Colour (0xff181820));
        l.setColour (juce::Label::outlineColourId,    juce::Colour (0xff3a3a44));
        l.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        l.setEditable (false, false, false);
    };
    styleReadout (positionLabel, juce::Justification::centredLeft);
    styleReadout (valueLabel,    juce::Justification::centredLeft);
    styleReadout (trackLabel,    juce::Justification::centredRight);
    addAndMakeVisible (positionLabel);
    addAndMakeVisible (valueLabel);
    addAndMakeVisible (trackLabel);

    // Combo boxes - JUCE's standard control with the look-and-feel
    // already in use across the app (Audio settings, mastering
    // limiter, channel strip).
    auto styleCombo = [] (juce::ComboBox& c)
    {
        c.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff181820));
        c.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xff3a3a44));
        c.setColour (juce::ComboBox::textColourId,       juce::Colour (0xffd0d0d8));
        c.setColour (juce::ComboBox::arrowColourId,      juce::Colour (0xff8090a0));
    };

    // Item IDs: 1=1/1, 2=1/2, 3=1/4, 4=1/8, 5=1/16, 6=1/32, 7=Off.
    gridCombo.addItem ("1/1",  1);
    gridCombo.addItem ("1/2",  2);
    gridCombo.addItem ("1/4",  3);
    gridCombo.addItem ("1/8",  4);
    gridCombo.addItem ("1/16", 5);
    gridCombo.addItem ("1/32", 6);
    gridCombo.addItem ("Off",  7);
    gridCombo.setSelectedId (5, juce::dontSendNotification);   // 1/16 default
    gridCombo.onChange = [this]
    {
        switch (gridCombo.getSelectedId())
        {
            case 1: snapTicks = (juce::int64) kMidiTicksPerQuarter * 4; break;
            case 2: snapTicks = (juce::int64) kMidiTicksPerQuarter * 2; break;
            case 3: snapTicks = (juce::int64) kMidiTicksPerQuarter;     break;
            case 4: snapTicks = (juce::int64) kMidiTicksPerQuarter / 2; break;
            case 5: snapTicks = (juce::int64) kMidiTicksPerQuarter / 4; break;
            case 6: snapTicks = (juce::int64) kMidiTicksPerQuarter / 8; break;
            case 7: snapTicks = 0; break;
            default: break;
        }
        repaint();
    };
    styleCombo (gridCombo);
    addAndMakeVisible (gridCombo);

    notesCombo.addItem ("Grid",    1);
    notesCombo.addItem ("Free",    2);
    notesCombo.addItem ("Triplet", 3);
    notesCombo.addItem ("Dotted",  4);
    notesCombo.setSelectedId (1, juce::dontSendNotification);
    notesCombo.onChange = [this]
    {
        noteEntryMode = (NoteEntryMode) (notesCombo.getSelectedId() - 1);
    };
    styleCombo (notesCombo);
    addAndMakeVisible (notesCombo);

    colorCombo.addItem ("Pitch",    1);
    colorCombo.addItem ("Velocity", 2);
    colorCombo.addItem ("Channel",  3);
    colorCombo.setSelectedId (1, juce::dontSendNotification);
    colorCombo.onChange = [this]
    {
        colorMode = (ColorMode) (colorCombo.getSelectedId() - 1);
        repaint();
    };
    styleCombo (colorCombo);
    addAndMakeVisible (colorCombo);

    keySnapToggle.setButtonText ("Key snap");
    keySnap = session.pianoRollKeySnap;
    keySnapToggle.setToggleState (keySnap, juce::dontSendNotification);
    keySnapToggle.setColour (juce::ToggleButton::textColourId,
                              juce::Colour (0xffd0d0d8));
    keySnapToggle.onClick = [this]
    {
        keySnap = keySnapToggle.getToggleState();
        session.pianoRollKeySnap = keySnap;   // persist across reopens
    };
    addAndMakeVisible (keySnapToggle);

    // Region-level mute / lock toggles - parity with AudioRegionEditor.
    // Submit through MidiRegionEditAction so Cmd+Z reverts.
    auto styleToggle = [] (juce::ToggleButton& t)
    {
        t.setColour (juce::ToggleButton::textColourId, juce::Colour (0xffd0d0d8));
    };
    muteToggle.setButtonText ("Mute");
    lockToggle.setButtonText ("Lock");
    styleToggle (muteToggle);
    styleToggle (lockToggle);
    if (auto* mr = region())
    {
        muteToggle.setToggleState (mr->muted,  juce::dontSendNotification);
        lockToggle.setToggleState (mr->locked, juce::dontSendNotification);
    }
    muteToggle.onClick = [this]
    {
        auto* r = region();
        if (r == nullptr) return;
        const MidiRegion before = *r;
        MidiRegion after = before;
        after.muted = muteToggle.getToggleState();
        auto& um = engine.getUndoManager();
        um.beginNewTransaction (after.muted ? "Mute MIDI region" : "Unmute MIDI region");
        um.perform (new MidiRegionEditAction (session, engine, trackIdx, regionIdx, before, after));
        repaint();
    };
    lockToggle.onClick = [this]
    {
        auto* r = region();
        if (r == nullptr) return;
        const MidiRegion before = *r;
        MidiRegion after = before;
        after.locked = lockToggle.getToggleState();
        auto& um = engine.getUndoManager();
        um.beginNewTransaction (after.locked ? "Lock MIDI region" : "Unlock MIDI region");
        um.perform (new MidiRegionEditAction (session, engine, trackIdx, regionIdx, before, after));
        repaint();
    };
    addAndMakeVisible (muteToggle);
    addAndMakeVisible (lockToggle);

    // Top icon row - mirror of the chip popup actions, plus a few
    // mouse-only conveniences (zoom-fit, CC-lane toggle).
    addAndMakeVisible (undoButton);
    undoButton.setTooltip ("Undo");
    undoButton.onClick = [this] { engine.getUndoManager().undo(); repaint(); };

    addAndMakeVisible (redoButton);
    redoButton.setTooltip ("Redo");
    redoButton.onClick = [this] { engine.getUndoManager().redo(); repaint(); };

    addAndMakeVisible (splitButton);
    splitButton.setTooltip ("Split at edit cursor");
    splitButton.onClick = [this] { splitSelectedAtCursor(); };

    addAndMakeVisible (glueButton);
    glueButton.setTooltip ("Glue selection");
    glueButton.onClick = [this]
    {
        if (selectedNotes.size() >= 2) { glueSelectedNotes(); repaint(); }
    };

    addAndMakeVisible (quantizeButton);
    quantizeButton.setTooltip ("Quantize...");
    quantizeButton.onClick = [this] { showQuantizePopup(); };

    addAndMakeVisible (propertiesButton);
    propertiesButton.setTooltip ("Region properties...");
    propertiesButton.onClick = [this] { showRegionPropertiesPopup(); };

    addAndMakeVisible (zoomOutButton);
    zoomOutButton.setTooltip ("Zoom out (-)");
    zoomOutButton.onClick = [this] { zoomByFactor (1.0f / 1.15f); };

    addAndMakeVisible (zoomInButton);
    zoomInButton.setTooltip ("Zoom in (=)");
    zoomInButton.onClick = [this] { zoomByFactor (1.15f); };

    addAndMakeVisible (zoomFitButton);
    zoomFitButton.setTooltip ("Zoom to fit region");
    zoomFitButton.onClick = [this] { zoomFit(); };

    // Edit-mode palette (shared with AudioRegionEditor + TapeStrip).
    // PianoRoll only supports Grab (select / box-select) and Draw
    // (the pencil — sole note-creation tool); Range / Cut / Grid are
    // inert here so we hide them from the palette to keep the pencil
    // discoverable as THE drawing tool.
    editModeToolbar = std::make_unique<EditModeToolbar> (engine);
    editModeToolbar->setVisibleModes ({ EditMode::Grab, EditMode::Draw });
    editModeToolbar->onEditModeChanged = [this] { syncEditModeToolbar(); };
    addAndMakeVisible (editModeToolbar.get());

    addAndMakeVisible (toggleCcButton);
    toggleCcButton.setTooltip ("Show / hide CC lane");
    toggleCcButton.onClick = [this] { toggleCcLane(); };

    // Chase toggle - auto-scroll the grid to follow the transport
    // playhead when it leaves the visible window. Off by default.
    chaseToggle.setClickingTogglesState (true);
    chaseToggle.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff222226));
    chaseToggle.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff406030));
    chaseToggle.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff909094));
    chaseToggle.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xffe0e0e0));
    chaseToggle.setTooltip ("Scroll the view to follow the playhead when it leaves the visible grid");
    addAndMakeVisible (chaseToggle);

    horizontalScrollBar.setAutoHide (false);
    horizontalScrollBar.addListener (this);
    addAndMakeVisible (horizontalScrollBar);

    refreshStatusBarReadouts();

    // 30 Hz playhead poll - same cadence as the meters elsewhere in
    // the app. Runs unconditionally; the timer body is a no-op when
    // the transport hasn't moved.
    startTimerHz (30);

    // Force default-cursored child labels / buttons to defer to this
    // editor's stored cursor — see EditCursors.h comment for the
    // JUCE-default-NormalCursor-shadows-parent bug.
    inheritCursorOnDescendants (*this);
}

PianoRollComponent::~PianoRollComponent()
{
    stopTimer();
}

MidiRegion* PianoRollComponent::region()
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return nullptr;
    auto& v = session.track (trackIdx).midiRegions.currentMutable();
    if (regionIdx < 0 || regionIdx >= (int) v.size()) return nullptr;
    return &v[(size_t) regionIdx];
}

const MidiRegion* PianoRollComponent::region() const
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return nullptr;
    const auto& v = session.track (trackIdx).midiRegions.current();
    if (regionIdx < 0 || regionIdx >= (int) v.size()) return nullptr;
    return &v[(size_t) regionIdx];
}

int PianoRollComponent::yForNoteNumber (int n) const
{
    // High notes at the top (matches a piano keyboard's visual layout).
    // Note 127 → row 0, note 0 → row kNumKeys-1. Top band = toolbar
    // (kToolbarHeight) + bar ruler (kHeaderHeight).
    return kToolbarHeight + kHeaderHeight
         + (kNumKeys - 1 - n) * kNoteHeight - scrollY;
}

int PianoRollComponent::noteNumberForY (int y) const
{
    const int topBand = kToolbarHeight + kHeaderHeight;
    const int row = (y - topBand + scrollY) / kNoteHeight;
    return juce::jlimit (0, kNumKeys - 1, kNumKeys - 1 - row);
}

int PianoRollComponent::xForTick (juce::int64 tick) const
{
    return kKeyboardWidth - scrollX
         + (int) std::round ((double) tick * pixelsPerTick);
}

juce::int64 PianoRollComponent::tickForX (int x) const
{
    const auto t = (juce::int64) std::round (
        (double) (x - kKeyboardWidth + scrollX) / pixelsPerTick);
    return juce::jmax ((juce::int64) 0, t);
}

void PianoRollComponent::resized()
{
    layoutIconRow    (juce::Rectangle<int> (0, 0, getWidth(), kToolbarHeight));
    layoutStatusBar  (juce::Rectangle<int> (0, getHeight() - kStatusBarH,
                                                getWidth(), kStatusBarH));
    horizontalScrollBar.setBounds (kKeyboardWidth,
                                     getHeight() - kStatusBarH - kScrollBarH,
                                     juce::jmax (1, getWidth() - kKeyboardWidth),
                                     kScrollBarH);
    syncScrollBarRange();
    inheritCursorOnDescendants (*this);
}

void PianoRollComponent::syncScrollBarRange()
{
    // Total content width = max(scrollX + visible, region length in px).
    // The piano roll has no hard end; grow the scrollable extent with
    // the largest x the user has scrolled to so the thumb shrinks as
    // they explore further.
    const int gridW = juce::jmax (1, getWidth() - kKeyboardWidth);
    const auto* r = region();
    const double regionTicks = (r != nullptr) ? (double) r->lengthInTicks : 0.0;
    const double regionPx    = regionTicks * pixelsPerTick;
    const double total       = juce::jmax ((double) (scrollX + gridW),
                                              regionPx + (double) gridW);
    horizontalScrollBar.setRangeLimits (0.0, total, juce::dontSendNotification);
    horizontalScrollBar.setCurrentRange ((double) scrollX, (double) gridW,
                                            juce::dontSendNotification);
}

void PianoRollComponent::scrollBarMoved (juce::ScrollBar* bar, double newRangeStart)
{
    if (bar != &horizontalScrollBar) return;
    scrollX = juce::jmax (0, (int) std::round (newRangeStart));
    repaint();
}

void PianoRollComponent::layoutIconRow (juce::Rectangle<int> area)
{
    auto inner = area.reduced (8, 3);
    const int dia = juce::jmin (inner.getHeight(), 28);
    const int gap = 6;

    auto place = [&] (IconButton& b)
    {
        b.setBounds (inner.removeFromLeft (dia)
                          .withSizeKeepingCentre (dia, dia));
        inner.removeFromLeft (gap);
    };
    auto placeRight = [&] (IconButton& b)
    {
        b.setBounds (inner.removeFromRight (dia)
                          .withSizeKeepingCentre (dia, dia));
        inner.removeFromRight (gap);
    };
    // Zoom cluster glued to the far right.
    placeRight (zoomFitButton);
    placeRight (zoomInButton);
    placeRight (zoomOutButton);
    inner.removeFromRight (gap);
    placeRight (toggleCcButton);
    inner.removeFromRight (gap);
    {
        const int chaseW = 56;
        chaseToggle.setBounds (inner.removeFromRight (chaseW)
                                  .withSizeKeepingCentre (chaseW, dia));
        inner.removeFromRight (gap);
    }
    inner.removeFromRight (8);

    place (undoButton);
    place (redoButton);
    inner.removeFromLeft (gap);          // small group separator
    place (splitButton);
    place (glueButton);
    place (quantizeButton);
    place (propertiesButton);

    // Edit-mode palette sized to its internal layout (5 buttons + Snap
    // toggle + denomination dropdown).
    if (editModeToolbar != nullptr && inner.getWidth() > 0)
    {
        constexpr int kToolbarWidth = 390;
        const int w = juce::jmin (kToolbarWidth, inner.getWidth());
        editModeToolbar->setBounds (inner.removeFromLeft (w));
    }
}

void PianoRollComponent::syncEditModeToolbar()
{
    if (editModeToolbar != nullptr) editModeToolbar->syncFromSession();
    // Only paint the edit-mode cursor when the pointer is actually inside
    // the note grid; chrome (toolbar / header / keyboard / velocity & CC
    // strips / scrollbar / status bar) keeps the normal arrow. Piano roll
    // only implements Grab (select / move) and Draw (pencil) — Range /
    // Cut / Grid have no piano-roll click handler, so leave their cursor
    // at the normal arrow instead of advertising an action that won't
    // happen. Mirrors the equivalent guard in mouseMove.
    const auto mode = session.editMode;
    const auto p    = getMouseXYRelative();
    // Don't hand the overlay glyph the cursor mid-drag - a move/resize/box
    // gesture owns a native cursor, and forcing NoCursor here (on a mode
    // toolbar sync) would flash the overlay over it. Also skip when the pointer
    // is over a note/edge: mouseMove suppresses the overlay glyph there, so
    // hiding the native cursor would leave NO cursor at all until the next move.
    bool onEdge = false;
    const bool overNote = noteGridArea().contains (p) && hitTestNote (p.x, p.y, onEdge) >= 0;
    if (mode == EditMode::Draw
        && dragMode == DragMode::None
        && noteGridArea().contains (p)
        && ! overNote)
        // Draw's pencil glyph is overlay-painted; hide the native cursor.
        // Grab keeps the arrow on empty grid (hand only over a note).
        setMouseCursor (juce::MouseCursor::NoCursor);
    else
        setMouseCursor (juce::MouseCursor::NormalCursor);
}

juce::Rectangle<int> PianoRollComponent::noteGridArea() const noexcept
{
    const int topBandH = kToolbarHeight + kHeaderHeight;
    const int gridH    = juce::jmax (0,
                              getHeight() - topBandH - velocityStripH - ccStripH
                                - kStatusBarH - kScrollBarH);
    const int gridW    = juce::jmax (0, getWidth() - kKeyboardWidth);
    return { kKeyboardWidth, topBandH, gridW, gridH };
}

void PianoRollComponent::layoutStatusBar (juce::Rectangle<int> area)
{
    auto inner = area.reduced (8, 4);

    // Left cluster: position + value readouts.
    positionLabel.setBounds (inner.removeFromLeft (110));
    inner.removeFromLeft (4);
    valueLabel.setBounds    (inner.removeFromLeft (62));
    inner.removeFromLeft (10);

    // Centre cluster: grid / notes / color combos + key-snap toggle.
    auto placeLabelled = [&] (juce::ComboBox& c, int w)
    {
        c.setBounds (inner.removeFromLeft (w));
        inner.removeFromLeft (8);
    };
    placeLabelled (gridCombo,  82);
    placeLabelled (notesCombo, 92);
    placeLabelled (colorCombo, 100);
    keySnapToggle.setBounds (inner.removeFromLeft (96));
    inner.removeFromLeft (8);
    muteToggle.setBounds (inner.removeFromLeft (60));
    inner.removeFromLeft (4);
    lockToggle.setBounds (inner.removeFromLeft (60));
    inner.removeFromLeft (8);

    // Right cluster: track / channel readout, anchored to the right edge.
    trackLabel.setBounds (inner.removeFromRight (juce::jmin (180, inner.getWidth())));
}

juce::String PianoRollComponent::formatBarBeat (juce::int64 tick) const
{
    const int beatsPerBar = juce::jmax (1, session.beatsPerBar.load (std::memory_order_relaxed));
    const auto ticksPerBeat = (juce::int64) kMidiTicksPerQuarter;
    const auto ticksPerBar  = ticksPerBeat * beatsPerBar;
    const auto safeTick = juce::jmax ((juce::int64) 0, tick);
    const auto bar  = safeTick / ticksPerBar;
    const auto rem  = safeTick % ticksPerBar;
    const auto beat = rem / ticksPerBeat;
    const auto sub  = rem % ticksPerBeat;
    return juce::String ((int) (bar + 1)) + "."
         + juce::String ((int) (beat + 1)) + "."
         + juce::String ((int) sub).paddedLeft ('0', 3);
}

int PianoRollComponent::activeVelocity() const noexcept
{
    if (selectedNotes.empty()) return -1;
    const auto* r = region();
    if (r == nullptr) return -1;
    const int idx = selectedNotes.back();   // most-recently-added
    if (idx < 0 || idx >= (int) r->notes.size()) return -1;
    return r->notes[(size_t) idx].velocity;
}

void PianoRollComponent::refreshStatusBarReadouts()
{
    const auto mode = (TimeDisplayMode) session.timeDisplayMode.load (std::memory_order_relaxed);
    const double sr  = juce::jmax (1.0, engine.getCurrentSampleRate());
    const float  bpm = session.tempoBpm.load (std::memory_order_relaxed);
    const int    bpb = session.beatsPerBar.load (std::memory_order_relaxed);
    juce::int64 timelineSample = 0;
    if (auto* r = region())
        timelineSample = r->timelineStart + ticksToSamples (editCursorTick, sr, bpm);
    positionLabel.setText ("pos " + formatSamplePosition (timelineSample, sr, bpm, bpb, mode),
                              juce::dontSendNotification);
    const int v = activeVelocity();
    valueLabel.setText (v < 0 ? juce::String ("vel -")
                                : "vel " + juce::String (v),
                            juce::dontSendNotification);

    const auto* r = region();
    int channel = 1;
    if (r != nullptr && ! r->notes.empty()) channel = r->notes.front().channel;
    // Lead with the track NAME (tinted with the track colour) so you can tell
    // which track you're editing - the full-screen roll otherwise gives no
    // clue. Unnamed tracks (name still the default number) fall back to
    // "Track N". Matches the audio region editor's header.
    const auto& trk = session.track (trackIdx);
    const auto trackId = (trk.name == juce::String (trackIdx + 1))
                             ? juce::String ("Track ") + trk.name
                             : trk.name;
    trackLabel.setText (trackId + "  -  ch " + juce::String (channel),
                            juce::dontSendNotification);
    trackLabel.setColour (juce::Label::textColourId, trk.colour.brighter (0.3f));
    if (r != nullptr)
    {
        muteToggle.setToggleState (r->muted,  juce::dontSendNotification);
        lockToggle.setToggleState (r->locked, juce::dontSendNotification);
    }
}

void PianoRollComponent::paint (juce::Graphics& g)
{
    // Pull the status-bar labels up to date before any drawing - the
    // labels are children and JUCE will repaint them on their own pass,
    // so setting their text here (with dontSendNotification) is the
    // cheapest sync point for the parent gesture handlers.
    refreshStatusBarReadouts();

    g.fillAll (kBgDark);

    const auto bounds = getLocalBounds();
    // Top stack: toolbar (mode indicators + hotkey legend) over the
    // bar/beat ruler.
    const auto toolbarArea = bounds.withHeight (kToolbarHeight);
    const auto headerArea  = juce::Rectangle<int> (0, kToolbarHeight,
                                                      bounds.getWidth(), kHeaderHeight);
    const int  topBandH    = kToolbarHeight + kHeaderHeight;

    // Bottom strips stack: status bar at the very bottom, then CC
    // lane, then velocity lane. The note grid fills the space between
    // header and velocity lane.
    const int statusBarBottom = bounds.getBottom();
    // The horizontal scrollbar sits between the status bar and the CC
    // lane. ccBottom is the y-edge above which the CC strip + velocity
    // strip + note grid stack.
    const int scrollBarTop    = statusBarBottom - kStatusBarH - kScrollBarH;
    const int ccBottom        = scrollBarTop;
    const auto ccArea = juce::Rectangle<int> (kKeyboardWidth,
                                                 ccBottom - ccStripH,
                                                 bounds.getWidth() - kKeyboardWidth,
                                                 ccStripH);
    const auto velocityArea = juce::Rectangle<int> (kKeyboardWidth,
                                                       ccArea.getY() - velocityStripH,
                                                       bounds.getWidth() - kKeyboardWidth,
                                                       velocityStripH);
    const auto keyboardArea = juce::Rectangle<int> (0, topBandH,
                                                       kKeyboardWidth,
                                                       bounds.getHeight() - topBandH
                                                         - kStatusBarH - kScrollBarH);
    const auto gridArea    = juce::Rectangle<int> (kKeyboardWidth, topBandH,
                                                     bounds.getWidth() - kKeyboardWidth,
                                                     bounds.getHeight() - topBandH
                                                       - velocityStripH - ccStripH
                                                       - kStatusBarH - kScrollBarH);

    // Status-bar background fill - children are positioned in resized().
    const auto statusArea = juce::Rectangle<int> (0, ccBottom,
                                                     bounds.getWidth(), kStatusBarH);
    g.setColour (juce::Colour (0xff181820));
    g.fillRect (statusArea);
    g.setColour (kGridLine);
    g.drawHorizontalLine (statusArea.getY(), (float) statusArea.getX(),
                                                (float) statusArea.getRight());

    paintNoteGrid     (g, gridArea);
    paintToolbar      (g, toolbarArea);
    paintBeatRuler    (g, headerArea);
    paintKeyboard     (g, keyboardArea);
    paintNotes        (g, gridArea);
    paintLoopPunchBrackets (g, headerArea, gridArea);
    if (rangeActive)
    {
        const auto a = juce::jmin (rangeStartTick, rangeEndTick);
        const auto b = juce::jmax (rangeStartTick, rangeEndTick);
        const int xa = xForTick (a);
        const int xb = xForTick (b);
        if (xb > xa && xb > gridArea.getX() && xa < gridArea.getRight())
        {
            juce::Graphics::ScopedSaveState saved (g);
            g.reduceClipRegion (gridArea);
            g.setColour (juce::Colour (0xffffd060).withAlpha (0.14f));
            g.fillRect (xa, gridArea.getY(), xb - xa, gridArea.getHeight());
            g.setColour (juce::Colour (0xffffd060).withAlpha (0.55f));
            g.drawVerticalLine (xa, (float) gridArea.getY(), (float) gridArea.getBottom());
            g.drawVerticalLine (xb, (float) gridArea.getY(), (float) gridArea.getBottom());
        }
    }
    paintEditCursor   (g, gridArea);
    paintTransportPlayhead (g, gridArea);
    paintVelocityStrip (g, velocityArea);
    paintCcStrip      (g, ccArea);

    // Rubber-band overlay during BoxSelect drag. Rendered LAST so it
    // sits on top of every other layer.
    if (! rubberBand.isEmpty())
    {
        g.setColour (kNoteSelected.withAlpha (0.18f));
        g.fillRect (rubberBand);
        g.setColour (kNoteSelected.withAlpha (0.85f));
        g.drawRect (rubberBand, 1);
    }
}

void PianoRollComponent::paintLoopPunchBrackets (juce::Graphics& g,
                                                 juce::Rectangle<int> ruler,
                                                 juce::Rectangle<int> grid)
{
    const auto* r = region();
    if (r == nullptr) return;
    const double sr  = juce::jmax (1.0, engine.getCurrentSampleRate());
    const float  bpm = session.tempoBpm.load (std::memory_order_relaxed);
    auto& transport = engine.getTransport();

    auto xForSample = [&] (juce::int64 s)
    {
        return xForTick (samplesToTicks (s - r->timelineStart, sr, bpm));
    };
    auto drawPair = [&] (juce::int64 a, juce::int64 b, juce::Colour col, bool enabled)
    {
        if (b < a) return;
        const int xa = xForSample (a);
        const int xb = xForSample (b);
        juce::Graphics::ScopedSaveState saved (g);
        g.reduceClipRegion (juce::Rectangle<int> (grid.getX(), ruler.getY(),
                                                  grid.getWidth(),
                                                  grid.getBottom() - ruler.getY()));
        const float alpha = enabled ? 1.0f : 0.4f;
        // Zero-width = a single in/out point set, partner not placed (e.g. right
        // after Shift+[ before Shift+]). Draw one edge marker for feedback; a>0
        // guard keeps an unset (0,0) range invisible.
        if (b == a)
        {
            if (a <= 0 || xa < grid.getX() || xa > grid.getRight()) return;
            g.setColour (col.withAlpha (0.9f * alpha));
            g.drawVerticalLine (xa, (float) ruler.getY(), (float) grid.getBottom());
            g.fillRect (xa, ruler.getY(), 6, 3); g.fillRect (xa, ruler.getBottom() - 3, 6, 3);
            return;
        }
        if (xb < grid.getX() || xa > grid.getRight()) return;
        g.setColour (col.withAlpha (0.08f * alpha));
        g.fillRect (juce::Rectangle<int> (xa, grid.getY(),
                                          juce::jmax (1, xb - xa), grid.getHeight()));
        g.setColour (col.withAlpha (0.9f * alpha));
        g.drawVerticalLine (xa, (float) ruler.getY(), (float) grid.getBottom());
        g.drawVerticalLine (xb, (float) ruler.getY(), (float) grid.getBottom());
        g.fillRect (xa, ruler.getY(), 6, 3); g.fillRect (xa, ruler.getBottom() - 3, 6, 3);
        g.fillRect (xb - 5, ruler.getY(), 6, 3); g.fillRect (xb - 5, ruler.getBottom() - 3, 6, 3);
    };
    drawPair (transport.getLoopStart(), transport.getLoopEnd(),
              juce::Colour (0xff3aa860), transport.isLoopEnabled());
    drawPair (transport.getPunchIn(), transport.getPunchOut(),
              juce::Colour (0xffd05a5a), transport.isPunchEnabled());
}

void PianoRollComponent::paintToolbar (juce::Graphics& g, juce::Rectangle<int> area)
{
    // Solid background a touch lighter than the grid so the toolbar
    // reads as a separate band. 1 px hairline at the bottom to
    // visually separate it from the bar ruler below.
    g.setColour (juce::Colour (0xff20202c));
    g.fillRect (area);
    g.setColour (kGridLine);
    g.drawHorizontalLine (area.getBottom() - 1,
                            (float) area.getX(), (float) area.getRight());

    // Icon row paints itself via the IconButton children. The toolbar
    // band itself just paints its background + separator hairline.
}

void PianoRollComponent::paintNoteGrid (juce::Graphics& g, juce::Rectangle<int> area)
{
    // ScopedSaveState restores the clip region on scope exit so
    // subsequent paint passes (toolbar / ruler / keyboard / velocity
    // / CC) aren't clipped to the grid. Without this, only the grid
    // + notes render and every other UI band is invisible.
    juce::Graphics::ScopedSaveState saved (g);
    g.reduceClipRegion (area);

    // Row stripes - alternate so each octave reads as a unit. White-key
    // rows get the lighter shade, black-key rows the darker one.
    for (int n = 0; n < kNumKeys; ++n)
    {
        const int y = yForNoteNumber (n);
        if (y + kNoteHeight < area.getY() || y > area.getBottom()) continue;
        g.setColour (isBlackKey (n) ? kRowBlack : kRowWhite);
        g.fillRect (area.getX(), y, area.getWidth(), kNoteHeight);
    }

    // Scale-highlight overlay: shade rows whose note isn't in the
    // active scale so in-scale notes pop. Skipped (cheap branch) when
    // scale is Off.
    if (scale != Scale::Off)
    {
        g.setColour (juce::Colours::black.withAlpha (0.28f));
        for (int n = 0; n < kNumKeys; ++n)
        {
            if (isInScale (n)) continue;
            const int y = yForNoteNumber (n);
            if (y + kNoteHeight < area.getY() || y > area.getBottom()) continue;
            g.fillRect (area.getX(), y, area.getWidth(), kNoteHeight);
        }
    }

    // Octave lines (C of each octave gets a brighter horizontal line).
    g.setColour (kKeyOctaveLine.withAlpha (0.20f));
    for (int n = 0; n < kNumKeys; n += 12)
    {
        const int y = yForNoteNumber (n);
        if (y < area.getY() || y > area.getBottom()) continue;
        g.drawHorizontalLine (y + kNoteHeight - 1,
                                (float) area.getX(), (float) area.getRight());
    }

    // Vertical beat lines. Pull tempo + region length so we can draw bars
    // 4 beats apart (assumes 4/4 - we'll consume Session::beatsPerBar
    // when the model surfaces it more directly).
    const auto* r = region();
    if (r == nullptr || r->lengthInTicks <= 0) return;
    const int beatsPerBar = juce::jmax (1, session.beatsPerBar.load (std::memory_order_relaxed));
    const int ticksPerBeat = kMidiTicksPerQuarter;
    const int ticksPerBar  = ticksPerBeat * beatsPerBar;
    const int totalBars = (int) (r->lengthInTicks / ticksPerBar) + 1;
    // Subdivision lines at the active snap step - drawn first so the
    // beat / bar lines paint over them. Skipped when snap is Off, when
    // the snap step is one full beat (would just duplicate beat lines),
    // or when zoomed too far out to read.
    const float pxPerBeat = (float) ticksPerBeat * pixelsPerTick;
    if (snapTicks > 0 && snapTicks < ticksPerBeat && pxPerBeat >= 28.0f)
    {
        g.setColour (kBeatLine.withAlpha (0.35f));
        const int subTicksTotal = (int) ((juce::int64) totalBars * ticksPerBar);
        for (juce::int64 t = 0; t <= subTicksTotal; t += snapTicks)
        {
            if (t % ticksPerBeat == 0) continue;   // beat / bar lines paint themselves
            const int sx = xForTick (t);
            if (sx < area.getX() || sx > area.getRight()) continue;
            g.drawVerticalLine (sx, (float) area.getY(), (float) area.getBottom());
        }
    }

    for (int bar = 0; bar <= totalBars; ++bar)
    {
        const auto barTick = (juce::int64) bar * ticksPerBar;
        const int  bx = xForTick (barTick);
        if (bx >= area.getX() && bx <= area.getRight())
        {
            // 2 px wide filled rect so the bar line reads as the
            // dominant rhythmic anchor against the lighter beat /
            // subdivision lines.
            g.setColour (kBarLine);
            g.fillRect (bx, area.getY(), 2, area.getHeight());
        }
        for (int beat = 1; beat < beatsPerBar; ++beat)
        {
            const int  bex = xForTick (barTick + beat * ticksPerBeat);
            if (bex < area.getX() || bex > area.getRight()) continue;
            g.setColour (kBeatLine);
            g.drawVerticalLine (bex, (float) area.getY(), (float) area.getBottom());
        }
    }

    // Region end shading - dim the area past the region's last tick so
    // the editable extent is visually obvious.
    const int endX = xForTick (r->lengthInTicks);
    if (endX < area.getRight())
    {
        g.setColour (juce::Colours::black.withAlpha (0.40f));
        g.fillRect (endX, area.getY(), area.getRight() - endX, area.getHeight());
    }
}

void PianoRollComponent::paintBeatRuler (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (kHeaderBg);
    g.fillRect (area);
    g.setColour (kGridLine);
    g.drawHorizontalLine (area.getBottom() - 1,
                            (float) area.getX(), (float) area.getRight());

    const auto* r = region();
    if (r == nullptr || r->lengthInTicks <= 0) return;
    const int beatsPerBar = juce::jmax (1, session.beatsPerBar.load (std::memory_order_relaxed));
    const int ticksPerBeat = kMidiTicksPerQuarter;
    const int ticksPerBar = ticksPerBeat * beatsPerBar;
    const int totalBars = (int) (r->lengthInTicks / ticksPerBar) + 1;

    g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
    g.setColour (kHeaderText);

    const auto mode = (TimeDisplayMode) session.timeDisplayMode.load (std::memory_order_relaxed);
    if (mode == TimeDisplayMode::Bars)
    {
        const float pxPerBeat = (float) ticksPerBeat * pixelsPerTick;
        const bool showSubBeats = pxPerBeat >= 40.0f;
        for (int bar = 0; bar <= totalBars; ++bar)
        {
            const int bx = xForTick ((juce::int64) bar * ticksPerBar);
            if (bx >= kKeyboardWidth && bx <= area.getRight())
            {
                g.drawText (juce::String (bar + 1), bx + 3, area.getY(),
                             48, area.getHeight(), juce::Justification::centredLeft, false);
            }
            if (! showSubBeats) continue;
            g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::plain)));
            g.setColour (kHeaderText.withAlpha (0.65f));
            for (int beat = 1; beat < beatsPerBar; ++beat)
            {
                const int bbx = xForTick ((juce::int64) bar * ticksPerBar
                                              + (juce::int64) beat * ticksPerBeat);
                if (bbx < kKeyboardWidth || bbx > area.getRight()) continue;
                g.drawText (juce::String (bar + 1) + "." + juce::String (beat + 1),
                             bbx + 3, area.getY(),
                             48, area.getHeight(),
                             juce::Justification::centredLeft, false);
            }
            g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
            g.setColour (kHeaderText);
        }
    }
    else
    {
        // Time mode - label every Nth second (5 / 10 / 30 by zoom).
        const double sr  = juce::jmax (1.0, engine.getCurrentSampleRate());
        const float  bpm = session.tempoBpm.load (std::memory_order_relaxed);
        if (bpm <= 0.0f) return;
        const double samplesPerTick = sr * 60.0 / ((double) bpm * (double) kMidiTicksPerQuarter);
        const double pxPerSec = pixelsPerTick / samplesPerTick * sr;
        double tickEverySec = 1.0;
        if      (pxPerSec < 16.0) tickEverySec = 30.0;
        else if (pxPerSec < 40.0) tickEverySec = 10.0;
        else if (pxPerSec < 80.0) tickEverySec = 5.0;

        const double regionLenSec = (double) r->lengthInSamples / sr;
        const double regionStartSec = (double) r->timelineStart / sr;
        for (double sec = 0.0; sec <= regionLenSec + 0.5; sec += tickEverySec)
        {
            const auto t = (juce::int64) std::round (samplesToTicks ((juce::int64) (sec * sr),
                                                                        sr, bpm));
            const int bx = xForTick (t);
            if (bx < kKeyboardWidth || bx > area.getRight()) continue;
            const double absSec = regionStartSec + sec;
            const int mins = (int) (absSec / 60.0);
            const int secs = (int) absSec % 60;
            g.drawText (juce::String::formatted ("%d:%02d", mins, secs),
                         bx + 3, area.getY(), 56, area.getHeight(),
                         juce::Justification::centredLeft, false);
        }
    }

    // Snap / colour / scale / CC indicators moved up to the new
    // toolbar (paintToolbar). Keep the bar ruler focused on bar +
    // beat numbers.
}

void PianoRollComponent::paintKeyboard (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (kBgDark);
    g.fillRect (area);

    for (int n = 0; n < kNumKeys; ++n)
    {
        const int y = yForNoteNumber (n);
        if (y + kNoteHeight < area.getY() || y > area.getBottom()) continue;
        const bool black = isBlackKey (n);
        g.setColour (black ? kKeyBlack : kKeyWhite);
        const int rectX = area.getX();
        const int rectW = black ? area.getWidth() - 14 : area.getWidth() - 1;
        g.fillRect (rectX, y, rectW, kNoteHeight - 1);

        if (! black && (n % 12 == 0))
        {
            g.setColour (kKeyText);
            g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
            g.drawText (noteNameForC (n),
                         area.getRight() - 30, y, 26, kNoteHeight,
                         juce::Justification::centredRight, false);
        }
    }

    // Right-edge separator between keyboard and grid.
    g.setColour (kGridLine);
    g.drawVerticalLine (area.getRight() - 1,
                         (float) area.getY(), (float) area.getBottom());
}

void PianoRollComponent::paintNotes (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto* r = region();
    if (r == nullptr) return;

    // Cull against the actual incoming dirty rect, not the full gridArea.
    // The 30 Hz playhead's partial-repaint band is only ~5 px wide; without
    // this, every visible note ran setColour + fillRect + drawRect on
    // every tick even when only the playhead strip needed to change,
    // dropping frames on busy regions and making the playhead jerky.
    const auto dirty = g.getClipBounds().getIntersection (area);
    if (dirty.isEmpty()) return;
    juce::Graphics::ScopedSaveState saved (g);
    g.reduceClipRegion (area);

    for (int i = 0; i < (int) r->notes.size(); ++i)
    {
        const auto& n = r->notes[(size_t) i];
        const int x  = xForTick (n.startTick);
        const int x2 = xForTick (n.startTick + n.lengthInTicks);
        const int y  = yForNoteNumber (n.noteNumber);
        if (x2 < dirty.getX() || x > dirty.getRight()) continue;
        if (y + kNoteHeight < dirty.getY() || y > dirty.getBottom()) continue;

        const auto rect = juce::Rectangle<int> (x, y + 1, juce::jmax (2, x2 - x), kNoteHeight - 2);
        const bool selected = isNoteSelected (i);
        const auto fill = selected ? kNoteSelected : colourForNote (n);
        g.setColour (fill);
        g.fillRect (rect);
        g.setColour (kNoteEdge);
        g.drawRect (rect, 1);
    }
}

void PianoRollComponent::paintEditCursor (juce::Graphics& g, juce::Rectangle<int> gridArea)
{
    const int cx = xForTick (editCursorTick);
    if (cx < gridArea.getX() - 1 || cx > gridArea.getRight() + 1) return;

    juce::Graphics::ScopedSaveState saved (g);
    g.reduceClipRegion (gridArea);
    // Reaper-style gold cursor. Slight alpha so the line reads as
    // "marker" rather than "transport playhead" - the latter would
    // be solid yellow and is reserved for when transport playhead
    // gets added.
    g.setColour (juce::Colour (0xffffd060).withAlpha (0.65f));
    g.drawVerticalLine (cx, (float) gridArea.getY(), (float) gridArea.getBottom());
    // Tiny triangle anchor at the top so the cursor is visible even
    // when it falls right under a note.
    juce::Path tri;
    tri.addTriangle ((float) cx - 4.0f, (float) gridArea.getY(),
                       (float) cx + 4.0f, (float) gridArea.getY(),
                       (float) cx,         (float) gridArea.getY() + 5.0f);
    g.setColour (juce::Colour (0xffffd060));
    g.fillPath (tri);
}

void PianoRollComponent::paintVelocityStrip (juce::Graphics& g, juce::Rectangle<int> area)
{
    // Strip + handle footprint: the resize handle paints ABOVE area.getY()
    // by kStripResizeGrabPx, so the "considered" rect for the empty-dirty
    // short-circuit must include those handle pixels. Without the extra
    // Y span, partial repaints that hit only the handle's pixels would
    // be early-returned and the handle would not redraw.
    const auto considered = area.withTop (area.getY() - kStripResizeGrabPx);
    if (g.getClipBounds().getIntersection (considered).isEmpty()) return;

    g.setColour (juce::Colour (0xff121218));
    g.fillRect (area);

    // Resize-grab strip just above the lane's top edge. Painted as a
    // hairline + 4 px gradient so the user has a visual cue that the
    // border is draggable. Sits ABOVE area.getY() in screen space.
    {
        const auto handleR = juce::Rectangle<int> (
            area.getX(), area.getY() - kStripResizeGrabPx,
            area.getWidth(), kStripResizeGrabPx);
        g.setColour (juce::Colour (0xff2a2a36));
        g.fillRect (handleR);
        g.setColour (juce::Colour (0xff4a4a58));
        g.fillRect (handleR.getX() + handleR.getWidth() / 2 - 18,
                     handleR.getY() + 1, 36, 2);
    }

    g.setColour (kGridLine);
    g.drawHorizontalLine (area.getY(),
                            (float) area.getX(), (float) area.getRight());

    const auto* r = region();
    if (r == nullptr || r->lengthInTicks <= 0) return;

    // Cull against the actual incoming dirty rect — matches the
    // paintNotes fix; without this, every visible velocity lollipop
    // ran the stem + head + ellipse + selection paints on every
    // 30 Hz playhead repaint tick.
    const auto dirty = g.getClipBounds().getIntersection (area);
    juce::Graphics::ScopedSaveState saved (g);
    g.reduceClipRegion (area);

    // Reference rules at 0 / 64 / 127 - read as a soft "level-meter" backdrop
    // behind the lollipops. The 64-line is brightest; the 0/127 are subtle.
    const float ax = (float) area.getX();
    const float ay = (float) area.getY();
    const float aw = (float) area.getWidth();
    const float ah = (float) area.getHeight();
    g.setColour (kKeyOctaveLine.withAlpha (0.18f));
    g.drawHorizontalLine ((int) (ay + ah * 0.5f), ax, ax + aw);
    g.setColour (kKeyOctaveLine.withAlpha (0.08f));
    g.drawHorizontalLine ((int) (ay + ah * 0.25f), ax, ax + aw);
    g.drawHorizontalLine ((int) (ay + ah * 0.75f), ax, ax + aw);

    // Velocity legend on the left edge - "127" / "64" / "0". Subtle so
    // it doesn't compete with the bars. Drawn inside the keyboard's
    // margin so it doesn't overlap the first bar.
    g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::plain)));
    g.setColour (juce::Colour (0xff556070));
    g.drawText ("127", (int) ax + 3, (int) ay + 2, 30, 11,
                 juce::Justification::centredLeft, false);
    g.drawText ("64",  (int) ax + 3, (int) (ay + ah * 0.5f - 5), 30, 11,
                 juce::Justification::centredLeft, false);
    g.drawText ("0",   (int) ax + 3, (int) (ay + ah - 12), 30, 11,
                 juce::Justification::centredLeft, false);

    constexpr float kStemWidth   = 2.0f;
    constexpr float kCircleR     = 5.5f;   // outer radius of the lollipop head
    const float baseY = ay + ah - 3.0f;    // baseline (velocity 0)
    const float topY  = ay + kCircleR + 2.0f;
    const float span  = baseY - topY;

    for (int i = 0; i < (int) r->notes.size(); ++i)
    {
        const auto& n = r->notes[(size_t) i];
        const int nx = xForTick (n.startTick);
        // Cull against dirty rect with 8 px slack (lollipop head radius).
        if (nx < dirty.getX() - 8 || nx > dirty.getRight() + 8) continue;

        const float frac   = juce::jlimit (0.0f, 1.0f, (float) n.velocity / 127.0f);
        const float headCY = baseY - span * frac;

        const bool selected = isNoteSelected (i);
        const auto baseCol  = selected ? kNoteSelected
                                          : colourForNote (n).withMultipliedBrightness (1.05f);

        // Stem: thin vertical bar from baseline to the head's centre.
        const auto stem = juce::Rectangle<float> ((float) nx - kStemWidth * 0.5f,
                                                     headCY,
                                                     kStemWidth,
                                                     baseY - headCY);
        g.setColour (baseCol.withAlpha (0.85f));
        g.fillRect (stem);

        // Head: filled disc with a subtle outer ring. Reads as a knob
        // the user can grab - matches the user's "lollipop" mental model.
        const auto head = juce::Rectangle<float> ((float) nx - kCircleR,
                                                     headCY - kCircleR,
                                                     kCircleR * 2.0f,
                                                     kCircleR * 2.0f);
        g.setColour (baseCol);
        g.fillEllipse (head);
        g.setColour (juce::Colours::white.withAlpha (selected ? 0.9f : 0.35f));
        g.drawEllipse (head, selected ? 1.4f : 0.9f);

        // Velocity readout near the head when selected - lets the user
        // see exact values during fine tweaks. Skipped for unselected
        // notes so the lane stays readable in dense regions.
        if (selected)
        {
            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            g.setColour (juce::Colours::white.withAlpha (0.92f));
            const auto text = juce::String (n.velocity);
            const int tw = g.getCurrentFont().getStringWidth (text) + 6;
            g.drawText (text,
                         (int) ((float) nx + kCircleR + 3.0f), (int) headCY - 7,
                         tw, 14, juce::Justification::centredLeft, false);
        }
    }
}

bool PianoRollComponent::isNoteSelected (int idx) const noexcept
{
    return std::binary_search (selectedNotes.begin(), selectedNotes.end(), idx);
}

void PianoRollComponent::clearSelection()
{
    selectedNotes.clear();
    dragAnchor = -1;
}

void PianoRollComponent::selectOnly (int idx)
{
    selectedNotes.clear();
    if (idx >= 0) selectedNotes.push_back (idx);
}

void PianoRollComponent::addToSelection (int idx)
{
    if (idx < 0) return;
    auto it = std::lower_bound (selectedNotes.begin(), selectedNotes.end(), idx);
    if (it == selectedNotes.end() || *it != idx)
        selectedNotes.insert (it, idx);
}

void PianoRollComponent::toggleSelected (int idx)
{
    if (idx < 0) return;
    auto it = std::lower_bound (selectedNotes.begin(), selectedNotes.end(), idx);
    if (it != selectedNotes.end() && *it == idx)
        selectedNotes.erase (it);
    else
        selectedNotes.insert (it, idx);
}

void PianoRollComponent::beginGroupDrag (int anchorIdx)
{
    dragAnchor = anchorIdx;
    dragSnapshots.clear();
    const auto* r = region();
    if (r == nullptr) return;
    dragSnapshots.reserve (selectedNotes.size());
    for (int idx : selectedNotes)
    {
        if (idx < 0 || idx >= (int) r->notes.size()) continue;
        const auto& n = r->notes[(size_t) idx];
        dragSnapshots.push_back ({ n.startTick, n.noteNumber, n.lengthInTicks, n.velocity });
    }
}

void PianoRollComponent::applyGroupMove (juce::int64 deltaTicks, int deltaPitch)
{
    auto* r = region();
    if (r == nullptr) return;
    if (selectedNotes.size() != dragSnapshots.size()) return;

    // Clamp the group delta so no note ends up out of bounds. Without
    // this, individual per-note clamps would let some notes "stick" at
    // the boundary while others kept moving, breaking relative spacing.
    juce::int64 minStart   = std::numeric_limits<juce::int64>::max();
    juce::int64 maxEnd     = std::numeric_limits<juce::int64>::min();
    int         minPitch   = 127;
    int         maxPitch   = 0;
    for (const auto& s : dragSnapshots)
    {
        minStart  = juce::jmin (minStart, s.startTick);
        maxEnd    = juce::jmax (maxEnd,   s.startTick + s.lengthInTicks);
        minPitch  = juce::jmin (minPitch, s.noteNumber);
        maxPitch  = juce::jmax (maxPitch, s.noteNumber);
    }
    const juce::int64 maxDeltaTickRight = r->lengthInTicks - maxEnd;
    deltaTicks = juce::jlimit (-minStart, maxDeltaTickRight, deltaTicks);
    deltaPitch = juce::jlimit (-minPitch, 127 - maxPitch, deltaPitch);

    for (size_t i = 0; i < selectedNotes.size(); ++i)
    {
        const int idx = selectedNotes[i];
        if (idx < 0 || idx >= (int) r->notes.size()) continue;
        auto& n = r->notes[(size_t) idx];
        n.startTick  = dragSnapshots[i].startTick + deltaTicks;
        n.noteNumber = dragSnapshots[i].noteNumber + deltaPitch;
    }
}

void PianoRollComponent::transposeSelected (int semitones)
{
    auto* r = region();
    if (r == nullptr || selectedNotes.empty()) return;
    for (int idx : selectedNotes)
    {
        if (idx < 0 || idx >= (int) r->notes.size()) continue;
        auto& n = r->notes[(size_t) idx];
        n.noteNumber = juce::jlimit (0, kNumKeys - 1, n.noteNumber + semitones);
    }
}

void PianoRollComponent::quantizeSelected (juce::int64 gridTicks, float strength)
{
    auto* r = region();
    if (r == nullptr || gridTicks <= 0) return;
    strength = juce::jlimit (0.0f, 1.0f, strength);
    // No selection → quantize every note in the region. Matches Logic
    // and Reaper (Q on empty selection = quantize all).
    auto applyToOne = [gridTicks, strength] (MidiNote& n)
    {
        const auto orig    = n.startTick;
        const auto snapped = ((orig + gridTicks / 2) / gridTicks) * gridTicks;
        const auto blended = orig + (juce::int64) std::round (
            (double) (snapped - orig) * strength);
        n.startTick = juce::jmax ((juce::int64) 0, blended);
    };
    if (selectedNotes.empty())
    {
        for (auto& n : r->notes) applyToOne (n);
    }
    else
    {
        for (int idx : selectedNotes)
        {
            if (idx < 0 || idx >= (int) r->notes.size()) continue;
            applyToOne (r->notes[(size_t) idx]);
        }
    }
}

void PianoRollComponent::humanizeVelocity (int rangePercent)
{
    auto* r = region();
    if (r == nullptr) return;
    rangePercent = juce::jlimit (1, 100, rangePercent);
    const int range = (int) std::round (127.0 * (double) rangePercent / 100.0);
    juce::Random rng;
    auto applyToOne = [&] (MidiNote& n)
    {
        const int delta = rng.nextInt ({ -range, range + 1 });
        n.velocity = juce::jlimit (1, 127, n.velocity + delta);
    };
    if (selectedNotes.empty()) for (auto& n : r->notes) applyToOne (n);
    else
        for (int idx : selectedNotes)
            if (idx >= 0 && idx < (int) r->notes.size())
                applyToOne (r->notes[(size_t) idx]);
}

void PianoRollComponent::setVelocityFor (int value)
{
    auto* r = region();
    if (r == nullptr) return;
    const int v = juce::jlimit (1, 127, value);
    if (selectedNotes.empty()) for (auto& n : r->notes) n.velocity = v;
    else
        for (int idx : selectedNotes)
            if (idx >= 0 && idx < (int) r->notes.size())
                r->notes[(size_t) idx].velocity = v;
}

void PianoRollComponent::showVelocityPopup()
{
    juce::PopupMenu m;
    // Two sections: humanise (random ±N% of 127) at the top, set-to
    // exact values below. Mirrors the quantize popup's structure.
    m.addSectionHeader ("Humanise (random +/- of 127)");
    m.addItem (1, "+/- 5 %");
    m.addItem (2, "+/- 10 %");
    m.addItem (3, "+/- 20 %");
    m.addItem (4, "+/- 30 %");
    m.addSeparator();
    m.addSectionHeader ("Set velocity");
    m.addItem (101, "Set to 127 (max)");
    m.addItem (102, "Set to 100");
    m.addItem (103, "Set to 80");
    m.addItem (104, "Set to 64 (mid)");
    m.addItem (105, "Set to 32");
    juce::Component::SafePointer<PianoRollComponent> safe (this);
    showContextMenu (m, *this,
        [safe] (int chosen)
        {
            auto* self = safe.getComponent();
            if (self == nullptr || chosen <= 0) return;
            switch (chosen)
            {
                case 1:   self->humanizeVelocity (5);   break;
                case 2:   self->humanizeVelocity (10);  break;
                case 3:   self->humanizeVelocity (20);  break;
                case 4:   self->humanizeVelocity (30);  break;
                case 101: self->setVelocityFor (127);   break;
                case 102: self->setVelocityFor (100);   break;
                case 103: self->setVelocityFor (80);    break;
                case 104: self->setVelocityFor (64);    break;
                case 105: self->setVelocityFor (32);    break;
                default: return;
            }
            self->repaint();
        });
}

void PianoRollComponent::setChannelForSelected (int channel)
{
    auto* r = region();
    if (r == nullptr) return;
    const int ch = juce::jlimit (1, 16, channel);
    auto applyToOne = [ch] (MidiNote& n) { n.channel = ch; };
    if (selectedNotes.empty()) for (auto& n : r->notes) applyToOne (n);
    else
        for (int idx : selectedNotes)
            if (idx >= 0 && idx < (int) r->notes.size())
                applyToOne (r->notes[(size_t) idx]);
}

void PianoRollComponent::setLengthTicksForSelected (juce::int64 ticks)
{
    auto* r = region();
    if (r == nullptr) return;
    if (ticks <= 0) return;
    auto applyToOne = [r, ticks] (MidiNote& n)
    {
        // Clamp so the note can't extend past the region. Hard floor
        // is 1 tick - any positive length is valid.
        const auto maxLen = juce::jmax ((juce::int64) 1, r->lengthInTicks - n.startTick);
        n.lengthInTicks = juce::jlimit ((juce::int64) 1, maxLen, ticks);
    };
    if (selectedNotes.empty()) for (auto& n : r->notes) applyToOne (n);
    else
        for (int idx : selectedNotes)
            if (idx >= 0 && idx < (int) r->notes.size())
                applyToOne (r->notes[(size_t) idx]);
}

void PianoRollComponent::showNotePropertiesPopup (int hitNoteIdx, juce::Point<int> screenPos)
{
    juce::PopupMenu m;
    m.addSectionHeader (selectedNotes.size() > 1
                          ? juce::String::formatted ("Note Properties (%d notes)",
                                                       (int) selectedNotes.size())
                          : juce::String ("Note Properties"));

    // Channel submenu (1..16). Highlight the channel of the
    // right-clicked note for visual context.
    int hitChannel = -1;
    auto* r = region();
    if (r != nullptr && hitNoteIdx >= 0 && hitNoteIdx < (int) r->notes.size())
        hitChannel = r->notes[(size_t) hitNoteIdx].channel;
    juce::PopupMenu chSub;
    for (int ch = 1; ch <= 16; ++ch)
        chSub.addItem (1000 + ch, "Channel " + juce::String (ch),
                        true, ch == hitChannel);
    m.addSubMenu ("Channel", chSub);

    // Length submenu - common note values plus dotted variants.
    juce::PopupMenu lenSub;
    struct LenChoice { const char* label; juce::int64 ticks; };
    const LenChoice lengths[] = {
        { "Whole",        kMidiTicksPerQuarter * 4 },
        { "Half",         kMidiTicksPerQuarter * 2 },
        { "Quarter",      kMidiTicksPerQuarter     },
        { "Eighth",       kMidiTicksPerQuarter / 2 },
        { "Sixteenth",    kMidiTicksPerQuarter / 4 },
        { "Thirty-second",kMidiTicksPerQuarter / 8 },
    };
    for (int i = 0; i < (int) (sizeof (lengths) / sizeof (lengths[0])); ++i)
        lenSub.addItem (2000 + i, lengths[i].label);
    m.addSubMenu ("Length", lenSub);

    // Velocity submenu - same set the dedicated 'V' popup uses.
    juce::PopupMenu velSub;
    velSub.addItem (3001, "127 (max)");
    velSub.addItem (3002, "100");
    velSub.addItem (3003, "80");
    velSub.addItem (3004, "64 (mid)");
    velSub.addItem (3005, "32");
    m.addSubMenu ("Velocity", velSub);

    juce::Component::SafePointer<PianoRollComponent> safe (this);
    showContextMenu (m, *this, screenPos,
        [safe] (int chosen)
        {
            auto* self = safe.getComponent();
            if (self == nullptr || chosen <= 0) return;
            if (chosen >= 1001 && chosen <= 1016)
            {
                self->setChannelForSelected (chosen - 1000);
            }
            else if (chosen >= 2000 && chosen < 2006)
            {
                static const juce::int64 lens[] = {
                    kMidiTicksPerQuarter * 4, kMidiTicksPerQuarter * 2,
                    kMidiTicksPerQuarter,     kMidiTicksPerQuarter / 2,
                    kMidiTicksPerQuarter / 4, kMidiTicksPerQuarter / 8,
                };
                self->setLengthTicksForSelected (lens[chosen - 2000]);
            }
            else
            {
                switch (chosen)
                {
                    case 3001: self->setVelocityFor (127); break;
                    case 3002: self->setVelocityFor (100); break;
                    case 3003: self->setVelocityFor (80);  break;
                    case 3004: self->setVelocityFor (64);  break;
                    case 3005: self->setVelocityFor (32);  break;
                    default: return;
                }
            }
            self->repaint();
        });
}

void PianoRollComponent::glueSelectedNotes()
{
    auto* r = region();
    if (r == nullptr || selectedNotes.size() < 2) return;

    // Group selected notes by pitch. Within each group, sort by
    // startTick, then walk consecutive pairs - if the second's
    // startTick falls at or before the first's endTick, absorb it
    // into the first (extending the first's length to cover both).
    // Track absorbed indices so we can erase them at the end in
    // descending order without invalidating earlier indices.
    std::map<int, std::vector<int>> byPitch;
    for (int idx : selectedNotes)
    {
        if (idx < 0 || idx >= (int) r->notes.size()) continue;
        byPitch[r->notes[(size_t) idx].noteNumber].push_back (idx);
    }

    std::vector<int> toErase;
    for (auto& [pitch, indices] : byPitch)
    {
        std::sort (indices.begin(), indices.end(),
            [&] (int a, int b) {
                return r->notes[(size_t) a].startTick < r->notes[(size_t) b].startTick;
            });

        for (size_t i = 0; i + 1 < indices.size(); )
        {
            const int aIdx = indices[i];
            const int bIdx = indices[i + 1];
            auto& a = r->notes[(size_t) aIdx];
            auto& b = r->notes[(size_t) bIdx];
            const auto aEnd = a.startTick + a.lengthInTicks;
            const auto bEnd = b.startTick + b.lengthInTicks;

            if (b.startTick <= aEnd)
            {
                // Absorb b into a. New length = max-end - a.startTick.
                a.lengthInTicks = juce::jmax (aEnd, bEnd) - a.startTick;
                toErase.push_back (bIdx);
                indices.erase (indices.begin() + (long) (i + 1));
                // Don't advance i - the new merged a may now be
                // contiguous with the (former i+2, now i+1) note.
            }
            else
            {
                ++i;
            }
        }
    }

    if (toErase.empty()) return;
    std::sort (toErase.begin(), toErase.end(), std::greater<int>());
    for (int idx : toErase)
    {
        if (idx >= 0 && idx < (int) r->notes.size())
            r->notes.erase (r->notes.begin() + idx);
    }
    // Selection indices are now stale (erases shifted later notes
    // down). Clearing is the simplest correct response - the user
    // can re-select if they need to.
    clearSelection();
}

void PianoRollComponent::duplicateSelectedNotes()
{
    auto* r = region();
    if (r == nullptr || selectedNotes.empty()) return;

    // Compute the selection span so the clone block sits cleanly
    // after the original. minStart/maxEnd over all selected notes;
    // span = maxEnd - minStart. Empty span (a single zero-length
    // note) falls back to one quarter-note shift.
    juce::int64 minStart = std::numeric_limits<juce::int64>::max();
    juce::int64 maxEnd   = std::numeric_limits<juce::int64>::min();
    for (int idx : selectedNotes)
    {
        if (idx < 0 || idx >= (int) r->notes.size()) continue;
        const auto& n = r->notes[(size_t) idx];
        minStart = juce::jmin (minStart, n.startTick);
        maxEnd   = juce::jmax (maxEnd,   n.startTick + n.lengthInTicks);
    }
    juce::int64 shift = maxEnd - minStart;
    if (shift <= 0) shift = kMidiTicksPerQuarter;

    // Append clones, recording the indices of the new notes so we
    // can replace the selection with them. We don't clamp against
    // r->lengthInTicks here - the user can extend the region or
    // move the clones afterwards.
    std::vector<int> newSelection;
    newSelection.reserve (selectedNotes.size());
    for (int idx : selectedNotes)
    {
        if (idx < 0 || idx >= (int) r->notes.size()) continue;
        MidiNote clone = r->notes[(size_t) idx];
        clone.startTick += shift;
        r->notes.push_back (clone);
        newSelection.push_back ((int) r->notes.size() - 1);
    }
    selectedNotes = std::move (newSelection);
    std::sort (selectedNotes.begin(), selectedNotes.end());
}

void PianoRollComponent::stepRecordNoteOn (int noteNumber, int velocity)
{
    auto* r = region();
    if (r == nullptr) return;
    const double sr  = engine.getCurrentSampleRate();
    const float  bpm = session.tempoBpm.load (std::memory_order_relaxed);
    if (sr <= 0.0 || bpm <= 0.0f) return;

    // Step length: piano-roll snap when set, otherwise default to a
    // 1/16 note. snapTicks=0 means "no snap" for note-edit drags;
    // step record still needs SOME advance so we fall back to 1/16.
    const juce::int64 stepTicks = snapTicks > 0
        ? snapTicks
        : (kMidiTicksPerQuarter / 4);

    // First note of a NEW chord (held drops to 0 between chords)
    // advances the playhead by step before placing. Subsequent notes
    // within the same chord share the start position.
    if (stepRecordHeld == 0 && stepRecordChordHad)
    {
        const juce::int64 advanceSamples = ticksToSamples (stepTicks, sr, bpm);
        engine.getTransport().setPlayhead (
            engine.getTransport().getPlayhead() + advanceSamples);
        stepRecordChordHad = false;
    }

    const auto playhead = engine.getTransport().getPlayhead();
    const juce::int64 sampleOffset = playhead - r->timelineStart;
    if (sampleOffset < 0) return;   // playhead is before the region; ignore
    const juce::int64 startTick = samplesToTicks (sampleOffset, sr, bpm);

    // Extend the region if the new note would go past its current
    // end. Without this the painter clips the note and the engine
    // wouldn't schedule it during playback.
    if (startTick + stepTicks > r->lengthInTicks)
    {
        r->lengthInTicks = startTick + stepTicks;
        r->lengthInSamples = ticksToSamples (r->lengthInTicks, sr, bpm);
    }

    MidiNote n;
    n.channel       = 1;
    n.noteNumber    = juce::jlimit (0, kNumKeys - 1, noteNumber);
    n.velocity      = juce::jlimit (1, 127, velocity);
    n.startTick     = startTick;
    n.lengthInTicks = stepTicks;
    r->notes.push_back (n);

    ++stepRecordHeld;
    stepRecordChordHad = true;
    repaint();
}

void PianoRollComponent::stepRecordNoteOff (int /*noteNumber*/)
{
    if (stepRecordHeld > 0)
        --stepRecordHeld;
    // Don't advance playhead here - we wait for the NEXT Note On
    // so chord input stays clean (place all notes, release all,
    // place next chord, ...).
}

void PianoRollComponent::resetStepRecordState() noexcept
{
    stepRecordHeld     = 0;
    stepRecordChordHad = false;
}

void PianoRollComponent::nudgeSelectedTicks (juce::int64 deltaTicks)
{
    auto* r = region();
    if (r == nullptr || selectedNotes.empty()) return;

    // Group-clamp so relative spacing is preserved and no note ends
    // up out of bounds. Same pattern as applyGroupMove but without a
    // drag snapshot - we read live state and clamp once.
    juce::int64 minStart = std::numeric_limits<juce::int64>::max();
    juce::int64 maxEnd   = std::numeric_limits<juce::int64>::min();
    for (int idx : selectedNotes)
    {
        if (idx < 0 || idx >= (int) r->notes.size()) continue;
        const auto& n = r->notes[(size_t) idx];
        minStart = juce::jmin (minStart, n.startTick);
        maxEnd   = juce::jmax (maxEnd,   n.startTick + n.lengthInTicks);
    }
    deltaTicks = juce::jlimit (-minStart,
                                  r->lengthInTicks - maxEnd,
                                  deltaTicks);
    if (deltaTicks == 0) return;
    for (int idx : selectedNotes)
    {
        if (idx < 0 || idx >= (int) r->notes.size()) continue;
        r->notes[(size_t) idx].startTick += deltaTicks;
    }
}

bool PianoRollComponent::isInScale (int noteNumber) const noexcept
{
    if (scale == Scale::Off) return true;
    static constexpr int kMajorIntervals[]      = { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr int kMinorIntervals[]      = { 0, 2, 3, 5, 7, 8, 10 };
    static constexpr int kDorianIntervals[]     = { 0, 2, 3, 5, 7, 9, 10 };
    static constexpr int kPhrygianIntervals[]   = { 0, 1, 3, 5, 7, 8, 10 };
    static constexpr int kLydianIntervals[]     = { 0, 2, 4, 6, 7, 9, 11 };
    static constexpr int kMixolydianIntervals[] = { 0, 2, 4, 5, 7, 9, 10 };
    static constexpr int kLocrianIntervals[]    = { 0, 1, 3, 5, 6, 8, 10 };
    const int* table = nullptr;
    int len = 7;
    switch (scale)
    {
        case Scale::Major:      table = kMajorIntervals;      break;
        case Scale::Minor:      table = kMinorIntervals;      break;
        case Scale::Dorian:     table = kDorianIntervals;     break;
        case Scale::Phrygian:   table = kPhrygianIntervals;   break;
        case Scale::Lydian:     table = kLydianIntervals;     break;
        case Scale::Mixolydian: table = kMixolydianIntervals; break;
        case Scale::Locrian:    table = kLocrianIntervals;    break;
        case Scale::Off: default: return true;
    }
    const int rel = ((noteNumber - scaleRoot) % 12 + 12) % 12;
    for (int i = 0; i < len; ++i) if (table[i] == rel) return true;
    return false;
}

void PianoRollComponent::showQuantizePopup()
{
    juce::PopupMenu m;
    // Each item is "<grid> @ <strength>". 100% items first (full snap),
    // then humanise variants. Selected/all-notes is implicit in
    // quantizeSelected (empty selection = whole region).
    struct Choice { const char* label; juce::int64 grid; float strength; };
    const Choice choices[] = {
        { "1/4 @ 100%",   kMidiTicksPerQuarter,        1.00f },
        { "1/8 @ 100%",   kMidiTicksPerQuarter / 2,    1.00f },
        { "1/16 @ 100%",  kMidiTicksPerQuarter / 4,    1.00f },
        { "1/32 @ 100%",  kMidiTicksPerQuarter / 8,    1.00f },
        { nullptr, 0, 0.0f },                          // separator
        // Triplet variants per spec §5e. A whole-quarter contains 3
        // triplet-eighth notes, so each = 480/3 = 160 ticks; 1/16T =
        // 480/6 = 80 ticks; 1/32T = 480/12 = 40 ticks.
        { "1/8T @ 100%",  kMidiTicksPerQuarter / 3,    1.00f },
        { "1/16T @ 100%", kMidiTicksPerQuarter / 6,    1.00f },
        { "1/32T @ 100%", kMidiTicksPerQuarter / 12,   1.00f },
        { nullptr, 0, 0.0f },                          // separator
        { "1/16 @ 75%",   kMidiTicksPerQuarter / 4,    0.75f },
        { "1/16 @ 50%",   kMidiTicksPerQuarter / 4,    0.50f },
        { "1/8 @ 50%",    kMidiTicksPerQuarter / 2,    0.50f },
    };
    for (int i = 0; i < (int) (sizeof (choices) / sizeof (choices[0])); ++i)
    {
        if (choices[i].label == nullptr) { m.addSeparator(); continue; }
        m.addItem (i + 1, choices[i].label);
    }
    juce::Component::SafePointer<PianoRollComponent> safe (this);
    showContextMenu (m, *this,
        [safe, choices] (int chosen)
        {
            if (chosen <= 0) return;
            auto* self = safe.getComponent();
            if (self == nullptr) return;
            const auto& c = choices[chosen - 1];
            self->quantizeSelected (c.grid, c.strength);
            self->repaint();
        });
}

void PianoRollComponent::showScalePopup()
{
    juce::PopupMenu m;
    static const char* roots[] = { "C", "C#", "D", "D#", "E", "F", "F#",
                                     "G", "G#", "A", "A#", "B" };
    static const char* scaleNames[] = { "Off", "Major", "Minor", "Dorian",
                                          "Phrygian", "Lydian", "Mixolydian", "Locrian" };
    // Item ID encoding: root * 100 + scaleType + 1. ScaleType 0 = Off
    // ignores root. Add an "Off" entry at the top for quick clear.
    m.addItem (1, "Off", true, scale == Scale::Off);
    m.addSeparator();
    for (int s = 1; s < 8; ++s)
    {
        juce::PopupMenu sub;
        for (int r = 0; r < 12; ++r)
        {
            const int id = r * 100 + s + 1;
            const bool ticked = (scale != Scale::Off
                                  && (int) scale == s
                                  && scaleRoot == r);
            sub.addItem (id, juce::String (roots[r]), true, ticked);
        }
        m.addSubMenu (juce::String (scaleNames[s]), sub);
    }
    juce::Component::SafePointer<PianoRollComponent> safe (this);
    showContextMenu (m, *this,
        [safe] (int chosen)
        {
            if (chosen <= 0) return;
            auto* self = safe.getComponent();
            if (self == nullptr) return;
            if (chosen == 1)
            {
                self->scale = Scale::Off;
            }
            else
            {
                const int s = (chosen - 1) % 100;        // 1..7
                const int r = (chosen - 1) / 100;        // 0..11
                self->scale     = (Scale) s;
                self->scaleRoot = r;
            }
            self->repaint();
        });
}

void PianoRollComponent::showColorModePopup()
{
    juce::PopupMenu m;
    m.addItem (1, "Pitch",    true, colorMode == ColorMode::Pitch);
    m.addItem (2, "Velocity", true, colorMode == ColorMode::Velocity);
    m.addItem (3, "Channel",  true, colorMode == ColorMode::Channel);
    juce::Component::SafePointer<PianoRollComponent> safe (this);
    showContextMenu (m, *this,
        [safe] (int chosen)
        {
            auto* self = safe.getComponent();
            if (self == nullptr || chosen <= 0) return;
            self->colorMode = chosen == 1 ? ColorMode::Pitch
                              : chosen == 2 ? ColorMode::Velocity
                                             : ColorMode::Channel;
            self->repaint();
        });
}

void PianoRollComponent::showCcControllerPopup()
{
    juce::PopupMenu m;
    // Common CCs surfaced as named entries; the rest behind a numeric
    // submenu so the chip doubles as a full controller picker.
    struct Named { int cc; const char* name; };
    static const Named common[] = {
        { 1,  "1 - Mod Wheel"   }, { 7,  "7 - Volume"     },
        { 11, "11 - Expression" }, { 64, "64 - Sustain"   },
        { 71, "71 - Resonance"  }, { 74, "74 - Filter"    },
        { 91, "91 - Reverb"     }, { 93, "93 - Chorus"    },
    };
    for (const auto& c : common)
        m.addItem (c.cc + 1, c.name, true, activeCcController == c.cc);

    juce::PopupMenu numeric;
    for (int cc = 0; cc < 128; ++cc)
        numeric.addItem (cc + 1, juce::String (cc), true, activeCcController == cc);
    m.addSeparator();
    m.addSubMenu ("All controllers (0-127)", numeric);

    juce::Component::SafePointer<PianoRollComponent> safe (this);
    showContextMenu (m, *this,
        [safe] (int chosen)
        {
            auto* self = safe.getComponent();
            if (self == nullptr || chosen <= 0) return;
            self->activeCcController = juce::jlimit (0, 127, chosen - 1);
            self->repaint();
        });
}

juce::Colour PianoRollComponent::colourForNote (const MidiNote& n) const noexcept
{
    const float velFactor = juce::jlimit (0.0f, 1.0f, (float) n.velocity / 127.0f);
    const float bri = 0.55f + 0.45f * velFactor;   // 0.55..1.00 brightness from velocity

    switch (colorMode)
    {
        case ColorMode::Pitch:
        {
            const int pc = ((n.noteNumber % 12) + 12) % 12;
            return kPitchClassPalette[pc].withMultipliedBrightness (bri);
        }
        case ColorMode::Channel:
        {
            // 16-way hue cycle. Channels 1..16 -> hue 0..15/16. Modulo 16
            // so channel 0 (running-status / unset) doesn't divide-by-zero.
            const int idx = juce::jlimit (1, 16, n.channel) - 1;
            const float hue = (float) idx / 16.0f;
            return juce::Colour::fromHSV (hue, 0.65f, bri, 1.0f);
        }
        case ColorMode::Velocity:
        default:
            return kNoteFill.withMultipliedBrightness (bri);
    }
}

void PianoRollComponent::paintCcStrip (juce::Graphics& g, juce::Rectangle<int> area)
{
    // Strip + handle footprint. Same shape as paintVelocityStrip: the
    // resize handle paints above area.getY(), so the empty-dirty
    // short-circuit must include those handle pixels.
    const auto considered = area.withTop (area.getY() - kStripResizeGrabPx);
    if (g.getClipBounds().getIntersection (considered).isEmpty()) return;

    g.setColour (juce::Colour (0xff0e0e14));
    g.fillRect (area);

    // Resize handle just above the CC lane's top edge - sits at the
    // boundary between velocity strip and cc strip. Drag to adjust
    // the cc lane's height.
    {
        const auto handleR = juce::Rectangle<int> (
            area.getX(), area.getY() - kStripResizeGrabPx,
            area.getWidth(), kStripResizeGrabPx);
        g.setColour (juce::Colour (0xff242430));
        g.fillRect (handleR);
        g.setColour (juce::Colour (0xff424252));
        g.fillRect (handleR.getX() + handleR.getWidth() / 2 - 18,
                     handleR.getY() + 1, 36, 2);
    }

    g.setColour (kGridLine);
    g.drawHorizontalLine (area.getY(), (float) area.getX(), (float) area.getRight());

    const auto* r = region();
    if (r == nullptr || r->lengthInTicks <= 0) return;

    // Cull against the actual dirty rect — same fix as paintNotes /
    // paintVelocityStrip. The 30 Hz partial playhead repaint should
    // skip every CC bar that doesn't intersect the playhead's strip.
    const auto dirty = g.getClipBounds().getIntersection (area);
    juce::Graphics::ScopedSaveState saved (g);
    g.reduceClipRegion (area);

    const float ax = (float) area.getX();
    const float ay = (float) area.getY();
    const float aw = (float) area.getWidth();
    const float ah = (float) area.getHeight();
    // 0 / 64 / 127 reference rules so the user can read approximate
    // values without dragging.
    g.setColour (kKeyOctaveLine.withAlpha (0.10f));
    g.drawHorizontalLine ((int) (ay + ah * 0.5f), ax, ax + aw);

    const float baseY = ay + ah - 2.0f;
    const float topY  = ay + 4.0f;
    const float span  = baseY - topY;
    constexpr float kBarWidth = 3.0f;

    // Use a fixed accent for CC bars - visual contrast against the
    // pitch-coloured note grid above. (Coloring CCs by velocity-of-
    // associated-note isn't meaningful; CCs are independent events.)
    const auto barFill = juce::Colour (0xff60c0a8);

    for (int i = 0; i < (int) r->ccs.size(); ++i)
    {
        const auto& c = r->ccs[(size_t) i];
        if (c.controller != activeCcController) continue;
        const int nx = xForTick (c.atTick);
        // Cull against dirty rect with 4 px slack (CC bar half-width).
        if (nx < dirty.getX() - 4 || nx > dirty.getRight() + 4) continue;

        const float frac = juce::jlimit (0.0f, 1.0f, (float) c.value / 127.0f);
        const float top  = baseY - span * frac;
        const auto bar = juce::Rectangle<float> ((float) nx - kBarWidth * 0.5f,
                                                    top, kBarWidth, baseY - top);
        g.setColour (barFill);
        g.fillRect (bar);
    }

    // Header label - "CC: <num> (<name>)" pinned to the upper-left of
    // the strip. Names cover the most-used controllers; unknown CCs
    // fall back to "CC <num>".
    const char* name =
        activeCcController == 1   ? "Mod"     :
        activeCcController == 7   ? "Volume"  :
        activeCcController == 11  ? "Express" :
        activeCcController == 64  ? "Sustain" :
        activeCcController == 74  ? "Filter"  :
                                     nullptr;
    juce::String label;
    if (name != nullptr) label = "CC " + juce::String (activeCcController) + " (" + name + ")";
    else                  label = "CC " + juce::String (activeCcController);
    g.setColour (kHeaderText.withAlpha (0.85f));
    g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    g.drawText (label, area.getX() + 8, area.getY() + 3,
                 area.getWidth() - 16, 18,
                 juce::Justification::centredLeft, false);
}

int PianoRollComponent::hitTestCcBar (int x, juce::Rectangle<int> stripArea) const
{
    const auto* r = region();
    if (r == nullptr) return -1;
    constexpr int kHitSlopPx = 5;
    // Walk newest-first so an overlapping bar prefers the most-recent
    // event - matches the painter's z-order.
    for (int i = (int) r->ccs.size() - 1; i >= 0; --i)
    {
        const auto& c = r->ccs[(size_t) i];
        if (c.controller != activeCcController) continue;
        const int nx = xForTick (c.atTick);
        if (std::abs (x - nx) <= kHitSlopPx
            && x >= stripArea.getX() && x <= stripArea.getRight()) return i;
    }
    return -1;
}

int PianoRollComponent::hitTestVelocityBar (int x, juce::Rectangle<int> stripArea) const
{
    const auto* r = region();
    if (r == nullptr) return -1;
    if (x < stripArea.getX() || x > stripArea.getRight()) return -1;

    // Column-based hit test - the user clicks anywhere in a note's
    // horizontal span and the lollipop for that note responds. Walk
    // newest-first so an overlap prefers the most-recently-added note,
    // matching the paint order.
    for (int i = (int) r->notes.size() - 1; i >= 0; --i)
    {
        const auto& n = r->notes[(size_t) i];
        const int x0 = xForTick (n.startTick);
        const int x1 = xForTick (n.startTick + n.lengthInTicks);
        if (x >= x0 && x <= x1) return i;
    }

    // Fallback: nearest lollipop head within a generous window. Lets
    // the user click slightly past a short note's tail and still hit
    // its head.
    constexpr int kFallbackPx = 18;
    int bestIdx = -1;
    int bestDist = kFallbackPx + 1;
    for (int i = (int) r->notes.size() - 1; i >= 0; --i)
    {
        const auto& n = r->notes[(size_t) i];
        const int nx = xForTick (n.startTick);
        const int d = std::abs (x - nx);
        if (d < bestDist) { bestDist = d; bestIdx = i; }
    }
    return bestIdx;
}

// Snap a tick to the nearest grid step. Pure helper so create / move /
// resize all use the same rounding rule. Negative ticks clamp to zero
// (the region origin); 0 snapTicks is the "no snap" sentinel.
static juce::int64 snapTick (juce::int64 t, juce::int64 step)
{
    if (step <= 0) return juce::jmax ((juce::int64) 0, t);
    if (t < 0) return 0;
    const auto half = step / 2;
    return ((t + half) / step) * step;
}

int PianoRollComponent::hitTestNote (int x, int y, bool& onRightEdge) const
{
    onRightEdge = false;
    const auto* r = region();
    if (r == nullptr) return -1;
    constexpr int kEdgeGrabPx = 5;
    // Walk newest-first so overlaid notes prefer the most-recently-added
    // (matches the painter's draw order which currently is in-order; for
    // overlaps in 4c, we'll switch to a z-order list).
    for (int i = (int) r->notes.size() - 1; i >= 0; --i)
    {
        const auto& n = r->notes[(size_t) i];
        const int x0 = xForTick (n.startTick);
        const int x1 = xForTick (n.startTick + n.lengthInTicks);
        const int yy = yForNoteNumber (n.noteNumber);
        if (x < x0 || x > x1) continue;
        if (y < yy || y > yy + kNoteHeight) continue;
        onRightEdge = (x1 - x) <= kEdgeGrabPx;
        return i;
    }
    return -1;
}

void PianoRollComponent::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    auto* r = region();
    if (r == nullptr) return;

    // Middle-mouse-drag in the grid pans the timeline + keyboard range
    // together. Captured first so it never gets shadowed by note hits /
    // range select / context menus. Constrained to the grid area
    // (right of the keyboard, above the velocity / cc strips) so middle-
    // clicking the toolbar / keyboard / strips still does nothing.
    const int gridTop    = kToolbarHeight + kHeaderHeight;
    const int ccTop      = getHeight() - kStatusBarH - ccStripH;
    const int velTopY    = ccTop - velocityStripH;
    if (e.mods.isMiddleButtonDown()
        && e.x >= kKeyboardWidth
        && e.y >= gridTop && e.y < velTopY)
    {
        dragMode        = DragMode::Pan;
        panStartMouseX  = e.x;
        panStartMouseY  = e.y;
        panStartScrollX = scrollX;
        panStartScrollY = scrollY;
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        return;
    }

    // Right-click on a note opens the per-note properties popup.
    // If the clicked note isn't already selected, promote it to a
    // single-note selection first so the menu's actions have a
    // sensible target. Acts on the entire selection when the
    // clicked note IS already part of one. Right-click on empty
    // grid space falls through to the existing logic (no menu;
    // nothing useful to set).
    if (e.mods.isPopupMenu())
    {
        bool onEdgeIgnored = false;
        const int rightClickHit = hitTestNote (e.x, e.y, onEdgeIgnored);
        if (rightClickHit >= 0)
        {
            if (! isNoteSelected (rightClickHit))
                selectOnly (rightClickHit);
            showNotePropertiesPopup (rightClickHit, e.getScreenPosition());
            repaint();
            return;
        }
    }

    // Click in the bar/beat ruler band (between toolbar and grid)
    // begins a time-range selection. Auto-selects every note whose
    // start falls in [start, end) on each drag tick so the user
    // gets live feedback. Plain click in the ruler (no drag) just
    // resets editCursorTick to that tick AND clears any active range.
    const auto rulerBand = juce::Rectangle<int> (kKeyboardWidth, kToolbarHeight,
                                                    getWidth() - kKeyboardWidth, kHeaderHeight);

    // Grab a loop / punch bracket edge in the ruler band before the
    // range-select gesture claims the click. 6px tolerance per edge,
    // loop edges take priority over punch when both land under the cursor.
    if (rulerBand.contains (e.x, e.y) && ! e.mods.isPopupMenu())
    {
        const double sr  = juce::jmax (1.0, engine.getCurrentSampleRate());
        const float  bpm = session.tempoBpm.load (std::memory_order_relaxed);
        auto& transport  = engine.getTransport();
        auto xForSample  = [&] (juce::int64 s)
            { return xForTick (samplesToTicks (s - r->timelineStart, sr, bpm)); };
        const int tol = 6;
        struct Edge { juce::int64 sample; DragMode mode; bool valid; };
        const Edge edges[] = {
            { transport.getLoopStart(),  DragMode::LoopIn,   transport.getLoopEnd()  > transport.getLoopStart() },
            { transport.getLoopEnd(),    DragMode::LoopOut,  transport.getLoopEnd()  > transport.getLoopStart() },
            { transport.getPunchIn(),    DragMode::PunchIn,  transport.getPunchOut() > transport.getPunchIn() },
            { transport.getPunchOut(),   DragMode::PunchOut, transport.getPunchOut() > transport.getPunchIn() },
        };
        for (const auto& ed : edges)
        {
            if (! ed.valid) continue;
            if (std::abs (xForSample (ed.sample) - e.x) <= tol)
            {
                dragMode = ed.mode;
                setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
                return;
            }
        }
    }
    if (rulerBand.contains (e.x, e.y) && ! e.mods.isPopupMenu())
    {
        // Plain ruler click seeks the transport playhead (Reaper / Ardour
        // muscle memory), parity with AudioRegionEditor.
        const auto tickHere = juce::jmax<juce::int64> (0, tickForX (e.x));
        const double sr  = engine.getCurrentSampleRate();
        const double bpm = session.tempoBpm.load (std::memory_order_relaxed);
        const auto sampleOffset = ticksToSamples (tickHere, sr, bpm);
        engine.getTransport().setPlayhead (r->timelineStart + sampleOffset);

        rangeStartTick = tickHere;
        rangeEndTick   = rangeStartTick;
        rangeActive    = false;
        dragMode       = DragMode::RangeSelect;
        clearSelection();
        repaint();
        return;
    }

    const bool extendSelection = e.mods.isShiftDown() || e.mods.isCommandDown();

    // Bottom-strip layout: status bar at the very bottom, CC lane
    // above it, velocity lane above the CC lane. Compute the rects
    // inline so they match paint().
    const int  ccBottom    = getHeight() - kStatusBarH;
    const auto ccArea = juce::Rectangle<int> (
        kKeyboardWidth, ccBottom - ccStripH,
        getWidth() - kKeyboardWidth, ccStripH);
    const auto velocityArea = juce::Rectangle<int> (
        kKeyboardWidth, ccArea.getY() - velocityStripH,
        getWidth() - kKeyboardWidth, velocityStripH);

    // Resize-handle gestures take priority over the lane interiors -
    // the handle is the kStripResizeGrabPx-tall strip immediately
    // above each lane's top edge. Drag-up grows the lane; drag-down
    // shrinks it. Clamped in mouseDrag so the grid always keeps a
    // sensible minimum height.
    if (e.x >= velocityArea.getX() && e.x <= velocityArea.getRight()
        && e.y >= velocityArea.getY() - kStripResizeGrabPx
        && e.y <  velocityArea.getY())
    {
        dragMode          = DragMode::ResizeVelocityStrip;
        resizeStartStripH = velocityStripH;
        resizeStartMouseY = e.y;
        return;
    }
    if (e.x >= ccArea.getX() && e.x <= ccArea.getRight()
        && e.y >= ccArea.getY() - kStripResizeGrabPx
        && e.y <  ccArea.getY())
    {
        dragMode          = DragMode::ResizeCcStrip;
        resizeStartStripH = ccStripH;
        resizeStartMouseY = e.y;
        return;
    }

    // CC lane click: edit existing bar, or create a new CC event at
    // the clicked tick + value. Drag-y updates the value continuously.
    if (ccArea.contains (e.x, e.y))
    {
        const float frac = juce::jlimit (0.0f, 1.0f,
            1.0f - ((float) (e.y - ccArea.getY())
                      / (float) juce::jmax (1, ccArea.getHeight())));
        const int value = juce::jlimit (0, 127, (int) std::round (frac * 127.0f));

        int hit = hitTestCcBar (e.x, ccArea);
        if (hit < 0)
        {
            // Empty CC space: create a new event at the snapped tick.
            MidiCc c;
            c.channel    = 1;
            c.controller = activeCcController;
            c.value      = value;
            const auto rawTick = tickForX (e.x);
            c.atTick = juce::jlimit<juce::int64> (0,
                juce::jmax ((juce::int64) 0, r->lengthInTicks - 1),
                snapTick (rawTick, snapTicks));
            r->ccs.push_back (c);
            hit = (int) r->ccs.size() - 1;
        }
        else
        {
            r->ccs[(size_t) hit].value = value;
        }
        draggedCcIdx = hit;
        dragMode = DragMode::EditCcValue;
        repaint();
        return;
    }
    if (velocityArea.contains (e.x, e.y))
    {
        const int barIdx = hitTestVelocityBar (e.x, velocityArea);
        if (barIdx >= 0)
        {
            selectOnly (barIdx);
            dragAnchor = barIdx;
            dragMode = DragMode::EditVelocity;
            const float frac = juce::jlimit (0.0f, 1.0f,
                1.0f - ((float) (e.y - velocityArea.getY())
                          / (float) juce::jmax (1, velocityArea.getHeight())));
            r->notes[(size_t) barIdx].velocity =
                juce::jlimit (1, 127, (int) std::round (frac * 127.0f));
            repaint();
        }
        return;
    }

    bool onEdge = false;
    const int hit = hitTestNote (e.x, e.y, onEdge);

    if (hit >= 0)
    {
        // Shift / Cmd-click on a note: toggle in/out of selection
        // without entering drag mode. Matches Logic / Reaper.
        if (extendSelection)
        {
            toggleSelected (hit);
            dragMode = DragMode::None;
            repaint();
            return;
        }

        // Plain click on a note: if it was already selected, keep the
        // existing multi-selection so a group drag works. Otherwise
        // collapse to single-note selection. Either way, snapshot the
        // selection's drag origins so MoveNote can apply a group delta.
        if (! isNoteSelected (hit))
            selectOnly (hit);

        const auto& n = r->notes[(size_t) hit];
        dragOriginTick    = tickForX (e.x) - n.startTick;
        dragOriginNoteNum = n.noteNumber;
        dragNoteStartTick = n.startTick;
        dragNoteLenTicks  = n.lengthInTicks;
        editCursorTick    = n.startTick;   // anchor the cursor at the clicked note
        // Alt-modified click on a note body promotes Move ->
        // EditNoteVelocity. Drag-vertical adjusts velocity instead
        // of moving the note. Mirrors the tape-strip Alt-on-region
        // gain-drag for muscle-memory consistency. Resize (right-
        // edge hit) ignores the modifier - we still want trim there.
        if (e.mods.isAltDown() && ! onEdge)
            dragMode = DragMode::EditNoteVelocity;
        else
            dragMode = onEdge ? DragMode::ResizeNote : DragMode::MoveNote;
        beginGroupDrag (hit);
        repaint();
        return;
    }

    // Empty-grid hit. Paths depend on edit mode:
    //   - Shift / Cmd held: box-select drag (regardless of mode).
    //   - Draw mode: create a new note at the click (legacy pencil).
    //   - Grab mode: clear selection + drop cursor, no creation.
    //     Click-and-drag on empty space promotes to a box-select.
    // Positive grid-containment guard: clicks in the status / scrollbar
    // gutter below the grid (not caught by the lane returns above) must not
    // start a range-select or get eaten by the Cut/Grid early-return.
    if (! noteGridArea().contains (e.x, e.y)) return;

    // Drop the edit cursor at the snapped click tick, regardless of
    // which sub-path runs next (box-select, note-create, or just an
    // anchor click). Step-record + future click-to-place actions key
    // off this position.
    editCursorTick = juce::jlimit<juce::int64> (0,
        juce::jmax ((juce::int64) 0, r->lengthInTicks),
        snapTick (tickForX (e.x), snapTicks));

    if (extendSelection)
    {
        rubberBand = juce::Rectangle<int> (e.x, e.y, 0, 0);
        dragMode = DragMode::BoxSelect;
        // Don't clear existing selection on Shift-drag; the new box
        // ADDS to the current selection on mouseUp. Plain Cmd/Shift-
        // empty-click can't reach here without a drag, so this is the
        // right behaviour for the only path that fires it.
        repaint();
        return;
    }

    // Grab mode: don't create a note on empty-grid clicks. Clear the
    // current selection (matches Reaper/Ardour Grab semantics) and
    // start a box-select drag if the user drags away from the click.
    if (session.editMode == EditMode::Grab)
    {
        clearSelection();
        rubberBand = juce::Rectangle<int> (e.x, e.y, 0, 0);
        dragMode   = DragMode::BoxSelect;
        repaint();
        return;
    }

    // Range mode: empty-grid click starts a time-range selection,
    // reusing the same range bookkeeping the ruler-click path at the
    // top of this function already drives. Without this branch a
    // Range-mode click on the note grid was falling through to the
    // note-creation block below — drawing a note on every click.
    if (session.editMode == EditMode::Range)
    {
        clearSelection();   // start a fresh time-range with no stale note selection (matches the ruler-start path)
        rangeStartTick = juce::jmax<juce::int64> (0, tickForX (e.x));
        rangeEndTick   = rangeStartTick;
        rangeActive    = false;
        dragMode       = DragMode::RangeSelect;
        repaint();
        return;
    }

    // Cut / Grid have no piano-roll click handler (the class-level
    // comment at lines 398-401 documents this). Eat the click so the
    // note-creation fall-through below doesn't accidentally drop a
    // note when the user just wanted to pick a mode and the mouse
    // happens to be over the grid.
    if (session.editMode == EditMode::Cut
        || session.editMode == EditMode::Grid)
    {
        return;
    }

    MidiNote n;
    n.channel = 1;
    n.noteNumber = noteNumberForY (e.y);
    n.velocity = 100;
    const auto rawStart = juce::jlimit<juce::int64> (0,
        juce::jmax ((juce::int64) 0, r->lengthInTicks - 1), tickForX (e.x));
    // noteEntryMode adjusts the effective snap step for note-creation
    // only - move / resize keep using `snapTicks` directly. Free skips
    // snap entirely; Triplet / Dotted reshape the step.
    juce::int64 createSnap = snapTicks;
    switch (noteEntryMode)
    {
        case NoteEntryMode::Free:    createSnap = 0; break;
        case NoteEntryMode::Triplet: createSnap = (snapTicks * 2) / 3; break;
        case NoteEntryMode::Dotted:  createSnap = (snapTicks * 3) / 2; break;
        case NoteEntryMode::Grid:    break;
    }
    n.startTick = juce::jlimit<juce::int64> (0,
        juce::jmax ((juce::int64) 0, r->lengthInTicks - 1),
        snapTick (rawStart, createSnap));
    n.lengthInTicks = juce::jmin ((juce::int64) kMidiTicksPerQuarter,
                                                  r->lengthInTicks - n.startTick);
    if (n.lengthInTicks <= 0) return;
    r->notes.push_back (n);
    const int newIdx = (int) r->notes.size() - 1;
    selectOnly (newIdx);
    dragAnchor        = newIdx;
    dragNoteStartTick = n.startTick;
    dragNoteLenTicks  = n.lengthInTicks;
    dragOriginTick    = 0;
    dragOriginNoteNum = n.noteNumber;
    dragMode = DragMode::ResizeNote;
    repaint();
}

void PianoRollComponent::mouseDrag (const juce::MouseEvent& e)
{
    pushCursorPosition (e.x, e.y);

    auto* r = region();
    if (r == nullptr) return;

    // Pan drag - translate scrollX + scrollY by the absolute mouse
    // delta from the drag origin. Inverted so dragging right moves the
    // timeline left (revealing content to the right). Y clamped to the
    // valid scroll range; X is open on the right (the user can pan past
    // the region's end, matching the editor's "open-ended grid" model).
    if (dragMode == DragMode::Pan)
    {
        const int dx = e.x - panStartMouseX;
        const int dy = e.y - panStartMouseY;
        scrollX = juce::jmax (0, panStartScrollX - dx);
        const int maxScrollY = juce::jmax (0,
            kFullGridHeight - (getHeight() - kToolbarHeight - kHeaderHeight
                                  - velocityStripH - ccStripH - kStatusBarH));
        scrollY = juce::jlimit (0, maxScrollY, panStartScrollY - dy);
        repaint();
        return;
    }

    if (dragMode == DragMode::LoopIn || dragMode == DragMode::LoopOut
        || dragMode == DragMode::PunchIn || dragMode == DragMode::PunchOut)
    {
        const double sr  = juce::jmax (1.0, engine.getCurrentSampleRate());
        const float  bpm = session.tempoBpm.load (std::memory_order_relaxed);
        auto& transport  = engine.getTransport();
        const auto tickHere = juce::jmax<juce::int64> (0, tickForX (e.x));
        const auto tl = r->timelineStart + ticksToSamples (tickHere, sr, bpm);
        switch (dragMode)
        {
            case DragMode::LoopIn:   transport.setLoopRange  (juce::jmin (tl, transport.getLoopEnd()),  transport.getLoopEnd());  break;
            case DragMode::LoopOut:  transport.setLoopRange  (transport.getLoopStart(), juce::jmax (tl, transport.getLoopStart())); break;
            case DragMode::PunchIn:  transport.setPunchRange (juce::jmin (tl, transport.getPunchOut()), transport.getPunchOut()); break;
            case DragMode::PunchOut: transport.setPunchRange (transport.getPunchIn(),  juce::jmax (tl, transport.getPunchIn()));  break;
            default: break;
        }
        repaint();
        return;
    }

    if (dragMode == DragMode::BoxSelect)
    {
        // Rubber band tracks from mouseDown origin to current point.
        // Keep the rect in screen coords so finalize can intersect
        // against each note's painted rect directly.
        const int x0 = e.getMouseDownX();
        const int y0 = e.getMouseDownY();
        rubberBand = juce::Rectangle<int> (juce::jmin (x0, e.x), juce::jmin (y0, e.y),
                                              std::abs (e.x - x0), std::abs (e.y - y0));
        repaint();
        return;
    }

    if (dragMode == DragMode::RangeSelect)
    {
        rangeEndTick = juce::jmax<juce::int64> (0, tickForX (e.x));
        rangeActive  = (rangeEndTick != rangeStartTick);
        // Live note auto-selection - rebuild from scratch each tick so
        // the user sees the selection grow / shrink with the drag.
        clearSelection();
        if (rangeActive)
        {
            const auto a = juce::jmin (rangeStartTick, rangeEndTick);
            const auto b = juce::jmax (rangeStartTick, rangeEndTick);
            for (int i = 0; i < (int) r->notes.size(); ++i)
            {
                const auto t = r->notes[(size_t) i].startTick;
                if (t >= a && t < b) addToSelection (i);
            }
        }
        refreshStatusBarReadouts();
        repaint();
        return;
    }

    if (dragMode == DragMode::EditCcValue)
    {
        if (draggedCcIdx < 0 || draggedCcIdx >= (int) r->ccs.size()) return;
        const auto ccArea = juce::Rectangle<int> (
            kKeyboardWidth, getHeight() - kStatusBarH - ccStripH,
            getWidth() - kKeyboardWidth, ccStripH);
        const float frac = juce::jlimit (0.0f, 1.0f,
            1.0f - ((float) (e.y - ccArea.getY())
                      / (float) juce::jmax (1, ccArea.getHeight())));
        r->ccs[(size_t) draggedCcIdx].value =
            juce::jlimit (0, 127, (int) std::round (frac * 127.0f));
        repaint();
        return;
    }

    if (dragMode == DragMode::ResizeVelocityStrip
        || dragMode == DragMode::ResizeCcStrip)
    {
        // Drag UP = grow this strip (cursor moves to a smaller y). The
        // grid area shrinks correspondingly. Clamp so the grid retains
        // a sensible minimum height even with both strips at max.
        const int delta = resizeStartMouseY - e.y;
        const int topBandH = kToolbarHeight + kHeaderHeight;
        constexpr int kMinGridH = 80;

        if (dragMode == DragMode::ResizeVelocityStrip)
        {
            const int maxAllowed = juce::jmax (kVelocityStripHMin,
                getHeight() - topBandH - ccStripH - kStatusBarH - kMinGridH);
            velocityStripH = juce::jlimit (kVelocityStripHMin,
                juce::jmin (kVelocityStripHMax, maxAllowed),
                resizeStartStripH + delta);
        }
        else
        {
            const int maxAllowed = juce::jmax (kCcStripHMin,
                getHeight() - topBandH - velocityStripH - kStatusBarH - kMinGridH);
            ccStripH = juce::jlimit (kCcStripHMin,
                juce::jmin (kCcStripHMax, maxAllowed),
                resizeStartStripH + delta);
        }
        repaint();
        return;
    }

    if (dragAnchor < 0 || dragAnchor >= (int) r->notes.size()) return;
    auto& anchor = r->notes[(size_t) dragAnchor];

    if (dragMode == DragMode::MoveNote)
    {
        // Compute the snapped delta against the anchor, then apply that
        // same delta to every selected note via applyGroupMove. This
        // keeps relative spacing across the whole selection.
        const auto newAnchorStart = snapTick (tickForX (e.x) - dragOriginTick, snapTicks);
        const int  newAnchorPitch = juce::jlimit (0, kNumKeys - 1, noteNumberForY (e.y));
        const auto deltaTicks = newAnchorStart - dragNoteStartTick;
        const int  deltaPitch = newAnchorPitch - dragOriginNoteNum;
        applyGroupMove (deltaTicks, deltaPitch);
        repaint();
    }
    else if (dragMode == DragMode::EditVelocity)
    {
        const auto velocityArea = juce::Rectangle<int> (
            kKeyboardWidth, getHeight() - kStatusBarH - ccStripH - velocityStripH,
            getWidth() - kKeyboardWidth, velocityStripH);
        const float frac = juce::jlimit (0.0f, 1.0f,
            1.0f - ((float) (e.y - velocityArea.getY())
                      / (float) juce::jmax (1, velocityArea.getHeight())));
        anchor.velocity = juce::jlimit (1, 127, (int) std::round (frac * 127.0f));
        repaint();
    }
    else if (dragMode == DragMode::ResizeNote)
    {
        // Resize the anchor note only; multi-resize is ambiguous and
        // most DAWs match this behaviour.
        const auto rawEnd  = tickForX (e.x);
        const auto snapped = snapTick (rawEnd, snapTicks);
        const auto endTick = juce::jlimit<juce::int64> (
            anchor.startTick + 1, r->lengthInTicks, snapped);
        anchor.lengthInTicks = endTick - anchor.startTick;
        repaint();
    }
    else if (dragMode == DragMode::EditNoteVelocity)
    {
        // Vertical drag on a note body adjusts its velocity. Up =
        // louder, down = quieter. ~0.5 velocity-units per pixel
        // gives a 254-unit range across a 250 px drag, so the user
        // never has to fight the cursor edge to reach the extremes.
        // Apply the same delta to every selected note via the
        // dragSnapshots' velocity baselines (preserves relative
        // velocities within the selection).
        const float deltaPx = (float) (e.getMouseDownY() - e.y);
        const int   delta   = (int) std::round (deltaPx * 0.5f);
        for (size_t i = 0; i < selectedNotes.size(); ++i)
        {
            const int idx = selectedNotes[i];
            if (idx < 0 || idx >= (int) r->notes.size()) continue;
            if (i >= dragSnapshots.size()) continue;
            r->notes[(size_t) idx].velocity =
                juce::jlimit (1, 127, dragSnapshots[i].velocity + delta);
        }
        repaint();
    }
}

void PianoRollComponent::pushCursorPosition (int x, int y)
{
    if (! (onMouseMovedForCursor && onMouseExitedForCursor)) return;

    // During an active drag (move / resize / box / range / velocity) the
    // native cursor owns the gesture - clear the overlay glyph rather than
    // fight it.
    if (dragMode != DragMode::None) { onMouseExitedForCursor(); return; }

    const auto grid = noteGridArea();
    const auto m    = session.editMode;
    const bool inContent = grid.contains (x, y);
    // Over a note, mouseMove sets a native resize / drag cursor; suppress the
    // overlay glyph there so the two don't show at once (empty grid keeps it).
    bool onEdge = false;
    const bool overNote = inContent && hitTestNote (x, y, onEdge) >= 0;
    // Only Draw paints a glyph on empty grid (pencil). Grab uses the plain
    // arrow over empty grid and a native drag-hand over a note body, so it
    // never needs the overlay glyph. Range / Cut / Grid stay native too.
    const bool wantsGlyph = inContent && ! overNote && m == EditMode::Draw;
    if (wantsGlyph) onMouseMovedForCursor (*this, juce::Point<int> (x, y), m, {});
    else            onMouseExitedForCursor();
}

void PianoRollComponent::mouseExit (const juce::MouseEvent&)
{
    if (onMouseExitedForCursor) onMouseExitedForCursor();
}

void PianoRollComponent::mouseMove (const juce::MouseEvent& e)
{
    pushCursorPosition (e.x, e.y);

    const auto mode = session.editMode;

    // Resize-handle hover feedback for the velocity / cc lane top edges.
    // The handle is the kStripResizeGrabPx-tall strip above each lane.
    const int ccTop  = getHeight() - kStatusBarH - ccStripH;
    const int velTop = ccTop - velocityStripH;
    if (e.x >= kKeyboardWidth)
    {
        if (e.y >= velTop - kStripResizeGrabPx && e.y < velTop)
        {
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
            return;
        }
        if (e.y >= ccTop - kStripResizeGrabPx && e.y < ccTop)
        {
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
            return;
        }
    }

    // Loop / punch bracket edges in the ruler band get the resize cursor
    // so the draggable edges are discoverable.
    const auto rulerBand = juce::Rectangle<int> (kKeyboardWidth, kToolbarHeight,
                                                    getWidth() - kKeyboardWidth, kHeaderHeight);
    if (rulerBand.contains (e.x, e.y))
    {
        if (const auto* r = region())
        {
            const double sr  = juce::jmax (1.0, engine.getCurrentSampleRate());
            const float  bpm = session.tempoBpm.load (std::memory_order_relaxed);
            auto& transport  = engine.getTransport();
            auto xForSample  = [&] (juce::int64 s)
                { return xForTick (samplesToTicks (s - r->timelineStart, sr, bpm)); };
            const juce::int64 edges[] = { transport.getLoopStart(), transport.getLoopEnd(),
                                          transport.getPunchIn(),   transport.getPunchOut() };
            const bool valid[] = { transport.getLoopEnd()  > transport.getLoopStart(),
                                   transport.getLoopEnd()  > transport.getLoopStart(),
                                   transport.getPunchOut() > transport.getPunchIn(),
                                   transport.getPunchOut() > transport.getPunchIn() };
            for (int i = 0; i < 4; ++i)
                if (valid[i] && std::abs (xForSample (edges[i]) - e.x) <= 6)
                {
                    setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
                    return;
                }
        }
    }

    // Chrome (keyboard column / ruler / velocity strip / cc strip /
    // status bar) keeps the normal arrow regardless of edit mode so
    // mode-flips don't show a hand / scissors over UI that can't act
    // on a grid click.
    const auto grid = noteGridArea();
    if (! grid.contains (e.x, e.y))
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }

    // Cursor feedback so the user can tell when Alt-on-note will do
    // something different. Without this, the only signal is the
    // resulting drag - too late.
    bool onEdge = false;
    const int hit = hitTestNote (e.x, e.y, onEdge);
    if (hit < 0)
    {
        // Empty grid: Draw = pencil (next click creates a note), painted by
        // the CursorOverlay, so hide the native cursor. Grab gets the plain
        // arrow here - the hand is reserved for hovering a note body. Other
        // modes are inert on the grid - normal arrow.
        if (mode == EditMode::Draw)
            setMouseCursor (juce::MouseCursor::NoCursor);
        else
            setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }
    if (e.mods.isAltDown() && ! onEdge)
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        return;
    }
    setMouseCursor (onEdge ? juce::MouseCursor::LeftRightResizeCursor
                            : juce::MouseCursor::DraggingHandCursor);
}

void PianoRollComponent::mouseUp (const juce::MouseEvent&)
{
    // Pan ends with no state to commit - clear the drag state and
    // restore the default cursor (mouseMove will update it on the next
    // motion to reflect whatever the cursor is now hovering over).
    if (dragMode == DragMode::Pan)
    {
        dragMode = DragMode::None;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }
    if (dragMode == DragMode::LoopIn || dragMode == DragMode::LoopOut
        || dragMode == DragMode::PunchIn || dragMode == DragMode::PunchOut)
    {
        dragMode = DragMode::None;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }
    if (dragMode == DragMode::BoxSelect)
    {
        // Finalise selection: every note whose painted rect intersects
        // the rubber band joins the existing selection. We work in
        // screen coords because that's what the rubber band tracks.
        const auto* r = region();
        if (r != nullptr && ! rubberBand.isEmpty())
        {
            for (int i = 0; i < (int) r->notes.size(); ++i)
            {
                const auto& n = r->notes[(size_t) i];
                const int x0 = xForTick (n.startTick);
                const int x1 = xForTick (n.startTick + n.lengthInTicks);
                const int yy = yForNoteNumber (n.noteNumber);
                const auto noteRect = juce::Rectangle<int> (x0, yy + 1,
                                                              juce::jmax (2, x1 - x0),
                                                              kNoteHeight - 2);
                if (rubberBand.intersects (noteRect))
                    addToSelection (i);
            }
        }
        rubberBand = {};
        repaint();
    }
    dragMode = DragMode::None;
    dragAnchor = -1;
    draggedCcIdx = -1;
    dragSnapshots.clear();
}

bool PianoRollComponent::keyPressed (const juce::KeyPress& k)
{
    // Edit-mode shortcuts. The modal grabs keyboard focus on open so
    // MainComponent::keyPressed doesn't see these — handle locally.
    if (! k.getModifiers().isAnyModifierKeyDown())
    {
        const auto ch = k.getTextCharacter();
        const bool isG = (ch == 'g' || ch == 'G');
        const bool isD = (ch == 'd' || ch == 'D');
        if (isG || isD)
        {
            session.editMode = isG ? EditMode::Grab : EditMode::Draw;
            syncEditModeToolbar();
            repaint();
            return true;
        }
    }

    // Cmd/Ctrl + Left/Right pans the view horizontally; Home / End jump
    // to the start of the region. Lets the user scroll without a trackpad.
    const bool cmdOrCtrl = k.getModifiers().isCommandDown()
                              || k.getModifiers().isCtrlDown();
    auto scrollUpperBound = [this] (int gridW)
    {
        const auto* r = region();
        const double regionPx = (r != nullptr)
                                 ? (double) r->lengthInTicks * pixelsPerTick
                                 : 0.0;
        const double maxScroll = juce::jmax (0.0, regionPx - (double) gridW);
        return (int) juce::jlimit (0.0,
                                      (double) std::numeric_limits<int>::max(),
                                      maxScroll);
    };
    if (cmdOrCtrl && (k == juce::KeyPress::leftKey || k == juce::KeyPress::rightKey))
    {
        const int gridW = juce::jmax (1, getWidth() - kKeyboardWidth);
        const int step  = juce::jmax (1, gridW / 4);
        const int maxScroll = scrollUpperBound (gridW);
        scrollX = juce::jlimit (0, maxScroll,
                                   scrollX + (k == juce::KeyPress::leftKey ? -step : step));
        repaint();
        return true;
    }
    if (k == juce::KeyPress::homeKey)
    {
        scrollX = 0;
        repaint();
        return true;
    }
    if (k == juce::KeyPress::endKey)
    {
        const int gridW = juce::jmax (1, getWidth() - kKeyboardWidth);
        scrollX = scrollUpperBound (gridW);
        repaint();
        return true;
    }

    if (k == juce::KeyPress::backspaceKey || k == juce::KeyPress::deleteKey)
    {
        auto* r = region();
        if (r == nullptr || selectedNotes.empty()) return false;
        // Erase in descending index order so earlier indices stay valid
        // through the loop. selectedNotes is sorted ascending, so walk
        // backwards.
        for (auto it = selectedNotes.rbegin(); it != selectedNotes.rend(); ++it)
        {
            const int idx = *it;
            if (idx >= 0 && idx < (int) r->notes.size())
                r->notes.erase (r->notes.begin() + idx);
        }
        clearSelection();
        repaint();
        return true;
    }
    if (k == juce::KeyPress::escapeKey)
    {
        // Clear in order of "narrowest current state first": an active
        // time-range, then any note selection, then close the overlay
        // when nothing's left to clear.
        if (rangeActive)
        {
            rangeActive = false;
            clearSelection();
            repaint();
            return true;
        }
        if (! selectedNotes.empty())
        {
            clearSelection();
            repaint();
            return true;
        }
        if (onCloseRequested) onCloseRequested();
        return true;
    }
    // Region navigation + local undo/redo. Same reasoning as the audio
    // editor's handler: the engine's UndoManager has a global key binding
    // but the editor needs an explicit refresh + repaint to pick up the
    // model change.
    {
        const bool cmdOrCtrl = k.getModifiers().isCommandDown()
                                  || k.getModifiers().isCtrlDown();
        if (cmdOrCtrl && (k.getKeyCode() == 'Z' || k.getKeyCode() == 'z'))
        {
            if (k.getModifiers().isShiftDown()) engine.getUndoManager().redo();
            else                                  engine.getUndoManager().undo();
            rangeActive = false;
            clearSelection();
            refreshStatusBarReadouts();
            repaint();
            return true;
        }
        if (cmdOrCtrl && k.getTextCharacter() == ']')
        {
            navigateRegion (+1);
            return true;
        }
        if (cmdOrCtrl && k.getTextCharacter() == '[')
        {
            navigateRegion (-1);
            return true;
        }
        // Loop enable toggles on Cmd/Ctrl+L — bare 'L' is taken by the CC-
        // controller cycle further down, so the loop toggle takes the modifier.
        if (cmdOrCtrl && k.getKeyCode() == 'L')
        {
            auto& transport = engine.getTransport();
            transport.setLoopEnabled (! transport.isLoopEnabled());
            repaint();
            return true;
        }
        // Loop / punch points against the edit cursor. Bare [ / ] set the loop
        // in/out; Shift+[ / Shift+] set the punch in/out; bare P toggles punch.
        // Mirrors the audio region editor, but the roll works in ticks so the
        // cursor maps through the region anchor.
        // On Linux/X11 getKeyCode() returns the SHIFTED glyph, so Shift+[ comes
        // through as '{' and Shift+] as '}' — fold them back to [ / ].
        const int  rawKc = k.getKeyCode();
        const int  kc = (rawKc == '{') ? '[' : (rawKc == '}') ? ']' : rawKc;
        const bool sh = k.getModifiers().isShiftDown();
        if (! cmdOrCtrl && ! k.getModifiers().isAltDown()
            && (kc == '[' || kc == ']' || kc == 'P'))
        {
            auto& transport = engine.getTransport();
            if (kc == 'P' && ! sh) { transport.setPunchEnabled (! transport.isPunchEnabled()); repaint(); return true; }
            if (const auto* r = region(); r != nullptr && (kc == '[' || kc == ']'))
            {
                const double sr  = juce::jmax (1.0, engine.getCurrentSampleRate());
                const float  bpm = session.tempoBpm.load (std::memory_order_relaxed);
                const auto cursorTl = r->timelineStart + ticksToSamples (editCursorTick, sr, bpm);
                if (kc == '[') { if (sh) transport.setPunchRange (cursorTl, juce::jmax (transport.getPunchOut(), cursorTl));
                                 else    transport.setLoopRange  (cursorTl, juce::jmax (transport.getLoopEnd(),  cursorTl)); }
                else { if (sh) transport.setPunchRange (juce::jmin (transport.getPunchIn(),   cursorTl), cursorTl);
                       else    transport.setLoopRange  (juce::jmin (transport.getLoopStart(), cursorTl), cursorTl); }
                repaint(); return true;
            }
        }
        // Select all notes in the open region. Replaces any existing
        // selection so a follow-up Delete or transpose acts on every
        // note at once. Clears the time range so it doesn't compete
        // with the new full-region selection.
        if (cmdOrCtrl && (k.getKeyCode() == 'A' || k.getKeyCode() == 'a'))
        {
            auto* r = region();
            if (r == nullptr) return true;
            clearSelection();
            for (int i = 0; i < (int) r->notes.size(); ++i)
                addToSelection (i);
            rangeActive = false;
            refreshStatusBarReadouts();
            repaint();
            return true;
        }
        // Copy/Cut/Paste of MIDI notes via the static sNoteClipboard.
        // startTicks stored RELATIVE to the earliest selected note so
        // paste lands the cluster at editCursorTick regardless of the
        // source region's anchor (matches Logic / Reaper behaviour).
        if (cmdOrCtrl && (k.getKeyCode() == 'C' || k.getKeyCode() == 'c'))
        {
            auto* r = region();
            if (r == nullptr || selectedNotes.empty()) return true;
            sNoteClipboard.clear();
            juce::int64 minTick = std::numeric_limits<juce::int64>::max();
            for (int idx : selectedNotes)
                if (idx >= 0 && idx < (int) r->notes.size())
                    minTick = juce::jmin (minTick, r->notes[(size_t) idx].startTick);
            for (int idx : selectedNotes)
            {
                if (idx < 0 || idx >= (int) r->notes.size()) continue;
                MidiNote n = r->notes[(size_t) idx];
                n.startTick -= minTick;       // normalize against earliest
                sNoteClipboard.push_back (n);
            }
            return true;
        }
        if (cmdOrCtrl && (k.getKeyCode() == 'X' || k.getKeyCode() == 'x'))
        {
            auto* r = region();
            if (r == nullptr || selectedNotes.empty()) return true;
            // Copy first, then delete the selection (same shape as the
            // Delete-key handler below). Mutates the live MidiRegion
            // directly - matches the existing note-edit pattern in this
            // file; not undoable (note edits aren't yet, by design).
            sNoteClipboard.clear();
            juce::int64 minTick = std::numeric_limits<juce::int64>::max();
            for (int idx : selectedNotes)
                if (idx >= 0 && idx < (int) r->notes.size())
                    minTick = juce::jmin (minTick, r->notes[(size_t) idx].startTick);
            for (int idx : selectedNotes)
            {
                if (idx < 0 || idx >= (int) r->notes.size()) continue;
                MidiNote n = r->notes[(size_t) idx];
                n.startTick -= minTick;
                sNoteClipboard.push_back (n);
            }
            for (auto it = selectedNotes.rbegin(); it != selectedNotes.rend(); ++it)
                if (*it >= 0 && *it < (int) r->notes.size())
                    r->notes.erase (r->notes.begin() + *it);
            clearSelection();
            repaint();
            return true;
        }
        if (cmdOrCtrl && (k.getKeyCode() == 'V' || k.getKeyCode() == 'v'))
        {
            auto* r = region();
            if (r == nullptr || sNoteClipboard.empty()) return true;
            // Paste at editCursorTick. Each clone is clamped against
            // the target region's bounds; notes that would land past
            // the end are dropped. The pasted notes become the new
            // selection so a follow-up nudge / transpose acts on them.
            clearSelection();
            for (const auto& src : sNoteClipboard)
            {
                MidiNote n = src;
                n.startTick = editCursorTick + src.startTick;
                if (n.startTick < 0) continue;
                if (n.startTick >= r->lengthInTicks) continue;
                if (n.startTick + n.lengthInTicks > r->lengthInTicks)
                    n.lengthInTicks = r->lengthInTicks - n.startTick;
                r->notes.push_back (n);
                addToSelection ((int) r->notes.size() - 1);
            }
            refreshStatusBarReadouts();
            repaint();
            return true;
        }
    }
    // Arrow-key transpose. Up/Down = ±1 semitone; Shift+Up/Down = ±12.
    // Acts on every selected note. Left/Right are reserved for future
    // tick-nudge (Phase 2c quantize work).
    if (! selectedNotes.empty())
    {
        if (k == juce::KeyPress::upKey)
        {
            transposeSelected (k.getModifiers().isShiftDown() ? 12 : 1);
            repaint();
            return true;
        }
        if (k == juce::KeyPress::downKey)
        {
            transposeSelected (k.getModifiers().isShiftDown() ? -12 : -1);
            repaint();
            return true;
        }
    }
    // Quantize shortcuts. Number keys swap the snap resolution; '0' = free.
    // Values are denominators of a quarter note (1 = whole at 4/4, 4 = 16th).
    // Picked the most common entry grids; the user can extend with triplet
    // / dotted variants in a polish pass.
    if (k.getKeyCode() == '1') { snapTicks = kMidiTicksPerQuarter * 4; repaint(); return true; }   // whole
    if (k.getKeyCode() == '2') { snapTicks = kMidiTicksPerQuarter * 2; repaint(); return true; }   // half
    if (k.getKeyCode() == '3') { snapTicks = kMidiTicksPerQuarter;     repaint(); return true; }   // quarter
    if (k.getKeyCode() == '4') { snapTicks = kMidiTicksPerQuarter / 2; repaint(); return true; }   // 8th
    if (k.getKeyCode() == '5') { snapTicks = kMidiTicksPerQuarter / 4; repaint(); return true; }   // 16th (default)
    if (k.getKeyCode() == '6') { snapTicks = kMidiTicksPerQuarter / 8; repaint(); return true; }   // 32nd
    if (k.getKeyCode() == '0') { snapTicks = 0;                       repaint(); return true; }    // free
    // 'C' cycles the colour mode: Pitch -> Velocity -> Channel -> Pitch.
    // 'C' was free in the existing key map (the 1..6/0 family + delete +
    // Esc are the only bindings). Lowercase falls through if Caps Lock is
    // off, so we match either case.
    if (k.getKeyCode() == 'C')
    {
        colorMode = colorMode == ColorMode::Pitch    ? ColorMode::Velocity
                  : colorMode == ColorMode::Velocity ? ColorMode::Channel
                                                      : ColorMode::Pitch;
        repaint();
        return true;
    }
    // 'L' cycles the active CC controller: 1 (mod) -> 7 (vol) -> 11
    // (express) -> 64 (sustain) -> 74 (filter) -> back to 1. Covers the
    // most-used continuous controllers; uncommon ones can still be
    // captured via Record (the region's ccs vector holds them all) and
    // viewed by extending this rotation later.
    if (k.getKeyCode() == 'L')
    {
        activeCcController =
            activeCcController == 1   ?  7 :
            activeCcController == 7   ? 11 :
            activeCcController == 11  ? 64 :
            activeCcController == 64  ? 74 :
                                          1;
        repaint();
        return true;
    }
    // 'Q' opens the quantize popup. Action runs on selected notes (or
    // every note if nothing selected).
    if (k.getKeyCode() == 'Q')
    {
        showQuantizePopup();
        return true;
    }
    // 'S' opens the scale-highlight picker (root × mode).
    if (k.getKeyCode() == 'S')
    {
        showScalePopup();
        return true;
    }
    // 'V' opens the velocity popup (humanise + set-to). Like the
    // quantize popup, an empty selection means "apply to whole region".
    if (k.getKeyCode() == 'V')
    {
        showVelocityPopup();
        return true;
    }
    // 'G' glues every selected same-pitch contiguous note pair into
    // one note. Useful for repairing stutter from sloppy playing
    // captured by the recorder. No-op when fewer than two notes are
    // selected.
    if (k.getKeyCode() == 'G' && selectedNotes.size() >= 2)
    {
        glueSelectedNotes();
        repaint();
        return true;
    }
    // Cmd/Ctrl+D duplicates every selected note - clones land
    // immediately after the selection's span, replacing the
    // selection with the new copies so the user can keep nudging /
    // transposing the duplicate without an extra click.
    if (k.getKeyCode() == 'D'
        && k.getModifiers().isCommandDown()
        && ! selectedNotes.empty())
    {
        duplicateSelectedNotes();
        repaint();
        return true;
    }
    // Left / Right arrow keys nudge every selected note by one snap
    // step. Falls back to a fine-grained 30-tick nudge when snap is
    // off so the keys still do something useful. Shift+arrow nudges
    // by a full beat for coarse positioning.
    if (! selectedNotes.empty()
        && (k == juce::KeyPress::leftKey || k == juce::KeyPress::rightKey))
    {
        const bool coarse = k.getModifiers().isShiftDown();
        const juce::int64 step =
            coarse           ? kMidiTicksPerQuarter :
            (snapTicks > 0)  ? snapTicks :
                                30;
        nudgeSelectedTicks (k == juce::KeyPress::leftKey ? -step : step);
        repaint();
        return true;
    }

    // Zoom shortcuts: '=' / '+' zoom in, '-' zoom out. Anchor on the
    // centre of the visible grid via zoomByFactor.
    {
        const auto ch = k.getTextCharacter();
        if (ch == '=' || ch == '+')
        {
            zoomByFactor (1.15f);
            return true;
        }
        if (ch == '-')
        {
            zoomByFactor (1.0f / 1.15f);
            return true;
        }
    }
    return false;
}

void PianoRollComponent::mouseWheelMove (const juce::MouseEvent& e,
                                          const juce::MouseWheelDetails& w)
{
    // Wheel inside the velocity / cc strip = zoom that strip vertically.
    // Wheel-up grows, wheel-down shrinks. Same min/max envelope as the
    // drag-resize gesture so both gestures land in the same valid range.
    const int ccTop  = getHeight() - kStatusBarH - ccStripH;
    const int velTop = ccTop - velocityStripH;
    if (e.x >= kKeyboardWidth)
    {
        const int topBandH = kToolbarHeight + kHeaderHeight;
        constexpr int kMinGridH = 80;
        const int delta = (int) std::round (w.deltaY * 32.0f);  // 1 click ~ 32 px

        if (e.y >= velTop && e.y < ccTop)
        {
            const int maxAllowed = juce::jmax (kVelocityStripHMin,
                getHeight() - topBandH - ccStripH - kStatusBarH - kMinGridH);
            velocityStripH = juce::jlimit (kVelocityStripHMin,
                juce::jmin (kVelocityStripHMax, maxAllowed),
                velocityStripH + delta);
            repaint();
            return;
        }
        if (e.y >= ccTop && e.y < ccTop + ccStripH)
        {
            const int maxAllowed = juce::jmax (kCcStripHMin,
                getHeight() - topBandH - velocityStripH - kStatusBarH - kMinGridH);
            ccStripH = juce::jlimit (kCcStripHMin,
                juce::jmin (kCcStripHMax, maxAllowed),
                ccStripH + delta);
            repaint();
            return;
        }
    }

    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        // Horizontal zoom anchored on the cursor's tick: capture the tick
        // under the cursor BEFORE changing pixelsPerTick, then adjust
        // scrollX so the same tick stays under the cursor afterwards.
        // This makes zoom feel like it's pulling the timeline through
        // the cursor instead of resetting to tick 0.
        const auto cursorTickBefore = tickForX (e.x);
        const float factor = w.deltaY > 0.0f ? 1.15f : (1.0f / 1.15f);
        pixelsPerTick = juce::jlimit (0.005f, 1.0f, pixelsPerTick * factor);
        const int newCursorPx = (int) std::round ((double) cursorTickBefore * pixelsPerTick);
        scrollX = juce::jmax (0, newCursorPx - (e.x - kKeyboardWidth));
        repaint();
        return;
    }

    // Horizontal scroll: trackpad delta-X always counts; shift+wheel maps
    // a vertical wheel onto horizontal motion. Vertical scroll is the
    // default.
    if (std::abs (w.deltaX) > 0.001f || e.mods.isShiftDown())
    {
        const float dx = std::abs (w.deltaX) > 0.001f ? w.deltaX : w.deltaY;
        const int dxPx = (int) (-dx * 120.0f);
        scrollX = juce::jmax (0, scrollX + dxPx);
        repaint();
        return;
    }
    // Vertical scroll. wheel deltaY > 0 = scroll up (show higher notes).
    const int delta = (int) (-w.deltaY * 60.0f);
    const int maxScroll = juce::jmax (0,
        kFullGridHeight - (getHeight() - kToolbarHeight - kHeaderHeight
                              - velocityStripH - ccStripH - kStatusBarH));
    scrollY = juce::jlimit (0, maxScroll, scrollY + delta);
    repaint();
}

// ── Top icon-row buttons ──────────────────────────────────────────────
PianoRollComponent::IconButton::IconButton (const juce::String& name, Glyph g)
    : juce::Button (name), glyph (g)
{
    setClickingTogglesState (false);
    // Don't steal keyboard focus from the host modal so hotkeys keep
    // routing to PianoRollComponent::keyPressed after a button click.
    setMouseClickGrabsKeyboardFocus (false);
    setWantsKeyboardFocus (false);
}

void PianoRollComponent::IconButton::paintButton (juce::Graphics& g,
                                                    bool isMouseOver, bool isButtonDown)
{
    const auto bounds = getLocalBounds().toFloat().reduced (2.0f);
    auto disc = juce::Colour (0xff262630);
    if (isButtonDown)      disc = disc.brighter (0.14f);
    else if (isMouseOver)  disc = disc.brighter (0.08f);
    g.setColour (disc);
    g.fillEllipse (bounds);
    g.setColour (juce::Colour (0xff3a3a44));
    g.drawEllipse (bounds, isButtonDown ? 2.0f : 1.4f);

    const auto centre = bounds.getCentre();
    const float r = bounds.getWidth() * 0.30f;   // glyph extent
    g.setColour (juce::Colour (0xffd0d0d0));

    auto strokeArrowhead = [&] (juce::Point<float> tip, juce::Point<float> from,
                                  float size)
    {
        // Small open arrowhead pointing toward `tip` from `from`.
        const auto dir = (tip - from);
        const auto len = juce::jmax (0.001f, dir.getDistanceFromOrigin());
        const auto u   = dir / len;
        const auto perp = juce::Point<float> (-u.y, u.x);
        const auto a = tip - u * size + perp * size * 0.6f;
        const auto b = tip - u * size - perp * size * 0.6f;
        juce::Path p;
        p.startNewSubPath (a);
        p.lineTo (tip);
        p.lineTo (b);
        g.strokePath (p, juce::PathStrokeType (1.6f));
    };

    switch (glyph)
    {
        case Glyph::Undo:
        case Glyph::Redo:
        {
            // Curved arrow forming about 250 deg of a circle, with an
            // arrowhead at the open end. Mirror horizontally for Redo.
            const float radius = r * 0.95f;
            juce::Path arc;
            arc.addCentredArc (centre.x, centre.y, radius, radius,
                                  0.0f,
                                  juce::MathConstants<float>::pi * 0.30f,
                                  juce::MathConstants<float>::pi * 1.85f,
                                  true);
            if (glyph == Glyph::Redo)
            {
                juce::AffineTransform mirror;
                mirror = juce::AffineTransform::scale (-1.0f, 1.0f, centre.x, centre.y);
                arc.applyTransform (mirror);
            }
            g.strokePath (arc, juce::PathStrokeType (1.6f));
            // Arrowhead at the start of the arc (top-left for Undo).
            const float startAng = juce::MathConstants<float>::pi * 0.30f;
            juce::Point<float> start (centre.x + std::sin (startAng) * radius,
                                          centre.y - std::cos (startAng) * radius);
            if (glyph == Glyph::Redo) start = juce::Point<float> (2 * centre.x - start.x, start.y);
            const auto inwards = juce::Point<float> (centre.x, centre.y) - start;
            strokeArrowhead (start, start + inwards * 0.001f, 4.0f);
            break;
        }
        case Glyph::Split:
        {
            // Vertical bar with a small gap and arrows pointing apart.
            const float h = r * 1.6f;
            g.fillRect (juce::Rectangle<float> (centre.x - 0.8f,
                                                   centre.y - h * 0.5f, 1.6f, h));
            // Two arrows, one to each side, just above the centre line.
            juce::Path l, ar;
            l.startNewSubPath (centre.x - r * 0.95f, centre.y);
            l.lineTo (centre.x - 2.0f, centre.y);
            ar.startNewSubPath (centre.x + 2.0f, centre.y);
            ar.lineTo (centre.x + r * 0.95f, centre.y);
            g.strokePath (l,  juce::PathStrokeType (1.4f));
            g.strokePath (ar, juce::PathStrokeType (1.4f));
            strokeArrowhead ({ centre.x - r * 0.95f, centre.y },
                                { centre.x - 2.0f,         centre.y }, 3.5f);
            strokeArrowhead ({ centre.x + r * 0.95f, centre.y },
                                { centre.x + 2.0f,         centre.y }, 3.5f);
            break;
        }
        case Glyph::Glue:
        {
            // Two small rectangles touching, with a short arrow under
            // them pointing inward to suggest "joining".
            const float w = r * 0.7f;
            const float h = r * 0.6f;
            g.fillRect (juce::Rectangle<float> (centre.x - w - 0.5f, centre.y - h * 0.5f - 1.0f, w, h));
            g.fillRect (juce::Rectangle<float> (centre.x        + 0.5f, centre.y - h * 0.5f - 1.0f, w, h));
            // Underline showing they merge.
            g.fillRect (juce::Rectangle<float> (centre.x - w - 0.5f, centre.y + h * 0.5f + 1.0f,
                                                   w * 2.0f + 1.0f, 1.5f));
            break;
        }
        case Glyph::Quantize:
        {
            // Three short vertical lines (grid) + a tiny arrow snapping
            // an offset note onto the grid.
            const float yTop = centre.y - r * 0.9f;
            const float yBot = centre.y + r * 0.5f;
            for (int i = -1; i <= 1; ++i)
            {
                const float x = centre.x + (float) i * r * 0.65f;
                g.drawLine (x, yTop, x, yBot, 1.2f);
            }
            // Note glyph below, snapping right.
            g.fillRect (juce::Rectangle<float> (centre.x - r * 0.55f,
                                                   yBot + 1.5f, r * 0.55f, 3.0f));
            strokeArrowhead ({ centre.x + r * 0.05f, yBot + 3.0f },
                                { centre.x - r * 0.10f, yBot + 3.0f }, 3.0f);
            break;
        }
        case Glyph::Properties:
        {
            // Simplified gear: outer ring + 6 short ticks.
            g.drawEllipse (centre.x - r * 0.55f, centre.y - r * 0.55f,
                              r * 1.10f, r * 1.10f, 1.4f);
            for (int i = 0; i < 6; ++i)
            {
                const float ang = juce::MathConstants<float>::twoPi * (float) i / 6.0f;
                const float x0 = centre.x + std::cos (ang) * r * 0.65f;
                const float y0 = centre.y + std::sin (ang) * r * 0.65f;
                const float x1 = centre.x + std::cos (ang) * r * 0.95f;
                const float y1 = centre.y + std::sin (ang) * r * 0.95f;
                g.drawLine (x0, y0, x1, y1, 1.4f);
            }
            // Inner dot.
            g.fillEllipse (centre.x - 1.5f, centre.y - 1.5f, 3.0f, 3.0f);
            break;
        }
        case Glyph::ZoomFit:
        {
            // Square brackets pointing inward: [ . ]
            const float w = r * 0.95f;
            const float h = r * 1.10f;
            const float bracketLen = h * 0.9f;
            const float armLen     = r * 0.45f;
            // Left bracket
            g.drawLine (centre.x - w, centre.y - bracketLen * 0.5f,
                          centre.x - w, centre.y + bracketLen * 0.5f, 1.4f);
            g.drawLine (centre.x - w, centre.y - bracketLen * 0.5f,
                          centre.x - w + armLen, centre.y - bracketLen * 0.5f, 1.4f);
            g.drawLine (centre.x - w, centre.y + bracketLen * 0.5f,
                          centre.x - w + armLen, centre.y + bracketLen * 0.5f, 1.4f);
            // Right bracket
            g.drawLine (centre.x + w, centre.y - bracketLen * 0.5f,
                          centre.x + w, centre.y + bracketLen * 0.5f, 1.4f);
            g.drawLine (centre.x + w, centre.y - bracketLen * 0.5f,
                          centre.x + w - armLen, centre.y - bracketLen * 0.5f, 1.4f);
            g.drawLine (centre.x + w, centre.y + bracketLen * 0.5f,
                          centre.x + w - armLen, centre.y + bracketLen * 0.5f, 1.4f);
            // Centre dot
            g.fillEllipse (centre.x - 1.5f, centre.y - 1.5f, 3.0f, 3.0f);
            break;
        }
        case Glyph::ZoomIn:
        {
            const float rad = r * 0.62f;
            const float cx  = centre.x - r * 0.18f;
            const float cy  = centre.y - r * 0.18f;
            g.drawEllipse (cx - rad, cy - rad, rad * 2.0f, rad * 2.0f, 1.4f);
            g.drawLine (cx + rad * 0.55f, cy + rad * 0.55f,
                          centre.x + r * 0.85f, centre.y + r * 0.85f, 1.8f);
            g.drawLine (cx - rad * 0.5f, cy, cx + rad * 0.5f, cy, 1.4f);
            g.drawLine (cx, cy - rad * 0.5f, cx, cy + rad * 0.5f, 1.4f);
            break;
        }
        case Glyph::ZoomOut:
        {
            const float rad = r * 0.62f;
            const float cx  = centre.x - r * 0.18f;
            const float cy  = centre.y - r * 0.18f;
            g.drawEllipse (cx - rad, cy - rad, rad * 2.0f, rad * 2.0f, 1.4f);
            g.drawLine (cx + rad * 0.55f, cy + rad * 0.55f,
                          centre.x + r * 0.85f, centre.y + r * 0.85f, 1.8f);
            g.drawLine (cx - rad * 0.5f, cy, cx + rad * 0.5f, cy, 1.4f);
            break;
        }
        case Glyph::ToggleCc:
        {
            // Three vertical bars of varying heights (CC envelope).
            const float yBot = centre.y + r * 0.6f;
            const float xs[3] = { centre.x - r * 0.55f, centre.x, centre.x + r * 0.55f };
            const float hs[3] = { r * 0.6f, r * 1.05f, r * 0.85f };
            for (int i = 0; i < 3; ++i)
                g.fillRect (juce::Rectangle<float> (xs[i] - 0.9f, yBot - hs[i],
                                                       1.8f, hs[i]));
            break;
        }
    }
}

void PianoRollComponent::splitSelectedAtCursor()
{
    auto* r = region();
    if (r == nullptr) return;
    const auto cursor = editCursorTick;

    // Operate on the current selection if any; otherwise consider every
    // note in the region. Walk newest-first so push_back inside the
    // loop doesn't disturb the index we're iterating.
    auto candidate = [&] (int idx)
    {
        return ! selectedNotes.empty()
            ? std::binary_search (selectedNotes.begin(), selectedNotes.end(), idx)
            : true;
    };

    const int initialCount = (int) r->notes.size();
    for (int i = initialCount - 1; i >= 0; --i)
    {
        if (! candidate (i)) continue;
        auto& n = r->notes[(size_t) i];
        if (cursor <= n.startTick || cursor >= n.startTick + n.lengthInTicks) continue;

        MidiNote tail = n;
        tail.startTick     = cursor;
        tail.lengthInTicks = (n.startTick + n.lengthInTicks) - cursor;
        n.lengthInTicks    = cursor - n.startTick;
        r->notes.push_back (tail);
    }
    repaint();
}

void PianoRollComponent::zoomFit()
{
    const auto* r = region();
    if (r == nullptr || r->lengthInTicks <= 0) return;
    const int gridW = juce::jmax (200, getWidth() - kKeyboardWidth - 8);
    pixelsPerTick = juce::jlimit (0.005f, 1.0f,
                                       (float) gridW / (float) r->lengthInTicks);
    scrollX = 0;
    repaint();
}

void PianoRollComponent::zoomByFactor (float factor)
{
    // Anchor zoom on the visible centre so the user's working area stays
    // under the eye across a step. Mirrors the Cmd+wheel ramp.
    const int gridLeft = kKeyboardWidth;
    const int viewportW = juce::jmax (1, getWidth() - gridLeft);
    const int centreX = gridLeft + viewportW / 2;
    const auto centreTickBefore = tickForX (centreX);
    pixelsPerTick = juce::jlimit (0.005f, 1.0f, pixelsPerTick * factor);
    const int newCentrePx = (int) std::round ((double) centreTickBefore * pixelsPerTick);
    scrollX = juce::jmax (0, newCentrePx - (centreX - gridLeft));
    repaint();
}

void PianoRollComponent::toggleCcLane()
{
    ccStripH = (ccStripH > 0 ? 0 : kCcStripHDefault);
    repaint();
}

void PianoRollComponent::showRegionPropertiesPopup()
{
    auto* r = region();
    if (r == nullptr) return;

    juce::PopupMenu m;
    m.addSectionHeader (juce::String::formatted ("Track %d  MIDI region %d",
                                                    trackIdx + 1, regionIdx + 1));

    // Rename / label — embedded text-input modal (H2 sweep). Same
    // shape as the audio editor's region-properties popup so users
    // see one consistent gesture across both region types. The panel
    // renders inside the main window's component tree via Embedded
    // Modal — no native juce::AlertWindow on the X11 / Wayland stack.
    const auto currentLabel = r->label;
    const juce::String renameLabel = currentLabel.isEmpty()
        ? juce::String ("Add label...")
        : juce::String ("Rename label...");
    m.addItem (renameLabel,
                [safe = juce::Component::SafePointer<PianoRollComponent> (this),
                 currentLabel]
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr) return;
                    auto* host = self->getTopLevelComponent();
                    if (host == nullptr) host = self;
                    showEmbeddedTextInput (*host, "MIDI region label",
                        "Type a label for this region:",
                        currentLabel,
                        [safe] (juce::String newLabel)
                        {
                            auto* s = safe.getComponent();
                            if (s == nullptr) return;
                            auto* rr = s->region();
                            if (rr == nullptr) return;
                            const MidiRegion before = *rr;
                            MidiRegion after = before;
                            after.label = std::move (newLabel);
                            auto& um = s->engine.getUndoManager();
                            um.beginNewTransaction ("Rename MIDI region");
                            um.perform (new MidiRegionEditAction (s->session, s->engine,
                                                                    s->trackIdx, s->regionIdx,
                                                                    before, after));
                            s->refreshStatusBarReadouts();
                            s->repaint();
                        });
                });

    m.addSeparator();

    m.addItem (r->muted ? "Unmute region" : "Mute region",
                [safe = juce::Component::SafePointer<PianoRollComponent> (this)]
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr) return;
                    auto* rr = self->region();
                    if (rr == nullptr) return;
                    const MidiRegion before = *rr;
                    MidiRegion after = before;
                    after.muted = ! before.muted;
                    auto& um = self->engine.getUndoManager();
                    um.beginNewTransaction (after.muted ? "Mute MIDI region" : "Unmute MIDI region");
                    um.perform (new MidiRegionEditAction (self->session, self->engine,
                                                            self->trackIdx, self->regionIdx,
                                                            before, after));
                    self->muteToggle.setToggleState (after.muted, juce::dontSendNotification);
                    self->repaint();
                });
    m.addItem (r->locked ? "Unlock region" : "Lock region",
                [safe = juce::Component::SafePointer<PianoRollComponent> (this)]
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr) return;
                    auto* rr = self->region();
                    if (rr == nullptr) return;
                    const MidiRegion before = *rr;
                    MidiRegion after = before;
                    after.locked = ! before.locked;
                    auto& um = self->engine.getUndoManager();
                    um.beginNewTransaction (after.locked ? "Lock MIDI region" : "Unlock MIDI region");
                    um.perform (new MidiRegionEditAction (self->session, self->engine,
                                                            self->trackIdx, self->regionIdx,
                                                            before, after));
                    self->lockToggle.setToggleState (after.locked, juce::dontSendNotification);
                    self->repaint();
                });

    m.addSeparator();

    juce::PopupMenu colourSub;
    for (int i = 0; i < kPaletteCount; ++i)
    {
        const bool isReset = (i == 0);
        colourSub.addItem (5000 + i,
                            kPalette[i].label,
                            true,
                            isReset ? r->customColour.isTransparent()
                                    : r->customColour.getARGB() == kPalette[i].argb);
    }
    m.addSubMenu ("Color", colourSub);

    m.addSeparator();
    m.addItem ("Delete region",
                [safe = juce::Component::SafePointer<PianoRollComponent> (this)]
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr) return;
                    auto& um = self->engine.getUndoManager();
                    um.beginNewTransaction ("Delete MIDI region");
                    um.perform (new DeleteMidiRegionAction (self->session, self->engine,
                                                              self->trackIdx, self->regionIdx));
                    if (self->onCloseRequested) self->onCloseRequested();
                });

    // Read-only metadata footer.
    m.addSeparator();
    {
        const int beats = juce::jmax (1, session.beatsPerBar.load (std::memory_order_relaxed));
        const auto bars = r->lengthInTicks / ((juce::int64) kMidiTicksPerQuarter * beats);
        const int channel = r->notes.empty() ? 1 : r->notes.front().channel;
        const auto info = juce::String ((int) r->notes.size()) + " notes  -  "
                            + juce::String ((int) bars) + " bars  -  "
                            + "ch " + juce::String (channel);
        m.addItem (9999, info, false /*disabled*/, false);
    }

    showContextMenu (m, propertiesButton,
        [safe = juce::Component::SafePointer<PianoRollComponent> (this)] (int chosen)
        {
            auto* self = safe.getComponent();
            if (self == nullptr) return;
            if (chosen < 5000 || chosen >= 5000 + kPaletteCount) return;
            auto* rr = self->region();
            if (rr == nullptr) return;
            const juce::Colour newColour { kPalette[chosen - 5000].argb };
            const MidiRegion before = *rr;
            if (before.customColour == newColour) return;
            MidiRegion after = before;
            after.customColour = newColour;
            auto& um = self->engine.getUndoManager();
            um.beginNewTransaction ("Set MIDI region colour");
            um.perform (new MidiRegionEditAction (self->session, self->engine,
                                                    self->trackIdx, self->regionIdx,
                                                    before, after));
            self->repaint();
        });
}

int PianoRollComponent::nextRegionIndex (int delta) const
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return -1;
    const auto& regs = session.track (trackIdx).midiRegions.current();
    if (regionIdx < 0 || regionIdx >= (int) regs.size()) return -1;

    using Key = std::pair<juce::int64, int>;
    const Key cur { regs[(size_t) regionIdx].timelineStart, regionIdx };
    int bestIdx = -1;
    Key best = (delta > 0)
        ? Key { std::numeric_limits<juce::int64>::max(), std::numeric_limits<int>::max() }
        : Key { std::numeric_limits<juce::int64>::min(), std::numeric_limits<int>::min() };
    for (int i = 0; i < (int) regs.size(); ++i)
    {
        if (i == regionIdx) continue;
        const Key k { regs[(size_t) i].timelineStart, i };
        if (delta > 0 ? (k > cur && k < best) : (k < cur && k > best))
        {
            best = k;
            bestIdx = i;
        }
    }
    return bestIdx;
}

void PianoRollComponent::navigateRegion (int delta)
{
    const int newIdx = nextRegionIndex (delta);
    if (newIdx < 0) return;
    if (onNavigateToRegion) onNavigateToRegion (trackIdx, newIdx);
}

int PianoRollComponent::transportPlayheadX (juce::Rectangle<int> gridArea) const
{
    const auto* r = region();
    if (r == nullptr || r->lengthInTicks <= 0) return -1;
    const auto playheadSample = engine.getTransport().getPlayhead();
    const auto localSample = playheadSample - r->timelineStart;
    if (localSample < 0 || localSample > r->lengthInSamples) return -1;
    const double sr = juce::jmax (1.0, engine.getCurrentSampleRate());
    const float bpm = juce::jmax (1.0f, session.tempoBpm.load (std::memory_order_relaxed));
    const auto localTick = samplesToTicks (localSample, sr, bpm);
    if (localTick < 0 || localTick > r->lengthInTicks) return -1;
    const int x = xForTick (localTick);
    if (x < gridArea.getX() || x > gridArea.getRight()) return -1;
    return x;
}

void PianoRollComponent::paintTransportPlayhead (juce::Graphics& g,
                                                    juce::Rectangle<int> gridArea)
{
    // Cache-vs-live decision by clip width:
    //   Narrow clip (timerCallback's partial-band invalidation): use
    //     lastPlayheadX so the painted line lands inside the dirty
    //     band that was queued for it. Without this the audio
    //     thread's between-tick advance would put a live X outside
    //     the band, JUCE's clip would eat its pixels and the line
    //     renders torn / partial.
    //   Wide clip (zoom, scroll, region change, parent resize,
    //     initial paint): use a fresh live read because the cache is
    //     in OLD pixel coords for any coord-space mutator that
    //     repainted the whole grid; the live read is correct for
    //     the current frame and the wide dirty rect cleanly
    //     contains it.
    constexpr int kNarrowBandWidth = 64;
    const int clipW = g.getClipBounds().getWidth();
    const bool useCache = clipW > 0
                            && clipW < kNarrowBandWidth
                            && lastPlayheadX >= 0;
    const int x = useCache ? lastPlayheadX : transportPlayheadX (gridArea);
    if (x < 0) return;
    juce::Graphics::ScopedSaveState saved (g);
    g.reduceClipRegion (gridArea);
    g.setColour (kTransportPlayhead.withAlpha (0.85f));
    g.drawVerticalLine (x, (float) gridArea.getY(), (float) gridArea.getBottom());
}

void PianoRollComponent::timerCallback()
{
    // Mirror scrollX onto the scrollbar every tick so paths that mutate
    // it (key arrows, mouse wheel, pan drag, zoom recentre) all stay
    // in visual sync.
    syncScrollBarRange();

    // Toolbar button enabled-state + toggle sync. Polled because
    // UndoManager + region atoms have no change-broadcaster we listen to.
    {
        const auto* r = region();
        auto& um = engine.getUndoManager();
        undoButton .setEnabled (um.canUndo());
        redoButton .setEnabled (um.canRedo());
        splitButton     .setEnabled (r != nullptr);
        quantizeButton  .setEnabled (r != nullptr);
        propertiesButton.setEnabled (r != nullptr);
        glueButton      .setEnabled (selectedNotes.size() >= 2);
        if (r != nullptr)
        {
            if (muteToggle.getToggleState() != r->muted)
                muteToggle.setToggleState (r->muted, juce::dontSendNotification);
            if (lockToggle.getToggleState() != r->locked)
                lockToggle.setToggleState (r->locked, juce::dontSendNotification);
        }
    }
    // Recompute the grid rect the same way paint() does so the
    // playhead-x match the painted layout.
    const auto bounds = getLocalBounds();
    const int  topBandH = kToolbarHeight + kHeaderHeight;
    const int  ccBottom = bounds.getBottom() - kStatusBarH;
    const auto gridArea = juce::Rectangle<int> (
        kKeyboardWidth, topBandH,
        bounds.getWidth() - kKeyboardWidth,
        bounds.getHeight() - topBandH - velocityStripH - ccStripH - kStatusBarH);
    juce::ignoreUnused (ccBottom);

    // Chase: when on + transport playing, scroll the grid so the playhead
    // stays in view. Computes the playhead's absolute pixel position
    // (region tick × pixelsPerTick) and re-anchors scrollX so the
    // playhead lands in the left quarter of the grid after the jump.
    if (chaseToggle.getToggleState() && engine.getTransport().isPlaying())
    {
        const auto* r = region();
        if (r != nullptr && r->lengthInTicks > 0 && pixelsPerTick > 0.0)
        {
            const auto playheadSample = engine.getTransport().getPlayhead();
            const auto localSample = playheadSample - r->timelineStart;
            if (localSample >= 0 && localSample <= r->lengthInSamples)
            {
                const double sr  = juce::jmax (1.0, engine.getCurrentSampleRate());
                const float  bpm = juce::jmax (1.0f, session.tempoBpm.load (std::memory_order_relaxed));
                const auto localTick = samplesToTicks (localSample, sr, bpm);
                const double playheadPx = (double) localTick * pixelsPerTick;
                const int gridW = juce::jmax (1, getWidth() - kKeyboardWidth);
                const int xRel  = (int) std::round (playheadPx) - scrollX;
                if (xRel < 0 || xRel > gridW)
                {
                    const double regionPx = (double) r->lengthInTicks * pixelsPerTick;
                    const double maxScroll = juce::jmax (0.0, regionPx - (double) gridW);
                    const int target = (int) juce::jlimit (0.0, maxScroll,
                                          playheadPx - (double) gridW * 0.25);
                    scrollX = target;
                    repaint (gridArea);
                }
            }
        }
    }

    const int x = transportPlayheadX (gridArea);
    if (x == lastPlayheadX) return;
    auto invalidateAt = [this, &gridArea] (int xx)
    {
        if (xx < 0) return;
        repaint (juce::Rectangle<int> (xx - 2, gridArea.getY(),
                                          5, gridArea.getHeight()));
    };
    invalidateAt (lastPlayheadX);
    invalidateAt (x);
    lastPlayheadX = x;
}
} // namespace duskstudio
