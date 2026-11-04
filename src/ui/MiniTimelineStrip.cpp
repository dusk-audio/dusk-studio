#include "MiniTimelineStrip.h"

#include "../session/Session.h"
#include "../engine/AudioEngine.h"
#include "EmbeddedModal.h"
#include "NativeEditorEmbedScale.h"
#if DUSKSTUDIO_HAS_NATIVE_UI
 #include "imgui/MiniTimelineView.h"
#endif

#include <algorithm>
#include <utility>

namespace duskstudio
{
struct MiniTimelineStrip::Tooltip final : juce::Component
{
    std::string text;

    Tooltip() { setInterceptsMouseClicks (false, false); }
    void paint (juce::Graphics& g) override
    {
        getLookAndFeel().drawTooltip (g, text, getWidth(), getHeight());
    }
};

MiniTimelineStrip::MiniTimelineStrip (Session& s, AudioEngine& e) : session (s), engine (e)
{
    getProperties().set (kPluginEditorTag, true);
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    failed = true;
   #endif
    startTimerHz (30);
}

MiniTimelineStrip::~MiniTimelineStrip()
{
    stopTimer();
   #if DUSKSTUDIO_HAS_NATIVE_UI
    window.reset();
   #endif
}

std::int64_t MiniTimelineStrip::songEndSamples() const noexcept
{
    auto end = engine.getTransport().getPlayhead();
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        for (const auto& r : session.track (t).regions)
            end = std::max (end, r.timelineStart + r.lengthInSamples);
        for (const auto& m : session.track (t).midiRegions.current())
            end = std::max (end, m.timelineStart + m.lengthInSamples);
    }
    for (const auto& marker : session.getMarkers()) end = std::max (end, marker.timelineSamples);
    return std::max (end, static_cast<std::int64_t> (engine.getTimelineSampleRate() * 60.0));
}

void MiniTimelineStrip::updateState()
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    if (view == nullptr || closing) return;
    view->state.end = songEndSamples();
    view->state.playhead = engine.getTransport().getPlayhead();
    view->state.markers.clear();
    for (const auto& marker : session.getMarkers())
        view->state.markers.push_back ({ marker.name.toStdString(), marker.timelineSamples, marker.colour.getARGB() });
   #endif
}

void MiniTimelineStrip::hideTooltip()
{
    if (shownTooltip.empty()) return;
    shownTooltip.clear();
    if (tooltip != nullptr) tooltip->setVisible (false);
}

