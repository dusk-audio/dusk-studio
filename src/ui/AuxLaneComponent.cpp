#include "AuxLaneComponent.h"
#include "DuskContextMenu.h"
#include "HardwareInsertEditor.h"
#include "PlatformWindowing.h"
#include "PluginPickerHelpers.h"
#include "../dsp/AuxLaneStrip.h"
#include "../engine/AudioEngine.h"
#include "../engine/PluginSlot.h"
#include "../engine/Transport.h"
#include "../session/MidiBindings.h"

namespace duskstudio
{
namespace
{
constexpr float kMeterMinDb = -60.0f;
constexpr float kMeterMaxDb =   6.0f;

float dbToMeterFrac (float db) noexcept
{
    if (db <= kMeterMinDb) return 0.0f;
    if (db >= kMeterMaxDb) return 1.0f;
    return (db - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb);
}

juce::Colour meterColourForFrac (float frac) noexcept
{
    // Green up to 0 dB (frac ~0.91), yellow into +3, red into clipping.
    if (frac >= 0.97f) return juce::Colour (0xffd04040);
    if (frac >= 0.91f) return juce::Colour (0xffd0a040);
    return juce::Colour (0xff60c060);
}
} // namespace

class AuxLaneComponent::StripMeter final : public juce::Component
{
public:
    explicit StripMeter (const AuxLaneParams& p) : params (p) {}

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff1c1c22));
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (juce::Colour (0xff34343c));
        g.drawRoundedRectangle (r.reduced (0.5f), 2.0f, 1.0f);

        const float lDb = params.meterPostL.load (std::memory_order_relaxed);
        const float rDb = params.meterPostR.load (std::memory_order_relaxed);
        const float lFrac = dbToMeterFrac (lDb);
        const float rFrac = dbToMeterFrac (rDb);

        const float w = r.getWidth();
        const float h = r.getHeight();
        const float colW = (w - 2.0f) / 2.0f;

        auto drawCol = [&] (float x, float frac)
        {
            const float fillH = h * frac;
            juce::Rectangle<float> bar (x, h - fillH, colW, fillH);
            g.setColour (meterColourForFrac (frac));
            g.fillRect (bar);
        };

        drawCol (1.0f,          lFrac);
        drawCol (1.0f + colW,   rFrac);

        // Zero-dB tick + -20 dB tick.
        const float zeroY = h * (1.0f - dbToMeterFrac (0.0f));
        g.setColour (juce::Colour (0xc0d04040));
        g.drawLine (0.0f, zeroY, w, zeroY, 1.0f);
        const float minus20Y = h * (1.0f - dbToMeterFrac (-20.0f));
        g.setColour (juce::Colour (0x60ffffff));
        g.drawLine (0.0f, minus20Y, w, minus20Y, 1.0f);

        // L / R glyphs at the bottom so the user can see the bar exists
        // even with no audio coming through.
        g.setColour (juce::Colour (0xff70707a));
        g.setFont (juce::Font (juce::FontOptions (9.0f)));
        g.drawText ("L", juce::Rectangle<float> (1.0f, h - 12.0f, colW, 10.0f),
                    juce::Justification::centred, false);
        g.drawText ("R", juce::Rectangle<float> (1.0f + colW, h - 12.0f, colW, 10.0f),
                    juce::Justification::centred, false);
    }

private:
    const AuxLaneParams& params;
};

