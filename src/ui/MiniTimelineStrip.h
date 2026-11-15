#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../foundation/MessageThread.h"
#include "GuiHost.h"
#include "NativeEmbeddedParent.h"

#include <chrono>
#include <cstdint>
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
   #if DUSKSTUDIO_HAS_NATIVE_UI
    std::int64_t contentEndSamples() const noexcept;
    std::uint64_t regionSignature() const noexcept;
   #endif

    Session& session;
    AudioEngine& engine;
    struct Tooltip;
    std::unique_ptr<Tooltip> tooltip;
   #if DUSKSTUDIO_HAS_NATIVE_UI
    std::unique_ptr<imgui::DuskPanelWindow> window;
    imgui::MiniTimelineView* view = nullptr;
   #endif
    platform::EmbeddedChildLatch latch;
    bool closing = false, capturePaint = false;
    std::string shownTooltip;
   #if DUSKSTUDIO_HAS_NATIVE_UI
    std::int64_t contentEnd = 0;
    std::uint64_t scannedRegions = 0;
    double scannedRate = 0.0;
    std::chrono::steady_clock::time_point nextRegionScan {};
   #endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MiniTimelineStrip)
};
} // namespace duskstudio
