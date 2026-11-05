#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../foundation/MessageThread.h"
#include "GuiHost.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace duskstudio
{
class Session;
class AudioEngine;
namespace imgui { class DuskPanelWindow; class MiniTimelineView; }

class MiniTimelineStrip final : public juce::Component, private dusk::Timer
{
public:
    MiniTimelineStrip (Session&, AudioEngine&);
    ~MiniTimelineStrip() override;

    void captureMarkerPaintForScenario (bool enabled) { capturePaint = enabled; }
    std::vector<scenario::MiniMarkerPaint> markerPaintForScenario() const;
    bool clickSampleForScenario (std::int64_t);
    bool clickMarkerForScenario (int);
    bool pressKeyForScenario (const std::string& description);

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;

    std::function<void (int)> onMarkerEdit;

private:
    void timerCallback() override;
    void syncWindow();
    void updateState();
    void hideTooltip();
    std::int64_t songEndSamples() const noexcept;

    Session& session;
    AudioEngine& engine;
    struct Tooltip;
    std::unique_ptr<Tooltip> tooltip;
   #if DUSKSTUDIO_HAS_NATIVE_UI
    std::unique_ptr<imgui::DuskPanelWindow> window;
    imgui::MiniTimelineView* view = nullptr;
   #endif
    std::uintptr_t parentHandle = 0;
    bool closing = false, failed = false, capturePaint = false;
    std::string shownTooltip;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MiniTimelineStrip)
};
} // namespace duskstudio
