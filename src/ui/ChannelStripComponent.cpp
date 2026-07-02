#include "ChannelStripComponent.h"
#include "CompBypassLed.h"
#include "DuskContextMenu.h"
#include "DuskStudioLookAndFeel.h"
#include "ChannelEqEditor.h"
#include "ChannelCompEditor.h"
#include "DimOverlay.h"
#include "EmbeddedModal.h"
#include "HardwareInsertEditor.h"
#include "PluginPickerHelpers.h"
#if DUSKSTUDIO_HAS_NATIVE_CLAP
  #include "ClapPluginEditorComponent.h"   // Linux-only native CLAP editor
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
  #include "Lv2PluginEditorComponent.h"    // Linux-only native LV2 editor (suil)
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
  #include "Vst3PluginEditorComponent.h"   // Linux-only native VST3 editor (IPlugView)
#endif
#include "DuskAlerts.h"
#include "FreezeDialog.h"
#include "../engine/AudioEngine.h"
#include "../engine/PluginSlot.h"
#include "../engine/PluginManager.h"
#include "PlatformWindowing.h"
#include "../session/ParamEditAction.h"
#include "../session/RegionEditActions.h"
#include <cstdio>

namespace duskstudio
{

// Custom header button for the COMP section. Acts as both an
// enable-toggle (left click) and a mode-picker (right click). Single
// affordance replacing the older COMP label + mode pill + bypass LED
// triplet — less visual noise, one obvious click target.
class ChannelStripComponent::CompHeaderButton final : public juce::Component,
                                                          public juce::SettableTooltipClient
{
public:
    CompHeaderButton (std::function<bool()> getEnabled,
                       std::function<void()> onToggleEnable,
                       std::function<void()> onPickMode)
        : isEnabledFn (std::move (getEnabled)),
          toggleFn    (std::move (onToggleEnable)),
          pickFn      (std::move (onPickMode))
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTooltip ("Left-click to enable / disable. Right-click to choose mode (OPTO / FET / VCA).");
    }

    void setLabelText (juce::String t)
    {
        if (text != t) { text = std::move (t); repaint(); }
    }

    // EQ header uses these to surface the active type (Brown / Black)
    // — see ChannelStripComponent::timerCallback. COMP header leaves
    // them at defaults.
    void setChipText (juce::String t)
    {
        if (chip != t) { chip = std::move (t); repaint(); }
    }
    void setAccentColour (juce::Colour c)
    {
        if (accent != c) { accent = c; repaint(); }
    }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        const bool on = isEnabledFn ? isEnabledFn() : false;

        // Pill background — same dark fill regardless of state.
        g.setColour (juce::Colour (0xff242428));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (juce::Colour (0xff3a3a40));
        g.drawRoundedRectangle (r, 4.0f, 0.8f);

        // LED on the left, vertically centered. Green when engaged.
        const float ledSize = juce::jmin (r.getHeight() - 4.0f, 8.0f);
        const float ledX    = r.getX() + 4.0f;
        const float ledY    = r.getCentreY() - ledSize / 2.0f;
        const auto ledRect  = juce::Rectangle<float> (ledX, ledY, ledSize, ledSize);
        g.setColour (juce::Colour (0xff0a0a0c));
        g.fillEllipse (ledRect.expanded (1.0f));
        g.setColour (on ? juce::Colour (0xff60d060) : juce::Colour (0xff2a3028));
        g.fillEllipse (ledRect);
        if (on)
        {
            g.setColour (juce::Colour (0xffa0f0a0).withAlpha (0.55f));
            g.fillEllipse (ledRect.reduced (ledSize * 0.35f, ledSize * 0.35f)
                                    .translated (-ledSize * 0.10f, -ledSize * 0.10f));
        }

        // Optional right-side chip (EQ type indicator).
        const float chipReserve = chip.isNotEmpty() ? 18.0f : 0.0f;
        if (chip.isNotEmpty())
        {
            const auto chipR = juce::Rectangle<float> (
                r.getRight() - chipReserve - 2.0f,
                r.getY() + 3.0f,
                chipReserve, r.getHeight() - 6.0f);
            const auto chipFill = accent.isTransparent()
                                       ? juce::Colour (0xff2a2a32)
                                       : accent;
            g.setColour (chipFill);
            g.fillRoundedRectangle (chipR, 2.5f);
            g.setColour (juce::Colour (0xff0a0a0c));
            g.drawRoundedRectangle (chipR, 2.5f, 0.6f);
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
            g.drawText (chip, chipR, juce::Justification::centred, false);
        }

        // Text always white, centered in the remaining space (offset
        // right of the LED so the visual centre lands on the pill).
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
        auto textBounds = r;
        textBounds.removeFromLeft (ledSize + 8.0f);
        textBounds.removeFromRight (4.0f + chipReserve);
        g.drawText (text, textBounds, juce::Justification::centred, false);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())   // right-click
        {
            if (pickFn) pickFn();
        }
        else                         // left-click
        {
            if (toggleFn) toggleFn();
            repaint();
        }
    }

private:
    std::function<bool()> isEnabledFn;
    std::function<void()> toggleFn;
    std::function<void()> pickFn;
    juce::String text { "COMP" };
    juce::String chip;                                  // empty = no chip
    juce::Colour accent { juce::Colours::transparentBlack };
};

namespace
{
struct BandSpec
{
    const char* rowName;
    juce::Colour accent;          // 4K-palette band color
    float freqMin, freqMax, freqDefault;
    std::atomic<float>* (*gainPtr) (ChannelStripParams&);
    std::atomic<float>* (*freqPtr) (ChannelStripParams&);
    // qPtr is non-null for bell-only mid bands (HM, LM). Shelf bands (HF, LF)
    // leave it null and get a 2-knob row instead of 3.
    std::atomic<float>* (*qPtr)    (ChannelStripParams&) = nullptr;
};

// Top-to-bottom order: HF (treble) on top, LF (bass) on the bottom - matches
// the SSL/console convention and the user's spatial expectation. HF and LF
// are shelf-only here (no Q), HM and LM are bell with Q exposed - three
// knobs per row, mirroring the SSL E-EQ layout.
static const std::array<BandSpec, 4>& bandSpecs()
{
    static const std::array<BandSpec, 4> specs {{
        { "HF", juce::Colour (sslEqColors::kHfRed),    ChannelStripParams::kHfFreqMin, ChannelStripParams::kHfFreqMax, 8000.0f,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hfGainDb; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hfFreq; } },
        { "HM", juce::Colour (sslEqColors::kHmGreen),  ChannelStripParams::kHmFreqMin, ChannelStripParams::kHmFreqMax, 2000.0f,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hmGainDb; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hmFreq; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hmQ; } },
        { "LM", juce::Colour (sslEqColors::kLmBlue),   ChannelStripParams::kLmFreqMin, ChannelStripParams::kLmFreqMax, 600.0f,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lmGainDb; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lmFreq; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lmQ; } },
        { "LF", juce::Colour (sslEqColors::kLfBlack),  ChannelStripParams::kLfFreqMin, ChannelStripParams::kLfFreqMax, 100.0f,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lfGainDb; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lfFreq; } },
    }};
    return specs;
}

void styleCompactKnob (juce::Slider& k, juce::Colour fill)
{
    k.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.setColour (juce::Slider::rotarySliderFillColourId, fill);
    k.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
}

void enableValueLabel (juce::Slider& k, const juce::String& suffix, int decimals,
                       int width = 56, int height = 14)
{
    // readOnly=false so single-click on the value text opens an inline
    // editor and the user can type a precise value. Background + outline
    // stay transparent so the value reads as a plain number against the
    // strip's tinted section bands rather than a boxed-in textbox.
    k.setTextBoxStyle (juce::Slider::TextBoxBelow, false, width, height);
    k.setTextValueSuffix (suffix);
    k.setNumDecimalPlacesToDisplay (decimals);
    k.setColour (juce::Slider::textBoxTextColourId,       juce::Colour (0xffd8d8d8));
    k.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    k.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
    k.setColour (juce::Slider::textBoxHighlightColourId,  juce::Colour (0xff404048));
}

// Format a frequency in Hz, switching to "1.2k" notation above 1 kHz and
// dropping a trailing ".0" so integer kHz values stay short ("2k", "8k")
// instead of "2.0k" / "8.0k". Tight-strip-friendly.
inline juce::String formatFrequency (double hz)
{
    if (hz >= 1000.0)
    {
        const double khz = hz / 1000.0;
        if (std::abs (khz - std::round (khz)) < 0.05)
            return juce::String ((int) std::round (khz)) + "k";
        return juce::String (khz, 1) + "k";
    }
    return juce::String ((int) std::round (hz));
}

// Format an EQ band gain in dB, dropping ".0" on integer values so "0" / "-2"
// / "+12" fit in narrow textboxes instead of "0.0" / "-2.0" / "+12.0".
inline juce::String formatBandGain (double db)
{
    const double rounded = std::round (db);
    if (std::abs (db - rounded) < 0.05)
    {
        const int idb = (int) rounded;
        if (idb > 0) return "+" + juce::String (idb);
        return juce::String (idb);
    }
    return juce::String (db, 1);
}
} // namespace

// Style helper for the EQ / COMP compact-mode placeholder buttons. They sit
// in the slots the inline EQ + COMP sections occupy in normal mode and
// open the corresponding editor as a popup when clicked. Hidden by default.
static void styleCompactSectionButton (juce::TextButton& b, juce::Colour accent)
{
    b.setClickingTogglesState (false);
    b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff222226));
    b.setColour (juce::TextButton::textColourOffId,  accent.brighter (0.3f));
    b.setColour (juce::TextButton::textColourOnId,   accent.brighter (0.3f));
    b.setVisible (false);
}

