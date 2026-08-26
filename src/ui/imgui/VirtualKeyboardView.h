#pragma once

#include "DuskPanelWindow.h"

#include <functional>

namespace duskstudio
{
class AudioEngine;

namespace imgui
{
// The typing keyboard as a MIDI note source: two octaves of piano with the
// triggering letter printed on each key, octave and channel steppers, and the notes
// posted into the engine's synthetic Virtual Keyboard input.
//
// noteOn / noteOff fire after each message is queued, which is what step-record into
// the piano roll listens to.
std::unique_ptr<DuskPanelView> makeVirtualKeyboardView (
    AudioEngine& engine,
    std::function<void (int note, int velocity, int channel)> noteOn,
    std::function<void (int note, int channel)> noteOff);
} // namespace imgui
} // namespace duskstudio
