#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <utility>
#include "../session/Session.h"

namespace duskstudio
{
// Mixbus-style compressor metering: two vertical LED bars side by side
// (input level on the left, gain reduction on the right) plus a draggable
// threshold marker on the input meter. Replaces the THR knob - drag the
// triangle handle up/down on the input meter to set the threshold relative
// to the live input signal.
class CompMeterStrip final : public juce::Component, private juce::Timer
{
public:
    // Generic data-source hook set. Bus + master strips wire this directly
    // so the same widget renders for any compressor topology. The Track&
    // ctor below builds one of these internally for channel strips.
    struct Source
    {
        std::function<float()>     getInputDb;        // peak post-EQ pre-comp input dB
        std::function<float()>     getGrDb;           // ≤ 0, peak gain reduction dB
        std::function<float()>     getThresholdDb;    // current threshold (drives triangle position)
        std::function<void(float)> setThresholdDb;    // drag writes a clamped value
        std::function<void()>      resetThreshold;    // dbl-click: no-comp reset
        std::function<bool()>      isEngaged;         // controls triangle colour
        std::function<void()>      autoEnable;        // touching threshold auto-enables the comp
        float floorDb   = -60.0f;                     // bottom of IN-meter scale
        // Raised from 0 to +12 so the drag handle can reach the VCA
        // threshold's +12 dB neutral ceiling (compVcaThreshDb range is
        // -38..+12, defaulting to +12 = no compression). OPTO/FET clamp
        // their per-mode params at 0 dB threshold so dragging above 0
        // there is a no-op; only VCA actually uses the extra range.
        float ceilingDb =  12.0f;                     // top of IN-meter scale
    };

    explicit CompMeterStrip (Source src);
    explicit CompMeterStrip (Track& trackRef);
    ~CompMeterStrip() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    // Hides the IN level bar (and its scale + "IN" caption) so the strip
    // becomes a slim handle + GR bar - used by the experimental track-3
    // fader-side layout where the main level meter already shows IN dB
    // and would duplicate the small IN bar inside this widget.
    void setShowInputBar (bool s);

    // Hides the threshold-drag triangle handle so the widget collapses
    // to just the GR bar (master-strip-style slim GR LED). Combined with
    // setShowInputBar(false) this gives a pure GR-only meter that takes
    // the same footprint as the master strip's grMeterArea.
    void setHandleVisible (bool s);

    // Places the triangle handle on the RIGHT side of the GR bar (default
    // is LEFT). Used by the channel-strip fader-side layout where the GR
    // LED hugs the main meter; the right-side handle leaves the GR bar's
    // left edge clean so the meter pair reads as one continuous element.
    void setHandleOnRight (bool right);

    // Override the dB range used by the threshold-drag math + the GR bar.
    // Used by the fader-side layout to make the threshold travel range
    // visually 1:1 with the level meter scale so dragging to e.g. -18 dB
    // lands on the -18 mark of the fader scale.
    //
    // Guards against a degenerate range: dbToFrac / fracToDb divide by
    // (ceiling - floor); if a caller swaps the args or passes equal values
    // we clamp the ceiling up to floor + kMinDbDelta so the downstream math
    // stays defined (handle never sticks at NaN, GR bar still draws).
    void setRangeDb (float floor, float ceiling) noexcept
    {
        constexpr float kMinDbDelta = 0.001f;
        if (ceiling < floor) std::swap (floor, ceiling);
        if (ceiling - floor < kMinDbDelta) ceiling = floor + kMinDbDelta;
        src.floorDb   = floor;
        src.ceilingDb = ceiling;
    }

    // GR bar rect in this component's local coordinates. Parent components
    // use it to align an external GR-scale label column with the bar's
    // vertical extent.
    juce::Rectangle<float> getGrBarArea() const noexcept { return grBarArea; }

    // Per-mode threshold helpers - exposed so parent components can drive
    // threshold drag from a different widget (e.g. the channel strip's
    // main level meter when the compMeter widget runs in slim pure-GR
    // mode). Mirrors the mode-specific mapping used internally by the
    // widget's own triangle drag.
    static void  writeThresholdForMode (Track& t, float threshDb);
    static float readThresholdForMode  (const Track& t);
    static void  resetThresholdForMode (Track& t);

    static constexpr float kFloorDb   = -60.0f;
    static constexpr float kCeilingDb =   0.0f;
    static constexpr float kGrFloorDb = -24.0f;
    static constexpr float kHandleW   = 10.0f;
    static constexpr float kBarW      = 10.0f;

private:
    void timerCallback() override;

    float dbToFrac (float db) const noexcept;
    float fracToDb (float frac) const noexcept;
    float yForDb (float db, juce::Rectangle<float> area) const noexcept;
    float dbForY (int y, juce::Rectangle<float> area) const noexcept;

    Source src;

    // Smoothed display values - updated from the Timer callback so the meter
    // breathes naturally with fast-attack / slow-decay envelopes.
    float displayedInputDb   = -100.0f;
    float inputPeakHoldDb    = -100.0f;
    int   inputPeakHoldFrames = 0;
    float displayedGrDb      = 0.0f;

    // Layout rectangles set in resized().
    juce::Rectangle<float> handleArea;
    juce::Rectangle<float> inputBarArea;
    juce::Rectangle<float> grBarArea;
    juce::Rectangle<float> scaleArea;   // dB-number column (Mixbus-style); empty for narrow strips
    bool hasCaptions = false;            // computed in resized(); paint reads without re-querying bounds

    bool draggingThreshold = false;
    bool showInputBar = true;
    bool showHandle   = true;
    bool handleOnRight = false;
};
} // namespace duskstudio
