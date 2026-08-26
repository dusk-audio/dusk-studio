#pragma once

#include "StripModel.h"

#include <DuskWidgets.hpp>

#include <string>

namespace duskspike
{

// Which interaction the frame just asked for, so the shell can drive the pieces that
// belong to the application rather than the strip.
struct StripFrameResult
{
    bool openIoModal      = false;
    bool openInsertMenu   = false;
    ImVec2 insertMenuAt {};
};

class ChannelStripView
{
public:
    StripFrameResult draw (DuskWidgets::Context& ctx, ImVec2 origin, float width, float height,
                           StripParams& params);

    void setEditingName (bool yes) noexcept { editingName = yes; nameFocusPending = yes; }

    // Golden captures need a frame that is the same every run, so the meters stop
    // smoothing and show whatever the source last wrote.
    void setStaticMeters (bool yes) noexcept { staticMeters = yes; }
    bool isEditingName() const noexcept { return editingName; }

    // The shell hides the pointer for the duration of a knob drag, the way the JUCE strip does.
    bool isDragging() const noexcept { return dragging; }

    // Read by the shell's overlay, so a run reports what the strip actually cost.
    int lastWidgetCount = 0;

private:
    DuskWidgets::MeterBallistics inputMeter;
    float displayedGrDb = 0.0f;
    bool dragging = false;
    bool editingName = false;
    bool nameFocusPending = false;
    bool staticMeters = false;
    char nameBuffer[64] = {};
};

} // namespace duskspike
