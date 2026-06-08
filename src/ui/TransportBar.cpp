#include "TransportBar.h"
#include "DuskAlerts.h"
#include "DuskContextMenu.h"
#include <algorithm>

namespace duskstudio
{
TransportIconButton::TransportIconButton (const juce::String& name, Icon icon,
                                            juce::Colour active)
    : juce::Button (name), iconType (icon), activeColour (active)
{
    setClickingTogglesState (false);
}

void TransportIconButton::paintButton (juce::Graphics& g, bool isMouseOver,
                                         bool isButtonDown)
{
    const bool active = getToggleState();
    const auto bounds = getLocalBounds().toFloat().reduced (2.0f);

    // Disc body - a darker shade of the active colour when on, near-black
    // otherwise. The rim is the saturated active colour so it reads as a
    // ring around the icon.
    const auto bg  = active ? activeColour.darker (0.55f) : juce::Colour (0xff262630);
    const auto rim = active ? activeColour                : juce::Colour (0xff3a3a44);

    auto disc = bg;
    if (isButtonDown) disc = disc.brighter (0.10f);
    else if (isMouseOver) disc = disc.brighter (0.06f);

    g.setColour (disc);
    g.fillEllipse (bounds);
    g.setColour (rim);
    g.drawEllipse (bounds, isButtonDown ? 2.4f : 1.6f);

    // Icon glyph - fits inside an inner square at ~46 % of the disc.
    const float iconExtent = bounds.getWidth() * 0.46f;
    auto iconBox = juce::Rectangle<float> (iconExtent, iconExtent)
                       .withCentre (bounds.getCentre());
    const auto iconColour = active ? juce::Colours::white
                                   : juce::Colour (0xffd0d0d0);
    g.setColour (iconColour);

    switch (iconType)
    {
        case Icon::Stop:
        {
            // Slightly inset so the square doesn't touch the disc rim.
            g.fillRoundedRectangle (iconBox.reduced (1.0f), 1.5f);
            break;
        }
        case Icon::Play:
        {
            // Right-pointing triangle, optically nudged so its visual centre
            // matches the geometric centre of the disc (triangles look
            // off-centre when their bounding box is centred).
            auto p = iconBox.translated (iconBox.getWidth() * 0.10f, 0.0f);
            juce::Path tri;
            tri.addTriangle (p.getX(),       p.getY(),
                              p.getX(),       p.getBottom(),
                              p.getRight(),   p.getCentreY());
            g.fillPath (tri);
            break;
        }
        case Icon::Record:
        {
            g.fillEllipse (iconBox);
            break;
        }
        case Icon::Rewind:
        {
            // Two left-pointing triangles, classic deck glyph. Nudged left
            // so the optical centre matches the disc centre (triangles).
            auto p = iconBox.translated (-iconBox.getWidth() * 0.05f, 0.0f);
            const float halfW = p.getWidth() * 0.5f;
            juce::Path g1, g2;
            g1.addTriangle (p.getX(),           p.getCentreY(),
                              p.getX() + halfW,    p.getY(),
                              p.getX() + halfW,    p.getBottom());
            g2.addTriangle (p.getX() + halfW,    p.getCentreY(),
                              p.getRight(),        p.getY(),
                              p.getRight(),        p.getBottom());
            g.fillPath (g1);
            g.fillPath (g2);
            break;
        }
        case Icon::Forward:
        {
            auto p = iconBox.translated (iconBox.getWidth() * 0.05f, 0.0f);
            const float halfW = p.getWidth() * 0.5f;
            juce::Path g1, g2;
            g1.addTriangle (p.getX(),           p.getY(),
                              p.getX(),           p.getBottom(),
                              p.getX() + halfW,    p.getCentreY());
            g2.addTriangle (p.getX() + halfW,    p.getY(),
                              p.getX() + halfW,    p.getBottom(),
                              p.getRight(),        p.getCentreY());
            g.fillPath (g1);
            g.fillPath (g2);
            break;
        }
        case Icon::Loop:
        {
            // Open circular arrow - 290° arc with a small triangular
            // arrowhead at the gap. Reads as "looped playback".
            auto box = iconBox.expanded (iconBox.getWidth() * 0.06f);
            const float cx = box.getCentreX();
            const float cy = box.getCentreY();
            const float r  = box.getWidth() * 0.5f;
            const float startAngle = juce::MathConstants<float>::pi * 0.15f;
            const float endAngle   = juce::MathConstants<float>::twoPi
                                    - juce::MathConstants<float>::pi * 0.15f;

            juce::Path arc;
            arc.addCentredArc (cx, cy, r, r, 0.0f, startAngle, endAngle, true);
            g.strokePath (arc, juce::PathStrokeType (1.8f,
                                                      juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));

            // Arrowhead at the start of the arc, pointing tangentially.
            const float ax = cx + r * std::sin (startAngle);
            const float ay = cy - r * std::cos (startAngle);
            const float ah = r * 0.55f;
            juce::Path tip;
            tip.addTriangle (ax,        ay - ah * 0.55f,
                              ax,        ay + ah * 0.55f,
                              ax - ah,   ay);
            g.fillPath (tip);
            break;
        }
        case Icon::Punch:
        {
            // Concentric ring + filled inner disc — "bullseye". Reads as
            // a punch-in target.
            const float ringW = iconBox.getWidth() * 0.15f;
            g.drawEllipse (iconBox, ringW);
            const float innerScale = 0.40f;
            auto inner = juce::Rectangle<float> (iconBox.getWidth() * innerScale,
                                                  iconBox.getHeight() * innerScale)
                              .withCentre (iconBox.getCentre());
            g.fillEllipse (inner);
            break;
        }
        case Icon::Keyboard:
        {
            // Simplified piano-keyboard glyph: outer rounded rectangle with
            // three black-key marks across the top half. White-key
            // separators are implied by the gaps between the black keys.
            const float corner = iconBox.getWidth() * 0.12f;
            g.drawRoundedRectangle (iconBox.reduced (0.5f), corner, 1.4f);

            const float keyW   = iconBox.getWidth() * 0.16f;
            const float keyH   = iconBox.getHeight() * 0.55f;
            const float keyTop = iconBox.getY() + iconBox.getHeight() * 0.10f;
            const float baseX  = iconBox.getX() + iconBox.getWidth() * 0.18f;
            const float stride = iconBox.getWidth() * 0.27f;
            for (int i = 0; i < 3; ++i)
                g.fillRect (juce::Rectangle<float> (baseX + i * stride, keyTop,
                                                     keyW, keyH));
            break;
        }
        case Icon::Bars:
        {
            // Bars/beats glyph: four ascending vertical bars representing a
            // bar count graphic — quick read as "musical time".
            const float w = iconBox.getWidth();
            const float h = iconBox.getHeight();
            const float bw = w * 0.16f;
            const float baseY = iconBox.getBottom();
            const float left = iconBox.getX() + w * 0.06f;
            const float stride = w * 0.22f;
            const float hs[4] = { h * 0.45f, h * 0.65f, h * 0.85f, h * 1.00f };
            for (int i = 0; i < 4; ++i)
                g.fillRect (juce::Rectangle<float> (left + i * stride,
                                                      baseY - hs[i],
                                                      bw, hs[i]));
            break;
        }
        case Icon::TimeClock:
        {
            // Clock face: circle outline + two hands (12 → up, 3 → right).
            g.drawEllipse (iconBox.reduced (0.5f), 1.4f);
            const auto c = iconBox.getCentre();
            const float r1 = iconBox.getWidth() * 0.34f;   // hour hand length
            const float r2 = iconBox.getWidth() * 0.42f;   // minute hand length
            g.drawLine (c.x, c.y, c.x, c.y - r1, 1.4f);    // 12 o'clock
            g.drawLine (c.x, c.y, c.x + r2, c.y, 1.4f);    // 3 o'clock
            g.fillEllipse (c.x - 1.2f, c.y - 1.2f, 2.4f, 2.4f);
            break;
        }
        case Icon::Tuner:
        {
            // Tuning fork: two vertical prongs joined by a U-bend,
            // a stem dropping below the bend, and a small bulb at the
            // bottom of the stem (concert-fork silhouette). Built from
            // a single stroked path for clarity at small sizes.
            const float w  = iconBox.getWidth();
            const float h  = iconBox.getHeight();
            const float cx = iconBox.getCentreX();
            const float topY    = iconBox.getY() + h * 0.05f;
            const float bendY   = iconBox.getY() + h * 0.55f;
            const float stemBot = iconBox.getBottom() - h * 0.10f;
            const float prongDx = w * 0.20f;
            const float bendR   = w * 0.18f;

            juce::Path fork;
            // Left prong, top down to the start of the U-bend.
            fork.startNewSubPath (cx - prongDx, topY);
            fork.lineTo          (cx - prongDx, bendY);
            // U-bend from left prong to right prong.
            fork.cubicTo (cx - prongDx, bendY + bendR,
                            cx + prongDx, bendY + bendR,
                            cx + prongDx, bendY);
            // Right prong, bottom up to the top.
            fork.lineTo (cx + prongDx, topY);

            // Stem: line from the bottom of the U-bend down to the bulb.
            juce::Path stem;
            stem.startNewSubPath (cx, bendY + bendR);
            stem.lineTo          (cx, stemBot - 1.5f);

            g.strokePath (fork, juce::PathStrokeType (1.6f,
                                                       juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
            g.strokePath (stem, juce::PathStrokeType (1.6f,
                                                       juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
            // Bulb at the stem's tip.
            const float bulbR = w * 0.07f;
            g.fillEllipse (cx - bulbR, stemBot - bulbR * 2.0f,
                             bulbR * 2.0f, bulbR * 2.0f);
            break;
        }
        case Icon::Metronome:
        {
            // Metronome silhouette: trapezoid body + a swinging arm
            // crossed up-right with a small pendulum bob at the tip.
            const float w = iconBox.getWidth();
            const float h = iconBox.getHeight();
            const float cx = iconBox.getCentreX();
            const float topY = iconBox.getY() + h * 0.10f;
            const float botY = iconBox.getBottom();
            juce::Path body;
            body.startNewSubPath (cx - w * 0.12f, topY);
            body.lineTo          (cx + w * 0.12f, topY);
            body.lineTo          (cx + w * 0.42f, botY);
            body.lineTo          (cx - w * 0.42f, botY);
            body.closeSubPath();
            g.strokePath (body, juce::PathStrokeType (1.4f));
            // Swinging arm + bob.
            const float armBaseX = cx;
            const float armBaseY = botY - h * 0.18f;
            const float armTipX  = cx + w * 0.22f;
            const float armTipY  = topY + h * 0.05f;
            g.drawLine (armBaseX, armBaseY, armTipX, armTipY, 1.4f);
            g.fillEllipse (armTipX - 2.0f, armTipY - 2.0f, 4.0f, 4.0f);
            // Base line.
            g.drawLine (iconBox.getX() + w * 0.10f, botY,
                          iconBox.getRight() - w * 0.10f, botY, 1.4f);
            break;
        }
    }
}

void TransportIconButton::buttonStateChanged()
{
    juce::Button::buttonStateChanged();
    heldDown = isDown();
}

TransportBar::TransportBar (AudioEngine& engineRef) : engine (engineRef)
{
    // Accessibility floor — screen readers announce buttons by name
    // (icon-only buttons would otherwise speak as "button"). Tooltip
    // text supplied below covers the description.
    setTitle ("Transport bar");
    playButton  .setTitle ("Play");
    stopButton  .setTitle ("Stop");
    recordButton.setTitle ("Record");
    rewButton   .setTitle ("Rewind");
    ffwdButton  .setTitle ("Fast forward");

    playButton.onClick   = [this] { engine.play();   refreshButtonStates(); };
    stopButton.onClick   = [this] { engine.stop();   notifyRecordStopped(); refreshButtonStates(); };
    recordButton.onClick = [this]
    {
        engine.record();
        surfaceRecordSetupFailures();
        refreshButtonStates();
    };

    playButton  .setTooltip ("Play (Space). Right-click for MIDI Learn.");
    stopButton  .setTooltip ("Stop. Right-click for MIDI Learn.");
    recordButton.setTooltip ("Record - arm a track first. Right-click for MIDI Learn.");

    // Right-click on the transport buttons opens the MIDI Learn menu.
    // We listen on each button so the routed mouseDown reaches us with
    // the source component as eventComponent.
    playButton  .addMouseListener (this, false);
    stopButton  .addMouseListener (this, false);
    recordButton.addMouseListener (this, false);

    // REW / FFWD - dual-action.
    //   Brief press (< kHoldThresholdMs):
    //     Stopped -> REW = goto zero, FFWD = goto last record point.
    //     Rolling -> REW = jump to prev marker, FFWD = jump to next marker.
    //   Held: 10x scrub via the timer (drives the playhead while button down).
    rewButton.setTooltip  ("Rewind: hold = 10x scrub. Tap = prev marker (rolling) "
                            "or jump to zero (stopped).");
    ffwdButton.setTooltip ("Forward: hold = 10x scrub. Tap = next marker (rolling) "
                            "or jump to last record point (stopped).");

    rewButton.onStateChange = [this]
    {
        if (rewButton.isHeldDown())
        {
            if (rewPressedAtMs == 0)   // edge: just pressed
                rewPressedAtMs = juce::Time::currentTimeMillis();
        }
        else if (rewPressedAtMs != 0)
        {
            const auto held = juce::Time::currentTimeMillis() - rewPressedAtMs;
            rewPressedAtMs = 0;
            if (rewIsScrubbing) { rewIsScrubbing = false; }
            else if (held < kHoldThresholdMs)
            {
                if (engine.getTransport().isStopped()) engine.jumpToZero();
                else                                    engine.jumpToPrevMarker();
            }
        }
    };

    ffwdButton.onStateChange = [this]
    {
        if (ffwdButton.isHeldDown())
        {
            if (ffwdPressedAtMs == 0)
                ffwdPressedAtMs = juce::Time::currentTimeMillis();
        }
        else if (ffwdPressedAtMs != 0)
        {
            const auto held = juce::Time::currentTimeMillis() - ffwdPressedAtMs;
            ffwdPressedAtMs = 0;
            if (ffwdIsScrubbing) { ffwdIsScrubbing = false; }
            else if (held < kHoldThresholdMs)
            {
                if (engine.getTransport().isStopped()) engine.jumpToLastRecordPoint();
                else                                    engine.jumpToNextMarker();
            }
        }
    };

    addAndMakeVisible (stopButton);
    addAndMakeVisible (rewButton);
    addAndMakeVisible (playButton);
    addAndMakeVisible (ffwdButton);
    addAndMakeVisible (recordButton);

    clockLabel.setJustificationType (juce::Justification::centred);
    clockLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe0e0e0));
    clockLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xff121214));
    clockLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                        18.0f, juce::Font::bold)));
    clockLabel.setText ("00:00.000", juce::dontSendNotification);
    // Direct numeric location entry. Double-click → text editor; Enter
    // commits via the parser; Esc reverts. Three formats accepted:
    //   1:23 / 1:23.456    minutes : seconds[.ms]
    //   12.3.4 / 12|3|4    bar . beat . subdivision (1-indexed bar/beat,
    //                       subdivision is sixteenths within the beat).
    //   7.5 / 90           bare number = seconds (or ":secondsonly")
    // Bad input reverts the label to the running-time format on the next
    // timer tick, no error dialog (cheap + stays out of the way).
    clockLabel.setEditable (false, true, false);
    clockLabel.setColour (juce::Label::backgroundWhenEditingColourId, juce::Colour (0xff202028));
    clockLabel.setColour (juce::Label::textWhenEditingColourId,        juce::Colours::white);
    clockLabel.setTooltip ("Playhead position. Double-click to type a time "
                            "(e.g. 1:23, 1:23.456, 12.3.4 for bar.beat.sub, "
                            "or a plain number of seconds).");
    clockLabel.onTextChange = [this]
    {
        const auto text = clockLabel.getText().trim();
        const double sr = engine.getCurrentSampleRate();
        if (text.isEmpty() || sr <= 0.0) return;

        // Bar.beat.sub: any '.' or '|' separator and no ':'. Sub is
        // 1-indexed sixteenths within the beat (1..4 in 4/4) for parity
        // with the snap denominations the piano roll uses.
        if (! text.containsChar (':')
            && (text.containsChar ('.') || text.containsChar ('|')))
        {
            auto parts = juce::StringArray::fromTokens (text, ".|", "");
            parts.removeEmptyStrings();
            if (parts.size() >= 1 && parts.size() <= 3)
            {
                const int bar  = juce::jmax (1, parts[0].getIntValue());
                const int beat = parts.size() >= 2 ? juce::jmax (1, parts[1].getIntValue()) : 1;
                const int sub  = parts.size() >= 3 ? juce::jmax (1, parts[2].getIntValue()) : 1;
                const float bpm = engine.getSession().tempoBpm.load (std::memory_order_relaxed);
                const int beatsPerBar = juce::jmax (1, engine.getSession().beatsPerBar.load (std::memory_order_relaxed));
                if (bpm > 0.0f)
                {
                    const double secondsPerBeat = 60.0 / (double) bpm;
                    const double secondsPerSub  = secondsPerBeat / 4.0;   // 16th
                    const double total = (bar - 1) * beatsPerBar * secondsPerBeat
                                       + (beat - 1) * secondsPerBeat
                                       + (sub  - 1) * secondsPerSub;
                    engine.getTransport().setPlayhead (
                        (juce::int64) std::round (total * sr));
                    return;
                }
            }
        }

        // mm:ss[.ms] or :ss[.ms]
        if (text.containsChar (':'))
        {
            const int colon = text.indexOfChar (':');
            const auto minStr = text.substring (0, colon);
            const auto secStr = text.substring (colon + 1);
            const double mins = minStr.isEmpty() ? 0.0 : (double) minStr.getDoubleValue();
            const double secs = (double) secStr.getDoubleValue();
            const auto target = (juce::int64) std::round ((mins * 60.0 + secs) * sr);
            engine.getTransport().setPlayhead (juce::jmax ((juce::int64) 0, target));
            return;
        }

        // Bare number = seconds.
        const double secs = (double) text.getDoubleValue();
        const auto target = (juce::int64) std::round (secs * sr);
        engine.getTransport().setPlayhead (juce::jmax ((juce::int64) 0, target));
    };
    addAndMakeVisible (clockLabel);

    // Current-section pill: muted-amber accent, sits just right of the clock.
    // Starts hidden; the timer shows it once a marker is at/before the playhead.
    sectionLabel.setJustificationType (juce::Justification::centredLeft);
    sectionLabel.setColour (juce::Label::textColourId,       juce::Colour (0xffd0a050));
    sectionLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xff121214));
    sectionLabel.setColour (juce::Label::outlineColourId,    juce::Colour (0xff2a2a32));
    sectionLabel.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    sectionLabel.setMinimumHorizontalScale (1.0f);
    sectionLabel.setTooltip ("Current song section — the most recent marker at "
                              "or before the playhead. Add markers with M.");
    sectionLabel.setVisible (false);
    addAndMakeVisible (sectionLabel);

    timeFormatToggle.setTooltip ("Flip display between Bars/Beats and mm:ss.ms. "
                                   "Affects the clock, tape ruler, and editor rulers. "
                                   "Right-click the clock to flip when this button is hidden.");
    auto flipTimeMode = [this]
    {
        auto& mode = engine.getSession().timeDisplayMode;
        const int cur = mode.load (std::memory_order_relaxed);
        mode.store (cur == (int) TimeDisplayMode::Bars ? (int) TimeDisplayMode::Time
                                                          : (int) TimeDisplayMode::Bars,
                    std::memory_order_relaxed);
        if (auto* p = getParentComponent()) p->repaint();
        repaint();
    };
    timeFormatToggle.onClick = flipTimeMode;
    // Always visible after the bank-row refactor — banks now live inline
    // beside the centered stage block, not overlaid on the transport bar's
    // x-band, so the time-format toggle no longer collides with them at
    // any width. Right-click on the clock label remains a redundant
    // fallback for changing time-display mode.
    addAndMakeVisible (timeFormatToggle);

    // Right-click clock label to flip time-display mode. Works in
    // every layout including compact-mode where the dedicated button
    // is hidden. The clock's own onTextChange (number-entry) still
    // owns left-clicks via setEditable(false, true, false).
    clockLabel.addMouseListener (this, false);
    flipTimeModeOnClock = std::move (flipTimeMode);

    hintLabel.setJustificationType (juce::Justification::centredLeft);
    hintLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8090a0));
    hintLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    addAndMakeVisible (hintLabel);

    auto styleModeToggle = [] (juce::TextButton& b, juce::Colour onColour)
    {
        b.setClickingTogglesState (true);
        b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
        b.setColour (juce::TextButton::buttonOnColourId, onColour.darker (0.45f));
        b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff909094));
        b.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    };

    // clickToggle is now a TransportIconButton — paints from its ctor
    // colour, no TextButton styling needed.
    styleModeToggle (countInToggle, juce::Colour (0xff60c060));   // green: same family as CLICK

    // Loop / Punch / Keyboard icon buttons in the transport cluster. Loop +
    // Punch are toggle buttons (state mirrors transport flags); Keyboard
    // toggles the embedded VKB modal owned by MainComponent.
    loopButton.setClickingTogglesState (true);
    loopButton.setTooltip ("Loop the timeline between IN and OUT during playback (L). "
                            "Right-click the ruler in the TIMELINE view to set the points.");
    loopButton.setToggleState (engine.getTransport().isLoopEnabled(), juce::dontSendNotification);
    loopButton.onClick = [this]
    {
        engine.getTransport().setLoopEnabled (loopButton.getToggleState());
    };
    addAndMakeVisible (loopButton);

    punchButton.setClickingTogglesState (true);
    punchButton.setTooltip ("Only commit recorded audio between PUNCH IN and PUNCH OUT (P). "
                             "Right-click for pre/post-roll settings.");
    punchButton.setToggleState (engine.getTransport().isPunchEnabled(), juce::dontSendNotification);
    punchButton.onClick = [this]
    {
        engine.getTransport().setPunchEnabled (punchButton.getToggleState());
    };
    punchButton.addMouseListener (this, false);  // right-click → settings menu
    addAndMakeVisible (punchButton);

    keyboardButton.setTooltip ("Virtual MIDI Keyboard (K) - type on your keyboard to play notes.");
    keyboardButton.onClick = [this] { if (onVirtualKeyboardToggle) onVirtualKeyboardToggle(); };
    addAndMakeVisible (keyboardButton);

    // Metronome toggle + BPM editable display.
    clickToggle.setClickingTogglesState (true);
    clickToggle.setTooltip ("Left-click: toggle metronome on/off. Right-click: choose when the click fires (recording / count-in / playing / polyphonic).");
    clickToggle.setToggleState (engine.getSession().metronomeEnabled.load(),
                                  juce::dontSendNotification);
    clickToggle.onClick = [this]
    {
        engine.getSession().metronomeEnabled.store (clickToggle.getToggleState());
    };
    // Right-click → behaviour menu (Click while recording / Only during
    // count-in / Click while playing / Polyphonic clicks). Routed via
    // addMouseListener so TransportBar::mouseDown sees popup events on
    // clickToggle without subclassing the button.
    clickToggle.addMouseListener (this, false);
    addAndMakeVisible (clickToggle);

    countInToggle.setTooltip ("Count-in: when on, hitting Record rolls the playhead "
                               "back one bar so the metronome ticks a full bar before "
                               "the take begins. The pre-roll audio is NOT recorded.");
    countInToggle.setToggleState (engine.getSession().countInEnabled.load(),
                                    juce::dontSendNotification);
    countInToggle.onClick = [this]
    {
        engine.getSession().countInEnabled.store (countInToggle.getToggleState());
    };
    addAndMakeVisible (countInToggle);

    bpmCaption.setText ("BPM", juce::dontSendNotification);
    bpmCaption.setJustificationType (juce::Justification::centredRight);
    bpmCaption.setColour (juce::Label::textColourId, juce::Colour (0xff707074));
    bpmCaption.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
    addAndMakeVisible (bpmCaption);

    bpmValue.setJustificationType (juce::Justification::centred);
    bpmValue.setColour (juce::Label::textColourId,        juce::Colour (0xffe0e0e0));
    bpmValue.setColour (juce::Label::backgroundColourId,  juce::Colour (0xff121214));
    bpmValue.setColour (juce::Label::outlineColourId,     juce::Colour (0xff2a2a32));
    bpmValue.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                      14.0f, juce::Font::bold)));
    bpmValue.setText (juce::String ((int) engine.getSession().tempoBpm.load()),
                       juce::dontSendNotification);
    // Read-only: it shows the tempo at the playhead (following the tempo map).
    // All tempo editing lives in the tape strip's Grid mode — the bar-1 point
    // is the starting tempo. (Tap, below, still sets the starting tempo.)
    bpmValue.setEditable (false, false);
    bpmValue.setTooltip ("Tempo at the playhead (beats per minute). Edit the "
                          "tempo in the tape strip's Grid mode; tap below to set "
                          "the starting tempo.");
    addAndMakeVisible (bpmValue);

    tapButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    tapButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffe0c050));
    tapButton.setTooltip ("Tap to set tempo. Click in time with the music; "
                          "BPM updates after the second tap and averages "
                          "across the most recent few. Two-second silence "
                          "resets the pulse.");
    tapButton.onClick = [this] { onTap(); };
    addAndMakeVisible (tapButton);

    timeSigButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff202024));
    timeSigButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffd0d0d0));
    timeSigButton.setTooltip ("Time signature. Click to pick a common time "
                              "(3/4, 4/4, 5/4, 6/8, 7/8, 12/8) or open Custom...");
    timeSigButton.onClick = [this] { showTimeSigMenu(); };
    addAndMakeVisible (timeSigButton);
    refreshTimeSigButton();

    // TUNE button - opens a modal pitch-tracker overlay on click.
    // Selection of which track to tune lives on MainComponent (it knows
    // the focused track from the TapeStrip selection); this button just
    // toggles the overlay.
    tuneButton.setTooltip ("Tuner - shows the pitch of the selected track's input. "
                            "Click a track first, then press TUNE.");
    tuneButton.onClick = [this] { if (onTunerToggle) onTunerToggle(); };
    addAndMakeVisible (tuneButton);

    tapeToggle.setClickingTogglesState (true);
    tapeToggle.setToggleState (true, juce::dontSendNotification);   // expanded by default
    // Initial text - syncCompactLabels() in resized() rewrites this whenever
    // the transport-bar width crosses the compact breakpoint.
    tapeToggle.setButtonText (juce::CharPointer_UTF8 ("\xe2\x96\xbe TIMELINE")); // "▾ TIMELINE"
    tapeToggle.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    tapeToggle.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2a3a48));
    tapeToggle.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff7090a8));
    tapeToggle.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xffd0e0f0));
    tapeToggle.setTooltip ("Show / hide the TIMELINE arrangement view");
    tapeToggle.onClick = [this]
    {
        // Re-render the label in whatever form (full / compact) the bar
        // is currently sized to. The chevron always reflects the new
        // toggle state.
        syncCompactLabels (getWidth() < kCompactTransportWidth);
        if (onTapeStripToggle) onTapeStripToggle (tapeToggle.getToggleState());
    };
    addAndMakeVisible (tapeToggle);

    refreshButtonStates();
    startTimerHz (20);
}