class AuxLaneComponent::SendSourcePanel final : public juce::Component
{
public:
    SendSourcePanel (Session& s, int laneIdx) : session (s), laneIndex (laneIdx) {}

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff141418));
        g.fillRoundedRectangle (bounds, 4.0f);
        g.setColour (juce::Colour (0xff2a2a2e));
        g.drawRoundedRectangle (bounds, 4.0f, 1.0f);

        auto inner = getLocalBounds().reduced (8);
        g.setColour (juce::Colour (0xffb0b0b8));
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        auto header = inner.removeFromTop (16);
        g.drawText ("SEND SOURCES", header, juce::Justification::centredLeft, false);
        inner.removeFromTop (4);

        const int rowH = juce::jmax (14, inner.getHeight() / Session::kNumTracks);
        g.setFont (juce::Font (juce::FontOptions (11.0f)));

        for (int i = 0; i < Session::kNumTracks; ++i)
        {
            auto row = inner.removeFromTop (rowH);
            if (row.getHeight() <= 0) break;

            const auto& tr = session.track (i);
            const float liveDb = tr.strip.liveAuxSendDb[(size_t) laneIndex]
                                     .load (std::memory_order_relaxed);
            const float setDb  = tr.strip.auxSendDb[(size_t) laneIndex]
                                     .load (std::memory_order_relaxed);
            const float inputDb = tr.meterInputDb.load (std::memory_order_relaxed);
            const float sendDb  = (liveDb <= ChannelStripParams::kAuxSendOffDb
                                     ? setDb : liveDb);
            const bool sendOn = sendDb > ChannelStripParams::kAuxSendOffDb;

            // Track index + colour swatch.
            auto idxArea = row.removeFromLeft (22);
            g.setColour (tr.colour.withAlpha (sendOn ? 0.9f : 0.35f));
            g.fillRect (idxArea.reduced (2, 4));
            g.setColour (sendOn ? juce::Colours::white : juce::Colour (0xff707078));
            g.drawText (juce::String (i + 1), idxArea, juce::Justification::centred, false);

            // Track name. Default Dusk Studio sessions name tracks "1".."16" -
            // the colour swatch on the left already shows the index, so
            // promote those defaults to "Trk N" to avoid printing the same
            // digit twice. User-renamed tracks pass through verbatim.
            auto nameArea = row.removeFromLeft (juce::jmax (60, row.getWidth() / 2));
            g.setColour (sendOn ? juce::Colour (0xffe0e0e4) : juce::Colour (0xff606068));
            const auto rawName = tr.name.trim();
            const auto displayName = (rawName.isEmpty() || rawName == juce::String (i + 1))
                                       ? juce::String ("Trk ") + juce::String (i + 1)
                                       : rawName;
            g.drawText (displayName, nameArea.reduced (4, 0),
                        juce::Justification::centredLeft, true);

            // dB readout on the right.
            auto dbArea = row.removeFromRight (52);
            g.setColour (sendOn ? juce::Colour (0xffe0e0e4) : juce::Colour (0xff505058));
            const auto dbText = sendOn ? juce::String (sendDb, 1) + " dB"
                                       : juce::String ("-inf");
            g.drawText (dbText, dbArea, juce::Justification::centredRight, false);

            // Meter bar between name and dB.
            auto meterArea = row.reduced (2, 3);
            if (! meterArea.isEmpty())
            {
                g.setColour (juce::Colour (0xff181820));
                g.fillRect (meterArea);
                if (sendOn)
                {
                    const float inFrac = dbToMeterFrac (inputDb);
                    juce::Rectangle<int> fill = meterArea;
                    fill.setWidth (juce::roundToInt ((float) meterArea.getWidth() * inFrac));
                    g.setColour (meterColourForFrac (inFrac).withAlpha (0.85f));
                    g.fillRect (fill);
                }
            }
        }
    }

private:
    Session& session;
    int laneIndex;
};

