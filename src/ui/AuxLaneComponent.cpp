#include "AuxLaneComponent.h"
#include "AuxEditorHost.h"
#include "HardwareInsertEditor.h"
#include "PlatformWindowing.h"
#include "PluginPickerHelpers.h"
#include "../dsp/AuxLaneStrip.h"
#include "../engine/AudioEngine.h"
#include "../engine/PluginSlot.h"
#include "../engine/Transport.h"
#include "../session/MidiBindings.h"

namespace focal
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

            // Track name. Default Focal sessions name tracks "1".."16" -
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
    nameLabel.setText (lane.name, juce::dontSendNotification);
    nameLabel.setJustificationType (juce::Justification::centredLeft);
    nameLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    nameLabel.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    nameLabel.setEditable (false, true, false);
    nameLabel.setColour (juce::Label::backgroundWhenEditingColourId, juce::Colour (0xff202024));
    nameLabel.setColour (juce::Label::textWhenEditingColourId,       juce::Colours::white);
    nameLabel.onTextChange = [this]
    {
        const auto txt = nameLabel.getText().trim();
        if (txt.isEmpty()) nameLabel.setText (lane.name, juce::dontSendNotification);
        else lane.name = txt;
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
        lane.params.mute.store (muteButton.getToggleState(), std::memory_order_relaxed);
    };
    muteButton.addMouseListener (this, false);
    addAndMakeVisible (muteButton);

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
    startTimerHz (30);
}

AuxLaneComponent::~AuxLaneComponent()
{
    // Stop FIRST so the timer thread can't fire on members that are
    // about to destruct. JUCE's Timer base destructor would stop it
    // eventually, but base-class destruction runs AFTER member
    // destruction - leaving a window for a UAF.
    stopTimer();
    for (int i = 0; i < AuxLaneParams::kMaxLanePlugins; ++i)
        destroyEditorHostForSlot (i);
    for (auto& s : slots)
        s.editor.reset();
}

void AuxLaneComponent::timerCallback()
{
    for (int i = 0; i < AuxLaneParams::kMaxLanePlugins; ++i)
        refreshSlotControls (i);
    if (stripMeter != nullptr) stripMeter->repaint();
    if (sendPanel  != nullptr) sendPanel->repaint();
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
        }
        if (slotRef.wasCrashed())
            ui.openOrAddButton.setButtonText ("! " + name + " (crashed)");
        else if (slotRef.wasAutoBypassed())
            ui.openOrAddButton.setButtonText ("! " + name + " (stalled)");
        ui.bypassButton.setVisible (true);
        ui.bypassButton.setToggleState (slotRef.isBypassed(), juce::dontSendNotification);
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

    juce::Component::SafePointer<AuxLaneComponent> safe (this);
    auto editor = std::make_unique<HardwareInsertEditor> (
        lane.hardwareInserts[(size_t) slotIdx],
        engine.getDeviceManager(),
        [safe, slotIdx]
        {
            if (auto* self = safe.getComponent())
            {
                self->hardwareInsertModal.close();
                self->refreshSlotControls (slotIdx);
            }
        });

    auto* parent = findParentComponentOfClass<juce::Component>();
    if (parent == nullptr) parent = this;
    hardwareInsertModal.show (*parent, std::move (editor));
}

void AuxLaneComponent::unloadSlot (int slotIdx)
{
    destroyEditorHostForSlot (slotIdx);
    auto& ui = slots[(size_t) slotIdx];
    ui.editor.reset();
    strip.getPluginSlot (slotIdx).unload();
    refreshSlotControls (slotIdx);
    rebuildSlots();
}

void AuxLaneComponent::toggleEditorForSlot (int slotIdx)
{
    auto& ui = slots[(size_t) slotIdx];
    if (ui.editorHost != nullptr)
        ui.editorHost->setHostHidden (ui.editorHost->isVisible());
    rebuildSlots();
}

