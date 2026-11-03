#include "SystemStatusBar.h"
#include "../dsp/ChordAnalyzer.h"

namespace duskstudio
{
SystemStatusBar::SystemStatusBar (AudioEngine& e) : engine (e)
{
    setOpaque (true);
    chordAnalyzer = std::make_unique<::ChordAnalyzer>();
    // Bump the tick rate so chord readout follows playing at a usable
    // latency. 10 Hz feels responsive without burning paint cycles.
    startTimerHz (10);
    timerCallback();
}

SystemStatusBar::~SystemStatusBar() { stopTimer(); }   // before derived members destruct

void SystemStatusBar::timerCallback()
{
    const double sr = engine.getCurrentSampleRate();
    const int    bs = engine.getCurrentBlockSize();
    // Engine-side CPU usage (smoothed callback wall-time / buffer audio-time).
    // More predictive of xruns than AudioDeviceManager::getCpuUsage which
    // averages over a longer window and includes time spent in JUCE/driver
    // glue we don't control.
    lastReadings = pinnedReadings.value_or (DspReadings { (double) engine.getCpuUsage(),
                                                          engine.getXRunCount(),
                                                          engine.getBackendXRunCount(),
                                                          engine.getRealtimeDemotionCount() });
    lastDspSeverity = dspSeverity (lastReadings);
    lastAudioWarn   = ! engine.hasUsableOutputs() && sr > 0.0;

    if (sr > 0.0 && bs > 0)
    {
        const double latencyMs = 1000.0 * (double) bs / sr;
        const double srKhz = sr / 1000.0;
        // 44.1 kHz / 88.2 kHz get a decimal; 48/96 stay as integers
        const auto srStr = (std::abs (srKhz - std::round (srKhz)) < 0.05)
                            ? juce::String ((int) std::round (srKhz))
                            : juce::String (srKhz, 1);
        audioInfo = "Audio: " + srStr + " kHz  " + juce::String (latencyMs, 1) + " ms";
    }
    else
    {
        audioInfo = "Audio: -";
    }

    // Override with a loud warning when the open device has 0 outputs -
    // engine is silent in that state, and we'd rather the user see this than
    // chase a "broken" engine. Replaces the rate/latency string entirely so
    // the warning isn't lost next to normal-looking telemetry.
    if (! engine.hasUsableOutputs() && sr > 0.0)
        audioInfo = "Audio: NO OUTPUTS - pick another device";

    // Two xrun counts: engine-self-detected (callback overrun) / backend
    // (driver EPIPE on ALSA, JACK xrun callback). They have different fixes
    // - surfacing both lets a glitchy session be diagnosed at a glance.
    dspInfo = "DSP: " + juce::String ((int) std::round (lastReadings.cpuUsage * 100.0)) + "%"
            + " (" + juce::String (lastReadings.engineOverruns) + "/" + juce::String (lastReadings.backendXruns) + ")";
    // Oversampling badge, only above 1x: a session saved at 2x/4x
    // multiplies the whole mixer's DSP cost, and without the badge that
    // cost reads as an inexplicable load spike.
    if (const int ox = engine.getSession().oversamplingFactor.load (std::memory_order_relaxed); ox > 1)
        dspInfo += " @" + juce::String (ox) + "x";
    // Audio threads a stalled plug-in pushed off realtime priority.
    if (lastReadings.realtimeDemotions > 0)
        dspInfo += (" RT " + std::to_string (lastReadings.realtimeDemotions)).c_str();

    // Chord readout. Audio thread maintains heldMidiNotes; we snapshot
    // here, fingerprint to skip re-analysis when nothing changed, and
    // run the analyzer on the message thread (it allocates a vector +
    // set internally). With <2 notes held, just blank - chord names
    // require a triad's worth of context to be musically meaningful.
    auto& s = engine.getSession();
    std::vector<int> heldNotes;
    int fingerprint = 0;
    for (int n = 0; n < Session::kNumMidiNotes; ++n)
    {
        if (s.heldMidiNotes[(size_t) n].load (std::memory_order_relaxed))
        {
            heldNotes.push_back (n);
            // Cheap mix into the fingerprint; collisions are tolerable
            // (worst case = one missed re-analyze on a held chord).
            fingerprint = fingerprint * 131 + (n + 1);
        }
    }
    if (fingerprint != lastHeldFingerprint)
    {
        lastHeldFingerprint = fingerprint;
        if (heldNotes.size() >= 2 && chordAnalyzer != nullptr)
        {
            const auto info = chordAnalyzer->analyze (heldNotes);
            chordInfo = ! info.name.empty()
                ? juce::String (juce::CharPointer_UTF8 ("\xe2\x99\xaa  ")) + juce::String (info.name)
                : juce::String();
        }
        else
        {
            chordInfo = {};
        }
    }

    // The severity is compared separately so the colour repaints when it
    // changes under an unchanged string, e.g. load crossing 0.85 within the
    // same rounded "85%".
    if (audioInfo     != paintedAudioInfo
        || dspInfo    != paintedDspInfo
        || chordInfo  != paintedChordInfo
        || lastAudioWarn != paintedAudioWarn
        || lastDspSeverity != paintedDspSeverity)
    {
        paintedAudioInfo = audioInfo;
        paintedDspInfo   = dspInfo;
        paintedChordInfo = chordInfo;
        paintedAudioWarn = lastAudioWarn;
        paintedDspSeverity = lastDspSeverity;
        repaint();
    }
}

void SystemStatusBar::paint (juce::Graphics& g)
{
    // Fonts are immutable + shareable in JUCE; build them once instead of
    // re-constructing (and re-querying the default mono font name) every paint.
    static const juce::Font telemetryFont (juce::FontOptions (
        juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
    static const juce::Font chordFont (juce::FontOptions (
        juce::Font::getDefaultMonospacedFontName(), 18.0f, juce::Font::bold));

    g.fillAll (juce::Colour (kStatusBarBackground));
    g.setColour (juce::Colour (0xff2a2a32));
    g.drawRect (getLocalBounds(), 1);

    auto bounds = getLocalBounds().reduced (8, 0);
    g.setFont (telemetryFont);

    // DSP info now reads "DSP: 12% (3/0)" at worst - engine/backend xrun
    // pair widens the right column slightly.
    const auto dspBounds = dspSegmentBounds();
    bounds.removeFromRight (dspBounds.getWidth() + 8);  // segment + small gap

    // Chord readout in the middle column. Held-notes-driven; blank when
    // nothing's playing or fewer than 2 notes are held. Bigger font + a
    // touch brighter than the telemetry text so it reads as the
    // performance signal instead of mixing with the system data. Wider
    // bounds to accommodate the larger glyphs (slash chords like "Cmaj7/G"
    // need the room).
    if (chordInfo.isNotEmpty())
    {
        auto chordBounds = bounds.removeFromRight (220);
        bounds.removeFromRight (8);
        g.setColour (juce::Colour (0xffe8e8f0));
        g.setFont (chordFont);
        g.drawText (chordInfo, chordBounds, juce::Justification::centredRight, false);
        // Restore the status-bar font for the audio / DSP segments below.
        g.setFont (telemetryFont);
    }

    // Audio segment goes red when the device opened with no outputs -
    // matches the "NO OUTPUTS" text override applied in timerCallback.
    // Cached on the same tick so colour matches the rendered string.
    g.setColour (lastAudioWarn ? juce::Colour (0xffe05050) : juce::Colour (0xffb0b0b8));
    g.drawText (audioInfo, bounds, juce::Justification::centredLeft, false);

    // The severity comes from the same tick as dspInfo; re-querying the engine
    // here could pair a fresh colour with stale text.
    g.setColour (juce::Colour (dspSeverityColour (lastDspSeverity)));
    g.drawText (dspInfo, dspBounds, juce::Justification::centredRight, false);
}

void SystemStatusBar::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Double-click the DSP readout to zero the xrun counters - lets the
    // user clear a stale warning after fixing whatever caused it and
    // watch for fresh ones.
    if (dspSegmentBounds().contains (e.getPosition()))
    {
        engine.resetXRunCounts();
        timerCallback();   // refresh the readout now, not next tick
    }
}

void SystemStatusBar::pinReadingsForScenario (std::optional<DspReadings> readings)
{
    pinnedReadings = readings;
    timerCallback();
}

std::uint32_t SystemStatusBar::dspPaintForScenario()
{
    const auto image = createComponentSnapshot (dspSegmentBounds());
    const auto channel = [] (std::uint32_t argb, int shift) { return (int) ((argb >> shift) & 0xff); };
    std::uint32_t strongest = kStatusBarBackground;
    int strongestDistance = 0;
    for (int y = 0; y < image.getHeight(); ++y)
        for (int x = 0; x < image.getWidth(); ++x)
        {
            const auto argb = image.getPixelAt (x, y).getARGB();
            int distance = 0;
            for (const int shift : { 16, 8, 0 })
                distance += std::abs (channel (argb, shift) - channel (kStatusBarBackground, shift));
            if (distance > strongestDistance)
            {
                strongestDistance = distance;
                strongest = argb;
            }
        }
    return strongest;
}
} // namespace duskstudio