AuxLaneComponent::AuxLaneComponent (AuxLane& l, AuxLaneStrip& s, int idx,
                                       AudioEngine& e)
    : lane (l), strip (s), engine (e), laneIndex (idx)
{
    // Accessibility floor — screen readers announce the lane as
    // "Aux N" instead of "Component".
    setTitle ("Aux " + juce::String (idx + 1));
    setDescription ("Aux send/return lane " + juce::String (idx + 1));

    nameLabel.setText (lane.name, juce::dontSendNotification);
    nameLabel.setJustificationType (juce::Justification::centredLeft);
    nameLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    nameLabel.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    nameLabel.setEditable (false, true, false);
    nameLabel.setColour (juce::Label::backgroundWhenEditingColourId, juce::Colour (0xff202024));
    nameLabel.setColour (juce::Label::textWhenEditingColourId,       juce::Colours::white);
    nameLabel.onTextChange = [this]
    {
        auto txt = nameLabel.getText().trim();
        if (txt.isEmpty())
        {
            nameLabel.setText (lane.name, juce::dontSendNotification);
            return;
        }
        // Cap aux name length — channel strips render this same name above
        // their AUX1-4 send knobs in a narrow column; keeping the cap here
        // ensures the channel-strip displays stay readable.
        constexpr int kAuxNameMaxChars = 12;
        if (txt.length() > kAuxNameMaxChars)
        {
            txt = txt.substring (0, kAuxNameMaxChars);
            nameLabel.setText (txt, juce::dontSendNotification);
        }
        lane.name = txt;
    };
    addAndMakeVisible (nameLabel);

    returnFader.setRange (ChannelStripParams::kFaderMinDb, ChannelStripParams::kFaderMaxDb, 0.1);
    returnFader.setSkewFactorFromMidPoint (-12.0);
    returnFader.setValue (lane.params.returnLevelDb.load (std::memory_order_relaxed),
                            juce::dontSendNotification);
    returnFader.setDoubleClickReturnValue (true, 0.0);
    returnFader.setTextValueSuffix (" dB");
    returnFader.onValueChange = [this]
    {
        lane.params.returnLevelDb.store ((float) returnFader.getValue(),
                                           std::memory_order_relaxed);
    };
    // Touch latch: faderTouched gates the audio-thread routing in
    // Touch mode (manual wins while grabbed, lane wins when released).
    // Also handles same-slot Write capture if the user grabs the
    // fader and rides it while in Write mode.
    returnFader.onDragStart = [this]
    {
        lane.params.faderTouched.store (true, std::memory_order_release);
    };
    returnFader.onDragEnd = [this]
    {
        lane.params.faderTouched.store (false, std::memory_order_release);
    };
    // Right-click on fader or mute → MIDI Learn menu (mouseDown handler
    // checks eventComponent).
    returnFader.addMouseListener (this, false);
    addAndMakeVisible (returnFader);

    muteButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::orangered);
    muteButton.setToggleState (lane.params.mute.load (std::memory_order_relaxed),
                                juce::dontSendNotification);
    muteButton.setTooltip ("Mute this AUX return lane");
    muteButton.onClick = [this]
    {
        const bool newState = muteButton.getToggleState();
        lane.params.mute.store (newState, std::memory_order_relaxed);

        // If we're playing in Write or Touch, drop a discrete mute point
        // at the playhead so the user-toggled state lands on the lane.
        const int amode = lane.params.automationMode.load (std::memory_order_relaxed);
        const bool playing = engine.getTransport().isPlaying();
        if (playing && (amode == (int) AutomationMode::Write
                        || amode == (int) AutomationMode::Touch))
            captureWritePoint (AutomationParam::Mute, newState ? 1.0f : 0.0f);
    };
    muteButton.addMouseListener (this, false);
    addAndMakeVisible (muteButton);

    // Automation mode button: cycles Off / R / W / T via popup menu.
    // Sits below the mute button on the strip.
    autoModeButton.setTooltip ("Automation mode: Off / Read / Write / Touch");
    autoModeButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    autoModeButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff909094));
    autoModeButton.onClick = [this] { showAutoModeMenu(); };
    {
        const int amode = lane.params.automationMode.load (std::memory_order_relaxed);
        autoModeButton.setButtonText (amode == (int) AutomationMode::Off   ? "Off"
                                       : amode == (int) AutomationMode::Read  ? "R"
                                       : amode == (int) AutomationMode::Write ? "W"
                                                                              : "T");
    }
    addAndMakeVisible (autoModeButton);

    stripMeter = std::make_unique<StripMeter> (lane.params);
    addAndMakeVisible (stripMeter.get());

    sendPanel = std::make_unique<SendSourcePanel> (engine.getSession(), laneIndex);
    addAndMakeVisible (sendPanel.get());

    for (int i = 0; i < AuxLaneParams::kMaxLanePlugins; ++i)
    {
        auto& s = slots[(size_t) i];

        s.openOrAddButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff222226));
        s.openOrAddButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff5a4880));
        s.openOrAddButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff9080c0));
        s.openOrAddButton.setColour (juce::TextButton::textColourOnId,  juce::Colours::white);
        s.openOrAddButton.onClick = [this, i]
        {
            auto& slotRef = strip.getPluginSlot (i);
            if (slotRef.isLoaded())
            {
                toggleEditorForSlot (i);
            }
            else
            {
                openPickerForSlot (i);
            }
        };
        addAndMakeVisible (s.openOrAddButton);

        s.bypassButton.setButtonText ("BYP");
        s.bypassButton.setClickingTogglesState (true);
        s.bypassButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd0a060));
        s.bypassButton.setTooltip ("Bypass this plugin slot");
        s.bypassButton.onClick = [this, i]
        {
            auto& slotRef = strip.getPluginSlot (i);
            auto& uiRef   = slots[(size_t) i];
            slotRef.setBypassed (uiRef.bypassButton.getToggleState());
            if (slotRef.wasAutoBypassed()) slotRef.clearAutoBypass();
            refreshSlotControls (i);
        };
        addChildComponent (s.bypassButton);

        s.removeButton.setButtonText ("X");
        s.removeButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff402020));
        s.removeButton.setTooltip ("Remove this plugin");
        s.removeButton.onClick = [this, i] { unloadSlot (i); };
        addChildComponent (s.removeButton);

    }

    for (int i = 0; i < AuxLaneParams::kMaxLanePlugins; ++i)
        refreshSlotControls (i);
    rebuildSlots();

    // Deeper a11y — name every user-driven control on the lane.
    const auto an = juce::String (laneIndex + 1);
    returnFader .setTitle ("Aux " + an + " return fader");
    muteButton  .setTitle ("Aux " + an + " mute");
    for (int i = 0; i < AuxLaneParams::kMaxLanePlugins; ++i)
        slots[(size_t) i].openOrAddButton.setTitle (
            "Aux " + an + " plugin slot " + juce::String (i + 1));

    startTimerHz (30);
}

AuxLaneComponent::~AuxLaneComponent()
{
    // Stop FIRST so the timer thread can't fire on members that are
    // about to destruct. JUCE's Timer base destructor would stop it
    // eventually, but base-class destruction runs AFTER member
    // destruction - leaving a window for a UAF.
    stopTimer();
    for (auto& s : slots)
    {
        s.editor.reset();
        s.hwInsertEditor.reset();
    }
}

