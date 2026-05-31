#include "AudioRegionEditor.h"
#include "DuskAlerts.h"
#include "DuskContextMenu.h"
#include "EditCursors.h"
#include "EditModeToolbar.h"
#include "FadeCurve.h"
#include "../engine/AudioEngine.h"
#include "../engine/Transport.h"
#include "../session/RegionEditActions.h"
#include "../session/SnapHelpers.h"
#include <cmath>
#include <limits>

namespace duskstudio
{
namespace
{
const juce::Colour kBgDark        { 0xff181820 };
const juce::Colour kHeaderBg      { 0xff202028 };
const juce::Colour kHeaderText    { 0xffb0b0b8 };
const juce::Colour kBarLine       { 0xff5a5a64 };
const juce::Colour kBeatLine      { 0xff3c3c46 };
const juce::Colour kWaveformFill  { 0xff80b0e0 };
const juce::Colour kFadeStroke    { 0xffe0c060 };
const juce::Colour kEditCursor    { 0xffffd060 };
// Soft cream so it reads against the yellow edit cursor without
// competing — same colour the piano roll uses for its transport line.
const juce::Colour kTransportPlayhead { 0xffffe8c0 };

// Region-properties popup colour palette. Mirrors the palette in
// [src/ui/TapeStrip.cpp] so all three surfaces (tape strip menu,
// audio editor properties, piano roll properties) share the same
// 8 swatches + reset.
struct PaletteEntry { const char* label; juce::uint32 argb; };
constexpr PaletteEntry kPalette[] = {
    { "Reset to track colour", 0x00000000 },
    { "Red",     0xffd05f5f }, { "Orange",  0xffd09060 },
    { "Yellow",  0xffd0c060 }, { "Green",   0xff60c070 },
    { "Cyan",    0xff60c0c0 }, { "Blue",    0xff6090d0 },
    { "Purple",  0xff9070c0 }, { "Magenta", 0xffc060a0 },
};
constexpr int kPaletteCount = (int) (sizeof (kPalette) / sizeof (kPalette[0]));
} // namespace

AudioRegionEditor::AudioRegionEditor (Session& s, AudioEngine& e, int t, int r)
    : session (s), engine (e), trackIdx (t), regionIdx (r)
{
    setOpaque (true);
    setWantsKeyboardFocus (true);
    formatManager.registerBasicFormats();

    // Top icon row.
    auto wireIcon = [this] (IconButton& b, const juce::String& tip,
                              std::function<void()> onClick)
    {
        addAndMakeVisible (b);
        b.setTooltip (tip);
        b.onClick = std::move (onClick);
    };
    wireIcon (undoButton,       "Undo",                [this] { engine.getUndoManager().undo(); refreshStatusBarReadouts(); repaint(); });
    wireIcon (redoButton,       "Redo",                [this] { engine.getUndoManager().redo(); refreshStatusBarReadouts(); repaint(); });
    wireIcon (splitButton,      "Split at edit cursor",[this] { splitAtCursor(); });
    wireIcon (normalizeButton,  "Normalize",           [this] { normalizeRegion(); });
    wireIcon (propertiesButton, "Region properties...", [this] { showRegionPropertiesPopup(); });
    wireIcon (zoomOutButton,    "Zoom out (-)",         [this] { zoomByFactor (1.0f / 1.15f); });
    wireIcon (zoomInButton,     "Zoom in (=)",          [this] { zoomByFactor (1.15f); });
    wireIcon (zoomFitButton,    "Zoom to fit region",   [this] { zoomFit(); });

    // Chase: when on AND transport is playing AND the playhead leaves the
    // visible window, scroll the view forward so the playhead stays in
    // sight. Stateless toggle - off by default.
    chaseToggle.setClickingTogglesState (true);
    chaseToggle.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff222226));
    chaseToggle.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff406030));
    chaseToggle.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff909094));
    chaseToggle.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xffe0e0e0));
    chaseToggle.setTooltip ("Scroll the view to follow the playhead when it leaves the visible window");
    addAndMakeVisible (chaseToggle);

    // Edit-mode + snap palette. Lives inline in the icon row band.
    // session.editMode drives both modal mouse handlers and TapeStrip's
    // dispatch, so a pick here is global.
    editModeToolbar = std::make_unique<EditModeToolbar> (engine);
    // Update the mouse cursor the instant the mode changes, without
    // waiting for the next mouse move.
    editModeToolbar->onEditModeChanged = [this] { updateModeCursor(); };
    addAndMakeVisible (editModeToolbar.get());

    // Bottom status-bar readouts.
    auto styleReadout = [] (juce::Label& l, juce::Justification just)
    {
        l.setJustificationType (just);
        l.setColour (juce::Label::textColourId,       juce::Colour (0xffd0d0d8));
        l.setColour (juce::Label::backgroundColourId, juce::Colour (0xff181820));
        l.setColour (juce::Label::outlineColourId,    juce::Colour (0xff3a3a44));
        l.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        l.setEditable (false, false, false);
    };
    styleReadout (positionLabel, juce::Justification::centredLeft);
    styleReadout (gainLabel,     juce::Justification::centredLeft);
    styleReadout (fadeLabel,     juce::Justification::centredLeft);
    styleReadout (infoLabel,     juce::Justification::centredRight);

    // Make gain + fade click-to-edit. Double-click enters edit mode;
    // Enter commits, Esc reverts. Parse is liberal: strips trailing
    // " dB" / " ms" / " / N ms" so the user can retype the displayed
    // string verbatim. Bad input reverts on the next status refresh.
    gainLabel.setEditable (false, true, false);
    gainLabel.setColour (juce::Label::backgroundWhenEditingColourId, juce::Colour (0xff202028));
    gainLabel.setColour (juce::Label::textWhenEditingColourId,        juce::Colours::white);
    gainLabel.setTooltip ("Double-click to type a dB value (range -24 to +12).");
    gainLabel.onTextChange = [this]
    {
        auto* r = region();
        if (r == nullptr) return;
        // Strip non-numeric tail so "3.5 dB" parses to 3.5.
        auto txt = gainLabel.getText().trim().retainCharacters ("0123456789.-+");
        if (txt.isEmpty()) { refreshStatusBarReadouts(); return; }
        const float target = juce::jlimit (-24.0f, 12.0f, (float) txt.getDoubleValue());
        const AudioRegion before = *r;
        AudioRegion after = before;
        after.gainDb = target;
        auto& um = engine.getUndoManager();
        um.beginNewTransaction ("Set region gain");
        um.perform (new RegionEditAction (session, engine, trackIdx, regionIdx, before, after));
        refreshStatusBarReadouts();
        repaint();
    };

    fadeLabel.setEditable (false, true, false);
    fadeLabel.setColour (juce::Label::backgroundWhenEditingColourId, juce::Colour (0xff202028));
    fadeLabel.setColour (juce::Label::textWhenEditingColourId,        juce::Colours::white);
    fadeLabel.setTooltip ("Double-click to type \"IN / OUT\" fade lengths in ms "
                            "(e.g. \"50 / 200\"). Single value sets fade-in only.");
    fadeLabel.onTextChange = [this]
    {
        auto* r = region();
        if (r == nullptr) return;
        const double sr = juce::jmax (1.0, engine.getCurrentSampleRate());
        // Accept "IN / OUT" or "IN" (single number = fade-in only).
        // Slash, comma, or "fade" prefix all tolerated so the user can
        // retype the displayed "fade 50 / 200 ms" verbatim.
        auto raw = fadeLabel.getText().trim().toLowerCase()
                        .replace ("fade", "").replace ("ms", "");
        const int slash = raw.indexOfChar ('/');
        const auto inStr  = (slash < 0) ? raw : raw.substring (0, slash);
        const auto outStr = (slash < 0) ? juce::String() : raw.substring (slash + 1);
        const double inMs  = juce::jmax (0.0, inStr.trim().getDoubleValue());
        const double outMs = juce::jmax (0.0, outStr.trim().getDoubleValue());
        const AudioRegion before = *r;
        AudioRegion after = before;
        const auto maxLen = juce::jmax<juce::int64> (0, r->lengthInSamples);
        after.fadeInSamples  = juce::jlimit<juce::int64> (0, maxLen,
                                                              (juce::int64) std::round (inMs * sr / 1000.0));
        after.fadeOutSamples = juce::jlimit<juce::int64> (0, juce::jmax<juce::int64> (0, maxLen - after.fadeInSamples),
                                                              (juce::int64) std::round (outMs * sr / 1000.0));
        auto& um = engine.getUndoManager();
        um.beginNewTransaction ("Set region fades");
        um.perform (new RegionEditAction (session, engine, trackIdx, regionIdx, before, after));
        refreshStatusBarReadouts();
        repaint();
    };

    // Title line above the bar ruler. Shows region label when set,
    // falling back to the source WAV filename. Editable so the user
    // can rename without opening the Properties popup.
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd0d0d8));
    titleLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xff181820));
    titleLabel.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
    titleLabel.setEditable (false, true, false);
    titleLabel.setColour (juce::Label::backgroundWhenEditingColourId, juce::Colour (0xff202028));
    titleLabel.setColour (juce::Label::textWhenEditingColourId,        juce::Colours::white);
    titleLabel.setTooltip ("Double-click to rename the region. Empty = use filename.");
    titleLabel.onTextChange = [this]
    {
        auto* r = region();
        if (r == nullptr) return;
        auto txt = titleLabel.getText().trim();
        // Don't let the filename fallback persist as the label - if
        // the user accepts the displayed filename verbatim, treat it
        // as "no label" so the fallback continues to track file rename.
        if (txt == r->file.getFileName()) txt = {};
        const AudioRegion before = *r;
        AudioRegion after = before;
        after.label = txt;
        auto& um = engine.getUndoManager();
        um.beginNewTransaction ("Rename region");
        um.perform (new RegionEditAction (session, engine, trackIdx, regionIdx, before, after));
        refreshStatusBarReadouts();
        repaint();
    };
    addAndMakeVisible (titleLabel);

    // Which track this region lives on - the full-screen editor otherwise
    // gives no clue (you see a waveform, not the strip). Tinted with the
    // track colour and non-editable so it reads as identity, not the
    // region's editable label.
    trackNameLabel.setJustificationType (juce::Justification::centredLeft);
    trackNameLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xff181820));
    trackNameLabel.setColour (juce::Label::textColourId, session.track (trackIdx).colour.brighter (0.3f));
    trackNameLabel.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
    trackNameLabel.setMinimumHorizontalScale (0.7f);
    trackNameLabel.setText (session.track (trackIdx).name, juce::dontSendNotification);
    trackNameLabel.setTooltip ("The track this region belongs to.");
    addAndMakeVisible (trackNameLabel);

    addAndMakeVisible (positionLabel);
    addAndMakeVisible (gainLabel);
    addAndMakeVisible (fadeLabel);
    addAndMakeVisible (infoLabel);

    muteToggle.setButtonText ("Mute");
    lockToggle.setButtonText ("Lock");
    auto styleToggle = [] (juce::ToggleButton& t)
    {
        t.setColour (juce::ToggleButton::textColourId, juce::Colour (0xffd0d0d8));
    };
    styleToggle (muteToggle);
    styleToggle (lockToggle);
    muteToggle.onClick = [this]
    {
        auto* r = region();
        if (r == nullptr) return;
        const AudioRegion before = *r;
        AudioRegion after = before;
        after.muted = muteToggle.getToggleState();
        auto& um = engine.getUndoManager();
        um.beginNewTransaction (after.muted ? "Mute region" : "Unmute region");
        um.perform (new RegionEditAction (session, engine, trackIdx, regionIdx, before, after));
        refreshStatusBarReadouts();
        repaint();
    };
    lockToggle.onClick = [this]
    {
        auto* r = region();
        if (r == nullptr) return;
        const AudioRegion before = *r;
        AudioRegion after = before;
        after.locked = lockToggle.getToggleState();
        auto& um = engine.getUndoManager();
        um.beginNewTransaction (after.locked ? "Lock region" : "Unlock region");
        um.perform (new RegionEditAction (session, engine, trackIdx, regionIdx, before, after));
        refreshStatusBarReadouts();
        repaint();
    };
    muteToggle.setMouseClickGrabsKeyboardFocus (false);
    muteToggle.setWantsKeyboardFocus (false);
    lockToggle.setMouseClickGrabsKeyboardFocus (false);
    lockToggle.setWantsKeyboardFocus (false);
    addAndMakeVisible (muteToggle);
    addAndMakeVisible (lockToggle);

    // Automation param picker — short pill button on the icon row.
    // Click pops a menu listing each automatable param (Off + Fader /
    // Pan / Mute / AuxSend1..4). Selection updates automationParam +
    // repaints so the lane overlay re-renders.
    automationParamButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff282830));
    automationParamButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffd0d0d8));
    automationParamButton.setTooltip ("Pick a parameter lane to draw automation on.");
    automationParamButton.setMouseClickGrabsKeyboardFocus (false);
    automationParamButton.setWantsKeyboardFocus (false);
    automationParamButton.onClick = [this] { showAutomationParamMenu(); };
    addAndMakeVisible (automationParamButton);

    // Horizontal scrollbar above the status bar. Sized to anchor-range
    // total samples; the visible-range thumb width tracks the on-screen
    // sample window (anchor / pixelsPerSample). Drag scrolls the view.
    horizontalScrollBar.setAutoHide (false);
    horizontalScrollBar.addListener (this);
    addAndMakeVisible (horizontalScrollBar);

    refreshStatusBarReadouts();

    // 30 Hz playhead poll - matches the meter cadence elsewhere. The
    // tick is cheap when transport is stopped (no repaint) so leaving
    // it running unconditionally keeps the start-of-playback latency
    // at one tick (~33 ms) without burning CPU at idle.
    startTimerHz (30);

    // Force default-cursored child labels / buttons to defer to this
    // editor's stored cursor — without this the JUCE default null-handle
    // cursor (which compares as NormalCursor, NOT ParentCursor)
    // silently shadows the editor's Grab / Cut cursor any time the
    // mouse hovers a label or readout sitting over the waveform.
    inheritCursorOnDescendants (*this);
}

AudioRegionEditor::~AudioRegionEditor()
{
    stopTimer();
    // ChangeListener removal is handled by AudioThumbnail's destructor
    // (it removes its listeners on the broadcast list it owns).
    if (thumb != nullptr) thumb->removeChangeListener (this);
}

AudioRegion* AudioRegionEditor::region()
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return nullptr;
    auto& v = session.track (trackIdx).regions;
    if (regionIdx < 0 || regionIdx >= (int) v.size()) return nullptr;
    return &v[(size_t) regionIdx];
}

const AudioRegion* AudioRegionEditor::region() const
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return nullptr;
    const auto& v = session.track (trackIdx).regions;
    if (regionIdx < 0 || regionIdx >= (int) v.size()) return nullptr;
    return &v[(size_t) regionIdx];
}

void AudioRegionEditor::rebuildThumbIfNeeded()
{
    const auto* r = region();
    if (r == nullptr) return;
    if (thumb != nullptr && r->file == loadedFile) return;

    if (thumb != nullptr) thumb->removeChangeListener (this);
    thumb = std::make_unique<juce::AudioThumbnail> (512, formatManager, thumbCache);
    if (r->file.existsAsFile())
        thumb->setSource (new juce::FileInputSource (r->file));
    else
        thumb->setSource (nullptr);
    thumb->addChangeListener (this);
    loadedFile = r->file;

    // Cache the WAV's native sample rate so the bar grid + thumbnail
    // time-domain agree. One-shot reader open just to read the metadata
    // header (no audio data read); destructor closes it before we leave.
    loadedFileSampleRate = 0.0;
    if (r->file.existsAsFile())
    {
        std::unique_ptr<juce::AudioFormatReader> reader (
            formatManager.createReaderFor (r->file));
        if (reader != nullptr)
            loadedFileSampleRate = reader->sampleRate;
    }
}

void AudioRegionEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshStatusBarReadouts();
    repaint();
}

void AudioRegionEditor::resized()
{
    layoutIconRow   (juce::Rectangle<int> (0, 0, getWidth(), kIconRowHeight));
    layoutStatusBar (juce::Rectangle<int> (0, getHeight() - kStatusBarH,
                                                getWidth(), kStatusBarH));
    horizontalScrollBar.setBounds (0, getHeight() - kStatusBarH - kScrollBarH,
                                     getWidth(), kScrollBarH);

    const auto* r = region();
    if (r == nullptr) return;
    const auto waveArea = juce::Rectangle<int> (
        0, kIconRowHeight + kRulerHeight,
        getWidth(),
        getHeight() - kIconRowHeight - kRulerHeight - kStatusBarH - kScrollBarH);
    zoomFitToArea (waveArea);
    syncScrollBarRange();

    // Re-apply cursor inheritance — covers any child added or relaid out
    // after the ctor (the dynamic IconRow + status-bar widgets all
    // reposition here).
    inheritCursorOnDescendants (*this);
}

void AudioRegionEditor::syncScrollBarRange()
{
    if (pixelsPerSample <= 0.0f || anchorTimelineLength <= 0) return;
    const double viewSamples = (double) getWidth() / (double) pixelsPerSample;
    horizontalScrollBar.setRangeLimits (0.0, (double) anchorTimelineLength,
                                          juce::dontSendNotification);
    horizontalScrollBar.setCurrentRange ((double) scrollSamples, viewSamples,
                                            juce::dontSendNotification);
}

void AudioRegionEditor::scrollBarMoved (juce::ScrollBar* bar, double newRangeStart)
{
    if (bar != &horizontalScrollBar) return;
    const juce::int64 viewSamples = (pixelsPerSample > 0.0f)
        ? (juce::int64) std::round ((double) getWidth() / pixelsPerSample)
        : anchorTimelineLength;
    const juce::int64 maxStart = juce::jmax<juce::int64> (0,
                                    anchorTimelineLength - viewSamples);
    scrollSamples = juce::jlimit<juce::int64> (0, maxStart,
                                                  (juce::int64) newRangeStart);
    repaint();
}

void AudioRegionEditor::zoomFitToArea (juce::Rectangle<int> area)
{
    const auto* r = region();
    if (r == nullptr || r->lengthInSamples <= 0) return;
    // Pin the anchor range to the focused region's CURRENT timeline
    // span. Splits later mutate region.lengthInSamples but anchor
    // stays put -> the waveform doesn't shift on screen, cuts just
    // add visible boundary lines.
    anchorTimelineStart  = r->timelineStart;
    anchorTimelineLength = r->lengthInSamples;
    const float w = (float) juce::jmax (1, area.getWidth() - 8);
    pixelsPerSample = w / (float) anchorTimelineLength;
    scrollSamples   = 0;
    editCursorSample = r->sourceOffset;
}

