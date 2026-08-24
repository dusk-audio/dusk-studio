#pragma once

#include "StripModel.h"

#include <DearImGui.hpp>

#include <string>

namespace duskspike
{

struct StripFonts
{
    ImFont* small   = nullptr;  // 8 pt column headers
    ImFont* label   = nullptr;  // 9 pt knob labels
    ImFont* pill    = nullptr;  // 10.5 pt module headers
    ImFont* band    = nullptr;  // 12 pt band / filter labels
    ImFont* name    = nullptr;  // 13 pt strip name
    ImFont* mono    = nullptr;  // 11 pt numeric readouts
    ImFont* monoBig = nullptr;  // 14 pt fader readout
};

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
    StripFrameResult draw (ImDrawList& dl,
                           ImVec2 origin,
                           float width,
                           float height,
                           float scale,
                           StripParams& params,
                           const StripFonts& fonts);

    void setEditingName (bool yes) noexcept { editingName = yes; }
    bool isEditingName() const noexcept { return editingName; }

    // Read by the shell's overlay, so a run reports what the strip actually cost.
    int lastWidgetCount = 0;

private:
    MeterBallistics inputMeter;
    float displayedGrDb = 0.0f;

    std::string activeDrag;
    float dragStartValue = 0.0f;
    bool editingName = false;
    char nameBuffer[64] = {};
};

} // namespace duskspike