void AuxLaneComponent::timerCallback()
{
    for (int i = 0; i < AuxLaneParams::kMaxLanePlugins; ++i)
        refreshSlotControls (i);
    if (stripMeter != nullptr) stripMeter->repaint();
    if (sendPanel  != nullptr) sendPanel->repaint();

    // Motor-fader animate + Write/Touch capture, mirroring the per-
    // channel pattern in ChannelStripComponent::timerCallback. Animate
    // is mode-agnostic — gated only on the user not dragging — so a
    // MIDI-bound aux return moves the on-screen fader in Off mode too.
    const int amode = lane.params.automationMode.load (std::memory_order_relaxed);
    const bool isWrite = amode == (int) AutomationMode::Write;
    const bool isTouch = amode == (int) AutomationMode::Touch;
    const bool playing = engine.getTransport().isPlaying();

    {
        const float live    = lane.params.liveReturnLevelDb.load (std::memory_order_relaxed);
        const bool  touched = lane.params.faderTouched.load (std::memory_order_relaxed);
        if (! touched && std::abs (live - displayedLiveReturnLevelDb) > 0.05f)
        {
            displayedLiveReturnLevelDb = live;
            returnFader.setValue (live, juce::dontSendNotification);
        }
        else if (touched)
        {
            displayedLiveReturnLevelDb = live;
        }

        const bool capturing = playing && (isWrite || (isTouch && touched));
        if (capturing)
            captureWritePoint (AutomationParam::FaderDb,
                                lane.params.returnLevelDb.load (std::memory_order_relaxed));
    }

    // Mute visual sync — read the manual atom in Off / Write so we
    // don't race against AudioEngine's 1-block-stale liveMute (which
    // used to re-toggle the button right after the user clicked,
    // making mute look stuck). Read / Touch still read the live atom
    // since the engine drives mute from the automation lane there.
    // Mirrors the track-strip fix in ChannelStripComponent.
    {
        const int amode = lane.params.automationMode.load (std::memory_order_relaxed);
        const bool laneDrives =
               amode == (int) AutomationMode::Read
            || amode == (int) AutomationMode::Touch;
        const bool effective = laneDrives
            ? lane.params.liveMute.load (std::memory_order_relaxed)
            : lane.params.mute    .load (std::memory_order_relaxed);
        if (muteButton.getToggleState() != effective)
            muteButton.setToggleState (effective, juce::dontSendNotification);
    }
}

void AuxLaneComponent::showAutoModeMenu()
{
    juce::PopupMenu m;
    const int cur = lane.params.automationMode.load (std::memory_order_relaxed);
    auto add = [&] (int v, const char* label)
    {
        m.addItem (v + 1, label, true, cur == v);
    };
    add ((int) AutomationMode::Off,   "Off");
    add ((int) AutomationMode::Read,  "Read");
    add ((int) AutomationMode::Write, "Write");
    add ((int) AutomationMode::Touch, "Touch");
    juce::Component::SafePointer<AuxLaneComponent> safe (this);
    showContextMenu (m, autoModeButton,
                       [safe] (int picked)
    {
        auto* self = safe.getComponent();
        if (self == nullptr || picked <= 0) return;
        self->setAutoMode ((AutomationMode) (picked - 1));
    });
}

void AuxLaneComponent::setAutoMode (AutomationMode m)
{
    // See ChannelStripComponent::setAutoMode for the full rationale —
    // auto-thin on mode-flip is racy until lanes move to AtomicSnapshot.
    // Pre-filter at capture time handles the worst bloat; File ▸
    // Optimize automation is the safe explicit RDP entrypoint.
    lane.params.automationMode.store ((int) m, std::memory_order_release);
    autoModeButton.setButtonText (m == AutomationMode::Off   ? "Off"
                                   : m == AutomationMode::Read  ? "R"
                                   : m == AutomationMode::Write ? "W"
                                                                : "T");
}

void AuxLaneComponent::captureWritePoint (AutomationParam param, float denormValue)
{
    auto normalize = [] (AutomationParam p, float v) -> float
    {
        switch (p)
        {
            case AutomationParam::FaderDb:
            {
                const float lo = ChannelStripParams::kFaderMinDb;
                const float hi = ChannelStripParams::kFaderMaxDb;
                return juce::jlimit (0.0f, 1.0f, (v - lo) / (hi - lo));
            }
            case AutomationParam::Mute:
                return v >= 0.5f ? 1.0f : 0.0f;
            // The other AutomationParam values (Pan, Solo, AuxSend*) are
            // valid in the enum but don't apply to aux lanes. captureWritePoint
            // is never called for them; the explicit cases silence
            // -Wswitch-enum.
            case AutomationParam::Pan:
            case AutomationParam::Solo:
            case AutomationParam::AuxSend1:
            case AutomationParam::AuxSend2:
            case AutomationParam::AuxSend3:
            case AutomationParam::AuxSend4:
            case AutomationParam::kCount:
                break;
        }
        return 0.0f;
    };

    auto& laneRef = lane.params.automationLanes[(size_t) param];
    AutomationPoint pt;
    pt.timeSamples   = engine.getTransport().getPlayhead();
    pt.value         = normalize (param, denormValue);
    pt.recordedAtBPM = engine.getSession().tempoBpm.load (std::memory_order_relaxed);

    // Pre-filter: skip near-identical samples close in time. Same shape
    // as the per-channel captureWritePoint pre-filter; spec lines
    // 750-753 (delta + max-span before RDP). The pt.timeSamples >=
    // last.timeSamples guard keeps us from short-circuiting after a
    // loop-wrap or transport rewind — those need the truncation block
    // below to drop the now-stale future points.
    if (isContinuousParam (param) && ! laneRef.points.empty())
    {
        constexpr float kDeltaEps = 0.001f;
        constexpr juce::int64 kMaxSpanSamples = 22050;   // ~500 ms @ 44.1 k
        const auto& last = laneRef.points.back();
        if (std::abs (pt.value - last.value) < kDeltaEps
            && pt.timeSamples >= last.timeSamples
            && (pt.timeSamples - last.timeSamples) < kMaxSpanSamples)
            return;
    }

    if (! laneRef.points.empty() && laneRef.points.back().timeSamples >= pt.timeSamples)
    {
        if (laneRef.points.back().timeSamples > pt.timeSamples)
        {
            auto cutoff = std::lower_bound (laneRef.points.begin(), laneRef.points.end(),
                pt.timeSamples,
                [] (const AutomationPoint& a, juce::int64 t) { return a.timeSamples < t; });
            laneRef.points.erase (cutoff, laneRef.points.end());
        }
        if (! laneRef.points.empty() && laneRef.points.back().timeSamples == pt.timeSamples)
        {
            laneRef.points.back() = pt;
            return;
        }
    }
    laneRef.points.push_back (pt);
}