ChannelStripComponent::ChannelStripComponent (int idx, Track& t, Session& s,
                                                PluginSlot& slot, AudioEngine& eng)
    : trackIndex (idx), track (t), session (s), pluginSlot (slot), engine (eng)
{
    // Accessibility floor: screen readers announce the strip as
    // "Track N" instead of the default "Component". Description
    // surfaces in extended-info readers.
    setTitle ("Track " + juce::String (idx + 1));
    setDescription ("Channel strip for track " + juce::String (idx + 1));

    // Listen for engine-side MIDI device-list rebuilds (USB hot-plug
    // refresh) so the dropdown stays in sync with the live device list.
    engine.addChangeListener (this);

    nameLabel.setText (track.name, juce::dontSendNotification);
    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    nameLabel.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    nameLabel.setEditable (false, true, false);  // single-click no, double-click YES, no submit-on-empty
    nameLabel.setTooltip ("Double-click to rename, right-click for colour");
    nameLabel.onTextChange = [this]
    {
        auto txt = nameLabel.getText().trim();
        if (txt.isEmpty()) txt = juce::String (trackIndex + 1);
        nameLabel.setText (txt, juce::dontSendNotification);
        if (txt == track.name) return;
        const auto oldName = track.name;
        auto& um = engine.getUndoManager();
        um.beginNewTransaction ("Track name");
        um.perform (new ParamEditAction (
            [&s = session, idx = trackIndex, txt]     { s.track (idx).name = txt; },
            [&s = session, idx = trackIndex, oldName] { s.track (idx).name = oldName; }));
    };
    addAndMakeVisible (nameLabel);

    // ── Top filter section: HPF + LPF, white-faced (SSL 9000 J grammar).
    const auto filterWhite = juce::Colour (sslEqColors::kFilterWhite);
    hpfLabel.setText ("HPF", juce::dontSendNotification);
    hpfLabel.setJustificationType (juce::Justification::centred);
    hpfLabel.setColour (juce::Label::textColourId, filterWhite);
    hpfLabel.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    addAndMakeVisible (hpfLabel);

    hpfKnob.setRange (ChannelStripParams::kHpfMinHz, ChannelStripParams::kHpfMaxHz, 1.0);
    hpfKnob.setSkewFactorFromMidPoint (80.0);
    hpfKnob.setDoubleClickReturnValue (true, ChannelStripParams::kHpfOffHz);
    hpfKnob.setTooltip ("HPF cutoff (turn fully down to bypass)");
    hpfKnob.setTitle ("High-pass frequency");
    hpfKnob.setHelpText ("Channel high-pass filter cutoff, in hertz; OFF when fully down.");
    styleCompactKnob (hpfKnob, filterWhite);
    enableValueLabel (hpfKnob, "", 0);
    hpfKnob.textFromValueFunction = [] (double v) -> juce::String
    {
        if (v <= ChannelStripParams::kHpfOffHz + 0.5) return "OFF";
        return formatFrequency (v);
    };
    hpfKnob.setValue (track.strip.hpfFreq.load (std::memory_order_relaxed), juce::dontSendNotification);
    hpfKnob.onValueChange = [this] { onHpfKnobChanged(); };
    hpfKnob.addMouseListener (this, false);
    addAndMakeVisible (hpfKnob);

    lpfLabel.setText ("LPF", juce::dontSendNotification);
    lpfLabel.setJustificationType (juce::Justification::centred);
    lpfLabel.setColour (juce::Label::textColourId, filterWhite);
    lpfLabel.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    addAndMakeVisible (lpfLabel);

    lpfKnob.setRange (ChannelStripParams::kLpfMinHz, ChannelStripParams::kLpfMaxHz, 1.0);
    lpfKnob.setSkewFactorFromMidPoint (8000.0);
    lpfKnob.setDoubleClickReturnValue (true, ChannelStripParams::kLpfOffHz);
    lpfKnob.setTooltip ("LPF cutoff (turn fully up to bypass)");
    lpfKnob.setTitle ("Low-pass frequency");
    lpfKnob.setHelpText ("Channel low-pass filter cutoff, in hertz; OFF when fully up.");
    styleCompactKnob (lpfKnob, filterWhite);
    enableValueLabel (lpfKnob, "", 0);
    lpfKnob.textFromValueFunction = [] (double v) -> juce::String
    {
        if (v >= ChannelStripParams::kLpfOffHz - 0.5) return "OFF";
        return formatFrequency (v);
    };
    lpfKnob.setValue (track.strip.lpfFreq.load (std::memory_order_relaxed), juce::dontSendNotification);
    lpfKnob.onValueChange = [this] { onLpfKnobChanged(); };
    addAndMakeVisible (lpfKnob);

    // ── EQ type chip (E/G) — small toggle that lives mid-strip between
    // the HM and LM rows in the freq column. Replaces the chip that
    // used to sit on the EQ header pill so the type indicator is more
    // prominent on the EQ panel itself.
    eqTypeChip.setMouseClickGrabsKeyboardFocus (false);
    eqTypeChip.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff5a3a20));   // brown default
    eqTypeChip.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff202024));   // black when G
    eqTypeChip.setColour (juce::TextButton::textColourOffId,  juce::Colours::white);
    eqTypeChip.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    eqTypeChip.setClickingTogglesState (true);
    eqTypeChip.setTooltip ("EQ type: E (Brown) / G (Black)");
    eqTypeChip.setToggleState (track.strip.eqBlackMode.load (std::memory_order_relaxed),
                                 juce::dontSendNotification);
    eqTypeChip.setButtonText (eqTypeChip.getToggleState() ? "G" : "E");
    eqTypeChip.onClick = [this]
    {
        const bool nowBlack = eqTypeChip.getToggleState();
        track.strip.eqBlackMode.store (nowBlack, std::memory_order_relaxed);
        eqTypeChip.setButtonText (nowBlack ? "G" : "E");
    };
    addAndMakeVisible (eqTypeChip);

    // ── EQ region ──
    // Single EQ header button — same grammar as COMP. Left-click
    // toggles eqEnabled; right-click pops the EQ-type picker
    // (Brown E / Black G). Built-in green LED illuminates when
    // engaged (auto-armed on first knob touch); text always white.
    // Replaces the prior [LED] + label + [E/G pill] triple.
    eqHeaderBtn = std::make_unique<CompHeaderButton> (
        [this] { return track.strip.eqEnabled.load (std::memory_order_relaxed); },
        [this]
        {
            const bool now = ! track.strip.eqEnabled.load (std::memory_order_relaxed);
            track.strip.eqEnabled.store (now, std::memory_order_release);
            if (eqHeaderBtn != nullptr) eqHeaderBtn->repaint();
        },
        [this] { showEqTypeMenu(); });
    eqHeaderBtn->setLabelText ("EQ");
    eqHeaderBtn->setTooltip ("Left-click to enable / disable. Right-click to choose type (Brown E / Black G).");
    addAndMakeVisible (eqHeaderBtn.get());

    // ── COMP region ──
    // Single COMP header button: left-click enables/disables, right-
    // click opens the mode picker. Shows "COMP" until the user picks
    // a mode, then shows the mode name (OPTO/FET/VCA). Built-in green
    // LED illuminates when the comp is engaged. Text always white.
    compModeButton = std::make_unique<CompHeaderButton> (
        [this] { return track.strip.compEnabled.load (std::memory_order_relaxed); },
        [this] { setCompEnabled (! track.strip.compEnabled.load (std::memory_order_relaxed)); },
        [this] { showCompModeMenu(); });
    addAndMakeVisible (compModeButton.get());

    refreshCompModeButtonState();
    refreshCompKnobVisibility();
    lastAppliedCompMode = juce::jlimit (0, 2,
        track.strip.compMode.load (std::memory_order_relaxed));

    auto setupCompKnob = [this] (juce::Slider& k, juce::Label& label, const juce::String& labelText,
                              double rangeMin, double rangeMax, double defaultVal,
                              double skewMid, const juce::String& tooltip,
                              std::atomic<float>& target,
                              const juce::String& valueSuffix, int decimals)
    {
        k.setRange (rangeMin, rangeMax, 0.01);
        if (skewMid > 0) k.setSkewFactorFromMidPoint (skewMid);
        k.setValue (target.load (std::memory_order_relaxed), juce::dontSendNotification);
        k.setDoubleClickReturnValue (true, defaultVal);
        k.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (fourKColors::kCompGold));
        k.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
        k.setTooltip (tooltip);
        // Accessibility (H4): tooltip drives the help text spoken on
        // focus/hover; labelText (e.g. "PEAK", "ATK") becomes the
        // role-name spoken before the value (e.g. "PEAK 30 percent").
        // textFromValueFunction (or the suffix + decimals fed to
        // enableValueLabel) supplies the spoken value via JUCE's
        // default AccessibilityHandler on juce::Slider.
        k.setTitle (labelText);
        k.setHelpText (tooltip);
        enableValueLabel (k, valueSuffix, decimals);
        k.onValueChange = [this, &k, &target]
        {
            target.store ((float) k.getValue(), std::memory_order_relaxed);
            armCompOnUserEdit();
        };

        label.setText (labelText, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, juce::Colour (0xffb07050));
        label.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
    };

    // ── Opto knobs (LA-2A: peak reduction + gain + LIMIT toggle) ──
    setupCompKnob (optoPeakRedKnob, optoPeakRedLabel, "PEAK",
                   0.0, 100.0, 30.0, 0.0, "Peak Reduction (%)",
                   track.strip.compOptoPeakRed, "", 0);
    setupCompKnob (optoGainKnob, optoGainLabel, "MAK",
                   0.0, 100.0, 50.0, 0.0, "Output gain (50 % = unity = 0 dB)",
                   track.strip.compOptoGain, "", 0);
    // Display the OPTO gain in dB (matches the popup editor's GAIN knob).
    // Storage stays in percent (0..100, 50 = unity) per the donor's
    // opto_gain parameter contract: dB = (pct - 50) / 2.5.
    optoGainKnob.textFromValueFunction = [] (double pct) -> juce::String
    {
        const double db = (pct - 50.0) / 2.5;
        return juce::String (db, 1) + " dB";
    };
    optoGainKnob.updateText();
    // Comp threshold + makeup right-click hooks. Each per-mode "threshold-
    // ish" / "makeup-ish" knob routes to the same logical MIDI Learn
    // target so one binding tracks the user's mode switches.
    optoPeakRedKnob.addMouseListener (this, false);
    optoGainKnob   .addMouseListener (this, false);
    addAndMakeVisible (optoPeakRedKnob);  addAndMakeVisible (optoPeakRedLabel);
    addAndMakeVisible (optoGainKnob);     addAndMakeVisible (optoGainLabel);

    optoLimitButton.setClickingTogglesState (true);
    optoLimitButton.setToggleState (track.strip.compOptoLimit.load (std::memory_order_relaxed),
                                     juce::dontSendNotification);
    optoLimitButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff181820));
    optoLimitButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd09060));
    optoLimitButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd09060));
    optoLimitButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    optoLimitButton.setTooltip ("Limit mode (more aggressive than Compress)");
    optoLimitButton.onClick = [this]
    {
        const bool nowOn = optoLimitButton.getToggleState();
        track.strip.compOptoLimit.store (nowOn, std::memory_order_relaxed);
        // Only arm the comp when the user is ENGAGING limit, not when
        // they're disengaging it. Toggling a sub-feature off shouldn't
        // imply "user wants the comp engaged".
        if (nowOn) armCompOnUserEdit();
    };
    addAndMakeVisible (optoLimitButton);

    // ── FET knobs (1176: in/out/atk/rel + ratio button row) ──
    setupCompKnob (fetInputKnob,   fetInputLabel,   "IN",
                   -20.0, 40.0, 0.0, 0.0, "Input drive (dB)",
                   track.strip.compFetInput, " dB", 1);
    setupCompKnob (fetOutputKnob,  fetOutputLabel,  "MAK",
                   -20.0, 20.0, 0.0, 0.0, "Output gain (dB)",
                   track.strip.compFetOutput, " dB", 1);
    setupCompKnob (fetAttackKnob,  fetAttackLabel,  "ATK",
                   0.02, 80.0, 0.2, 0.5, "Attack (ms)",
                   track.strip.compFetAttack, " ms", 2);
    setupCompKnob (fetReleaseKnob, fetReleaseLabel, "REL",
                   50.0, 1100.0, 400.0, 300.0, "Release (ms)",
                   track.strip.compFetRelease, " ms", 0);
    fetInputKnob .addMouseListener (this, false);
    fetOutputKnob.addMouseListener (this, false);
    addAndMakeVisible (fetInputKnob);   addAndMakeVisible (fetInputLabel);
    addAndMakeVisible (fetOutputKnob);  addAndMakeVisible (fetOutputLabel);
    addAndMakeVisible (fetAttackKnob);  addAndMakeVisible (fetAttackLabel);
    addAndMakeVisible (fetReleaseKnob); addAndMakeVisible (fetReleaseLabel);

    // FET ratio: 5-step knob (4:1 / 8:1 / 12:1 / 20:1 / All) - replaces the
    // earlier button row which couldn't fit the "All" label in narrow strips.
    fetRatioKnob.setRange (0.0, 4.0, 1.0);
    fetRatioKnob.setValue ((double) track.strip.compFetRatio.load (std::memory_order_relaxed),
                            juce::dontSendNotification);
    fetRatioKnob.setDoubleClickReturnValue (true, 0.0);
    fetRatioKnob.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (fourKColors::kCompGold));
    fetRatioKnob.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
    fetRatioKnob.setTooltip ("FET Ratio (4 / 8 / 12 / 20 / All-buttons)");
    fetRatioKnob.setTitle ("FET ratio");
    fetRatioKnob.setHelpText ("FET compressor ratio: 4 to 1, 8 to 1, 12 to 1, 20 to 1, or All-buttons-in mode.");
    enableValueLabel (fetRatioKnob, "", 0);
    fetRatioKnob.textFromValueFunction = [] (double v) -> juce::String
    {
        const int idx = juce::jlimit (0, 4, (int) std::round (v));
        const char* names[] = { "4:1", "8:1", "12:1", "20:1", "All" };
        return names[idx];
    };
    fetRatioKnob.onValueChange = [this]
    {
        track.strip.compFetRatio.store (
            juce::jlimit (0, 4, (int) std::round (fetRatioKnob.getValue())),
            std::memory_order_relaxed);
        armCompOnUserEdit();
    };
    fetRatioKnob.updateText();
    addAndMakeVisible (fetRatioKnob);

    fetRatioLabel.setText ("RATIO", juce::dontSendNotification);
    fetRatioLabel.setJustificationType (juce::Justification::centred);
    fetRatioLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb07050));
    fetRatioLabel.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
    addAndMakeVisible (fetRatioLabel);

    // ── VCA knobs (ratio/attack/release/output - threshold lives on the
    //   meter-strip drag handle so the engineer pulls it directly against
    //   the input level, the way a real VCA panel works) ──
    setupCompKnob (vcaRatioKnob,   vcaRatioLabel,   "RAT",
                   1.0, 120.0, 4.0, 4.0, "Ratio (n:1)",
                   track.strip.compVcaRatio, ":1", 1);
    setupCompKnob (vcaAttackKnob,  vcaAttackLabel,  "ATK",
                   0.1, 50.0, 1.0, 5.0, "Attack (ms)",
                   track.strip.compVcaAttack, " ms", 1);
    setupCompKnob (vcaReleaseKnob, vcaReleaseLabel, "REL",
                   10.0, 5000.0, 100.0, 200.0, "Release (ms)",
                   track.strip.compVcaRelease, " ms", 0);
    setupCompKnob (vcaOutputKnob,  vcaOutputLabel,  "MAK",
                   -20.0, 20.0, 0.0, 0.0, "Output gain (dB)",
                   track.strip.compVcaOutput, " dB", 1);

    vcaOutputKnob.addMouseListener (this, false);
    addAndMakeVisible (vcaRatioKnob);    addAndMakeVisible (vcaRatioLabel);
    addAndMakeVisible (vcaAttackKnob);   addAndMakeVisible (vcaAttackLabel);
    addAndMakeVisible (vcaReleaseKnob);  addAndMakeVisible (vcaReleaseLabel);
    addAndMakeVisible (vcaOutputKnob);   addAndMakeVisible (vcaOutputLabel);

    // Vertical comp metering with threshold drag-handle (Mixbus-style).
    compMeter = std::make_unique<CompMeterStrip> (track);
    // Experimental track-3 fader-side layout: hide the IN bar (the main
    // level meter already shows it) but keep the threshold-drag triangle
    // handle so the engineer can still set the comp threshold from the
    // strip. GR bar stays slim — column width sized accordingly below.
    if (usesFaderThresholdLayout())
    {
        compMeter->setShowInputBar (false);
        compMeter->setHandleVisible (true);   // pure-GR + handle stays
        // Handle on the RIGHT side of the GR bar — keeps the bar's left
        // edge clean against the main meter glued to it on the left.
        compMeter->setHandleOnRight (true);
        // Lock the threshold-drag range to the level-meter scale's visible
        // range (no +6 headroom, no +12 VCA neutral extension) so the
        // triangle handle's y position is 1:1 with the fader scale: drag
        // to -18 dB and the handle sits on the "-18" tick of the fader
        // scale. VCA's +12 "no compression" neutral can still be reached
        // via the COMP editor's threshold rotary if needed.
        compMeter->setRangeDb (-60.0f, 0.0f);
        // Note: fader-side overrides (NoTextBox + restricted range) are
        // applied AFTER the default fader setup further down in the
        // ctor (search for "Track-3 fader overrides") — otherwise the
        // default block clobbers them.
        // Drop unit suffixes on the single-row FET / VCA comp knobs so
        // the value text ("0.0", "0.20", "400", "4.0") fits in the narrow
        // 4-knob cell width. Units already implied by the RAT/OUT/ATK/REL
        // header labels.
        for (auto* k : { &fetOutputKnob, &fetAttackKnob, &fetReleaseKnob,
                         &vcaRatioKnob, &vcaAttackKnob, &vcaReleaseKnob, &vcaOutputKnob })
        {
            k->setTextValueSuffix ("");
            k->updateText();
        }
    }
    // Track-3 always shows the AUX-sends row beneath COMP, regardless of
    // mixingMode (which gates aux visibility on other strips). Forced
    // visible at ctor so the row appears on the first paint.
    if (usesFaderThresholdLayout())
    {
        for (auto& l : auxIndexLabels) l.setVisible (true);
        for (auto& k : auxKnobs)       if (k != nullptr) k->setVisible (true);
        for (auto& l : auxKnobLabels)  l.setVisible (true);
    }
    addAndMakeVisible (compMeter.get());

    startTimerHz (30);  // input + GR meter refresh on the main strip

    for (size_t i = 0; i < bandSpecs().size(); ++i)
    {
        const auto& spec = bandSpecs()[i];
        auto& row = eqRows[i];

        row.rowLabel.setText (spec.rowName, juce::dontSendNotification);
        row.rowLabel.setJustificationType (juce::Justification::centred);
        row.rowLabel.setColour (juce::Label::textColourId, spec.accent.brighter (0.2f));
        row.rowLabel.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        addAndMakeVisible (row.rowLabel);

        row.gain = std::make_unique<juce::Slider>();
        styleCompactKnob (*row.gain, spec.accent);
        row.gain->setRange (ChannelStripParams::kBandGainMin, ChannelStripParams::kBandGainMax, 0.1);
        row.gain->setDoubleClickReturnValue (true, 0.0);
        row.gain->setTooltip (juce::String (spec.rowName) + " Gain (dB)");
        enableValueLabel (*row.gain, "", 1);
        row.gain->textFromValueFunction = [] (double v) { return formatBandGain (v); };
        row.gain->setValue (spec.gainPtr (track.strip)->load (std::memory_order_relaxed),
                            juce::dontSendNotification);
        row.gain->updateText();  // setValue skips updateText() when value didn't change (default 0 → loaded 0)
        {
            auto* atomicPtr = spec.gainPtr (track.strip);
            auto* knob = row.gain.get();
            auto* eqEnabledPtr = &track.strip.eqEnabled;
            knob->onValueChange = [knob, atomicPtr, eqEnabledPtr]
            {
                atomicPtr->store ((float) knob->getValue(), std::memory_order_relaxed);
                // Auto-arm: touching any EQ-band knob engages the EQ.
                // Off by default (Session.h) so the LED only lights once
                // the engineer actually shapes the sound.
                eqEnabledPtr->store (true, std::memory_order_release);
            };
        }
        // Mouse listener so the strip's mouseDown handler can route a
        // right-click on this band's gain slider to MIDI Learn.
        row.gain->addMouseListener (this, false);
        addAndMakeVisible (row.gain.get());

        row.freq = std::make_unique<juce::Slider>();
        styleCompactKnob (*row.freq, spec.accent);
        row.freq->setRange (spec.freqMin, spec.freqMax, 1.0);
        row.freq->setSkewFactorFromMidPoint ((double) spec.freqDefault);
        row.freq->setDoubleClickReturnValue (true, spec.freqDefault);
        row.freq->setTooltip (juce::String (spec.rowName) + " Frequency (Hz)");
        enableValueLabel (*row.freq, "", 0);
        // textFromValueFunction must be set BEFORE setValue, otherwise the
        // initial text is rendered with the default formatter ("2000") and
        // doesn't get our "2.0k" notation until the user moves the knob.
        row.freq->textFromValueFunction = [] (double v) { return formatFrequency (v); };
        row.freq->setValue (spec.freqPtr (track.strip)->load (std::memory_order_relaxed),
                            juce::dontSendNotification);

        // Q knob (mid-bands only - bell-only HM/LM). Same size and styling
        // as gain and freq, with a "Q 0.7"-style value label below so the
        // user can read the bandwidth without hovering.
        if (spec.qPtr != nullptr)
        {
            row.q = std::make_unique<juce::Slider>();
            styleCompactKnob (*row.q, spec.accent);
            row.q->setRange (ChannelStripParams::kBandQMin, ChannelStripParams::kBandQMax, 0.01);
            row.q->setSkewFactorFromMidPoint (1.0);  // Q midpoint at 1.0
            row.q->setDoubleClickReturnValue (true, 0.7);
            row.q->setTooltip (juce::String (spec.rowName) + " Q (bandwidth)");
            enableValueLabel (*row.q, "", 1);
            row.q->setValue (spec.qPtr (track.strip)->load (std::memory_order_relaxed),
                              juce::dontSendNotification);
            {
                auto* atomicPtr = spec.qPtr (track.strip);
                auto* knob = row.q.get();
                auto* eqEnabledPtr = &track.strip.eqEnabled;
                knob->onValueChange = [knob, atomicPtr, eqEnabledPtr]
                {
                    atomicPtr->store ((float) knob->getValue(), std::memory_order_relaxed);
                    eqEnabledPtr->store (true, std::memory_order_release);
                };
            }
            row.q->addMouseListener (this, false);   // right-click → MIDI Learn (TrackEqQ)
            addAndMakeVisible (row.q.get());

            row.qLabel.setText ("Q", juce::dontSendNotification);
            row.qLabel.setJustificationType (juce::Justification::centred);
            row.qLabel.setColour (juce::Label::textColourId, spec.accent.brighter (0.2f));
            row.qLabel.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            addAndMakeVisible (row.qLabel);
        }
        {
            auto* atomicPtr = spec.freqPtr (track.strip);
            auto* knob = row.freq.get();
            auto* eqEnabledPtr = &track.strip.eqEnabled;
            knob->onValueChange = [knob, atomicPtr, eqEnabledPtr]
            {
                atomicPtr->store ((float) knob->getValue(), std::memory_order_relaxed);
                eqEnabledPtr->store (true, std::memory_order_release);
            };
        }
        row.freq->addMouseListener (this, false);   // right-click → MIDI Learn (TrackEqFreq)
        addAndMakeVisible (row.freq.get());
    }

    // ── Bus assigns - on/off toggles laid out vertically alongside the fader.
    // Each button takes the colour of its corresponding aux bus header (the
    // same HSV ramp Session::Session() uses to colour the four aux strips),
    // so the engineer reads "BUS 1 = teal, BUS 2 = blue, …" without consulting
    // the strip headers. ──
    for (int i = 0; i < ChannelStripParams::kNumBuses; ++i)
    {
        const auto busColour = session.bus (i).colour;
        lastBusColours[(size_t) i] = busColour.getARGB();
        auto btn = std::make_unique<juce::TextButton> (juce::String (i + 1));
        btn->setClickingTogglesState (true);
        btn->setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
        btn->setColour (juce::TextButton::buttonOnColourId, busColour);
        btn->setColour (juce::TextButton::textColourOffId,  busColour.brighter (0.15f));
        btn->setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
        btn->setToggleState (track.strip.busAssign[(size_t) i].load (std::memory_order_relaxed),
                              juce::dontSendNotification);
        btn->setTooltip ("Route post-fader signal to BUS " + juce::String (i + 1));
        btn->onClick = [this, i]
        {
            track.strip.busAssign[(size_t) i].store (busButtons[(size_t) i]->getToggleState(),
                                                      std::memory_order_relaxed);
        };
        addAndMakeVisible (btn.get());
        busButtons[(size_t) i] = std::move (btn);
    }

    // ── Pan / Fader / M / S ──
    panLabel.setText ("PAN", juce::dontSendNotification);
    panLabel.setJustificationType (juce::Justification::centred);
    panLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe07070));
    panLabel.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
    addAndMakeVisible (panLabel);

    panKnob.setRange (-1.0, 1.0, 0.001);
    panKnob.setDoubleClickReturnValue (true, 0.0);
    panKnob.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xffc04040));  // red pan
    panKnob.setTitle ("Pan");
    panKnob.setHelpText ("Stereo pan; L100 to R100, C is centre.");
    enableValueLabel (panKnob, "", 0);
    panKnob.textFromValueFunction = [] (double v) -> juce::String
    {
        if (std::abs (v) < 0.01) return "C";
        const int pct = (int) std::round (std::abs (v) * 100.0);
        return (v < 0 ? "L" : "R") + juce::String (pct);
    };
    panKnob.setValue (track.strip.pan.load (std::memory_order_relaxed), juce::dontSendNotification);
    panKnob.updateText();   // force "C" / "L<pct>" / "R<pct>" via textFromValueFunction at init
    panKnob.onValueChange = [this]
    {
        track.strip.pan.store ((float) panKnob.getValue(), std::memory_order_relaxed);
    };
    panKnob.onDragStart = [this]
    {
        track.strip.panTouched.store (true, std::memory_order_release);
    };
    panKnob.onDragEnd = [this]
    {
        track.strip.panTouched.store (false, std::memory_order_release);
    };
    addAndMakeVisible (panKnob);

    faderSlider.setRange (ChannelStripParams::kFaderMinDb, ChannelStripParams::kFaderMaxDb, 0.1);
    faderSlider.setSkewFactorFromMidPoint (-12.0);
    faderSlider.setValue (track.strip.faderDb.load (std::memory_order_relaxed), juce::dontSendNotification);
    faderSlider.setDoubleClickReturnValue (true, 0.0);
    faderSlider.setTitle ("Channel fader");
    faderSlider.setHelpText ("Channel level in decibels; double-click to reset to 0 dB.");
    // Domain-specific text for screen readers + the visible textbox.
    // JUCE's default AccessibilityHandler on juce::Slider calls
    // getCurrentValueAsText() → textFromValueFunction, so the announced
    // value matches what's drawn ("-4.2 dB" rather than raw "-4.2").
    faderSlider.textFromValueFunction = [] (double v) -> juce::String
    {
        if (v <= ChannelStripParams::kFaderMinDb + 0.05) return "-INF dB";
        return juce::String (v, 1) + " dB";
    };
    // No "dB" suffix - strip is narrow enough that "0.0 dB" truncates.
    // The dB scale column to the right of the meter makes the unit obvious.
    faderSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 50, 16);
    // Borderless value box — matches the rotary-knob textbox grammar so the
    // fader number reads as text-only instead of a framed pill.
    faderSlider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    faderSlider.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
    faderSlider.setColour (juce::Slider::textBoxTextColourId,       juce::Colour (0xffd8d8d8));
    faderSlider.setTooltip ("Channel fader (dB) - double-click to reset to 0 dB");
    faderSlider.onValueChange = [this]
    {
        const auto newDb = (float) faderSlider.getValue();
        track.strip.faderDb.store (newDb, std::memory_order_relaxed);

        // Fader-group propagation. While dragging, write the same
        // relative-dB delta to every peer in this strip's group.
        // peerActive[] was built at onDragStart so the inner walk is
        // a single pass; we don't re-query session.tracks every value
        // change. Peer ChannelStrips' own 30 Hz timers pull faderDb back
        // to their slider widgets so the visual sync is automatic.
        const int gid = track.strip.faderGroupId.load (std::memory_order_relaxed);
        if (gid != 0)
        {
            const float delta = newDb - faderDragAnchorDb;
            for (int t = 0; t < Session::kNumTracks; ++t)
            {
                if (t == trackIndex || ! peerActive[(size_t) t]) continue;
                const float target = juce::jlimit (
                    ChannelStripParams::kFaderMinDb, ChannelStripParams::kFaderMaxDb,
                    peerAnchorsDb[(size_t) t] + delta);
                session.track (t).strip.faderDb.store (target,
                                                          std::memory_order_relaxed);
            }
        }
    };
    // Touch-mode hooks: while the user has the fader grabbed, set the
    // strip's faderTouched flag so the audio engine routes the manual
    // setpoint instead of the lane (and the timerCallback captures into
    // the lane while touched). On release, the existing fader smoother's
    // 20 ms ramp blends from manual back to lane - a fast but smooth
    // glide. The configurable 100 ms / 500 ms / 1 s glide-back from the
    // spec is a refinement that lands later if 20 ms feels jarring in
    // practice.
    faderSlider.onDragStart = [this]
    {
        track.strip.faderTouched.store (true, std::memory_order_release);

        // Snapshot anchors for fader-group propagation. Capture this
        // strip's current dB AND every peer's so the relative offset
        // between members is preserved across the drag (no "smash to
        // unison" if the user grabs a fader at +3 in a group where
        // others sit at -10).
        faderDragAnchorDb = (float) faderSlider.getValue();
        peerActive.fill (false);
        const int gid = track.strip.faderGroupId.load (std::memory_order_relaxed);
        if (gid != 0)
        {
            for (int t = 0; t < Session::kNumTracks; ++t)
            {
                if (t == trackIndex) continue;
                if (session.track (t).strip.faderGroupId.load (std::memory_order_relaxed) == gid)
                {
                    peerActive[(size_t) t]    = true;
                    peerAnchorsDb[(size_t) t] = session.track (t).strip.faderDb.load (
                        std::memory_order_relaxed);
                }
            }
        }
    };
    faderSlider.onDragEnd = [this]
    {
        track.strip.faderTouched.store (false, std::memory_order_release);
        peerActive.fill (false);
    };
    faderSlider.addMouseListener (this, false);
    addAndMakeVisible (faderSlider);

    // Track-3 fader overrides — applied AFTER the default range / textbox
    // setup above so they aren't clobbered. Restricts range to the
    // visible scale (-90 .. +6) and uses the on-fader labels instead of
    // a textbox.
    if (usesFaderThresholdLayout())
    {
        faderSlider.getProperties().set ("dusk_drawFaderScaleLabels", true);
        // NO textbox inside the slider — at min value the cap (36 px tall)
        // would always overlap a textbox hosted inside the slider's
        // bounds (textbox is 14 px tall, can't escape the cap geometry).
        // Use a separate faderValueLabel positioned below the slider in
        // resized(), updated from onValueChange below.
        faderSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        faderSlider.setRange (-90.0, 6.0, 0.1);
        faderSlider.setSkewFactorFromMidPoint (-12.0);
        const float curDb = track.strip.faderDb.load (std::memory_order_relaxed);
        faderSlider.setValue (juce::jlimit (-90.0, 6.0, (double) curDb),
                                 juce::dontSendNotification);

        // Configure the standalone value readout.
        faderValueLabel.setJustificationType (juce::Justification::centred);
        faderValueLabel.setColour (juce::Label::textColourId,
                                     juce::Colour (0xffd8d8d8));
        faderValueLabel.setColour (juce::Label::backgroundColourId,
                                     juce::Colours::transparentBlack);
        faderValueLabel.setColour (juce::Label::outlineColourId,
                                     juce::Colours::transparentBlack);
        faderValueLabel.setFont (juce::Font (juce::FontOptions (
            juce::Font::getDefaultMonospacedFontName(), 14.0f, juce::Font::bold)));
        auto formatDb = [] (double db) -> juce::String
        {
            return (db <= -89.95) ? juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x9e"))   /* ∞ = -inf dB / fully off */
                                  : juce::String (db, 1);
        };
        faderValueLabel.setText (formatDb (faderSlider.getValue()),
                                   juce::dontSendNotification);
        addAndMakeVisible (faderValueLabel);

        // Update the standalone label whenever the slider value changes
        // (drag, automation, programmatic setValue). Chain onto the
        // existing onValueChange handler set earlier in the ctor —
        // capture it so the original behaviour (atom store, group
        // propagation) still runs.
        const auto previousOnChange = faderSlider.onValueChange;
        faderSlider.onValueChange = [this, previousOnChange]
        {
            if (previousOnChange) previousOnChange();
            refreshFaderValueLabel();
        };
    }

    muteButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::orangered);
    muteButton.setToggleState (track.strip.mute.load (std::memory_order_relaxed), juce::dontSendNotification);
    muteButton.setTooltip ("Mute (M) - silences this channel at the master");
    muteButton.onClick = [this]
    {
        const bool newState = muteButton.getToggleState();
        track.strip.mute.store (newState, std::memory_order_relaxed);

        // Discrete-param automation capture: a mute click in Write or
        // Touch mode (with transport playing) writes a transition point
        // into the lane at the current playhead. Discrete = no
        // interpolation, no tick-based capture - the click IS the only
        // event worth recording. WRITE only: discrete params have no
        // `touched` flag, and the audio thread reads the discrete lane
        // unconditionally in Touch (routeDiscrete) - so capturing in Touch
        // would push_back into a vector the audio thread is mid-read on
        // (data race / realloc UAF). In Write the audio reads `manual`, so
        // the append never overlaps a lane read. (In Touch the lane already
        // overrides the manual toggle, so the click was a no-op audibly.)
        const int amode = track.automationMode.load (std::memory_order_relaxed);
        const bool capturing = engine.getTransport().isPlaying()
            && amode == (int) AutomationMode::Write;
        if (capturing)
            captureWritePoint (AutomationParam::Mute, newState ? 1.0f : 0.0f);
    };
    muteButton.addMouseListener (this, false);
    addAndMakeVisible (muteButton);

    soloButton.setClickingTogglesState (true);
    soloButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::yellow.darker (0.2f));
    soloButton.setToggleState (track.strip.solo.load (std::memory_order_relaxed), juce::dontSendNotification);
    soloButton.setTooltip ("Solo (S) - when any channel is soloed, only soloed channels are heard");
    soloButton.onClick = [this]
    {
        // Route through the session-level setter so the counter-backed
        // path stays current. Note that anyTrackSoloed() now scans
        // liveSolo so it works with automation-overridden solos too,
        // but the counter is still updated for consistency.
        session.setTrackSoloed (trackIndex, soloButton.getToggleState());

        // Discrete-param automation capture - WRITE only, same rationale as
        // mute: the audio thread reads the discrete lane in Touch, so a
        // Touch-mode capture would race that read.
        const int amode = track.automationMode.load (std::memory_order_relaxed);
        const bool capturing = engine.getTransport().isPlaying()
            && amode == (int) AutomationMode::Write;
        if (capturing)
            captureWritePoint (AutomationParam::Solo, soloButton.getToggleState() ? 1.0f : 0.0f);
    };
    soloButton.addMouseListener (this, false);
    addAndMakeVisible (soloButton);

    phaseButton.setClickingTogglesState (true);
    phaseButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff70c0d0));
    phaseButton.setToggleState (track.strip.phaseInvert.load (std::memory_order_relaxed),
                                 juce::dontSendNotification);
    phaseButton.setTooltip ("Polarity invert - flips the input signal's polarity");
    phaseButton.onClick = [this]
    {
        track.strip.phaseInvert.store (phaseButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (phaseButton);

    autoModeButton.setTooltip ("Automation mode (click to pick: Off / Read / Write / Touch). "
                                "READ replays the recorded ride; WRITE captures "
                                "fader+pan moves; TOUCH replays until you grab a "
                                "control, then captures while held.");
    autoModeButton.onClick = [this] { showAutoModeMenu(); };
    // Borderless: same grammar as the other strip labels — text only,
    // mode colour applied to the text via refreshAutoModeButton().
    autoModeButton.setColour (juce::TextButton::buttonColourId,   juce::Colours::transparentBlack);
    autoModeButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (autoModeButton);
    refreshAutoModeButton();
    displayedLiveFaderDb = track.strip.liveFaderDb.load (std::memory_order_relaxed);
    displayedLivePan     = track.strip.livePan    .load (std::memory_order_relaxed);

    // ── Peak input level readout ──
    inputPeakLabel.setJustificationType (juce::Justification::centred);
    inputPeakLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd0d0d0));
    inputPeakLabel.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    // No outline - the label sat next to the fader's value textbox and the
    // 1 px border drew on top of the textbox edge, looking like overlap.
    inputPeakLabel.setColour (juce::Label::outlineColourId,    juce::Colours::transparentBlack);
    inputPeakLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                            10.0f, juce::Font::bold)));
    inputPeakLabel.setMinimumHorizontalScale (1.0f);   // never truncate to "..."
    inputPeakLabel.setText ("-inf", juce::dontSendNotification);
    addAndMakeVisible (inputPeakLabel);

    // GR readout - sits to the right of the input peak. Negative dB when
    // the comp is pulling the signal down. Uses gold-on-black to read as a
    // comp-section indicator distinct from the input level.
    grPeakLabel.setJustificationType (juce::Justification::centred);
    grPeakLabel.setColour (juce::Label::textColourId,        juce::Colour (0xffe0c050));
    grPeakLabel.setColour (juce::Label::backgroundColourId,  juce::Colours::transparentBlack);
    grPeakLabel.setColour (juce::Label::outlineColourId,     juce::Colours::transparentBlack);
    grPeakLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                          12.0f, juce::Font::bold)));
    grPeakLabel.setText ("0.0", juce::dontSendNotification);
    grPeakLabel.setTooltip ("Gain reduction in dB (negative = comp pulling down). "
                             "Goes inert when the comp is bypassed.");
    addAndMakeVisible (grPeakLabel);

    // Track-3 fader-side GR numeric readout — mirrors grPeakLabel's
    // styling so it reads as the same UI element, just attached to the
    // fader-side slim LED instead of the COMP section. Hidden by default;
    // resized() flips it on when the layout is active.
    grReadoutLabel.setJustificationType (juce::Justification::centred);
    grReadoutLabel.setColour (juce::Label::textColourId,        juce::Colour (0xffe0c050));
    grReadoutLabel.setColour (juce::Label::backgroundColourId,  juce::Colours::transparentBlack);
    grReadoutLabel.setColour (juce::Label::outlineColourId,     juce::Colours::transparentBlack);
    grReadoutLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                              12.0f, juce::Font::bold)));
    grReadoutLabel.setMinimumHorizontalScale (1.0f);
    grReadoutLabel.setText ("0.0", juce::dontSendNotification);
    grReadoutLabel.setTooltip ("Gain reduction in dB. Inert while the comp is bypassed.");
    grReadoutLabel.setVisible (false);
    addAndMakeVisible (grReadoutLabel);

    // Track-3 fader-side layout: the bottom-row readouts live in a far
    // narrower column than the default kPeakColumnW slot, so the 12-pt
    // monospaced font (chosen for the wider default layout) truncates
    // "-13.5"/"-inf" with an ellipsis. Override AFTER the default setup
    // so these stick; small font + permissive horizontal scale so JUCE
    // squishes to fit instead of clipping.
    if (usesFaderThresholdLayout())
    {
        const juce::Font readoutFont (juce::FontOptions (
            juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::bold));
        inputPeakLabel.setFont (readoutFont);
        inputPeakLabel.setMinimumHorizontalScale (0.40f);
        grReadoutLabel.setFont (readoutFont);
        grReadoutLabel.setMinimumHorizontalScale (0.40f);
    }

    // "THR" label sits above CompMeterStrip's drag handle so the engineer
    // sees what the triangle pointer controls. Same gold tone as the COMP
    // section labels so it reads as part of the comp UI.
    threshMeterLabel.setText ("THR", juce::dontSendNotification);
    threshMeterLabel.setJustificationType (juce::Justification::centred);
    threshMeterLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb07050));
    threshMeterLabel.setFont (juce::Font (juce::FontOptions (8.5f, juce::Font::bold)));
    addAndMakeVisible (threshMeterLabel);

    // ── Input monitor toggle (IN) ──
    monitorButton.setClickingTogglesState (true);
    monitorButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    monitorButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (fourKColors::kPanCyan));
    monitorButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff708090));
    monitorButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    monitorButton.setToggleState (track.inputMonitor.load (std::memory_order_relaxed),
                                   juce::dontSendNotification);
    monitorButton.setTooltip ("Input monitor - when on, live input is heard at the master. "
                              "When off, the track still records and meters but is silent.");
    monitorButton.onClick = [this]
    {
        track.inputMonitor.store (monitorButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (monitorButton);

    // ── Record arm ──
    armButton.setClickingTogglesState (true);
    armButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    armButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd03030));
    armButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd06060));
    armButton.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    armButton.setToggleState (track.recordArmed.load (std::memory_order_relaxed),
                               juce::dontSendNotification);
    armButton.setTooltip ("Record arm - press REC on the transport to capture this input");
    armButton.onClick = [this]
    {
        // A frozen track can't record (playback is the baked WAV) — captured
        // MIDI would be silent and desync the rendering. Refuse + restore the
        // toggle; unfreeze to record.
        if (armButton.getToggleState()
            && track.frozen.load (std::memory_order_relaxed))
        {
            armButton.setToggleState (false, juce::dontSendNotification);
            showDuskAlert (*this, "Track is frozen", "Unfreeze this track to record.");
            return;
        }
        session.setTrackArmed (trackIndex, armButton.getToggleState());
    };
    armButton.addMouseListener (this, false);
    addAndMakeVisible (armButton);

    // PRINT toggle - when on, recording captures the post-EQ/post-comp signal
    // so effects are committed to the WAV. Off (default) = clean input on
    // disk so the engineer can re-EQ / re-comp at mix time.
    printButton.setClickingTogglesState (true);
    printButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    printButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd09060));
    printButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff8a7060));
    printButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    printButton.setToggleState (track.printEffects.load (std::memory_order_relaxed),
                                 juce::dontSendNotification);
    printButton.setTooltip ("PRINT - when on, the recorded WAV captures the post-EQ/post-comp signal "
                             "(effects baked in). Off = clean input recorded; effects only at playback.");
    printButton.onClick = [this]
    {
        // Branch on the SAME condition refreshPrintButtonForMode uses to label
        // the button: FREEZE for any MIDI track or a recorded audio track,
        // PRINT for an empty audio track.
        const bool isMidi  = track.mode.load (std::memory_order_relaxed)
                                 == (int) Track::Mode::Midi;
        const bool frozen  = track.frozen.load (std::memory_order_relaxed);
        const bool freeze  = frozen || isMidi || ! track.regions.empty();
        if (freeze)
            handleFreezeClick();           // FREEZE / unfreeze command
        else
            track.printEffects.store (printButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (printButton);
    // Mode-aware enabled state. PRINT only commits post-effects
    // audio; on a MIDI track the audio source is the instrument
    // plugin rendered at playback time, not at capture, so PRINT
    // is a no-op there. Grey out + swap the tooltip so the user
    // doesn't waste a click. Re-run on every applyMode() so a
    // mode flip toggles the disabled state.
    refreshPrintButtonForMode();

    // ── Input selector ──
    // -2 = follow track index; -1 = none; 0..N = explicit input. We populate
    // a small set of options here; the device may have fewer inputs at runtime
    // and we'll just route silence in that case.
    inputSelector.addItem ("In " + juce::String (trackIndex + 1), 1);   // ID 1 = follow (-2)
    inputSelector.addItem ("None",                                  2); // ID 2 = -1
    for (int i = 0; i < 16; ++i)
        inputSelector.addItem ("In " + juce::String (i + 1) + " (fixed)", 100 + i);  // ID 100+i = explicit

    const int currentSrc = track.inputSource.load (std::memory_order_relaxed);
    if      (currentSrc == -2) inputSelector.setSelectedId (1, juce::dontSendNotification);
    else if (currentSrc == -1) inputSelector.setSelectedId (2, juce::dontSendNotification);
    else                       inputSelector.setSelectedId (100 + currentSrc, juce::dontSendNotification);

    auto styleCombo = [] (juce::ComboBox& c)
    {
        c.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff202024));
        c.setColour (juce::ComboBox::textColourId,       juce::Colour (0xffd0d0d0));
        c.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xff404048));
    };
    styleCombo (inputSelector);
    inputSelector.onChange = [this] { onInputSelectorChanged(); };
    addChildComponent (inputSelector);   // hidden inline; lives in the I/O popup

    // ── Mode selector (Mono / Stereo / MIDI) ──
    modeSelector.addItem ("Mono",   1);   // ID 1 = Mode::Mono
    modeSelector.addItem ("Stereo", 2);   // ID 2 = Mode::Stereo
    modeSelector.addItem ("MIDI",   3);   // ID 3 = Mode::Midi
    modeSelector.setSelectedId (track.mode.load (std::memory_order_relaxed) + 1,
                                  juce::dontSendNotification);
    modeSelector.setTooltip ("Track signal mode. Mono = 1 audio input. "
                              "Stereo = 2 audio inputs (L + R) recorded as a "
                              "stereo WAV. MIDI = capture from a MIDI port "
                              "and feed the strip's hosted plugin.");
    styleCombo (modeSelector);
    modeSelector.onChange = [this] { onTrackModeChanged(); };
    addChildComponent (modeSelector);   // hidden inline; lives in the I/O popup

    // ── Stereo R-channel input (mirrors the L selector's options) ──
    inputSelectorR.addItem ("In " + juce::String (trackIndex + 2), 1);   // ID 1 = follow (-2 -> L+1)
    inputSelectorR.addItem ("None", 2);                                   // ID 2 = -1
    for (int i = 0; i < 16; ++i)
        inputSelectorR.addItem ("In " + juce::String (i + 1) + " (fixed)", 100 + i);
    {
        const int rSrc = track.inputSourceR.load (std::memory_order_relaxed);
        if      (rSrc == -2) inputSelectorR.setSelectedId (1, juce::dontSendNotification);
        else if (rSrc == -1) inputSelectorR.setSelectedId (2, juce::dontSendNotification);
        else                 inputSelectorR.setSelectedId (100 + rSrc, juce::dontSendNotification);
    }
    styleCombo (inputSelectorR);
    inputSelectorR.onChange = [this]
    {
        const int id = inputSelectorR.getSelectedId();
        int v = -2;
        if      (id == 1)              v = -2;
        else if (id == 2)              v = -1;
        else if (id >= 100 && id < 200) v = id - 100;
        track.inputSourceR.store (v, std::memory_order_relaxed);
        refreshIoConfigButton();
    };
    addChildComponent (inputSelectorR);   // visibility toggled by refreshInputSelectorVisibility

    // ── MIDI input selector — populated from the engine's current MIDI
    // input bank. Re-populated whenever AudioEngine signals a refresh
    // (USB hot-plug etc.) via the ChangeListener wiring in the dtor /
    // changeListenerCallback below. Item ID 1 = "(none)" (maps to
    // track.midiInputIndex = -1). Subsequent IDs are 2 + deviceIndex.
    rebuildMidiInputDropdown();
    midiInputSelector.onChange = [this]
    {
        const int id = midiInputSelector.getSelectedId();
        const int idx = id <= 1 ? -1 : (id - 2);
        track.midiInputIndex.store (idx, std::memory_order_relaxed);
        refreshIoConfigButton();
        // Capture the stable identifier alongside the index so a later
        // session save records WHICH device this was, not just where it
        // happened to sit in the list. Re-querying inside the change
        // handler is fine - it's a message-thread-only operation that
        // runs at most once per user click.
        if (idx >= 0)
        {
            const auto& inputs = engine.getMidiInputDevices();
            track.midiInputIdentifier = (idx < inputs.size())
                                          ? inputs[idx].identifier
                                          : juce::String();
        }
        else
        {
            track.midiInputIdentifier = juce::String();
        }
    };
    styleCombo (midiInputSelector);
    addChildComponent (midiInputSelector);

    // ── MIDI channel filter (Omni / 1..16) ──
    // ID 1 = Omni (atom = 0), IDs 2..17 = channels 1..16 (atom = 1..16).
    midiChannelSelector.addItem ("Omni", 1);
    for (int ch = 1; ch <= 16; ++ch)
        midiChannelSelector.addItem ("Ch " + juce::String (ch), 1 + ch);
    {
        const int chSaved = track.midiChannel.load (std::memory_order_relaxed);
        midiChannelSelector.setSelectedId (chSaved == 0 ? 1 : (1 + chSaved),
                                            juce::dontSendNotification);
    }
    midiChannelSelector.onChange = [this]
    {
        const int id = midiChannelSelector.getSelectedId();
        track.midiChannel.store (id <= 1 ? 0 : (id - 1), std::memory_order_relaxed);
        refreshIoConfigButton();
    };
    styleCombo (midiChannelSelector);
    addChildComponent (midiChannelSelector);

    // ── MIDI OUTPUT selector. Routes this track's per-block MIDI buffer
    // to a hardware synth port in addition to (or instead of) the
    // loaded instrument plugin. Visible in MIDI mode only.
    rebuildMidiOutputDropdown();
    midiOutputSelector.onChange = [this]
    {
        const int id = midiOutputSelector.getSelectedId();
        const int idx = id <= 1 ? -1 : (id - 2);
        track.midiOutputIndex.store (idx, std::memory_order_relaxed);
        if (idx >= 0)
        {
            const auto& outs = engine.getMidiOutputDevices();
            track.midiOutputIdentifier = (idx < outs.size())
                                           ? outs[idx].identifier
                                           : juce::String();
            // Eagerly open the port so the first audio-thread emission
            // doesn't race a synchronous ALSA snd_seq_connect.
            engine.ensureMidiOutputOpen (idx);
        }
        else
        {
            track.midiOutputIdentifier = juce::String();
        }
    };
    styleCombo (midiOutputSelector);
    addChildComponent (midiOutputSelector);

    // MIDI activity LED. Tiny custom paint inside a juce::Component child;
    // intercepts no clicks. Blink state is owned by ChannelStripComponent —
    // timerCallback flips midiActivityLit from track.midiActivity (clear-
    // on-read) and triggers a repaint.
    midiActivityLed.setInterceptsMouseClicks (false, false);
    midiActivityLed.setOpaque (false);
    midiActivityLed.setPaintingIsUnclipped (true);
    addChildComponent (midiActivityLed);

    refreshInputSelectorVisibility();

    // I/O config button. Replaces the inline mode/input/midi rows; opens a
    // modal populated with those widgets when clicked. Saves ~60 px
    // vertical per strip without losing access to any setting.
    ioConfigButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff222226));
    ioConfigButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffa0a8b8));
    ioConfigButton.setTooltip ("Click to configure track type, audio inputs, MIDI port and channel.");
    ioConfigButton.onClick = [this] { openIoConfigPopup(); };
    addAndMakeVisible (ioConfigButton);
    refreshIoConfigButton();

    // ── Aux send knobs (Mixing stage only) ──
    // Phase A: knobs + atomics only - audio routing through the aux buses
    // happens in Phase B. The four send levels feed AUX 1..4's plugin chain
    // (reverb / delay / etc.). Default -inf (no send).
    static const juce::Colour kAuxColours[ChannelStripParams::kNumAuxSends] = {
        juce::Colour (0xff5a8ad0),    // AUX 1 - blue
        juce::Colour (0xff9080c0),    // AUX 2 - violet
        juce::Colour (0xffe0c050),    // AUX 3 - amber
        juce::Colour (0xff60c060),    // AUX 4 - green
    };

    for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
    {
        auto& lbl = auxIndexLabels[(size_t) i];
        // Read the current aux-lane name from session (defaults to "AUX 1"
        // .. "AUX 4" on a fresh session — see Session::Session()).
        lbl.setText (session.auxLane (i).name, juce::dontSendNotification);
        lbl.setJustificationType (juce::Justification::centred);
        lbl.setColour (juce::Label::textColourId, kAuxColours[i].brighter (0.2f));
        lbl.setFont (juce::Font (juce::FontOptions (8.5f, juce::Font::bold)));
        lbl.setMinimumHorizontalScale (0.5f);     // shrink-to-fit on narrow columns
        lbl.setTooltip ("Double-click to rename this AUX send (e.g. \"Reverb\", \"Delay\"). "
                          "Renames the aux lane globally so all channel strips show the same name.");
        lbl.setEditable (false, true, false);
        lbl.setColour (juce::Label::backgroundWhenEditingColourId, juce::Colour (0xff202024));
        lbl.setColour (juce::Label::textWhenEditingColourId,       juce::Colours::white);
        lbl.onTextChange = [this, i]
        {
            auto& lblRef = auxIndexLabels[(size_t) i];
            auto txt = lblRef.getText().trim();
            if (txt.isEmpty())
            {
                // Empty rename — revert to the existing session name.
                lblRef.setText (session.auxLane (i).name, juce::dontSendNotification);
                return;
            }
            constexpr int kAuxNameMaxChars = 12;
            if (txt.length() > kAuxNameMaxChars)
                txt = txt.substring (0, kAuxNameMaxChars);
            session.auxLane (i).name = txt;
            lblRef.setText (txt, juce::dontSendNotification);
        };
        if (usesFaderThresholdLayout())
            addAndMakeVisible (lbl);
        else
            addChildComponent (lbl);
    }

    auto formatAuxSend = [] (float dB, bool preFader)
    {
        // Build the base level string (either the "−" sentinel for a
        // muted send or the dB number), then append " PRE" if the send
        // is pre-fader regardless of level — a user who's parked the
        // knob at off still needs to see whether it's PRE so they know
        // what'll happen when they bump it back up.
        // Tight format: integer when |v| >= 10 ("-12"), else 1 decimal ("0.0").
        // Drops the "dB" suffix so the label fits the narrow column.
        juce::String text;
        if (dB <= ChannelStripParams::kAuxSendMinDb + 0.01f)
            text = juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92"));   // "−" (U+2212)
        else
            text = (std::abs (dB) >= 10.0f)
                       ? juce::String ((int) std::round (dB))
                       : juce::String (dB, 1);
        if (preFader) text += " PRE";
        return text;
    };

    for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
    {
        auto knob = std::make_unique<juce::Slider> (
            juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
        knob->setRange (ChannelStripParams::kAuxSendMinDb, ChannelStripParams::kAuxSendMaxDb, 0.1);
        knob->setSkewFactorFromMidPoint (-12.0);   // detail near unity
        knob->setColour (juce::Slider::rotarySliderFillColourId,    kAuxColours[i]);
        // Pre-fader sends get a bright amber outline as a glanceable
        // indicator that the send taps signal before the fader. Post-
        // fader keeps the dim default outline so a strip with all-post
        // sends looks visually quiet.
        {
            const bool initialPre = track.strip.auxSendPreFader[(size_t) i]
                                          .load (std::memory_order_relaxed);
            knob->setColour (juce::Slider::rotarySliderOutlineColourId,
                              initialPre ? juce::Colour (0xffffc060)
                                         : juce::Colour (0xff404048));
        }
        knob->setDoubleClickReturnValue (true, ChannelStripParams::kAuxSendOffDb);
        knob->setTooltip ("AUX " + juce::String (i + 1) + " send level. "
                          "Right-click for PRE/POST toggle + MIDI Learn.");

        // Map "fully CCW" of the slider onto the kAuxSendOffDb sentinel so a
        // knob at minimum stops the audio path entirely (Phase B will read
        // this). Without the sentinel, even a "minimum-but-not-quite" send
        // would still tap audio, making muting via the knob impossible.
        const float initial = track.strip.auxSendDb[i].load (std::memory_order_relaxed);
        knob->setValue (initial <= ChannelStripParams::kAuxSendMinDb + 0.01f
                          ? ChannelStripParams::kAuxSendOffDb
                          : initial,
                          juce::dontSendNotification);

        auto* knobPtr = knob.get();
        const int idx = i;
        knob->onValueChange = [this, knobPtr, idx, formatAuxSend]
        {
            const float v = (float) knobPtr->getValue();
            const float stored = (v <= ChannelStripParams::kAuxSendMinDb + 0.01f)
                                    ? ChannelStripParams::kAuxSendOffDb : v;
            track.strip.auxSendDb[idx].store (stored, std::memory_order_relaxed);
            const bool preFader = track.strip.auxSendPreFader[(size_t) idx]
                                       .load (std::memory_order_relaxed);
            auxKnobLabels[(size_t) idx].setText (formatAuxSend (stored, preFader),
                                                    juce::dontSendNotification);
        };
        knob->onDragStart = [this, idx]
        {
            track.strip.auxSendTouched[(size_t) idx].store (true, std::memory_order_release);
        };
        knob->onDragEnd = [this, idx]
        {
            track.strip.auxSendTouched[(size_t) idx].store (false, std::memory_order_release);
        };
        // Route right-clicks on the aux knob through the strip's mouseDown
        // so MIDI Learn picks up `e.eventComponent == auxKnobs[i].get()`.
        knob->addMouseListener (this, false);
        if (usesFaderThresholdLayout())
            addAndMakeVisible (knob.get());
        else
            addChildComponent (knob.get());
        auxKnobs[(size_t) i] = std::move (knob);

        auxKnobLabels[(size_t) i].setJustificationType (juce::Justification::centred);
        auxKnobLabels[(size_t) i].setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
        auxKnobLabels[(size_t) i].setColour (juce::Label::textColourId, kAuxColours[i].brighter (0.2f));
        const bool initialPre = track.strip.auxSendPreFader[(size_t) i]
                                     .load (std::memory_order_relaxed);
        auxKnobLabels[(size_t) i].setText (formatAuxSend (initial, initialPre),
                                              juce::dontSendNotification);
        if (usesFaderThresholdLayout())
            addAndMakeVisible (auxKnobLabels[(size_t) i]);
        else
            addChildComponent (auxKnobLabels[(size_t) i]);
    }

    // Insert slot button. Empty state shows "Insert"; plugin loaded
    // shows the plugin name; hardware insert shows "HW: out N-M / in N-M"
    // (or "HW (unrouted)"). refreshPluginSlotButton drives the label.
    pluginSlotButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff222226));
    pluginSlotButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff9080c0));
    pluginSlotButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xffd0c0e0));
    pluginSlotButton.setTooltip (juce::CharPointer_UTF8 (
        "Empty: click to pick a plugin (VST3 / LV2) or an External "
        "Hardware Insert. Loaded plugin: click to toggle the editor; "
        "right-click for Replace / Remove. Hardware insert: click to "
        "open the routing editor."));
    pluginSlotButton.onClick = [this]
    {
        if (pluginSlot.isLoaded()
            || engine.getChannelStrip (trackIndex).isNativeClapLoaded()
            || engine.getChannelStrip (trackIndex).isNativeLv2Loaded()
            || engine.getChannelStrip (trackIndex).isNativeVst3Loaded())
        {
            togglePluginEditor();
            return;
        }
        // Empty slot may already be in Hardware mode (user picked HW
        // earlier, then clicked the slot again to revisit routing).
        // Route straight to the HW editor instead of showing the
        // insert-kind chooser again.
        const auto mode = engine.getChannelStrip (trackIndex)
                                  .insertMode.load (std::memory_order_acquire);
        if (mode == ChannelStrip::kInsertHardware)
            openHardwareInsertEditor();
        else
            openPluginPicker();
    };
    pluginSlotButton.onRightClick = [this] (const juce::MouseEvent&)
    {
        showPluginSlotMenu();
    };
    addAndMakeVisible (pluginSlotButton);

    // Compact-mode placeholder buttons. Hidden by default; setCompactMode(true)
    // makes them visible and hides the full inline EQ + COMP + AUX controls.
    styleCompactSectionButton (eqCompactButton,   juce::Colour (fourKColors::kLfGreen));
    styleCompactSectionButton (compCompactButton, juce::Colour (fourKColors::kCompGold));
    styleCompactSectionButton (auxCompactButton,  juce::Colour (0xff9080c0));   // SEND violet
    eqCompactButton  .setButtonText ("EQ");
    compCompactButton.setButtonText ("COMP");
    auxCompactButton .setButtonText ("AUX");
    // Same grammar as the expanded EQ/COMP headers: left-click toggles the section
    // on/off, right-click opens the editor. The illuminated background (driven by the
    // 30 Hz refresh) reflects the enabled state.
    eqCompactButton  .setTooltip ("Left-click EQ on/off. Right-click to open the editor.");
    compCompactButton.setTooltip ("Left-click comp on/off. Right-click to open the editor.");
    auxCompactButton .setTooltip ("Click to open the AUX sends panel (click again to close).");
    eqCompactButton  .onClick = [this]
    {
        track.strip.eqEnabled.store (! track.strip.eqEnabled.load (std::memory_order_relaxed),
                                      std::memory_order_release);
    };
    eqCompactButton  .onRightClick = [this] (const juce::MouseEvent&) { openEqEditorPopup(); };
    compCompactButton.onClick      = [this] { setCompEnabled (! track.strip.compEnabled.load (std::memory_order_relaxed)); };
    compCompactButton.onRightClick = [this] (const juce::MouseEvent&) { openCompEditorPopup(); };
    auxCompactButton .onClick = [this] { openAuxEditorPopup(); };
    addChildComponent (eqCompactButton);    // hidden until compact mode flips on
    addChildComponent (compCompactButton);
    addChildComponent (auxCompactButton);

    // Accessibility floor (deeper a11y pass — names every user-driven
    // control on the strip so screen readers announce intent, not just
    // role+index). Track-relative names because each strip's controls
    // are otherwise indistinguishable to the AT.
    const auto tn = juce::String (trackIndex + 1);
    faderSlider .setTitle ("Track " + tn + " fader");
    panKnob     .setTitle ("Track " + tn + " pan");
    muteButton  .setTitle ("Track " + tn + " mute");
    soloButton  .setTitle ("Track " + tn + " solo");
    armButton   .setTitle ("Track " + tn + " record arm");
    monitorButton.setTitle ("Track " + tn + " input monitor");
    printButton .setTitle ("Track " + tn + " print effects on record");
    hpfKnob     .setTitle ("Track " + tn + " high-pass filter frequency");
    lpfKnob     .setTitle ("Track " + tn + " low-pass filter frequency");
    if (eqHeaderBtn != nullptr) eqHeaderBtn->setTitle ("Track " + tn + " EQ enable / type");
    pluginSlotButton.setTitle ("Track " + tn + " insert slot");
    for (size_t i = 0; i < auxKnobs.size(); ++i)
        if (auxKnobs[i] != nullptr)
            auxKnobs[i]->setTitle ("Track " + tn + " aux " + juce::String ((int) i + 1) + " send");
}