void MiniTimelineStrip::syncWindow()
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    auto* top = getTopLevelComponent();
    const auto parent = top != nullptr ? embedscale::nativeParentHandle (*top) : 0;
    const bool wanted = isShowing() && ! getLocalBounds().isEmpty() && parent != 0
                     && (int) getProperties().getWithDefault ("dusk_modalHideCount", 0) == 0
                     && ! PluginEditorHider::hasActiveSurface();
    if (! wanted) hideTooltip();
    if (window != nullptr && window->isOpen())
    {
        if (! wanted || parent != parentHandle)
        {
            closing = true;
            window->close();
        }
        return;
    }
    if (! wanted || closing || failed) return;
    window = std::make_unique<imgui::DuskPanelWindow> ("dusk-studio-mini-timeline", "mini-timeline", "Mini timeline");
    juce::Component::SafePointer<MiniTimelineStrip> safe (this);
    imgui::DuskPanelWindow::Callbacks callbacks;
    callbacks.geometry = [safe]
    {
        auto* self = safe.getComponent();
        if (self == nullptr) return imgui::DuskPanelWindow::Geometry {};
        auto* topLevel = self->getTopLevelComponent();
        const auto g = embedscale::childGeometryFor (*topLevel, topLevel->getLocalArea (self, self->getLocalBounds()));
        return imgui::DuskPanelWindow::Geometry { g.x, g.y, g.width, g.height, g.scale };
    };
    callbacks.closed = [safe]
    {
        if (auto* self = safe.getComponent())
        {
            self->view = nullptr;
            self->failed = ! std::exchange (self->closing, false);
            self->hideTooltip();
            self->repaint();
        }
    };
    const auto geometry = callbacks.geometry();
    window->setCallbacks (std::move (callbacks));
    auto content = std::make_unique<imgui::MiniTimelineView>();
    view = content.get();
    view->shellKey = [safe] (std::string description)
    {
        const auto key = juce::KeyPress::createFromDescription (description);
        dusk::callAsync ([safe, key]
        {
            auto* self = safe.getComponent();
            if (self == nullptr || ! self->isShowing() || PluginEditorHider::hasActiveSurface()) return;
            if (auto* shell = self->getParentComponent())
                shell->keyPressed (key);
        });
    };
    view->seek = [this, safe] (std::int64_t sample)
    {
        engine.getTransport().locate (sample);
        // The strip has no keyboard controls; keep the shell's full shortcut set after a click.
        dusk::callAsync ([safe]
        {
            auto* self = safe.getComponent();
            if (self == nullptr || ! self->isShowing() || PluginEditorHider::hasActiveSurface()) return;
            if (auto* peer = self->getPeer()) peer->grabFocus();
            if (auto* shell = self->getParentComponent()) shell->grabKeyboardFocus();
        });
    };
    view->editMarker = [safe] (int index)
    {
        dusk::callAsync ([safe, index]
        {
            if (auto* self = safe.getComponent(); self != nullptr && self->onMarkerEdit)
                self->onMarkerEdit (index);
        });
    };
    view->tooltip = [this] (const std::string& text, float x, float y)
    {
        if (text == shownTooltip) return;
        if (text.empty()) { hideTooltip(); return; }
        auto* shell = getParentComponent();
        if (shell == nullptr) return;
        shownTooltip = text;
        if (tooltip == nullptr) tooltip = std::make_unique<Tooltip>();
        shell->addAndMakeVisible (*tooltip);
        tooltip->text = text;
        const auto point = shell->getLocalPoint (this,
            juce::Point<int> (static_cast<int> (x), static_cast<int> (y)));
        auto bounds = getLookAndFeel().getTooltipBounds (text, point, shell->getLocalBounds());
        const auto strip = shell->getLocalArea (this, getLocalBounds());
        if (bounds.intersects (strip))
        {
            const int below = std::max (0, shell->getHeight() - strip.getBottom() - 2);
            const int above = std::max (0, strip.getY() - 2);
            const bool useBelow = below >= bounds.getHeight() || below >= above;
            bounds.setHeight (std::min (bounds.getHeight(), useBelow ? below : above));
            bounds.setY (useBelow ? strip.getBottom() + 2 : strip.getY() - bounds.getHeight() - 2);
        }
        tooltip->setBounds (bounds);
        tooltip->toFront (false);
        tooltip->repaint();
    };
    updateState();
    window->setView (std::move (content));
    parentHandle = parent;
    if (! window->open (parent, geometry))
    {
        view = nullptr;
        failed = true;
        repaint();
    }
   #endif
}

void MiniTimelineStrip::timerCallback()
{
    syncWindow();
    if (isShowing()) updateState();
}

void MiniTimelineStrip::resized() { hideTooltip(); syncWindow(); }
void MiniTimelineStrip::visibilityChanged() { syncWindow(); }
void MiniTimelineStrip::parentHierarchyChanged() { syncWindow(); }

void MiniTimelineStrip::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff15151a));
    if (failed)
    {
        g.setColour (juce::Colour (0xffe0e0e6));
        g.drawText ("Mini timeline unavailable", getLocalBounds(), juce::Justification::centred, false);
    }
}

std::vector<scenario::MiniMarkerPaint> MiniTimelineStrip::markerPaintForScenario() const
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    if (capturePaint && view != nullptr && ! closing) return view->markerPaint;
   #endif
    return {};
}

bool MiniTimelineStrip::clickSampleForScenario (std::int64_t sample)
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    return window != nullptr && ! closing && window->clickControlForScenario ("sample:" + std::to_string (sample));
   #else
    (void) sample;
    return false;
   #endif
}

bool MiniTimelineStrip::clickMarkerForScenario (int index)
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    return window != nullptr && ! closing && window->clickControlForScenario ("marker:" + std::to_string (index));
   #else
    (void) index;
    return false;
   #endif
}
} // namespace duskstudio