void AuxLaneComponent::refreshSlotControls (int i)
{
    auto& slotRef = strip.getPluginSlot (i);
    auto& ui      = slots[(size_t) i];

    const int mode = strip.insertMode[(size_t) i].load (std::memory_order_relaxed);
    if (mode == AuxLaneStrip::kInsertHardware)
    {
        // Hardware-active slot: show the routed channel pair so the user
        // can see at a glance where the lane is patched. Each side
        // (out / in) is formatted independently so a mono routing
        // doesn't print a misleading "L-0".
        const auto routing = lane.hardwareInserts[(size_t) i].routing.current();
        auto formatPair = [] (int l, int r) -> juce::String
        {
            if (l < 0 && r < 0) return {};
            if (r < 0)          return juce::String (l + 1);
            if (l < 0)          return juce::String (r + 1);
            if (l == r)         return juce::String (l + 1);
            return juce::String (l + 1) + "-" + juce::String (r + 1);
        };
        const auto out = formatPair (routing.outputChL, routing.outputChR);
        const auto in  = formatPair (routing.inputChL,  routing.inputChR);
        juce::String label;
        if (out.isEmpty() && in.isEmpty())
            label = "HW (unrouted)";
        else
            label = juce::String ("HW: out ")
                  + (out.isNotEmpty() ? out : juce::String ("-"))
                  + " / in "
                  + (in .isNotEmpty() ? in  : juce::String ("-"));
        if (label != ui.displayedName)
        {
            ui.displayedName = label;
            ui.openOrAddButton.setButtonText (label);
        }
        ui.bypassButton.setVisible (true);
        ui.bypassButton.setToggleState (false, juce::dontSendNotification);
        ui.removeButton.setVisible (true);
        return;
    }

    if (slotRef.isLoaded())
    {
        const auto name = slotRef.getLoadedName();
        if (name != ui.displayedName)
        {
            ui.displayedName = name;
            ui.openOrAddButton.setButtonText (name);
            // First-time-loaded trigger (mirrors the offline branch
            // below). If we transitioned offline → loaded, the prior
            // resized() ran the offline layout which skips
            // bypassButton.setBounds — without re-running resized()
            // here the bypass button is visible at the empty layout's
            // zero rectangle and uncliclable.
            resized();
        }
        if (slotRef.wasCrashed())
            ui.openOrAddButton.setButtonText ("! " + name + " (crashed)");
        else if (slotRef.wasAutoBypassed())
            ui.openOrAddButton.setButtonText ("! " + name + " (stalled)");
        ui.bypassButton.setVisible (true);
        ui.bypassButton.setToggleState (slotRef.isBypassed(), juce::dontSendNotification);
        ui.removeButton.setVisible (true);
    }
    else if (slotRef.isOffline())
    {
        // Saved-session plugin missing on this host. Preserve the saved
        // name so the user knows what to reinstall; the slot's stashed
        // descXml + state base64 will round-trip on the next save.
        const auto offline = slotRef.getOfflineName();
        const auto label = juce::String (juce::CharPointer_UTF8 ("\xe2\x9a\xa0 "))
                         + (offline.isNotEmpty() ? offline : juce::String ("offline"))
                         + " (offline)";
        if (label != ui.displayedName)
        {
            ui.displayedName = label;
            ui.openOrAddButton.setButtonText (label);
            // First time we enter offline state for this slot — re-run
            // resized() so removeButton gets its header-strip bounds.
            // Without this the button is visible at the prior empty
            // layout's zero rectangle and the user can't dismiss the
            // placeholder.
            resized();
        }
        ui.bypassButton.setVisible (false);
        ui.removeButton.setVisible (true);
    }
    else
    {
        ui.displayedName.clear();
        ui.openOrAddButton.setButtonText ("Insert");
        ui.bypassButton.setVisible (false);
        ui.removeButton.setVisible (false);
    }
}

void AuxLaneComponent::openPickerForSlot (int slotIdx)
{
    const auto cursor = juce::Desktop::getMousePosition();
    juce::Component::SafePointer<AuxLaneComponent> safe (this);
    pluginpicker::openPickerMenu (strip.getPluginSlot (slotIdx),
                                    slots[(size_t) slotIdx].openOrAddButton,
                                    activePluginChooser,
                                    [safe, slotIdx]
                                    {
                                        if (auto* self = safe.getComponent())
                                        {
                                            // Picking a plugin flips this slot back to Plugin mode.
                                            self->strip.insertMode[(size_t) slotIdx]
                                                .store (AuxLaneStrip::kInsertPlugin,
                                                         std::memory_order_release);
                                            self->refreshSlotControls (slotIdx);
                                            self->rebuildSlots();
                                        }
                                    },
                                    pluginpicker::PluginKind::Effects,
                                    cursor,
                                    [safe, slotIdx]
                                    {
                                        if (auto* self = safe.getComponent())
                                            self->openHardwareInsertEditor (slotIdx);
                                    });
}