ChannelStripComponent::~ChannelStripComponent()
{
    stopTimer();   // before derived members destruct (base Timer::~Timer is too late)
    engine.removeChangeListener (this);

    // If a popup editor is still open when the strip dies, destroy its body
    // NOW (synchronously) rather than via close()'s deferred callAsync: on the
    // app-quit path the message loop has already exited, so a deferred body
    // teardown never runs (leak) or fires during MessageManager shutdown after
    // members are gone. The I/O popup additionally re-parents this strip's live
    // modeSelector / midiActivityLed children, so its body must be gone while
    // those members are still alive — closeAndDeleteBodyNow() runs here, before
    // any member destructs. Same variant MainComponent uses for shutdown.
    eqEditorModal.closeAndDeleteBodyNow();
    compEditorModal.closeAndDeleteBodyNow();
    auxEditorModal.closeAndDeleteBodyNow();
    ioConfigModal.closeAndDeleteBodyNow();
    // Drop the cached editor BEFORE the strip's PluginSlot destructs,
    // since the editor's destructor calls editorBeingDeleted on its
    // owning AudioProcessor. dropPluginEditor() also closes the modal.
    dropPluginEditor();
}

void ChannelStripComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Engine signalled a MIDI device-list refresh (post-rebuild). The
    // device order may have changed; re-resolve our track's index from
    // its saved identifier and rebuild both dropdowns (input + output).
    rebuildMidiInputDropdown();
    rebuildMidiOutputDropdown();
}

void ChannelStripComponent::rebuildMidiInputDropdown()
{
    midiInputSelector.clear (juce::dontSendNotification);
    midiInputSelector.addItem ("(none)", 1);
    const auto& inputs = engine.getMidiInputDevices();
    for (int i = 0; i < inputs.size(); ++i)
        midiInputSelector.addItem (inputs[i].name, 2 + i);

    // Prefer identifier-based selection when we have one - it survives
    // device-list reordering. Fall back to the raw index for older
    // sessions / never-touched tracks.
    int idx = -1;
    if (track.midiInputIdentifier.isNotEmpty())
    {
        for (int i = 0; i < inputs.size(); ++i)
        {
            if (inputs[i].identifier == track.midiInputIdentifier)
            {
                idx = i; break;
            }
        }
    }
    else
    {
        idx = track.midiInputIndex.load (std::memory_order_relaxed);
        if (idx >= inputs.size()) idx = -1;
    }
    track.midiInputIndex.store (idx, std::memory_order_relaxed);
    midiInputSelector.setSelectedId (idx >= 0 ? (2 + idx) : 1,
                                      juce::dontSendNotification);
}

void ChannelStripComponent::rebuildMidiOutputDropdown()
{
    // Mirror of rebuildMidiInputDropdown for the output side. ID 1 =
    // "(none)" (= idx -1, no external output); 2..N = device index.
    midiOutputSelector.clear (juce::dontSendNotification);
    midiOutputSelector.addItem ("(none)", 1);
    const auto& outs = engine.getMidiOutputDevices();
    for (int i = 0; i < outs.size(); ++i)
        midiOutputSelector.addItem (outs[i].name, 2 + i);

    int idx = -1;
    if (track.midiOutputIdentifier.isNotEmpty())
    {
        for (int i = 0; i < outs.size(); ++i)
        {
            if (outs[i].identifier == track.midiOutputIdentifier)
            {
                idx = i; break;
            }
        }
    }
    else
    {
        idx = track.midiOutputIndex.load (std::memory_order_relaxed);
        if (idx >= outs.size()) idx = -1;
    }
    track.midiOutputIndex.store (idx, std::memory_order_relaxed);
    midiOutputSelector.setSelectedId (idx >= 0 ? (2 + idx) : 1,
                                        juce::dontSendNotification);
}

void ChannelStripComponent::setEqSectionVisible (bool visible)
{
    hpfKnob.setVisible (visible);
    hpfLabel.setVisible (visible);
    lpfKnob.setVisible (visible);
    lpfLabel.setVisible (visible);
    eqTypeChip.setVisible (visible);
    if (eqHeaderBtn != nullptr) eqHeaderBtn->setVisible (visible);
    for (auto& row : eqRows)
    {
        if (row.gain) row.gain->setVisible (visible);
        if (row.freq) row.freq->setVisible (visible);
        if (row.q)    row.q   ->setVisible (visible);
        row.labelLeft .setVisible (visible);
        row.labelRight.setVisible (visible);
        row.rowLabel  .setVisible (visible);
        row.qLabel    .setVisible (visible);
    }
}

void ChannelStripComponent::setCompSectionVisible (bool visible)
{
    if (compModeButton != nullptr) compModeButton->setVisible (visible);
    optoPeakRedKnob .setVisible (visible);  optoPeakRedLabel.setVisible (visible);
    optoGainKnob    .setVisible (visible);  optoGainLabel   .setVisible (visible);
    optoLimitButton .setVisible (visible);
    fetInputKnob    .setVisible (visible);  fetInputLabel   .setVisible (visible);
    fetOutputKnob   .setVisible (visible);  fetOutputLabel  .setVisible (visible);
    fetAttackKnob   .setVisible (visible);  fetAttackLabel  .setVisible (visible);
    fetReleaseKnob  .setVisible (visible);  fetReleaseLabel .setVisible (visible);
    fetRatioKnob    .setVisible (visible);  fetRatioLabel   .setVisible (visible);
    vcaRatioKnob    .setVisible (visible);  vcaRatioLabel   .setVisible (visible);
    vcaAttackKnob   .setVisible (visible);  vcaAttackLabel  .setVisible (visible);
    vcaReleaseKnob  .setVisible (visible);  vcaReleaseLabel .setVisible (visible);
    vcaOutputKnob   .setVisible (visible);  vcaOutputLabel  .setVisible (visible);
    // compMeter now lives INSIDE the COMP section (next to the per-mode
    // knobs) so it follows section visibility. In compact mode the popup
    // owns its own threshold drag so we don't lose access.
    if (compMeter != nullptr) compMeter->setVisible (visible);
    threshMeterLabel.setVisible (false);   // legacy "THR" header, no longer used
    grPeakLabel    .setVisible (false);    // numeric GR readout retired - the
                                            // meter bar already shows GR clearly

    // Re-apply the per-mode filter so only the active mode's knobs are
    // shown. Without this, flipping out of TIMELINE (or any path that
    // re-shows the section) leaves all 13 per-mode knobs visible at the
    // same physical positions and they overlap.
    if (visible)
    {
        refreshCompModeButtonState();
        refreshCompKnobVisibility();
    }
}

void ChannelStripComponent::setAuxSectionVisible (bool visible)
{
    for (auto& l : auxIndexLabels)  l.setVisible (visible);
    for (auto& k : auxKnobs)        if (k != nullptr) k->setVisible (visible);
    for (auto& l : auxKnobLabels)   l.setVisible (visible);
}

void ChannelStripComponent::setCompactMode (bool compact)
{
    if (compactMode == compact) return;
    compactMode = compact;

    setEqSectionVisible   (! compact);
    setCompSectionVisible (! compact);
    eqCompactButton  .setVisible (compact);
    compCompactButton.setVisible (compact);

    // AUX row only exists when mixingMode is on OR this is track-3's always-
    // visible fader-side layout. In compact mode the inline knobs collapse
    // to a single AUX button regardless of which path put them on screen.
    const bool auxRowAvailable = mixingMode || usesFaderThresholdLayout();
    if (compact && auxRowAvailable)
    {
        setAuxSectionVisible (false);
        auxCompactButton.setVisible (true);
    }
    else
    {
        auxCompactButton.setVisible (false);
        setAuxSectionVisible (auxRowAvailable);
    }

    resized();
    repaint();
}

void ChannelStripComponent::setMixingMode (bool mixing)
{
    if (mixingMode == mixing) return;
    mixingMode = mixing;

    // The mode/input/IN/ARM/PRINT block is hidden in Mixing - those controls
    // are tracking-stage only. The aux send knobs take that real estate.
    // ioConfigButton must hide too: resized() only re-lays-out the I/O region
    // when !mixingMode, so left visible it keeps its stale recording-mode
    // bounds and bleeds out behind the Insert button (still clickable).
    ioConfigButton     .setVisible (! mixing);
    modeSelector       .setVisible (! mixing);
    monitorButton      .setVisible (! mixing);
    armButton          .setVisible (! mixing);
    printButton        .setVisible (! mixing);
    // The 6 input / MIDI selectors are also mode-dependent (a mono track has
    // no R-input selector, an audio track no MIDI selectors). Don't force them
    // all visible here - refreshInputSelectorVisibility() is the single source
    // of truth, gating on BOTH mixingMode and track mode, so it's correct
    // entering AND leaving Mixing. (A plain setVisible(!mixing) here left
    // phantom rows when leaving Mixing onto a mono / MIDI strip.)
    refreshInputSelectorVisibility();

    const bool showAux = mixing || usesFaderThresholdLayout();
    if (showAux && compactMode)
    {
        setAuxSectionVisible (false);
        auxCompactButton.setVisible (true);
    }
    else
    {
        setAuxSectionVisible (showAux);
        auxCompactButton.setVisible (false);
    }

    resized();
    repaint();
}

// Click semantics for the compact-mode placeholder buttons:
//   • Click while no popup is open → open this section's popup
//   • Click while THIS popup is already open → close it (toggle off)
//   • Click while the OTHER popup is open → close the other, open this one
// The mutual-exclusion path lets the user move quickly between EQ and COMP
// without first having to dismiss the open dialog manually.
void ChannelStripComponent::openPluginPicker (bool useChooser)
{
    // MIDI tracks need an instrument plugin (synth / sampler) - the strip
    // routes per-track MIDI events into the slot. Mono / Stereo tracks
    // route audio through the slot, which only makes sense for effect
    // plugins; instrument plugins ignore the audio input entirely and
    // (in the case of some VSTGUI-based instruments) fail to render
    // their UI when loaded as an audio insert.
    const auto kind = (track.mode.load (std::memory_order_relaxed) == (int) Track::Mode::Midi)
                        ? pluginpicker::PluginKind::Instruments
                        : pluginpicker::PluginKind::Effects;

    juce::Component::SafePointer<ChannelStripComponent> safe (this);
    auto openHwEditor = [safe]
    {
        if (auto* self = safe.getComponent())
            self->openHardwareInsertEditor();
    };

    // Two-step flow: chooser modal first (Hardware / Soundfont / Plugin)
    // so the plugin list isn't the user's only path. Picking "Plugin"
    // from the chooser routes to the existing openPickerMenu — same
    // callbacks below. The right-click "Replace plugin..." path passes
    // useChooser=false: the user is already committed to a plugin, so
    // skip the chooser and jump straight to the plugin list.
    auto onChange = [safe]
                                    {
                                        auto* self = safe.getComponent();
                                        if (self == nullptr) return;
                                        // Picking a plugin flips the strip back to Plugin mode
                                        // (overriding any prior Hardware selection on this slot).
                                        auto& chStrip = self->engine.getChannelStrip (self->trackIndex);
                                        chStrip.insertMode.store (ChannelStrip::kInsertPlugin,
                                                                  std::memory_order_release);

                                        // One host per insert: a successful JUCE load evicts any
                                        // native slot — the audio chain checks natives first, so a
                                        // lingering native would silently shadow the picked plugin.
                                        if (self->pluginSlot.isLoaded()
                                            && (chStrip.isNativeClapLoaded() || chStrip.isNativeLv2Loaded()
                                                || chStrip.isNativeVst3Loaded()))
                                        {
   #if DUSKSTUDIO_HAS_NATIVE_CLAP
                                            self->clapEditor.reset();   // references the dying instance
   #endif
   #if DUSKSTUDIO_HAS_NATIVE_LV2
                                            self->lv2Editor.reset();
   #endif
   #if DUSKSTUDIO_HAS_NATIVE_VST3
                                            self->vst3Editor.reset();
   #endif
                                            self->engine.suspendProcessing();
                                            chStrip.unloadNativeClap();
                                            chStrip.unloadNativeLv2();
                                            chStrip.unloadNativeVst3();
                                            self->engine.resumeProcessing();
                                            self->track.nativeClapPath = {};
                                            self->track.nativeClapStateBase64 = {};
                                            self->track.nativeLv2Path = {};
                                            self->track.nativeLv2StateBase64 = {};
                                            self->track.nativeVst3Path = {};
                                            self->track.nativeVst3StateBase64 = {};
                                        }

                                        // Loading an instrument (soundfont or VST/LV2 synth) on a
                                        // non-MIDI track leaves the strip silent — instrument
                                        // ignores audio input, no MIDI source feeds it. Flip the
                                        // track to MIDI automatically so the user doesn't have to.
                                        // setSelectedId on the mode selector fires its own change
                                        // handler which publishes the new mode atomically and
                                        // refreshes I/O / slot button visibility consistently.
                                        if (self->pluginSlot.isLoadedPluginInstrument())
                                        {
                                            if (self->track.mode.load (std::memory_order_relaxed)
                                                  != (int) Track::Mode::Midi)
                                                self->modeSelector.setSelectedId (
                                                    (int) Track::Mode::Midi + 1,
                                                    juce::sendNotificationSync);

                                            // No MIDI input bound yet? Route the Virtual
                                            // Keyboard so the user can immediately audition
                                            // the freshly loaded instrument without hunting
                                            // through the input dropdown. ID convention
                                            // (see rebuildMidiInputDropdown): 1 = (none),
                                            // 2 + deviceIndex = real input N.
                                            if (self->track.midiInputIndex.load (
                                                    std::memory_order_relaxed) < 0)
                                            {
                                                const int vkbIdx =
                                                    self->engine.getVirtualKeyboardInputIndex();
                                                if (vkbIdx >= 0)
                                                    self->midiInputSelector.setSelectedId (
                                                        2 + vkbIdx, juce::sendNotificationSync);
                                            }
                                        }

                                        // Close the modal synchronously so the cached editor
                                        // (still bound to the just-replaced processor) gets
                                        // detached from the parent component before the next
                                        // paint cycle. The cached editor unique_ptr is dropped
                                        // by refreshPluginSlotButton when it sees the slot's
                                        // processor pointer change.
                                        //
                                        // Defer that drop + the slot-button refresh by one
                                        // message-loop tick so it doesn't run inside the
                                        // picker's own dismissal stack. Plugin destructors on
                                        // Linux frequently do X11 / OpenGL cleanup that
                                        // confuses Mutter when fired during another widget's
                                        // teardown - this single-tick gap is what stops the
                                        // Replace plugin... action from crashing the DAW (or
                                        // the entire compositor).
                                        //
                                        // Auto-open the new plugin's editor so a pick / replace
                                        // immediately surfaces the GUI. Sequence each step on its
                                        // own message-loop tick: close (now) → refresh (next
                                        // tick) → open (the tick after that). The two-tick gap
                                        // separates the old editor's destruction from the new
                                        // editor's construction so plugins with fragile lifecycle
                                        // teardown (Diva, MininnDrum) don't see destroy+create
                                        // collapse into a single frame.
                                        self->closePluginEditor();
                                        juce::Component::SafePointer<ChannelStripComponent> deferred (self);
                                        juce::MessageManager::callAsync ([deferred]
                                        {
                                            auto* s = deferred.getComponent();
                                            if (s == nullptr) return;
                                            s->refreshPluginSlotButton();
                                            juce::Component::SafePointer<ChannelStripComponent> openLater (s);
                                            juce::MessageManager::callAsync ([openLater]
                                            {
                                                auto* ss = openLater.getComponent();
                                                if (ss == nullptr) return;
                                                if (ss->pluginSlot.isLoaded() && ! ss->isPluginEditorOpen())
                                                    ss->openPluginEditor();
                                            });
                                        });
                                    };

    // Native CLAP route — effects (audio) slots only; CLAP instruments aren't hosted
    // natively yet. Selecting a CLAP row loads it through the channel's native host.
    // Linux-only; elsewhere the callback stays empty so the picker shows no CLAP rows.
    std::function<void (const juce::File&)> onClap;
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    if (kind == pluginpicker::PluginKind::Effects)
        onClap = [safe] (const juce::File& f)
        {
            if (auto* self = safe.getComponent())
                self->loadNativeClapForChannel (f);
        };
#endif
    // Native LV2 route — same shape; LV2-Native rows replace the JUCE LV2 rows.
    std::function<void (const juce::File&)> onLv2;
#if DUSKSTUDIO_HAS_NATIVE_LV2
    if (kind == pluginpicker::PluginKind::Effects)
        onLv2 = [safe] (const juce::File& f)
        {
            if (auto* self = safe.getComponent())
                self->loadNativeLv2ForChannel (f);
        };
#endif
    // Native VST3 route — same shape; VST3-Native rows replace the JUCE VST3 rows.
    std::function<void (const juce::File&)> onVst3;
#if DUSKSTUDIO_HAS_NATIVE_VST3
    if (kind == pluginpicker::PluginKind::Effects)
        onVst3 = [safe] (const juce::File& f)
        {
            if (auto* self = safe.getComponent())
                self->loadNativeVst3ForChannel (f);
        };
#endif

    if (useChooser)
    {
        pluginpicker::openInsertChooser (pluginSlot,
                                          pluginSlotButton,
                                          activePluginChooser,
                                          std::move (onChange),
                                          kind,
                                          std::move (openHwEditor),
                                          std::move (onClap),
                                          std::move (onLv2),
                                          std::move (onVst3));
    }
    else
    {
        // Replace-plugin path: skip the chooser, suppress the picker's
        // own secondary buttons (HW + Soundfont) since the user has
        // already committed to "this is a plugin".
        pluginpicker::openPickerMenu (pluginSlot,
                                       pluginSlotButton,
                                       activePluginChooser,
                                       std::move (onChange),
                                       kind,
                                       { -1, -1 },
                                       /*onPickHardwareInsert*/ {},
                                       /*suppressSecondaryButtons*/ true,
                                       std::move (onClap),
                                       std::move (onLv2),
                                       std::move (onVst3));
    }
}

void ChannelStripComponent::openHardwareInsertEditor()
{
    // Flip the strip to Hardware mode FIRST so the audio thread's
    // crossfade gate (Phase 3) starts ramping in even before the user
    // touches a control. The editor itself mutates `track.hardwareInsert`
    // via AtomicSnapshot::publish + scalar atomic stores.
    engine.getChannelStrip (trackIndex)
        .insertMode.store (ChannelStrip::kInsertHardware,
                            std::memory_order_release);
    refreshPluginSlotButton();

    juce::Component::SafePointer<ChannelStripComponent> safe (this);
    auto editor = std::make_unique<HardwareInsertEditor> (
        track.hardwareInsert,
        engine.getDeviceManager(),
        [safe]
        {
            if (auto* self = safe.getComponent())
            {
                self->hardwareInsertModal.close();
                self->refreshPluginSlotButton();
            }
        });

    auto* parent = findParentComponentOfClass<juce::Component>();
    if (parent == nullptr) parent = this;
    hardwareInsertModal.show (*parent, std::move (editor));
}

void ChannelStripComponent::unloadPluginSlot()
{
    // Close any open editor BEFORE the plugin is destroyed - the editor
    // holds a reference back to the AudioProcessor and would crash on
    // teardown otherwise.
    closePluginEditor();

    // Native CLAP insert: drop our editor + tear down the slot under the engine
    // gate, then return the strip to the empty-insert resting state. Linux-only.
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    auto& strip = engine.getChannelStrip (trackIndex);
    if (strip.isNativeClapLoaded())
    {
        clapEditor.reset();
        engine.suspendProcessing();
        strip.unloadNativeClap();
        strip.insertMode.store (ChannelStrip::kInsertPlugin, std::memory_order_release);
        engine.resumeProcessing();
        track.nativeClapPath = {};
        track.nativeClapStateBase64 = {};
        refreshPluginSlotButton();
        return;
    }
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
    {
        auto& lv2Strip = engine.getChannelStrip (trackIndex);
        if (lv2Strip.isNativeLv2Loaded())
        {
            lv2Editor.reset();   // references the dying instance
            engine.suspendProcessing();
            lv2Strip.unloadNativeLv2();
            lv2Strip.insertMode.store (ChannelStrip::kInsertPlugin, std::memory_order_release);
            engine.resumeProcessing();
            track.nativeLv2Path = {};
            track.nativeLv2StateBase64 = {};
            refreshPluginSlotButton();
            return;
        }
    }
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
    {
        auto& vst3Strip = engine.getChannelStrip (trackIndex);
        if (vst3Strip.isNativeVst3Loaded())
        {
            vst3Editor.reset();   // references the dying instance
            engine.suspendProcessing();
            vst3Strip.unloadNativeVst3();
            vst3Strip.insertMode.store (ChannelStrip::kInsertPlugin, std::memory_order_release);
            engine.resumeProcessing();
            track.nativeVst3Path = {};
            track.nativeVst3StateBase64 = {};
            refreshPluginSlotButton();
            return;
        }
    }
#endif

    pluginSlot.unload();
    refreshPluginSlotButton();
}