int AudioRegionEditor::xForTimelineSample (juce::int64 timelineSample,
                                              juce::Rectangle<int> area) const
{
    const auto rel = timelineSample - anchorTimelineStart - scrollSamples;
    return area.getX() + 4 + (int) std::round ((double) rel * pixelsPerSample);
}

juce::int64 AudioRegionEditor::timelineSampleForX (int x, juce::Rectangle<int> area) const
{
    if (pixelsPerSample <= 0.0f) return anchorTimelineStart;
    const auto rel = (juce::int64) std::round (
        (double) (x - area.getX() - 4) / pixelsPerSample);
    return anchorTimelineStart + scrollSamples + rel;
}

int AudioRegionEditor::xForSample (juce::int64 absSample,
                                      juce::Rectangle<int> area) const
{
    // File-sample form: convert through the focused region's
    // sourceOffset -> timelineStart mapping, then forward to the
    // timeline-domain primitive. Keeps existing call sites (fade
    // discs, trim strips, edit cursor) working unchanged.
    const auto* r = region();
    if (r == nullptr) return area.getX();
    const auto t = r->timelineStart + (absSample - r->sourceOffset);
    return xForTimelineSample (t, area);
}

juce::int64 AudioRegionEditor::snapFileSampleToGrid (juce::int64 fileSample,
                                                       bool bypass) const noexcept
{
    if (bypass || ! session.snapToGrid) return fileSample;
    const auto* r = region();
    if (r == nullptr) return fileSample;
    const auto fileToTimeline = r->timelineStart - r->sourceOffset;
    const auto timelineSample = fileSample + fileToTimeline;
    const auto sr = juce::jmax (1.0,
        loadedFileSampleRate > 0.0 ? loadedFileSampleRate
                                    : engine.getCurrentSampleRate());
    const auto snapped = snap::snapAbsoluteToGrid (timelineSample, session, sr);
    return snapped - fileToTimeline;
}

juce::int64 AudioRegionEditor::snapTimelineSampleToGrid (juce::int64 timelineSample,
                                                          bool bypass) const noexcept
{
    if (bypass || ! session.snapToGrid) return timelineSample;
    const auto sr = juce::jmax (1.0,
        loadedFileSampleRate > 0.0 ? loadedFileSampleRate
                                    : engine.getCurrentSampleRate());
    return snap::snapAbsoluteToGrid (timelineSample, session, sr);
}

juce::int64 AudioRegionEditor::sampleForX (int x, juce::Rectangle<int> area) const
{
    const auto* r = region();
    if (r == nullptr) return 0;
    const auto t = timelineSampleForX (x, area);
    const auto fileSample = r->sourceOffset + (t - r->timelineStart);
    return juce::jlimit<juce::int64> (
        r->sourceOffset,
        r->sourceOffset + r->lengthInSamples,
        fileSample);
}

int AudioRegionEditor::regionIndexAtX (int x, juce::Rectangle<int> area) const
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return -1;
    const auto t = timelineSampleForX (x, area);
    const auto& regs = session.track (trackIdx).regions;
    for (int i = 0; i < (int) regs.size(); ++i)
    {
        const auto& reg = regs[(size_t) i];
        if (t >= reg.timelineStart
            && t < reg.timelineStart + reg.lengthInSamples)
            return i;
    }
    return -1;
}

void AudioRegionEditor::paint (juce::Graphics& g)
{
    rebuildThumbIfNeeded();

    g.fillAll (kBgDark);

    // Top icon row band - flat fill; icons paint themselves.
    const auto iconRowArea = juce::Rectangle<int> (0, 0, getWidth(), kIconRowHeight);
    g.setColour (juce::Colour (0xff20202c));
    g.fillRect (iconRowArea);
    g.setColour (kBarLine);
    g.drawHorizontalLine (iconRowArea.getBottom() - 1,
                            (float) iconRowArea.getX(), (float) iconRowArea.getRight());

    const auto rulerArea = juce::Rectangle<int> (0, kIconRowHeight,
                                                    getWidth(), kRulerHeight);
    const auto waveArea  = juce::Rectangle<int> (
        0, kIconRowHeight + kRulerHeight,
        getWidth(),
        getHeight() - kIconRowHeight - kRulerHeight - kStatusBarH - kScrollBarH);

    // Bottom status-bar band background - children paint over.
    const auto statusArea = juce::Rectangle<int> (
        0, getHeight() - kStatusBarH, getWidth(), kStatusBarH);
    g.setColour (juce::Colour (0xff181820));
    g.fillRect (statusArea);
    g.setColour (kBarLine);
    g.drawHorizontalLine (statusArea.getY(),
                            (float) statusArea.getX(), (float) statusArea.getRight());

    paintRuler         (g, rulerArea);
    paintWaveform      (g, waveArea);
    paintFadeEnvelopes (g, waveArea);

    // Visible drag affordances - painted under the edit cursor so the
    // cursor always wins on top. Handles are intentionally bold against
    // the dark waveform background; subtle styling reads as "decoration".
    if (region() != nullptr)
    {
        // Gain line - 2 px green horizontal, full width of wave area.
        const int gy = gainLineY (waveArea);
        g.setColour (juce::Colour (0xff80e070));
        g.fillRect (waveArea.getX() + 4, gy - 1,
                     waveArea.getWidth() - 8, 2);
        // Centre drag chip - oversized so the user has a clear grab target.
        g.setColour (juce::Colour (0xff80ff70));
        g.fillRoundedRectangle ((float) (waveArea.getCentreX() - 22),
                                   (float) (gy - 5), 44.0f, 10.0f, 2.0f);
        g.setColour (juce::Colour (0xff182018));
        g.drawRoundedRectangle ((float) (waveArea.getCentreX() - 22),
                                   (float) (gy - 5), 44.0f, 10.0f, 2.0f, 1.2f);
        g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
        const auto db = juce::String (region()->gainDb, 1) + " dB";
        g.drawText (db, waveArea.getCentreX() - 22, gy - 6, 44, 12,
                     juce::Justification::centred, false);

        // Fade-in handle - 18 px yellow disc at the current fadeIn end.
        // Painted as filled ellipse with a strong dark outline so it
        // pops against the waveform.
        auto paintFadeDisc = [&] (juce::Rectangle<int> r)
        {
            if (r.isEmpty()) return;
            const auto rf = r.toFloat();
            g.setColour (juce::Colour (0xffffd040));
            g.fillEllipse (rf);
            g.setColour (juce::Colour (0xff181820));
            g.drawEllipse (rf, 2.0f);
        };
        paintFadeDisc (fadeInHandleRect  (waveArea));
        paintFadeDisc (fadeOutHandleRect (waveArea));

        // Trim grab strips - subtle 3 px bands at the focused slice's
        // edges. Reaper-style: barely visible until you hover them so
        // the slice boundary doesn't look like a hard wall. Hit-test
        // rect stays the original 8 px width (see trimStartRect /
        // trimEndRect) so the grab target isn't tiny.
        g.setColour (juce::Colour (0xff707078).withAlpha (0.5f));
        {
            auto ts = trimStartRect (waveArea);
            auto te = trimEndRect   (waveArea);
            ts.setWidth (3); ts.translate (3, 0);
            te.setWidth (3); te.translate (2, 0);
            g.fillRect (ts);
            g.fillRect (te);
        }
    }

    // Range band painted BELOW the cursor / playhead so the
    // active edit indicators stay visible on top.
    if (rangeActive && region() != nullptr)
    {
        const int xa = xForSample (juce::jmin (rangeStartSample, rangeEndSample), waveArea);
        const int xb = xForSample (juce::jmax (rangeStartSample, rangeEndSample), waveArea);
        if (xb > xa)
        {
            g.setColour (juce::Colour (0xffffd060).withAlpha (0.18f));
            g.fillRect (xa, waveArea.getY(), xb - xa, waveArea.getHeight());
            g.setColour (juce::Colour (0xffffd060).withAlpha (0.6f));
            g.drawVerticalLine (xa, (float) waveArea.getY(), (float) waveArea.getBottom());
            g.drawVerticalLine (xb, (float) waveArea.getY(), (float) waveArea.getBottom());
        }
    }
    paintEditCursor    (g, waveArea);
    paintTransportPlayhead (g, waveArea);
    paintAutomationOverlay (g, waveArea);

    // Snap guide - thin vertical line at the snapped target of the
    // active drag. Painted last so it sits on top of every overlay.
    if (snapGuideTimelineSample >= 0)
    {
        const int gx = xForTimelineSample (snapGuideTimelineSample, waveArea);
        if (gx >= waveArea.getX() && gx < waveArea.getRight())
        {
            g.setColour (juce::Colour (0xff80c0ff).withAlpha (0.85f));
            g.drawVerticalLine (gx, (float) waveArea.getY(),
                                 (float) waveArea.getBottom());
        }
    }

    // Stale-region guard: if the indices fell out of range (e.g. user
    // deleted the region while the editor was open), paint a hint.
    if (region() == nullptr)
    {
        g.setColour (kHeaderText);
        g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::plain)));
        g.drawText ("region unavailable", getLocalBounds(),
                     juce::Justification::centred, false);
    }
}

void AudioRegionEditor::paintRuler (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (kHeaderBg);
    g.fillRect (area);
    g.setColour (kBarLine);
    g.drawHorizontalLine (area.getBottom() - 1,
                            (float) area.getX(), (float) area.getRight());

    const auto* r = region();
    if (r == nullptr || r->lengthInSamples <= 0) return;
    // The bar grid + the "time" ruler must speak in the WAV's OWN
    // sample-rate domain — region.timelineStart / lengthInSamples were
    // stored at recording-time SR, and the rendered waveform uses the
    // WAV file's native SR via the thumbnail. Using engine.getCurrentSampleRate()
    // here drifts the grid against the audio whenever the user hot-swaps
    // to a device at a different rate. Fall back to engine SR when the
    // WAV's SR hasn't been cached yet (thumb not loaded).
    const double sr = juce::jmax (1.0,
        loadedFileSampleRate > 0.0 ? loadedFileSampleRate
                                    : engine.getCurrentSampleRate());

    g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));

    const auto mode = (TimeDisplayMode) session.timeDisplayMode.load (std::memory_order_relaxed);
    if (mode == TimeDisplayMode::Bars)
    {
        const double bpm = juce::jmax (1.0, (double) session.tempoBpm.load (std::memory_order_relaxed));
        const int beatsPerBar = juce::jmax (1, session.beatsPerBar.load (std::memory_order_relaxed));
        const double samplesPerBeat = sr * 60.0 / bpm;
        const double samplesPerBar  = samplesPerBeat * beatsPerBar;

        // Iterate bars across the WHOLE anchor range so splits inside
        // the editor still paint a continuous ruler. anchorTimeline*
        // is captured at open and stays fixed under splits.
        const double anchorStart = (double) anchorTimelineStart;
        const double anchorEnd   = anchorStart + (double) anchorTimelineLength;
        const double firstBar = std::floor (anchorStart / samplesPerBar);
        const double lastBar  = std::ceil  (anchorEnd   / samplesPerBar);

        for (double bar = firstBar; bar <= lastBar; bar += 1.0)
        {
            const auto barTimeline = (juce::int64) std::round (bar * samplesPerBar);
            if (barTimeline < anchorTimelineStart
                || barTimeline > anchorTimelineStart + anchorTimelineLength) continue;
            const int x = xForTimelineSample (barTimeline, area);
            if (x < area.getX() || x > area.getRight()) continue;
            g.setColour (kBarLine);
            g.drawVerticalLine (x, (float) area.getY(), (float) area.getBottom());
            g.setColour (kHeaderText);
            g.drawText (juce::String ((int) bar + 1),
                         juce::Rectangle<int> (x + 3, area.getY(), 40, area.getHeight()),
                         juce::Justification::centredLeft, false);
        }
    }
    else
    {
        // Time mode - tick across whole anchor range, same density
        // the tape strip uses (1 / 5 / 10 / 30 s steps).
        const double pxPerSec = (double) pixelsPerSample * sr;
        double tickEverySec = 1.0;
        if      (pxPerSec < 6.0)  tickEverySec = 30.0;
        else if (pxPerSec < 16.0) tickEverySec = 10.0;
        else if (pxPerSec < 40.0) tickEverySec = 5.0;

        const double anchorStartSec = (double) anchorTimelineStart / sr;
        const double anchorEndSec   = anchorStartSec + (double) anchorTimelineLength / sr;
        const double firstSec = std::floor (anchorStartSec / tickEverySec) * tickEverySec;

        for (double sec = firstSec; sec <= anchorEndSec; sec += tickEverySec)
        {
            const auto secTimeline = (juce::int64) std::round (sec * sr);
            if (secTimeline < anchorTimelineStart
                || secTimeline > anchorTimelineStart + anchorTimelineLength) continue;
            const int x = xForTimelineSample (secTimeline, area);
            if (x < area.getX() || x > area.getRight()) continue;
            g.setColour (kBarLine);
            g.drawVerticalLine (x, (float) area.getY(), (float) area.getBottom());
            g.setColour (kHeaderText);
            const int mins = (int) (sec / 60.0);
            const int secs = (int) sec % 60;
            g.drawText (juce::String::formatted ("%d:%02d", mins, secs),
                         juce::Rectangle<int> (x + 3, area.getY(), 60, area.getHeight()),
                         juce::Justification::centredLeft, false);
        }
    }
}

void AudioRegionEditor::paintWaveform (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto* focused = region();
    if (focused == nullptr || thumb == nullptr) return;

    // Waveform thumbnail expects seconds in the FILE'S native SR domain.
    // Use the cached WAV SR so a hot-swap of the audio device doesn't
    // stretch the waveform vs the bar grid above. Fall back to engine
    // SR before the thumb has loaded its metadata.
    const double sr = juce::jmax (1.0,
        loadedFileSampleRate > 0.0 ? loadedFileSampleRate
                                    : engine.getCurrentSampleRate());
    auto inset = area.reduced (4, 4);
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.fillRect (inset);

    // Neighborhood view: iterate every region on this track and paint
    // any whose timeline span intersects the anchor range. Slices
    // sharing the focused file render via the cached thumbnail at
    // their own sourceOffset / length. The focused slice gets full
    // brightness; the others are dimmed so the user can still see
    // them but knows what edits apply to.
    const auto anchorEnd = anchorTimelineStart + anchorTimelineLength;
    const auto& regs = session.track (trackIdx).regions;

    for (int i = 0; i < (int) regs.size(); ++i)
    {
        const auto& reg = regs[(size_t) i];
        if (reg.lengthInSamples <= 0) continue;
        const auto regEnd = reg.timelineStart + reg.lengthInSamples;
        if (regEnd <= anchorTimelineStart || reg.timelineStart >= anchorEnd) continue;
        if (reg.file != loadedFile) continue;  // different audio source, can't share thumbnail

        const int xa = xForTimelineSample (reg.timelineStart, area);
        const int xb = xForTimelineSample (regEnd, area);
        if (xb <= xa) continue;

        const auto slice = juce::Rectangle<int> (
            juce::jmax (xa, inset.getX()),
            inset.getY(),
            juce::jmin (xb, inset.getRight()) - juce::jmax (xa, inset.getX()),
            inset.getHeight());
        if (slice.getWidth() <= 0) continue;

        // Per-slice background tint so unfocused slices read as
        // separate clips.
        const bool isFocused = (i == regionIdx);
        if (! isFocused)
        {
            g.setColour (juce::Colour (0xff181820).withAlpha (0.6f));
            g.fillRect (slice);
        }

        const auto sliceColour = (reg.customColour.isTransparent()
                                      ? kWaveformFill
                                      : reg.customColour);
        g.setColour (sliceColour.withMultipliedBrightness (isFocused ? 1.05f : 0.55f)
                                  .withAlpha (isFocused ? 1.0f : 0.85f));

        if (thumb->isFullyLoaded() || thumb->getNumChannels() > 0)
        {
            const double t0 = (double) reg.sourceOffset / sr;
            const double t1 = t0 + (double) reg.lengthInSamples / sr;
            // Clip so we only draw inside the slice's x-range even when
            // it overflows the inset (e.g. region partially outside the
            // anchor window).
            juce::Graphics::ScopedSaveState saved (g);
            g.reduceClipRegion (slice);
            thumb->drawChannels (g,
                                  juce::Rectangle<int> (xa, inset.getY(),
                                                          xb - xa, inset.getHeight()),
                                  t0, t1, 0.95f);
        }
    }

    // Slice boundary lines: subtle dark stroke at each region's edge.
    // Reaper-style — visible enough to identify a cut but not so loud
    // it competes with the waveform itself.
    g.setColour (juce::Colour (0xff404048).withAlpha (0.55f));
    for (const auto& reg : regs)
    {
        if (reg.lengthInSamples <= 0) continue;
        if (reg.file != loadedFile) continue;
        const auto regEnd = reg.timelineStart + reg.lengthInSamples;
        if (reg.timelineStart > anchorTimelineStart
            && reg.timelineStart < anchorEnd)
        {
            const int x = xForTimelineSample (reg.timelineStart, area);
            g.drawVerticalLine (x, (float) inset.getY(), (float) inset.getBottom());
        }
        if (regEnd > anchorTimelineStart && regEnd < anchorEnd)
        {
            const int x = xForTimelineSample (regEnd, area);
            g.drawVerticalLine (x, (float) inset.getY(), (float) inset.getBottom());
        }
    }

    // Centre rule (zero amplitude reference).
    g.setColour (kBeatLine.withAlpha (0.6f));
    g.drawHorizontalLine (inset.getCentreY(),
                            (float) inset.getX(), (float) inset.getRight());

    // Multi-selection highlight: faint accent overlay over every region
    // index in additionalSelectedRegions. The focused regionIdx already
    // reads bright via the per-slice paint; this band differentiates the
    // SECONDARY selection so the user knows which slices a group op
    // (delete / move / nudge) would hit.
    if (! additionalSelectedRegions.empty())
    {
        g.setColour (juce::Colour (0xff80c0ff).withAlpha (0.18f));
        for (int idx : additionalSelectedRegions)
        {
            if (idx < 0 || idx >= (int) regs.size()) continue;
            const auto& reg = regs[(size_t) idx];
            if (reg.lengthInSamples <= 0) continue;
            const auto regEnd = reg.timelineStart + reg.lengthInSamples;
            const auto a = juce::jmax (reg.timelineStart, anchorTimelineStart);
            const auto b = juce::jmin (regEnd, anchorEnd);
            if (b <= a) continue;
            const int xa = xForTimelineSample (a, area);
            const int xb = xForTimelineSample (b, area);
            if (xb > xa)
                g.fillRect (xa, inset.getY(), xb - xa, inset.getHeight());
        }
    }
}