void AuxLaneComponent::openHardwareInsertEditor (int slotIdx)
{
    if (slotIdx < 0 || slotIdx >= AuxLaneParams::kMaxLanePlugins) return;

    // Flip the lane's slot to Hardware mode immediately so the audio
    // thread's crossfade gate (Phase 3) begins ramping in even before
    // the user touches a control inside the editor.
    strip.insertMode[(size_t) slotIdx].store (AuxLaneStrip::kInsertHardware,
                                                std::memory_order_release);
    refreshSlotControls (slotIdx);
    // rebuildSlots will see the flipped mode and call
    // attachHardwareInsertForSlot, mirroring the plugin-editor inline
    // path. No popup modal.
    rebuildSlots();
}

void AuxLaneComponent::unloadSlot (int slotIdx)
{
    // Defer the whole teardown to next message-loop tick. unloadSlot
    // runs from the remove-button's onClick stack; doing the editor
    // destruction (which for OOP/XEmbed plugins tears down child-
    // process IPC + native windows), plugin instance unload, and
    // rebuild synchronously from inside that click handler has been
    // observed to crash on plugins like Multi-Q whose editor destructor
    // posts further messages that race with the in-flight click event.
    // Letting the click handler unwind first puts the teardown on a
    // clean stack.
    juce::Component::SafePointer<AuxLaneComponent> safe (this);
    juce::MessageManager::callAsync ([safe, slotIdx]
    {
        auto* self = safe.getComponent();
        if (self == nullptr) return;
        self->detachEditorForSlot (slotIdx);
        self->detachHardwareInsertForSlot (slotIdx);
        self->strip.getPluginSlot (slotIdx).unload();
        // Clear the model's enabled flag so any consumer that polls
        // lane.hardwareInserts[slotIdx].enabled sees a disabled slot
        // after unload — without this the flag could stay true and
        // confuse session save / routing diagnostics.
        self->lane.hardwareInserts[(size_t) slotIdx].enabled.store (
            false, std::memory_order_release);
        // Flip back to Plugin mode so the next picker open lands in the
        // default code path (and the audio thread stops routing through
        // the hardware-insert crossfade).
        self->strip.insertMode[(size_t) slotIdx].store (
            AuxLaneStrip::kInsertPlugin, std::memory_order_release);
        self->refreshSlotControls (slotIdx);
        self->rebuildSlots();
    });
}

void AuxLaneComponent::toggleEditorForSlot (int /*slotIdx*/)
{
    // Editor embeds inline whenever the slot is loaded — nothing to
    // toggle. Clicks on the slot's name button when loaded fall through
    // to a no-op; users use the X button to unload the slot.
}

void AuxLaneComponent::attachEditorForSlot (int slotIdx)
{
    auto& ui = slots[(size_t) slotIdx];
    if (ui.editor == nullptr) return;
    if (ui.editor->getParentComponent() == this) return;

    // Tag so EmbeddedModal::show can find + hide this plugin editor
    // for the lifetime of any modal. Plugin editors (OOP / XEmbed / GL)
    // sometimes render above JUCE's modal layer and steal click input;
    // setVisible(false) for the modal's duration forces them under.
    ui.editor->getProperties().set ("dusk_pluginEditor", true);

    addAndMakeVisible (*ui.editor);
    layoutEditorForSlot (slotIdx);

    // LV2 / OOP plugin editors sometimes finalize their preferred size
    // after a few X11 idle pumps - the bounds reported at
    // createEditorIfNeeded() time are stale. Re-layout after a few
    // delays so the inline embed picks up the final geometry.
    scheduleEditorRefits (slotIdx);
}

void AuxLaneComponent::detachEditorForSlot (int slotIdx)
{
    auto& ui = slots[(size_t) slotIdx];
    if (ui.editor == nullptr) return;
    // editor destructor auto-removes from parent, but be explicit so
    // bounds-changed callbacks during teardown can't fire against a
    // half-destructed editor.
    removeChildComponent (ui.editor.get());
    ui.editor.reset();
}

void AuxLaneComponent::attachHardwareInsertForSlot (int slotIdx)
{
    auto& ui = slots[(size_t) slotIdx];
    if (ui.hwInsertEditor != nullptr) return;

    ui.hwInsertEditor = std::make_unique<HardwareInsertEditor> (
        lane.hardwareInserts[(size_t) slotIdx],
        engine.getDeviceManager(),
        /*onDone*/ [] {},
        /*embedded*/ true);
    addAndMakeVisible (*ui.hwInsertEditor);
    layoutEditorForSlot (slotIdx);
}

void AuxLaneComponent::detachHardwareInsertForSlot (int slotIdx)
{
    auto& ui = slots[(size_t) slotIdx];
    if (ui.hwInsertEditor == nullptr) return;
    removeChildComponent (ui.hwInsertEditor.get());
    ui.hwInsertEditor.reset();
}