void ChannelStripComponent::showPluginSlotMenu()
{
    juce::PopupMenu menu;
    const bool nativeLoaded = engine.getChannelStrip (trackIndex).isNativeClapLoaded()
                           || engine.getChannelStrip (trackIndex).isNativeLv2Loaded()
                           || engine.getChannelStrip (trackIndex).isNativeVst3Loaded();
    if (pluginSlot.isLoaded() || nativeLoaded)
    {
        // Editor toggle headline so right-click ALSO becomes a way to open
        // the plugin GUI (some users find right-click more discoverable).
        const bool editorOpen = isPluginEditorOpen();
        menu.addItem (2001, editorOpen ? "Close editor" : "Open editor");
        menu.addSeparator();
        menu.addItem (2002, "Replace plugin...");
        menu.addItem (2003, "Remove plugin");
        // Crash/auto-bypass recovery is a JUCE-slot concept (native hosts run
        // in-process without the watchdog).
        if (! nativeLoaded)
        {
            if (pluginSlot.wasCrashed())
                menu.addItem (2004, "Re-enable plugin (crashed)");
            else if (pluginSlot.wasAutoBypassed())
                menu.addItem (2004, "Re-enable plugin (auto-bypassed)");
        }
        // MIDI Learn for the last parameter the user touched via the plugin's
        // own UI. Disabled when no parameter has been touched since the slot
        // loaded (no last-touched stamp to bind to). Native slots track
        // touches from their editors.
        {
            auto& chStrip = engine.getChannelStrip (trackIndex);
            int lastParam = -1;
#if DUSKSTUDIO_HAS_NATIVE_CLAP
            if (chStrip.isNativeClapLoaded())
                lastParam = chStrip.getNativeClapSlot().lastTouchedParamIndex();
            else
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
            if (chStrip.isNativeLv2Loaded())
                lastParam = chStrip.getNativeLv2Slot().lastTouchedParamIndex();
            else
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
            if (chStrip.isNativeVst3Loaded())
                lastParam = chStrip.getNativeVst3Slot().lastTouchedParamIndex();
            else
#endif
            if (! nativeLoaded)
                lastParam = pluginSlot.getLastTouchedParamIndex();
            menu.addSeparator();
            menu.addItem (2005,
                           "MIDI Learn last-touched parameter",
                           lastParam >= 0);
        }
    }
    else
    {
        menu.addItem (2010, "Pick plugin...");
    }

    juce::Component::SafePointer<ChannelStripComponent> safe (this);
    showContextMenu (menu, pluginSlotButton,
        [safe] (int result)
        {
            auto* self = safe.getComponent();
            if (self == nullptr || result <= 0) return;
            switch (result)
            {
                case 2001: self->togglePluginEditor();             break;
                case 2002: self->openPluginPicker (/*useChooser*/ false); break;
                case 2003: self->unloadPluginSlot();               break;
                case 2004: self->pluginSlot.clearAutoBypass();     break;
                case 2005:
                    // Fire the MIDI Learn workflow targeting THIS
                    // track's plugin slot. The pending state stores
                    // only the track; the resolve site (TransportBar's
                    // timer) reads the slot's last-touched param at
                    // the moment a MIDI source arrives so the param
                    // index reflects what the user moved between
                    // clicking Learn and triggering the controller.
                    midilearn::showLearnMenu (
                        self->pluginSlotButton, self->session,
                        MidiBindingTarget::TrackPluginParam,
                        self->trackIndex);
                    break;
                case 2010: self->openPluginPicker (/*useChooser*/ false); break;
                default: break;
            }
        });
}

void ChannelStripComponent::refreshInsertButtonForCapture()
{
    refreshPluginSlotButton();
    repaint();
}

void ChannelStripComponent::refreshPluginSlotButton()
{
    // Native CLAP insert — name from the loaded .clap bundle. The JUCE pluginSlot is
    // empty for a native insert, so this must precede the slot-based bookkeeping below
    // (which would otherwise mislabel "Insert" and drop the live clap editor). Linux-only.
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    if (engine.getChannelStrip (trackIndex).isNativeClapLoaded())
    {
        const auto nm = juce::File (engine.getChannelStrip (trackIndex)
                                      .getNativeClapSlot().getPath())
                          .getFileNameWithoutExtension();
        const auto label = juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xbe ")) + nm;
        if (label == lastSlotName) return;
        lastSlotName = label;
        pluginSlotButton.setButtonText (label);
        return;
    }
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
    if (engine.getChannelStrip (trackIndex).isNativeLv2Loaded())
    {
        // Bundle directory name minus the ".lv2" suffix.
        const auto nm = juce::File (engine.getChannelStrip (trackIndex)
                                      .getNativeLv2Slot().getPath())
                          .getFileNameWithoutExtension();
        const auto label = juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xbe ")) + nm;
        if (label == lastSlotName) return;
        lastSlotName = label;
        pluginSlotButton.setButtonText (label);
        return;
    }
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
    if (engine.getChannelStrip (trackIndex).isNativeVst3Loaded())
    {
        const auto nm = juce::File (engine.getChannelStrip (trackIndex)
                                      .getNativeVst3Slot().getPath())
                          .getFileNameWithoutExtension();
        const auto label = juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xbe ")) + nm;
        if (label == lastSlotName) return;
        lastSlotName = label;
        pluginSlotButton.setButtonText (label);
        return;
    }
#endif

    const int mode = engine.getChannelStrip (trackIndex)
                       .insertMode.load (std::memory_order_relaxed);
    juce::String label;
    if (mode == ChannelStrip::kInsertHardware)
    {
        // Loaded hardware: show the routed channel pair so the user knows
        // at a glance where the strip is patched. Each side (out / in) is
        // formatted independently - mono when only L is assigned, stereo
        // when both are - and the label falls back to "HW (unrouted)" when
        // neither side has audio routing.
        const auto routing = track.hardwareInsert.routing.current();
        auto formatPair = [] (int l, int r) -> juce::String
        {
            if (l < 0 && r < 0) return {};
            if (r < 0)          return juce::String (l + 1);                       // mono
            if (l < 0)          return juce::String (r + 1);                       // mono on R only
            if (l == r)         return juce::String (l + 1);                       // same channel both
            return juce::String (l + 1) + "-" + juce::String (r + 1);              // stereo pair
        };
        const auto out = formatPair (routing.outputChL, routing.outputChR);
        const auto in  = formatPair (routing.inputChL,  routing.inputChR);
        if (out.isEmpty() && in.isEmpty())
            label = "HW (unrouted)";
        else
            label = juce::String ("HW: out ")
                  + (out.isNotEmpty() ? out : juce::String ("-"))
                  + " / in "
                  + (in .isNotEmpty() ? in  : juce::String ("-"));
    }
    else
    {
        const auto name = pluginSlot.getLoadedName();
        if (name.isNotEmpty())
            label = juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xbe ")) + name;
        else if (pluginSlot.isOffline())
        {
            // Slot held a plugin in the saved session but couldn't be
            // re-instantiated (plugin missing / moved / format unsupported).
            // Show the saved name with a warning marker so the user knows
            // the slot's saved state is preserved on disk and that
            // reinstalling the plugin will restore it.
            const auto offline = pluginSlot.getOfflineName();
            label = juce::String (juce::CharPointer_UTF8 ("\xe2\x9a\xa0 "))
                  + (offline.isNotEmpty() ? offline : juce::String ("offline"))
                  + " (offline)";
        }
        else
            label = "Insert";
    }

    if (label == lastSlotName) return;
    lastSlotName = label;
    pluginSlotButton.setButtonText (label);

    // Re-fetch the plugin name for the dropPluginEditor / cached-editor
    // bookkeeping below (kept on the same atomic field the old code used).
    const auto name = pluginSlot.getLoadedName();

    // If the plugin was unloaded out from under an open editor (e.g. via
    // the right-click menu's Remove), the cached editor references a
    // now-being-destructed AudioProcessor. Drop it before that processor
    // disappears so we don't leave a dangling pointer in pluginEditor.
    if (name.isEmpty())
        dropPluginEditor();
    else if (pluginEditor != nullptr
             && pluginEditorOwner != pluginSlot.getInstance())
    {
        // Plugin was Replaced - the cached editor belongs to the prior
        // instance. Drop it so the next Open Editor builds a fresh one
        // for the new instance.
        dropPluginEditor();
    }
}

void ChannelStripComponent::togglePluginEditor()
{
    // Toggle: if the editor is up, close it; otherwise open. Same shape as
    // the EQ / COMP popup buttons in compact mode.
    if (isPluginEditorOpen())
        closePluginEditor();
    else
        openPluginEditor();
}


bool ChannelStripComponent::isPluginEditorOpen() const noexcept
{
    return pluginEditorModal.isOpen();
}

void ChannelStripComponent::openPluginEditor()
{
    if (isPluginEditorOpen()) return;

    juce::Component::SafePointer<ChannelStripComponent> safe (this);
    auto* parent = getTopLevelComponent();
    if (parent == nullptr) parent = this;

    auto onClose = [safe]
    {
        if (auto* self = safe.getComponent())
            self->closePluginEditor();
    };

    // Native CLAP insert — show our editor (shared instance) borrowed in the modal,
    // so closing hides (not destroys) it. Takes precedence over the JUCE editor. Linux-only.
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    if (engine.getChannelStrip (trackIndex).isNativeClapLoaded())
    {
        auto* inst = engine.getChannelStrip (trackIndex).getNativeClapSlot().getInstance();
        if (inst == nullptr) return;
        if (clapEditor == nullptr)
        {
            clapEditor = std::make_unique<ClapPluginEditorComponent>();
            juce::String err;
            if (! clapEditor->attach (*inst, err)) { clapEditor.reset(); return; }
        }
        pluginEditorModal.showBorrowed (*parent, *clapEditor, onClose);
        return;
    }
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
    if (engine.getChannelStrip (trackIndex).isNativeLv2Loaded())
    {
        auto* inst = engine.getChannelStrip (trackIndex).getNativeLv2Slot().getInstance();
        if (inst == nullptr) return;
        if (lv2Editor == nullptr)
        {
            lv2Editor = std::make_unique<Lv2PluginEditorComponent>();
            juce::String err;
            if (! lv2Editor->attach (*inst, err))
            {
                std::fprintf (stderr, "[chan lv2] %s\n", err.toRawUTF8());
                lv2Editor.reset();
                return;
            }
        }
        pluginEditorModal.showBorrowed (*parent, *lv2Editor, onClose);
        return;
    }
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
    if (engine.getChannelStrip (trackIndex).isNativeVst3Loaded())
    {
        auto* inst = engine.getChannelStrip (trackIndex).getNativeVst3Slot().getInstance();
        if (inst == nullptr) return;
        if (vst3Editor == nullptr)
        {
            vst3Editor = std::make_unique<Vst3PluginEditorComponent>();
            juce::String err;
            if (! vst3Editor->attach (*inst, err))
            {
                std::fprintf (stderr, "[chan vst3] %s\n", err.toRawUTF8());
                vst3Editor.reset();
                return;
            }
        }
        pluginEditorModal.showBorrowed (*parent, *vst3Editor, onClose);
        return;
    }
#endif

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    if (pluginSlot.isRemote())
    {
        // OOP path: ask the child to show the editor + report its
        // native window handle. The child owns the editor's lifecycle;
        // we either embed the handle (Linux XEmbed) or let it float
        // as its own top-level window (Mac — cross-process NSView
        // embedding is a polish item, not a 1.0 blocker).
        std::uint64_t windowId = 0;
        int w = 0, h = 0;
        if (! pluginSlot.showRemoteEditor (windowId, w, h)) return;
        if (windowId == 0) return;

       #if JUCE_LINUX
        if (remoteEditorEmbed == nullptr)
        {
            // XEmbedComponent ctor: takes a Window (unsigned long) and
            // adopts it as the foreign client.
            // wantsKeyboardFocus = true so plugin GUIs receive key events.
            remoteEditorEmbed = std::make_unique<juce::XEmbedComponent> (
                (unsigned long) windowId,
                /*wantsKeyboardFocus*/ true,
                /*allowForeignWidgetToResizeComponent*/ false);
            remoteEditorEmbed->setSize (juce::jmax (200, w),
                                          juce::jmax (200, h));
            // Tag so EmbeddedModal hides this XEmbed window for the
            // lifetime of any subsequent settings / quit modal — native
            // X11 z-order otherwise paints the plugin window above the
            // JUCE-rendered modal.
            remoteEditorEmbed->getProperties().set (kPluginEditorTag, true);
        }
        pluginEditorModal.showBorrowed (*parent, *remoteEditorEmbed, onClose);
       #elif JUCE_MAC
        // Mac OOP: dual-load shell pattern (3c-2). DSP stays in the
        // child; the editor lives in this process via PluginSlot's
        // shell instance. windowId from showRemoteEditor is unused on
        // Mac because the child's native window is NOT what we embed;
        // the parent's shell-instance editor is. The IPC ShowEditor
        // call still ran above for symmetry + so future param-mirror
        // (3c-3) state-sync IPC can piggyback on it.
        juce::ignoreUnused (windowId);
        std::unique_ptr<juce::Component> embed;
        juce::String shellErr;
        if (pluginSlot.ensureShellInstanceForEditor (shellErr))
        {
            // First-open state sync (3c-3b). Pulls the child's live
            // AudioProcessor state and applies it to the shell BEFORE
            // we hand the editor off to the modal. Without this the
            // shell editor would show its default state, not the
            // running DSP's state — confusing UX after a session
            // reload + immediate editor open. Failure logs + continues;
            // the user can still drive the mirror by moving knobs.
            juce::String syncErr;
            if (! pluginSlot.syncShellStateFromChild (syncErr))
            {
                std::fprintf (stderr,
                              "[Dusk Studio/ChannelStripComponent] shell state-sync "
                              "failed: %s - shell editor opens with defaults.\n",
                              syncErr.toRawUTF8());
            }

            // Take ownership of the raw editor immediately so a factory
            // return-nullptr (bad_alloc on the wrapper, or a future
            // failure branch in createInProcessEditorHost) doesn't leak
            // the editor. JUCE's contract: deleting an AudioProcessor
            // Editor fires AudioProcessor::editorBeingDeleted on the
            // shell instance, which is the correct cleanup path for an
            // editor we never wrapped. On success we .release() the
            // unique_ptr — ownership transfers into the wrapper's own
            // unique_ptr<AudioProcessorEditor> member.
            std::unique_ptr<juce::AudioProcessorEditor> shellEditor (
                pluginSlot.createShellEditor());
            if (shellEditor != nullptr)
            {
                embed = duskstudio::platform::createInProcessEditorHost (shellEditor.get());
                if (embed != nullptr)
                {
                    shellEditor.release();  // ownership now in wrapper
                    // Register the wrapper with PluginSlot so it can
                    // refuse a second concurrent editor + defer shell
                    // release until this wrapper destructs. SafePointer
                    // auto-nulls on wrapper dtor — no explicit "released"
                    // callback needed.
                    pluginSlot.notifyShellEditorWrapper (embed.get());
                }
                // else: shellEditor's unique_ptr dtor runs at scope exit,
                // deleting the JUCE editor and firing editorBeingDeleted.
            }
        }
        else if (shellErr.isNotEmpty())
        {
            std::fprintf (stderr,
                          "[Dusk Studio/ChannelStripComponent] Mac shell-editor "
                          "load failed: %s - falling back to floating child window.\n",
                          shellErr.toRawUTF8());
        }
        if (embed != nullptr)
        {
            embed->setSize (juce::jmax (200, w), juce::jmax (200, h));
            // Tag so a later settings / quit modal hides the shell-editor
            // wrapper too (same reason as the Linux XEmbed + in-process
            // paths). Defensive today — createInProcessEditorHost is a Mac
            // stub returning nullptr — but correct once the shell host lands.
            embed->getProperties().set (kPluginEditorTag, true);
            pluginEditorModal.showBorrowed (*parent, *embed, onClose);
            remoteForeignEmbed = std::move (embed);
        }
        else
        {
            // Fallback: child opened its own native-titlebar top-level.
            // Subsequent clicks re-fire ShowEditor and the child raises
            // the existing window. NOTE: this floating window lives in the
            // child process and is NOT a child of our parent, so EmbeddedModal
            // cannot hide it under a settings/quit modal — that needs an
            // explicit IPC HideEditor/ShowEditor round-trip driven by modal
            // open/close, deferred to the tracked NSView-embed work.
            juce::ignoreUnused (w, h);
        }
       #else
        // Windows: cross-process HWND reparenting via SetParent
        // (ForeignHwndEmbed). The child created its window with
        // WS_OVERLAPPEDWINDOW; the wrapper strips chrome bits on attach
        // and SetParent's the HWND back to desktop on destruct.
        auto embed = duskstudio::platform::createForeignNativeWindowEmbed (windowId);
        if (embed != nullptr)
        {
            embed->setSize (juce::jmax (200, w), juce::jmax (200, h));
            // Tag so a later settings / quit modal hides the reparented HWND
            // wrapper too — native window z-order otherwise paints the plugin
            // above the JUCE-rendered modal.
            embed->getProperties().set (kPluginEditorTag, true);
            pluginEditorModal.showBorrowed (*parent, *embed, onClose);
            remoteForeignEmbed = std::move (embed);
        }
        else
        {
            // Embed creation failed (HWND no longer valid, etc.) — let
            // the child's floating window stand in.
            juce::ignoreUnused (w, h);
        }
       #endif
        return;
    }
   #endif

    auto* instance = pluginSlot.getInstance();
    if (instance == nullptr) return;

    if (pluginEditor != nullptr && pluginEditorOwner != instance)
        dropPluginEditor();

    if (pluginEditor == nullptr)
    {
        // LV2 plugin editors eager-create an embedded X11 sub-window
        // inside their Editor constructor. X11 latch routes the nested
        // peer creation to LinuxComponentPeer (X11) so the reparent
        // target is a valid X11 host. clearPreferX11ForNativeWindow is
        // a no-op on Linux (sticky-on) but kept for cross-platform
        // symmetry.
        std::unique_ptr<juce::AudioProcessorEditor> fresh;
        if (instance->hasEditor())
        {
            // Dry-pass this strip while the editor constructs — sampler
            // editors can block processBlock on their internal loader lock
            // for seconds, which would stall the whole audio callback.
            const juce::SpinLock::ScopedLockType processExclusion (
                pluginSlot.getProcessLock());
            duskstudio::platform::preferX11ForNextNativeWindow();
            fresh.reset (instance->createEditorIfNeeded());
            duskstudio::platform::clearPreferX11ForNativeWindow();
        }
        if (fresh == nullptr)
            fresh = std::make_unique<juce::GenericAudioProcessorEditor> (*instance);
        pluginEditor      = std::move (fresh);
        pluginEditorOwner = instance;
    }

    // Tag so EmbeddedModal hides the editor when a settings / quit /
    // alert modal opens. In-process plugin editors with GL contexts or
    // foreign-window embeds can paint above the modal otherwise.
    pluginEditor->getProperties().set (kPluginEditorTag, true);

    pluginEditorModal.showBorrowed (*parent, *pluginEditor, onClose);
}

void ChannelStripComponent::closePluginEditor()
{
   #if DUSKSTUDIO_HAS_OOP_PLUGINS && ! JUCE_LINUX
    // Mac OOP floating-window path (no embed available): the editor
    // window lives in the child process; the parent has no modal body
    // to tear down — just send HideEditor.
    if (pluginSlot.isRemote() && remoteForeignEmbed == nullptr)
    {
        pluginSlot.hideRemoteEditor();
        return;
    }
   #endif

    pluginEditorModal.close();

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    if (pluginSlot.isRemote())
    {
        // Tell the child to dismiss the editor before our embed
        // (XEmbedComponent on Linux / ForeignHwndEmbed on Windows)
        // destructs. On Linux this lets the XEmbed reparent dance
        // wind down cleanly; on Windows the ~ForeignHwndEmbed call
        // SetParent's the HWND back to desktop, then hideRemoteEditor
        // tells the OOP host to hide the window.
        pluginSlot.hideRemoteEditor();
    }
   #endif
}

void ChannelStripComponent::dropPluginEditor()
{
    closePluginEditor();

    // Native CLAP editor: leak rather than destroy — u-he plugins hang in
    // gui->destroy on shutdown (same class as the JUCE leak-on-shutdown). The
    // shared instance is leaked separately by the engine's shutdown loop. Linux-only.
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    if (clapEditor != nullptr)
    {
        clapEditor->leakForShutdown();
        clapEditor.reset();
    }
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
    if (lv2Editor != nullptr)
    {
        lv2Editor->leakForShutdown();
        lv2Editor.reset();
    }
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
    vst3Editor.reset();   // in-process C++ teardown — no leak path needed
#endif
    // ~AudioProcessorEditor tears down the plugin's internal X11
    // children synchronously (colour pickers, preset browsers,
    // transient popups). On a Wayland session, any of those could
    // theoretically be mutter's focus_window. A yield via
    // requestFocusOnMainWaylandSurface lets the compositor process
    // any pending unmaps before the synchronous destruction lands.
    //
    // Why not defer pluginEditor.reset() into a callAsync the same way
    // closePluginEditor defers the wrapper window: the pluginEditor
    // destructor calls editorBeingDeleted on its AudioProcessor. The
    // AudioProcessor is only kept alive across ONE swap by PluginSlot's
    // previousInstance keep-alive; two quick "Replace plugin" actions
    // within the deferred-reset window would destroy the processor
    // before the deferred reset, turning a rare focus race into a
    // certain use-after-free.
    duskstudio::platform::requestFocusOnMainWaylandSurface();
    pluginEditor.reset();
    pluginEditorOwner = nullptr;

   #if JUCE_LINUX && DUSKSTUDIO_HAS_OOP_PLUGINS
    // Drop the OOP-side embed too. The XEmbedComponent's destructor
    // tells the foreign X11 client to detach; the child has already
    // been told to hide the editor by closePluginEditor above (or by
    // unloadPluginSlot which calls dropPluginEditor before
    // pluginSlot.unload). Either way the X client is safe to release.
    remoteEditorEmbed.reset();
   #endif
   #if DUSKSTUDIO_HAS_OOP_PLUGINS && ! JUCE_LINUX
    // Windows: ~ForeignHwndEmbed (in PlatformWindowing_Windows.cpp)
    // SetParent's the child HWND back to desktop so the OOP host can
    // hide/destroy it cleanly. Mac: nullptr — no-op.
    remoteForeignEmbed.reset();
   #endif
}

#if DUSKSTUDIO_HAS_NATIVE_CLAP
void ChannelStripComponent::loadNativeClapForChannel (const juce::File& clapFile)
{
    auto& strip = engine.getChannelStrip (trackIndex);

    // One insert per strip — tear down whatever editor is open first. The app is
    // alive here (not shutdown), so a plain reset destroys the old clap editor
    // cleanly; only shutdown needs the leak path.
    closePluginEditor();
    clapEditor.reset();
#if DUSKSTUDIO_HAS_NATIVE_LV2
    lv2Editor.reset();   // an evicted LV2's UI must not outlive its instance
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
    vst3Editor.reset();
#endif
    pluginEditor.reset();
    pluginEditorOwner = nullptr;
    // Release any OOP-side embed too — replacing a remote (OOP) plugin with a native
    // CLAP would otherwise leave the foreign X11 / HWND wrapper behind (mirrors
    // dropPluginEditor's teardown).
   #if JUCE_LINUX && DUSKSTUDIO_HAS_OOP_PLUGINS
    remoteEditorEmbed.reset();
   #endif
   #if DUSKSTUDIO_HAS_OOP_PLUGINS && ! JUCE_LINUX
    remoteForeignEmbed.reset();
   #endif

    // NativeClapSlot::load is NOT RT-safe (tears down + rebuilds the instance), so
    // fence the audio thread with the engine process gate around the swap.
    std::string err;
    engine.suspendProcessing();
    pluginSlot.unload();                       // ensure no JUCE plugin lingers
    const bool ok = strip.loadNativeClap (clapFile, err);
    if (ok)
        strip.insertMode.store (ChannelStrip::kInsertPlugin, std::memory_order_release);
    engine.resumeProcessing();

    if (! ok)
    {
        std::fprintf (stderr, "[chan clap] load failed: %s\n", err.c_str());
        showDuskAlert (*this, "Couldn't load CLAP plugin",
                       clapFile.getFileNameWithoutExtension() + ":\n" + juce::String (err));
        // The runtime slot is now empty — clear the persisted native-CLAP refs so a
        // save doesn't carry a stale path/state for a slot the user sees as empty.
        track.nativeClapPath = {};
        track.nativeClapStateBase64 = {};
        return;
    }

    track.nativeClapPath = clapFile.getFullPathName();
    // Fresh bundle: don't reuse the previous plugin's state blob. The next save
    // re-captures this plugin's real state.
    track.nativeClapStateBase64 = {};
    // The load evicted the other native hosts in this insert — drop their refs too.
    track.nativeLv2Path = {};
    track.nativeLv2StateBase64 = {};
    track.nativeVst3Path = {};
    track.nativeVst3StateBase64 = {};
    refreshPluginSlotButton();
    openPluginEditor();
}
#endif // DUSKSTUDIO_HAS_NATIVE_CLAP

#if DUSKSTUDIO_HAS_NATIVE_LV2
void ChannelStripComponent::loadNativeLv2ForChannel (const juce::File& bundleDir)
{
    auto& strip = engine.getChannelStrip (trackIndex);

    // One insert per strip — tear down whatever editor is open first (the CLAP
    // editor references an instance the load below evicts). Mirrors
    // loadNativeClapForChannel.
    closePluginEditor();
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    clapEditor.reset();
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
    vst3Editor.reset();
#endif
    lv2Editor.reset();
    pluginEditor.reset();
    pluginEditorOwner = nullptr;
   #if JUCE_LINUX && DUSKSTUDIO_HAS_OOP_PLUGINS
    remoteEditorEmbed.reset();
   #endif
   #if DUSKSTUDIO_HAS_OOP_PLUGINS && ! JUCE_LINUX
    remoteForeignEmbed.reset();
   #endif

    // NativeLv2Slot::load is NOT RT-safe; fence the audio thread around the swap.
    // loadNativeLv2 itself evicts the CLAP + JUCE occupants.
    std::string err;
    engine.suspendProcessing();
    const bool ok = strip.loadNativeLv2 (bundleDir, err);
    if (ok)
        strip.insertMode.store (ChannelStrip::kInsertPlugin, std::memory_order_release);
    engine.resumeProcessing();

    if (! ok)
    {
        std::fprintf (stderr, "[chan lv2] load failed: %s\n", err.c_str());
        showDuskAlert (*this, "Couldn't load LV2 plugin",
                       bundleDir.getFileNameWithoutExtension() + ":\n" + juce::String (err));
        track.nativeLv2Path = {};
        track.nativeLv2StateBase64 = {};
        return;
    }

    track.nativeLv2Path = bundleDir.getFullPathName();
    track.nativeLv2StateBase64 = {};
    track.nativeClapPath = {};
    track.nativeClapStateBase64 = {};
    track.nativeVst3Path = {};
    track.nativeVst3StateBase64 = {};
    refreshPluginSlotButton();
    openPluginEditor();
}
#endif // DUSKSTUDIO_HAS_NATIVE_LV2

#if DUSKSTUDIO_HAS_NATIVE_VST3
void ChannelStripComponent::loadNativeVst3ForChannel (const juce::File& vst3File)
{
    auto& strip = engine.getChannelStrip (trackIndex);

    // One insert per strip — tear down whatever editor is open first (any open
    // editor references an instance the load below evicts). Mirrors
    // loadNativeClapForChannel.
    closePluginEditor();
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    clapEditor.reset();
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
    lv2Editor.reset();
#endif
    vst3Editor.reset();
    pluginEditor.reset();
    pluginEditorOwner = nullptr;
   #if JUCE_LINUX && DUSKSTUDIO_HAS_OOP_PLUGINS
    remoteEditorEmbed.reset();
   #endif
   #if DUSKSTUDIO_HAS_OOP_PLUGINS && ! JUCE_LINUX
    remoteForeignEmbed.reset();
   #endif

    // NativeVst3Slot::load is NOT RT-safe; fence the audio thread around the swap.
    // loadNativeVst3 itself evicts the CLAP + LV2 + JUCE occupants.
    std::string err;
    engine.suspendProcessing();
    const bool ok = strip.loadNativeVst3 (vst3File, err);
    if (ok)
        strip.insertMode.store (ChannelStrip::kInsertPlugin, std::memory_order_release);
    engine.resumeProcessing();

    if (! ok)
    {
        std::fprintf (stderr, "[chan vst3] load failed: %s\n", err.c_str());
        showDuskAlert (*this, "Couldn't load VST3 plugin",
                       vst3File.getFileNameWithoutExtension() + ":\n" + juce::String (err));
        track.nativeVst3Path = {};
        track.nativeVst3StateBase64 = {};
        return;
    }

    track.nativeVst3Path = vst3File.getFullPathName();
    track.nativeVst3StateBase64 = {};
    track.nativeClapPath = {};
    track.nativeClapStateBase64 = {};
    track.nativeLv2Path = {};
    track.nativeLv2StateBase64 = {};
    refreshPluginSlotButton();
    openPluginEditor();
}
#endif // DUSKSTUDIO_HAS_NATIVE_VST3

namespace
{
// Popup body for the I/O config button. Re-parents the 6 inline ComboBoxes
// (+ MIDI activity LED) into a single column. When the popup closes the
// widgets become orphans; ChannelStripComponent retains ownership and
// state lives on Track atoms, so reopening the popup just re-parents.
class IoConfigPopup : public juce::Component
{
public:
    IoConfigPopup (juce::ComboBox& mode, juce::ComboBox& in, juce::ComboBox& inR,
                    juce::ComboBox& mIn, juce::ComboBox& mCh, juce::ComboBox& mOut,
                    juce::Component& led)
        : modeSelector (mode), inputSelector (in), inputSelectorR (inR),
          midiInputSelector (mIn), midiChannelSelector (mCh),
          midiOutputSelector (mOut), activityLed (led)
    {
        addAndMakeVisible (modeSelector);
        addAndMakeVisible (inputSelector);
        addAndMakeVisible (inputSelectorR);
        addAndMakeVisible (midiInputSelector);
        addAndMakeVisible (midiChannelSelector);
        addAndMakeVisible (midiOutputSelector);
        addAndMakeVisible (activityLed);
        setSize (240, 160);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (8);
        constexpr int kRowH = 24, kGap = 6;

        modeSelector.setBounds (area.removeFromTop (kRowH));
        area.removeFromTop (kGap);

        const int mode = juce::jlimit (0, 2, modeSelector.getSelectedId() - 1);
        inputSelector.setVisible (mode == 0 || mode == 1);
        inputSelectorR.setVisible (mode == 1);
        midiInputSelector  .setVisible (mode == 2);
        midiChannelSelector.setVisible (mode == 2);
        midiOutputSelector .setVisible (mode == 2);
        activityLed       .setVisible (mode == 2);

        if (mode == 0)
        {
            inputSelector.setBounds (area.removeFromTop (kRowH));
        }
        else if (mode == 1)
        {
            auto row = area.removeFromTop (kRowH);
            const int half = (row.getWidth() - 4) / 2;
            inputSelector .setBounds (row.removeFromLeft (half));
            row.removeFromLeft (4);
            inputSelectorR.setBounds (row);
        }
        else
        {
            constexpr int kLedW = 14;
            auto inRow = area.removeFromTop (kRowH);
            activityLed.setBounds (inRow.removeFromRight (kLedW).reduced (1));
            inRow.removeFromRight (4);
            midiInputSelector.setBounds (inRow);
            area.removeFromTop (kGap);
            midiChannelSelector.setBounds (area.removeFromTop (kRowH));
            area.removeFromTop (kGap);
            midiOutputSelector.setBounds (area.removeFromTop (kRowH));
        }
    }

private:
    juce::ComboBox& modeSelector;
    juce::ComboBox& inputSelector;
    juce::ComboBox& inputSelectorR;
    juce::ComboBox& midiInputSelector;
    juce::ComboBox& midiChannelSelector;
    juce::ComboBox& midiOutputSelector;
    juce::Component& activityLed;
};
} // namespace