void TransportBar::setTapeStripExpanded (bool expanded)
{
    // Single source of truth: setTapeToggleVisualState sets the state + runs
    // syncCompactLabels, which picks the right chevron/label for the current
    // compact mode (this used to hard-set the full label, drifting from the
    // compact path).
    setTapeToggleVisualState (expanded);
}

bool TransportBar::isTapeStripExpanded() const
{
    return tapeToggle.getToggleState();
}

TransportBar::~TransportBar() = default;

void TransportBar::timerCallback()
{
    // 10x scrub. Once a REW / FFWD button has been held past
    // kHoldThresholdMs, advance the playhead by (sr * kScrubMultiplier *
    // tickPeriod) samples per tick. Continues until the button releases.
    // Direct setPlayhead - scrub does NOT engage the transport so audio
    // stays silent and the audio thread sees a "playhead jumped" event
    // (which fires the existing All Notes Off MIDI flush; safe).
    const auto nowMs = juce::Time::currentTimeMillis();
    const double sr  = engine.getCurrentSampleRate();
    if (sr > 0.0)
    {
        const auto handleScrub = [&] (TransportIconButton& btn,
                                        juce::int64& pressedAt,
                                        bool& scrubbing,
                                        int direction)
        {
            if (! btn.isHeldDown() || pressedAt == 0) return;
            const auto held = nowMs - pressedAt;
            if (held < kHoldThresholdMs) return;
            scrubbing = true;
            // First scrub tick: seed lastScrubTickMs so the delta is the
            // tick period, not "ms since press" (which would teleport on
            // the threshold crossing).
            if (lastScrubTickMs == 0 || lastScrubTickMs < pressedAt)
                lastScrubTickMs = nowMs;
            const auto dtMs = nowMs - lastScrubTickMs;
            lastScrubTickMs = nowMs;
            const auto delta = (juce::int64) ((double) dtMs * 0.001 * sr * kScrubMultiplier);
            const auto cur = engine.getTransport().getPlayhead();
            engine.getTransport().setPlayhead (juce::jmax ((juce::int64) 0,
                cur + (juce::int64) direction * delta));
        };
        handleScrub (rewButton,  rewPressedAtMs,  rewIsScrubbing,  -1);
        handleScrub (ffwdButton, ffwdPressedAtMs, ffwdIsScrubbing, +1);
        if (! rewIsScrubbing && ! ffwdIsScrubbing) lastScrubTickMs = 0;
    }

    const auto playhead = engine.getTransport().getPlayhead();

    // BPM field tracks the tempo at the playhead (refreshButtonStates() below
    // sets it; it reads the playhead too so the two never disagree).

    // Current-section pill: name of the latest marker at or before the
    // playhead. Empty (hidden) when there are no markers or the playhead
    // sits before the first one. Re-lays out + re-clamps the stage tabs only
    // when the text actually changes (rare — marker-boundary crossings).
    {
        const auto& markers = engine.getSession().getMarkers();
        const Marker* cur = nullptr;
        for (const auto& mk : markers)
            if (mk.timelineSamples <= playhead
                && (cur == nullptr || mk.timelineSamples > cur->timelineSamples))
                cur = &mk;
        const juce::String text = cur != nullptr
            ? juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb8 ")) + cur->name   // "▸ "
            : juce::String();
        if (text != sectionLabel.getText())
        {
            sectionLabel.setText (text, juce::dontSendNotification);
            sectionLabel.setVisible (text.isNotEmpty());
            resized();
            if (onSectionChanged) onSectionChanged();
        }
    }

    // Skip the periodic refresh while the user is mid-edit so we don't
    // stomp their typed input. Same pattern the BPM editor uses below.
    // When MIDI learn is pending, the clock label dims and shows a
    // "MIDI LEARN…" prompt so the user knows the next CC/Note will be
    // captured. Cleared on capture (drained below in the same tick).
    if (! clockLabel.isBeingEdited())
    {
        const int learnTarget = engine.getSession().midiLearnPending.load (std::memory_order_relaxed);
        if (learnTarget >= 0)
        {
            clockLabel.setColour (juce::Label::textColourId, juce::Colour (0xffffd060));
            clockLabel.setText ("MIDI LEARN...", juce::dontSendNotification);
        }
        else
        {
            clockLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe0e0e0));
            const auto mode = (TimeDisplayMode) engine.getSession()
                                                   .timeDisplayMode.load (std::memory_order_relaxed);
            const float bpm = engine.getSession().tempoBpm.load (std::memory_order_relaxed);
            const int   bpb = engine.getSession().beatsPerBar.load (std::memory_order_relaxed);
            clockLabel.setText (formatSamplePosition (playhead,
                                                        engine.getCurrentSampleRate(),
                                                        bpm, bpb, mode),
                                 juce::dontSendNotification);
            // Icon shows what the next click will switch TO — matches the
            // convention used by Reaper / Pro Tools secondary time-scale
            // buttons. Bars-mode shows the clock (click → time); Time-mode
            // shows the bars glyph (click → bars).
            timeFormatToggle.setIcon (mode == TimeDisplayMode::Bars
                                          ? TransportIconButton::Icon::TimeClock
                                          : TransportIconButton::Icon::Bars);
        }
    }

    refreshButtonStates();

    // Auto-punch post-roll: while recording with punch enabled and the
    // playhead has crossed punchOut + postRoll samples, auto-stop. Done
    // here on the message thread so engine.stop()'s teardown is safe.
    // postRoll == 0 disables the auto-stop (matches the previous behaviour
    // where punch never auto-stopped); punch-disabled also disables it.
    {
        auto& transport = engine.getTransport();
        if (transport.isRecording() && transport.isPunchEnabled())
        {
            const auto pIn  = transport.getPunchIn();
            const auto pOut = transport.getPunchOut();
            const float postRoll = engine.getSession().postRollEnabled.load (std::memory_order_relaxed)
                                     ? engine.getSession().postRollSeconds.load (std::memory_order_relaxed)
                                     : 0.0f;
            if (pOut > pIn && postRoll > 0.0f && sr > 0.0)
            {
                const auto stopAt = pOut + (juce::int64) ((double) postRoll * sr);
                if (engine.getTransport().getPlayhead() >= stopAt)
                {
                    engine.stop();
                    notifyRecordStopped();
                }
            }
        }
    }

    // Drain the audio-thread queues for the MIDI controller infrastructure.
    // Both are atom-based handoffs - no allocation, no contention.
    {
        auto& s = engine.getSession();

        // Playhead locate queue: MTC chase pokes a target sample value
        // on the audio thread; Transport::setPlayhead isn't RT-safe so
        // we drain it here. -1 sentinel means "nothing pending". The
        // locate runs BEFORE pendingTransportAction so a combined
        // "locate + play" rising-edge from MTC chase lands the playhead
        // first and then engine.play() picks it up.
        if (const auto target = s.pendingTransportPlayhead.exchange (
                (juce::int64) -1, std::memory_order_relaxed); target >= 0)
        {
            engine.getTransport().setPlayhead (target);
        }

        // Transport-action queue: a binding hit on the audio thread (which
        // can't safely call engine.play/stop/record) pokes the queue; we
        // call the right method here on the message thread, then clear.
        const auto pending = (PendingTransportAction)
            s.pendingTransportAction.exchange ((int) PendingTransportAction::None,
                                                  std::memory_order_relaxed);
        switch (pending)
        {
            case PendingTransportAction::Play:   engine.play();   break;
            case PendingTransportAction::Stop:   engine.stop();   notifyRecordStopped(); break;
            case PendingTransportAction::Record: engine.record(); surfaceRecordSetupFailures(); break;
            case PendingTransportAction::Toggle:
                if (engine.getTransport().isStopped()) engine.play();
                else                                    { engine.stop(); notifyRecordStopped(); }
                break;
            case PendingTransportAction::LoopToggle:
            {
                auto& tr = engine.getTransport();
                tr.setLoopEnabled (! tr.isLoopEnabled());
                s.savedLoopEnabled = tr.isLoopEnabled();
                break;
            }
            case PendingTransportAction::GoToEnd:
            {
                // No fixed timeline end — jump to the end of the furthest
                // content across all tracks (audio + MIDI regions).
                juce::int64 end = 0;
                for (int t = 0; t < Session::kNumTracks; ++t)
                {
                    for (const auto& r : s.track (t).regions)
                        end = juce::jmax (end, r.timelineStart + r.lengthInSamples);
                    for (const auto& r : s.track (t).midiRegions.current())
                        end = juce::jmax (end, r.timelineStart + r.lengthInSamples);
                }
                engine.getTransport().setPlayhead (end);
                break;
            }
            case PendingTransportAction::None:   break;
        }

        // Learn capture: when learnPending is set and the audio thread has
        // stamped a captured (channel, dataNumber, trigger), append a
        // binding (replacing any existing one on the same source) and
        // clear both signals. The new binding starts driving its target
        // immediately on the next block.
        const auto learnTarget = s.midiLearnPending.load (std::memory_order_relaxed);
        const auto cap         = s.midiLearnCapture.load (std::memory_order_relaxed);
        if (learnTarget >= 0 && learnCaptureIsValid (cap))
        {
            MidiBinding b;
            b.channel     = unpackLearnCaptureChannel (cap);
            b.dataNumber  = unpackLearnCaptureDataNumber (cap);
            b.trigger     = unpackLearnCaptureTrigger (cap);
            b.target      = unpackLearnTargetKind (learnTarget);
            b.targetIndex = unpackLearnTargetIndex (learnTarget);
            // TrackPluginParam needs a second index (which parameter
            // of the loaded plugin). Read it from the slot's
            // last-touched tracker - the user must have moved the
            // target parameter via the plugin's own UI between
            // clicking Learn and triggering the MIDI source.
            //
            // -1 is the documented sentinel for "no parameter touched
            // since the slot was loaded" (see PluginSlot::
            // lastTouchedParamIndex declaration). The unload() path
            // resets to -1; load() also resets to -1 + installs the
            // listener that stamps the next user-touch. If a future
            // refactor changes that sentinel, the < 0 check here +
            // the < 0 guard in AudioEngine.cpp's apply path both need
            // to move with it.
            if (b.target == MidiBindingTarget::TrackPluginParam
                && b.targetIndex >= 0
                && b.targetIndex < Session::kNumTracks)
            {
                b.paramIndex = engine.getChannelStrip (b.targetIndex)
                                     .getPluginSlot()
                                     .getLastTouchedParamIndex();
                if (b.paramIndex < 0)
                {
                    // No parameter touched since the slot loaded -
                    // bail without writing a bogus binding. The
                    // learn-pending state clears so the user can
                    // retry after moving a knob.
                    s.midiLearnCapture.store (0, std::memory_order_relaxed);
                    s.midiLearnPending.store (-1, std::memory_order_relaxed);
                    return;
                }
            }
            // Drop any existing binding from the same source before
            // appending the new one - prevents stacking duplicate
            // mappings if a user re-learns the same fader.
            s.midiBindings.mutate ([&] (std::vector<MidiBinding>& binds)
            {
                binds.erase (std::remove_if (binds.begin(), binds.end(),
                    [&] (const MidiBinding& x)
                    {
                        return x.sourceMatches (b.channel, b.dataNumber, b.trigger);
                    }), binds.end());
                binds.push_back (b);
            });
            s.midiLearnCapture.store (0, std::memory_order_relaxed);
            s.midiLearnPending.store (-1, std::memory_order_relaxed);
        }
    }
}