void AudioRegionEditor::overlapNeighbours (juce::int64& overlapPrev,
                                             juce::int64& overlapNext,
                                             FadeShape& prevOutShape,
                                             FadeShape& nextInShape) const
{
    overlapPrev = overlapNext = 0;
    prevOutShape = nextInShape = FadeShape::EqualPower;
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return;

    const auto& regs = session.track (trackIdx).regions;
    if (regionIdx < 0 || regionIdx >= (int) regs.size()) return;

    std::vector<int> order;
    order.reserve (regs.size());
    for (int i = 0; i < (int) regs.size(); ++i) order.push_back (i);
    std::sort (order.begin(), order.end(), [&] (int a, int b)
    {
        return regs[(size_t) a].timelineStart < regs[(size_t) b].timelineStart;
    });

    auto overlapLen = [&] (const AudioRegion& a, const AudioRegion& b) -> juce::int64
    {
        const auto aEnd = a.timelineStart + a.lengthInSamples;
        if (aEnd <= b.timelineStart) return 0;
        return juce::jmin (aEnd - b.timelineStart,
                            juce::jmin (a.lengthInSamples, b.lengthInSamples));
    };

    int pos = -1;
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] == regionIdx) { pos = (int) i; break; }
    if (pos < 0) return;

    const auto& self = regs[(size_t) regionIdx];
    if (pos > 0)
    {
        const auto& prev = regs[(size_t) order[(size_t) pos - 1]];
        overlapPrev  = overlapLen (prev, self);
        prevOutShape = prev.fadeOutShape;
    }
    if (pos + 1 < (int) order.size())
    {
        const auto& next = regs[(size_t) order[(size_t) pos + 1]];
        overlapNext = overlapLen (self, next);
        nextInShape = next.fadeInShape;
    }
}

void AudioRegionEditor::paintFadeEnvelopes (juce::Graphics& g,
                                              juce::Rectangle<int> area)
{
    const auto* r = region();
    if (r == nullptr || r->lengthInSamples <= 0) return;
    auto inset = area.reduced (4, 4);

    const auto curveCol = kFadeStroke.withAlpha (0.9f);
    const auto fillCol  = juce::Colour (0x18ffffff);   // subtle crossfade tint

    // Single-region fade curve (no overlapping neighbour).
    auto strokeFade = [&] (int xStart, int xEnd, FadeShape shape, bool isFadeIn)
    {
        if (xEnd <= xStart) return;
        const juce::Rectangle<float> zone ((float) xStart, (float) inset.getY(),
                                            (float) (xEnd - xStart), (float) inset.getHeight());
        g.setColour (curveCol);
        g.strokePath (fadeviz::buildFadeCurve (zone, shape, isFadeIn),
                       juce::PathStrokeType (1.4f));
    };

    juce::int64 overlapPrev = 0, overlapNext = 0;
    FadeShape prevOutShape = FadeShape::EqualPower, nextInShape = FadeShape::EqualPower;
    overlapNeighbours (overlapPrev, overlapNext, prevOutShape, nextInShape);

    if (r->fadeInSamples > 0)
    {
        const int xStart = xForSample (r->sourceOffset, area);
        const int xEnd   = xForSample (r->sourceOffset + r->fadeInSamples, area);
        const juce::Rectangle<float> zone ((float) xStart, (float) inset.getY(),
                                            (float) (xEnd - xStart), (float) inset.getHeight());
        if (overlapPrev > 0)
            // Crossfade: this region's fade-in rises while the previous
            // region's fade-out falls across the same zone -> "X".
            fadeviz::drawCrossfade (g, zone, prevOutShape, r->fadeInShape, curveCol, fillCol);
        else
            strokeFade (xStart, xEnd, r->fadeInShape, /*isFadeIn*/ true);
    }
    if (r->fadeOutSamples > 0)
    {
        const int xStart = xForSample (r->sourceOffset + r->lengthInSamples - r->fadeOutSamples, area);
        const int xEnd   = xForSample (r->sourceOffset + r->lengthInSamples, area);
        const juce::Rectangle<float> zone ((float) xStart, (float) inset.getY(),
                                            (float) (xEnd - xStart), (float) inset.getHeight());
        if (overlapNext > 0)
            fadeviz::drawCrossfade (g, zone, r->fadeOutShape, nextInShape, curveCol, fillCol);
        else
            strokeFade (xStart, xEnd, r->fadeOutShape, /*isFadeIn*/ false);
    }
}

void AudioRegionEditor::paintEditCursor (juce::Graphics& g,
                                            juce::Rectangle<int> area)
{
    const int cx = xForSample (editCursorSample, area);
    if (cx < area.getX() - 1 || cx > area.getRight() + 1) return;

    g.setColour (kEditCursor.withAlpha (0.7f));
    g.drawVerticalLine (cx, (float) area.getY(), (float) area.getBottom());

    juce::Path tri;
    tri.addTriangle ((float) cx - 4.0f, (float) area.getY(),
                       (float) cx + 4.0f, (float) area.getY(),
                       (float) cx,         (float) area.getY() + 5.0f);
    g.setColour (kEditCursor);
    g.fillPath (tri);
}

// ── Handle hit-test rects ────────────────────────────────────────────
// All rects are in screen coords inside the waveform area. Each handle
// gets a generous grab slop wider than the painted glyph (forgiving for
// mouse precision, especially at low pixelsPerSample).
juce::Rectangle<int> AudioRegionEditor::fadeInHandleRect (juce::Rectangle<int> waveArea) const
{
    const auto* r = region();
    if (r == nullptr) return {};
    const int x = xForSample (r->sourceOffset + r->fadeInSamples, waveArea);
    return juce::Rectangle<int> (x - 9, waveArea.getY() + 4, 18, 18);
}

juce::Rectangle<int> AudioRegionEditor::fadeOutHandleRect (juce::Rectangle<int> waveArea) const
{
    const auto* r = region();
    if (r == nullptr) return {};
    const int x = xForSample (r->sourceOffset + r->lengthInSamples - r->fadeOutSamples, waveArea);
    return juce::Rectangle<int> (x - 9, waveArea.getY() + 4, 18, 18);
}

juce::Rectangle<int> AudioRegionEditor::trimStartRect (juce::Rectangle<int> waveArea) const
{
    const auto* r = region();
    if (r == nullptr) return {};
    const int x = xForSample (r->sourceOffset, waveArea);
    return juce::Rectangle<int> (x - 4, waveArea.getY() + 18,
                                    8, waveArea.getHeight() - 22);
}

juce::Rectangle<int> AudioRegionEditor::trimEndRect (juce::Rectangle<int> waveArea) const
{
    const auto* r = region();
    if (r == nullptr) return {};
    const int x = xForSample (r->sourceOffset + r->lengthInSamples, waveArea);
    return juce::Rectangle<int> (x - 4, waveArea.getY() + 18,
                                    8, waveArea.getHeight() - 22);
}

int AudioRegionEditor::gainLineY (juce::Rectangle<int> waveArea) const
{
    const auto* r = region();
    if (r == nullptr) return waveArea.getCentreY();
    // 0 dB sits at the vertical centre. Positive gain raises the line
    // (smaller y); negative drops it. Range: ±24 dB across half the
    // waveform area's height.
    const auto inset = waveArea.reduced (4, 4);
    const float spanPx = (float) inset.getHeight() * 0.5f;
    const float frac = juce::jlimit (-24.0f, 12.0f, r->gainDb) / 24.0f;
    return inset.getCentreY() - (int) std::round (frac * spanPx);
}

juce::MouseCursor AudioRegionEditor::cursorForPoint (int x, int y) const
{
    // Single source of truth for cursor selection — shared between
    // mouseMove and updateModeCursor so a toolbar-driven session.editMode
    // flip can't overwrite a stationary resize cursor.
    const auto waveArea = juce::Rectangle<int> (0, kIconRowHeight + kRulerHeight,
                                                   getWidth(),
                                                   getHeight() - kIconRowHeight - kRulerHeight - kStatusBarH - kScrollBarH);

    if (fadeInHandleRect (waveArea).contains (x, y)
        || fadeOutHandleRect (waveArea).contains (x, y))
        return juce::MouseCursor::LeftRightResizeCursor;

    if (trimStartRect (waveArea).contains (x, y)
        || trimEndRect (waveArea).contains (x, y))
        return juce::MouseCursor::LeftRightResizeCursor;

    if (waveArea.contains (x, y) && std::abs (y - gainLineY (waveArea)) <= 4)
        return juce::MouseCursor::UpDownResizeCursor;

    if (waveArea.contains (x, y))
    {
        // For Grab / Cut / Draw the CursorOverlay paints the glyph
        // directly at the mouse position; hide the native cursor so
        // both don't show at once. Range (IBeam) + Grid (Crosshair)
        // stay native — they're standard OS cursors that render
        // correctly without the overlay.
        const auto m = session.editMode;
        if (m == EditMode::Grab || m == EditMode::Cut || m == EditMode::Draw)
            return invisibleCursor();
        return cursorForEditMode (m);
    }

    return juce::MouseCursor::NormalCursor;
}

void AudioRegionEditor::pushCursorPosition (int x, int y)
{
    if (! (onMouseMovedForCursor && onMouseExitedForCursor)) return;

    const auto waveArea = juce::Rectangle<int> (0, kIconRowHeight + kRulerHeight,
                                                   getWidth(),
                                                   getHeight() - kIconRowHeight - kRulerHeight - kStatusBarH - kScrollBarH);
    const auto m = session.editMode;
    const bool inContent = waveArea.contains (x, y);
    const bool wantsGlyph = inContent
                           && (m == EditMode::Grab || m == EditMode::Cut || m == EditMode::Draw);

    // In Cut mode the dashed cut-line + scissor variant only fires
    // when the cursor X is over the REGION's painted audio range
    // (between its timelineStart and timelineStart + lengthInSamples
    // mapped through xForTimelineSample). Empty area before / after
    // the region — scrolled past the audio block — gets the plain
    // scissor with no dashed line.
    juce::Range<int> cutLine;
    int visualX = x;
    if (wantsGlyph && m == EditMode::Cut)
    {
        // Cut splits the slice under the cursor (regionIndexAtX), which in the
        // multi-slice neighbourhood view may not be the focused region - so
        // preview the hovered slice's bounds, falling back to the focused one.
        const int hovIdx = regionIndexAtX (x, waveArea);
        const auto& regs = session.track (trackIdx).regions;
        const AudioRegion* r = (hovIdx >= 0 && hovIdx < (int) regs.size())
                                 ? &regs[(size_t) hovIdx] : region();
        if (r != nullptr)
        {
            const int xStart = xForTimelineSample (r->timelineStart, waveArea);
            const int xEnd   = xForTimelineSample (r->timelineStart + r->lengthInSamples,
                                                     waveArea);
            const auto regionX = juce::Range<int> (juce::jmin (xStart, xEnd),
                                                     juce::jmax (xStart, xEnd));
            if (regionX.contains (x))
            {
                cutLine = juce::Range<int> (waveArea.getY(), waveArea.getBottom());

                // Snap the visual cursor X to the same grid the cut
                // commits to (snapTimelineSampleToGrid). Without this
                // the scissor renders at the raw cursor X while the
                // actual split lands at the nearest grid line — user
                // reads it as "the cut happened left of the scissor".
                if (session.snapToGrid)
                {
                    const auto rawT     = timelineSampleForX (x, waveArea);
                    const auto snappedT = snapTimelineSampleToGrid (rawT, /*bypass*/ false);
                    visualX = xForTimelineSample (snappedT, waveArea);
                }
            }
        }
    }

    if (wantsGlyph) onMouseMovedForCursor (*this, juce::Point<int> (visualX, y), m, cutLine);
    else            onMouseExitedForCursor();
}

void AudioRegionEditor::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (cursorForPoint (e.x, e.y));
    pushCursorPosition (e.x, e.y);
}

void AudioRegionEditor::updateModeCursor()
{
    const auto p = getMouseXYRelative();
    setMouseCursor (cursorForPoint (p.x, p.y));
    // Mode changed (toolbar / 'G' hotkey) with no mouse event to follow -
    // refresh the overlay glyph from the same point so it doesn't show the
    // previous mode's glyph until the next move.
    pushCursorPosition (p.x, p.y);
}

void AudioRegionEditor::mouseExit (const juce::MouseEvent&)
{
    if (onMouseExitedForCursor) onMouseExitedForCursor();
}