void ChannelStripComponent::openIoConfigPopup()
{
    if (ioConfigModal.isOpen()) { ioConfigModal.close(); return; }

    auto panel = std::make_unique<IoConfigPopup> (modeSelector, inputSelector, inputSelectorR,
                                                   midiInputSelector, midiChannelSelector,
                                                   midiOutputSelector, midiActivityLed);
    const int mode = juce::jlimit (0, 2, modeSelector.getSelectedId() - 1);
    const int rows = mode == 2 ? 4 : 2;
    panel->setSize (240, 8 + rows * 24 + (rows - 1) * 6 + 8);

    auto* topLevel = getTopLevelComponent();
    if (topLevel == nullptr) topLevel = this;

    // The panel borrows this strip's live combo children; owning show() re-parents
    // them into the panel. On close the panel is destroyed and the combos are left
    // parentless (still owned as strip members, so they survive) — invisible until
    // the next open re-parents them, which is what we want: compact mode shows the
    // ioConfigButton, never the bare combos. The DuskComboBox dropdowns render as
    // nested in-window modals, so no CallOutBox-style "sticky while a native popup
    // is up" guard is needed.
    ioConfigModal.show (*topLevel, std::move (panel),
                        /*onDismiss*/ {}, /*dismissOnClickOutside*/ true,
                        /*dismissOnEscape*/ true, kEditorDimAlpha);
}

void ChannelStripComponent::refreshAuxSendLabel (int auxIdx)
{
    if (auxIdx < 0 || auxIdx >= (int) auxKnobLabels.size()) return;
    // Read the LIVE (animated) value so the label matches the knob, which is
    // driven from liveAuxSendDb (lane value in Read/Touch, setpoint in Off);
    // reading auxSendDb left the text stale during automation / MIDI moves.
    const float dB    = track.strip.liveAuxSendDb[(size_t) auxIdx]
                              .load (std::memory_order_relaxed);
    const bool  isPre = track.strip.auxSendPreFader[(size_t) auxIdx]
                              .load (std::memory_order_relaxed);

    // Inline the formatter — the ctor's `formatAuxSend` lambda is
    // out of scope here. Same logic, kept terse. PRE suffix appended
    // regardless of level so users can see pre-fader state even when
    // the send is parked at off.
    juce::String text;
    if (dB <= ChannelStripParams::kAuxSendMinDb + 0.01f)
        text = juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92"));
    else if (std::abs (dB) >= 10.0f)
        text = juce::String ((int) std::round (dB));
    else
        text = juce::String (dB, 1);
    if (isPre) text += " PRE";

    auxKnobLabels[(size_t) auxIdx].setText (text, juce::dontSendNotification);

    // Outline-colour cue mirrors the PRE text — bright amber ring when
    // pre-fader, dim default when post. Updated here (not just in ctor)
    // so the right-click PRE/POST toggle reflects on the knob ring
    // without needing a strip rebuild.
    if (auxIdx < (int) auxKnobs.size() && auxKnobs[(size_t) auxIdx] != nullptr)
    {
        auxKnobs[(size_t) auxIdx]->setColour (
            juce::Slider::rotarySliderOutlineColourId,
            isPre ? juce::Colour (0xffffc060)
                  : juce::Colour (0xff404048));
        auxKnobs[(size_t) auxIdx]->repaint();
    }
}

void ChannelStripComponent::refreshPrintButtonForMode()
{
    const bool isMidi = (track.mode.load (std::memory_order_relaxed)
                        == (int) Track::Mode::Midi);
    const bool frozen = track.frozen.load (std::memory_order_relaxed);

    // The button is FREEZE for any MIDI track, and for an audio track once it
    // has a recorded take — at that point PRINT (a capture-time decision) is
    // moot and you'd rather bake the insert + EQ/comp to reclaim CPU (a big win
    // at high oversampling). An empty audio track keeps PRINT. Driven each timer
    // tick, so the button flips to FREEZE the moment a recording lands.
    const bool showFreeze = frozen || isMidi || ! track.regions.empty();

    if (! showFreeze)
    {
        // Empty audio track: PRINT toggle (capture post-EQ/comp into the take).
        printButton.setClickingTogglesState (true);
        printButton.setEnabled (true);
        printButton.setButtonText ("PRINT");
        printButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff202024));
        printButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff8a7060));
        printButton.setToggleState (track.printEffects.load (std::memory_order_relaxed),
                                     juce::dontSendNotification);
        printButton.setTooltip ("PRINT - when on, the recorded WAV captures "
                                  "the post-EQ/post-comp signal (effects baked "
                                  "in). Off = clean input recorded; effects "
                                  "only at playback.");
        // Restore the PRINT-specific a11y metadata — otherwise a FREEZE→PRINT flip
        // leaves the stale "Freeze track" title/help for assistive tech.
        printButton.setTitle ("Print effects on record");
        printButton.setHelpText (printButton.getTooltip());
        return;
    }

    // FREEZE: render the track (MIDI instrument or recorded audio, plus its
    // insert + EQ + comp) to a WAV and bypass that DSP to free CPU. A frozen
    // track shows a snowflake; click toggles freeze/unfreeze.
    const juce::String what = isMidi ? "instrument + EQ + comp"
                                      : "insert + EQ + comp";
    printButton.setClickingTogglesState (false);
    // Clear any leftover toggle bit from a prior PRINT-ON state — otherwise the
    // button paints with buttonOnColourId/textColourOnId (the orange PRINT-on
    // look) instead of the FREEZE/snowflake colours set below.
    printButton.setToggleState (false, juce::dontSendNotification);
    printButton.setEnabled (true);
    printButton.setButtonText (frozen
        ? juce::String::charToString ((juce::juce_wchar) 0x2744)   // ❄ snowflake
        : juce::String ("FREEZE"));
    printButton.setColour (juce::TextButton::buttonColourId,
                            frozen ? juce::Colour (0xff2a5a78) : juce::Colour (0xff202024));
    printButton.setColour (juce::TextButton::textColourOffId,
                            frozen ? juce::Colour (0xffbfe4ff) : juce::Colour (0xff8a7060));
    printButton.setTooltip (frozen
        ? "FROZEN - the " + what + " are rendered to audio and bypassed to free "
          "CPU. Click to unfreeze and edit again."
        : "FREEZE - render this track (" + what + ") to audio and bypass that "
          "DSP to free CPU. Click again to unfreeze.");
    // Keep assistive-tech metadata in step with the current mode (not the old
    // "print effects on record" label).
    printButton.setTitle (frozen ? "Unfreeze track" : "Freeze track");
    printButton.setHelpText (printButton.getTooltip());
}

void ChannelStripComponent::handleFreezeClick()
{
    // Unfreeze is cheap (no render) — do it inline and refresh the button.
    if (track.frozen.load (std::memory_order_relaxed))
    {
        engine.unfreezeTrack (trackIndex);
        refreshPrintButtonForMode();
        return;
    }

    // Freeze renders offline; run it async behind a progress + cancel modal so a
    // long render never wedges the UI. The dialog commits the freeze on success;
    // we just refresh the button (snowflake) when it closes.
    auto dialog = std::make_unique<FreezeDialog> (engine, session,
                                                   engine.getDeviceManager(), trackIndex);
    dialog->setSize (440, 150);

    juce::Component::SafePointer<ChannelStripComponent> safe (this);
    dialog->onRequestClose = [safe]
    {
        if (auto* self = safe.getComponent())
        {
            self->freezeModal.close();
            self->refreshPrintButtonForMode();
        }
    };

    auto* parent = findParentComponentOfClass<juce::Component>();
    if (parent == nullptr) parent = this;
    // Non-dismissable by stray click / Escape: only the dialog's Cancel / Close
    // buttons drive teardown, so a render is never torn down out from under the
    // worker (which would join the thread on the message thread mid-render).
    freezeModal.show (*parent, std::move (dialog), {}, false, false);
}

void ChannelStripComponent::refreshIoConfigButton()
{
    const int mode = juce::jlimit (0, 2, track.mode.load (std::memory_order_relaxed));
    juce::String text;
    if (mode == 0)        // Mono
    {
        text = "Mono ";
        const auto in = inputSelector.getText();
        text += in.isEmpty() ? "(none)" : in;
    }
    else if (mode == 1)   // Stereo
    {
        const auto inL = inputSelector .getText();
        const auto inR = inputSelectorR.getText();
        text = "Stereo " + (inL.isEmpty() ? juce::String ("?") : inL)
                + " / " + (inR.isEmpty() ? juce::String ("?") : inR);
    }
    else                  // MIDI
    {
        const auto port = midiInputSelector.getText();
        const auto ch   = midiChannelSelector.getText();
        // U+00B7 middle dot via CharPointer_UTF8 - juce::String's char*
        // ctor uses the system locale which mangles UTF-8 on Linux.
        const juce::String midDot (juce::CharPointer_UTF8 ("\xc2\xb7"));
        text = "MIDI " + (port.isEmpty() ? juce::String ("(none)") : port)
                + " " + midDot + " " + (ch.isEmpty() ? juce::String ("Omni") : ch);
    }
    if (ioConfigButton.getButtonText() != text)
        ioConfigButton.setButtonText (text);
}

void ChannelStripComponent::openEqEditorPopup()
{
    if (eqEditorModal.isOpen()) { eqEditorModal.close(); return; }

    // Mutual exclusion — one processing editor at a time.
    if (compEditorModal.isOpen()) compEditorModal.close();
    if (auxEditorModal.isOpen())  auxEditorModal.close();

    auto panel = std::make_unique<ChannelEqEditor> (track);
    panel->setSize (380, 648);   // match ChannelEqEditor's intrinsic height (full bell rows)

    auto* topLevel = getTopLevelComponent();
    if (topLevel == nullptr) topLevel = this;

    eqEditorModal.show (*topLevel, std::move (panel),
                        /*onDismiss*/ {}, /*dismissOnClickOutside*/ true,
                        /*dismissOnEscape*/ true, kEditorDimAlpha);
}

void ChannelStripComponent::openCompEditorPopup()
{
    if (compEditorModal.isOpen()) { compEditorModal.close(); return; }

    if (eqEditorModal.isOpen())  eqEditorModal.close();
    if (auxEditorModal.isOpen()) auxEditorModal.close();

    // ChannelCompEditor sizes itself in its ctor (uniform height across all
    // comp modes — see refreshLabelsForMode), so it's fully sized before show().
    auto panel = std::make_unique<ChannelCompEditor> (track);

    auto* topLevel = getTopLevelComponent();
    if (topLevel == nullptr) topLevel = this;

    compEditorModal.show (*topLevel, std::move (panel),
                          /*onDismiss*/ {}, /*dismissOnClickOutside*/ true,
                          /*dismissOnEscape*/ true, kEditorDimAlpha);
}

namespace
{
// Compact-mode popup body for the AUX sends. Mirrors the inline 4-knob row's
// behaviour: same param atoms (track.strip.auxSendDb), same colour grammar,
// same off-sentinel mapping. Sized so the 4 knobs sit at a comfortable
// editing diameter — bigger than the inline strip's 24 px rotaries.
class AuxSendsCompactPanel : public juce::Component, private juce::Timer
{
public:
    AuxSendsCompactPanel (Track& t, Session& s) : track (t), sessionRef (s)
    {
        startTimerHz (15);   // poll the session aux names so an edit here
                              // syncs with renames elsewhere on the strip.
        static const juce::Colour colours[ChannelStripParams::kNumAuxSends] = {
            juce::Colour (0xff5a8ad0), juce::Colour (0xff9080c0),
            juce::Colour (0xffe0c050), juce::Colour (0xff60c060),
        };
        auto formatAuxSend = [] (float dB, bool preFader) -> juce::String
        {
            juce::String s;
            if (dB <= ChannelStripParams::kAuxSendMinDb + 0.01f)
                s = juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92"));   // "−"
            else
                s = (std::abs (dB) >= 10.0f)
                        ? juce::String ((int) std::round (dB))
                        : juce::String (dB, 1);
            if (preFader) s += " PRE";
            return s;
        };

        for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
        {
            auto& k = knobs[(size_t) i];
            k.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            k.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            k.setRange (ChannelStripParams::kAuxSendMinDb,
                          ChannelStripParams::kAuxSendMaxDb, 0.1);
            k.setSkewFactorFromMidPoint (-12.0);
            k.setColour (juce::Slider::rotarySliderFillColourId,    colours[i]);
            {
                const bool initialPre = track.strip.auxSendPreFader[(size_t) i]
                                              .load (std::memory_order_relaxed);
                k.setColour (juce::Slider::rotarySliderOutlineColourId,
                              initialPre ? juce::Colour (0xffffc060)
                                          : juce::Colour (0xff404048));
            }
            k.setDoubleClickReturnValue (true, ChannelStripParams::kAuxSendOffDb);
            k.setTooltip ("AUX " + juce::String (i + 1) + " send level. "
                            "Double-click for OFF.");

            const float initial = track.strip.auxSendDb[(size_t) i].load (std::memory_order_relaxed);
            k.setValue (initial <= ChannelStripParams::kAuxSendMinDb + 0.01f
                            ? ChannelStripParams::kAuxSendOffDb : initial,
                          juce::dontSendNotification);

            k.onValueChange = [this, i, formatAuxSend]
            {
                const float v = (float) knobs[(size_t) i].getValue();
                const float stored = (v <= ChannelStripParams::kAuxSendMinDb + 0.01f)
                                            ? ChannelStripParams::kAuxSendOffDb : v;
                track.strip.auxSendDb[(size_t) i].store (stored, std::memory_order_relaxed);
                const bool preFader = track.strip.auxSendPreFader[(size_t) i]
                                          .load (std::memory_order_relaxed);
                valueLabels[(size_t) i].setText (formatAuxSend (stored, preFader),
                                                    juce::dontSendNotification);
            };
            k.onDragStart = [this, i]
            {
                track.strip.auxSendTouched[(size_t) i].store (true, std::memory_order_release);
            };
            k.onDragEnd = [this, i]
            {
                track.strip.auxSendTouched[(size_t) i].store (false, std::memory_order_release);
            };
            addAndMakeVisible (k);

            auto& il = indexLabels[(size_t) i];
            il.setText (sessionRef.auxLane (i).name, juce::dontSendNotification);
            il.setJustificationType (juce::Justification::centred);
            il.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
            il.setColour (juce::Label::textColourId, colours[i].brighter (0.2f));
            il.setMinimumHorizontalScale (0.5f);
            il.setTooltip ("Double-click to rename this AUX send.");
            il.setEditable (false, true, false);
            il.setColour (juce::Label::backgroundWhenEditingColourId, juce::Colour (0xff202024));
            il.setColour (juce::Label::textWhenEditingColourId,       juce::Colours::white);
            il.onTextChange = [this, i]
            {
                auto& lbl = indexLabels[(size_t) i];
                auto txt = lbl.getText().trim();
                if (txt.isEmpty())
                {
                    lbl.setText (sessionRef.auxLane (i).name, juce::dontSendNotification);
                    return;
                }
                constexpr int kAuxNameMaxChars = 12;
                if (txt.length() > kAuxNameMaxChars)
                    txt = txt.substring (0, kAuxNameMaxChars);
                sessionRef.auxLane (i).name = txt;
                lbl.setText (txt, juce::dontSendNotification);
            };
            addAndMakeVisible (il);

            auto& vl = valueLabels[(size_t) i];
            vl.setJustificationType (juce::Justification::centred);
            vl.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
            vl.setColour (juce::Label::textColourId, juce::Colour (0xffd8d8d8));
            {
                const bool initialPre = track.strip.auxSendPreFader[(size_t) i]
                                             .load (std::memory_order_relaxed);
                vl.setText (formatAuxSend (initial, initialPre),
                              juce::dontSendNotification);
            }
            addAndMakeVisible (vl);
        }

        setSize (260, 130);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10);
        const int colW = area.getWidth() / ChannelStripParams::kNumAuxSends;
        for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
        {
            auto col = juce::Rectangle<int> (area.getX() + i * colW, area.getY(),
                                                colW, area.getHeight());
            indexLabels[(size_t) i].setBounds (col.removeFromTop (14));
            valueLabels[(size_t) i].setBounds (col.removeFromBottom (14));
            const int knobSize = juce::jmin (col.getWidth() - 4, col.getHeight() - 4);
            const int kx = col.getCentreX() - knobSize / 2;
            const int ky = col.getCentreY() - knobSize / 2;
            knobs[(size_t) i].setBounds (kx, ky, knobSize, knobSize);
        }
    }

    void timerCallback() override
    {
        for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
        {
            auto& lbl = indexLabels[(size_t) i];
            const auto& laneName = sessionRef.auxLane (i).name;
            if (! lbl.isBeingEdited() && lbl.getText (false) != laneName)
                lbl.setText (laneName, juce::dontSendNotification);

            // Live value (lane in Read/Touch, setpoint in Off) so the compact
            // popup's text + knob outline match what's actually being sent,
            // not the stale setpoint, during automation / MIDI-driven moves.
            const float dB    = track.strip.liveAuxSendDb[(size_t) i]
                                    .load (std::memory_order_relaxed);
            const bool  isPre = track.strip.auxSendPreFader[(size_t) i]
                                    .load (std::memory_order_relaxed);

            // Mirror the knob to the live value (gated on not-dragging so we
            // don't fight a user gesture) - the label + outline below follow
            // liveAuxSendDb, and the knob must too or text and knob desync
            // during automation / MIDI-driven moves.
            if (! track.strip.auxSendTouched[(size_t) i].load (std::memory_order_acquire))
            {
                const float rawVal = (dB <= ChannelStripParams::kAuxSendMinDb + 0.01f)
                                          ? ChannelStripParams::kAuxSendOffDb : dB;
                // Clamp to the slider's range before comparing - the off
                // sentinel (-100) sits below the slider min (-60), so an
                // unclamped compare never converges and refires setValue every
                // tick while the send is off.
                const float knobVal = (float) juce::jlimit (knobs[(size_t) i].getMinimum(),
                                                            knobs[(size_t) i].getMaximum(),
                                                            (double) rawVal);
                if (std::abs ((float) knobs[(size_t) i].getValue() - knobVal) > 0.05f)
                    knobs[(size_t) i].setValue (knobVal, juce::dontSendNotification);
            }

            juce::String txt;
            if (dB <= ChannelStripParams::kAuxSendMinDb + 0.01f)
                txt = juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92"));
            else
                txt = (std::abs (dB) >= 10.0f)
                          ? juce::String ((int) std::round (dB))
                          : juce::String (dB, 1);
            if (isPre) txt += " PRE";
            auto& vl = valueLabels[(size_t) i];
            if (vl.getText (false) != txt)
                vl.setText (txt, juce::dontSendNotification);

            // Outline-colour cue mirrors the PRE text — bright amber
            // ring when pre-fader, dim default when post.
            const auto wantOutline = isPre ? juce::Colour (0xffffc060)
                                            : juce::Colour (0xff404048);
            auto& kk = knobs[(size_t) i];
            if (kk.findColour (juce::Slider::rotarySliderOutlineColourId) != wantOutline)
            {
                kk.setColour (juce::Slider::rotarySliderOutlineColourId, wantOutline);
                kk.repaint();
            }
        }
    }

private:
    Track&   track;
    Session& sessionRef;
    std::array<juce::Slider, ChannelStripParams::kNumAuxSends> knobs;
    std::array<juce::Label,  ChannelStripParams::kNumAuxSends> indexLabels;
    std::array<juce::Label,  ChannelStripParams::kNumAuxSends> valueLabels;
};
} // namespace

void ChannelStripComponent::openAuxEditorPopup()
{
    if (auxEditorModal.isOpen()) { auxEditorModal.close(); return; }

    if (eqEditorModal.isOpen())   eqEditorModal.close();
    if (compEditorModal.isOpen()) compEditorModal.close();

    // AuxSendsCompactPanel sizes itself in its ctor (setSize 260x130).
    auto panel = std::make_unique<AuxSendsCompactPanel> (track, session);

    auto* topLevel = getTopLevelComponent();
    if (topLevel == nullptr) topLevel = this;

    auxEditorModal.show (*topLevel, std::move (panel),
                         /*onDismiss*/ {}, /*dismissOnClickOutside*/ true,
                         /*dismissOnEscape*/ true, kEditorDimAlpha);
}

int ChannelStripComponent::groupMasterIndex() const noexcept
{
    const int gid = track.strip.faderGroupId.load (std::memory_order_relaxed);
    if (gid == 0) return -1;
    for (int t = 0; t < Session::kNumTracks; ++t)
        if (session.track (t).strip.faderGroupId.load (std::memory_order_relaxed) == gid)
            return t;
    return -1;
}

void ChannelStripComponent::refreshFaderValueLabel()
{
    const double db = faderSlider.getValue();
    faderValueLabel.setText (db <= -89.95 ? juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x9e"))   /* ∞ = -inf dB / fully off */
                                          : juce::String (db, 1),
                             juce::dontSendNotification);
}

