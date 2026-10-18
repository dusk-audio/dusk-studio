#pragma once

#include "DuskPanelWindow.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace duskstudio
{
class AudioEngine;
class Session;

namespace imgui
{
struct PianoRollHost
{
    // These callbacks can run inside a native frame; shell actions that close
    // the window must wait until the frame returns.
    std::function<void (int track, int region)> navigateToRegion;
    std::function<void (int region)> regionChanged;
    std::function<void()> regionsGone;
    std::function<void (bool redo)> undo;
    // Dispatch a known shell binding without offering it back to this view.
    // The window routes ShellShortcut bindings unless the covering keyboard claims them.
    std::function<bool (const KeyChord&)> unhandledKey;
    std::function<bool()> covered;
    std::function<void()> dismissCover;
    std::function<bool (const KeyChord&)> keyboardOwnsKey;
    std::function<bool (ShellShortcut)> keyboardClaimsShortcut;
};

class PianoRollView : public DuskPanelView
{
public:
    virtual void setAvailableSize (float width, float height) = 0;
    virtual int trackIndex() const = 0;
    virtual int regionIndex() const = 0;
    virtual void focusRegion (int region) = 0;
    virtual void followTrack (int track) = 0;
    virtual void followRegionEdits() = 0;
    virtual void showCommittedRegion (int region = -1) = 0;
    virtual void showSwappedTake() = 0;
    virtual void yieldDragToRecordCommit() = 0;
    virtual void stepRecordNoteOn (int note, int velocity) = 0;
    virtual void stepRecordNoteOff (int note) = 0;
    virtual void stepRecordKeyboardClosed() noexcept = 0;
    virtual bool handleShellKey (const std::string& description, bool repeat) = 0;
    virtual bool chaseEnabled() const = 0;

    // The viewport uses design pixels. Lane bounds and points include the native
    // body's origin and scale, as DuskPanelWindow's scenario input expects.
    virtual std::array<double, 4> viewportForScenario() const = 0;
    virtual std::array<int, 4> optionsForScenario() const = 0;
    virtual bool notePointForScenario (std::int64_t tick, int pitch, ImVec2& point) const = 0;
    virtual std::array<double, 4> laneBoundsForScenario (bool cc) const = 0;
    virtual std::array<double, 4> gridBoundsForScenario() const = 0;
    virtual ImVec2 bodyPointForScenario (ImVec2 nativePoint) const = 0;
    virtual float scaleForScenario() const = 0;
    virtual std::array<int, 2> mergeStateForScenario() const = 0;
    virtual std::array<double, 4> controlBoundsForScenario (const std::string& name) const = 0;
    virtual bool popupForScenario() const = 0;
    virtual std::vector<std::string> menuItemsForScenario() const = 0;
    virtual bool wheelForScenario (float delta, bool command, bool shift, bool smooth, bool lane) = 0;
    virtual std::vector<int> selectionForScenario() const = 0;
    virtual std::int64_t editCursorForScenario() const = 0;
};

std::unique_ptr<PianoRollView> makePianoRollView (Session&, AudioEngine&, int track, int region,
                                               PianoRollHost host);
} // namespace imgui
} // namespace duskstudio
