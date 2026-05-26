#include "VirtualKeyboardComponent.h"

namespace duskstudio
{
namespace
{
// Lookup table: ASCII code -> semitone offset from centreNote.
// Filled at startup; -1 means the code isn't part of the layout.
struct KeyMap
{
    std::array<int, 128> offset {};

    KeyMap()
    {
        offset.fill (-1);
        // Lower row: C-octave
        const char* low = "ZSXDCVGBHNJM";
        for (int i = 0; low[i]; ++i)
        {
            offset[(size_t) low[i]]                           = i;
            offset[(size_t) juce::CharacterFunctions::toLowerCase (low[i])] = i;
        }
        // Upper row: C+1 octave
        const char* up = "Q2W3ER5T6Y7U";
        for (int i = 0; up[i]; ++i)
        {
            offset[(size_t) up[i]]                           = 12 + i;
            offset[(size_t) juce::CharacterFunctions::toLowerCase (up[i])] = 12 + i;
        }
        // Top row: C+2 partial
        const char* top = "I9O0P";
        for (int i = 0; top[i]; ++i)
        {
            offset[(size_t) top[i]]                           = 24 + i;
            offset[(size_t) juce::CharacterFunctions::toLowerCase (top[i])] = 24 + i;
        }
    }
};

const KeyMap& keyMap()
{
    static const KeyMap k;
    return k;
}

bool isBlackKey (int midiNote) noexcept
{
    const int pc = ((midiNote % 12) + 12) % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}
} // namespace

VirtualKeyboardComponent::VirtualKeyboardComponent (AudioEngine& engineRef)
    : engine (engineRef)
{
    setWantsKeyboardFocus (true);

    auto styleHeaderBtn = [] (juce::TextButton& b)
    {
        b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff262630));
        b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd0d0d4));
        b.setMouseClickGrabsKeyboardFocus (false);
    };
    styleHeaderBtn (octDownBtn);
    styleHeaderBtn (octUpBtn);
    styleHeaderBtn (chDownBtn);
    styleHeaderBtn (chUpBtn);

    octDownBtn.onClick = [this]
    {
        const int prev = centreNote;
        centreNote = juce::jlimit (0, 120, centreNote - 12);
        if (centreNote != prev) { releaseAll(); repaint(); }
    };
    octUpBtn.onClick = [this]
    {
        const int prev = centreNote;
        centreNote = juce::jlimit (0, 120, centreNote + 12);
        if (centreNote != prev) { releaseAll(); repaint(); }
    };
    chDownBtn.onClick = [this]
    {
        const int prev = channel;
        channel = juce::jlimit (1, 16, channel - 1);
        if (channel != prev) repaint();
    };
    chUpBtn.onClick = [this]
    {
        const int prev = channel;
        channel = juce::jlimit (1, 16, channel + 1);
        if (channel != prev) repaint();
    };

    addAndMakeVisible (octDownBtn);
    addAndMakeVisible (octUpBtn);
    addAndMakeVisible (chDownBtn);
    addAndMakeVisible (chUpBtn);

    startTimerHz (30);
}

VirtualKeyboardComponent::~VirtualKeyboardComponent()
{
    // Stop the timer BEFORE releaseAll() so the timerCallback can't
    // fire on a half-released keyboard state. See BusComponent::
    // ~BusComponent for the broader rationale.
    stopTimer();
    releaseAll();
}

int VirtualKeyboardComponent::noteForKeyCode (int keyCode) const noexcept
{
    if (keyCode < 0 || keyCode >= (int) keyMap().offset.size())
        return -1;
    const int off = keyMap().offset[(size_t) keyCode];
    if (off < 0) return -1;
    const int note = centreNote + off;
    if (note < 0 || note > 127) return -1;
    return note;
}