void ChannelStripComponent::timerCallback()
{
    // Plugin-slot button reflects the slot's current load state. Cheap -
    // just an atomic-pointer read + string compare against the cached name.
    refreshPluginSlotButton();

    // PRINT↔FREEZE flips on an audio track the moment a recording lands (or its
    // last region is deleted). Idempotent + cheap (setButtonText/Colour are
    // no-ops when unchanged), so polling here avoids a cross-component signal.
    refreshPrintButtonForMode();

    // Fader-group chip: relayout + repaint only when this strip's group, or
    // its master status, actually changes - the group can be edited from
    // another strip's context menu, so polling here keeps every strip's chip
    // (and which member is master) consistent without a cross-strip signal.
    {
        const int gid = track.strip.faderGroupId.load (std::memory_order_relaxed);
        const bool isMaster = (gid != 0 && groupMasterIndex() == trackIndex);
        if (gid != lastGroupId || isMaster != lastGroupMaster)
        {
            lastGroupId     = gid;
            lastGroupMaster = isMaster;
            resized();
            repaint();
        }
    }

    // Sync nameLabel with track.name when something external rewrites it
    // (e.g. an audio import auto-renames the track to the file's basename).
    // Skip while the user is mid-edit so their typed text isn't stomped.
    if (! nameLabel.isBeingEdited()
        && nameLabel.getText (false) != track.name)
        nameLabel.setText (track.name, juce::dontSendNotification);

    if (lastTrackColour != track.colour)
    {
        lastTrackColour = track.colour;
        repaint();
    }

    // Aux send name sync — when another strip (or the AuxLaneComponent
    // header) renames an aux lane, every strip's matching label must
    // update so the rename reads as global.
    for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
    {
        auto& lbl = auxIndexLabels[(size_t) i];
        const auto& laneName = session.auxLane (i).name;
        if (! lbl.isBeingEdited() && lbl.getText (false) != laneName)
            lbl.setText (laneName, juce::dontSendNotification);
    }

    // MIDI activity LED. Read-and-clear the engine's flag each tick — a
    // continuous stream sets it back to true on the next block, so the LED
    // stays lit while traffic flows and turns off ~33 ms after it stops.
    {
        const bool fired = track.midiActivity.exchange (false, std::memory_order_relaxed);
        if (midiActivityLed.lit != fired)
        {
            midiActivityLed.lit = fired;
            midiActivityLed.repaint();
        }
    }

    // Sync the inline COMP mode button with the underlying atoms so it
    // reflects writes from other surfaces (meter-strip threshold drag,
    // popup editor's ON toggle, MIDI binding, etc.). State-only refresh
    // every tick is cheap; the heavier knob-visibility filter only
    // fires when compMode actually changed since the last apply
    // (catches session-load + MIDI-binding mode swaps that bypass the
    // popup-menu callback).
    refreshCompModeButtonState();
    {
        const int liveMode = juce::jlimit (0, 2,
            track.strip.compMode.load (std::memory_order_relaxed));
        if (liveMode != lastAppliedCompMode)
        {
            lastAppliedCompMode = liveMode;
            refreshCompKnobVisibility();
            resized();   // re-layout so the new mode's knobs land in
                          // their cells instead of the prior mode's
                          // stale positions.
        }
    }
    if (eqHeaderBtn != nullptr)
    {
        // Surface the active EQ type on the header so the user can
        // Type cue now lives on the dedicated eqTypeChip mid-strip
        // (between HM and LM rows). Just repaint the header so its LED
        // tracks atom changes from external mutation paths.
        eqHeaderBtn->repaint();
    }
    // Sync the dedicated E/G chip's label + toggle state from the atom
    // so external writes (popup editor type picker) reflect inline.
    {
        const bool isBlack = track.strip.eqBlackMode.load (std::memory_order_relaxed);
        if (eqTypeChip.getToggleState() != isBlack)
        {
            eqTypeChip.setToggleState (isBlack, juce::dontSendNotification);
            eqTypeChip.setButtonText (isBlack ? "G" : "E");
        }
    }

    // Sync the inline COMP knobs with their underlying atoms so writes
    // from the meter-strip threshold drag or the popout editor reflect
    // here without the user having to reload. Skip a knob the user is
    // actively dragging — otherwise their drag would snap back to the
    // stored value mid-motion.
    {
        auto syncKnob = [] (juce::Slider& k, float target)
        {
            if (k.isMouseButtonDown()) return;
            if (std::abs ((float) k.getValue() - target) < 1.0e-4f) return;
            k.setValue (target, juce::dontSendNotification);
        };
        const auto& sp = track.strip;
        syncKnob (optoPeakRedKnob, sp.compOptoPeakRed.load (std::memory_order_relaxed));
        syncKnob (optoGainKnob,    sp.compOptoGain.load    (std::memory_order_relaxed));
        syncKnob (fetInputKnob,    sp.compFetInput.load    (std::memory_order_relaxed));
        syncKnob (fetOutputKnob,   sp.compFetOutput.load   (std::memory_order_relaxed));
        syncKnob (fetAttackKnob,   sp.compFetAttack.load   (std::memory_order_relaxed));
        syncKnob (fetReleaseKnob,  sp.compFetRelease.load  (std::memory_order_relaxed));
        syncKnob (fetRatioKnob,    (float) sp.compFetRatio.load (std::memory_order_relaxed));
        syncKnob (vcaRatioKnob,    sp.compVcaRatio.load    (std::memory_order_relaxed));
        syncKnob (vcaAttackKnob,   sp.compVcaAttack.load   (std::memory_order_relaxed));
        syncKnob (vcaReleaseKnob,  sp.compVcaRelease.load  (std::memory_order_relaxed));
        syncKnob (vcaOutputKnob,   sp.compVcaOutput.load   (std::memory_order_relaxed));
    }

    // TIMELINE-mode compact buttons get an on-air indicator so the user
    // knows whether EQ / COMP is engaged without having to open the
    // popup. Bullet character + brighter background when on.
    if (compactMode)
    {
        // Channel-strip EQ illuminates when the user has explicitly
        // enabled the 4-band EQ (auto-armed on first knob touch, also
        // user-toggleable via the LED in the EQ header) OR when HPF is
        // engaged (HPF has its own atomic gate independent of eqEnabled).
        // Comp has a real on/off atom.
        const auto& sp = track.strip;
        const bool eqOn   = sp.eqEnabled.load (std::memory_order_relaxed)
                          || sp.hpfEnabled.load (std::memory_order_relaxed);
        const bool compOn = sp.compEnabled.load (std::memory_order_relaxed);

        const auto eqAccent   = juce::Colour (fourKColors::kLfGreen);
        const auto compAccent = juce::Colour (fourKColors::kCompGold);

        // Text stays as the section name; the illuminated background
        // (set below) is the on-state indicator. No leading bullet/dot.
        if (eqCompactButton.getButtonText() != "EQ")
            eqCompactButton.setButtonText ("EQ");
        if (compCompactButton.getButtonText() != "COMP")
            compCompactButton.setButtonText ("COMP");

        eqCompactButton  .setColour (juce::TextButton::buttonColourId,
                                       eqOn   ? eqAccent.darker (0.55f)   : juce::Colour (0xff222226));
        compCompactButton.setColour (juce::TextButton::buttonColourId,
                                       compOn ? compAccent.darker (0.55f) : juce::Colour (0xff222226));
    }

    // Detect bus-colour edits made via the aux strip's right-click menu and
    // re-skin the bus assignment buttons accordingly. Polling at 30 Hz is
    // negligible (16 strips × 4 buses).
    for (int i = 0; i < ChannelStripParams::kNumBuses; ++i)
    {
        const auto current = session.bus (i).colour;
        if (current.getARGB() != lastBusColours[(size_t) i])
        {
            lastBusColours[(size_t) i] = current.getARGB();
            if (auto& btn = busButtons[(size_t) i])
            {
                btn->setColour (juce::TextButton::buttonOnColourId, current);
                btn->setColour (juce::TextButton::textColourOffId,  current.brighter (0.15f));
                btn->repaint();
            }
        }
    }

    const float gr = track.meterGrDb.load (std::memory_order_relaxed);
    if (gr < displayedGrDb)
        displayedGrDb = gr;                              // instant attack
    else
        // ~48 ms recovery (was 0.18 = ~167 ms, which masked comp release times
        // faster than the meter's own ballistic). Truer GR readout.
        displayedGrDb += (gr - displayedGrDb) * 0.5f;

    // Input level meter - fast attack on rise, slow decay; with a peak-hold
    // marker that lingers for ~600 ms before falling. Stereo mode also
    // smooths the R channel so the second LED bar can be drawn alongside.
    auto smoothMeter = [] (float incoming, float& displayed,
                            float& peakHold, int& peakHoldFrames)
    {
        if (incoming > displayed) displayed = incoming;
        else                       displayed += (incoming - displayed) * 0.15f;

        if (incoming >= peakHold)
        {
            peakHold       = incoming;
            peakHoldFrames = 18;  // ~600 ms at 30 Hz
        }
        else if (peakHoldFrames > 0) --peakHoldFrames;
        else peakHold = juce::jmax (-100.0f, peakHold - 1.5f);
    };

    const bool stereoMode = (track.mode.load (std::memory_order_relaxed)
                              == (int) Track::Mode::Stereo);

    // Meter follows the monitored source: pre-fader INPUT while the track is
    // input-monitoring (IN engaged, so record levels read correctly), post-
    // fader OUTPUT otherwise (the track's contribution to the mix). Mono shows
    // the hotter output channel so a hard pan doesn't read as silence.
    const bool  inputMon = track.inputMonitor.load (std::memory_order_relaxed);
    const float outL     = track.meterOutLDb.load (std::memory_order_relaxed);
    const float outR     = track.meterOutRDb.load (std::memory_order_relaxed);
    const float lDb = inputMon ? track.meterInputDb.load (std::memory_order_relaxed)
                               : (stereoMode ? outL : juce::jmax (outL, outR));
    smoothMeter (lDb, displayedInputDb, inputPeakHoldDb, inputPeakHoldFrames);

    if (stereoMode)
    {
        const float rDb = inputMon ? track.meterInputRDb.load (std::memory_order_relaxed)
                                   : outR;
        smoothMeter (rDb, displayedInputRDb, inputPeakHoldRDb, inputPeakHoldRFrames);
    }
    else
    {
        // Bleed the R channel back to silence when the user flips out of
        // stereo so the dual-bar doesn't ghost-hold its last value.
        displayedInputRDb = -100.0f;
        inputPeakHoldRDb  = -100.0f;
    }

    // GR repaint is handled inside CompMeterStrip's own Timer.
    if (! inputMeterArea.isEmpty())  repaint (inputMeterArea);

    // Update the numeric readout below the meter.
    // One-decimal dB readout, matching the bus + master output-peak readouts
    // so the level number reads identically across every strip type. The
    // label's minimumHorizontalScale keeps "-12.3" inside the narrow column.
    if (inputPeakHoldDb <= -60.0f)
        inputPeakLabel.setText ("-inf", juce::dontSendNotification);
    else
        inputPeakLabel.setText (juce::String (inputPeakHoldDb, 1),
                                  juce::dontSendNotification);

    // Tint the readout when peaks get hot.
    inputPeakLabel.setColour (juce::Label::textColourId,
        inputPeakHoldDb >= -3.0f  ? juce::Colour (0xffff5050) :
        inputPeakHoldDb >= -12.0f ? juce::Colour (0xffe0c050) :
                                     juce::Colour (0xffd0d0d0));

    // GR readout: show "-X.X" when the comp is reducing, dim "0.0" otherwise.
    // displayedGrDb is already smoothed above (asymmetric: fast attack on
    // rise, slow release on fall), matching the visual GR meter.
    if (displayedGrDb <= -0.05f)
    {
        grPeakLabel.setText (juce::String (displayedGrDb, 1), juce::dontSendNotification);
        grPeakLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe0c050));
    }
    else
    {
        grPeakLabel.setText ("0.0", juce::dontSendNotification);
        grPeakLabel.setColour (juce::Label::textColourId, juce::Colour (0xff606064));
    }

    // Track-3 fader-side GR readout: numeric value sitting below the slim
    // GR LED. Inert grey when bypassed or no compression; gold when active.
    if (usesFaderThresholdLayout())
    {
        const bool engaged = track.strip.compEnabled.load (std::memory_order_relaxed);
        if (engaged && displayedGrDb <= -0.05f)
        {
            grReadoutLabel.setText (juce::String (displayedGrDb, 1),
                                      juce::dontSendNotification);
            grReadoutLabel.setColour (juce::Label::textColourId,
                                        juce::Colour (0xffe0c050));
        }
        else
        {
            grReadoutLabel.setText ("0.0", juce::dontSendNotification);
            grReadoutLabel.setColour (juce::Label::textColourId,
                                        juce::Colour (0xff606064));
        }
    }

    // Motor-fader / motor-pan animation: when the audio engine is feeding
    // a live atom from the lane (Read, or Touch when not grabbed), mirror
    // it into the slider/knob visually. Gate on a small delta to avoid
    // setValue churn every tick when manual mode just mirrors the user's
    // setpoint. While the user is grabbing in Touch mode, their gesture
    // IS the value source, so we don't fight it.
    {
        const int amode = track.automationMode.load (std::memory_order_relaxed);
        const bool isWrite = amode == (int) AutomationMode::Write;
        const bool isTouch = amode == (int) AutomationMode::Touch;
        const bool playing = engine.getTransport().isPlaying();

        // Fader animate / capture. Animate whenever liveFaderDb diverges
        // from what we've drawn AND the user isn't dragging — this covers
        // automation Read/Touch AS WELL AS external sources that mutate
        // faderDb directly (MIDI Learn bindings, MCU faders, future
        // remote controls). Gating on `isRead || isTouch` previously
        // meant a CC-bound fader could move the audio but leave the
        // on-screen slider frozen in Off mode.
        {
            const float live    = track.strip.liveFaderDb.load (std::memory_order_relaxed);
            const bool  touched = track.strip.faderTouched.load (std::memory_order_relaxed);
            if (! touched && std::abs (live - displayedLiveFaderDb) > 0.05f)
            {
                displayedLiveFaderDb = live;
                faderSlider.setValue (live, juce::dontSendNotification);
                // dontSendNotification skips onValueChange, which is what
                // refreshes the standalone dB readout — do it explicitly so an
                // external fader move (MIDI binding / MCU / automation) updates
                // the value shown under the fader, not just the cap position.
                refreshFaderValueLabel();
            }
            else if (touched)
            {
                displayedLiveFaderDb = live;
            }

            const bool capturing = playing && (isWrite || (isTouch && touched));
            if (capturing)
                captureWritePoint (AutomationParam::FaderDb,
                                    track.strip.faderDb.load (std::memory_order_relaxed));
        }

        // Pan animate / capture. Same shape as fader; threshold is in
        // pan units (-1..+1), so 0.005 = 0.25 % of the knob's travel,
        // well below visible.
        {
            const float live    = track.strip.livePan.load (std::memory_order_relaxed);
            const bool  touched = track.strip.panTouched.load (std::memory_order_relaxed);
            if (! touched && std::abs (live - displayedLivePan) > 0.005f)
            {
                displayedLivePan = live;
                panKnob.setValue (live, juce::dontSendNotification);
            }
            else if (touched)
            {
                displayedLivePan = live;
            }

            const bool capturing = playing && (isWrite || (isTouch && touched));
            if (capturing)
                captureWritePoint (AutomationParam::Pan,
                                    track.strip.pan.load (std::memory_order_relaxed));
        }

        // Mute / Solo - discrete params. In Off / Write modes the button
        // is authoritative — read the manual atom so we don't race
        // against the audio thread's 1-block-stale liveSolo / liveMute
        // (which used to re-light the button right after the user
        // clicked, making solo / mute look "stuck"). Only Read / Touch
        // need the live atom because there the audio engine drives the
        // value from the automation lane.
        const int amodeForSyncs = track.automationMode.load (std::memory_order_relaxed);
        const bool laneDrivesMuteSolo =
               amodeForSyncs == (int) AutomationMode::Read
            || amodeForSyncs == (int) AutomationMode::Touch;
        {
            const bool effective = laneDrivesMuteSolo
                ? track.strip.liveMute.load (std::memory_order_relaxed)
                : track.strip.mute    .load (std::memory_order_relaxed);
            if (muteButton.getToggleState() != effective)
                muteButton.setToggleState (effective, juce::dontSendNotification);
        }
        {
            const bool effective = laneDrivesMuteSolo
                ? track.strip.liveSolo.load (std::memory_order_relaxed)
                : track.strip.solo    .load (std::memory_order_relaxed);
            if (soloButton.getToggleState() != effective)
                soloButton.setToggleState (effective, juce::dontSendNotification);
        }
        // ARM has no Live atom (no automation lane) — read recordArmed
        // directly. Needed so a MIDI-bound arm toggle reflects on screen.
        {
            const bool armed = track.recordArmed.load (std::memory_order_relaxed);
            if (armButton.getToggleState() != armed)
                armButton.setToggleState (armed, juce::dontSendNotification);
        }

        // Aux sends - animate + capture in lockstep with fader / pan.
        // Threshold 0.1 dB on a -60..+6 dB knob (~0.15 % of travel).
        // Knob is null when the strip isn't in mixing mode (visible).
        // Same touched-only gate as fader/pan so MIDI-bound sends move
        // the on-screen knob regardless of automation mode.
        for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
        {
            const float live    = track.strip.liveAuxSendDb[(size_t) i].load (std::memory_order_relaxed);
            const bool  touched = track.strip.auxSendTouched[(size_t) i].load (std::memory_order_relaxed);
            if (! touched && std::abs (live - displayedLiveAuxSendDb[(size_t) i]) > 0.1f)
            {
                displayedLiveAuxSendDb[(size_t) i] = live;
                if (auto* knob = auxKnobs[(size_t) i].get())
                    knob->setValue (live, juce::dontSendNotification);
                refreshAuxSendLabel (i);   // keep the dB text in step with the live knob
            }
            else if (touched)
            {
                displayedLiveAuxSendDb[(size_t) i] = live;
            }

            // External pre/post flips (TrackAuxSendPrePost MIDI binding,
            // undo, session reload) update the atom without touching the
            // UI — poll it here so the " PRE" label suffix + amber
            // outline ring stay in sync without needing a strip rebuild.
            const bool curPre = track.strip.auxSendPreFader[(size_t) i]
                                      .load (std::memory_order_relaxed);
            if (curPre != displayedAuxPreFader[(size_t) i])
            {
                displayedAuxPreFader[(size_t) i] = curPre;
                refreshAuxSendLabel (i);
            }

            const bool capturing = playing && (isWrite || (isTouch && touched));
            if (capturing)
            {
                const auto param = (AutomationParam) ((int) AutomationParam::AuxSend1 + i);
                captureWritePoint (param,
                                    track.strip.auxSendDb[(size_t) i].load (std::memory_order_relaxed));
            }
        }
    }
}

void ChannelStripComponent::captureWritePoint (AutomationParam param, float denormValue)
{
    // Convert denormalized value back to lane storage (0..1). Mirrors
    // Session.cpp's denormalizeAutomation - kept here as a small switch
    // because there are only two callers (this and a future Touch hook)
    // and a free function isn't worth the noise.
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
            case AutomationParam::Pan:
                return juce::jlimit (0.0f, 1.0f, (v + 1.0f) * 0.5f);
            case AutomationParam::Mute:
            case AutomationParam::Solo:
                return v >= 0.5f ? 1.0f : 0.0f;
            case AutomationParam::AuxSend1:
            case AutomationParam::AuxSend2:
            case AutomationParam::AuxSend3:
            case AutomationParam::AuxSend4:
            {
                if (v <= ChannelStripParams::kAuxSendOffDb) return 0.0f;
                const float lo = ChannelStripParams::kAuxSendMinDb;
                const float hi = ChannelStripParams::kAuxSendMaxDb;
                return juce::jlimit (0.0f, 1.0f, (v - lo) / (hi - lo));
            }
            case AutomationParam::kCount: break;
        }
        return 0.0f;
    };

    auto& lane = track.automationLanes[(size_t) param].mutableForWritePass();  // in-place; audio does not read this lane in Write/Touch-touched
    AutomationPoint pt;
    pt.timeSamples   = engine.getTransport().getPlayhead();
    pt.value         = normalize (param, denormValue);
    pt.recordedAtBPM = session.tempoBpm.load (std::memory_order_relaxed);

    // Pre-filter: drop near-identical samples close together in time
    // (the timer fires every 33 ms; if the fader hasn't moved, those
    // ticks are noise). Continuous params only — discrete state needs
    // every transition. Spec: delta + max-span pre-filter. 0.001
    // normalized = 0.1% of the lane's storage range. The
    // pt.timeSamples >= last.timeSamples guard prevents a backward
    // playhead jump (loop wrap / transport rewind) from sliding under
    // the short-span cutoff and skipping the future-tail truncation
    // block below.
    if (isContinuousParam (param) && ! lane.empty())
    {
        constexpr float kDeltaEps = 0.001f;
        constexpr juce::int64 kMaxSpanSamples = 22050;   // ~500 ms @ 44.1 k
        const auto& last = lane.back();
        if (std::abs (pt.value - last.value) < kDeltaEps
            && pt.timeSamples >= last.timeSamples
            && (pt.timeSamples - last.timeSamples) < kMaxSpanSamples)
            return;
    }

    // Coalesce: if the most recent point is at the same timeline sample
    // (or earlier), replace its value (don't append). This handles two
    // cases: (a) Timer fires faster than transport advances (paused?
    // shouldn't happen since we gated on isPlaying), (b) Time-travel
    // backward via loop wraparound mid-Write -- subsequent appends
    // belong AFTER the most recent timeline position, not before it.
    // Strict ascending invariant is required by evaluateLane's binary
    // search.
    if (! lane.empty() && lane.back().timeSamples >= pt.timeSamples)
    {
        // Loop wraparound case: drop the rest of the lane that's now in
        // the future relative to playhead, so the binary-search invariant
        // (sorted ascending) holds and the upcoming Write captures land
        // in their natural order. Discrete params (mute / solo) keep
        // the same rule.
        if (lane.back().timeSamples > pt.timeSamples)
        {
            auto cutoff = std::lower_bound (lane.begin(), lane.end(),
                pt.timeSamples,
                [] (const AutomationPoint& a, juce::int64 t) { return a.timeSamples < t; });
            lane.erase (cutoff, lane.end());
        }
        // Same-sample replace: keep the latest value at this exact sample.
        if (! lane.empty() && lane.back().timeSamples == pt.timeSamples)
        {
            lane.back() = pt;
            return;
        }
    }
    lane.push_back (pt);
}

void ChannelStripComponent::showAutoModeMenu()
{
    const int cur = track.automationMode.load (std::memory_order_relaxed);
    juce::PopupMenu menu;
    menu.addItem (1, "Off",   true, cur == (int) AutomationMode::Off);
    menu.addItem (2, "Read",  true, cur == (int) AutomationMode::Read);
    menu.addItem (3, "Write", true, cur == (int) AutomationMode::Write);
    menu.addItem (4, "Touch", true, cur == (int) AutomationMode::Touch);
    showContextMenu (menu, autoModeButton,
        [safeThis = juce::Component::SafePointer<ChannelStripComponent> (this)]
        (int chosen)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr || chosen < 1 || chosen > 4) return;
            const AutomationMode picked[] = {
                AutomationMode::Off, AutomationMode::Read,
                AutomationMode::Write, AutomationMode::Touch
            };
            self->setAutoMode (picked[chosen - 1]);
        });
}

void ChannelStripComponent::setAutoMode (AutomationMode mode)
{
    // When transitioning OUT of Write or Touch, the points just appended
    // to the lane need to be visible to the audio thread BEFORE it starts
    // reading from the lane. The release-store on mode synchronizes those
    // writes - any prior append to lane happens-before the audio
    // thread's acquire-load of the new mode.
    //
    // Auto-thin on mode-flip would be tempting here, but the existing
    // concurrency model partitions lane reads/writes by (mode, touched)
    // and handleWritePassComplete rewrites lane unconditionally —
    // there's no safe ordering relative to the audio thread acquiring
    // the new mode (or to OTHER strips that are in Read). Thinning needs
    // AtomicSnapshot on each lane before it can fire on mode-flip; for
    // now the capture-time pre-filter handles the worst bloat and the
    // safe RDP entry point is File ▸ Optimize automation, which gates
    // on transport-stopped + every strip's mode set to Off.
    track.automationMode.store ((int) mode, std::memory_order_release);

    // Read mode disables every automated control (spec: "User cannot
    // override"). Off / Write / Touch leave them interactive so the
    // user can either ride them (Write) or grab to override (Touch).
    const bool interactive = mode != AutomationMode::Read;
    faderSlider.setEnabled (interactive);
    panKnob    .setEnabled (interactive);
    muteButton .setEnabled (interactive);
    soloButton .setEnabled (interactive);
    for (auto& knob : auxKnobs)
        if (knob != nullptr) knob->setEnabled (interactive);

    refreshAutoModeButton();
}

void ChannelStripComponent::refreshAutoModeButton()
{
    const int m = track.automationMode.load (std::memory_order_relaxed);
    const char* label = "OFF";
    // Off state has no fill — reads as a plain text label so the bottom
    // of the strip doesn't get crowded with a boxed "OFF" pill. Armed
    // modes keep their semantic colours (green/red/amber) since the
    // fill IS the indicator that automation is live.
    juce::Colour bg = juce::Colours::transparentBlack;
    juce::Colour fg (0xff707074);
    switch (m)
    {
        case (int) AutomationMode::Read:
            label = "READ";
            bg = juce::Colour (0xff20603a);   // muted green
            fg = juce::Colour (0xffd0e8d0);
            break;
        case (int) AutomationMode::Write:
            label = "WRITE";
            bg = juce::Colour (0xff803020);   // muted red - 3c-ii
            fg = juce::Colour (0xfff0d0c8);
            break;
        case (int) AutomationMode::Touch:
            label = "TOUCH";
            bg = juce::Colour (0xff806020);   // muted amber - 3c-ii
            fg = juce::Colour (0xfff0e0c0);
            break;
        case (int) AutomationMode::Off:
        default: break;
    }
    autoModeButton.setButtonText (label);
    autoModeButton.setColour (juce::TextButton::buttonColourId, bg);
    autoModeButton.setColour (juce::TextButton::textColourOffId, fg);
}

void ChannelStripComponent::mouseDown (const juce::MouseEvent& e)
{
    // Any click on the strip (background pixels - children consume their
    // own mouse first) puts the focus on this track so keyboard shortcuts
    // (A / S / X) target it. Fires on left AND right clicks because the
    // user reasonably expects the right-click colour-menu to ALSO have
    // selected the track.
    if (onTrackFocusRequested) onTrackFocusRequested (trackIndex);

    // Right-click on a specific child surface routes to the MIDI Learn
    // menu for that target. The route is gated on eventComponent so a
    // hit on the strip background still falls through to the colour menu.
    if (e.mods.isPopupMenu())
    {
        if (e.eventComponent == &faderSlider)
        {
            midilearn::showLearnMenu (faderSlider, session,
                                        MidiBindingTarget::TrackFader, trackIndex);
            return;
        }
        if (e.eventComponent == &muteButton)
        {
            midilearn::showLearnMenu (muteButton, session,
                                        MidiBindingTarget::TrackMute, trackIndex);
            return;
        }
        if (e.eventComponent == &soloButton)
        {
            midilearn::showLearnMenu (soloButton, session,
                                        MidiBindingTarget::TrackSolo, trackIndex);
            return;
        }
        if (e.eventComponent == &armButton)
        {
            midilearn::showLearnMenu (armButton, session,
                                        MidiBindingTarget::TrackArm, trackIndex);
            return;
        }
        // Right-click on any aux knob -> custom popup combining the
        // PRE/POST toggle with the existing MIDI Learn entries. Two
        // distinct Learn entries: one for the send LEVEL (TrackAux
        // Send) and one for the pre/post TOGGLE (TrackAuxSendPrePost),
        // so a user can map a knob to the level AND a footswitch to
        // the pre/post flip on the same aux.
        for (int i = 0; i < (int) auxKnobs.size(); ++i)
        {
            if (auxKnobs[(size_t) i] != nullptr
                && e.eventComponent == auxKnobs[(size_t) i].get())
            {
                const int packed = packTrackAux (trackIndex, i);
                const bool isPre = track.strip.auxSendPreFader[(size_t) i]
                                       .load (std::memory_order_relaxed);

                // SafePointer captures: the menu's item lambdas can fire
                // after the strip is destroyed (e.g. mode flip rebuilds
                // ConsoleView with the popup still open). Capture a
                // SafePointer<ChannelStripComponent> instead of raw
                // `this` so each lambda no-ops cleanly if the strip is
                // gone — without these guards the lambdas would
                // dereference `track`, `session`, `auxKnobs`, etc. on
                // freed memory.
                juce::Component::SafePointer<ChannelStripComponent> safeThis (this);
                juce::Component::SafePointer<juce::Slider> safeKnob (auxKnobs[(size_t) i].get());

                juce::PopupMenu m;
                m.addSectionHeader ("Track " + juce::String (trackIndex + 1)
                                       + " AUX " + juce::String (i + 1) + " send");

                // The toggle: shows the CURRENT state in the label so
                // the user knows what clicking will do.
                const juce::String toggleLabel = isPre ? "Switch to POST-fader"
                                                       : "Switch to PRE-fader";
                m.addItem (toggleLabel, [safeThis, i]
                {
                    auto* self = safeThis.getComponent();
                    if (self == nullptr) return;
                    auto& a = self->track.strip.auxSendPreFader[(size_t) i];
                    const bool nowPre = ! a.load (std::memory_order_relaxed);
                    a.store (nowPre, std::memory_order_relaxed);
                    self->refreshAuxSendLabel (i);
                });

                m.addItem ("Reset to off", [safeKnob]
                {
                    if (auto* k = safeKnob.getComponent())
                        k->setValue (ChannelStripParams::kAuxSendOffDb,
                                      juce::sendNotificationSync);
                });

                m.addSeparator();

                m.addItem ("MIDI Learn level...", [safeThis, safeKnob, packed]
                {
                    auto* self = safeThis.getComponent();
                    auto* k    = safeKnob.getComponent();
                    if (self == nullptr || k == nullptr) return;
                    midilearn::showLearnMenu (*k, self->session,
                                                 MidiBindingTarget::TrackAuxSend,
                                                 packed);
                });
                m.addItem ("MIDI Learn pre/post...", [safeThis, safeKnob, packed]
                {
                    auto* self = safeThis.getComponent();
                    auto* k    = safeKnob.getComponent();
                    if (self == nullptr || k == nullptr) return;
                    midilearn::showLearnMenu (*k, self->session,
                                                 MidiBindingTarget::TrackAuxSendPrePost,
                                                 packed);
                });

                duskstudio::showContextMenu (m, *auxKnobs[(size_t) i]);
                return;
            }
        }
        // HPF + EQ band gains. Same eventComponent-match shape as the
        // other strip controls. Freq + Q knobs aren't bindable in v1 -
        // gain is the most-automated EQ knob in practice.
        if (e.eventComponent == &hpfKnob)
        {
            midilearn::showLearnMenu (hpfKnob, session,
                                        MidiBindingTarget::TrackHpfFreq, trackIndex);
            return;
        }
        for (int i = 0; i < (int) eqRows.size(); ++i)
        {
            auto& row = eqRows[(size_t) i];
            if (row.gain != nullptr && e.eventComponent == row.gain.get())
            {
                midilearn::showLearnMenu (*row.gain, session,
                                            MidiBindingTarget::TrackEqGain,
                                            packTrackEqBand (trackIndex, i));
                return;
            }
            if (row.freq != nullptr && e.eventComponent == row.freq.get())
            {
                midilearn::showLearnMenu (*row.freq, session,
                                            MidiBindingTarget::TrackEqFreq,
                                            packTrackEqBand (trackIndex, i));
                return;
            }
            if (row.q != nullptr && e.eventComponent == row.q.get())
            {
                midilearn::showLearnMenu (*row.q, session,
                                            MidiBindingTarget::TrackEqQ,
                                            packTrackEqBand (trackIndex, i));
                return;
            }
        }
        // Comp threshold/makeup: each per-mode knob routes to the LOGICAL
        // target so the binding survives mode swaps (audio thread reads
        // compMode and writes to the matching atom). VCA threshold has
        // no dedicated knob in the UI (it lives on the GR-strip drag
        // handle); right-clicking the Opto/FET threshold knob binds it
        // for all modes.
        if (e.eventComponent == &optoPeakRedKnob
            || e.eventComponent == &fetInputKnob)
        {
            auto& src = (e.eventComponent == &optoPeakRedKnob)
                            ? optoPeakRedKnob : fetInputKnob;
            midilearn::showLearnMenu (src, session,
                                        MidiBindingTarget::TrackCompThresh, trackIndex);
            return;
        }
        if (e.eventComponent == &optoGainKnob
            || e.eventComponent == &fetOutputKnob
            || e.eventComponent == &vcaOutputKnob)
        {
            auto& src = (e.eventComponent == &optoGainKnob)
                            ? optoGainKnob
                            : (e.eventComponent == &fetOutputKnob ? fetOutputKnob
                                                                    : vcaOutputKnob);
            midilearn::showLearnMenu (src, session,
                                        MidiBindingTarget::TrackCompMakeup, trackIndex);
            return;
        }
    }

    // Right-click anywhere on the strip body opens the colour menu. Children
    // (sliders, buttons, labels) consume their own mouse events first, so this
    // only fires on background pixels - exactly the affordance we want.
    if (e.mods.isPopupMenu())
        showColourMenu();
    else
        Component::mouseDown (e);
}

void ChannelStripComponent::applyTrackColour (juce::Colour c)
{
    if (track.colour == c) return;
    const auto oldColour = track.colour;
    auto& um = engine.getUndoManager();
    um.beginNewTransaction ("Track colour");
    um.perform (new ParamEditAction (
        [&s = session, idx = trackIndex, c]         { s.track (idx).colour = c; },
        [&s = session, idx = trackIndex, oldColour] { s.track (idx).colour = oldColour; }));
    repaint();
}

void ChannelStripComponent::showColourMenu()
{
    // Eight 4K-palette presets for fast picking, plus a "Custom…" entry that
    // pops a JUCE ColourSelector for fine-grained choice.
    const std::pair<const char*, juce::uint32> presets[] = {
        { "Red",        fourKColors::kHfRed     },
        { "Orange",     fourKColors::kHmOrange  },
        { "Amber",      fourKColors::kLmAmber   },
        { "Green",      fourKColors::kLfGreen   },
        { "Cyan",       fourKColors::kPanCyan   },
        { "Blue",       fourKColors::kHpfBlue   },
        { "Purple",     fourKColors::kSendPurple},
        { "Tan",        fourKColors::kMasterTan },
    };

    juce::PopupMenu menu;
    menu.addSectionHeader ("Track colour");
    for (size_t i = 0; i < std::size (presets); ++i)
    {
        juce::PopupMenu::Item item;
        item.itemID = (int) (i + 1);
        item.text = presets[i].first;
        item.colour = juce::Colour (presets[i].second);
        menu.addItem (item);
    }
    menu.addSeparator();
    menu.addItem (1001, "Rename track...");

    // Plugin slot menu items, only meaningful when a plugin is loaded.
    // Replace/Remove live here (rather than on the slot button itself) so
    // the slot button's primary click stays as a toggle for the editor.
    if (pluginSlot.isLoaded()
        || engine.getChannelStrip (trackIndex).isNativeClapLoaded()
        || engine.getChannelStrip (trackIndex).isNativeLv2Loaded()
        || engine.getChannelStrip (trackIndex).isNativeVst3Loaded())
    {
        menu.addSeparator();
        menu.addItem (1010, "Replace plugin...");
        menu.addItem (1011, "Remove plugin");
        if (pluginSlot.wasCrashed())
            menu.addItem (1012, "Re-enable plugin (crashed)");
        else if (pluginSlot.wasAutoBypassed())
            menu.addItem (1012, "Re-enable plugin (auto-bypassed)");
    }

    // Clone-to submenu. IDs 2000 + dest-index target the destination
    // track. The action class CAPTURES the dest's prior state at first
    // perform() so the user gets clean undo even when overwriting a
    // populated track.
    menu.addSeparator();
    juce::PopupMenu cloneMenu;
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        if (t == trackIndex) continue;
        const auto& destTrack = session.track (t);
        const auto label = juce::String (t + 1) + ": "
            + (destTrack.name.isNotEmpty() ? destTrack.name : juce::String());
        cloneMenu.addItem (2000 + t, label);
    }
    menu.addSubMenu ("Clone to track...", cloneMenu);

    // Fader group submenu. 8 group slots is plenty for a 16-channel
    // console (max parallel groups in practice is 4-5: drums, guitars,
    // synths, vocals, BG). IDs 2100 + group-id; 2100 = ungrouped.
    juce::PopupMenu groupMenu;
    const int currentGroup = track.strip.faderGroupId.load (std::memory_order_relaxed);
    groupMenu.addItem (2100, "None", true, currentGroup == 0);
    for (int g = 1; g <= 8; ++g)
        groupMenu.addItem (2100 + g, "Group " + juce::String (g),
                            true, currentGroup == g);
    menu.addSubMenu ("Fader group...", groupMenu);

    juce::Component::SafePointer<ChannelStripComponent> safe (this);
    // Copy the presets into a std::vector so the async callback owns its own
    // copy (the local C-array on the stack is gone by the time the menu fires).
    std::vector<std::pair<juce::String, juce::uint32>> presetCopy;
    presetCopy.reserve (std::size (presets));
    for (auto& p : presets) presetCopy.emplace_back (juce::String (p.first), p.second);

    showContextMenu (menu, *this,
        [safe, presetCopy] (int result)
        {
            if (result <= 0) return;
            auto* self = safe.getComponent();
            if (self == nullptr) return;
            if (result == 1001)
            {
                self->nameLabel.showEditor();
                return;
            }
            if (result == 1010) { self->openPluginPicker (/*useChooser*/ false); return; }
            if (result == 1011) { self->unloadPluginSlot();      return; }
            if (result == 1012) { self->pluginSlot.clearAutoBypass(); return; }
            if (result >= 2000 && result < 2000 + Session::kNumTracks)
            {
                const int dest = result - 2000;
                auto& um = self->engine.getUndoManager();
                um.beginNewTransaction ("Clone track");
                um.perform (new CloneTrackAction (self->session, self->engine,
                                                    self->trackIndex, dest));
                return;
            }
            if (result >= 2100 && result <= 2108)
            {
                const int gid = result - 2100;   // 0 = ungrouped, 1..8
                self->track.strip.faderGroupId.store (gid, std::memory_order_relaxed);
                self->resized();   // reserve / release the group-chip slot now
                self->repaint();
                return;
            }
            const int idx = result - 1;
            if (idx >= 0 && idx < (int) presetCopy.size())
                self->applyTrackColour (juce::Colour (presetCopy[(size_t) idx].second));
        });
}

void ChannelStripComponent::onInputSelectorChanged()
{
    const int id = inputSelector.getSelectedId();
    int src = -2;  // follow track index
    if      (id == 1)            src = -2;
    else if (id == 2)            src = -1;
    else if (id >= 100)          src = id - 100;
    track.inputSource.store (src, std::memory_order_relaxed);
    refreshIoConfigButton();
}