void AudioRegionEditor::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    const auto* r = region();
    if (r == nullptr) return;
    const auto waveArea = juce::Rectangle<int> (0, kIconRowHeight + kRulerHeight,
                                                   getWidth(),
                                                   getHeight() - kIconRowHeight - kRulerHeight - kStatusBarH - kScrollBarH);

    // Middle-mouse-drag pans the timeline. Captured before any other
    // branch so it never gets shadowed by handle / range / context-menu
    // logic. Active inside the wave area only - middle-click on the
    // ruler / status bar does nothing.
    if (e.mods.isMiddleButtonDown() && waveArea.contains (e.x, e.y))
    {
        dragMode = DragMode::Pan;
        panStartMouseX = e.x;
        panStartScroll = scrollSamples;
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        return;
    }

    // Automation overlay: when a param lane is active, intercept clicks
    // inside the wave area BEFORE the region-edit branches so the
    // engineer can edit points without first hitting "Off" on the
    // automation picker. Editing is GATED ON TRANSPORT STOPPED — the
    // audio thread reads lane.points lock-free in Read/Touch mode, so
    // mid-play mutation would race.
    if (automationParam >= 0
        && automationParam < kNumAutomationParams
        && waveArea.contains (e.x, e.y)
        && trackIdx >= 0 && trackIdx < Session::kNumTracks
        && engine.getTransport().getState() == Transport::State::Stopped)
    {
        auto& lane = session.track (trackIdx)
                          .automationLanes[(size_t) automationParam];
        const int hit = hitTestAutomationPoint (e.x, e.y, waveArea);

        if (e.mods.isPopupMenu())
        {
            // Right-click a dot → delete. Right-click empty → no-op so
            // the user can still get the region context menu by aiming
            // away from any dot.
            if (hit >= 0)
            {
                lane.points.erase (lane.points.begin() + hit);
                // Publish the lane mutation via a release-store on the
                // track's automationMode atomic (Session.h synchronisation
                // contract: every message-thread append/erase must
                // happen-before the audio thread's next acquire-load).
                // Cheap — single re-store of the same value.
                {
                    auto& trk = session.track (trackIdx);
                    const int m = trk.automationMode.load (std::memory_order_relaxed);
                    trk.automationMode.store (m, std::memory_order_release);
                }
                repaint();
                return;
            }
        }
        else
        {
            if (hit >= 0)
            {
                draggedPointIdx = hit;
                dragMode = DragMode::AutomationPoint;
                setMouseCursor (juce::MouseCursor::DraggingHandCursor);
                return;
            }
            // Empty area click → add a point at the snapped click position.
            const auto fileSample = sampleForX (e.x, waveArea);
            const auto snappedFile = snapFileSampleToGrid (fileSample,
                                                              e.mods.isCommandDown());
            const auto fileToTimeline = r->timelineStart - r->sourceOffset;
            const juce::int64 t = snappedFile + fileToTimeline;
            float v01 = automationValueForY (e.y, waveArea);
            // Discrete lanes (Mute / Solo) store 0/1 only — intermediate
            // values would render correctly via the connected-dot view
            // but evaluateLane thresholds at 0.5, so a 0.3-stored value
            // round-trips to 0 anyway. Quantize at storage time so the
            // dot's painted position matches its audible behaviour.
            if (! isContinuousParam ((AutomationParam) automationParam))
                v01 = (v01 >= 0.5f) ? 1.0f : 0.0f;

            AutomationPoint pt;
            pt.timeSamples = juce::jmax<juce::int64> (0, t);
            pt.value       = v01;
            pt.recordedAtBPM = session.tempoBpm.load (std::memory_order_relaxed);

            auto insertAt = std::upper_bound (
                lane.points.begin(), lane.points.end(), pt,
                [] (const AutomationPoint& a, const AutomationPoint& b)
                { return a.timeSamples < b.timeSamples; });
            const int newIdx = (int) (insertAt - lane.points.begin());
            lane.points.insert (insertAt, pt);

            // Auto-arm: flip the track's automation mode to Read so
            // the freshly-drawn lane is audible immediately on Play.
            // No-op if already Read / Touch. Without this the engineer
            // draws a curve, hits Play, and hears nothing because the
            // engine ignores lanes in Off mode.
            // The mode store is ALSO the release publication of the
            // lane.points.insert above (Session.h sync contract). If
            // the mode is already Read / Touch (no transition), do an
            // explicit re-store so the audio thread's next acquire-load
            // still happens-after this insert.
            auto& trk = session.track (trackIdx);
            const int curMode = trk.automationMode.load (std::memory_order_acquire);
            if (curMode == (int) AutomationMode::Off
                || curMode == (int) AutomationMode::Write)
            {
                trk.automationMode.store ((int) AutomationMode::Read,
                                            std::memory_order_release);
            }
            else
            {
                trk.automationMode.store (curMode, std::memory_order_release);
            }

            draggedPointIdx = newIdx;
            dragMode = DragMode::AutomationPoint;
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
            repaint();
            return;
        }
    }

    // Right-click → context menu (split / reset gain / reset fades / mute /
    // lock / colour). Captured here so it doesn't slip through to the
    // edit-cursor branch below. Right-click on a fade handle goes to the
    // fade-shape submenu instead so the user can pick the curve type.
    if (e.mods.isPopupMenu())
    {
        if (! r->locked)
        {
            if (fadeInHandleRect (waveArea).contains (e.x, e.y))
            {
                showFadeShapeMenu (e.getScreenPosition(), /*isFadeIn*/ true);
                return;
            }
            if (fadeOutHandleRect (waveArea).contains (e.x, e.y))
            {
                showFadeShapeMenu (e.getScreenPosition(), /*isFadeIn*/ false);
                return;
            }
        }
        if (waveArea.contains (e.x, e.y))
        {
            editCursorSample = snapFileSampleToGrid (sampleForX (e.x, waveArea),
                                                     e.mods.isCommandDown());
            refreshStatusBarReadouts();
        }
        showContextMenu (e.getScreenPosition());
        return;
    }

    // Drag in the bar/time ruler at the top of the modal → start a
    // range selection. Captured BEFORE the handle / waveform branches
    // so it never gets shadowed by them. Plain click in the ruler
    // clears an existing range without forcing a drag-to-clear.
    const auto rulerArea = juce::Rectangle<int> (0, kIconRowHeight,
                                                    getWidth(), kRulerHeight);
    if (rulerArea.contains (e.x, e.y))
    {
        // Ruler click seeks the transport playhead (Reaper/Ardour
        // muscle memory) AND starts a range-select drag. A click
        // without drag is therefore a pure seek; a drag refines the
        // selection on top. Both the seek + the range origin honour
        // SNAP so the playhead and selection land on the grid.
        const bool bypassSnap = e.mods.isCommandDown();
        engine.getTransport().setPlayhead (
            snapTimelineSampleToGrid (timelineSampleForX (e.x, waveArea), bypassSnap));
        rangeStartSample = snapFileSampleToGrid (sampleForX (e.x, waveArea), bypassSnap);
        rangeEndSample   = rangeStartSample;
        rangeActive      = false;   // becomes true once the drag distance > 0
        dragMode         = DragMode::Range;
        repaint();
        return;
    }

    // Locked regions: visible but un-mutatable. Cursor still drops.
    const bool locked = r->locked;

    // Snapshot the region's current state for the eventual RegionEditAction.
    regionAtDragStart = *r;
    dragOriginMouseY  = e.y;
    dragOriginGainDb  = r->gainDb;

    if (! locked)
    {
        if (fadeInHandleRect (waveArea).contains (e.x, e.y))
        {
            dragMode = DragMode::FadeIn;
            return;
        }
        if (fadeOutHandleRect (waveArea).contains (e.x, e.y))
        {
            dragMode = DragMode::FadeOut;
            return;
        }
        if (trimStartRect (waveArea).contains (e.x, e.y))
        {
            dragMode = DragMode::TrimStart;
            return;
        }
        if (trimEndRect (waveArea).contains (e.x, e.y))
        {
            dragMode = DragMode::TrimEnd;
            return;
        }
        if (waveArea.contains (e.x, e.y) && std::abs (e.y - gainLineY (waveArea)) <= 4)
        {
            dragMode = DragMode::Gain;
            return;
        }
    }

    if (waveArea.contains (e.x, e.y))
    {
        const int hitIdx = regionIndexAtX (e.x, waveArea);

        // Cmd/Ctrl+click on a neighborhood region toggles it into the
        // additional-selection set (mirrors TapeStrip's multi-select
        // gesture). Edge / fade / gain handles are still single-region
        // ops above; only body hits participate. Bypass mode-specific
        // dispatch so Range / Cut don't swallow the gesture.
        if (e.mods.isCommandDown() && hitIdx >= 0)
        {
            if (hitIdx == regionIdx)
            {
                // No-op: the focused region is implicitly selected.
            }
            else
            {
                auto it = std::find (additionalSelectedRegions.begin(),
                                      additionalSelectedRegions.end(), hitIdx);
                if (it != additionalSelectedRegions.end())
                    additionalSelectedRegions.erase (it);
                else
                    additionalSelectedRegions.push_back (hitIdx);
            }
            dragMode = DragMode::None;
            repaint();
            return;
        }

        // Shift+click on a region body fills every region between the
        // primary (focused regionIdx) and the clicked region into the
        // additional-selection set, in timeline order. Matches the
        // TapeStrip Shift+click gesture so muscle memory carries over.
        // Shift+click in a gap still starts a time-range drag below.
        if (e.mods.isShiftDown() && hitIdx >= 0 && hitIdx != regionIdx)
        {
            const auto& regs = session.track (trackIdx).regions;
            if (regionIdx >= 0 && regionIdx < (int) regs.size()
                && hitIdx < (int) regs.size())
            {
                const auto anchorTL = regs[(size_t) regionIdx].timelineStart;
                const auto clickTL  = regs[(size_t) hitIdx].timelineStart;
                const auto lo = juce::jmin (anchorTL, clickTL);
                const auto hi = juce::jmax (anchorTL, clickTL);
                additionalSelectedRegions.clear();
                for (int i = 0; i < (int) regs.size(); ++i)
                {
                    if (i == regionIdx) continue;
                    const auto t = regs[(size_t) i].timelineStart;
                    if (t < lo || t > hi) continue;
                    additionalSelectedRegions.push_back (i);
                }
                dragMode = DragMode::None;
                repaint();
                return;
            }
        }

        // Edit-mode dispatch in the waveform body. Cut: single click splits
        // the region UNDER THE CLICK (not the editor's focused region) so
        // a sequence of cuts works even after the first split has moved
        // the boundary out from under regionIdx. Range: any body click
        // forces a range-select drag regardless of where it lands.
        const auto mode = session.editMode;
        if (mode == EditMode::Cut && hitIdx >= 0)
        {
            const auto& reg = session.track (trackIdx).regions[(size_t) hitIdx];
            if (! reg.locked)
            {
                const auto timelineSplit = snapTimelineSampleToGrid (
                    timelineSampleForX (e.x, waveArea), e.mods.isCommandDown());
                auto& um = engine.getUndoManager();
                um.beginNewTransaction ("Split region");
                um.perform (new SplitRegionAction (
                    session, engine, trackIdx, hitIdx, timelineSplit));
                // After split, regs[hitIdx] is the left half and the
                // right half is at hitIdx+1. Focus the slice that the
                // user just cut (= the left half) so the modal's
                // neighborhood paint stays anchored on the cut zone.
                regionIdx = hitIdx;
                refreshStatusBarReadouts();
                repaint();
            }
            return;
        }

        // Shift-drag anywhere in the waveform body OR plain drag in a gap
        // between regions starts a range selection (Grab Mode). Range Mode
        // forces this path on every body click. Matches Reaper's body-drag
        // = time-selection muscle memory while keeping a plain click on a
        // region body free for cursor-drop / move in Grab Mode.
        if (mode == EditMode::Range || e.mods.isShiftDown() || hitIdx < 0)
        {
            rangeStartSample = sampleForX (e.x, waveArea);
            rangeEndSample   = rangeStartSample;
            rangeActive      = false;
            dragMode         = DragMode::Range;
            repaint();
            return;
        }

        // Helper: snapshot timelineStart for every additional-selected
        // region so MoveRegion drag can translate them all by the same
        // delta. Ordering mirrors additionalSelectedRegions.
        auto captureMultiOrigins = [&] ()
        {
            dragMultiOriginStarts.clear();
            const auto& regs = session.track (trackIdx).regions;
            for (int idx : additionalSelectedRegions)
            {
                if (idx >= 0 && idx < (int) regs.size())
                    dragMultiOriginStarts.push_back (regs[(size_t) idx].timelineStart);
                else
                    dragMultiOriginStarts.push_back (0);
            }
        };

        const bool clickedSelected = (hitIdx >= 0
            && (hitIdx == regionIdx
                || std::find (additionalSelectedRegions.begin(),
                              additionalSelectedRegions.end(), hitIdx)
                   != additionalSelectedRegions.end()));

        // Click-to-focus inside the neighborhood view: if the click
        // landed on a DIFFERENT slice than the currently-focused one,
        // swap regionIdx to that slice and re-anchor the edit cursor
        // inside it. Same-slice clicks fall through to the existing
        // edit-cursor-drop + MoveRegion-prep behaviour. Transport
        // playhead is NOT moved by region clicks — the user picks up
        // an item to move it, not to seek; the ruler band handles
        // explicit seek clicks separately. Clicking on a region that
        // is already part of the multi-selection preserves the set so
        // the drag translates the whole group.
        if (hitIdx >= 0 && hitIdx != regionIdx)
        {
            if (! clickedSelected) additionalSelectedRegions.clear();
            regionIdx = hitIdx;
            if (auto* nr = region())
            {
                regionAtDragStart = *nr;
                dragOriginGainDb = nr->gainDb;
                editCursorSample = nr->sourceOffset
                    + juce::jlimit<juce::int64> (0, nr->lengthInSamples,
                        timelineSampleForX (e.x, waveArea) - nr->timelineStart);
                dragOriginTimelineSample = nr->timelineStart;
            }
            captureMultiOrigins();
            dragMode = DragMode::MoveRegion;
            refreshStatusBarReadouts();
            repaint();
            return;
        }

        // Same-slice click: edit cursor follows (editor-internal,
        // doesn't move the transport playhead). dragOriginTimeline
        // tracked so a follow-up horizontal drag past the 3 px
        // threshold promotes the click into a MoveRegion drag.
        if (! clickedSelected) additionalSelectedRegions.clear();
        editCursorSample = snapFileSampleToGrid (sampleForX (e.x, waveArea),
                                                 e.mods.isCommandDown());
        dragOriginTimelineSample = r->timelineStart;
        captureMultiOrigins();
        dragMode = DragMode::MoveCursor;
        refreshStatusBarReadouts();
        repaint();
    }
}

void AudioRegionEditor::mouseDrag (const juce::MouseEvent& e)
{
    pushCursorPosition (e.x, e.y);

    auto* r = region();
    if (r == nullptr || dragMode == DragMode::None) return;
    const auto waveArea = juce::Rectangle<int> (0, kIconRowHeight + kRulerHeight,
                                                   getWidth(),
                                                   getHeight() - kIconRowHeight - kRulerHeight - kStatusBarH - kScrollBarH);

    // Pan drag: convert the mouse-x delta to a sample delta and apply
    // against the captured scroll origin so the timeline tracks the
    // cursor 1:1. Inverted because dragging right should move the
    // timeline left (revealing content to the right of the visible
    // window).
    if (dragMode == DragMode::Pan)
    {
        const int dxPixels = e.x - panStartMouseX;
        const auto dxSamples = (juce::int64) std::round (
            - (double) dxPixels / juce::jmax (1.0e-5f, pixelsPerSample));
        scrollSamples = juce::jlimit<juce::int64> (0,
            juce::jmax<juce::int64> (0, anchorTimelineLength - 1),
            panStartScroll + dxSamples);
        repaint();
        return;
    }

    // Automation point drag: move time + value of the in-flight point.
    // Re-sorts the lane if the drag pushed the point past a neighbour
    // so the polyline + binary-search lookup stay consistent.
    if (dragMode == DragMode::AutomationPoint
        && automationParam >= 0
        && automationParam < kNumAutomationParams
        && draggedPointIdx >= 0
        && trackIdx >= 0 && trackIdx < Session::kNumTracks)
    {
        auto& lane = session.track (trackIdx)
                          .automationLanes[(size_t) automationParam];
        if (draggedPointIdx >= (int) lane.points.size())
        {
            draggedPointIdx = -1;
            dragMode = DragMode::None;
            return;
        }
        const auto* rr = region();
        if (rr == nullptr) return;
        const auto fileSample = sampleForX (e.x, waveArea);
        const auto snappedFile = snapFileSampleToGrid (fileSample,
                                                          e.mods.isCommandDown());
        const auto fileToTimeline = rr->timelineStart - rr->sourceOffset;
        const juce::int64 t = juce::jmax<juce::int64> (0, snappedFile + fileToTimeline);
        float v01 = automationValueForY (e.y, waveArea);
        // Discrete lanes (Mute / Solo): same quantization rationale as
        // the click-add path above — store 0/1 so the painted dot and
        // the audible behaviour agree.
        if (! isContinuousParam ((AutomationParam) automationParam))
            v01 = (v01 >= 0.5f) ? 1.0f : 0.0f;

        auto& pt = lane.points[(size_t) draggedPointIdx];
        pt.timeSamples = t;
        pt.value       = v01;

        // Maintain sort: if the new time crosses a neighbour, sort the
        // lane and refind our point's new index by identity (time +
        // value match — collisions are negligible at click resolution).
        const auto needSort = (draggedPointIdx > 0
                                  && lane.points[(size_t) draggedPointIdx - 1].timeSamples > t)
                             || (draggedPointIdx + 1 < (int) lane.points.size()
                                  && lane.points[(size_t) draggedPointIdx + 1].timeSamples < t);
        if (needSort)
        {
            std::stable_sort (lane.points.begin(), lane.points.end(),
                [] (const AutomationPoint& a, const AutomationPoint& b)
                { return a.timeSamples < b.timeSamples; });
            int refound = -1;
            for (int i = 0; i < (int) lane.points.size(); ++i)
                if (lane.points[(size_t) i].timeSamples == t
                    && std::abs (lane.points[(size_t) i].value - v01) < 1e-4f)
                {
                    refound = i;
                    break;
                }
            // Refind miss: float-tolerance match failed (rare but
            // possible after value quantization on discrete lanes).
            // Bail the drag rather than mutate a stale index that
            // could update the wrong point on the next mouseDrag.
            if (refound < 0)
            {
                draggedPointIdx = -1;
                dragMode = DragMode::None;
                setMouseCursor (juce::MouseCursor::NormalCursor);
                repaint();
                return;
            }
            draggedPointIdx = refound;
        }
        repaint();
        return;
    }

    // Snap-on-drag. Cmd bypasses (matches TransportBar's snap-toggle UI:
    // permanent toggle for the default, modifier-key escape for one-off
    // overrides). Snap operates in TIMELINE space (where the user reads
    // the bar/beat ruler) but the underlying state mutations are
    // file-sample-based, so we round-trip through the focused slice's
    // sourceOffset → timelineStart mapping.
    const bool bypassSnap = e.mods.isCommandDown();
    const double sampleRate = engine.getCurrentSampleRate();

    auto snapFileSampleToTimelineGrid = [&] (juce::int64 fileSample) -> juce::int64
    {
        snapGuideTimelineSample = -1;
        if (bypassSnap || ! session.snapToGrid) return fileSample;
        auto* rr = region();
        if (rr == nullptr) return fileSample;
        const auto fileToTimeline = rr->timelineStart - rr->sourceOffset;
        const auto timelineSample = fileSample + fileToTimeline;
        const auto snapped = snap::snapAbsoluteToGrid (timelineSample, session, sampleRate);
        if (snapped != timelineSample)
            snapGuideTimelineSample = snapped;
        return snapped - fileToTimeline;
    };

    auto snapDeltaSamples = [&] (juce::int64 delta) -> juce::int64
    {
        if (bypassSnap) { snapGuideTimelineSample = -1; return delta; }
        return snap::snapDeltaToGrid (delta, session, sampleRate);
    };

    if (dragMode == DragMode::MoveCursor)
    {
        // Promote to MoveRegion once the drag exceeds a small threshold,
        // so a click on the focused slice's body can be dragged-to-move
        // without losing the cursor-drop behaviour for plain clicks.
        constexpr int kMoveRegionPromoteThresholdPx = 3;
        if (! r->locked
            && std::abs (e.x - e.getMouseDownX()) > kMoveRegionPromoteThresholdPx)
        {
            dragMode = DragMode::MoveRegion;
            // fall through into the MoveRegion branch below
        }
        else
        {
            editCursorSample = sampleForX (e.x, waveArea);
            refreshStatusBarReadouts();
            repaint();
            return;
        }
    }

    if (dragMode == DragMode::MoveRegion)
    {
        // Live-mutate timelineStart so the user sees the slice slide
        // while dragging. Commit (RegionEditAction) fires on mouseUp.
        // Anchor stays put -> view doesn't shift, only the focused
        // slice (and any additional-selected regions) move visually.
        if (r->locked) return;
        const auto cursorTimeline = timelineSampleForX (e.x, waveArea);
        const auto dragStartTimeline = timelineSampleForX (e.getMouseDownX(), waveArea);
        const auto rawDelta = cursorTimeline - dragStartTimeline;
        const auto delta = snapDeltaSamples (rawDelta);
        const auto newStart = juce::jmax<juce::int64> (0, dragOriginTimelineSample + delta);
        r->timelineStart = newStart;
        if (! bypassSnap && session.snapToGrid && delta != rawDelta)
            snapGuideTimelineSample = newStart;
        // Multi-select translate: every additional region shifts by the
        // same delta as the focused region. dragMultiOriginStarts was
        // populated in mouseDown so we don't accumulate drift across
        // mouseDrag invocations.
        auto& regs = session.track (trackIdx).regions;
        for (size_t i = 0; i < additionalSelectedRegions.size()
                              && i < dragMultiOriginStarts.size(); ++i)
        {
            const int idx = additionalSelectedRegions[i];
            if (idx < 0 || idx >= (int) regs.size()) continue;
            auto& other = regs[(size_t) idx];
            if (other.locked) continue;
            other.timelineStart = juce::jmax<juce::int64> (0,
                dragMultiOriginStarts[i] + delta);
        }
        repaint();
        return;
    }

    if (dragMode == DragMode::Range)
    {
        const auto snappedFileSample = snapFileSampleToTimelineGrid (sampleForX (e.x, waveArea));
        rangeEndSample = snappedFileSample;
        rangeActive = (rangeEndSample != rangeStartSample);
        repaint();
        return;
    }

    switch (dragMode)
    {
        case DragMode::FadeIn:
        {
            const auto sample = snapFileSampleToTimelineGrid (sampleForX (e.x, waveArea));
            const auto fadeIn = juce::jlimit<juce::int64> (
                0,
                juce::jmax<juce::int64> (0, r->lengthInSamples - r->fadeOutSamples),
                sample - r->sourceOffset);
            r->fadeInSamples = fadeIn;
            snapGuideTimelineSample = r->timelineStart + r->fadeInSamples;
            break;
        }
        case DragMode::FadeOut:
        {
            const auto sample = snapFileSampleToTimelineGrid (sampleForX (e.x, waveArea));
            const auto endAbs = r->sourceOffset + r->lengthInSamples;
            const auto fadeOut = juce::jlimit<juce::int64> (
                0,
                juce::jmax<juce::int64> (0, r->lengthInSamples - r->fadeInSamples),
                endAbs - sample);
            r->fadeOutSamples = fadeOut;
            snapGuideTimelineSample = r->timelineStart
                                          + r->lengthInSamples - r->fadeOutSamples;
            break;
        }
        case DragMode::Gain:
        {
            // 0.1 dB per pixel matches TapeStrip's Alt-drag scale, so the
            // muscle-memory carries over. Vertical drag - no horizontal
            // snap.
            snapGuideTimelineSample = -1;
            const float deltaDb = (float) (dragOriginMouseY - e.y) * 0.1f;
            r->gainDb = juce::jlimit (-24.0f, 12.0f, dragOriginGainDb + deltaDb);
            break;
        }
        case DragMode::TrimStart:
        {
            // Trim-start moves the slice's leading edge along the source
            // file. sourceOffset shifts by the delta so the audio stays
            // continuous (mirrors TapeStrip's TrimStart op). lengthInSamples
            // shrinks/grows by the same amount.
            const auto newSourceOffset = juce::jlimit<juce::int64> (
                0,
                regionAtDragStart.sourceOffset
                    + regionAtDragStart.lengthInSamples - 1,
                snapFileSampleToTimelineGrid (sampleForX (e.x, waveArea)));
            const auto delta = newSourceOffset - regionAtDragStart.sourceOffset;
            r->sourceOffset    = newSourceOffset;
            r->lengthInSamples = regionAtDragStart.lengthInSamples - delta;
            r->timelineStart   = regionAtDragStart.timelineStart   + delta;
            // Clamp fades against the new length.
            r->fadeInSamples  = juce::jlimit<juce::int64> (0, r->lengthInSamples, r->fadeInSamples);
            r->fadeOutSamples = juce::jlimit<juce::int64> (0, r->lengthInSamples - r->fadeInSamples, r->fadeOutSamples);
            break;
        }
        case DragMode::TrimEnd:
        {
            const auto endAbs = snapFileSampleToTimelineGrid (sampleForX (e.x, waveArea));
            const auto newLength = juce::jlimit<juce::int64> (
                1,
                std::numeric_limits<juce::int64>::max(),
                endAbs - regionAtDragStart.sourceOffset);
            r->lengthInSamples = newLength;
            r->fadeInSamples  = juce::jlimit<juce::int64> (0, newLength, r->fadeInSamples);
            r->fadeOutSamples = juce::jlimit<juce::int64> (0, newLength - r->fadeInSamples, r->fadeOutSamples);
            break;
        }
        default: break;
    }
    refreshStatusBarReadouts();
    repaint();
}