bool VirtualKeyboardComponent::keyPressed (const juce::KeyPress& k)
{
    const int code = k.getKeyCode();

    if (code == juce::KeyPress::upKey)
    {
        const int prev = centreNote;
        centreNote = juce::jlimit (0, 120, centreNote + 12);
        if (centreNote != prev) { releaseAll(); repaint(); }
        return true;
    }
    if (code == juce::KeyPress::downKey)
    {
        const int prev = centreNote;
        centreNote = juce::jlimit (0, 120, centreNote - 12);
        if (centreNote != prev) { releaseAll(); repaint(); }
        return true;
    }
    if (code == juce::KeyPress::leftKey)
    {
        const int prev = channel;
        channel = juce::jlimit (1, 16, channel - 1);
        if (channel != prev) repaint();
        return true;
    }
    if (code == juce::KeyPress::rightKey)
    {
        const int prev = channel;
        channel = juce::jlimit (1, 16, channel + 1);
        if (channel != prev) repaint();
        return true;
    }

    if (code < 0 || code >= (int) held.size())
        return false;

    const int note = noteForKeyCode (code);
    if (note < 0) return false;

    // Auto-repeat fires keyPressed again for an already-held key — ignore.
    if (held[(size_t) code].note >= 0) return true;

    held[(size_t) code] = { note, channel };
    sendNoteOn (note, velocity, channel);
    repaint();
    return true;
}

void VirtualKeyboardComponent::timerCallback()
{
    bool any = false;
    for (int code = 0; code < (int) held.size(); ++code)
    {
        auto& slot = held[(size_t) code];
        if (slot.note < 0) continue;
        if (! juce::KeyPress::isKeyCurrentlyDown (code))
        {
            sendNoteOff (slot.note, slot.channel);
            slot = {};
            any = true;
        }
    }
    if (any) repaint();
}

void VirtualKeyboardComponent::sendNoteOn (int note, int vel, int chan)
{
    if (auto* col = engine.getVirtualKeyboardCollector())
        col->addMessageToQueue (juce::MidiMessage::noteOn (chan, note, (juce::uint8) vel));
    if (onNoteOn) onNoteOn (note, vel, chan);
}

void VirtualKeyboardComponent::sendNoteOff (int note, int chan)
{
    if (auto* col = engine.getVirtualKeyboardCollector())
        col->addMessageToQueue (juce::MidiMessage::noteOff (chan, note));
    if (onNoteOff) onNoteOff (note, chan);
}

void VirtualKeyboardComponent::releaseAll()
{
    for (auto& slot : held)
    {
        if (slot.note >= 0)
        {
            sendNoteOff (slot.note, slot.channel);
            slot = {};
        }
    }
    if (mouseHeld.note >= 0)
    {
        sendNoteOff (mouseHeld.note, mouseHeld.channel);
        mouseHeld = {};
    }
}

juce::String VirtualKeyboardComponent::letterForNote (int note) const
{
    const int targetOffset = note - centreNote;
    if (targetOffset < 0 || targetOffset > 28) return {};
    const auto& km = keyMap().offset;
    // Walk uppercase letters first (preferred display), then digits.
    for (int c = 'A'; c <= 'Z'; ++c)
        if (km[(size_t) c] == targetOffset)
            return juce::String::charToString ((juce::juce_wchar) c);
    for (int c = '0'; c <= '9'; ++c)
        if (km[(size_t) c] == targetOffset)
            return juce::String::charToString ((juce::juce_wchar) c);
    return {};
}