void TransportBar::surfaceRecordSetupFailures()
{
    const auto& failed = engine.getRecordManager().getLastSetupFailures();
    if (failed.empty()) return;

    juce::String trackList;
    for (size_t i = 0; i < failed.size(); ++i)
    {
        if (i > 0) trackList += ", ";
        trackList += juce::String (failed[i] + 1);
    }
    const juce::String body =
        "These armed tracks could not start recording:\n\n"
        "    Tracks " + trackList + "\n\n"
        "Common causes: disk full, missing write permission on the "
        "session's audio folder, or a corrupted audio directory. The "
        "other armed tracks are recording normally; the listed tracks "
        "are NOT capturing audio. Stop the transport and check the "
        "session folder before continuing.";
    if (auto* tlw = getTopLevelComponent())
        showDuskAlert (*tlw, "Recording setup failed", body);
}

void TransportBar::notifyRecordStopped()
{
    const auto& errs = engine.getRecordManager().getLastRecordErrors();
    if (errs.empty()) return;

    juce::String body =
        "The last take captured with errors. Listed tracks may be partial "
        "or missing audio / MIDI data:\n\n";
    for (const auto& e : errs)
    {
        const juce::String kind =
            e.kind == RecordManager::RecordErrorKind::WavWrite
                ? "WAV write failed (disk full / I/O error)"
                : "MIDI events dropped (capture buffer full)";
        body += "    Track " + juce::String (e.trackIndex + 1)
              + " — " + kind + " ("
              + juce::String (e.count) + ")\n";
    }
    body += "\nCheck the session's audio folder for free space and the "
            "session log for I/O details before continuing.";

    if (auto* tlw = getTopLevelComponent())
        showDuskAlert (*tlw, "Recording errors", body);
}

