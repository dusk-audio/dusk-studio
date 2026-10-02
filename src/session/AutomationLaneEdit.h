#pragma once

#include "Session.h"

#include <cstdint>
#include <vector>

namespace duskstudio
{
// Breakpoint edits made by hand on a lane the audio thread is not reading: the
// caller edits a copy or a stopped-transport lane and publishes the result.

// Mute and Solo store only 0 or 1, the values evaluateLane plays them as, so a
// point sits where it sounds.
float quantizeAutomationValue (AutomationParam param, float v01) noexcept;

// Inserts after any point at the same time and returns the new point's index.
int insertAutomationPoint (std::vector<AutomationPoint>& points, const AutomationPoint& point);

// Moves points[index] to (time, value) and keeps the lane sorted. Returns the
// point's new index, or -1 when index is out of range.
int moveAutomationPoint (std::vector<AutomationPoint>& points, int index, std::int64_t time, float value);

// The pencil's memory between steps of one stroke.
struct AutomationStroke
{
    bool started = false;
    std::int64_t lastTime = 0;
    float lastX = 0.0f;
};

// One step of a freehand stroke at pointer column x. A step lays a point once the
// pointer has moved stepPixels since the last one, replacing the points swept
// since then; a smaller move only retunes the last point's value.
void paintAutomationStep (std::vector<AutomationPoint>& points, AutomationStroke& stroke,
                          std::int64_t time, float value, float bpm, float x, float stepPixels);

// Drawing on a lane makes it play: Off and Write become Read, Read and Touch stay.
AutomationMode automationModeAfterEdit (AutomationMode mode) noexcept;
} // namespace duskstudio