int VirtualKeyboardComponent::noteAtPoint (juce::Point<int> p) const
{
    if (! keyboardArea.contains (p)) return -1;

    const int firstNote = juce::jmax (0, centreNote - 12);
    const int lastNote  = juce::jmin (127, firstNote + 36);
    int numWhite = 0;
    for (int m = firstNote; m <= lastNote; ++m)
        if (! isBlackKey (m)) ++numWhite;
    if (numWhite < 1) return -1;

    const float wkW = (float) keyboardArea.getWidth() / (float) numWhite;
    const float bkW = wkW * 0.62f;
    const float bkH = (float) keyboardArea.getHeight() * 0.62f;
    const float kbX = (float) keyboardArea.getX();
    const float kbY = (float) keyboardArea.getY();

    // Black keys checked FIRST since they sit on top of whites.
    float x = kbX;
    for (int m = firstNote; m <= lastNote; ++m)
    {
        if (isBlackKey (m))
        {
            const float bx = x - bkW * 0.5f;
            if ((float) p.x >= bx && (float) p.x < bx + bkW
                && (float) p.y >= kbY && (float) p.y < kbY + bkH)
                return m;
        }
        else
        {
            x += wkW;
        }
    }
    // Then white keys (full rect — black keys above already short-circuited).
    x = kbX;
    for (int m = firstNote; m <= lastNote; ++m)
    {
        if (isBlackKey (m)) continue;
        if ((float) p.x >= x && (float) p.x < x + wkW)
            return m;
        x += wkW;
    }
    return -1;
}

void VirtualKeyboardComponent::mouseDown (const juce::MouseEvent& e)
{
    const int note = noteAtPoint (e.getPosition());
    if (note < 0) return;
    mouseHeld = { note, channel };
    sendNoteOn (note, velocity, channel);
    repaint();
}

void VirtualKeyboardComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (mouseHeld.note < 0) return;
    const int note = noteAtPoint (e.getPosition());
    if (note < 0 || note == mouseHeld.note) return;
    // Glissando — release the previous note before triggering the new one.
    sendNoteOff (mouseHeld.note, mouseHeld.channel);
    mouseHeld = { note, channel };
    sendNoteOn (note, velocity, channel);
    repaint();
}

void VirtualKeyboardComponent::mouseUp (const juce::MouseEvent&)
{
    if (mouseHeld.note < 0) return;
    sendNoteOff (mouseHeld.note, mouseHeld.channel);
    mouseHeld = {};
    repaint();
}

void VirtualKeyboardComponent::resized()
{
    auto bounds = getLocalBounds();
    auto header = bounds.removeFromTop (24);
    // Header buttons: octave -/+ and channel -/+ pinned to the right
    // alongside the status text. Each button is 56 px wide.
    constexpr int kBtnW = 56;
    constexpr int kBtnGap = 4;
    chUpBtn  .setBounds (header.removeFromRight (kBtnW).reduced (2, 2));
    header.removeFromRight (kBtnGap);
    chDownBtn.setBounds (header.removeFromRight (kBtnW).reduced (2, 2));
    header.removeFromRight (kBtnGap + 12);
    octUpBtn .setBounds (header.removeFromRight (kBtnW).reduced (2, 2));
    header.removeFromRight (kBtnGap);
    octDownBtn.setBounds (header.removeFromRight (kBtnW).reduced (2, 2));

    // Keyboard fills the rest (no footer legend — letters live ON the
    // keys now).
    keyboardArea = bounds;
}

void VirtualKeyboardComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    g.setColour (juce::Colour (0xff1a1a20));
    g.fillRect (bounds);

    // Title strip — text on the left, status text in the middle (between
    // the buttons that resized() already placed on the right).
    auto titleArea = bounds.removeFromTop (24.0f);
    g.setColour (juce::Colour (0xffd0d0d0));
    g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    g.drawText ("VIRTUAL MIDI KEYBOARD", titleArea.reduced (10.0f, 2.0f),
                juce::Justification::centredLeft);

    g.setColour (juce::Colour (0xff8090a0));
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    const auto status = juce::String ("CH ") + juce::String (channel)
                      + "   Centre: " + juce::MidiMessage::getMidiNoteName (centreNote, true, true, 4);
    // Leave room on the right for the 4 header buttons (4 × 56 + gaps ≈ 252).
    const float reservedRight = 260.0f;
    auto statusRect = titleArea.reduced (10.0f, 2.0f);
    statusRect.removeFromRight (reservedRight);
    g.drawText (status, statusRect, juce::Justification::centredRight);

    // Piano keyboard. Show 3 octaves anchored so centreNote sits at the
    // start of the middle octave (so the lowest mapped key, Z=centreNote,
    // is visible with an octave of context below it for orientation).
    const int firstNote = juce::jmax (0, centreNote - 12);
    const int lastNote  = juce::jmin (127, firstNote + 36);
    const int numWhite  = [&] {
        int n = 0;
        for (int m = firstNote; m <= lastNote; ++m)
            if (! isBlackKey (m)) ++n;
        return juce::jmax (1, n);
    }();

    const auto kb = keyboardArea.toFloat();
    const float wkW = kb.getWidth() / (float) numWhite;
    const float wkH = kb.getHeight();
    const float bkW = wkW * 0.62f;
    const float bkH = wkH * 0.62f;

    // Highlight set: notes currently held (any source — typing keyboard OR mouse).
    std::array<bool, 128> isHeld {};
    for (const auto& slot : held)
        if (slot.note >= 0)
            isHeld[(size_t) slot.note] = true;
    if (mouseHeld.note >= 0) isHeld[(size_t) mouseHeld.note] = true;

    // First pass: white keys + their letter labels.
    float x = kb.getX();
    for (int m = firstNote; m <= lastNote; ++m)
    {
        if (isBlackKey (m)) continue;

        juce::Rectangle<float> r (x, kb.getY(), wkW - 1.0f, wkH);

        const bool active = isHeld[(size_t) m];
        if (active)
            g.setColour (juce::Colour (0xff5fa0d0));
        else
            g.setColour (juce::Colour (0xfff0f0f0));
        g.fillRoundedRectangle (r, 2.0f);

        g.setColour (juce::Colour (0xff404048));
        g.drawRoundedRectangle (r, 2.0f, 0.8f);

        // Octave label (C with octave number) on every C, painted at
        // the very bottom of the key.
        const int pc = ((m % 12) + 12) % 12;
        auto labelArea = r;
        if (pc == 0)
        {
            g.setColour (juce::Colour (0xff707078));
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            g.drawText (juce::MidiMessage::getMidiNoteName (m, true, true, 4),
                        labelArea.removeFromBottom (14.0f),
                        juce::Justification::centred);
        }

        // Typing-keyboard letter painted ON the key (large, dark on
        // white). Only shown when this note maps to a letter under the
        // current centreNote shift.
        const auto letter = letterForNote (m);
        if (letter.isNotEmpty())
        {
            g.setColour (active ? juce::Colour (0xff203a50)
                                 : juce::Colour (0xff404048));
            g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
            // Above the C-label if present, otherwise centred-bottom.
            auto letterArea = labelArea.removeFromBottom (18.0f);
            g.drawText (letter, letterArea, juce::Justification::centred);
        }
        x += wkW;
    }

    // Second pass: black keys overlaid + their letter labels.
    x = kb.getX();
    for (int m = firstNote; m <= lastNote; ++m)
    {
        if (isBlackKey (m))
        {
            const float bx = x - bkW * 0.5f;
            juce::Rectangle<float> r (bx, kb.getY(), bkW, bkH);

            const bool active = isHeld[(size_t) m];
            g.setColour (active ? juce::Colour (0xff5fa0d0).darker (0.20f)
                                : juce::Colour (0xff181820));
            g.fillRoundedRectangle (r, 2.0f);
            g.setColour (juce::Colour (0xff000000));
            g.drawRoundedRectangle (r, 2.0f, 0.6f);

            // Letter on black key — white text, bold, near the bottom
            // so it doesn't compete with the white keys' visual mass
            // above.
            const auto letter = letterForNote (m);
            if (letter.isNotEmpty())
            {
                g.setColour (active ? juce::Colour (0xffe0e0e8)
                                     : juce::Colour (0xffb0b0b8));
                g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
                g.drawText (letter, r.removeFromBottom (16.0f),
                             juce::Justification::centred);
            }
            continue;
        }
        x += wkW;
    }
}
} // namespace duskstudio