void TransportBar::refreshButtonStates()
{
    const auto state = engine.getTransport().getState();
    playButton.setToggleState   (state == Transport::State::Playing,   juce::dontSendNotification);
    stopButton.setToggleState   (state == Transport::State::Stopped,   juce::dontSendNotification);
    recordButton.setToggleState (state == Transport::State::Recording, juce::dontSendNotification);

    // Sync mode-toggle visuals with the engine - they may have been
    // changed externally (session load, or a hotkey).
    loopButton.setToggleState  (engine.getTransport().isLoopEnabled(),
                                 juce::dontSendNotification);
    punchButton.setToggleState (engine.getTransport().isPunchEnabled(),
                                 juce::dontSendNotification);
    clickToggle.setToggleState (engine.getSession().metronomeEnabled.load(),
                                juce::dontSendNotification);
    countInToggle.setToggleState (engine.getSession().countInEnabled.load(),
                                   juce::dontSendNotification);

    // Avoid stomping the user's mid-edit text - only refresh the BPM
    // display when the label isn't currently being typed into. Show the tempo
    // AT THE PLAYHEAD (follows the tempo map), not the constant bar-1 tempo —
    // otherwise this clobbers the playhead-driven value timerCallback just set.
    if (! bpmValue.isBeingEdited())
        bpmValue.setText (juce::String ((int) std::round (
                              engine.getSession().bpmAt (engine.getTransport().getPlayhead()))),
                           juce::dontSendNotification);

    // BPM caption priority:
    //   "MTC" + amber  — incoming MTC quarter-frames decoded (master is
    //                    driving absolute time)
    //   "EXT" + amber  — incoming MIDI Clock producing a stable BPM
    //   "BPM" + grey   — Dusk Studio-internal tempo
    // MTC takes priority over EXT because MTC implies a richer
    // master-slave relationship; if both flow, the user cares more
    // about the absolute-time chase.
    {
        const auto& s = engine.getSession();
        const bool mtcRolling = s.externalTimeCodeRolling.load (std::memory_order_relaxed);
        const float ext = s.externalBpm.load (std::memory_order_relaxed);
        const bool clockActive = ext > 0.0f;
        const char* captionText = mtcRolling ? "MTC"
                                : clockActive ? "EXT" : "BPM";
        const bool externalActive = mtcRolling || clockActive;
        bpmCaption.setText (captionText, juce::dontSendNotification);
        bpmCaption.setColour (juce::Label::textColourId,
            externalActive ? juce::Colour (0xffe0c060)
                            : juce::Colour (0xff707074));
    }

    juce::String hint;
    switch (state)
    {
        case Transport::State::Stopped:
            hint = engine.getRecordManager().isActive() ? "Finalising..." :
                   "Arm a track and press REC to record. Press PLAY to play back.";
            break;
        case Transport::State::Playing:   hint = "Playing...";   break;
        case Transport::State::Recording: hint = "Recording - press STOP to commit"; break;
    }
    hintLabel.setText (hint, juce::dontSendNotification);
}