void ChannelStripComponent::onTrackModeChanged()
{
    // ComboBox IDs: 1 = Mono, 2 = Stereo, 3 = MIDI. Stored as int on the
    // Track so the audio thread can read it lock-free.
    const int id = modeSelector.getSelectedId();
    const int mode = juce::jlimit (0, 2, id - 1);  // 0..2 = Track::Mode

    // A frozen track is locked to its current mode — its baked WAV + bypassed
    // DSP assume it. Block ANY change (restoring the selector to the track's
    // real mode) until the user unfreezes. Audio tracks freeze too now, so this
    // must restore Mono/Stereo, not hardcode MIDI.
    const int currentMode = track.mode.load (std::memory_order_relaxed);
    if (track.frozen.load (std::memory_order_relaxed) && mode != currentMode)
    {
        modeSelector.setSelectedId (currentMode + 1, juce::dontSendNotification);
        showDuskAlert (*this, "Track is frozen",
                        "Unfreeze this track before changing its mode.");
        return;
    }

    // Auto-unload a mode-mismatched plugin: the picker filter prevents
    // loading the wrong type in the first place, but flipping a track's
    // mode after-the-fact bypasses that gate and would leave the strip
    // with a plugin that's silent (effect on a MIDI strip ignores MIDI
    // and processes silence; instrument on an audio strip ignores audio
    // input). Unloading here keeps the rule consistent and avoids the
    // confusing-silence trap. Editor goes with the slot via
    // unloadPluginSlot, which closes the modal first.
    if (pluginSlot.isLoaded())
    {
        const bool willBeMidi   = (mode == (int) Track::Mode::Midi);
        const bool isInstrument = pluginSlot.isLoadedPluginInstrument();
        if (willBeMidi != isInstrument)
            unloadPluginSlot();
    }
    else if ((engine.getChannelStrip (trackIndex).isNativeClapLoaded()
              || engine.getChannelStrip (trackIndex).isNativeLv2Loaded()
              || engine.getChannelStrip (trackIndex).isNativeVst3Loaded())
             && mode == (int) Track::Mode::Midi)
    {
        // Native inserts are effects-only; a MIDI strip can't host one.
        unloadPluginSlot();
    }

    track.mode.store (mode, std::memory_order_relaxed);
    refreshInputSelectorVisibility();
    refreshPluginSlotButton();
    refreshIoConfigButton();
    refreshPrintButtonForMode();
    // If the I/O popup is open it needs to grow / shrink (rows differ per
    // mode: 2 for audio, 4 for MIDI), then re-centre — EmbeddedModal centres
    // only at show() time, so a plain setSize would leave the grown MIDI panel
    // anchored off-centre with its extra rows clipped. resized() runs
    // unconditionally after setSize because mono ↔ stereo both use rows == 2
    // (no height change → setSize no-ops), yet the inner layout still differs
    // (full-width mono input vs L/R halves) and must re-lay-out.
    if (ioConfigModal.isOpen())
    {
        if (auto* body = ioConfigModal.getBody())
        {
            const int rows = (mode == 2) ? 4 : 2;
            body->setSize (240, 8 + rows * 24 + (rows - 1) * 6 + 8);
            body->resized();
            ioConfigModal.recenterBody();
        }
    }
    // Resize so the layout reflects the new mode (extra dropdown for stereo,
    // hidden audio dropdown for MIDI).
    resized();
    repaint();
}

void ChannelStripComponent::MidiActivityLed::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat().reduced (1.5f);
    if (area.isEmpty()) return;
    // Rim + body. Lit = bright green; dim = dark green so the LED is always
    // visible (the user knows the LED exists even when no MIDI is flowing).
    const juce::Colour rim  (0xff202024);
    const juce::Colour off  (0xff2a4a30);
    const juce::Colour onG  (0xff7afb8a);
    g.setColour (rim);
    g.drawEllipse (area, 1.0f);
    g.setColour (lit ? onG : off);
    g.fillEllipse (area.reduced (1.5f));
}

void ChannelStripComponent::refreshInputSelectorVisibility()
{
    const int mode = juce::jlimit (0, 2, track.mode.load (std::memory_order_relaxed));
    const bool isMono   = (mode == 0);
    const bool isStereo = (mode == 1);
    const bool isMidi   = (mode == 2);

    // Tracking-stage selectors hide entirely when the strip is in
    // Mixing mode — without this gate, post-setMixingMode calls (track
    // mode change, plugin load triggering a re-layout) flip them back
    // on and leave stale rows above the EQ on MIDI / stereo strips.
    const bool trackingVisible = ! mixingMode;
    inputSelector      .setVisible (trackingVisible && (isMono || isStereo));
    inputSelectorR     .setVisible (trackingVisible && isStereo);
    midiInputSelector  .setVisible (trackingVisible && isMidi);
    midiChannelSelector.setVisible (trackingVisible && isMidi);
    midiActivityLed    .setVisible (trackingVisible && isMidi);
    midiOutputSelector .setVisible (trackingVisible && isMidi);
}

void ChannelStripComponent::onHpfKnobChanged()
{
    const float freq = (float) hpfKnob.getValue();
    track.strip.hpfFreq.store (freq, std::memory_order_relaxed);
    // Bypass the HPF DSP entirely when the knob is at the floor - saves
    // 16 channels worth of biquad cost when nobody's using HPF.
    const bool hpfOn = freq > ChannelStripParams::kHpfOffHz + 0.5f;
    track.strip.hpfEnabled.store (hpfOn, std::memory_order_relaxed);
    // Auto-arm the EQ header LED whenever the HPF is engaged — band-
    // knob touches do the same via the EQ rows' onValueChange paths.
    if (hpfOn)
        track.strip.eqEnabled.store (true, std::memory_order_release);
}

void ChannelStripComponent::onLpfKnobChanged()
{
    const float freq = (float) lpfKnob.getValue();
    track.strip.lpfFreq.store (freq, std::memory_order_relaxed);
    // At max → bypass the LPF DSP. Same per-channel cost optimisation
    // as HPF — donor BritishEQProcessor skips its LPF biquad when the
    // enabled flag is false.
    const bool lpfOn = freq < ChannelStripParams::kLpfOffHz - 0.5f;
    track.strip.lpfEnabled.store (lpfOn, std::memory_order_relaxed);
    if (lpfOn)
        track.strip.eqEnabled.store (true, std::memory_order_release);
}

void ChannelStripComponent::setCompMode (int modeIndex)
{
    track.strip.compMode.store (modeIndex, std::memory_order_relaxed);
    // First explicit pick locks the button into showing the mode name.
    track.strip.compModePicked.store (true, std::memory_order_relaxed);
    refreshCompModeButtonState();
    refreshCompKnobVisibility();
    resized();  // mode swap reshapes the comp body
}

void ChannelStripComponent::refreshCompModeButtonState()
{
    if (compModeButton == nullptr) return;
    // Label always carries "COMP" so the section reads as a compressor at a
    // glance whatever its state: disabled → "COMP"; enabled → "COMP - <mode>"
    // (OPTO / FET / VCA) so the active topology is explicit without right-
    // clicking the mode menu. The LED on the left of the pill also shows
    // engaged state.
    const bool enabled = track.strip.compEnabled.load (std::memory_order_relaxed);
    const int m = juce::jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    const char* names[] = { "OPTO", "FET", "VCA" };
    compModeButton->setLabelText (enabled ? juce::String ("COMP - ") + names[m]
                                          : juce::String ("COMP"));
    compModeButton->repaint();   // refresh LED state too
}

void ChannelStripComponent::refreshCompKnobVisibility()
{
    // In compact mode the entire COMP section is hidden (replaced by the
    // compCompactButton). Bail before touching per-mode knob visibility.
    if (compactMode) return;

    const int m = juce::jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    const bool isOpto = (m == 0), isFet = (m == 1), isVca = (m == 2);

    // OPTO peak-reduction + FET input + VCA threshold are all set via
    // the triangle drag handle on the GR meter strip. No "main amount"
    // knob is shown for any mode — the per-mode knobs in the right-side
    // grid are the remaining mode-specific controls only.
    optoPeakRedKnob.setVisible (false);  optoPeakRedLabel.setVisible (false);
    fetInputKnob   .setVisible (false);  fetInputLabel   .setVisible (false);

    optoGainKnob   .setVisible (isOpto);  optoGainLabel   .setVisible (isOpto);
    optoLimitButton.setVisible (isOpto);

    fetOutputKnob  .setVisible (isFet);   fetOutputLabel  .setVisible (isFet);
    fetAttackKnob  .setVisible (isFet);   fetAttackLabel  .setVisible (isFet);
    fetReleaseKnob .setVisible (isFet);   fetReleaseLabel .setVisible (isFet);
    fetRatioKnob   .setVisible (isFet);   fetRatioLabel   .setVisible (isFet);

    vcaRatioKnob   .setVisible (isVca);   vcaRatioLabel   .setVisible (isVca);
    vcaAttackKnob  .setVisible (isVca);   vcaAttackLabel  .setVisible (isVca);
    vcaReleaseKnob .setVisible (isVca);   vcaReleaseLabel .setVisible (isVca);
    vcaOutputKnob  .setVisible (isVca);   vcaOutputLabel  .setVisible (isVca);
}

void ChannelStripComponent::showCompModeMenu()
{
    // Mode-only dropdown. Bypass lives on the round LED in the lower
    // left of the comp section — click that LED to toggle compEnabled.
    const int m = juce::jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    juce::PopupMenu menu;
    menu.addItem (2, "OPTO - program-dependent, smooth", true, m == 0);
    menu.addItem (3, "FET - fast attack, gritty under load", true, m == 1);
    menu.addItem (4, "VCA - clean, predictable", true, m == 2);
    showContextMenu (menu, *compModeButton,
        [safeThis = juce::Component::SafePointer<ChannelStripComponent> (this)]
        (int chosen)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr) return;
            if (chosen >= 2 && chosen <= 4)
                self->setCompMode (chosen - 2);
        });
}

void ChannelStripComponent::showEqTypeMenu()
{
    // EQ type picker — right-click on the EQ header button. Mirrors
    // showCompModeMenu's grammar (ticked current choice). Two types
    // for the British EQ donor: Brown (E-series) vs Black (G-series).
    const bool isBlack = track.strip.eqBlackMode.load (std::memory_order_relaxed);
    juce::PopupMenu menu;
    menu.addItem (1, "Brown - E-series, classic",   true, ! isBlack);
    menu.addItem (2, "Black - G-series, modern",    true,   isBlack);
    showContextMenu (menu, *eqHeaderBtn,
        [safeThis = juce::Component::SafePointer<ChannelStripComponent> (this)]
        (int chosen)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr) return;
            if (chosen == 1 || chosen == 2)
            {
                self->track.strip.eqBlackMode.store (chosen == 2, std::memory_order_relaxed);
                if (self->eqHeaderBtn != nullptr) self->eqHeaderBtn->repaint();
            }
        });
}

void ChannelStripComponent::setCompEnabled (bool enabled)
{
    track.strip.compEnabled.store (enabled, std::memory_order_relaxed);
    refreshCompModeButtonState();   // only the lit state changed; knobs unchanged
}

void ChannelStripComponent::armCompOnUserEdit()
{
    if (! track.strip.compEnabled.load (std::memory_order_relaxed))
    {
        track.strip.compEnabled.store (true, std::memory_order_relaxed);
        refreshCompModeButtonState();
    }
}

static void drawSectionPlaceholder (juce::Graphics& g, juce::Rectangle<int> r,
                                    const juce::String& label, juce::Colour accent)
{
    if (r.isEmpty()) return;
    g.setColour (juce::Colour (0xff222226));
    g.fillRoundedRectangle (r.toFloat(), 3.0f);
    g.setColour (accent.withAlpha (0.45f));
    g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 3.0f, 0.8f);
    g.setColour (accent.withAlpha (0.85f));
    g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
    g.drawText (label, r.reduced (4, 2), juce::Justification::centredTop, false);
}

namespace
{
// 8 distinct hues for the fader-group chips so grouped strips read as a set.
juce::Colour groupColour (int gid)
{
    const float hue = (float) ((gid - 1) % 8) / 8.0f;   // gid is 1..8
    return juce::Colour::fromHSV (hue, 0.65f, 0.95f, 1.0f);
}
} // namespace

void ChannelStripComponent::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.5f);
    g.setColour (juce::Colour (0xff1a1a1c));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (track.colour.withAlpha (0.85f));
    g.fillRoundedRectangle (r.removeFromTop (4.0f), 2.0f);
    g.setColour (juce::Colour (0xff2a2a2e));
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 4.0f, 1.0f);

    // Fader-group chip in the name row's right edge (resized() reserves the
    // space). Filled for the group master (lowest member index), outlined for
    // the rest, so grouped strips read as a set and the master is obvious.
    if (! groupChipBounds.isEmpty())
    {
        const int gid = track.strip.faderGroupId.load (std::memory_order_relaxed);
        if (gid != 0)
        {
            const bool isMaster = (groupMasterIndex() == trackIndex);
            const auto chip = groupChipBounds.toFloat();
            const auto col  = groupColour (gid);
            if (isMaster)
            {
                g.setColour (col);
                g.fillRoundedRectangle (chip, 3.0f);
            }
            else
            {
                g.setColour (col.withAlpha (0.18f));
                g.fillRoundedRectangle (chip, 3.0f);
                g.setColour (col);
                g.drawRoundedRectangle (chip.reduced (0.5f), 3.0f, 1.0f);
            }
            g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
            g.setColour (isMaster ? juce::Colours::black.withAlpha (0.85f) : col);
            g.drawText ("G" + juce::String (gid), groupChipBounds,
                        juce::Justification::centred, false);
        }
    }

    // EQ region: full background (header + HPF row + 4 band rows)
    if (! eqArea.isEmpty())
    {
        g.setColour (juce::Colour (0xff1f231e));
        g.fillRoundedRectangle (eqArea.toFloat(), 3.0f);
        g.setColour (juce::Colour (0xff80c090).withAlpha (0.40f));
        g.drawRoundedRectangle (eqArea.toFloat().reduced (0.5f), 3.0f, 0.8f);
    }

    if (! compArea.isEmpty())
    {
        g.setColour (juce::Colour (0xff241f1c));
        g.fillRoundedRectangle (compArea.toFloat(), 3.0f);
        g.setColour (juce::Colour (fourKColors::kCompGold).withAlpha (0.40f));
        g.drawRoundedRectangle (compArea.toFloat().reduced (0.5f), 3.0f, 0.8f);
    }

    // SEND box (Mixing stage only) - same framed-block shape as EQ/COMP
    // with the AUX purple accent so the row reads as a coherent section
    // instead of floating loose knobs above PAN.
    if (! auxRowArea.isEmpty())
    {
        g.setColour (juce::Colour (0xff1f1d24));
        g.fillRoundedRectangle (auxRowArea.toFloat(), 3.0f);
        g.setColour (juce::Colour (0xff9080c0).withAlpha (0.40f));
        g.drawRoundedRectangle (auxRowArea.toFloat().reduced (0.5f), 3.0f, 0.8f);
    }

    // ── Channel input LED (next to the fader). Shows the pre-fader signal
    //    level so the engineer always sees what's hitting the strip.
    //    Threshold + GR meters live INSIDE the COMP section now, so this is
    //    just a clean input bar with a peak-hold tick.
    //
    // The LED's dB-to-y mapping uses the fader's NormalisableRange — same
    // skew, same range — so the meter's "0 dB" line sits at exactly the
    // same Y as the fader's "0" tick. Linear (-60..+6) was the prior
    // approach and put 0 dB at 91 % of bar height while the fader's "0"
    // tick (with SkewFactorFromMidPoint(-12) on a -100..+12 range) lived
    // at ~96 % — they read as two different scales for the same signal.
    const auto& faderRange = faderSlider.getNormalisableRange();
    auto dbToFrac = [&] (float db) {
        const double clamped = juce::jlimit ((double) faderRange.start,
                                              (double) faderRange.end,
                                              (double) db);
        return (float) juce::jlimit (0.0, 1.0,
                                      faderRange.convertTo0to1 (clamped));
    };

    if (! inputMeterArea.isEmpty())
    {
        const bool stereoMode = (track.mode.load (std::memory_order_relaxed)
                                  == (int) Track::Mode::Stereo);

        // Single bar (mono / midi) or split L|R bars (stereo) in the same
        // total meter footprint. Stereo halves the bar width so both fit
        // without expanding the meter column.
        auto drawBar = [&] (juce::Rectangle<float> bar, float dispDb, float peakDb)
        {
            g.setColour (juce::Colour (0xff060608));
            g.fillRoundedRectangle (bar, 1.5f);
            g.setColour (juce::Colour (0xff2a2a30));
            g.drawRoundedRectangle (bar, 1.5f, 0.5f);

            // LED-style colour zones — hard transitions at -5 dB (green ->
            // yellow) and +5 dB (yellow -> red). Bright saturated values
            // matching the reference image's hardware-LED look instead of
            // the prior soft gradient.
            const juce::Colour kLedGreen  (0xff20d040);
            const juce::Colour kLedYellow (0xfff0e020);
            const juce::Colour kLedRed    (0xffff2020);
            auto colourForDb = [&] (float db) -> juce::Colour
            {
                if (db >=  5.0f) return kLedRed;
                if (db >= -5.0f) return kLedYellow;
                return kLedGreen;
            };

            const float frac = dbToFrac (dispDb);
            if (frac > 0.001f)
            {
                const float fillH = (bar.getHeight() - 2.0f) * frac;
                const float x = bar.getX() + 1.0f;
                const float w = bar.getWidth() - 2.0f;
                const float y = bar.getBottom() - 1.0f - fillH;
                const auto fillRect = juce::Rectangle<float> (x, y, w, fillH);

                // Soft outer glow under the fill — tip-colour driven so the
                // glow shifts with the meter peak.
                const auto tipCol = colourForDb (dispDb);
                g.setColour (tipCol.withAlpha (0.20f));
                g.fillRect (fillRect.expanded (1.5f));
                g.setColour (tipCol.withAlpha (0.10f));
                g.fillRect (fillRect.expanded (3.0f));

                // Three hard zones stacked from the top of the fill down.
                // Each zone's pixel range = clip(filled vs zone boundary).
                const float yRedTop    = bar.getBottom() - 1.0f - dbToFrac ( 5.0f) * (bar.getHeight() - 2.0f);
                const float yYellowTop = bar.getBottom() - 1.0f - dbToFrac (-5.0f) * (bar.getHeight() - 2.0f);
                const float yFillTop   = y;
                const float yFillBot   = bar.getBottom() - 1.0f;

                auto fillBand = [&] (float top, float bottom, juce::Colour col)
                {
                    if (bottom <= top) return;
                    g.setColour (col);
                    g.fillRect (juce::Rectangle<float> (x, top, w, bottom - top));
                };
                // Red band: fill top -> min(yRedTop, fillBot)
                fillBand (juce::jmax (yFillTop, bar.getY()),
                            juce::jmin (yRedTop, yFillBot),
                            kLedRed);
                // Yellow band: max(fillTop, yRedTop) -> min(yYellowTop, fillBot)
                fillBand (juce::jmax (yFillTop, yRedTop),
                            juce::jmin (yYellowTop, yFillBot),
                            kLedYellow);
                // Green band: max(fillTop, yYellowTop) -> fillBot
                fillBand (juce::jmax (yFillTop, yYellowTop),
                            yFillBot,
                            kLedGreen);
            }

            const float peakFrac = dbToFrac (peakDb);
            if (peakFrac > 0.001f)
            {
                const float y = bar.getBottom() - 1.0f - peakFrac * (bar.getHeight() - 2.0f);
                g.setColour (peakDb >= 5.0f ? juce::Colour (0xffff8080)
                                              : juce::Colour (0xfff0f0f0));
                g.fillRect (juce::Rectangle<float> (bar.getX() + 1.0f, y - 0.5f,
                                                      bar.getWidth() - 2.0f, 1.4f));
            }

            const int segments = juce::jlimit (8, 30, (int) (bar.getHeight() / 3.5f));
            const float segStep = bar.getHeight() / (float) segments;
            g.setColour (juce::Colour (0xff020203));
            for (int i = 1; i < segments; ++i)
            {
                const float yy = bar.getY() + i * segStep;
                g.fillRect (juce::Rectangle<float> (bar.getX() + 1.0f, yy - 0.4f,
                                                      bar.getWidth() - 2.0f, 0.8f));
            }
        };

        if (stereoMode)
        {
            const auto full = inputMeterArea.toFloat();
            const float halfW = (full.getWidth() - 1.0f) * 0.5f;   // 1 px gap between bars
            const auto barL = juce::Rectangle<float> (full.getX(),               full.getY(),
                                                        halfW, full.getHeight());
            const auto barR = juce::Rectangle<float> (full.getX() + halfW + 1.0f, full.getY(),
                                                        halfW, full.getHeight());
            drawBar (barL, displayedInputDb,  inputPeakHoldDb);
            drawBar (barR, displayedInputRDb, inputPeakHoldRDb);

            // Tiny "L" / "R" caption above each bar so the user immediately
            // sees the meter is now stereo.
            g.setColour (juce::Colour (0xffa0a0a8));
            g.setFont (juce::Font (juce::FontOptions (7.5f, juce::Font::bold)));
            // Push the caption down past the pan slice (kPanLabelH +
            // kPanBlockH from resized()) so it lands at the visual top of
            // the meter bar instead of the strip-wide top of the column,
            // where it floats inside the COMP section.
            constexpr float kPanReserve = 11.0f + 40.0f;
            const float captionY = barL.getY() + kPanReserve - 9.0f;
            g.drawText ("L", juce::Rectangle<float> (barL.getX(), captionY,
                                                       barL.getWidth(), 8.0f),
                          juce::Justification::centred, false);
            g.drawText ("R", juce::Rectangle<float> (barR.getX(), captionY,
                                                       barR.getWidth(), 8.0f),
                          juce::Justification::centred, false);
        }
        else
        {
            drawBar (inputMeterArea.toFloat(), displayedInputDb, inputPeakHoldDb);
        }
    }

    // Track-3 fader scale labels — drawn here (not in the LookAndFeel)
    // so they can extend into the strip area to the LEFT of the slider
    // bounds. LookAndFeel only draws the ticks for this layout.
    if (usesFaderThresholdLayout())
    {
        const auto& range = faderSlider.getNormalisableRange();
        const auto sliderB = faderSlider.getBounds().toFloat();
        const float trackCx = sliderB.getCentreX();
        // trackW = jmin(4, sliderW*0.18) inside the LookAndFeel; mirror so
        // labels sit just left of the tick stub.
        const float trackW  = juce::jmin (4.0f, sliderB.getWidth() * 0.18f);
        const float trackLx = trackCx - trackW * 0.5f;

        for (const auto& t : kFaderTicks)
        {
            if (t.db < range.start - 0.01f || t.db > range.end + 0.01f) continue;
            const float y = faderYForDb (faderSlider, t.db);
            const bool isZero   = (std::abs (t.db) < 0.01f);
            const bool isBottom = (t.db <= -89.0f);

            g.setColour (isZero ? juce::Colour (0xffffffff)
                                : juce::Colour (0xffb8b8c0));
            // Uniform font for every label so weight differences don't
            // throw off the visual right-alignment — the brighter colour
            // alone marks 0 dB. The ∞ glyph renders visually smaller than the
            // digits at a given point size, so upsize it to match their height.
            const juce::Font font (juce::FontOptions (isBottom ? 16.0f : 10.5f,
                                                      juce::Font::plain));
            g.setFont (font);
            constexpr float kSharedXOver  = 24.0f;
            const float labelRight = trackLx - kSharedXOver - 6.0f;
            const juce::String label = isBottom ? juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x9e"))   /* ∞ = -inf dB / fully off */
                                                : juce::String (t.label);
            // Visible glyph spans (baseline - ascent) to (baseline + descent).
            // For visible centre == tick y → baseline = y + (ascent - descent)/2.
            // Empirical -2 px bias compensates for JUCE FreeType's tendency
            // to include a small amount of line-leading inside ascent on
            // X11; without it the glyph centre drifts ~2 px below the tick.
            const float ascent  = font.getAscent();
            const float descent = font.getDescent();
            const float baselineY = y + (ascent - descent) * 0.5f - 2.0f;
            const float textW = juce::GlyphArrangement::getStringWidth (font, label);
            g.drawSingleLineText (label,
                                    juce::roundToInt (labelRight - textW),
                                    juce::roundToInt (baselineY),
                                    juce::Justification::left);
        }
    }

    // Fader dB scale - labels in the scale column aligned with the tick
    // marks the LookAndFeel paints across the fader's track. Same set of
    // values as kFaderTicks; format matches the screenshot's absolute-
    // value style ("6", "3", "0", "3", "6", "12", "24", "40", "90").
    if (! meterScaleArea.isEmpty())
    {
        const auto scale = meterScaleArea;
        const auto& range = faderSlider.getNormalisableRange();
        // Map scale labels to the METER's dB-to-Y curve (using kFloorDb /
        // kCeilingDb, same dbToFrac the bar fill uses) instead of the
        // FADER's skewed slider track. They sit next to the LED bar so the
        // user reads the scale as the meter's level scale — not the
        // fader's position scale — and they need to line up with the bar
        // fill height, not the slider thumb position.
        const auto meterRect = inputMeterArea.toFloat();
        if (! meterRect.isEmpty())
        {
            // Hardware-fader style: a horizontal tick line on the LEFT
            // side of the scale column (pointing toward the meter) with
            // the number right-aligned next to it. "-90 dB" becomes "∞"
            // (−inf / fully off) at the bottom of the range.
            for (const auto& t : kFaderTicks)
            {
                if (t.db < (float) faderRange.start - 0.01f
                    || t.db > (float) faderRange.end + 0.01f) continue;
                const float frac = dbToFrac (t.db);
                const float y = meterRect.getBottom() - 1.0f
                                  - frac * (meterRect.getHeight() - 2.0f);
                if (y - 7.0f > (float) scale.getBottom()) continue;

                const bool isZero    = (std::abs (t.db) < 0.01f);
                const bool isBottom  = (t.db <= -89.0f);
                const float tickLen  = isZero ? 10.0f : (isBottom ? 6.0f : 8.0f);
                const float tickX0   = (float) scale.getX();
                const float tickX1   = tickX0 + tickLen;
                const float lineW    = isZero ? 1.2f : 0.7f;
                g.setColour (isZero ? juce::Colour (0xffe8e8ec)
                                    : juce::Colour (0xff707078));
                g.drawLine (tickX0, y, tickX1, y, lineW);

                g.setColour (isZero ? juce::Colour (0xffffffff)
                                    : juce::Colour (0xffc0c0c8));
                // ∞ upsized to match the digit height (it renders small at a
                // given point size); 0 dB bold; the rest plain.
                g.setFont (juce::Font (juce::FontOptions (
                    isBottom ? 14.0f : (isZero ? 10.5f : 9.5f),
                    isZero ? juce::Font::bold : juce::Font::plain)));
                const auto labelRect = juce::Rectangle<float> (tickX1 + 1.0f, y - 7.0f,
                                                                 (float) scale.getRight() - (tickX1 + 1.0f),
                                                                 14.0f);
                const juce::String label = isBottom ? juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x9e"))   /* ∞ = -inf dB / fully off */ : juce::String (t.label);
                g.drawText (label, labelRect, juce::Justification::centredLeft, false);
            }
        }
    }
}

