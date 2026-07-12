#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>

namespace duskstudio
{
// Classic analog VU meter - cream face, swept needle, red overload zone past
// 0 VU. Reads one or two std::atomic<float> linear-RMS sources (audio-thread
// integrated at 300 ms tau, matching IEC 60268-17 and the TapeMachine
// plugin's internal VU integrator). The UI thread converts to VU dB
// (20·log10(rms) - referenceDbFs) and runs the mechanical ballistics
// (spring + RC) on the needle position.
//
// Reference level (dBFS that maps to 0 VU) is configurable; default -18 dBFS
// matches the common DAW convention. At 0 dBFS the needle pegs hard right
// (well past +3) which visually communicates clipping the same way an analog
// meter would slam its endstop. Two atoms = stereo (two needles overlaid on
// one face); one atom = single-channel (e.g. master bus summed RMS, or a
// caller that already collapses L/R upstream).
class AnalogVuMeter final : public juce::Component, private juce::Timer
{
public:
    // leftRmsAtom must be non-null; rightRmsAtom may be null for mono. Atoms
    // hold linear RMS amplitude (not dB) so the UI does one log10 + position
    // map per refresh tick rather than per sample. Both pointed-to atoms
    // must outlive this component; in Dusk Studio these live on Session params
    // (BusParams / MasterBusParams), constructed before any UI and destructed
    // after, so the lifetime contract holds implicitly.
    AnalogVuMeter (const std::atomic<float>* leftRmsAtom,
                    const std::atomic<float>* rightRmsAtom = nullptr);
    ~AnalogVuMeter() override;

    // dBFS that maps to 0 VU on the scale. Common values: -18 (broadcast /
    // DAW), -20 (cinema), -14 (loudness-normalised streaming). The face is
    // re-rendered when this changes so the labels remain consistent.
    void setReferenceLevelDb (float refDb);

    // Compact-scale mode for bus / per-channel VUs: drops the numeric scale
    // labels and the "VU" badge regardless of width, shrinks the tick
    // marks, and paints "-" / "+" glyphs at the scale endpoints in the
    // Sifam / Harrison Mixbus style. The face background stays consistent
    // with the master VU so the visual family reads as one unit.
    void setCompactScale (bool compact);

    // Opt-in "rich" photoreal face: warm-cream radial gradient + aging
    // vignette, glass specular sheen, beveled bezel, chrome screw hub, and a
    // tapered shadowed needle. Default OFF = the plain face every meter has
    // shipped with. Enabled only on the master meter for now so the bus meters
    // stay unchanged for an A/B. Triggers a face rebuild.
    void setRichStyle (bool rich);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void rebuildCachedFace();

    const std::atomic<float>* leftRms  = nullptr;
    const std::atomic<float>* rightRms = nullptr;
    float referenceDbFs = -18.0f;
    bool  compactScale  = false;
    bool  richStyle     = false;   // warm-cream photoreal face (master only)

    // Re-used per frame for the tapered needle so paint() stays allocation-free
    // (clear() + rebuild rather than constructing a Path each tick).
    juce::Path needlePath;

    // Display scale the cachedFace was rasterised at, so paint() can blit it
    // back to logical size 1:1 (HiDPI sharpness) and we can rebuild if the
    // window moves to a different-DPI monitor.
    float lastScale = 1.0f;

    // Needle position per channel as an angleFrac in [-1, +1] - -1 is the
    // resting left endstop (-20 VU), +1 is the +3 VU endstop. Mapped to a
    // physical angle by the .cpp's anchor table so the needle motion is
    // non-linear, matching the hardware face's compressed dB spacing.
    // Driven on the UI Timer with IEC 60268-17 ballistics + a damped-
    // spring overshoot.
    float needlePosL = -1.0f;
    float needlePosR = -1.0f;
    float needleVelL = 0.0f;
    float needleVelR = 0.0f;

    // Pre-rendered face. Rebuilt on resize/reference-change; the per-frame
    // paint just blits this and overlays the needle(s). Cheaper than
    // re-rasterising the arcs and labels at 30 Hz.
    juce::Image cachedFace;

    // Needle pivot is below the visible face (off-screen) so the needle
    // sweeps a shallow arc across the top of the meter, matching the
    // mechanics of a real analog galvanometer. Cached at resize.
    juce::Point<float> pivot;
    float arcRadius   = 0.0f;
    float baselineRad = 0.0f;    // where the scale baseline + tick bottoms sit
    float halfArcDeg  = 45.0f;   // each side from straight-up

    // PEAK LED hold deadline (Time::getMillisecondCounter()). 0 = idle.
    // Set by timerCallback when the unclamped VU dB on either channel
    // crosses kPeakTriggerVuDb; paint() lights the LED until now() passes
    // this stamp. Single timestamp covers both channels - one PEAK LED.
    std::uint32_t peakHoldUntilMs = 0;

    // Cached LED geometry computed in rebuildCachedFace so paint() can
    // overlay the lit state without recomputing layout each frame.
    juce::Rectangle<float> peakLedRect;
};
} // namespace duskstudio
