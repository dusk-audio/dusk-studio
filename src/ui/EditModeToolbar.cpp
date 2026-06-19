#include "EditModeToolbar.h"
#include "DuskContextMenu.h"

namespace duskstudio
{
namespace
{
const juce::Colour kBg          { 0xff181820 };
const juce::Colour kBorder      { 0xff404048 };
const juce::Colour kButtonFill  { 0xff282830 };
const juce::Colour kButtonHover { 0xff363642 };
const juce::Colour kActiveRim   { 0xff80c0ff };
const juce::Colour kGlyph       { 0xffd0d0d8 };
const juce::Colour kGlyphActive { 0xffffffff };

void paintGrabGlyph (juce::Graphics& g, juce::Rectangle<float> r)
{
    // Pointing hand: index finger extended up, the other fingers curled into a
    // fist with a thumb off the left — the classic Ardour Grab-Mode cursor.
    // Drawn as one filled path (no bitmap asset).
    const float cx = r.getCentreX();
    const float cy = r.getCentreY();
    const float s  = juce::jmin (r.getWidth(), r.getHeight());

    juce::Path p;

    // Fist (palm + curled fingers): rounded body in the lower half. The index
    // sits over its LEFT edge, so bias the fist right of centre.
    const float fistW = s * 0.46f;
    const float fistH = s * 0.40f;
    const float fistX = cx - fistW * 0.42f;
    const float fistY = cy - s * 0.04f;
    p.addRoundedRectangle (fistX, fistY, fistW, fistH, s * 0.10f);

    // Index finger: a tall rounded bar rising straight out of the fist's left.
    const float fingerW   = s * 0.15f;
    const float fingerX   = fistX + fistW * 0.06f;
    const float fingerTop = cy - s * 0.42f;
    p.addRoundedRectangle (fingerX, fingerTop,
                              fingerW, (fistY + fistH * 0.5f) - fingerTop,
                              fingerW * 0.5f);

    // Two knuckle bumps on top of the fist (right of the index) so it reads as
    // curled fingers rather than a plain box.
    for (int i = 0; i < 2; ++i)
    {
        const float kx = fingerX + fingerW + s * 0.02f + (float) i * (s * 0.13f);
        p.addRoundedRectangle (kx, fistY - s * 0.045f, s * 0.11f, s * 0.16f, s * 0.05f);
    }

    // Thumb: short bump on the left side of the fist.
    p.addRoundedRectangle (fistX - s * 0.07f, fistY + fistH * 0.22f,
                              s * 0.09f, fistH * 0.5f, s * 0.045f);

    g.fillPath (p);
}

void paintRangeGlyph (juce::Graphics& g, juce::Rectangle<float> r)
{
    // Horizontal double-arrow with end caps - selection range.
    const float cy = r.getCentreY();
    const float x0 = r.getX() + 4.0f;
    const float x1 = r.getRight() - 4.0f;
    juce::Path p;
    // Bar line
    p.startNewSubPath (x0 + 4.0f, cy); p.lineTo (x1 - 4.0f, cy);
    // Left cap
    p.startNewSubPath (x0, cy - 4.0f); p.lineTo (x0, cy + 4.0f);
    p.startNewSubPath (x0 + 4.0f, cy); p.lineTo (x0, cy - 3.0f);
    p.startNewSubPath (x0 + 4.0f, cy); p.lineTo (x0, cy + 3.0f);
    // Right cap
    p.startNewSubPath (x1, cy - 4.0f); p.lineTo (x1, cy + 4.0f);
    p.startNewSubPath (x1 - 4.0f, cy); p.lineTo (x1, cy - 3.0f);
    p.startNewSubPath (x1 - 4.0f, cy); p.lineTo (x1, cy + 3.0f);
    g.strokePath (p, juce::PathStrokeType (1.4f));
}

void paintCutGlyph (juce::Graphics& g, juce::Rectangle<float> r)
{
    // Scissors-ish: two crossing strokes with small loops.
    const float cx = r.getCentreX();
    const float cy = r.getCentreY();
    const float a  = juce::jmin (r.getWidth(), r.getHeight()) * 0.32f;
    juce::Path p;
    p.startNewSubPath (cx - a, cy - a); p.lineTo (cx + a, cy + a);
    p.startNewSubPath (cx + a, cy - a); p.lineTo (cx - a, cy + a);
    g.strokePath (p, juce::PathStrokeType (1.6f));
    // Loop bottoms
    g.drawEllipse (cx - a - 1.5f, cy + a - 1.5f, 3.0f, 3.0f, 1.2f);
    g.drawEllipse (cx + a - 1.5f, cy + a - 1.5f, 3.0f, 3.0f, 1.2f);
}

void paintGridGlyph (juce::Graphics& g, juce::Rectangle<float> r)
{
    const auto inner = r.reduced (4.0f);
    const float step = inner.getWidth() / 3.0f;
    juce::Path p;
    for (int i = 1; i < 3; ++i)
    {
        const float x = inner.getX() + step * (float) i;
        const float y = inner.getY() + step * (float) i;
        p.startNewSubPath (x, inner.getY());     p.lineTo (x, inner.getBottom());
        p.startNewSubPath (inner.getX(), y);     p.lineTo (inner.getRight(), y);
    }
    g.strokePath (p, juce::PathStrokeType (1.0f));
    g.drawRect (inner, 1.0f);
}

void paintDrawGlyph (juce::Graphics& g, juce::Rectangle<float> r)
{
    // Solid pencil matching the Draw-mode cursor (paintPencilGlyph): lead tip
    // lower-left, eraser upper-right. Filled in the button's glyph colour - the
    // cursor is a solid pencil, so a stroked outline here read as a hollow,
    // inverted version of it.
    const auto cell = r.reduced (4.0f);
    auto P = [&] (float gx, float gy)
    {
        return juce::Point<float> (cell.getX() + (gx / 24.0f) * cell.getWidth(),
                                   cell.getY() + (gy / 24.0f) * cell.getHeight());
    };

    constexpr float kInv = 0.70710678f;
    const juce::Point<float> tip { 4.0f, 20.0f };
    auto axis = [&] (float t) { return juce::Point<float> (tip.x + kInv * t, tip.y - kInv * t); };
    auto eA = [&] (float t, float w) { auto c = axis (t); return P (c.x - kInv * w, c.y - kInv * w); };
    auto eB = [&] (float t, float w) { auto c = axis (t); return P (c.x + kInv * w, c.y + kInv * w); };

    constexpr float W = 3.0f, tWood = 5.8f, tEnd = 21.0f;

    // Filled silhouette: tip apex -> wood widen -> straight barrel -> eraser end.
    juce::Path sil;
    sil.startNewSubPath (P (tip.x, tip.y));
    sil.lineTo (eA (tWood, W));
    sil.lineTo (eA (tEnd,  W));
    sil.lineTo (eB (tEnd,  W));
    sil.lineTo (eB (tWood, W));
    sil.closeSubPath();
    g.fillPath (sil);
}
} // namespace

EditModeToolbar::ModeButton::ModeButton (const juce::String& name, EditMode m)
    : juce::Button (name), mode (m)
{
    setClickingTogglesState (false);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    // Don't steal keyboard focus from the host modal on click — otherwise
    // a follow-up Ctrl+Z lands on the button instead of the editor's
    // keyPressed handler.
    setMouseClickGrabsKeyboardFocus (false);
    setWantsKeyboardFocus (false);
}

void EditModeToolbar::ModeButton::paintButton (juce::Graphics& g,
                                                  bool isMouseOver, bool /*isButtonDown*/)
{
    auto r = getLocalBounds().toFloat().reduced (2.0f);
    g.setColour (isMouseOver ? kButtonHover : kButtonFill);
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour (active ? kActiveRim : kBorder);
    g.drawRoundedRectangle (r, 3.0f, active ? 1.6f : 1.0f);

    g.setColour (active ? kGlyphActive : kGlyph);
    switch (mode)
    {
        case EditMode::Grab:  paintGrabGlyph  (g, r); break;
        case EditMode::Range: paintRangeGlyph (g, r); break;
        case EditMode::Cut:   paintCutGlyph   (g, r); break;
        case EditMode::Grid:  paintGridGlyph  (g, r); break;
        case EditMode::Draw:  paintDrawGlyph  (g, r); break;
    }
}

EditModeToolbar::EditModeToolbar (AudioEngine& engineRef) : engine (engineRef)
{
    auto wireButton = [this] (ModeButton& b)
    {
        b.onClick = [this, &b] { setEditMode (b.getMode()); };
        addAndMakeVisible (b);
    };
    wireButton (grabButton);
    wireButton (rangeButton);
    wireButton (cutButton);
    wireButton (gridButton);
    wireButton (drawButton);

    grabButton.setTooltip  ("Grab Mode (select/move objects)\nShortcut: G");
    rangeButton.setTooltip ("Range Mode (select time ranges)\nShortcut: R");
    cutButton.setTooltip   ("Cut Mode (split regions)\nShortcut: C");
    gridButton.setTooltip  ("Grid Mode (edit tempo-map, drag/drop music-time grid)");
    drawButton.setTooltip  ("Draw Mode (draw and edit notes / region gain envelope)");

    // Default the snap toggle to the timeline enable; editors override via
    // setSnapEnabledAccessor so each surface snaps independently.
    snapEnabledGet = [this] { return engine.getSession().snapToGrid; };
    snapEnabledSet = [this] (bool v) { engine.getSession().snapToGrid = v; };

    snapToggleButton.setClickingTogglesState (true);
    snapToggleButton.setToggleState (snapEnabledGet(), juce::dontSendNotification);
    snapToggleButton.setMouseClickGrabsKeyboardFocus (false);
    snapToggleButton.setWantsKeyboardFocus (false);
    // Toggle-on uses the same accent-rim colour as the active edit-mode
    // button so the user gets a clear "snap is on" cue. JUCE default
    // toggle look only differs subtly in shade between states — too
    // easy to miss in a row of similar buttons.
    snapToggleButton.setColour (juce::TextButton::buttonOnColourId,  kActiveRim);
    snapToggleButton.setColour (juce::TextButton::buttonColourId,    kButtonFill);
    snapToggleButton.setColour (juce::TextButton::textColourOnId,    juce::Colours::black);
    snapToggleButton.setColour (juce::TextButton::textColourOffId,   kGlyph);
    snapToggleButton.onClick = [this]
    {
        if (snapEnabledSet) snapEnabledSet (snapToggleButton.getToggleState());
        if (onSnapChanged) onSnapChanged();
    };
    addAndMakeVisible (snapToggleButton);

    snapResolutionButton.setTooltip ("Snap resolution (musical / triplet / dotted / timecode)");
    snapResolutionButton.setMouseClickGrabsKeyboardFocus (false);
    snapResolutionButton.setWantsKeyboardFocus (false);
    snapResolutionButton.onClick = [this] { showSnapResolutionMenu(); };
    addAndMakeVisible (snapResolutionButton);

    syncFromSession();
}

juce::String EditModeToolbar::labelFor (SnapResolution r)
{
    switch (r)
    {
        case SnapResolution::Bar:              return "Bar";
        case SnapResolution::Half:             return "1/2 Note";
        case SnapResolution::Quarter:          return "1/4 Note";
        case SnapResolution::Eighth:           return "1/8 Note";
        case SnapResolution::Sixteenth:        return "1/16 Note";
        case SnapResolution::ThirtySecond:     return "1/32 Note";
        case SnapResolution::SixtyFourth:      return "1/64 Note";
        case SnapResolution::OneTwentyEighth:  return "1/128 Note";
        case SnapResolution::HalfTriplet:      return "1/2 Triplet";
        case SnapResolution::QuarterTriplet:   return "1/4 Triplet";
        case SnapResolution::EighthTriplet:    return "1/8 Triplet";
        case SnapResolution::SixteenthTriplet: return "1/16 Triplet";
        case SnapResolution::ThirtySecondTrip: return "1/32 Triplet";
        case SnapResolution::HalfDotted:       return "1/2 Dotted";
        case SnapResolution::QuarterDotted:    return "1/4 Dotted";
        case SnapResolution::EighthDotted:     return "1/8 Dotted";
        case SnapResolution::SixteenthDotted:  return "1/16 Dotted";
        case SnapResolution::Timecode:         return "Timecode";
        case SnapResolution::MinSec:           return "MinSec";
        case SnapResolution::CDFrames:         return "CD Frames";
    }
    return "1/4 Note";
}

void EditModeToolbar::showSnapResolutionMenu()
{
    const auto current = engine.getSession().snapResolution;
    auto makeItem = [&] (juce::PopupMenu& m, SnapResolution r)
    {
        m.addItem ((int) r + 1, labelFor (r), true, current == r);
    };

    juce::PopupMenu m;
    makeItem (m, SnapResolution::Bar);
    makeItem (m, SnapResolution::Half);
    makeItem (m, SnapResolution::Quarter);
    makeItem (m, SnapResolution::Eighth);
    makeItem (m, SnapResolution::Sixteenth);
    makeItem (m, SnapResolution::ThirtySecond);
    makeItem (m, SnapResolution::SixtyFourth);
    makeItem (m, SnapResolution::OneTwentyEighth);

    juce::PopupMenu triplets;
    makeItem (triplets, SnapResolution::HalfTriplet);
    makeItem (triplets, SnapResolution::QuarterTriplet);
    makeItem (triplets, SnapResolution::EighthTriplet);
    makeItem (triplets, SnapResolution::SixteenthTriplet);
    makeItem (triplets, SnapResolution::ThirtySecondTrip);
    m.addSubMenu ("Triplets", triplets);

    juce::PopupMenu dotted;
    makeItem (dotted, SnapResolution::HalfDotted);
    makeItem (dotted, SnapResolution::QuarterDotted);
    makeItem (dotted, SnapResolution::EighthDotted);
    makeItem (dotted, SnapResolution::SixteenthDotted);
    m.addSubMenu ("Dotted", dotted);

    m.addSeparator();
    makeItem (m, SnapResolution::Timecode);
    makeItem (m, SnapResolution::MinSec);
    makeItem (m, SnapResolution::CDFrames);

    juce::Component::SafePointer<EditModeToolbar> safe (this);
    showContextMenu (m, snapResolutionButton,
        [safe] (int chosen)
        {
            auto* self = safe.getComponent();
            if (self == nullptr || chosen <= 0) return;
            const int v = chosen - 1;
            if (v < 0 || v > (int) SnapResolution::CDFrames) return;
            self->engine.getSession().snapResolution = (SnapResolution) v;
            self->snapResolutionButton.setButtonText (labelFor ((SnapResolution) v));
            if (self->onSnapChanged) self->onSnapChanged();
        });
}

void EditModeToolbar::syncFromSession()
{
    updateButtonStates();
    snapToggleButton.setToggleState (snapEnabledGet ? snapEnabledGet() : false,
                                      juce::dontSendNotification);
    snapResolutionButton.setButtonText (labelFor (engine.getSession().snapResolution));
}

void EditModeToolbar::setSnapEnabledAccessor (std::function<bool()> get,
                                                std::function<void (bool)> set)
{
    snapEnabledGet = std::move (get);
    snapEnabledSet = std::move (set);
    snapToggleButton.setToggleState (snapEnabledGet ? snapEnabledGet() : false,
                                      juce::dontSendNotification);
}

void EditModeToolbar::setVisibleModes (juce::Array<EditMode> modes)
{
    auto setIf = [&modes] (ModeButton& b)
    {
        b.setVisible (modes.contains (b.getMode()));
    };
    setIf (grabButton);
    setIf (rangeButton);
    setIf (cutButton);
    setIf (gridButton);
    setIf (drawButton);

    // If the active mode just became hidden (e.g. an arrange-view Range/Cut
    // mode carried into the piano roll's Grab/Draw-only palette), snap to the
    // first visible mode so the mouse handlers don't keep acting on a mode with
    // no button. setEditMode updates button states + notifies observers.
    if (! modes.isEmpty() && ! modes.contains (engine.getSession().editMode))
        setEditMode (modes.getFirst());

    resized();
}

void EditModeToolbar::setSnapResolutionVisible (bool shouldBeVisible)
{
    snapResolutionButton.setVisible (shouldBeVisible);
    resized();
}

void EditModeToolbar::updateButtonStates()
{
    const auto current = engine.getSession().editMode;
    grabButton.setActive  (current == EditMode::Grab);
    rangeButton.setActive (current == EditMode::Range);
    cutButton.setActive   (current == EditMode::Cut);
    gridButton.setActive  (current == EditMode::Grid);
    drawButton.setActive  (current == EditMode::Draw);
}

void EditModeToolbar::setEditMode (EditMode m)
{
    if (engine.getSession().editMode == m) return;
    engine.getSession().editMode = m;
    updateButtonStates();
    if (onEditModeChanged) onEditModeChanged();
}

void EditModeToolbar::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
    g.setColour (kBorder);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
}

void EditModeToolbar::resized()
{
    auto r = getLocalBounds().reduced (4, 6);
    constexpr int kBtn = 36;
    constexpr int kGap = 3;
    // Skip hidden buttons so PianoRoll's two-mode palette doesn't leave
    // gaps where Range / Cut / Grid would be.
    for (auto* b : { &grabButton, &rangeButton, &cutButton, &gridButton, &drawButton })
    {
        if (! b->isVisible()) continue;
        b->setBounds (r.removeFromLeft (kBtn));
        r.removeFromLeft (kGap);
    }
    r.removeFromLeft (12);  // group separator
    snapToggleButton.setBounds (r.removeFromLeft (56));
    if (snapResolutionButton.isVisible())
    {
        r.removeFromLeft (4);
        snapResolutionButton.setBounds (r.removeFromLeft (110));
    }
}
} // namespace duskstudio