void TransportBar::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff181820));
    g.setColour (juce::Colour (0xff2a2a32));
    g.drawRect (getLocalBounds(), 1);
}

void TransportBar::resized()
{
    auto area = getLocalBounds().reduced (8, 6);

   #if JUCE_MAC
    int macTitlePad = 78;
    if (auto* top = getTopLevelComponent())
        if (auto* peer = top->getPeer())
            if (peer->isFullScreen() || peer->isKioskMode())
                macTitlePad = 0;
    area.removeFromLeft (macTitlePad);
   #endif

    constexpr int kBtnDia = 36;
    constexpr int kBtnGap = 4;
    auto buttons = area.removeFromLeft (kBtnDia * 8 + kBtnGap * 7);
    const int yPad = juce::jmax (0, (buttons.getHeight() - kBtnDia) / 2);
    auto buttonRow = buttons.reduced (0, yPad);

    auto place = [&] (juce::Component& c)
    {
        c.setBounds (buttonRow.removeFromLeft (kBtnDia));
        if (buttonRow.getWidth() >= kBtnGap)
            buttonRow.removeFromLeft (kBtnGap);
    };
    place (stopButton);
    place (rewButton);
    place (playButton);
    place (ffwdButton);
    place (recordButton);
    place (loopButton);
    place (punchButton);
    place (keyboardButton);

    const bool compact = getWidth() < kCompactTransportWidth;
    syncCompactLabels (compact);

    area.removeFromLeft (12);
    timeFormatToggle.setVisible (true);
    {
        auto tfRect = area.removeFromLeft (kBtnDia);
        const int tfPad = juce::jmax (0, (tfRect.getHeight() - kBtnDia) / 2);
        timeFormatToggle.setBounds (tfRect.reduced (0, tfPad));
        area.removeFromLeft (6);
    }
    clockLabel.setBounds (area.removeFromLeft (compact ? 110 : 130));

    // Current-section pill, sized to its text, just right of the clock.
    if (sectionLabel.isVisible())
    {
        area.removeFromLeft (8);
        const int textW = sectionLabel.getFont().getStringWidth (sectionLabel.getText()) + 16;
        const int maxW  = compact ? 110 : 190;
        sectionLabel.setBounds (area.removeFromLeft (juce::jmin (textW, maxW)).reduced (0, 4));
    }

    tapeToggle.setBounds (area.removeFromRight (compact ? 32 : 84).reduced (1));
    area.removeFromRight (12);

    countInToggle.setBounds (area.removeFromRight (compact ? 34 : 44).reduced (1, 4));
    area.removeFromRight (4);
    {
        auto rect = area.removeFromRight (kBtnDia);
        const int pad = juce::jmax (0, (rect.getHeight() - kBtnDia) / 2);
        clickToggle.setBounds (rect.reduced (0, pad));
    }
    area.removeFromRight (4);
    tapButton.setBounds  (area.removeFromRight (compact ? 42 : 48).reduced (1, 4));
    area.removeFromRight (4);
    timeSigButton.setBounds (area.removeFromRight (compact ? 44 : 52).reduced (1, 4));
    area.removeFromRight (4);
    bpmValue.setBounds   (area.removeFromRight (compact ? 44 : 52).reduced (1, 4));
    bpmCaption.setVisible (! compact);
    if (! compact)
    {
        bpmCaption.setBounds (area.removeFromRight (32).reduced (1, 4));
        area.removeFromRight (8);
    }
    else
    {
        area.removeFromRight (4);
    }
    {
        auto rect = area.removeFromRight (kBtnDia);
        const int pad = juce::jmax (0, (rect.getHeight() - kBtnDia) / 2);
        tuneButton.setBounds (rect.reduced (0, pad));
    }
    area.removeFromRight (compact ? 8 : 12);

    area.removeFromLeft (12);
    hintLabel.setBounds (area);
}