void AuxLaneComponent::createEditorHostForSlot (int slotIdx)
{
    auto& ui = slots[(size_t) slotIdx];
    if (ui.editor == nullptr || ui.editorHost != nullptr) return;

    auto* instance = strip.getPluginSlot (slotIdx).getInstance();
    ui.editorHost = std::make_unique<AuxEditorHost> (
        lane.name + " - " + ui.editor->getName(),
        *ui.editor,
        instance,
        &engine);

    const auto target = computeSlotScreenRect (slotIdx);
    if (! target.isEmpty())
        ui.editorHost->setLaneScreenBounds (target);
}

void AuxLaneComponent::destroyEditorHostForSlot (int slotIdx)
{
    auto& ui = slots[(size_t) slotIdx];
    if (ui.editorHost == nullptr) return;
    focal::platform::prepareForTopLevelDestruction (*ui.editorHost);
    ui.editorHost.reset();
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

juce::Rectangle<int> AuxLaneComponent::computeSlotScreenRect (int slotIdx) const
{
    if (slotIdx < 0 || slotIdx >= (int) slots.size()) return {};
    if (! strip.getPluginSlot (slotIdx).isLoaded()) return {};

    auto center = getCenterArea();
    if (center.isEmpty()) return {};
    center.removeFromTop (kSlotHeaderH);
    center.removeFromTop (4);
    if (center.isEmpty()) return {};
    return localAreaToGlobal (center);
}

void AuxLaneComponent::repositionEditorHosts()
{
    for (int i = 0; i < (int) slots.size(); ++i)
    {
        auto& ui = slots[(size_t) i];
        if (ui.editorHost == nullptr) continue;
        const auto target = computeSlotScreenRect (i);
        if (! target.isEmpty())
            ui.editorHost->setLaneScreenBounds (target);
    }
}

void AuxLaneComponent::setEditorHostsHidden (bool hidden)
{
    for (auto& ui : slots)
        if (ui.editorHost != nullptr)
            ui.editorHost->setHostHidden (hidden);
}

void AuxLaneComponent::closeAllPopoutsForShutdown()
{
    for (int i = 0; i < (int) slots.size(); ++i)
        destroyEditorHostForSlot (i);
}

void AuxLaneComponent::rebuildSlots()
{
    for (int i = 0; i < AuxLaneParams::kMaxLanePlugins; ++i)
    {
        auto& ui = slots[(size_t) i];
        auto* instance = strip.getPluginSlot (i).getInstance();
        if (instance != nullptr && ui.editor == nullptr)
        {
            if (instance->hasEditor())
            {
                focal::platform::preferX11ForNextNativeWindow();
                ui.editor.reset (instance->createEditorIfNeeded());
                focal::platform::clearPreferX11ForNativeWindow();
            }
            if (ui.editor == nullptr)
                ui.editor = std::make_unique<juce::GenericAudioProcessorEditor> (*instance);
        }
        if (instance != nullptr && ui.editor != nullptr && ui.editorHost == nullptr)
            createEditorHostForSlot (i);
        if (instance == nullptr && (ui.editor != nullptr || ui.editorHost != nullptr))
        {
            destroyEditorHostForSlot (i);
            ui.editor.reset();
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
    // Left column: header row (name + M) + vertical return fader + meter.
    auto stripCol = getStripArea().reduced (6);
    stripCol.removeFromTop (6);
    {
        auto headerRow = stripCol.removeFromTop (22);
        muteButton.setBounds (headerRow.removeFromRight (28));
        headerRow.removeFromRight (4);
        nameLabel.setBounds (headerRow);
    }
    stripCol.removeFromTop (8);

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
    if (strip.getPluginSlot (0).isLoaded())
    {
        auto headerStrip = center.removeFromTop (kSlotHeaderH);
        ui.removeButton.setBounds (headerStrip.removeFromRight (28));
        headerStrip.removeFromRight (4);
        ui.bypassButton.setBounds (headerStrip.removeFromRight (44));
        headerStrip.removeFromRight (4);
        ui.openOrAddButton.setBounds (headerStrip);
    }
    else
    {
        ui.openOrAddButton.setBounds (center);
    }

    // Right column: send-source panel fills.
    if (sendPanel != nullptr) sendPanel->setBounds (getSendPanelArea());

    repositionEditorHosts();
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
} // namespace focal