void ChannelStripComponent::resized()
{
    auto area = getLocalBounds().reduced (4);
    area.removeFromTop (6);

    {
        auto nameRow = area.removeFromTop (20);
        if (track.strip.faderGroupId.load (std::memory_order_relaxed) != 0)
            groupChipBounds = nameRow.removeFromRight (22).reduced (2, 4);
        else
            groupChipBounds = {};
        nameLabel.setBounds (nameRow);
    }
    area.removeFromTop (2);

    // Mixing stage swaps the tracking-only block (mode + input + IN/ARM/PRINT)
    // for a row of 4 AUX send knobs. Recording/Mastering keep the original
    // block. setMixingMode() drives this; here we just lay out whatever's
    // currently visible.
    // Tracking-only block (mode selector + input + IN/ARM/PRINT). Hidden in
    // Mixing stage - the AUX send knobs land below the COMP section instead
    // (signal-flow placement: EQ → COMP → SENDS → PAN → fader).
    if (! mixingMode)
    {
        // I/O region: one summary button (opens the I/O config popup with
        // mode + input/output dropdowns) followed by the IN/ARM/PRINT row.
        // Same fixed height for every track type so EQ / COMP / fader line
        // up across audio + MIDI strips.
        constexpr int kIoButtonH = 18;
        constexpr int kIoRowH    = 18;
        constexpr int kIoGap     = 3;
        constexpr int kIoRegionH = kIoButtonH + kIoGap + kIoRowH;
        auto ioRegion = area.removeFromTop (kIoRegionH);

        ioConfigButton.setBounds (ioRegion.removeFromTop (kIoButtonH));
        ioRegion.removeFromTop (kIoGap);

        auto buttonRow = ioRegion.removeFromTop (kIoRowH);
        const int colW = buttonRow.getWidth() / 3;
        monitorButton.setBounds (buttonRow.removeFromLeft (colW).reduced (1));
        armButton    .setBounds (buttonRow.removeFromLeft (colW).reduced (1));
        printButton  .setBounds (buttonRow.reduced (1));
    }

    // Plugin-slot button right below the IN/ARM/PRINT row. Always visible -
    // available in both compact and full modes since it's independent of the
    // EQ/COMP collapse.
    // reduced(4,0) matches the EQ / COMP / AUX compact buttons below so all
    // four section buttons share one width - otherwise the wider Insert border
    // sticks out past them and reads as a double / stacked outline.
    pluginSlotButton.setBounds (area.removeFromTop (18).reduced (4, 0));
    area.removeFromTop (2);

    // In compact mode the EQ + COMP sections collapse to two narrow buttons
    // so the fader / bus assigns / meter / M-S-Ø stay visible when the
    // TIMELINE view is consuming half the window. Click either button to
    // open the full editor as a popup.
    if (compactMode)
    {
        // Match bus + master compact pill sizing (20h × reduced(4,0))
        // so the three strip types read with the same compact grammar.
        constexpr int kCompactBtnH = 20;
        eqCompactButton  .setBounds (area.removeFromTop (kCompactBtnH).reduced (4, 0));
        area.removeFromTop (4);
        compCompactButton.setBounds (area.removeFromTop (kCompactBtnH).reduced (4, 0));
        area.removeFromTop (8);
        eqArea   = juce::Rectangle<int>();
        compArea = juce::Rectangle<int>();
    }
    else
    {

    // ── EQ block ─ SSL 9000J / E-EQ inspired layout.
    //   • Each band is a 2-column pair: GAIN on the left, FREQ on the right
    //     (same Y, prominent same-size knobs).
    //   • Bell bands (HM, LM) add a Q knob STACKED BELOW the gain in
    //     the same left column - no third column competing for horizontal
    //     space, freq stays on the right.
    //   • HPF lives at the top of the block as a single centred knob.
    //   • Shelf rows are short; bell rows are taller (gain block + Q block).
    // EQ knobs sit just a touch larger than the 24 px AUX / COMP knobs — big
    // enough for the SSL milled-skirt knob to read, small enough that the EQ
    // block doesn't eat fader height. Q matches gain/freq; no subordinate size.
    constexpr int kKnobSize    = 26;
    constexpr int kValueLabelH = 12;
    constexpr int kKnobBlockH  = kKnobSize + kValueLabelH + 2;
    constexpr int kQKnobSize   = 26;
    constexpr int kQBlockH     = kQKnobSize + kValueLabelH;
    constexpr int kFreqYStagger = 2;                                  // tighter SSL nudge - was 4
    constexpr int kEqHeaderH   = 16;  // matches COMP header row height for visual parity
    constexpr int kFilterLabelH = 10;                                 // small HPF/LPF captions above the white knobs
    constexpr int kEqHpfRowH    = kFilterLabelH + kKnobBlockH;        // 48 - label strip + knob row
    constexpr int kEqShelfRowH  = kKnobBlockH + kFreqYStagger + 2;    // 42 - tighter than before
    constexpr int kEqBellRowH   = kKnobBlockH + kQBlockH;             // 74
    constexpr int kEqTypeChipH  = 16;                                 // E/G chip slot between HM (row 1) and LM (row 2)

    // 1 HPF row + 2 shelf rows (HF, LF) + 2 bell rows (HM, LM). No vertical
    // reduce - every pixel of EQ height is used so the knobs hug the header.
    eqArea = area.removeFromTop (kEqHeaderH + kEqHpfRowH
                                    + 2 * kEqShelfRowH + 2 * kEqBellRowH
                                    + kEqTypeChipH);
    {
        auto s = eqArea.reduced (3, 0);
        const auto header = s.removeFromTop (kEqHeaderH);
        // Single CompHeaderButton-style EQ header. Left-click toggles
        // eqEnabled (LED reflects state); right-click pops the
        // Brown/Black type picker. Bounds = full header for the same
        // visual rhythm as the COMP header button.
        if (eqHeaderBtn != nullptr) eqHeaderBtn->setBounds (header);

        constexpr int kRowLabelW = 28;

        // Filter row — HPF | LPF aligned with the band-row columns
        // below. Label strip on top (centred captions over each knob),
        // knob strip below sized identically to band gain/freq knobs
        // (same colW, same kKnobBlockH) so the white knobs read as the
        // same size as the coloured ones.
        {
            constexpr int kFilterLabelH = 10;
            auto labelRow = s.removeFromTop (kFilterLabelH);
            auto knobRow  = s.removeFromTop (kKnobBlockH);

            // Skip kRowLabelW on the left so the filter columns line
            // up with the band rows' gain/freq columns (which also
            // carve out kRowLabelW for the "HF"/"HM"/etc row labels).
            labelRow.removeFromLeft (kRowLabelW);
            knobRow .removeFromLeft (kRowLabelW);
            const int colW = knobRow.getWidth() / 2;
            const int leftX  = knobRow.getX();
            const int rightX = leftX + colW;

            hpfLabel.setBounds (leftX,  labelRow.getY(), colW, kFilterLabelH);
            lpfLabel.setBounds (rightX, labelRow.getY(), colW, kFilterLabelH);
            hpfKnob .setBounds (leftX,  knobRow.getY(),  colW, kKnobBlockH);
            lpfKnob .setBounds (rightX, knobRow.getY(),  colW, kKnobBlockH);
        }

        // Band rows. Knobs are top-aligned within each row (no leading
        // padding) - keeps everything pulled up toward the EQ header.
        for (size_t i = 0; i < eqRows.size(); ++i)
        {
            const bool hasQ = eqRows[i].q != nullptr;
            const int rowH = hasQ ? kEqBellRowH : kEqShelfRowH;
            auto row = s.removeFromTop (rowH);
            // LM row (i == 2) shifted up a few pixels so it sits closer
            // to the HM row above. Overlaps the bottom of HM's Q value
            // label slot — that slot has padding so visible text isn't
            // clipped. Pure visual lift; rowH still reserves the same
            // vertical chunk so downstream rows (LF) stay put.
            if (i == 2)
                row.translate (0, -6);
            // Row-label area, vertically aligned to the rotary's centre so
            // "HM" / "LM" / etc sit next to the actual knob, not centred
            // within the row's full height (which puts them next to the
            // value text instead). The label box is sized to the rotary
            // area only (kKnobSize) - JUCE's centred Label vertical-centres
            // the text within those bounds.
            auto labelArea = row.removeFromLeft (kRowLabelW);
            const int rotaryH = kKnobSize;  // height of the actual rotary
            eqRows[i].rowLabel.setBounds (labelArea.getX(), row.getY(),
                                           labelArea.getWidth(), rotaryH);
            if (hasQ)
            {
                const int qRotaryH = kQKnobSize;
                const int qY       = row.getY() + kKnobBlockH;
                eqRows[i].qLabel.setBounds (labelArea.getX(), qY,
                                             labelArea.getWidth(), qRotaryH);
            }

            const int colW = row.getWidth() / 2;
            const int leftX  = row.getX();
            const int rightX = row.getX() + colW;
            const int gainY = row.getY();
            // Bell rows have a Q knob stacked under the gain on the left;
            // the freq sits in its own column on the right and is now
            // vertically centred inside the FULL bell row (between the
            // gain and Q rows). Shelf rows keep the small SSL nudge.
            const int freqY = hasQ
                ? row.getY() + (rowH - kKnobBlockH) / 2
                : gainY + kFreqYStagger;

            eqRows[i].gain->setBounds (leftX,  gainY, colW, kKnobBlockH);
            eqRows[i].freq->setBounds (rightX, freqY, colW, kKnobBlockH);

            if (hasQ)
            {
                const int qW    = juce::jmin (colW, 44);
                const int qX    = leftX + (colW - qW) / 2;
                const int qY    = gainY + kKnobBlockH;
                eqRows[i].q->setBounds (qX, qY, qW, kQBlockH);
            }

            // E/G type chip slot between HM (i==1) and LM (i==2),
            // positioned in the freq column so it sits between the
            // HM freq knob above and the LM freq knob below — the
            // user-requested mid-strip prominence.
            if (i == 1)
            {
                auto chipSlot = s.removeFromTop (kEqTypeChipH);
                const int chipW  = juce::jmin (colW - 8, 28);
                const int chipX  = rightX + (colW - chipW) / 2;
                eqTypeChip.setBounds (chipX, chipSlot.getY() + 1, chipW, kEqTypeChipH - 2);
            }
        }
    }
    area.removeFromTop (3);

    // COMP region:
    //   Header  : ON button
    //   Mode    : O / F / V
    //   Body    : per-mode knob set on the LEFT, threshold/IN/GR meter on
    //             the RIGHT. Putting the meter inside the comp section
    //             (rather than next to the fader) keeps all comp UI grouped
    //             and frees up the fader column for a taller fader.
    // COMP knob diameter matches EQ + AUX (24 px) so every knob down the
    // strip reads as one visual rhythm.
    constexpr int kCompKnobSize     = 24;
    constexpr int kCompKnobBlockH   = kCompKnobSize + kValueLabelH + 2;
    constexpr int kCompKnobLabelH   = 10;
    constexpr int kCompKnobRowH     = kCompKnobLabelH + kCompKnobBlockH;
    constexpr int kCompKnobGap      = 4;
    constexpr int kCompMeterW       = 36;   // handle + IN bar + dB scale + GR bar
    constexpr int kCompMeterGap     = 4;

    // Body height = standard 2 × knob row + gap, plus a small extra
    // strip kept at 0 — the Fst/Slo hint labels under ATK/REL were
    // removed; the value-label rendering inside each knob block now
    // gets the full block height with no extra padding.
    constexpr int kCompBodyExtraH = 0;
    constexpr int kCompBodyH = 2 * kCompKnobRowH + kCompKnobGap + kCompBodyExtraH;
    // Track-3 collapses every comp mode to a single-row body: FET/VCA
    // pack 4 knobs side-by-side; OPTO packs GAIN + LIMIT side-by-side.
    // Half the vertical space of the default 2-row layout.
    const bool track3OneRowComp = usesFaderThresholdLayout();
    const int effectiveCompBodyH = track3OneRowComp ? kCompKnobRowH : kCompBodyH;
    compArea = area.removeFromTop (16 + 2 + effectiveCompBodyH + 4);
    {
        auto s = compArea.reduced (3, 2);

        // Header row: single centered COMP button. Left-click toggles
        // enabled; right-click pops the mode picker. Built-in LED +
        // label.
        auto headerRow = s.removeFromTop (16);
        if (compModeButton != nullptr)
            compModeButton->setBounds (headerRow);
        s.removeFromTop (2);

        // Body: GR meter strip (handle + IN + dB scale + GR) on the
        // LEFT — matches Mixbus's threshold-fader-on-the-left grammar.
        // Remaining width on the right holds the mode-specific knob
        // grid. The "main amount" param (OPTO peak red / FET input /
        // VCA threshold) is set by dragging the triangle handle on the
        // meter strip — no dedicated knob or slider for it.
        auto body = s.removeFromTop (effectiveCompBodyH);
        // Track 3 (experimental): the CompMeterStrip moves to a slim
        // column next to the fader, so the COMP section's body uses the
        // full width for the mode-specific knob grid. compMeter's bounds
        // are set later in the fader-column block.
        if (! usesFaderThresholdLayout())
        {
            auto meterRect = body.removeFromLeft (kCompMeterW);
            body.removeFromLeft (kCompMeterGap);
            if (compMeter != nullptr)
                compMeter->setBounds (meterRect);
        }

        auto layoutKnobCell = [&] (juce::Rectangle<int> cell,
                                    juce::Slider& knob, juce::Label& label)
        {
            label.setBounds (cell.removeFromTop (kCompKnobLabelH));
            knob.setBounds  (cell.getX(), cell.getY(), cell.getWidth(), kCompKnobBlockH);
        };

        const int currentMode = juce::jlimit (0, 2,
            track.strip.compMode.load (std::memory_order_relaxed));

        if (currentMode == 0)  // OPTO: GAIN knob top, LIMIT toggle bottom
        {
            if (usesFaderThresholdLayout())
            {
                // Track-3 OPTO single-row: GAIN cell on the LEFT half,
                // LIMIT toggle vertically centred in the RIGHT half.
                // Matches the 1-row footprint used by FET/VCA so the
                // strip's COMP section height is mode-independent.
                auto row     = body.removeFromTop (kCompKnobRowH);
                const int colW = row.getWidth() / 2;
                auto gainCell  = row.removeFromLeft (colW);
                layoutKnobCell (gainCell, optoGainKnob, optoGainLabel);
                const int limW = juce::jmin (54, row.getWidth());
                optoLimitButton.setBounds (
                    row.getX() + (row.getWidth() - limW) / 2,
                    row.getY() + (row.getHeight() - 18) / 2,
                    limW, 18);
            }
            else
            {
                auto row1 = body.removeFromTop (kCompKnobRowH);
                body.removeFromTop (kCompKnobGap);
                auto row2 = body.removeFromTop (kCompKnobRowH);
                layoutKnobCell (row1, optoGainKnob, optoGainLabel);
                const int limW = juce::jmin (54, row2.getWidth());
                optoLimitButton.setBounds (row2.getX() + (row2.getWidth() - limW) / 2,
                                            row2.getY() + (row2.getHeight() - 18) / 2,
                                            limW, 18);
            }
        }
        else if (currentMode == 1)  // FET
        {
            if (track3OneRowComp)
            {
                // RAT / ATK / REL / MAK — matches the bus comp's single-row
                // order so every comp in the mixer reads identically.
                auto row = body.removeFromTop (kCompKnobRowH);
                const int colW = row.getWidth() / 4;
                layoutKnobCell (row.removeFromLeft (colW), fetRatioKnob,   fetRatioLabel);
                layoutKnobCell (row.removeFromLeft (colW), fetAttackKnob,  fetAttackLabel);
                layoutKnobCell (row.removeFromLeft (colW), fetReleaseKnob, fetReleaseLabel);
                layoutKnobCell (row,                        fetOutputKnob,  fetOutputLabel);
            }
            else
            {
                auto row1 = body.removeFromTop (kCompKnobRowH);
                body.removeFromTop (kCompKnobGap);
                auto row2 = body.removeFromTop (kCompKnobRowH);
                const int colW = row1.getWidth() / 2;
                layoutKnobCell (row1.removeFromLeft (colW), fetRatioKnob,   fetRatioLabel);
                layoutKnobCell (row1,                       fetOutputKnob,  fetOutputLabel);
                layoutKnobCell (row2.removeFromLeft (colW), fetAttackKnob,  fetAttackLabel);
                layoutKnobCell (row2,                        fetReleaseKnob, fetReleaseLabel);
            }
        }
        else  // VCA
        {
            if (track3OneRowComp)
            {
                // RAT / ATK / REL / MAK — matches the bus comp's single-row
                // order so every comp in the mixer reads identically.
                auto row = body.removeFromTop (kCompKnobRowH);
                const int colW = row.getWidth() / 4;
                layoutKnobCell (row.removeFromLeft (colW), vcaRatioKnob,   vcaRatioLabel);
                layoutKnobCell (row.removeFromLeft (colW), vcaAttackKnob,  vcaAttackLabel);
                layoutKnobCell (row.removeFromLeft (colW), vcaReleaseKnob, vcaReleaseLabel);
                layoutKnobCell (row,                        vcaOutputKnob,  vcaOutputLabel);
            }
            else
            {
                auto row1 = body.removeFromTop (kCompKnobRowH);
                body.removeFromTop (kCompKnobGap);
                auto row2 = body.removeFromTop (kCompKnobRowH);
                const int colW = row1.getWidth() / 2;
                layoutKnobCell (row1.removeFromLeft (colW), vcaRatioKnob,   vcaRatioLabel);
                layoutKnobCell (row1,                       vcaOutputKnob,  vcaOutputLabel);
                layoutKnobCell (row2.removeFromLeft (colW), vcaAttackKnob,  vcaAttackLabel);
                layoutKnobCell (row2,                        vcaReleaseKnob, vcaReleaseLabel);
            }
        }
    }
    area.removeFromTop (usesFaderThresholdLayout() ? 6 : 3);
    }   // end of else (! compactMode)

    // ── AUX sends (Mixing stage only). Single row of 4 knobs with a slight
    //    vertical zig-zag - even-index knobs sit higher, odd-index sit lower.
    //    Staggering keeps each knob full-size while they share a narrow strip
    //    width that wouldn't allow 4 knobs at the same Y without crowding the
    //    value labels. Sits between COMP and PAN to match signal-flow order:
    //    EQ → COMP → SENDS → PAN → fader.
    if ((mixingMode || usesFaderThresholdLayout()) && compactMode)
    {
        // Match the EQ + COMP compact pill geometry (20h × reduced(4,0))
        // so the three stacked pills read as one cohesive group.
        auxCompactButton.setBounds (area.removeFromTop (20).reduced (4, 0));
        area.removeFromTop (4);
        auxRowArea = juce::Rectangle<int>();
    }
    else if (mixingMode || usesFaderThresholdLayout())
    {
        constexpr int kAuxKnobSize  = 24;
        constexpr int kAuxStaggerY  = 10;     // odd knobs offset down by this much
        constexpr int kAuxValueH    = 10;
        constexpr int kAuxBlockH    = kAuxStaggerY + kAuxKnobSize + 2 + kAuxValueH;
        constexpr int kAuxLabelH    = 11;
        constexpr int kAuxLabelGap  = 1;
        constexpr int kAuxRowTotalH = kAuxLabelH + kAuxLabelGap + kAuxBlockH;

        // Capture the framed-box bounds for paint() (drawn in the same
        // style as eqArea / compArea, with the SEND purple accent).
        auxRowArea = area.removeFromTop (kAuxRowTotalH);

        // Lay out the label + knob block inside the box, with a small
        // horizontal padding so the SEND knobs don't kiss the frame.
        auto inner = auxRowArea.reduced (3, 0);
        auto headerRow = inner.removeFromTop (kAuxLabelH);
        inner.removeFromTop (kAuxLabelGap);

        auto block = inner.removeFromTop (kAuxBlockH);
        const int colW = block.getWidth() / ChannelStripParams::kNumAuxSends;

        for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
        {
            auto col = juce::Rectangle<int> (block.getX() + i * colW, block.getY(),
                                                colW, block.getHeight());

            // Index numeral sits centred above its knob column, painted
            // in the matching kAuxColours tint. Same X-grid as the knob
            // below, so it scans as a header for that knob.
            auxIndexLabels[(size_t) i].setBounds (col.getX(), headerRow.getY(),
                                                    col.getWidth(), kAuxLabelH);

            const int knobX = col.getX() + (col.getWidth() - kAuxKnobSize) / 2;
            const int knobY = col.getY() + ((i % 2 == 0) ? 0 : kAuxStaggerY);
            if (auxKnobs[(size_t) i] != nullptr)
                auxKnobs[(size_t) i]->setBounds (knobX, knobY,
                                                  kAuxKnobSize, kAuxKnobSize);

            const int labelY = col.getBottom() - kAuxValueH;
            auxKnobLabels[(size_t) i].setBounds (col.getX(), labelY,
                                                   col.getWidth(), kAuxValueH);
        }

        area.removeFromTop (3);
    }
    else
    {
        // Non-mixing stages don't show the SEND row, so the framed box
        // disappears with it.
        auxRowArea = juce::Rectangle<int>();
    }

    // The horizontal BUSES region used to live here. Bus toggles now sit in
    // a vertical column to the left of the fader (laid out below).

    // Pan section is laid out together with the fader (see below) so
    // the knob can be horizontally centered over the FADER COLUMN
    // (busColumn + faderArea, excluding the right-side peak column),
    // not over the strip itself. Centering over the strip puts the
    // knob offset to the right of the visible fader because the peak
    // column eats 31-37 px from the right edge.

    auto buttons = area.removeFromBottom (20);
    const int btnW = buttons.getWidth() / 3;
    muteButton .setBounds (buttons.removeFromLeft (btnW).reduced (1));
    soloButton .setBounds (buttons.removeFromLeft (btnW).reduced (1));
    phaseButton.setBounds (buttons.reduced (1));
    area.removeFromBottom (2);

    // Automation mode button - sits as a thin full-width row directly above
    // M/S/Ø. Single label that mirrors the current mode; click cycles.
    auto autoRow = area.removeFromBottom (16);
    autoModeButton.setBounds (autoRow.reduced (1, 0));
    area.removeFromBottom (4);

    // Fader + input meter pinned to the bottom of the strip. To the right of
    // the fader: a small dB scale column (0/-12/-24/-60) and a vertical LED
    // input meter. GR + threshold drag moved into the COMP section, so this
    // column is now slim and the fader gets the reclaimed width.
    constexpr int kMaxFaderHeight  = 360;  // 280 -> 360: bank row left ConsoleView, freed height goes to faders
    constexpr int kPeakLabelH      = 18;
    // Meter column width is mode-aware: in stereo we draw two bars side by
    // side, so we need extra room. Mono / Midi use a narrower column.
    const bool stereoMode = (track.mode.load (std::memory_order_relaxed)
                              == (int) Track::Mode::Stereo);
    const int kMeterWidth = stereoMode ? 18 : 12;
    constexpr int kMeterScaleWidth = 16;
    constexpr int kMeterGap        = 3;

    auto faderArea = area;
    // Reserve a row at the very bottom for the numeric output-peak readout,
    // centred under the meter + GR-LED cluster (mirrors the bus / master
    // strips). Carved before the columns so the fader, level meter and GR LED
    // all shorten together and keep the meter's 1:1 scale alignment intact.
    auto peakRow = faderArea.removeFromBottom (kPeakLabelH);
    faderArea.removeFromBottom (2);
    // Track 3 wants the full remaining vertical real estate for its
    // fader column — skip the kMaxFaderHeight cap that other strips use.
    if (! usesFaderThresholdLayout()
        && faderArea.getHeight() > kMaxFaderHeight + kPeakLabelH + 2)
        faderArea = faderArea.removeFromBottom (kMaxFaderHeight + kPeakLabelH + 2);

    // Vertical bus-assign column. Lives on the LEFT of the fader by default
    // (Tascam Model 2400 grammar — buttons under the engineer's left hand).
    // At narrow strip widths it migrates to the RIGHT side, past the meter
    // cluster, so it can't visually overlap the fader cap when the strip
    // is squeezed below its design min width.
    constexpr int kBusColumnW   = 18;
    constexpr int kBusColumnGap = 3;
    constexpr int kBusButtonH   = 20;   // fixed height; evenly spaced across the 0→∞ scale
    juce::Rectangle<int> busColumn;

    juce::Rectangle<int> meterColumn, scaleColumn;
    juce::Rectangle<int> faderCompMeterCol;
    if (usesFaderThresholdLayout())
    {
        // Track-3 layout (right → left): GR LED hugs the right side of
        // the main level meter (1 px gap — reads as one continuous pair),
        // both glued IMMEDIATELY to the right of the fader. compMeter
        // internally flips its handle to the RIGHT (see setHandleOnRight)
        // so the triangle points LEFT at the GR bar instead of poking
        // into the meter column. kFaderLeftShift consumes empty space
        // on the LEFT of the fader column so cap + PAN knob centre under
        // the LIMIT button (which is centred on the strip), not biased
        // left toward the bus-assign column.
        constexpr int kGrLedW          = 20;   // GR bar + handle
        constexpr int kMeterToGrGap    = 1;
        constexpr int kFaderToMeterGap = 1;
        constexpr int kRightPad        = 14;
        constexpr int kFaderLeftShift  = 29;
        constexpr int kFaderColMinReserve = 22;

        // Wide-layout fader-column width prediction: bus on the LEFT, full
        // leftShift, full right-side cluster carved from the right.
        const int wideRightStack = kRightPad + kGrLedW + kMeterToGrGap
                                  + kMeterWidth + kFaderToMeterGap;
        const int faderColWideW  = faderArea.getWidth() - kBusColumnW
                                  - kBusColumnGap - wideRightStack
                                  - kFaderLeftShift;
        const bool narrowMode = (faderColWideW < kFaderColMinReserve);

        scaleColumn = juce::Rectangle<int>();
        grScaleArea = juce::Rectangle<int>();
        if (narrowMode)
        {
            // Bus migrates to the FAR RIGHT past the meter cluster; no
            // leftShift needed since the fader column starts at the strip
            // edge. Cap drifts slightly left of strip centre but can no
            // longer overlap the bus buttons — the right-side stack is the
            // ONLY thing carved from the strip, fader gets all remaining
            // width.
            faderArea.removeFromRight (kRightPad);
            busColumn = faderArea.removeFromRight (kBusColumnW);
            faderArea.removeFromRight (kBusColumnGap);
            faderCompMeterCol = faderArea.removeFromRight (kGrLedW);
            faderArea.removeFromRight (kMeterToGrGap);
            meterColumn = faderArea.removeFromRight (kMeterWidth);
            faderArea.removeFromRight (kFaderToMeterGap);
        }
        else
        {
            // Wide path: bus on the LEFT, full leftShift centres the cap on
            // strip centre under the strip-centred GAIN / LIMIT controls.
            busColumn = faderArea.removeFromLeft (kBusColumnW);
            faderArea.removeFromLeft (kBusColumnGap);
            faderArea.removeFromRight (kRightPad);
            faderCompMeterCol = faderArea.removeFromRight (kGrLedW);
            faderArea.removeFromRight (kMeterToGrGap);
            meterColumn = faderArea.removeFromRight (kMeterWidth);
            faderArea.removeFromRight (kFaderToMeterGap);
            faderArea.removeFromLeft (kFaderLeftShift);
        }
    }
    else
    {
        busColumn = faderArea.removeFromLeft (kBusColumnW);
        faderArea.removeFromLeft (kBusColumnGap);
        grScaleArea = juce::Rectangle<int>();
        meterColumn = faderArea.removeFromRight (kMeterWidth);
        faderArea.removeFromRight (kMeterGap);
        scaleColumn = faderArea.removeFromRight (kMeterScaleWidth);
        faderArea.removeFromRight (kMeterGap);
    }

    // Peak readout beneath the meter column. GR readout retired - the GR bar
    // inside the COMP section is the canonical readout now.
    grPeakLabel  .setVisible (false);
    grReadoutLabel.setVisible (false);

    // Numeric output-peak readout centred under the meter + GR-LED cluster,
    // matching the bus / master strips (the LED itself is the level scale;
    // the number is the peak hold). The cluster spans the level meter through
    // the GR LED to its right; the meter bottom is trimmed below to clear it.
    inputPeakLabel.setVisible (true);
    {
        const int clusterX = meterColumn.getX();
        const int clusterR = faderCompMeterCol.isEmpty() ? meterColumn.getRight()
                                                         : faderCompMeterCol.getRight();
        inputPeakLabel.setBounds (clusterX, peakRow.getY(),
                                    juce::jmax (1, clusterR - clusterX),
                                    peakRow.getHeight());
    }

    // Track-3: trim only enough to leave the slider's 6-px track padding
    // so the meter bottom lands exactly on the "off" tick. Default layout
    // still reserves the peak-readout label space below the meter.
    const int bottomTrim = usesFaderThresholdLayout() ? 6 : (kPeakLabelH + 2);
    meterColumn = meterColumn.withTrimmedBottom (bottomTrim);
    scaleColumn = scaleColumn.withTrimmedBottom (bottomTrim);

    // Hide the legacy "THR" header - threshold drag now lives in the COMP
    // section's meter strip, so the header next to the fader is misleading.
    threshMeterLabel.setVisible (false);

    // Pan section pinned to the TOP of the fader column. Knob is
    // centered on the fader's x-centre (faderArea after busColumn +
    // peakColumn carve-outs) — NOT the strip centre — so it visually
    // sits over the slider thumb's track.
    constexpr int kPanKnobSize = 26;
    constexpr int kPanValueH   = 12;
    constexpr int kPanBlockH   = kPanKnobSize + kPanValueH + 2;
    constexpr int kPanLabelH   = 11;
    constexpr int kPanBlockW   = 56;
    // Slider geometry relative to faderArea (pre-pan-removal) top:
    //   sliderTop = panSlice (= kPanLabelH + kPanBlockH) + kPanFaderGap [+ track-3 extra trim]
    //   +6 tick   = sliderTop + kFaderTrackPad (LookAndFeel pads sliderBounds
    //               by kFaderTrackPad top + bottom so the cap fully fits)
    // Meter LED + GR LED tops pin to the +6 tick. kPanFaderGap is the
    // visible gap between pan-knob's "C" textbox bottom and the cap-at-max
    // top — small positive value leaves a clean separator without the cap
    // reaching into the pan area.
    constexpr int kPanFaderGap     = 4;
    constexpr int kTrack3ExtraTrim = 0;    // no additional withTrimmedTop — slider eats reclaimed space
    const int sliderTopRelative = kPanLabelH + kPanBlockH
                                 + (usesFaderThresholdLayout() ? kTrack3ExtraTrim : 0)
                                 + kPanFaderGap;
    const int topTrim = sliderTopRelative + (int) duskstudio::kFaderTrackPad;
    inputMeterArea = meterColumn.withTrimmedTop (topTrim);
    meterScaleArea = scaleColumn.withTrimmedTop (topTrim);

    auto panSlice = faderArea.removeFromTop (kPanLabelH + kPanBlockH);
    const int faderCentreX = panSlice.getCentreX();      // centre of fader column
    const int knobX        = faderCentreX - kPanBlockW / 2;
    const int labelBlockW  = juce::jmax (kPanBlockW, 40);
    panLabel.setBounds (faderCentreX - labelBlockW / 2,
                          panSlice.getY(),
                          labelBlockW, kPanLabelH);
    panKnob.setBounds (knobX, panSlice.getY() + kPanLabelH,
                          kPanBlockW, kPanBlockH);

    // Slider top sits kPanFaderGap below the pan slice so the cap at max
    // value has a small clean gap below the pan-knob "C" textbox instead
    // of overlapping it.
    auto sliderBounds = faderArea.withTrimmedTop (kPanFaderGap);
    if (usesFaderThresholdLayout())
    {
        // Cap (36 px tall) centres on the value Y. Trim top so cap.top
        // at max value clears the PAN knob above. Trim bottom enough
        // that the cap at min sits comfortably above the standalone
        // value label hosted below the slider.
        sliderBounds = sliderBounds.withTrimmedTop (kTrack3ExtraTrim).withTrimmedBottom (26);

        // Reserve a slot AT the slider's bottom edge for faderValueLabel.
        // The label sits just below the cap's lowest position so the
        // engineer always sees the current value as a separate readout.
        const int kFaderValueH = 18;
        faderValueLabel.setBounds (sliderBounds.getX(),
                                      sliderBounds.getBottom() + 6,
                                      sliderBounds.getWidth(),
                                      kFaderValueH);
    }
    faderSlider.setBounds (sliderBounds);

    // Bus buttons (1-4): span the fader scale from the "0" tick (top of #1) to
    // the "off" tick (bottom of #4), dividing the range into four equal slots.
    // faderYForDb gives the exact tick Y for both the normal and threshold fader
    // layouts; anchoring to the meter column would miss it on strips that reserve
    // peak-label space below the meter.
    {
        // Fixed-height buttons, evenly spaced: #1's top lines up with the "0"
        // tick and #4's bottom with the "∞" tick. Distribute the TOPs linearly
        // across [zeroY, offY - kBusButtonH]; the inter-button gap falls out as
        // (span - kNumBuses*H)/(kNumBuses-1).
        const int zeroY = (int) std::lround (duskstudio::faderYForDb (faderSlider, 0.0f));
        const int offY  = (int) std::lround (duskstudio::faderYForDb (faderSlider, -90.0f));
        const int span  = juce::jmax (ChannelStripParams::kNumBuses * kBusButtonH, offY - zeroY);
        for (int i = 0; i < ChannelStripParams::kNumBuses; ++i)
        {
            const int top = zeroY + (int) std::lround (
                (double) (span - kBusButtonH) * i / (ChannelStripParams::kNumBuses - 1));
            busButtons[(size_t) i]->setBounds (busColumn.getX(), top, kBusColumnW, kBusButtonH);
        }
    }

    // Track 3 (experimental): place the hoisted CompMeterStrip in its
    // fader-column slot. Vertically constrained to the trimmed meter
    // column rect so the handle / IN bar / GR bar share the same Y
    // extent as the main level meter to the right.
    if (usesFaderThresholdLayout() && compMeter != nullptr
        && ! faderCompMeterCol.isEmpty())
    {
        // Anchor the GR LED's bar top (grBarArea.top, which sits 10 px below
        // compMeter.top to leave room for the "GR" caption) on the LEVEL
        // METER's 0 dB tick — so the threshold-drag triangle's MAX position
        // lines up with the visible "0" mark on the fader scale instead of
        // floating between "0" and "+6".
        // Use the SAME dB-to-Y mapping the level-meter draw uses
        // (faderSlider.getNormalisableRange()) so the GR threshold
        // handle sits on the visible 0 dB tick. Linear -60..+6 was
        // wrong once the level meter moved to the fader's skewed
        // range — handle stayed at the old ~91% position while the
        // visible 0 tick had moved to ~96%.
        const auto& faderRange = faderSlider.getNormalisableRange();
        const float zeroFrac = (float) faderRange.convertTo0to1 (0.0);
        const int zeroY = inputMeterArea.getBottom() - 1
                        - juce::roundToInt (zeroFrac * (float) (inputMeterArea.getHeight() - 2));
        constexpr int kGrCaptionReserve = 10;   // matches CompMeterStrip::resized's hasCaptions branch
        const int compTop = zeroY - kGrCaptionReserve;
        auto compRect = faderCompMeterCol
                            .withY (compTop)
                            .withHeight (inputMeterArea.getBottom() - compTop);
        compMeter->setBounds (compRect);
    }
}
} // namespace duskstudio
