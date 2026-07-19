#include "VirtualKeyboardComponent.h"
#include "AppConfig.h"
#if defined(__linux__)
 #include "KeyboardStateLinux.h"
#endif

#include <algorithm>

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

// Is the typing key for this JUCE key code physically held? On Linux this
// queries the X server's physical key state (see KeyboardStateLinux), which is
// reliable under XWayland auto-repeat; juce::KeyPress::isKeyCurrentlyDown there
// goes stale mid-repeat and would drop a held note. Elsewhere (and if the X
// query is unavailable) it falls back to the JUCE query.
bool isCodePhysicallyDown (int code) noexcept
{
   #if defined(__linux__)
    const int phys = isKeyPhysicallyDown (code);
    if (phys >= 0)
        return phys == 1;
   #endif
    return juce::KeyPress::isKeyCurrentlyDown (code);
}
} // namespace

VirtualKeyboardComponent::VirtualKeyboardComponent (AudioEngine& engineRef)
    : engine (engineRef)
{
    setWantsKeyboardFocus (true);

    // Restore the last-used centre note (default C2 = MIDI 36 on first run).
    // Clamp to the same range shiftCentre enforces so a hand-edited / corrupt
    // config can't seed an out-of-range centre.
    centreNote = std::clamp (appconfig::getVkbCentreNote(), 0, 120);

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

    octDownBtn.onClick = [this] { shiftCentre (-12); };
    octUpBtn  .onClick = [this] { shiftCentre (+12); };
    chDownBtn.onClick = [this]
    {
        const int prev = channel;
        channel = std::clamp (channel - 1, 1, 16);
        if (channel != prev) repaint();
    };
    chUpBtn.onClick = [this]
    {
        const int prev = channel;
        channel = std::clamp (channel + 1, 1, 16);
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

    if (code == juce::KeyPress::upKey)   { shiftCentre (+12); return true; }
    if (code == juce::KeyPress::downKey) { shiftCentre (-12); return true; }
    if (code == juce::KeyPress::leftKey)
    {
        const int prev = channel;
        channel = std::clamp (channel - 1, 1, 16);
        if (channel != prev) repaint();
        return true;
    }
    if (code == juce::KeyPress::rightKey)
    {
        const int prev = channel;
        channel = std::clamp (channel + 1, 1, 16);
        if (channel != prev) repaint();
        return true;
    }

    if (code < 0 || code >= (int) held.size())
        return false;

    const int note = noteForKeyCode (code);
    if (note < 0) return false;

    // keyPressed fires again for an already-held key from two sources:
    //   - auto-repeat - the key never left the board; silentScans stays 0
    //     because timerCallback (which uses the reliable X11 physical-state
    //     query) keeps seeing it down. Swallow: don't re-trigger Note On.
    //   - a genuine fast re-press - the key was released and pressed again
    //     before the kReleaseScans debounce fired Note Off; the timer observed
    //     it physically up, so silentScans > 0. Retrigger: Note Off the still-
    //     sounding old note, then Note On the new one.
    if (held[(size_t) code].note >= 0)
    {
        // Retrigger only on CONFIRMED release evidence - the key must have read
        // not-down for at least kReleaseScans-1 consecutive scans. A lone silentScans
        // increment (one stale read) coinciding with an auto-repeat keyPressed must NOT
        // fire a spurious Note Off / Note On on a note that's really still held.
        if (held[(size_t) code].silentScans >= kReleaseScans - 1)
        {
            sendNoteOff (held[(size_t) code].note, held[(size_t) code].channel);
            held[(size_t) code] = { note, channel };
            sendNoteOn (note, velocity, channel);
            repaint();
            return true;
        }
        held[(size_t) code].silentScans = 0;
        return true;
    }

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
        if (isCodePhysicallyDown (code))
        {
            slot.silentScans = 0;
            continue;
        }
        // Not down this scan. Only fire Note Off after it stays not-down for
        // kReleaseScans in a row - cheap guard against a one-off stale read.
        if (++slot.silentScans >= kReleaseScans)
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
    // Clamp rather than mask the channel: masking would wrap an out-of-range
    // one onto a different channel instead of the nearest valid one.
    const std::uint8_t bytes[3] { (std::uint8_t) (0x90 | (std::clamp (chan, 1, 16) - 1)),
                                  (std::uint8_t) (note & 0x7f),
                                  (std::uint8_t) (vel & 0x7f) };
    engine.postVirtualKeyboardMidi (bytes, 3);
    if (onNoteOn) onNoteOn (note, vel, chan);
}

void VirtualKeyboardComponent::sendNoteOff (int note, int chan)
{
    const std::uint8_t bytes[3] { (std::uint8_t) (0x80 | (std::clamp (chan, 1, 16) - 1)),
                                  (std::uint8_t) (note & 0x7f),
                                  0 };
    engine.postVirtualKeyboardMidi (bytes, 3);
    if (onNoteOff) onNoteOff (note, chan);
}

void VirtualKeyboardComponent::shiftCentre (int semitones)
{
    const int prev = centreNote;
    centreNote = std::clamp (centreNote + semitones, 0, 120);
    if (centreNote == prev) return;
    releaseAll();
    appconfig::setVkbCentreNote (centreNote);
    repaint();
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

    const int firstNote = std::max (0, centreNote - 12);
    // One octave of bass context below the centre, then up to the top typed key
    // (centreNote + 28 = the 'P' key in the I9O0P top row) so every typeable
    // note is on-screen + labelled. Must match paint() exactly.
    const int lastNote  = std::min (127, centreNote + 28);
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
    // Then white keys (full rect - black keys above already short-circuited).
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
    // Glissando - release the previous note before triggering the new one.
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

    // Keyboard fills the rest (no footer legend - letters live ON the
    // keys now).
    keyboardArea = bounds;
}

void VirtualKeyboardComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    g.setColour (juce::Colour (0xff1a1a20));
    g.fillRect (bounds);

    // Title strip - text on the left, status text in the middle (between
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

    // Piano keyboard. Show 2 octaves anchored so centreNote sits at the
    // start of the upper octave - one octave of context below (so the
    // user has bass-side reference) and one octave above where the typed
    // Z/Q-row letters land. Centre C2 -> display C1..C3 by default.
    const int firstNote = std::max (0, centreNote - 12);
    // One octave of bass context below the centre, then up to the top typed key
    // (centreNote + 28 = the 'P' key in the I9O0P top row) so every typeable
    // note is on-screen + labelled. Must match noteAtPoint() exactly.
    const int lastNote  = std::min (127, centreNote + 28);
    const int numWhite  = [&] {
        int n = 0;
        for (int m = firstNote; m <= lastNote; ++m)
            if (! isBlackKey (m)) ++n;
        return std::max (1, n);
    }();

    const auto kb = keyboardArea.toFloat();
    const float wkW = kb.getWidth() / (float) numWhite;
    const float wkH = kb.getHeight();
    const float bkW = wkW * 0.62f;
    const float bkH = wkH * 0.62f;

    // Highlight set: notes currently held (any source - typing keyboard OR mouse).
    std::array<bool, 128> isHeld {};
    for (const auto& slot : held)
        if (slot.note >= 0)
            isHeld[(size_t) slot.note] = true;
    if (mouseHeld.note >= 0) isHeld[(size_t) mouseHeld.note] = true;

    // Path with only its bottom corners rounded - the Logic / hardware
    // key silhouette (square shoulders at the top, rounded toe).
    auto bottomRoundedKey = [] (juce::Rectangle<float> r, float radius)
    {
        radius = std::min ({radius, r.getWidth() * 0.5f, r.getHeight() * 0.5f});
        juce::Path p;
        p.startNewSubPath (r.getX(), r.getY());
        p.lineTo (r.getRight(), r.getY());
        p.lineTo (r.getRight(), r.getBottom() - radius);
        p.quadraticTo (r.getRight(), r.getBottom(), r.getRight() - radius, r.getBottom());
        p.lineTo (r.getX() + radius, r.getBottom());
        p.quadraticTo (r.getX(), r.getBottom(), r.getX(), r.getBottom() - radius);
        p.closeSubPath();
        return p;
    };

    const juce::Colour kActive   (0xff4a90d8);   // Logic-style blue
    const juce::Colour kActiveLo (0xff3a78b8);

    // First pass: white keys. Subtle top-to-bottom gradient + a darker
    // toe band, square shoulders, rounded toe - reads like a real key.
    float x = kb.getX();
    for (int m = firstNote; m <= lastNote; ++m)
    {
        if (isBlackKey (m)) continue;

        juce::Rectangle<float> r (x, kb.getY(), wkW - 1.0f, wkH);
        const bool active = isHeld[(size_t) m];
        auto path = bottomRoundedKey (r, 4.0f);

        juce::ColourGradient grad (
            active ? kActive            : juce::Colour (0xffe6e7ea), r.getX(), r.getY(),
            active ? kActiveLo          : juce::Colour (0xfffdfdfe), r.getX(), r.getBottom(),
            false);
        // Brightest band sits ~70% down, like a key catching light.
        grad.addColour (0.62, active ? juce::Colour (0xff5fa0e0) : juce::Colour (0xffffffff));
        g.setGradientFill (grad);
        g.fillPath (path);

        // Toe shadow - a thin darker strip at the very bottom for depth.
        g.setColour ((active ? kActiveLo : juce::Colour (0xffcccdd2)).withAlpha (0.9f));
        g.fillRect (r.getX(), r.getBottom() - 3.0f, r.getWidth(), 3.0f);

        // Hairline edge.
        g.setColour (juce::Colour (0xff303036));
        g.strokePath (path, juce::PathStrokeType (0.8f));

        const int pc = ((m % 12) + 12) % 12;
        auto labelArea = r;

        // Octave label bottom-left on every C, Logic-style.
        if (pc == 0)
        {
            g.setColour (active ? juce::Colour (0xffe8f0fa) : juce::Colour (0xff6a6a72));
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            g.drawText (juce::MidiMessage::getMidiNoteName (m, true, true, 4),
                        r.reduced (4.0f, 3.0f).removeFromBottom (12.0f),
                        juce::Justification::bottomLeft);
        }

        // Typing-keyboard letter on the key.
        const auto letter = letterForNote (m);
        if (letter.isNotEmpty())
        {
            g.setColour (active ? juce::Colour (0xff10283e) : juce::Colour (0xff707078));
            g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
            g.drawText (letter, labelArea.removeFromBottom (32.0f).removeFromTop (18.0f),
                        juce::Justification::centred);
        }
        x += wkW;
    }

    // Second pass: black keys - beveled 3D look (lighter top cap, near-
    // black body, rounded toe), overlaid on the white keys.
    x = kb.getX();
    for (int m = firstNote; m <= lastNote; ++m)
    {
        if (isBlackKey (m))
        {
            const float bx = x - bkW * 0.5f;
            juce::Rectangle<float> r (bx, kb.getY(), bkW, bkH);
            const bool active = isHeld[(size_t) m];
            auto path = bottomRoundedKey (r, 3.0f);

            juce::ColourGradient grad (
                active ? kActiveLo          : juce::Colour (0xff45454d), r.getX(), r.getY(),
                active ? kActive.darker (0.3f) : juce::Colour (0xff0b0b0f), r.getX(), r.getBottom(),
                false);
            grad.addColour (0.10, active ? kActive : juce::Colour (0xff2a2a30));
            g.setGradientFill (grad);
            g.fillPath (path);

            // Glossy top cap - a slightly inset lighter rectangle near
            // the top edge gives the raised, beveled appearance.
            auto cap = r.reduced (r.getWidth() * 0.18f, 0.0f)
                          .withHeight (r.getHeight() * 0.16f)
                          .translated (0.0f, r.getHeight() * 0.06f);
            g.setColour ((active ? juce::Colour (0xff8fc0f0)
                                  : juce::Colour (0xff6a6a74)).withAlpha (0.55f));
            g.fillRoundedRectangle (cap, 1.5f);

            g.setColour (juce::Colour (0xff000000));
            g.strokePath (path, juce::PathStrokeType (0.8f));

            const auto letter = letterForNote (m);
            if (letter.isNotEmpty())
            {
                g.setColour (active ? juce::Colour (0xffeaf2fc) : juce::Colour (0xffc0c0c8));
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