void AudioRegionEditor::mouseUp (const juce::MouseEvent&)
{
    auto* r = region();
    // Pan ends with no undo entry / no state to commit - just clear the
    // drag state and restore the default cursor.
    if (dragMode == DragMode::Pan)
    {
        dragMode = DragMode::None;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }
    // Automation point drag finishes: clear the in-flight index. No
    // undoable wrapper for v1 — direct lane mutation matches the
    // existing piano-roll-edit race profile (transport must be stopped,
    // gated at mouseDown). Future commit can add an undo action.
    if (dragMode == DragMode::AutomationPoint)
    {
        // Publish the drag's lane.points writes via a release-store on
        // automationMode so the audio thread's next acquire-load sees
        // them. Per-frame publication during the drag would be wasteful;
        // one re-store at drag-end is sufficient because editing is
        // gated on transport=Stopped (no acquire-load can race during
        // the drag).
        if (trackIdx >= 0 && trackIdx < Session::kNumTracks
            && automationParam >= 0 && automationParam < kNumAutomationParams)
        {
            auto& trk = session.track (trackIdx);
            const int m = trk.automationMode.load (std::memory_order_relaxed);
            trk.automationMode.store (m, std::memory_order_release);
        }
        draggedPointIdx = -1;
        dragMode = DragMode::None;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }
    if (dragMode == DragMode::Range)
    {
        // Normalise so start <= end (drag-leftwards still works).
        if (rangeEndSample < rangeStartSample)
            std::swap (rangeStartSample, rangeEndSample);
        rangeActive = (rangeEndSample > rangeStartSample);
        dragMode = DragMode::None;
        snapGuideTimelineSample = -1;
        repaint();
        return;
    }
    const bool wasUndoableEdit = (r != nullptr
                                    && dragMode != DragMode::None
                                    && dragMode != DragMode::MoveCursor);
    if (wasUndoableEdit)
    {
        AudioRegion afterState = *r;
        // Manual fade-handle drag promotes the fade to user-owned so the
        // auto-crossfade pass on subsequent moves won't retract or
        // resize it.
        if (dragMode == DragMode::FadeIn)
        {
            afterState.fadeInAuto = false;
            r->fadeInAuto = false;
        }
        if (dragMode == DragMode::FadeOut)
        {
            afterState.fadeOutAuto = false;
            r->fadeOutAuto = false;
        }
        // Skip the action if nothing actually changed (e.g. user grabbed
        // a handle and didn't drag). UndoManager would happily record a
        // no-op transaction otherwise.
        const bool changed =
            afterState.sourceOffset    != regionAtDragStart.sourceOffset
         || afterState.timelineStart   != regionAtDragStart.timelineStart
         || afterState.lengthInSamples != regionAtDragStart.lengthInSamples
         || afterState.fadeInSamples   != regionAtDragStart.fadeInSamples
         || afterState.fadeOutSamples  != regionAtDragStart.fadeOutSamples
         || afterState.fadeInAuto      != regionAtDragStart.fadeInAuto
         || afterState.fadeOutAuto     != regionAtDragStart.fadeOutAuto
         || afterState.gainDb          != regionAtDragStart.gainDb;
        if (changed)
        {
            // Roll the live state back to the before-state, then submit
            // the action - UndoManager.perform() calls perform() which
            // applies after-state. This way the action's stored before
            // is the authoritative one.
            *r = regionAtDragStart;
            auto& um = engine.getUndoManager();
            um.beginNewTransaction (
                dragMode == DragMode::FadeIn     ? "Fade-in"      :
                dragMode == DragMode::FadeOut    ? "Fade-out"     :
                dragMode == DragMode::Gain       ? "Region gain"  :
                dragMode == DragMode::TrimStart  ? "Trim start"   :
                dragMode == DragMode::TrimEnd    ? "Trim end"     :
                dragMode == DragMode::MoveRegion ? "Move region"  :
                                                       "Edit region");
            um.perform (new RegionEditAction (
                session, engine, trackIdx, regionIdx,
                regionAtDragStart, afterState));

            // Multi-select Move: every additional region also has its
            // timelineStart shifted live. Commit each as its own
            // RegionEditAction inside the same undo transaction so a
            // single Cmd+Z reverts the whole group move.
            if (dragMode == DragMode::MoveRegion)
            {
                auto& regs = session.track (trackIdx).regions;
                for (size_t i = 0; i < additionalSelectedRegions.size()
                                      && i < dragMultiOriginStarts.size(); ++i)
                {
                    const int idx = additionalSelectedRegions[i];
                    if (idx < 0 || idx >= (int) regs.size()) continue;
                    auto& other = regs[(size_t) idx];
                    if (other.locked) continue;
                    AudioRegion otherAfter = other;
                    if (otherAfter.timelineStart == dragMultiOriginStarts[i]) continue;
                    AudioRegion otherBefore = otherAfter;
                    otherBefore.timelineStart = dragMultiOriginStarts[i];
                    other = otherBefore;
                    um.perform (new RegionEditAction (
                        session, engine, trackIdx, idx,
                        otherBefore, otherAfter));
                }

            }
            // Auto-crossfade sync runs after any geometry edit so both
            // sides of an overlap update uniformly.
            if (dragMode == DragMode::MoveRegion
                || dragMode == DragMode::TrimStart
                || dragMode == DragMode::TrimEnd)
            {
                syncAutoCrossfades();
            }
        }
    }
    dragMultiOriginStarts.clear();
    dragMode = DragMode::None;
    snapGuideTimelineSample = -1;
    refreshStatusBarReadouts();
    repaint();
}

void AudioRegionEditor::showContextMenu (juce::Point<int> screenPos)
{
    auto* r = region();
    if (r == nullptr) return;

    juce::PopupMenu m;
    m.addItem (1, "Split at edit cursor");
    m.addItem (6, "Join selected regions",
                /*enabled*/ ! additionalSelectedRegions.empty());
    m.addSeparator();
    m.addItem (2, "Reset gain (0 dB)");
    m.addItem (3, "Reset fades");
    m.addSeparator();
    m.addItem (4, r->muted  ? "Unmute"  : "Mute");
    m.addItem (5, r->locked ? "Unlock"  : "Lock");

    juce::Component::SafePointer<AudioRegionEditor> safe (this);
    duskstudio::showContextMenu (m, *this, screenPos,
        [safe] (int chosen)
        {
            auto* self = safe.getComponent();
            if (self == nullptr || chosen <= 0) return;
            auto* rr = self->region();
            if (rr == nullptr) return;

            auto& um = self->engine.getUndoManager();

            if (chosen == 1)
            {
                // Split needs an absolute timeline-sample, not a file
                // sample. Map the editor's editCursorSample (which is
                // file-relative inside the slice) back onto the timeline.
                const auto timelineSplit =
                    rr->timelineStart + (self->editCursorSample - rr->sourceOffset);
                um.beginNewTransaction ("Split region");
                um.perform (new SplitRegionAction (
                    self->session, self->engine,
                    self->trackIdx, self->regionIdx, timelineSplit));
                self->refreshStatusBarReadouts();
                self->repaint();
                return;
            }

            if (chosen == 6)
            {
                if (self->additionalSelectedRegions.empty()) return;
                std::vector<int> idxs = self->additionalSelectedRegions;
                idxs.push_back (self->regionIdx);
                um.beginNewTransaction (
                    juce::String ("Join ") + juce::String ((int) idxs.size())
                    + " regions");
                um.perform (new JoinRegionsAction (
                    self->session, self->engine, self->trackIdx, idxs));
                self->additionalSelectedRegions.clear();
                // The merged region sits at the lowest source index; re-anchor
                // the focused regionIdx to it so the modal continues editing
                // the result.
                int minIdx = self->regionIdx;
                for (int i : idxs) if (i >= 0 && i < minIdx) minIdx = i;
                self->regionIdx = juce::jmax (0, minIdx);
                const int total = (int) self->session.track (self->trackIdx).regions.size();
                if (self->regionIdx >= total) self->regionIdx = total - 1;
                self->refreshStatusBarReadouts();
                self->repaint();
                return;
            }

            // The remaining actions all become RegionEditActions.
            const AudioRegion before = *rr;
            AudioRegion after = before;
            const char* label = "Edit region";
            switch (chosen)
            {
                case 2: after.gainDb         = 0.0f;  label = "Reset gain";  break;
                case 3: after.fadeInSamples  = 0;
                          after.fadeOutSamples = 0;
                          after.fadeInAuto    = false;
                          after.fadeOutAuto   = false;
                          label = "Reset fades"; break;
                case 4: after.muted          = ! before.muted;
                        label = before.muted ? "Unmute" : "Mute"; break;
                case 5: after.locked         = ! before.locked;
                        label = before.locked ? "Unlock" : "Lock"; break;
                default: return;
            }
            um.beginNewTransaction (label);
            um.perform (new RegionEditAction (
                self->session, self->engine,
                self->trackIdx, self->regionIdx,
                before, after));
            self->refreshStatusBarReadouts();
            self->repaint();
        });
}

void AudioRegionEditor::showFadeShapeMenu (juce::Point<int> screenPos, bool isFadeIn)
{
    auto* r = region();
    if (r == nullptr) return;

    const FadeShape current = isFadeIn ? r->fadeInShape : r->fadeOutShape;
    juce::PopupMenu m;
    auto addShape = [&] (FadeShape s, const juce::String& name)
    {
        m.addItem ((int) s + 1, name, true, current == s);
    };
    addShape (FadeShape::Linear,     "Linear");
    addShape (FadeShape::EqualPower, "Equal-power");
    addShape (FadeShape::Sigmoid,    "S-curve");
    addShape (FadeShape::Exp,        "Exponential");
    addShape (FadeShape::Log,        "Logarithmic");

    juce::Component::SafePointer<AudioRegionEditor> safe (this);
    duskstudio::showContextMenu (m, *this, screenPos,
        [safe, isFadeIn] (int chosen)
        {
            auto* self = safe.getComponent();
            if (self == nullptr || chosen <= 0) return;
            auto* rr = self->region();
            if (rr == nullptr) return;
            const FadeShape picked = (FadeShape) (chosen - 1);
            const AudioRegion before = *rr;
            AudioRegion after = before;
            if (isFadeIn) { after.fadeInShape  = picked; after.fadeInAuto  = false; }
            else          { after.fadeOutShape = picked; after.fadeOutAuto = false; }
            // Compare auto flags too: picking the same shape that's
            // already active still needs to clear the auto flag, which
            // is a real change that should produce an undo entry.
            if (after.fadeInShape  == before.fadeInShape
                && after.fadeOutShape == before.fadeOutShape
                && after.fadeInAuto  == before.fadeInAuto
                && after.fadeOutAuto == before.fadeOutAuto) return;
            auto& um = self->engine.getUndoManager();
            um.beginNewTransaction (isFadeIn ? "Fade-in shape" : "Fade-out shape");
            um.perform (new RegionEditAction (
                self->session, self->engine,
                self->trackIdx, self->regionIdx,
                before, after));
            self->refreshStatusBarReadouts();
            self->repaint();
        });
}

void AudioRegionEditor::mouseWheelMove (const juce::MouseEvent& e,
                                          const juce::MouseWheelDetails& w)
{
    const auto* r = region();
    if (r == nullptr) return;
    const auto waveArea = juce::Rectangle<int> (0, kIconRowHeight + kRulerHeight,
                                                   getWidth(),
                                                   getHeight() - kIconRowHeight - kRulerHeight - kStatusBarH - kScrollBarH);

    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        // Zoom anchored on the cursor's current sample.
        const auto cursorSampleBefore = sampleForX (e.x, waveArea);
        const float factor = w.deltaY > 0.0f ? 1.15f : (1.0f / 1.15f);
        pixelsPerSample = juce::jlimit (1.0e-5f, 1.0f, pixelsPerSample * factor);
        // After zoom, adjust scrollSamples so the same sample stays at
        // the cursor's x-position.
        const auto cursorSampleAfterRaw = sampleForX (e.x, waveArea);
        scrollSamples += (cursorSampleBefore - cursorSampleAfterRaw);
        scrollSamples = juce::jlimit<juce::int64> (0,
            juce::jmax<juce::int64> (0, anchorTimelineLength - 1), scrollSamples);
        repaint();
        return;
    }

    // Plain wheel / trackpad = horizontal scroll. Prefer deltaX when the
    // trackpad reports one (macOS two-finger horizontal swipe) and fall
    // back to deltaY so vertical-wheel-only mice keep panning the timeline
    // (matches the legacy behaviour and the PianoRoll convention).
    const float deltaForPan = std::abs (w.deltaX) > 0.001f ? w.deltaX
                                                              : w.deltaY;
    const auto step = (juce::int64) std::round (
        - (double) deltaForPan * 64.0 / juce::jmax (1.0e-5f, pixelsPerSample));
    scrollSamples = juce::jlimit<juce::int64> (0,
        juce::jmax<juce::int64> (0, anchorTimelineLength - 1),
        scrollSamples + step);
    repaint();
}