void AuxLaneComponent::layoutEditorForSlot (int slotIdx)
{
    auto& ui = slots[(size_t) slotIdx];

    // Plugin editor and hardware-insert editor share the same center
    // area below the slot header; only one is ever attached at a time.
    juce::Component* body = nullptr;
    if (ui.editor != nullptr && ui.editor->getParentComponent() == this)
        body = ui.editor.get();
    else if (ui.hwInsertEditor != nullptr && ui.hwInsertEditor->getParentComponent() == this)
        body = ui.hwInsertEditor.get();
    if (body == nullptr) return;

    auto center = getCenterArea();
    auto& slot = strip.getPluginSlot (slotIdx);
    const int mode = strip.insertMode[(size_t) slotIdx].load (std::memory_order_relaxed);
    if (slot.isLoaded() || slot.isOffline() || mode == AuxLaneStrip::kInsertHardware)
        center.removeFromTop (kSlotHeaderH + 4);

    if (center.isEmpty()) return;

    const int prefW = body->getWidth();
    const int prefH = body->getHeight();
    if (prefW <= 0 || prefH <= 0) return;

    const int w = juce::jmin (prefW, center.getWidth());
    const int h = juce::jmin (prefH, center.getHeight());
    const int x = center.getX() + (center.getWidth()  - w) / 2;
    const int y = center.getY() + (center.getHeight() - h) / 2;
    body->setBounds (x, y, w, h);
}

void AuxLaneComponent::scheduleEditorRefits (int slotIdx)
{
    for (int delayMs : { 100, 350, 800 })
    {
        juce::Component::SafePointer<AuxLaneComponent> safe (this);
        juce::Timer::callAfterDelay (delayMs, [safe, slotIdx]
        {
            if (auto* self = safe.getComponent())
                self->layoutEditorForSlot (slotIdx);
        });
    }
}

juce::Rectangle<int> AuxLaneComponent::getStripArea() const noexcept
{
    auto area = getLocalBounds().reduced (6);
    return area.removeFromLeft (kStripWidth);
}

juce::Rectangle<int> AuxLaneComponent::getSendPanelArea() const noexcept
{
    auto area = getLocalBounds().reduced (6);
    return area.removeFromRight (kSendPanelWidth);
}

juce::Rectangle<int> AuxLaneComponent::getCenterArea() const noexcept
{
    auto area = getLocalBounds().reduced (6);
    area.removeFromLeft  (kStripWidth + kColumnGap);
    area.removeFromRight (kSendPanelWidth + kColumnGap);
    return area;
}

void AuxLaneComponent::rebuildSlots()
{
    for (int i = 0; i < AuxLaneParams::kMaxLanePlugins; ++i)
    {
        auto& ui = slots[(size_t) i];
        const int mode = strip.insertMode[(size_t) i].load (std::memory_order_relaxed);

        if (mode == AuxLaneStrip::kInsertHardware)
        {
            // HW mode wins — drop any plugin editor that was attached
            // before the mode flip.
            if (ui.editor != nullptr) detachEditorForSlot (i);
            if (ui.hwInsertEditor == nullptr) attachHardwareInsertForSlot (i);
        }
        else
        {
            if (ui.hwInsertEditor != nullptr) detachHardwareInsertForSlot (i);

            auto* instance = strip.getPluginSlot (i).getInstance();
            if (instance != nullptr && ui.editor == nullptr)
            {
                if (instance->hasEditor())
                {
                    // X11 latch is a no-op when main is already X11, but
                    // keep it for safety on platforms that don't force the
                    // main peer to X11 (and to keep the call sites uniform
                    // with ChannelStripComponent).
                    duskstudio::platform::preferX11ForNextNativeWindow();
                    ui.editor.reset (instance->createEditorIfNeeded());
                    duskstudio::platform::clearPreferX11ForNativeWindow();
                }
                if (ui.editor == nullptr)
                    ui.editor = std::make_unique<juce::GenericAudioProcessorEditor> (*instance);
                attachEditorForSlot (i);
            }
            if (instance == nullptr && ui.editor != nullptr)
                detachEditorForSlot (i);
        }
    }
    resized();
}

void AuxLaneComponent::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.5f);
    g.setColour (juce::Colour (0xff181820));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (lane.colour.withAlpha (0.85f));
    g.fillRoundedRectangle (r.removeFromTop (4.0f), 2.0f);
    g.setColour (juce::Colour (0xff2a2a2e));
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 4.0f, 1.0f);

    // Left-strip backplate.
    auto stripCol = getStripArea();
    if (! stripCol.isEmpty())
    {
        g.setColour (juce::Colour (0xff141418));
        g.fillRoundedRectangle (stripCol.toFloat(), 4.0f);
        g.setColour (juce::Colour (0xff2a2a2e));
        g.drawRoundedRectangle (stripCol.toFloat(), 4.0f, 1.0f);
    }
}

