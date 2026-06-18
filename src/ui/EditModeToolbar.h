#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../engine/AudioEngine.h"

namespace duskstudio
{
// Ardour-style edit-mode palette. Sits above the TapeStrip in
// MainComponent and lets the user pick a mouse-tool mode: Grab (default
// move/select), Range (time selection), Cut (split at click), Grid
// (tempo-map editing — Phase 3c), Draw (MIDI notes / region gain
// envelope — Phase 3d). Stretch is deliberately absent (Dusk Studio spec
// forbids time-stretching).
//
// State is mirrored into session.editMode so it persists across reloads.
// Mouse handlers in TapeStrip + AudioRegionEditor dispatch on the
// session value, not on the toolbar's local state, so the toolbar is
// the single source of truth visually but session.editMode is the
// source of truth behaviorally.
class EditModeToolbar final : public juce::Component
{
public:
    explicit EditModeToolbar (AudioEngine& engineRef);

    void paint (juce::Graphics&) override;
    void resized() override;

    // Mode buttons trigger this whenever the user picks a new mode.
    // MainComponent listens so it can repaint dependent surfaces (the
    // cursor changes, the toolbar's selected-state border, etc.).
    std::function<void()> onEditModeChanged;

    // Snap toggle / denomination dropdown trigger this. Phase 3a stubs
    // the dropdown (locks at "1 beat"); Phase 3b expands the menu.
    std::function<void()> onSnapChanged;

    // Pull session.editMode into the visual state. Called by
    // MainComponent after session load / undo.
    void syncFromSession();

    // Point the snap on/off toggle at a specific enable flag. Defaults to
    // session.snapToGrid (the timeline / tape strip). The audio and MIDI
    // editors override it with their own per-editor enable so the three
    // surfaces snap independently.
    void setSnapEnabledAccessor (std::function<bool()> get,
                                  std::function<void (bool)> set);

    // Hide modes that aren't meaningful in the host editor. Default
    // shows all five; PianoRoll calls with { Grab, Draw } so the
    // pencil is the obvious note-drawing tool and Range / Cut / Grid
    // (no-op on the note grid) don't clutter the toolbar.
    void setVisibleModes (juce::Array<EditMode> modes);

    // Pure formatter for a grid resolution ("1/4 Note", "Bar", …). Static +
    // public so other surfaces (the header snap picker) can reuse it.
    static juce::String labelFor (SnapResolution r);

private:
    // Compact rectangular icon button that paints a glyph for an
    // EditMode. Active state shown via a lit-up rim, mirroring
    // TransportIconButton's look but using a rounded-rect body so the
    // palette reads as a flat strip rather than a row of discs.
    class ModeButton final : public juce::Button
    {
    public:
        ModeButton (const juce::String& name, EditMode mode);
        void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;
        void setActive (bool a) noexcept { active = a; repaint(); }
        EditMode getMode() const noexcept { return mode; }
    private:
        EditMode mode;
        bool active = false;
    };

    AudioEngine& engine;

    ModeButton grabButton  { "Grab",  EditMode::Grab };
    ModeButton rangeButton { "Range", EditMode::Range };
    ModeButton cutButton   { "Cut",   EditMode::Cut };
    ModeButton gridButton  { "Grid",  EditMode::Grid };
    ModeButton drawButton  { "Draw",  EditMode::Draw };

    // Which enable the Snap toggle reads/writes. Defaults to session.snapToGrid;
    // overridden per editor via setSnapEnabledAccessor.
    std::function<bool()>      snapEnabledGet;
    std::function<void (bool)> snapEnabledSet;

    juce::TextButton snapToggleButton     { "Snap" };
    // Drop-down style button: clicking opens a PopupMenu with musical
    // resolutions + Triplets / Dotted submenus + Timecode / MinSec /
    // CD Frames. ComboBox doesn't support submenus so we use a labeled
    // TextButton and re-paint its label whenever snapResolution changes.
    juce::TextButton snapResolutionButton { {} };

    void setEditMode (EditMode m);
    void updateButtonStates();
    void showSnapResolutionMenu();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditModeToolbar)
};
} // namespace duskstudio