bool AudioRegionEditor::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey)
    {
        // First Esc clears an active range OR multi-selection; second
        // Esc closes. Matches the piano roll's "Esc clears selection
        // then closes" pattern.
        if (rangeActive)
        {
            rangeActive = false;
            repaint();
            return true;
        }
        if (! additionalSelectedRegions.empty())
        {
            additionalSelectedRegions.clear();
            repaint();
            return true;
        }
        if (onCloseRequested) onCloseRequested();
        return true;
    }
    const bool cmdOrCtrl = k.getModifiers().isCommandDown()
                              || k.getModifiers().isCtrlDown();
    const bool noMods = ! cmdOrCtrl && ! k.getModifiers().isShiftDown()
                        && ! k.getModifiers().isAltDown();

    // Cmd/Ctrl + Left / Right pans the view horizontally; Home / End jump
    // to the start / end of the anchor range. Lets the user scroll without
    // a trackpad - keeps zoomed-in editing workable on a plain mouse.
    if (cmdOrCtrl && (k == juce::KeyPress::leftKey || k == juce::KeyPress::rightKey))
    {
        const juce::int64 widthSamples = (juce::int64) std::round (
            (double) (getWidth() - 0) / juce::jmax (1.0e-9f, pixelsPerSample));
        const juce::int64 step = juce::jmax<juce::int64> (1, widthSamples / 4);
        const juce::int64 delta = (k == juce::KeyPress::leftKey ? -step : step);
        const juce::int64 maxStart = juce::jmax<juce::int64> (0,
                                        anchorTimelineLength - widthSamples);
        scrollSamples = juce::jlimit<juce::int64> (0, maxStart, scrollSamples + delta);
        repaint();
        return true;
    }
    if (k == juce::KeyPress::homeKey)
    {
        scrollSamples = 0;
        repaint();
        return true;
    }
    if (k == juce::KeyPress::endKey)
    {
        const juce::int64 widthSamples = (juce::int64) std::round (
            (double) getWidth() / juce::jmax (1.0e-9f, pixelsPerSample));
        scrollSamples = juce::jmax<juce::int64> (0, anchorTimelineLength - widthSamples);
        repaint();
        return true;
    }

    // Edit-mode shortcuts. The modal grabs keyboard focus on open so
    // MainComponent::keyPressed never sees these while the editor is up.
    // G/R/C also reclaim the global transport bindings (R=record toggle,
    // C=metronome toggle) for the duration of the modal — handled locally,
    // returns true so the global handler doesn't re-fire.
    if (noMods)
    {
        const auto ch = k.getTextCharacter();
        const bool isG = (ch == 'g' || ch == 'G');
        const bool isR = (ch == 'r' || ch == 'R');
        const bool isC = (ch == 'c' || ch == 'C');
        if (isG || isR || isC)
        {
            session.editMode = isG ? EditMode::Grab
                              : isR ? EditMode::Range
                                    : EditMode::Cut;
            syncEditModeToolbar();
            repaint();
            return true;
        }
    }

    // Local Cmd+Z / Cmd+Shift+Z. The engine's UndoManager has a global
    // keyboard binding, but it doesn't know to invalidate this editor's
    // view — so without this handler the audio data reverts but the
    // modal still paints the pre-undo geometry. Calling undo() here
    // and then repainting + refreshing the readouts keeps the view in
    // sync with the model on every step of the undo stack.
    // Use getKeyCode() rather than getTextCharacter(): the text char is
    // suppressed (returns 0 / a control byte) on most platforms while
    // Ctrl/Cmd is held, so the old check never fired the undo branch.
    {
        const int code = k.getKeyCode();
        if (cmdOrCtrl && (code == 'Z' || code == 'z'))
        {
            if (k.getModifiers().isShiftDown()) engine.getUndoManager().redo();
            else                                  engine.getUndoManager().undo();
            rangeActive = false;
            additionalSelectedRegions.clear();
            // Undo can shuffle region order (split-merge restores the
            // pre-split state but the focused index may now point past
            // the end if the editor was on a slice that got merged).
            // Clamp to a still-existing index, or close the modal when
            // the track has no regions at all.
            const auto regCount = (int) session.track (trackIdx).regions.size();
            if (regCount == 0)
            {
                if (onCloseRequested) onCloseRequested();
                return true;
            }
            if (regionIdx >= regCount) regionIdx = regCount - 1;
            if (regionIdx < 0)         regionIdx = 0;
            refreshStatusBarReadouts();
            repaint();
            return true;
        }
    }
    // Region navigation - same-track only, no wrap.
    if (cmdOrCtrl && (k.getKeyCode() == ']' || k.getTextCharacter() == ']'))
    { navigateRegion (+1); return true; }
    if (cmdOrCtrl && (k.getKeyCode() == '[' || k.getTextCharacter() == '['))
    { navigateRegion (-1); return true; }

    // Split: 'S' or Cmd+E. With a range active, splits at BOTH
    // boundaries (3 regions). Without a range, splits at the edit
    // cursor (existing single-cut behaviour from splitAtCursor()).
    const auto ch = k.getTextCharacter();
    const bool isSplit = (ch == 's' || ch == 'S')
                          || (cmdOrCtrl && (k.getKeyCode() == 'E' || k.getKeyCode() == 'e'));
    if (isSplit)
    {
        auto* r = region();
        if (r == nullptr) return true;
        if (rangeActive)
        {
            auto& um = engine.getUndoManager();
            um.beginNewTransaction ("Split range");
            // Split at the right boundary FIRST so the left boundary's
            // sample index stays valid in the same regions vector.
            const auto a = juce::jmin (rangeStartSample, rangeEndSample);
            const auto b = juce::jmax (rangeStartSample, rangeEndSample);
            const auto tlA = r->timelineStart + (a - r->sourceOffset);
            const auto tlB = r->timelineStart + (b - r->sourceOffset);
            um.perform (new SplitRegionAction (session, engine, trackIdx, regionIdx, tlB));
            um.perform (new SplitRegionAction (session, engine, trackIdx, regionIdx, tlA));
            rangeActive = false;
            refreshStatusBarReadouts();
            repaint();
            return true;
        }
        splitAtCursor();
        return true;
    }

    // Fade-fit: F = fade-in spans the selection length;
    // Shift+F = fade-out spans the selection length. Both are
    // single RegionEditAction edits, so Cmd+Z reverts cleanly.
    if ((ch == 'f' || ch == 'F') && rangeActive)
    {
        auto* r = region();
        if (r == nullptr) return true;
        const auto fadeLen = juce::jmax<juce::int64> (
            0, juce::jmax (rangeStartSample, rangeEndSample)
                 - juce::jmin (rangeStartSample, rangeEndSample));
        const AudioRegion before = *r;
        AudioRegion after = before;
        if (k.getModifiers().isShiftDown())
            after.fadeOutSamples = juce::jmin (fadeLen,
                                                  juce::jmax<juce::int64> (0, r->lengthInSamples - r->fadeInSamples));
        else
            after.fadeInSamples = juce::jmin (fadeLen,
                                                 juce::jmax<juce::int64> (0, r->lengthInSamples - r->fadeOutSamples));
        auto& um = engine.getUndoManager();
        um.beginNewTransaction (k.getModifiers().isShiftDown() ? "Fade-out to selection"
                                                                  : "Fade-in to selection");
        um.perform (new RegionEditAction (session, engine, trackIdx, regionIdx, before, after));
        refreshStatusBarReadouts();
        repaint();
        return true;
    }

    // Clipboard - bridges to the engine-wide region clipboard
    // shared with TapeStrip. Pattern matches TapeStrip's
    // copySelectedRegion / cutSelectedRegion / pasteAtPlayhead.
    //
    // With rangeActive: C/X operate on the SELECTED CHUNK rather than
    // the whole region. The clipboard region is constructed as if it
    // were a free-standing slice of the source file - same audio file,
    // sourceOffset advanced to the chunk's start, length = chunk
    // length. Paste then drops it as a new region at the edit cursor.
    if (cmdOrCtrl && (k.getKeyCode() == 'C' || k.getKeyCode() == 'c'))
    {
        auto* r = region();
        if (r == nullptr) return true;
        auto& clip = engine.getRegionClipboard();
        if (rangeActive)
        {
            const auto a = juce::jmin (rangeStartSample, rangeEndSample);
            const auto b = juce::jmax (rangeStartSample, rangeEndSample);
            AudioRegion chunk = *r;
            chunk.sourceOffset    = a;
            chunk.lengthInSamples = b - a;
            chunk.timelineStart   = 0;            // paste rewrites this
            chunk.fadeInSamples   = 0;            // chunk has no inherited fades
            chunk.fadeOutSamples  = 0;
            chunk.previousTakes.clear();          // takes don't carry across paste
            clip.region      = chunk;
        }
        else
        {
            clip.region = *r;
        }
        clip.sourceTrack = trackIdx;
        clip.hasContent  = true;
        return true;
    }
    if (cmdOrCtrl && (k.getKeyCode() == 'X' || k.getKeyCode() == 'x'))
    {
        auto* r = region();
        if (r == nullptr) return true;
        auto& clip = engine.getRegionClipboard();
        auto& um = engine.getUndoManager();

        if (rangeActive)
        {
            // Cut chunk: same shape as Copy + the Backspace-with-range
            // path. Editor stays open on the left slice (regionIdx
            // remains valid because SplitRegionAction inserts the
            // right slice at regionIdx+1).
            const auto a = juce::jmin (rangeStartSample, rangeEndSample);
            const auto b = juce::jmax (rangeStartSample, rangeEndSample);
            AudioRegion chunk = *r;
            chunk.sourceOffset    = a;
            chunk.lengthInSamples = b - a;
            chunk.timelineStart   = 0;
            chunk.fadeInSamples   = 0;
            chunk.fadeOutSamples  = 0;
            chunk.previousTakes.clear();
            clip.region      = chunk;
            clip.sourceTrack = trackIdx;
            clip.hasContent  = true;

            const auto tlA = r->timelineStart + (a - r->sourceOffset);
            const auto tlB = r->timelineStart + (b - r->sourceOffset);
            um.beginNewTransaction ("Cut chunk");
            um.perform (new SplitRegionAction (session, engine, trackIdx, regionIdx, tlB));
            um.perform (new SplitRegionAction (session, engine, trackIdx, regionIdx, tlA));
            um.perform (new DeleteRegionAction (session, engine, trackIdx, regionIdx + 1));
            rangeActive = false;
            refreshStatusBarReadouts();
            repaint();
            return true;
        }

        // Whole-region cut. Editor closes (its anchor region is gone).
        clip.region      = *r;
        clip.sourceTrack = trackIdx;
        clip.hasContent  = true;
        um.beginNewTransaction ("Cut region");
        um.perform (new DeleteRegionAction (session, engine, trackIdx, regionIdx));
        if (onCloseRequested) onCloseRequested();
        return true;
    }
    if (cmdOrCtrl && (k.getKeyCode() == 'V' || k.getKeyCode() == 'v'))
    {
        auto& clip = engine.getRegionClipboard();
        if (! clip.hasContent) return true;
        // Paste at the EDIT CURSOR's timeline position when the editor
        // owns the gesture. The editor's edit cursor is what the user
        // last clicked / dropped, so paste-at-cursor is what they
        // expect. Falls back to the transport playhead only when no
        // region is open (defensive; shouldn't fire today).
        auto* r = region();
        AudioRegion pasted = clip.region;
        if (r != nullptr)
            pasted.timelineStart = r->timelineStart + (editCursorSample - r->sourceOffset);
        else
            pasted.timelineStart = engine.getTransport().getPlayhead();
        const int targetTrack = (clip.sourceTrack >= 0 && clip.sourceTrack < Session::kNumTracks)
                                  ? clip.sourceTrack : trackIdx;
        auto& um = engine.getUndoManager();
        um.beginNewTransaction ("Paste region");
        um.perform (new PasteRegionAction (session, engine, targetTrack, pasted));
        return true;
    }

    // Delete - two paths.
    //   • Range active: split at both boundaries, then delete the
    //     middle slice. Region's anchor stays valid (the left slice
    //     keeps the original regionIdx) so the editor stays open.
    //   • No range: delete the whole region (Editor closes).
    // Both routed through UndoManager - Cmd+Z reverts.
    if (k == juce::KeyPress::backspaceKey || k == juce::KeyPress::deleteKey)
    {
        auto* r = region();
        if (rangeActive && r != nullptr)
        {
            auto& um = engine.getUndoManager();
            um.beginNewTransaction ("Delete chunk");
            const auto a = juce::jmin (rangeStartSample, rangeEndSample);
            const auto b = juce::jmax (rangeStartSample, rangeEndSample);
            const auto tlA = r->timelineStart + (a - r->sourceOffset);
            const auto tlB = r->timelineStart + (b - r->sourceOffset);
            // Split at B first, then A, so regionIdx still points at
            // the LEFT slice after both splits. The middle slice is
            // then regionIdx + 1.
            um.perform (new SplitRegionAction (session, engine, trackIdx, regionIdx, tlB));
            um.perform (new SplitRegionAction (session, engine, trackIdx, regionIdx, tlA));
            um.perform (new DeleteRegionAction (session, engine, trackIdx, regionIdx + 1));
            rangeActive = false;
            refreshStatusBarReadouts();
            repaint();
            return true;
        }
        auto& um = engine.getUndoManager();
        // Multi-select Delete: remove every additional region first
        // (descending index order so earlier deletes don't shift later
        // indices). Then delete the focused region last and close.
        if (! additionalSelectedRegions.empty())
        {
            std::vector<int> idxs = additionalSelectedRegions;
            idxs.push_back (regionIdx);
            std::sort (idxs.begin(), idxs.end(), std::greater<int>());
            idxs.erase (std::unique (idxs.begin(), idxs.end()), idxs.end());
            um.beginNewTransaction (idxs.size() > 1 ? "Delete regions" : "Delete region");
            for (int idx : idxs)
                um.perform (new DeleteRegionAction (session, engine, trackIdx, idx));
            additionalSelectedRegions.clear();
            if (onCloseRequested) onCloseRequested();
            return true;
        }
        um.beginNewTransaction ("Delete region");
        um.perform (new DeleteRegionAction (session, engine, trackIdx, regionIdx));
        if (onCloseRequested) onCloseRequested();
        return true;
    }

    // Move - arrow keys nudge timelineStart by 1 beat (Shift = 1 bar).
    // Up / Down are reserved for future gain-nudge; keeping them no-op
    // for now keeps the surface focused on horizontal motion.
    if (k == juce::KeyPress::leftKey || k == juce::KeyPress::rightKey)
    {
        auto* r = region();
        if (r == nullptr) return true;
        const double sr = juce::jmax (1.0, engine.getCurrentSampleRate());
        const double bpm = juce::jmax (1.0, (double) session.tempoBpm.load (std::memory_order_relaxed));
        const int    bpb = juce::jmax (1, session.beatsPerBar.load (std::memory_order_relaxed));
        const auto samplesPerBeat = (juce::int64) std::round (sr * 60.0 / bpm);
        const auto step = (k.getModifiers().isShiftDown() ? samplesPerBeat * bpb
                                                              : samplesPerBeat);
        const auto delta = (k == juce::KeyPress::leftKey ? -step : step);
        auto& um = engine.getUndoManager();
        um.beginNewTransaction (delta < 0 ? "Nudge region left" : "Nudge region right");
        auto nudgeOne = [&] (int idx)
        {
            if (idx < 0 || idx >= (int) session.track (trackIdx).regions.size()) return;
            auto& reg = session.track (trackIdx).regions[(size_t) idx];
            if (reg.locked) return;
            const AudioRegion before = reg;
            AudioRegion after = before;
            after.timelineStart = juce::jmax<juce::int64> (0, before.timelineStart + delta);
            if (after.timelineStart == before.timelineStart) return;
            um.perform (new RegionEditAction (session, engine, trackIdx, idx, before, after));
        };
        nudgeOne (regionIdx);
        for (int idx : additionalSelectedRegions) nudgeOne (idx);
        refreshStatusBarReadouts();
        repaint();
        return true;
    }

    // Zoom - '=' (or '+') and '-'. Anchors on the edit cursor so the
    // visible part of the slice stays under the user's finger across
    // a zoom step. Mirrors mouseWheelMove's Cmd+wheel ramp.
    if (ch == '=' || ch == '+' || ch == '-')
    {
        zoomByFactor (ch == '-' ? (1.0f / 1.15f) : 1.15f);
        return true;
    }
    return false;
}

void AudioRegionEditor::zoomByFactor (float factor)
{
    const auto* r = region();
    if (r == nullptr) return;
    const auto waveArea = juce::Rectangle<int> (
        0, kIconRowHeight + kRulerHeight,
        getWidth(),
        getHeight() - kIconRowHeight - kRulerHeight - kStatusBarH - kScrollBarH);
    const auto anchorXBefore = xForSample (editCursorSample, waveArea);
    pixelsPerSample = juce::jlimit (1.0e-5f, 1.0f, pixelsPerSample * factor);
    const auto anchorSampleAfterRaw = sampleForX (anchorXBefore, waveArea);
    scrollSamples += (editCursorSample - anchorSampleAfterRaw);
    scrollSamples = juce::jlimit<juce::int64> (0,
        juce::jmax<juce::int64> (0, anchorTimelineLength - 1), scrollSamples);
    repaint();
}

// ── Top icon-row buttons ──────────────────────────────────────────────
AudioRegionEditor::IconButton::IconButton (const juce::String& name, Glyph g)
    : juce::Button (name), glyph (g)
{
    setClickingTogglesState (false);
    // Don't steal keyboard focus from the host modal — otherwise a
    // follow-up Ctrl+Z (or any other modal hotkey) routes to the
    // button instead of the editor's keyPressed.
    setMouseClickGrabsKeyboardFocus (false);
    setWantsKeyboardFocus (false);
}

void AudioRegionEditor::IconButton::paintButton (juce::Graphics& g,
                                                    bool isMouseOver, bool isButtonDown)
{
    const auto bounds = getLocalBounds().toFloat().reduced (2.0f);
    auto disc = juce::Colour (0xff262630);
    if (isButtonDown)      disc = disc.brighter (0.14f);
    else if (isMouseOver)  disc = disc.brighter (0.08f);
    g.setColour (disc);
    g.fillEllipse (bounds);
    g.setColour (juce::Colour (0xff3a3a44));
    g.drawEllipse (bounds, isButtonDown ? 2.0f : 1.4f);

    const auto centre = bounds.getCentre();
    const float r = bounds.getWidth() * 0.30f;
    g.setColour (juce::Colour (0xffd0d0d0));

    auto strokeArrowhead = [&] (juce::Point<float> tip, juce::Point<float> from, float size)
    {
        const auto dir = (tip - from);
        const auto len = juce::jmax (0.001f, dir.getDistanceFromOrigin());
        const auto u   = dir / len;
        const auto perp = juce::Point<float> (-u.y, u.x);
        const auto a = tip - u * size + perp * size * 0.6f;
        const auto b = tip - u * size - perp * size * 0.6f;
        juce::Path p;
        p.startNewSubPath (a);
        p.lineTo (tip);
        p.lineTo (b);
        g.strokePath (p, juce::PathStrokeType (1.6f));
    };

    switch (glyph)
    {
        case Glyph::Undo:
        case Glyph::Redo:
        {
            const float radius = r * 0.95f;
            juce::Path arc;
            arc.addCentredArc (centre.x, centre.y, radius, radius,
                                  0.0f,
                                  juce::MathConstants<float>::pi * 0.30f,
                                  juce::MathConstants<float>::pi * 1.85f, true);
            if (glyph == Glyph::Redo)
                arc.applyTransform (juce::AffineTransform::scale (-1.0f, 1.0f, centre.x, centre.y));
            g.strokePath (arc, juce::PathStrokeType (1.6f));
            const float startAng = juce::MathConstants<float>::pi * 0.30f;
            juce::Point<float> start (centre.x + std::sin (startAng) * radius,
                                          centre.y - std::cos (startAng) * radius);
            if (glyph == Glyph::Redo) start = juce::Point<float> (2 * centre.x - start.x, start.y);
            const auto inwards = juce::Point<float> (centre.x, centre.y) - start;
            strokeArrowhead (start, start + inwards * 0.001f, 4.0f);
            break;
        }
        case Glyph::Split:
        {
            // Vertical bar with arrows pointing apart.
            const float h = r * 1.6f;
            g.fillRect (juce::Rectangle<float> (centre.x - 0.8f, centre.y - h * 0.5f, 1.6f, h));
            juce::Path l, ar;
            l.startNewSubPath (centre.x - r * 0.95f, centre.y);
            l.lineTo (centre.x - 2.0f, centre.y);
            ar.startNewSubPath (centre.x + 2.0f, centre.y);
            ar.lineTo (centre.x + r * 0.95f, centre.y);
            g.strokePath (l,  juce::PathStrokeType (1.4f));
            g.strokePath (ar, juce::PathStrokeType (1.4f));
            strokeArrowhead ({ centre.x - r * 0.95f, centre.y },
                                { centre.x - 2.0f,         centre.y }, 3.5f);
            strokeArrowhead ({ centre.x + r * 0.95f, centre.y },
                                { centre.x + 2.0f,         centre.y }, 3.5f);
            break;
        }
        case Glyph::Normalize:
        {
            // Waveform peak hitting a ceiling line.
            const float yTop = centre.y - r * 0.85f;
            const float yMid = centre.y;
            const float yBot = centre.y + r * 0.85f;
            // Ceiling line.
            g.drawLine (centre.x - r, yTop, centre.x + r, yTop, 1.2f);
            // Waveform peaks - bars of varying heights, the middle one
            // touching the ceiling.
            const float bw = 1.6f;
            const float xs[5] = { centre.x - r * 0.7f, centre.x - r * 0.35f, centre.x,
                                     centre.x + r * 0.35f, centre.x + r * 0.7f };
            const float hs[5] = { r * 0.4f, r * 0.7f, r * 0.85f, r * 0.55f, r * 0.3f };
            for (int i = 0; i < 5; ++i)
                g.fillRect (juce::Rectangle<float> (xs[i] - bw * 0.5f, yMid - hs[i],
                                                       bw, hs[i] * 2.0f));
            (void) yBot;
            break;
        }
        case Glyph::ZoomFit:
        {
            const float w = r * 0.95f;
            const float h = r * 1.10f;
            const float bracketLen = h * 0.9f;
            const float armLen     = r * 0.45f;
            g.drawLine (centre.x - w, centre.y - bracketLen * 0.5f,
                          centre.x - w, centre.y + bracketLen * 0.5f, 1.4f);
            g.drawLine (centre.x - w, centre.y - bracketLen * 0.5f,
                          centre.x - w + armLen, centre.y - bracketLen * 0.5f, 1.4f);
            g.drawLine (centre.x - w, centre.y + bracketLen * 0.5f,
                          centre.x - w + armLen, centre.y + bracketLen * 0.5f, 1.4f);
            g.drawLine (centre.x + w, centre.y - bracketLen * 0.5f,
                          centre.x + w, centre.y + bracketLen * 0.5f, 1.4f);
            g.drawLine (centre.x + w, centre.y - bracketLen * 0.5f,
                          centre.x + w - armLen, centre.y - bracketLen * 0.5f, 1.4f);
            g.drawLine (centre.x + w, centre.y + bracketLen * 0.5f,
                          centre.x + w - armLen, centre.y + bracketLen * 0.5f, 1.4f);
            g.fillEllipse (centre.x - 1.5f, centre.y - 1.5f, 3.0f, 3.0f);
            break;
        }
        case Glyph::ZoomIn:
        {
            // Magnifier with a "+" inside.
            const float rad = r * 0.62f;
            const float cx  = centre.x - r * 0.18f;
            const float cy  = centre.y - r * 0.18f;
            g.drawEllipse (cx - rad, cy - rad, rad * 2.0f, rad * 2.0f, 1.4f);
            g.drawLine (cx + rad * 0.55f, cy + rad * 0.55f,
                          centre.x + r * 0.85f, centre.y + r * 0.85f, 1.8f);
            g.drawLine (cx - rad * 0.5f, cy, cx + rad * 0.5f, cy, 1.4f);
            g.drawLine (cx, cy - rad * 0.5f, cx, cy + rad * 0.5f, 1.4f);
            break;
        }
        case Glyph::ZoomOut:
        {
            // Magnifier with a "-" inside.
            const float rad = r * 0.62f;
            const float cx  = centre.x - r * 0.18f;
            const float cy  = centre.y - r * 0.18f;
            g.drawEllipse (cx - rad, cy - rad, rad * 2.0f, rad * 2.0f, 1.4f);
            g.drawLine (cx + rad * 0.55f, cy + rad * 0.55f,
                          centre.x + r * 0.85f, centre.y + r * 0.85f, 1.8f);
            g.drawLine (cx - rad * 0.5f, cy, cx + rad * 0.5f, cy, 1.4f);
            break;
        }
        case Glyph::Properties:
        {
            g.drawEllipse (centre.x - r * 0.55f, centre.y - r * 0.55f,
                              r * 1.10f, r * 1.10f, 1.4f);
            for (int i = 0; i < 6; ++i)
            {
                const float ang = juce::MathConstants<float>::twoPi * (float) i / 6.0f;
                g.drawLine (centre.x + std::cos (ang) * r * 0.65f,
                              centre.y + std::sin (ang) * r * 0.65f,
                              centre.x + std::cos (ang) * r * 0.95f,
                              centre.y + std::sin (ang) * r * 0.95f, 1.4f);
            }
            g.fillEllipse (centre.x - 1.5f, centre.y - 1.5f, 3.0f, 3.0f);
            break;
        }
    }
}

