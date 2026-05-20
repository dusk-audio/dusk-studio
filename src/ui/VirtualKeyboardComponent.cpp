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
}

void VirtualKeyboardComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    g.setColour (juce::Colour (0xff1a1a20));
    g.fillRect (bounds);

    // Title strip
    auto titleArea = bounds.removeFromTop (24.0f);
    g.setColour (juce::Colour (0xffd0d0d0));
    g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    g.drawText ("VIRTUAL MIDI KEYBOARD", titleArea.reduced (10.0f, 2.0f),
                juce::Justification::centredLeft);

    // Status strip on the right of the title
    g.setColour (juce::Colour (0xff8090a0));
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    const auto status = juce::String ("CH ") + juce::String (channel)
                      + "   Centre: " + juce::MidiMessage::getMidiNoteName (centreNote, true, true, 4);
    g.drawText (status, titleArea.reduced (10.0f, 2.0f),
                juce::Justification::centredRight);

    // Footer: legend
    auto footer = bounds.removeFromBottom (40.0f);
    g.setColour (juce::Colour (0xff202028));
    g.fillRect (footer);
    g.setColour (juce::Colour (0xff8090a0));
    g.setFont (juce::Font (juce::FontOptions (11.0f)));
    g.drawText ("Z S X D C V G B H N J M  =  C..B (centre octave)    "
                "Q 2 W 3 E R 5 T 6 Y 7 U  =  octave up    "
                "Up/Down: octave   Left/Right: channel",
                footer.reduced (10.0f, 4.0f),
                juce::Justification::centredLeft);

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

    auto kb = bounds;
    const float wkW = kb.getWidth() / (float) numWhite;
    const float wkH = kb.getHeight();
    const float bkW = wkW * 0.62f;
    const float bkH = wkH * 0.62f;

    // Highlight set: notes currently held (any code).
    std::array<bool, 128> isHeld {};
    for (const auto& slot : held)
        if (slot.note >= 0)
            isHeld[(size_t) slot.note] = true;

    // First pass: white keys.
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

        // Mark middle-C and any C with a faint label so the user can orient.
        const int pc = ((m % 12) + 12) % 12;
        if (pc == 0)
        {
            g.setColour (juce::Colour (0xff707078));
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            g.drawText (juce::MidiMessage::getMidiNoteName (m, true, true, 4),
                        r.removeFromBottom (16.0f),
                        juce::Justification::centred);
        }
        x += wkW;
    }

    // Second pass: black keys overlaid. Their x position is the boundary
    // between the two flanking white keys, offset by half-bk-width.
    x = kb.getX();
    for (int m = firstNote; m <= lastNote; ++m)
    {
        if (isBlackKey (m))
        {
            // Black key sits at the right edge of the previous white key.
            const float bx = x - bkW * 0.5f;
            juce::Rectangle<float> r (bx, kb.getY(), bkW, bkH);

            const bool active = isHeld[(size_t) m];
            g.setColour (active ? juce::Colour (0xff5fa0d0).darker (0.20f)
                                : juce::Colour (0xff181820));
            g.fillRoundedRectangle (r, 2.0f);
            g.setColour (juce::Colour (0xff000000));
            g.drawRoundedRectangle (r, 2.0f, 0.6f);
            continue;
        }
        x += wkW;
    }
}
} // namespace duskstudio