void TransportBar::setHintVisible (bool visible)
{
    hintLabel.setVisible (visible);
}

void TransportBar::mouseDown (const juce::MouseEvent& e)
{
    // Right-click on PUNCH opens the auto-punch settings (pre-roll /
    // post-roll). Plain click still toggles punch on/off.
    if (e.eventComponent == &punchButton && e.mods.isPopupMenu())
    {
        showPunchSettingsMenu();
        return;
    }
    // Right-click on the metronome (CLICK) button opens the behaviour
    // menu — when the click fires (recording / count-in / playing) +
    // polyphonic toggle. Plain click stays as on/off.
    if (e.eventComponent == &clickToggle && e.mods.isPopupMenu())
    {
        showMetronomeSettingsMenu();
        return;
    }
    // Right-click on clock flips the time-display mode. Always
    // available — including in compact mode where the dedicated
    // timeFormatToggle button is hidden to avoid bank-button collision.
    if (e.eventComponent == &clockLabel && e.mods.isPopupMenu())
    {
        if (flipTimeModeOnClock) flipTimeModeOnClock();
        return;
    }
    // Right-click on the transport buttons opens the MIDI Learn menu.
    if (e.mods.isPopupMenu())
    {
        if (e.eventComponent == &playButton)
        {
            midilearn::showLearnMenu (playButton, engine.getSession(),
                                        MidiBindingTarget::TransportPlay);
            return;
        }
        if (e.eventComponent == &stopButton)
        {
            midilearn::showLearnMenu (stopButton, engine.getSession(),
                                        MidiBindingTarget::TransportStop);
            return;
        }
        if (e.eventComponent == &recordButton)
        {
            midilearn::showLearnMenu (recordButton, engine.getSession(),
                                        MidiBindingTarget::TransportRecord);
            return;
        }
    }
    juce::Component::mouseDown (e);
}