void AuxLaneComponent::resized()
{
    // Left column: header row (name + M) + auto-mode row + vertical
    // return fader + meter.
    auto stripCol = getStripArea().reduced (6);
    stripCol.removeFromTop (6);
    {
        auto headerRow = stripCol.removeFromTop (22);
        muteButton.setBounds (headerRow.removeFromRight (28));
        headerRow.removeFromRight (4);
        nameLabel.setBounds (headerRow);
    }
    {
        auto autoRow = stripCol.removeFromTop (18);
        autoModeButton.setBounds (autoRow.removeFromRight (44));
    }
    stripCol.removeFromTop (6);

    // Fader on the left of the column body, meter on the right.
    auto faderArea = stripCol.removeFromLeft (stripCol.getWidth() - 22);
    stripCol.removeFromLeft (4);
    returnFader.setBounds (faderArea);
    if (stripMeter != nullptr) stripMeter->setBounds (stripCol);

    // Center column: plugin slot header + editor-host rect underneath.
    // resized() only lays out slots[0]; extra slots would need their own
    // sub-areas computed off getCenterArea(). The single-slot contract is
    // enforced at compile time so adding kMaxLanePlugins > 1 forces this
    // layout block to be revisited.
    static_assert (AuxLaneParams::kMaxLanePlugins == 1,
                     "AuxLaneComponent::resized assumes a single plugin slot — "
                     "extend the layout block before raising kMaxLanePlugins.");
    auto center = getCenterArea();
    auto& ui = slots[0];
    auto& slot0 = strip.getPluginSlot (0);
    const int mode0 = strip.insertMode[0].load (std::memory_order_relaxed);
    const bool hardware = mode0 == AuxLaneStrip::kInsertHardware;
    // Offline placeholder + hardware-insert mode share the header
    // layout. Without the hardware branch the HW editor would overlap
    // openOrAddButton (which would otherwise fill the entire center),
    // and removeButton — made visible by refreshSlotControls in HW
    // mode — would never get bounds and the user couldn't dismiss
    // the HW insert.
    if (slot0.isLoaded() || slot0.isOffline() || hardware)
    {
        auto headerStrip = center.removeFromTop (kSlotHeaderH);
        ui.removeButton.setBounds (headerStrip.removeFromRight (28));
        headerStrip.removeFromRight (4);
        if (slot0.isLoaded())
        {
            ui.bypassButton.setBounds (headerStrip.removeFromRight (44));
            headerStrip.removeFromRight (4);
        }
        ui.openOrAddButton.setBounds (headerStrip);
    }
    else
    {
        ui.openOrAddButton.setBounds (center);
    }

    // Right column: send-source panel fills.
    if (sendPanel != nullptr) sendPanel->setBounds (getSendPanelArea());

    // Inline plugin editor — sits below the slot header in the center
    // column, centered horizontally and vertically inside the remaining
    // area. layoutEditorForSlot is a no-op when the editor isn't
    // attached, so loaded-vs-empty branches above don't need to know.
    for (int i = 0; i < AuxLaneParams::kMaxLanePlugins; ++i)
        layoutEditorForSlot (i);
}

void AuxLaneComponent::childBoundsChanged (juce::Component* child)
{
    // Plugin editors (especially LV2 / OOP) self-resize after their
    // first X11 idle pumps. Re-center inside the editor area when the
    // hosted editor reports a new size. Same path applies to the
    // hardware-insert editor (it has a fixed size, but resized() may
    // run early before bounds settle).
    for (int i = 0; i < AuxLaneParams::kMaxLanePlugins; ++i)
    {
        auto& ui = slots[(size_t) i];
        juce::Component* body = nullptr;
        if (ui.editor.get() == child) body = ui.editor.get();
        else if (ui.hwInsertEditor.get() == child) body = ui.hwInsertEditor.get();
        if (body != nullptr)
        {
            // Only re-center if the editor is sized within the area —
            // direct recursion through setBounds is impossible because
            // layoutEditorForSlot pins width/height to jmin(pref,area).
            // Calling setBounds with the same w/h re-emits
            // childBoundsChanged so we guard against repeat layout when
            // x/y already match.
            auto center = getCenterArea();
            auto& slot = strip.getPluginSlot (i);
            const int mode = strip.insertMode[(size_t) i].load (std::memory_order_relaxed);
            if (slot.isLoaded() || slot.isOffline() || mode == AuxLaneStrip::kInsertHardware)
                center.removeFromTop (kSlotHeaderH + 4);
            if (center.isEmpty()) return;

            const int prefW = body->getWidth();
            const int prefH = body->getHeight();
            const int w = juce::jmin (prefW, center.getWidth());
            const int h = juce::jmin (prefH, center.getHeight());
            const int x = center.getX() + (center.getWidth()  - w) / 2;
            const int y = center.getY() + (center.getHeight() - h) / 2;
            if (body->getX() == x && body->getY() == y
                && body->getWidth() == w && body->getHeight() == h)
                return;
            body->setBounds (x, y, w, h);
            return;
        }
    }
}

void AuxLaneComponent::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu()) return;
    auto& session = engine.getSession();
    // Per-control MIDI Learn routes. The strip is registered as a mouse
    // listener on its return fader + mute toggle (see ctor); eventComponent
    // resolves to whichever was right-clicked. Anything else (background
    // pixels, plugin-slot column) is ignored - the strip has no other
    // context menu.
    if (e.eventComponent == &returnFader)
    {
        midilearn::showLearnMenu (returnFader, session,
                                    MidiBindingTarget::AuxLaneFader, laneIndex);
        return;
    }
    if (e.eventComponent == &muteButton)
    {
        midilearn::showLearnMenu (muteButton, session,
                                    MidiBindingTarget::AuxLaneMute, laneIndex);
        return;
    }
}
} // namespace duskstudio