void AudioRegionEditor::layoutIconRow (juce::Rectangle<int> area)
{
    auto inner = area.reduced (8, 6);
    const int dia = juce::jmin (inner.getHeight(), 36);
    const int gap = 8;
    auto place = [&] (IconButton& b)
    {
        b.setBounds (inner.removeFromLeft (dia).withSizeKeepingCentre (dia, dia));
        inner.removeFromLeft (gap);
    };
    // Zoom cluster anchored on the FAR RIGHT (Fit | + | -). Lay it out
    // first by removing from the right so it stays glued there as the
    // rest of the row grows.
    auto placeRight = [&] (IconButton& b)
    {
        b.setBounds (inner.removeFromRight (dia).withSizeKeepingCentre (dia, dia));
        inner.removeFromRight (gap);
    };
    placeRight (zoomFitButton);
    placeRight (zoomInButton);
    placeRight (zoomOutButton);
    inner.removeFromRight (8);
    // Chase toggle between zoom cluster and edit-mode palette.
    {
        const int chaseW = 56;
        chaseToggle.setBounds (inner.removeFromRight (chaseW)
                                  .withSizeKeepingCentre (chaseW, dia - 8));
        inner.removeFromRight (gap);
    }

    place (undoButton);
    place (redoButton);
    inner.removeFromLeft (gap);
    place (splitButton);
    place (normalizeButton);
    place (propertiesButton);
    inner.removeFromLeft (12);
    // Edit-mode palette + snap controls sized to match its internal
    // layout (5 buttons × 30 + gaps + Snap + denomination dropdown).
    if (editModeToolbar != nullptr)
    {
        constexpr int kToolbarWidth = 390;
        const int w = juce::jmin (kToolbarWidth, inner.getWidth());
        editModeToolbar->setBounds (inner.removeFromLeft (w));
        inner.removeFromLeft (gap);
    }
    // Automation param picker — sits between the edit-mode toolbar and
    // the title strip so it's discoverable without crowding the actions.
    {
        constexpr int kAutoBtnW = 130;
        const int w = juce::jmin (kAutoBtnW, inner.getWidth());
        if (w > 0)
        {
            automationParamButton.setBounds (
                inner.removeFromLeft (w).withSizeKeepingCentre (w, dia - 6));
            inner.removeFromLeft (gap);
        }
    }
    // Track name first, then the region title strip stretches to fill what
    // remains so long labels / filenames don't truncate.
    if (inner.getWidth() > 0)
    {
        const int tnW = juce::jmin (130, inner.getWidth() / 2);
        trackNameLabel.setBounds (inner.removeFromLeft (tnW).reduced (8, 2));
        titleLabel.setBounds (inner.reduced (8, 2));
    }
}

void AudioRegionEditor::syncEditModeToolbar()
{
    if (editModeToolbar != nullptr) editModeToolbar->syncFromSession();
}

void AudioRegionEditor::syncAutoCrossfades()
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return;
    auto& regs = session.track (trackIdx).regions;
    if (regs.empty()) return;

    // Sort indices by timelineStart so adjacent pairs match playback
    // ordering. We then know, for each region, its immediate
    // predecessor + successor in time.
    std::vector<int> order;
    order.reserve (regs.size());
    for (int i = 0; i < (int) regs.size(); ++i) order.push_back (i);
    std::sort (order.begin(), order.end(),
                [&] (int a, int b)
                {
                    return regs[(size_t) a].timelineStart
                         < regs[(size_t) b].timelineStart;
                });

    auto overlapWith = [&] (const AudioRegion& a, const AudioRegion& b) -> juce::int64
    {
        const auto aEnd = a.timelineStart + a.lengthInSamples;
        if (aEnd <= b.timelineStart) return 0;
        return juce::jmin (
            aEnd - b.timelineStart,
            juce::jmin (a.lengthInSamples, b.lengthInSamples));
    };

    auto& um = engine.getUndoManager();
    for (size_t pos = 0; pos < order.size(); ++pos)
    {
        const int idx = order[pos];
        auto& self = regs[(size_t) idx];
        if (self.locked) continue;

        juce::int64 overlapPrev = 0, overlapNext = 0;
        if (pos > 0)
            overlapPrev = overlapWith (regs[(size_t) order[pos - 1]], self);
        if (pos + 1 < order.size())
            overlapNext = overlapWith (self, regs[(size_t) order[pos + 1]]);

        const AudioRegion before = self;
        AudioRegion after = before;

        // Fade-in is governed by overlap with the preceding region.
        // Mutate only when the fade is auto-managed (or absent —
        // implicitly auto, will be promoted). User-pinned fades stay.
        if (after.fadeInAuto || after.fadeInSamples == 0)
        {
            if (overlapPrev > 0)
            {
                after.fadeInSamples = overlapPrev;
                if (after.fadeInShape == FadeShape::Linear)
                    after.fadeInShape = FadeShape::EqualPower;
                after.fadeInAuto = true;
            }
            else if (after.fadeInAuto)
            {
                after.fadeInSamples = 0;
                after.fadeInShape   = FadeShape::Linear;
                after.fadeInAuto    = false;
            }
        }

        // Fade-out governed by overlap with the following region.
        if (after.fadeOutAuto || after.fadeOutSamples == 0)
        {
            if (overlapNext > 0)
            {
                after.fadeOutSamples = overlapNext;
                if (after.fadeOutShape == FadeShape::Linear)
                    after.fadeOutShape = FadeShape::EqualPower;
                after.fadeOutAuto = true;
            }
            else if (after.fadeOutAuto)
            {
                after.fadeOutSamples = 0;
                after.fadeOutShape   = FadeShape::Linear;
                after.fadeOutAuto    = false;
            }
        }

        // Re-clamp so fadeIn + fadeOut stays within the region length.
        after.fadeInSamples  = juce::jlimit<juce::int64> (
            0, after.lengthInSamples, after.fadeInSamples);
        after.fadeOutSamples = juce::jlimit<juce::int64> (
            0, after.lengthInSamples - after.fadeInSamples,
            after.fadeOutSamples);

        if (after.fadeInSamples  == before.fadeInSamples
            && after.fadeOutSamples == before.fadeOutSamples
            && after.fadeInShape    == before.fadeInShape
            && after.fadeOutShape   == before.fadeOutShape
            && after.fadeInAuto     == before.fadeInAuto
            && after.fadeOutAuto    == before.fadeOutAuto)
            continue;

        self = before;   // RegionEditAction.perform re-applies after
        um.perform (new RegionEditAction (
            session, engine, trackIdx, idx, before, after));
    }
}

void AudioRegionEditor::layoutStatusBar (juce::Rectangle<int> area)
{
    auto inner = area.reduced (8, 4);
    positionLabel.setBounds (inner.removeFromLeft (110));
    inner.removeFromLeft (4);
    gainLabel    .setBounds (inner.removeFromLeft (84));
    inner.removeFromLeft (4);
    fadeLabel    .setBounds (inner.removeFromLeft (162));
    inner.removeFromLeft (10);

    // Right-aligned cluster: lock, mute, info readout.
    lockToggle.setBounds (inner.removeFromRight (60));
    inner.removeFromRight (4);
    muteToggle.setBounds (inner.removeFromRight (60));
    inner.removeFromRight (10);
    infoLabel .setBounds (inner);
}

juce::String formatBarBeatTickAt (juce::int64 sliceFileSample,
                                     const AudioRegion& r,
                                     double sampleRate, double bpm, int beatsPerBar)
{
    const double samplesPerBeat = juce::jmax (1.0, sampleRate * 60.0 / juce::jmax (1.0, bpm));
    const double samplesPerBar  = samplesPerBeat * juce::jmax (1, beatsPerBar);
    // Convert file-sample → timeline-sample (slice-relative offset added to timelineStart).
    const double timelineSample = (double) r.timelineStart
                                    + (double) (sliceFileSample - r.sourceOffset);
    const double bar  = std::floor (timelineSample / samplesPerBar);
    const double rem1 = timelineSample - bar * samplesPerBar;
    const double beat = std::floor (rem1 / samplesPerBeat);
    const double rem2 = rem1 - beat * samplesPerBeat;
    const int    sub  = (int) std::round ((rem2 / samplesPerBeat) * 1000.0);
    return juce::String ((int) bar + 1) + "."
         + juce::String ((int) beat + 1) + "."
         + juce::String (juce::jlimit (0, 999, sub)).paddedLeft ('0', 3);
}

void AudioRegionEditor::refreshStatusBarReadouts()
{
    const auto* r = region();
    // Use the WAV's native SR — same reasoning as paintRuler /
    // paintWaveform: anchorTimelineLength + lengthInSamples were
    // captured at recording-time SR. Reading at engine SR drifts the
    // bar/beat display vs the rendered audio after a device hot-swap.
    const double sr  = juce::jmax (1.0,
        loadedFileSampleRate > 0.0 ? loadedFileSampleRate
                                    : engine.getCurrentSampleRate());
    const double bpm = juce::jmax (1.0, (double) session.tempoBpm.load (std::memory_order_relaxed));
    const int    bpb = juce::jmax (1, session.beatsPerBar.load (std::memory_order_relaxed));

    if (r != nullptr)
    {
        const auto mode = (TimeDisplayMode) session.timeDisplayMode.load (std::memory_order_relaxed);
        const auto timelineSample = r->timelineStart + (editCursorSample - r->sourceOffset);
        positionLabel.setText ("pos " + formatSamplePosition (timelineSample, sr,
                                                                (float) bpm, bpb, mode),
                                  juce::dontSendNotification);
        gainLabel.setText (juce::String (r->gainDb, 1) + " dB",
                              juce::dontSendNotification);
        const double fadeInMs  = (double) r->fadeInSamples  * 1000.0 / sr;
        const double fadeOutMs = (double) r->fadeOutSamples * 1000.0 / sr;
        fadeLabel.setText ("fade " + juce::String (fadeInMs, 0)
                              + " / " + juce::String (fadeOutMs, 0) + " ms",
                              juce::dontSendNotification);
        const int channels = juce::jmax (1, r->numChannels);
        const double durSec = (double) r->lengthInSamples / sr;
        const int mins = (int) (durSec / 60.0);
        const double secs = durSec - mins * 60.0;
        infoLabel.setText (juce::String (channels) + "ch - "
                              + juce::String ((int) std::round (sr / 1000.0)) + " kHz - "
                              + juce::String (mins) + ":"
                              + juce::String (secs, 3),
                              juce::dontSendNotification);
        muteToggle.setToggleState (r->muted,  juce::dontSendNotification);
        lockToggle.setToggleState (r->locked, juce::dontSendNotification);

        // Title: prefer user label, fall back to filename. Skip update
        // while the label is in edit mode so we don't stomp typed input.
        if (! titleLabel.isBeingEdited())
        {
            const auto displayTitle = r->label.isNotEmpty()
                                          ? r->label
                                          : r->file.getFileName();
            titleLabel.setText (displayTitle, juce::dontSendNotification);
        }

        // Keep the track-name header in sync if the strip is renamed while
        // the editor is open.
        trackNameLabel.setText (session.track (trackIdx).name, juce::dontSendNotification);
    }
}

void AudioRegionEditor::splitAtCursor()
{
    auto* r = region();
    if (r == nullptr) return;
    const auto timelineSplit = r->timelineStart + (editCursorSample - r->sourceOffset);
    auto& um = engine.getUndoManager();
    um.beginNewTransaction ("Split region");
    um.perform (new SplitRegionAction (session, engine, trackIdx, regionIdx, timelineSplit));
    refreshStatusBarReadouts();
    repaint();
}

void AudioRegionEditor::zoomFit()
{
    const auto waveArea = juce::Rectangle<int> (0, kIconRowHeight + kRulerHeight,
                                                   getWidth(),
                                                   getHeight() - kIconRowHeight - kRulerHeight - kStatusBarH - kScrollBarH);
    zoomFitToArea (waveArea);
    repaint();
}

void AudioRegionEditor::normalizeRegion()
{
    auto* r = region();
    if (r == nullptr || ! r->file.existsAsFile() || r->lengthInSamples <= 0) return;

    // Non-destructive: scan the slice in chunks to find peak, then adjust
    // gainDb so peak hits ~-0.1 dBFS. Source file untouched. Chunked so a
    // multi-minute region doesn't allocate gigabytes.
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (r->file));
    if (reader == nullptr) return;

    const int channels = juce::jmax (1, (int) reader->numChannels);
    constexpr int kChunkSamples = 1 << 16;   // 64k frames per chunk
    juce::AudioBuffer<float> buf (channels, kChunkSamples);

    float peak = 0.0f;
    juce::int64 done = 0;
    while (done < r->lengthInSamples)
    {
        const int thisChunk = (int) juce::jmin<juce::int64> (kChunkSamples,
                                                                  r->lengthInSamples - done);
        reader->read (&buf, 0, thisChunk, r->sourceOffset + done, true, true);
        for (int c = 0; c < channels; ++c)
        {
            const auto rng = buf.findMinMax (c, 0, thisChunk);
            peak = juce::jmax (peak, std::abs (rng.getStart()), std::abs (rng.getEnd()));
        }
        done += thisChunk;
    }
    if (peak <= 1.0e-6f) return;   // silence; nothing to normalize

    const float targetPeak = 0.99f;   // -0.087 dBFS
    const float deltaDb    = juce::Decibels::gainToDecibels (targetPeak / peak);

    const AudioRegion before = *r;
    AudioRegion after = before;
    after.gainDb = juce::jlimit (-24.0f, 12.0f, before.gainDb + deltaDb);

    auto& um = engine.getUndoManager();
    um.beginNewTransaction ("Normalize");
    um.perform (new RegionEditAction (session, engine, trackIdx, regionIdx, before, after));
    refreshStatusBarReadouts();
    repaint();
}