void TransportBar::syncCompactLabels (bool compact)
{
    const bool tapeExpanded = tapeToggle.getToggleState();
    if (compact)
    {
        tapeToggle .setButtonText (tapeExpanded
            ? juce::CharPointer_UTF8 ("\xe2\x96\xbe")    // "▾"
            : juce::CharPointer_UTF8 ("\xe2\x96\xb8")); // "▸"
    }
    else
    {
        tapeToggle .setButtonText (tapeExpanded
            ? juce::CharPointer_UTF8 ("\xe2\x96\xbe TIMELINE")    // "▾ TIMELINE"
            : juce::CharPointer_UTF8 ("\xe2\x96\xb8 TIMELINE")); // "▸ TIMELINE"
    }
}

void TransportBar::showPunchSettingsMenu()
{
    juce::PopupMenu m;
    auto& s = engine.getSession();
    const float pre  = s.preRollSeconds.load (std::memory_order_relaxed);
    const float post = s.postRollSeconds.load (std::memory_order_relaxed);

    auto formatSecs = [] (float v)
    {
        if (v <= 0.0f) return juce::String ("Off");
        const auto numeric = (std::abs (v - std::round (v)) < 0.05f)
                                ? juce::String ((int) std::round (v))
                                : juce::String (v, 1);
        return numeric + " s";
    };

    static constexpr float kPresets[] = { 0.0f, 1.0f, 2.0f, 3.0f, 5.0f, 10.0f };

    juce::PopupMenu preMenu;
    for (const float v : kPresets)
        preMenu.addItem (formatSecs (v), true, std::abs (pre - v) < 0.05f,
            [&s, v] { s.preRollSeconds.store  (v, std::memory_order_relaxed); });

    juce::PopupMenu postMenu;
    for (const float v : kPresets)
        postMenu.addItem (formatSecs (v), true, std::abs (post - v) < 0.05f,
            [&s, v] { s.postRollSeconds.store (v, std::memory_order_relaxed); });

    const bool preEn  = s.preRollEnabled.load  (std::memory_order_relaxed);
    const bool postEn = s.postRollEnabled.load (std::memory_order_relaxed);

    m.addSectionHeader ("Auto-punch");
    m.addItem ("Roll IN before punch (audible context)", true, preEn,
        [&s, preEn] { s.preRollEnabled.store (! preEn, std::memory_order_relaxed); });
    m.addSubMenu ("Pre-roll: "  + formatSecs (pre),  preMenu);
    m.addItem ("Stop after punch-out (auto-stop)",     true, postEn,
        [&s, postEn] { s.postRollEnabled.store (! postEn, std::memory_order_relaxed); });
    m.addSubMenu ("Post-roll: " + formatSecs (post), postMenu);
    showContextMenu (m, punchButton);
}

void TransportBar::showMetronomeSettingsMenu()
{
    auto& s = engine.getSession();
    const bool whileRec   = s.metronomeClickWhileRecording.load (std::memory_order_relaxed);
    const bool onlyCount  = s.metronomeOnlyDuringCountIn  .load (std::memory_order_relaxed);
    const bool whilePlay  = s.metronomeClickWhilePlaying  .load (std::memory_order_relaxed);
    const bool poly       = s.metronomePolyphonic         .load (std::memory_order_relaxed);

    juce::PopupMenu m;
    m.addSectionHeader ("Click behaviour");
    m.addItem ("Click while recording",  true, whileRec,
                [&s, whileRec] { s.metronomeClickWhileRecording.store (! whileRec, std::memory_order_relaxed); });
    m.addItem ("Only during count-in",   true, onlyCount,
                [&s, onlyCount] { s.metronomeOnlyDuringCountIn.store (! onlyCount, std::memory_order_relaxed); });
    m.addItem ("Click while playing",    true, whilePlay,
                [&s, whilePlay] { s.metronomeClickWhilePlaying.store (! whilePlay, std::memory_order_relaxed); });
    m.addSeparator();
    m.addItem ("Polyphonic clicks",      true, poly,
                [&s, poly] { s.metronomePolyphonic.store (! poly, std::memory_order_relaxed); });
    showContextMenu (m, clickToggle);
}

void TransportBar::onTap()
{
    // Stamp this tap. If the previous tap was longer than the timeout
    // ago, treat this as the start of a new pulse and reset the ring.
    // Otherwise append to the ring (oldest stamp falls off).
    const auto nowMs = juce::Time::currentTimeMillis();
    if (tapStampCount > 0)
    {
        const auto& lastStamp = tapStamps[(size_t) (tapStampCount - 1) % (size_t) kTapWindow];
        if (nowMs - lastStamp > kTapTimeoutMs)
            tapStampCount = 0;
    }

    if (tapStampCount < kTapWindow)
    {
        tapStamps[(size_t) tapStampCount] = nowMs;
        ++tapStampCount;
    }
    else
    {
        // Shift left by one - drop oldest, append newest. Small N so a
        // memmove-style shift is fine; no allocation.
        for (int i = 1; i < kTapWindow; ++i)
            tapStamps[(size_t) (i - 1)] = tapStamps[(size_t) i];
        tapStamps[(size_t) (kTapWindow - 1)] = nowMs;
    }

    if (tapStampCount < 2) return;   // need at least one interval

    // Average inter-tap interval over the active window. The session BPM
    // is clamped to the same 30..300 range as the manual editor.
    const int intervals = tapStampCount - 1;
    juce::int64 sumMs = 0;
    for (int i = 1; i < tapStampCount; ++i)
        sumMs += tapStamps[(size_t) i] - tapStamps[(size_t) (i - 1)];
    const double avgMs = (double) sumMs / (double) intervals;
    if (avgMs <= 0.0) return;
    const float bpm = juce::jlimit (30.0f, 300.0f, (float) (60000.0 / avgMs));
    const float oldBpm = engine.getSession().tempoBpm.load (std::memory_order_relaxed);
    confirmAndApplyBpm (bpm, oldBpm);
}