void AudioRegionEditor::showRegionPropertiesPopup()
{
    auto* r = region();
    if (r == nullptr) return;

    juce::PopupMenu m;
    m.addSectionHeader (juce::String::formatted ("Track %d  region %d",
                                                    trackIdx + 1, regionIdx + 1));

    // Rename / label - AlertWindow modal text input. Lambda captures
    // a SafePointer so the editor's death between menu-show and OK
    // is a no-op rather than a use-after-free.
    const auto currentLabel = r->label;
    const juce::String renameLabel = currentLabel.isEmpty()
        ? juce::String ("Add label...")
        : juce::String ("Rename label...");
    m.addItem (renameLabel,
                [safe = juce::Component::SafePointer<AudioRegionEditor> (this),
                 currentLabel]
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr) return;
                    // Fall back to the editor component itself if no top-
                    // level exists — the rename should still proceed; the
                    // modal just renders centred on the editor instead of
                    // the whole window.
                    auto* host = self->getTopLevelComponent();
                    if (host == nullptr) host = self;
                    showDuskTextInput (*host, "Audio region label",
                                          "Type a label for this region:",
                                          currentLabel,
                                          [safe] (const juce::String& text)
                                          {
                                              auto* s = safe.getComponent();
                                              if (s == nullptr) return;
                                              auto* rr = s->region();
                                              if (rr == nullptr) return;
                                              const AudioRegion before = *rr;
                                              AudioRegion after = before;
                                              after.label = text;
                                              auto& um = s->engine.getUndoManager();
                                              um.beginNewTransaction ("Rename region");
                                              um.perform (new RegionEditAction (
                                                  s->session, s->engine,
                                                  s->trackIdx, s->regionIdx,
                                                  before, after));
                                              s->refreshStatusBarReadouts();
                                              s->repaint();
                                          });
                });

    m.addSeparator();

    // Mute / Lock toggles - same path as the status-bar checkboxes,
    // exposed here as a discoverable surface alongside rename/colour.
    m.addItem (r->muted ? "Unmute region" : "Mute region",
                [safe = juce::Component::SafePointer<AudioRegionEditor> (this)]
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr) return;
                    auto* rr = self->region();
                    if (rr == nullptr) return;
                    const AudioRegion before = *rr;
                    AudioRegion after = before;
                    after.muted = ! before.muted;
                    auto& um = self->engine.getUndoManager();
                    um.beginNewTransaction (after.muted ? "Mute region" : "Unmute region");
                    um.perform (new RegionEditAction (self->session, self->engine,
                                                        self->trackIdx, self->regionIdx,
                                                        before, after));
                    self->muteToggle.setToggleState (after.muted, juce::dontSendNotification);
                    self->refreshStatusBarReadouts();
                    self->repaint();
                });
    m.addItem (r->locked ? "Unlock region" : "Lock region",
                [safe = juce::Component::SafePointer<AudioRegionEditor> (this)]
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr) return;
                    auto* rr = self->region();
                    if (rr == nullptr) return;
                    const AudioRegion before = *rr;
                    AudioRegion after = before;
                    after.locked = ! before.locked;
                    auto& um = self->engine.getUndoManager();
                    um.beginNewTransaction (after.locked ? "Lock region" : "Unlock region");
                    um.perform (new RegionEditAction (self->session, self->engine,
                                                        self->trackIdx, self->regionIdx,
                                                        before, after));
                    self->lockToggle.setToggleState (after.locked, juce::dontSendNotification);
                    self->refreshStatusBarReadouts();
                    self->repaint();
                });

    m.addSeparator();

    // Colour submenu - 8 swatches + "Reset to track colour".
    juce::PopupMenu colourSub;
    for (int i = 0; i < kPaletteCount; ++i)
    {
        const bool isReset = (i == 0);
        colourSub.addItem (5000 + i,
                            kPalette[i].label,
                            true,
                            isReset ? r->customColour.isTransparent()
                                    : r->customColour.getARGB() == kPalette[i].argb);
    }
    m.addSubMenu ("Color", colourSub);

    m.addSeparator();
    // Delete - no confirmation, trust the UndoManager. Closing the
    // editor on success matches "the region you were editing is gone";
    // the host pops the modal away.
    m.addItem ("Delete region",
                [safe = juce::Component::SafePointer<AudioRegionEditor> (this)]
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr) return;
                    auto& um = self->engine.getUndoManager();
                    um.beginNewTransaction ("Delete region");
                    um.perform (new DeleteRegionAction (self->session, self->engine,
                                                          self->trackIdx, self->regionIdx));
                    if (self->onCloseRequested) self->onCloseRequested();
                });

    // Read-only metadata footer. Disabled item so it can't be picked
    // but still reads as part of the menu.
    m.addSeparator();
    {
        const double sr = juce::jmax (1.0, engine.getCurrentSampleRate());
        const double durSec = (double) r->lengthInSamples / sr;
        const int mins = (int) (durSec / 60.0);
        const double secRem = durSec - 60.0 * mins;
        const auto info = r->file.getFileName() + "  -  "
                            + juce::String ((int) std::round (sr / 1000.0)) + " kHz  -  "
                            + juce::String (r->numChannels) + "ch  -  "
                            + juce::String (mins)
                            + ":" + juce::String (secRem, 3).paddedLeft ('0', 6);
        m.addItem (9999, info, false /*disabled*/, false);
    }

    duskstudio::showContextMenu (m, propertiesButton,
        [safe = juce::Component::SafePointer<AudioRegionEditor> (this)] (int chosen)
        {
            auto* self = safe.getComponent();
            if (self == nullptr) return;
            // Only the colour-submenu IDs land here; the per-item
            // lambdas above handle everything else.
            if (chosen < 5000 || chosen >= 5000 + kPaletteCount) return;
            auto* rr = self->region();
            if (rr == nullptr) return;
            const juce::Colour newColour { kPalette[chosen - 5000].argb };
            const AudioRegion before = *rr;
            if (before.customColour == newColour) return;
            AudioRegion after = before;
            after.customColour = newColour;
            auto& um = self->engine.getUndoManager();
            um.beginNewTransaction ("Set region colour");
            um.perform (new RegionEditAction (self->session, self->engine,
                                                self->trackIdx, self->regionIdx,
                                                before, after));
            self->refreshStatusBarReadouts();
            self->repaint();
        });
}

int AudioRegionEditor::nextRegionIndex (int delta) const
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return -1;
    const auto& regs = session.track (trackIdx).regions;
    if (regionIdx < 0 || regionIdx >= (int) regs.size()) return -1;

    // Sort by (timelineStart, storageIndex) so ties are deterministic.
    // delta=+1 → smallest key strictly greater than current's key;
    // delta=-1 → largest key strictly less than current's key.
    using Key = std::pair<juce::int64, int>;
    const Key cur { regs[(size_t) regionIdx].timelineStart, regionIdx };
    int bestIdx = -1;
    Key best = (delta > 0)
        ? Key { std::numeric_limits<juce::int64>::max(), std::numeric_limits<int>::max() }
        : Key { std::numeric_limits<juce::int64>::min(), std::numeric_limits<int>::min() };
    for (int i = 0; i < (int) regs.size(); ++i)
    {
        if (i == regionIdx) continue;
        const Key k { regs[(size_t) i].timelineStart, i };
        if (delta > 0 ? (k > cur && k < best) : (k < cur && k > best))
        {
            best = k;
            bestIdx = i;
        }
    }
    return bestIdx;
}

void AudioRegionEditor::navigateRegion (int delta)
{
    const int newIdx = nextRegionIndex (delta);
    if (newIdx < 0) return;     // boundary: no neighbour, no-op
    if (onNavigateToRegion) onNavigateToRegion (trackIdx, newIdx);
}

int AudioRegionEditor::transportPlayheadX (juce::Rectangle<int> waveArea) const
{
    // Map the transport playhead via the anchor TIMELINE window so the
    // line shows up across the entire wave area — including over the
    // dimmed neighborhood slices and the gaps between them — not just
    // inside the focused region's file-sample range.
    const auto playheadTimeline = engine.getTransport().getPlayhead();
    if (anchorTimelineLength <= 0) return -1;
    if (playheadTimeline < anchorTimelineStart
        || playheadTimeline >= anchorTimelineStart + anchorTimelineLength)
        return -1;
    const int x = xForTimelineSample (playheadTimeline, waveArea);
    if (x < waveArea.getX() || x > waveArea.getRight()) return -1;
    return x;
}

void AudioRegionEditor::paintTransportPlayhead (juce::Graphics& g,
                                                  juce::Rectangle<int> waveArea)
{
    // Cache-vs-live decision by clip width — see the matching
    // comment in PianoRollComponent::paintTransportPlayhead for the
    // full rationale. Narrow clip → cache (avoids torn line from
    // live-read drift past the queued dirty band). Wide clip →
    // live (cache may be stale after zoom/scroll/region change).
    constexpr int kNarrowBandWidth = 64;
    const int clipW = g.getClipBounds().getWidth();
    const bool useCache = clipW > 0
                            && clipW < kNarrowBandWidth
                            && lastPlayheadX >= 0;
    const int x = useCache ? lastPlayheadX : transportPlayheadX (waveArea);
    if (x < 0) return;
    g.setColour (kTransportPlayhead.withAlpha (0.85f));
    g.drawVerticalLine (x, (float) waveArea.getY(), (float) waveArea.getBottom());
}

void AudioRegionEditor::timerCallback()
{
    // Keep the horizontal scrollbar in sync with whichever path mutated
    // scrollSamples since the last tick (keyboard arrows, mouse wheel,
    // zoom recentre, etc.). One cheap call per tick beats threading
    // notifications through every scroll-changing site.
    syncScrollBarRange();

    // Toolbar button enabled-state + toggle sync. UndoManager has no
    // change-broadcaster we listen to and the modal can't see edits made
    // through external surfaces (TapeStrip drag, RecordManager commit)
    // any other way. Polling at 30 Hz is cheap.
    {
        const auto* r = region();
        auto& um = engine.getUndoManager();
        undoButton .setEnabled (um.canUndo());
        redoButton .setEnabled (um.canRedo());
        splitButton    .setEnabled (r != nullptr);
        normalizeButton.setEnabled (r != nullptr);
        propertiesButton.setEnabled (r != nullptr);
        if (r != nullptr)
        {
            if (muteToggle.getToggleState() != r->muted)
                muteToggle.setToggleState (r->muted, juce::dontSendNotification);
            if (lockToggle.getToggleState() != r->locked)
                lockToggle.setToggleState (r->locked, juce::dontSendNotification);
        }
    }
    const auto waveArea = juce::Rectangle<int> (
        0, kIconRowHeight + kRulerHeight,
        getWidth(),
        getHeight() - kIconRowHeight - kRulerHeight - kStatusBarH - kScrollBarH);

    // Cache the per-tick playhead X — used by both the chase logic and
    // the standard repaint-when-the-line-moves check.
    int x = transportPlayheadX (waveArea);

    // Chase: when on and transport is playing AND the playhead is inside
    // the anchor range, scroll the view forward so the playhead lands at
    // left-quarter after the jump. The anchor-range guard prevents 30 Hz
    // repaint storms when the playhead is OUTSIDE the region entirely -
    // transportPlayheadX returns -1 for "out of anchor", which would
    // otherwise trip the xNow < 0 branch every tick.
    if (chaseToggle.getToggleState() && engine.getTransport().isPlaying()
        && pixelsPerSample > 0.0f && anchorTimelineLength > 0)
    {
        const auto playhead   = engine.getTransport().getPlayhead();
        const juce::int64 rel = playhead - anchorTimelineStart;
        if (rel >= 0 && rel < anchorTimelineLength && x < 0)
        {
            const juce::int64 viewSamples = (juce::int64) std::round (
                (double) getWidth() / pixelsPerSample);
            const juce::int64 maxStart = juce::jmax<juce::int64> (0,
                                            anchorTimelineLength - viewSamples);
            scrollSamples = juce::jlimit<juce::int64> (0, maxStart,
                                rel - viewSamples / 4);
            repaint (waveArea);
            x = transportPlayheadX (waveArea);   // playhead is in view now
        }
    }

    if (x == lastPlayheadX) return;
    // Repaint just the strip the playhead vacated AND the strip it
    // entered - 4 px wide each so the line + a tiny halo redraws
    // cleanly without invalidating the whole waveform area.
    auto invalidateAt = [this, &waveArea] (int xx)
    {
        if (xx < 0) return;
        repaint (juce::Rectangle<int> (xx - 2, waveArea.getY(),
                                          5, waveArea.getHeight()));
    };
    invalidateAt (lastPlayheadX);
    invalidateAt (x);
    lastPlayheadX = x;
}

// ── Automation overlay ────────────────────────────────────────────────────

namespace
{
const char* automationParamName (int p)
{
    switch ((AutomationParam) p)
    {
        case AutomationParam::FaderDb:  return "Fader (dB)";
        case AutomationParam::Pan:      return "Pan";
        case AutomationParam::Mute:     return "Mute";
        case AutomationParam::Solo:     return "Solo";
        case AutomationParam::AuxSend1: return "Aux 1";
        case AutomationParam::AuxSend2: return "Aux 2";
        case AutomationParam::AuxSend3: return "Aux 3";
        case AutomationParam::AuxSend4: return "Aux 4";
        default: return "Off";
    }
}

juce::Colour automationParamColour (int p)
{
    switch ((AutomationParam) p)
    {
        case AutomationParam::FaderDb:  return juce::Colour (0xff5fc46f);   // green
        case AutomationParam::Pan:      return juce::Colour (0xffe07a40);   // orange
        case AutomationParam::Mute:     return juce::Colour (0xffd05050);   // red
        case AutomationParam::AuxSend1: return juce::Colour (0xff60a8d8);
        case AutomationParam::AuxSend2: return juce::Colour (0xff70b0c0);
        case AutomationParam::AuxSend3: return juce::Colour (0xff8090d0);
        case AutomationParam::AuxSend4: return juce::Colour (0xffa080c0);
        default:                         return juce::Colour (0xff909098);
    }
}
} // namespace

void AudioRegionEditor::showAutomationParamMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader ("Automate parameter");
    m.addItem (1000, "Off", true, automationParam < 0);
    m.addSeparator();
    static constexpr int kParams[] = {
        (int) AutomationParam::FaderDb,
        (int) AutomationParam::Pan,
        (int) AutomationParam::Mute,
        (int) AutomationParam::Solo,
        (int) AutomationParam::AuxSend1,
        (int) AutomationParam::AuxSend2,
        (int) AutomationParam::AuxSend3,
        (int) AutomationParam::AuxSend4,
    };
    for (int p : kParams)
        m.addItem (p + 1, automationParamName (p), true, automationParam == p);

    juce::Component::SafePointer<AudioRegionEditor> safe (this);
    duskstudio::showContextMenu (m, automationParamButton,
        [safe] (int chosen)
        {
            if (safe == nullptr || chosen == 0) return;
            if (chosen == 1000)
                safe->automationParam = -1;
            else
                safe->automationParam = chosen - 1;
            safe->automationParamButton.setButtonText (
                juce::String ("Auto: ") + automationParamName (safe->automationParam));
            safe->repaint();
        });
}

juce::Rectangle<int> AudioRegionEditor::automationLaneArea (juce::Rectangle<int> waveArea) const
{
    // Overlay sits OVER the waveform — not a separate band — so the lane
    // line + dots stay aligned with the audio they're shaping. Inset
    // matches paintWaveform's reduced(4,4) so dots don't clip the frame.
    return waveArea.reduced (4, 4);
}

int AudioRegionEditor::automationYForValue (float v01, juce::Rectangle<int> waveArea) const
{
    const auto a = automationLaneArea (waveArea);
    const float clamped = juce::jlimit (0.0f, 1.0f, v01);
    return a.getBottom() - (int) std::round (clamped * (float) a.getHeight());
}

float AudioRegionEditor::automationValueForY (int y, juce::Rectangle<int> waveArea) const
{
    const auto a = automationLaneArea (waveArea);
    if (a.getHeight() <= 0) return 0.0f;
    const float frac = (float) (a.getBottom() - y) / (float) a.getHeight();
    return juce::jlimit (0.0f, 1.0f, frac);
}

int AudioRegionEditor::hitTestAutomationPoint (int x, int y,
                                                  juce::Rectangle<int> waveArea) const
{
    if (automationParam < 0 || automationParam >= kNumAutomationParams) return -1;
    const auto& lane = session.track (trackIdx)
                            .automationLanes[(size_t) automationParam];
    constexpr int kHitR = 8;
    for (int i = 0; i < (int) lane.points.size(); ++i)
    {
        const auto& pt = lane.points[(size_t) i];
        const int px = xForTimelineSample (pt.timeSamples, waveArea);
        const int py = automationYForValue (pt.value, waveArea);
        if (std::abs (px - x) <= kHitR && std::abs (py - y) <= kHitR)
            return i;
    }
    return -1;
}

void AudioRegionEditor::paintAutomationOverlay (juce::Graphics& g, juce::Rectangle<int> waveArea)
{
    if (automationParam < 0 || automationParam >= kNumAutomationParams) return;
    const auto& lane = session.track (trackIdx)
                            .automationLanes[(size_t) automationParam];
    const auto colour = automationParamColour (automationParam);
    const auto laneArea = automationLaneArea (waveArea);

    // Header strip: param name + value scale hint at the top-right of
    // the wave area so the user knows which lane they're editing.
    {
        g.setColour (colour.withAlpha (0.85f));
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawText (juce::String ("AUTO: ") + automationParamName (automationParam),
                     waveArea.removeFromTop (16).reduced (6, 2),
                     juce::Justification::topLeft, false);
    }

    // Empty lane: just paint the baseline ghost line.
    if (lane.points.empty())
    {
        g.setColour (colour.withAlpha (0.25f));
        const int midY = laneArea.getY() + laneArea.getHeight() / 2;
        g.drawHorizontalLine (midY, (float) laneArea.getX(), (float) laneArea.getRight());
        return;
    }

    // Polyline through the points. Step-hold for discrete (Mute / Solo),
    // linear otherwise. evaluateLane already encodes the right semantic
    // but for the overlay we just connect dots — same visual idiom Reaper
    // / Ardour use for envelope lanes.
    const bool discrete = ! isContinuousParam ((AutomationParam) automationParam);
    juce::Path line;
    bool first = true;
    int prevX = laneArea.getX(), prevY = laneArea.getBottom();
    for (const auto& pt : lane.points)
    {
        const int px = xForTimelineSample (pt.timeSamples, laneArea);
        const int py = automationYForValue (pt.value, laneArea);
        if (first)
        {
            line.startNewSubPath ((float) px, (float) py);
            first = false;
        }
        else
        {
            if (discrete)
                line.lineTo ((float) px, (float) prevY);
            line.lineTo ((float) px, (float) py);
        }
        prevX = px;
        prevY = py;
    }
    g.setColour (colour.withAlpha (0.9f));
    g.strokePath (line, juce::PathStrokeType (1.6f));

    // Dots
    for (const auto& pt : lane.points)
    {
        const int px = xForTimelineSample (pt.timeSamples, laneArea);
        const int py = automationYForValue (pt.value, laneArea);
        g.setColour (juce::Colour (0xff0a0a0a));
        g.fillEllipse ((float) px - 5.0f, (float) py - 5.0f, 10.0f, 10.0f);
        g.setColour (colour);
        g.fillEllipse ((float) px - 3.5f, (float) py - 3.5f, 7.0f, 7.0f);
    }
}
} // namespace duskstudio