void TransportBar::refreshTimeSigButton()
{
    auto& s = engine.getSession();
    const int n = juce::jmax (1, s.beatsPerBar.load (std::memory_order_relaxed));
    const int d = juce::jmax (1, s.beatUnit   .load (std::memory_order_relaxed));
    timeSigButton.setButtonText (juce::String (n) + "/" + juce::String (d));
}

void TransportBar::applyTimeSig (int numerator, int denominator)
{
    // Beat unit is restricted to power-of-two divisions; anything else
    // breaks the metronome subdivision math. Clamp numerator to a
    // musical range (1..32) - 32 is enough for double-time 16-bar phrases.
    static const int kAllowedDenoms[] = { 1, 2, 4, 8, 16, 32 };
    int safeD = denominator;
    bool denomOk = false;
    for (int d : kAllowedDenoms) if (d == safeD) { denomOk = true; break; }
    if (! denomOk) safeD = 4;
    const int safeN = juce::jlimit (1, 32, numerator);

    auto& s = engine.getSession();
    s.beatsPerBar.store (safeN, std::memory_order_release);
    s.beatUnit   .store (safeD, std::memory_order_release);
    refreshTimeSigButton();
}

void TransportBar::promptCustomTimeSig()
{
    auto& s = engine.getSession();
    const auto current = juce::String (s.beatsPerBar.load (std::memory_order_relaxed))
                       + "/"
                       + juce::String (s.beatUnit   .load (std::memory_order_relaxed));
    auto* tlw = getTopLevelComponent();
    if (tlw == nullptr) return;

    juce::Component::SafePointer<TransportBar> safe (this);
    showDuskTextInput (*tlw, "Custom time signature",
                          "Enter N/D — numerator first, then denominator "
                          "(power of two: 1, 2, 4, 8, 16, 32).",
                          current,
                          [safe] (const juce::String& text)
                          {
                              if (safe == nullptr) return;
                              const auto slash = text.indexOfChar ('/');
                              if (slash <= 0) return;
                              const int n = text.substring (0, slash).getIntValue();
                              const int d = text.substring (slash + 1).getIntValue();
                              safe->applyTimeSig (n, d);
                          });
}

void TransportBar::showTimeSigMenu()
{
    auto& s = engine.getSession();
    const int curN = s.beatsPerBar.load (std::memory_order_relaxed);
    const int curD = s.beatUnit   .load (std::memory_order_relaxed);

    // std::array (not a raw C array) so the lambda capture below is
    // unambiguously by-value across every compiler the project targets.
    // A raw `const Preset[6]` capture by value IS well-formed in C++17,
    // but std::array removes the question entirely and reads identically.
    struct Preset { int n, d; };
    const std::array<Preset, 6> presets = { {
        { 3, 4 }, { 4, 4 }, { 5, 4 },
        { 6, 8 }, { 7, 8 }, { 12, 8 },
    } };

    juce::PopupMenu m;
    for (int i = 0; i < (int) presets.size(); ++i)
    {
        const auto& p = presets[(size_t) i];
        const bool ticked = (p.n == curN && p.d == curD);
        m.addItem (i + 1,
                    juce::String (p.n) + "/" + juce::String (p.d),
                    true, ticked);
    }
    m.addSeparator();
    // static: referenced inside the context-menu lambda below; static
    // storage duration avoids MSVC C3493 (implicit-capture) without the
    // explicit capture that clang flags as -Wunused-lambda-capture.
    static constexpr int kCustomId = 100;
    m.addItem (kCustomId, "Custom...");

    juce::Component::SafePointer<TransportBar> safe (this);
    showContextMenu (m, timeSigButton,
        [safe, presets] (int chosen)
        {
            if (safe == nullptr || chosen <= 0) return;
            if (chosen == kCustomId) { safe->promptCustomTimeSig(); return; }
            if (chosen >= 1 && chosen <= (int) presets.size())
            {
                const auto& p = presets[(size_t) (chosen - 1)];
                safe->applyTimeSig (p.n, p.d);
            }
        });
}

void TransportBar::confirmAndApplyBpm (float newBpm, float oldBpm)
{
    auto& s = engine.getSession();

    // Treat tempo changes inside the BPM-spinner's own integer rounding
    // as no-ops. Without this, double-clicking the field without editing
    // would still pop the dialog.
    if (std::abs (newBpm - oldBpm) < 0.5f)
    {
        bpmValue.setText (juce::String ((int) newBpm), juce::dontSendNotification);
        return;
    }

    // When a tempo map is active it governs timing directly, so the BPM field
    // edits the bar-1 tempo point instead of running the legacy single-tempo
    // region retime (and there's no retime dialog — nothing is rescaled).
    if (! s.tempoMap.empty())
    {
        auto pts = s.tempoMap.points();
        pts.front().bpm = newBpm;
        engine.setTempoPoints (std::move (pts));
        bpmValue.setText (juce::String ((int) newBpm), juce::dontSendNotification);
        return;
    }

    int lockedMidi = 0, floatMidi = 0;
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        const auto& v = s.track (t).midiRegions.current();
        for (const auto& r : v)
            (r.tempoLock ? lockedMidi : floatMidi) += 1;
    }

    int autoPoints = 0;
    for (int t = 0; t < Session::kNumTracks; ++t)
        for (const auto& lane : s.track (t).automationLanes)
            autoPoints += (int) lane.pointsConst().size();
    for (int a = 0; a < Session::kNumAuxLanes; ++a)
        for (const auto& lane : s.auxLane (a).params.automationLanes)
            autoPoints += (int) lane.pointsConst().size();
    for (const auto& lane : s.master().automationLanes)
        autoPoints += (int) lane.pointsConst().size();

    const bool empty = (lockedMidi == 0 && floatMidi == 0 && autoPoints == 0);
    if (empty)
    {
        applyTempoChange (s, newBpm, engine.getCurrentSampleRate());
        bpmValue.setText (juce::String ((int) newBpm), juce::dontSendNotification);
        return;
    }

    juce::String body;
    body << "Change tempo from " << (int) oldBpm << " to "
         << (int) newBpm << " BPM?\n\n";
    if (lockedMidi > 0)
        body << "    " << lockedMidi
             << " tempo-locked MIDI region" << (lockedMidi == 1 ? "" : "s")
             << " will retime to keep musical position.\n";
    if (floatMidi > 0)
        body << "    " << floatMidi
             << " floating MIDI region" << (floatMidi == 1 ? "" : "s")
             << " will keep sample position (musical length changes).\n";
    if (autoPoints > 0)
        body << "    " << autoPoints
             << " automation point" << (autoPoints == 1 ? "" : "s")
             << " will scale (already anchored to recordedAtBPM).\n";
    body << "\nAudio regions and markers are NOT retimed.";

    // Capture by value so the lambda doesn't hold a dangling reference
    // if the TransportBar is destroyed before the user clicks.
    juce::Component::SafePointer<TransportBar> safe (this);
    auto* tlw = getTopLevelComponent();
    if (tlw == nullptr) return;
    showDuskConfirm (*tlw, "Confirm tempo change", body,
                       /*primary*/   "Apply",
                       /*onPrimary*/ [safe, newBpm]
                       {
                           if (safe == nullptr) return;
                           applyTempoChange (safe->engine.getSession(), newBpm,
                                               safe->engine.getCurrentSampleRate());
                           safe->bpmValue.setText (juce::String ((int) newBpm),
                                                     juce::dontSendNotification);
                       },
                       /*secondary*/   "Cancel",
                       /*onSecondary*/ [safe, oldBpm]
                       {
                           if (safe == nullptr) return;
                           safe->bpmValue.setText (juce::String ((int) oldBpm),
                                                     juce::dontSendNotification);
                       });
}
} // namespace duskstudio
