#include "AudioSettingsPanel.h"
#include "AppConfig.h"
#include "MidiBindingsPanel.h"
#include "SelfTestPanel.h"
#include "../engine/AudioEngine.h"
#include "../session/Session.h"
#if defined(__linux__)
 #include "../engine/alsa/AlsaAudioIODevice.h"
#endif

namespace duskstudio
{
AudioSettingsPanel::AudioSettingsPanel (juce::AudioDeviceManager& dm,
                                          AudioEngine& e, Session& s)
    : deviceManager (dm), engine (e), session (s)
{
    selector = std::make_unique<juce::AudioDeviceSelectorComponent>(
        dm,
        /*minIn*/  0, /*maxIn*/  16,
        /*minOut*/ 2, /*maxOut*/ 2,
        /*showMidi*/ false, /*showMidiOut*/ false,
        /*stereoPairs*/ false, /*hideAdvanced*/ false);
    addAndMakeVisible (*selector);

#if defined(__linux__)
    addAndMakeVisible (periodsLabel);
    periodsLabel.setJustificationType (juce::Justification::centredRight);

    // Sensible USB-audio range. 2 is the minimum that gives the kernel any
    // slack at all; 4 is the JUCE default and what most DAWs use; 8/16 add
    // latency but give the kernel more headroom against scheduler jitter.
    for (int p : { 2, 3, 4, 8, 16 })
        periodsCombo.addItem (juce::String (p), p);
    {
        // getRequestedPeriods() is clamped to [2,16] but the combo only exposes
        // a discrete subset; fall back to 4 (JUCE default) for any value that
        // doesn't have a corresponding item, otherwise the combo renders blank.
        const int requested = AlsaAudioIODevice::getRequestedPeriods();
        const bool inSet = (requested == 2 || requested == 3 || requested == 4
                            || requested == 8 || requested == 16);
        periodsCombo.setSelectedId (inSet ? requested : 4, juce::dontSendNotification);
    }
    periodsCombo.setTooltip ("ALSA period count. Only applies to ALSA backend. "
                              "Increase if you hear xruns or distortion at low "
                              "buffer sizes; decrease for lower latency.");
    periodsCombo.onChange = [this] { applyPeriodsChange(); };
    addAndMakeVisible (periodsCombo);
#endif

    selfTestButton.onClick = [this] { openSelfTest(); };
    selfTestButton.setTooltip (juce::CharPointer_UTF8 (
        "Open the headless audio pipeline self-test panel "
        "- synthetic engine tests + backend cycle."));
    addAndMakeVisible (selfTestButton);

    rescanButton.onClick = [this] { applyRescan(); };
    rescanButton.setTooltip ("Re-enumerate audio backends and devices. "
                              "Use after plugging in or removing a USB / "
                              "Thunderbolt audio interface.");
    addAndMakeVisible (rescanButton);

    // Effect oversampling - global. ComboBox IDs are the literal factor (1,
    // 2, 4) so we read the value back without an extra mapping table.
    // CharPointer_UTF8 wrappers are required because the "×" multiplication
    // sign (U+00D7) is two-byte UTF-8; without the explicit ctor JUCE's
    // juce::String defaults to Latin-1 and renders mojibake ("Ã-").
    addAndMakeVisible (oversamplingLabel);
    oversamplingLabel.setJustificationType (juce::Justification::centredRight);
    oversamplingCombo.addItem (juce::CharPointer_UTF8 ("1× (native)"), 1);
    oversamplingCombo.addItem (juce::CharPointer_UTF8 ("2×"),          2);
    oversamplingCombo.addItem (juce::CharPointer_UTF8 ("4×"),          4);
    {
        const int current = juce::jlimit (1, 4, session.oversamplingFactor.load (std::memory_order_relaxed));
        oversamplingCombo.setSelectedId ((current == 2 || current == 4) ? current : 1,
                                          juce::dontSendNotification);
    }
    oversamplingCombo.setTooltip (
        "Global effect oversampling. 1x is native rate "
        "(lowest CPU). 2x / 4x engage internal "
        "oversampling on the master + aux bus comps and "
        "the master tape saturation. Per-channel comp "
        "and EQ stay at native rate regardless.");
    oversamplingCombo.onChange = [this] { applyOversamplingChange(); };
    addAndMakeVisible (oversamplingCombo);

    // MIDI sync source. Populated on construction + re-populated on
    // every engine MIDI-bank rebuild via the engine's ChangeBroadcaster
    // (hot-plug, refreshMidiInputs). Selection writes session's saved
    // identifier; the engine's rebuild path resolves it back to an
    // index. Tooltip explains the v1 "tempo only" semantics.
    syncSourceLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (syncSourceLabel);
    syncSourceCombo.setTooltip (
        "MIDI Clock sync source. When set, Dusk Studio's tempo "
        "follows incoming MIDI Clock (24 PPQN) from the chosen "
        "input. Start/Stop chase is not wired yet - this is "
        "tempo-only sync in v1.");
    syncSourceCombo.onChange = [this] { applySyncSourceChange(); };
    addAndMakeVisible (syncSourceCombo);
    populateSyncSourceCombo();

    // Chase-transport toggle. v1 Phase 2 - off by default so existing
    // "tempo follower" workflows aren't surprised by the master also
    // starting/stopping local playback.
    syncChaseTransportToggle.setToggleState (
        session.externalSyncChasesTransport.load (std::memory_order_relaxed),
        juce::dontSendNotification);
    syncChaseTransportToggle.setTooltip (
        "When on, MIDI Start (FA / FB) plays Dusk Studio and MIDI Stop (FC) "
        "stops it. Off = tempo-only sync; the user controls Dusk Studio's "
        "transport locally.");
    syncChaseTransportToggle.onClick = [this]
    {
        session.externalSyncChasesTransport.store (
            syncChaseTransportToggle.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (syncChaseTransportToggle);

    // MIDI sync OUTPUT picker + emit toggle (Dusk Studio as master).
    syncOutputLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (syncOutputLabel);
    syncOutputCombo.setTooltip (
        "Pick a MIDI output port to send Clock + Start/Stop to. "
        "Used when Dusk Studio acts as the master. Emission only fires "
        "while the 'Emit clock' toggle is on.");
    syncOutputCombo.onChange = [this] { applySyncOutputChange(); };
    addAndMakeVisible (syncOutputCombo);
    populateSyncOutputCombo();

    syncEmitClockToggle.setToggleState (
        session.syncOutputEmitClock.load (std::memory_order_relaxed),
        juce::dontSendNotification);
    syncEmitClockToggle.setTooltip (
        "When on, Dusk Studio emits 24-PPQN MIDI Clock + Start/Stop "
        "transport bytes to the chosen output. Drum machines and "
        "external sequencers will follow Dusk Studio's tempo + transport.");
    syncEmitClockToggle.onClick = [this]
    {
        session.syncOutputEmitClock.store (
            syncEmitClockToggle.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (syncEmitClockToggle);

    // MTC chase toggle (slave-side absolute-time follow). Off by
    // default — same UX baseline as the Clock chase toggle.
    mtcChaseToggle.setToggleState (
        session.externalTimeCodeChasesTransport.load (std::memory_order_relaxed),
        juce::dontSendNotification);
    mtcChaseToggle.setTooltip (
        "When on, Dusk Studio's transport follows incoming MTC absolute time. "
        "Initial lock on the master's Play edge; freewheels within ~2 "
        "frames of drift; soft re-locates on sustained drift. Off = "
        "MTC time displayed only, transport stays under your control.");
    mtcChaseToggle.onClick = [this]
    {
        session.externalTimeCodeChasesTransport.store (
            mtcChaseToggle.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (mtcChaseToggle);

    // MTC emit toggle (master-side). Multiplexes onto the existing
    // Sync Output port — same MIDI cable carries Clock + MTC.
    mtcEmitToggle.setToggleState (
        session.syncOutputEmitTimeCode.load (std::memory_order_relaxed),
        juce::dontSendNotification);
    mtcEmitToggle.setTooltip (
        "When on, Dusk Studio emits MTC quarter-frames + full-frame sysex "
        "to the chosen Sync Output. Video editors and outboard tape "
        "machines will chase Dusk Studio's absolute playback position.");
    mtcEmitToggle.onClick = [this]
    {
        session.syncOutputEmitTimeCode.store (
            mtcEmitToggle.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (mtcEmitToggle);

    mtcEmitFrameRateLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (mtcEmitFrameRateLabel);
    mtcEmitFrameRateCombo.addItem ("24 fps",        1);
    mtcEmitFrameRateCombo.addItem ("25 fps",        2);
    mtcEmitFrameRateCombo.addItem ("29.97 DF",      3);
    mtcEmitFrameRateCombo.addItem ("30 fps",        4);
    mtcEmitFrameRateCombo.setSelectedId (
        session.syncOutputTimeCodeFrameRate.load (std::memory_order_relaxed) + 1,
        juce::dontSendNotification);
    mtcEmitFrameRateCombo.setTooltip (
        "SMPTE frame rate used when emitting MTC. 29.97 DF for NTSC "
        "video sync (drop-frame); 25 fps for PAL; 24 fps for film; "
        "30 fps non-drop for audio-only workflows.");
    mtcEmitFrameRateCombo.onChange = [this]
    {
        const int id = mtcEmitFrameRateCombo.getSelectedId();
        if (id >= 1 && id <= 4)
            session.syncOutputTimeCodeFrameRate.store (id - 1,
                                                         std::memory_order_relaxed);
    };
    addAndMakeVisible (mtcEmitFrameRateCombo);

    // MCU control surface input + output. Independent of the
    // sync-source pair so a user can run a master clock on one device
    // and an MCU controller on another. Receiver / Controller are
    // wired up by AudioEngine once both identifiers resolve to a
    // device index.
    mcuInputLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (mcuInputLabel);
    mcuInputCombo.setTooltip (
        "Pick the MIDI input port your Mackie Control / X-Touch / "
        "Tascam Model 12 (MCU mode) is sending on. Faders, V-pot "
        "encoders, transport buttons, and mute/solo/arm presses arrive "
        "on this port. Setting this gates that device's MIDI from the "
        "generic MIDI Learn surface so MCU traffic doesn't double-fire.");
    populateMcuInputCombo();
    mcuInputCombo.onChange = [this] { applyMcuInputChange(); };
    addAndMakeVisible (mcuInputCombo);

    mcuOutputLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (mcuOutputLabel);
    mcuOutputCombo.setTooltip (
        "Pick the MIDI output port for motorized fader / button LED / "
        "LCD / timecode / meter feedback to the control surface. "
        "Typically the same physical device as the MCU input.");
    populateMcuOutputCombo();
    mcuOutputCombo.onChange = [this] { applyMcuOutputChange(); };
    addAndMakeVisible (mcuOutputCombo);

    midiBindingsButton.setTooltip (
        "Open the MIDI Bindings panel: list everything currently mapped, "
        "remove individual bindings, or clear all. Use right-click on "
        "any fader / knob / button to add new bindings.");
    midiBindingsButton.onClick = [this]
    {
        auto body = std::make_unique<MidiBindingsPanel> (
            session, engine,
            [this] { midiBindingsModal.close(); });
        midiBindingsModal.show (*this, std::move (body));
    };
    addAndMakeVisible (midiBindingsButton);

    // Subscribe to the engine's ChangeBroadcaster so a hot-plug MIDI
    // device rebuild repopulates the sync-source combo. Without this,
    // a user who plugs in a new MIDI device after opening the panel
    // would have to close + reopen to see it.
    engine.addChangeListener (this);

    // UI scale - user override on top of JUCE's per-display DPI. Lives
    // in app config (per-machine), separate from session state.
    uiScaleLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (uiScaleLabel);

    uiScaleSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    uiScaleSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 20);
    uiScaleSlider.setRange (appconfig::kUiScaleMin, appconfig::kUiScaleMax, 0.05);
    uiScaleSlider.setValue (appconfig::getUiScaleOverride(), juce::dontSendNotification);
    uiScaleSlider.setNumDecimalPlacesToDisplay (2);
    uiScaleSlider.setTextValueSuffix ("x");
    uiScaleSlider.setTooltip (
        "Multiplier applied on top of the OS-reported "
        "display DPI. 1.00x = follow the OS. Range "
        "0.50x to 2.00x.");

    // Defer the actual setGlobalScaleFactor call until the user RELEASES
    // the slider (or commits a typed value). Applying mid-drag re-lays out
    // the slider itself, which makes the drag handle chase the mouse -
    // very jumpy. The slider's own textbox still updates live so the user
    // sees the target value during drag; only the world reflows on release.
    uiScaleSlider.onDragStart = [this] { uiScaleDragging = true; };
    uiScaleSlider.onDragEnd   = [this]
    {
        uiScaleDragging = false;
        applyUiScaleChange();
    };
    uiScaleSlider.onValueChange = [this]
    {
        if (! uiScaleDragging) applyUiScaleChange();
    };
    addAndMakeVisible (uiScaleSlider);

    uiScaleHint.setJustificationType (juce::Justification::centredLeft);
    uiScaleHint.setColour (juce::Label::textColourId, juce::Colour (0xff909094));
    uiScaleHint.setFont (juce::Font (juce::FontOptions (10.0f)));
    uiScaleHint.setText ("Saved per-machine; takes effect immediately.",
                          juce::dontSendNotification);
    addAndMakeVisible (uiScaleHint);
}

void AudioSettingsPanel::resized()
{
    auto area = getLocalBounds();

    // Bottom row: Periods + Oversampling + Rescan + Self-Test buttons.
    auto bottom = area.removeFromBottom (32);
#if defined(__linux__)
    periodsLabel.setBounds       (bottom.removeFromLeft (180).reduced (4, 4));
    periodsCombo.setBounds       (bottom.removeFromLeft (100).reduced (4, 4));
#endif
    oversamplingLabel.setBounds  (bottom.removeFromLeft (160).reduced (4, 4));
    oversamplingCombo.setBounds  (bottom.removeFromLeft (120).reduced (4, 4));
    selfTestButton.setBounds     (bottom.removeFromRight (160).reduced (4, 4));
    rescanButton.setBounds       (bottom.removeFromRight (140).reduced (4, 4));

    // Row above: UI scale slider + hint.
    auto scaleRow = area.removeFromBottom (28);
    uiScaleLabel.setBounds  (scaleRow.removeFromLeft (180).reduced (4, 2));
    uiScaleSlider.setBounds (scaleRow.removeFromLeft (260).reduced (4, 2));
    uiScaleHint.setBounds   (scaleRow.reduced (4, 2));

    // MTC OUTPUT row: emit toggle + frame-rate dropdown. Sits above
    // the Clock output row because MTC and Clock multiplex onto the
    // same Sync Output port — the rate dropdown only makes sense for
    // MTC, so it lives in the MTC row.
    auto mtcOutRow = area.removeFromBottom (28);
    mtcEmitFrameRateLabel.setBounds (mtcOutRow.removeFromLeft (180).reduced (4, 2));
    mtcEmitFrameRateCombo.setBounds (mtcOutRow.removeFromLeft (140).reduced (4, 2));
    mtcEmitToggle.setBounds (mtcOutRow.reduced (8, 2));

    // MIDI Clock OUTPUT row (master mode): output picker + emit toggle.
    auto syncOutRow = area.removeFromBottom (28);
    syncOutputLabel.setBounds (syncOutRow.removeFromLeft (180).reduced (4, 2));
    syncOutputCombo.setBounds (syncOutRow.removeFromLeft (300).reduced (4, 2));
    syncEmitClockToggle.setBounds (syncOutRow.reduced (8, 2));

    // MTC INPUT row: chase toggle only (the picker + frame-rate auto-
    // detected from incoming QF nibble 7 / full-frame sysex).
    auto mtcInRow = area.removeFromBottom (28);
    mtcInRow.removeFromLeft (180 + 300 + 8);   // align with toggle column
    mtcChaseToggle.setBounds (mtcInRow.reduced (4, 2));

    // MIDI Clock INPUT row: source picker + chase toggle.
    auto syncRow = area.removeFromBottom (28);
    syncSourceLabel.setBounds (syncRow.removeFromLeft (180).reduced (4, 2));
    syncSourceCombo.setBounds (syncRow.removeFromLeft (300).reduced (4, 2));
    syncChaseTransportToggle.setBounds (syncRow.reduced (8, 2));

    // MCU control surface OUTPUT row: motor faders / LEDs / LCD / meters.
    auto mcuOutRow = area.removeFromBottom (28);
    mcuOutputLabel.setBounds (mcuOutRow.removeFromLeft (180).reduced (4, 2));
    mcuOutputCombo.setBounds (mcuOutRow.removeFromLeft (300).reduced (4, 2));

    // MCU control surface INPUT row: faders / encoders / buttons.
    auto mcuInRow = area.removeFromBottom (28);
    mcuInputLabel.setBounds (mcuInRow.removeFromLeft (180).reduced (4, 2));
    mcuInputCombo.setBounds (mcuInRow.removeFromLeft (300).reduced (4, 2));

    // MIDI Bindings entry button. Right-aligned on its own row so the
    // sync rows above stay clean.
    auto bindingsRow = area.removeFromBottom (28);
    midiBindingsButton.setBounds (
        bindingsRow.removeFromLeft (180).reduced (4, 2));

    selector->setBounds (area);
}

void AudioSettingsPanel::openSelfTest()
{
    auto* panel = new SelfTestPanel (engine, deviceManager, session);
    panel->setSize (760, 560);

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned (panel);
    opts.dialogTitle = "Audio Pipeline Self-Test";
    opts.dialogBackgroundColour = juce::Colour (0xff202024);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = true;
    opts.resizable = true;
    if (auto* dw = opts.launchAsync()) dw->toFront (true);
}

void AudioSettingsPanel::applyUiScaleChange()
{
    const float scale = (float) uiScaleSlider.getValue();
    appconfig::setUiScaleOverride (scale);
    juce::Desktop::getInstance().setGlobalScaleFactor (scale);
}

void AudioSettingsPanel::applyRescan()
{
    // Re-enumerate every registered audio backend. AudioIODeviceType's
    // scanForDevices() repopulates the type's internal device list; the
    // listener notification path (fired by callDeviceChangeListeners() on
    // our ALSA type, and by JUCE's own backends on theirs) tells the
    // AudioDeviceSelectorComponent to re-query and rebuild its dropdowns.
    //
    // We iterate every type rather than only the current one so that
    // switching backend (e.g. ALSA -> JACK) after a hot-plug still sees
    // the freshly-enumerated devices on the new backend.
    const auto& types = deviceManager.getAvailableDeviceTypes();
    for (auto* type : types)
        if (type != nullptr)
            type->scanForDevices();

    // The selector subscribes to AudioDeviceManager change broadcasts. Most
    // backends' scanForDevices() will already fire callDeviceChangeListeners()
    // (which routes through audioDeviceListChanged() → sendChangeMessage()),
    // but some only broadcast on a real diff. Force a refresh either way.
    // Re-applying the same setup via setAudioDeviceSetup is a no-op when
    // newSetup == currentSetup (JUCE early-returns without notifying), so
    // poke the broadcaster directly.
    deviceManager.sendChangeMessage();

    // Same rescan also re-enumerates MIDI inputs so freshly plugged-in
    // controllers appear in the per-strip MIDI dropdowns. The engine
    // detaches its callbacks for the duration so audio briefly drops
    // out, which is acceptable for a user-triggered rescan; tracks
    // re-resolve their saved device identifiers to current indices on
    // completion.
    engine.refreshMidiInputs();
}

#if defined(__linux__)
void AudioSettingsPanel::applyPeriodsChange()
{
    const int p = periodsCombo.getSelectedId();
    if (p <= 0) return;

    AlsaAudioIODevice::setRequestedPeriods (p);

    // Re-open the device with the same setup so setParameters() runs and
    // picks up the new period count. Without this, the change only takes
    // effect on the next manual device switch.
    auto setup = deviceManager.getAudioDeviceSetup();
    deviceManager.setAudioDeviceSetup (setup, /*treatAsChosenDevice*/ true);
}
#endif

void AudioSettingsPanel::applyOversamplingChange()
{
    const int factor = oversamplingCombo.getSelectedId();
    if (factor != 1 && factor != 2 && factor != 4) return;

    session.oversamplingFactor.store (factor, std::memory_order_relaxed);

    // Re-prepare engine DSP so the new factor takes effect on the next
    // callback. Bouncing setAudioDeviceSetup forces audioDeviceAboutToStart
    // → AudioEngine::prepareForSelfTest → master/aux prepare(...,factor),
    // which is the cheapest way to apply the change without restarting.
    auto setup = deviceManager.getAudioDeviceSetup();
    deviceManager.setAudioDeviceSetup (setup, /*treatAsChosenDevice*/ true);
}

AudioSettingsPanel::~AudioSettingsPanel()
{
    engine.removeChangeListener (this);
}

void AudioSettingsPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Engine broadcasts after refreshMidiInputs / hot-plug. Repopulate
    // so a newly-arrived device appears in the picker (and conversely,
    // a removed one disappears + falls back to "(none)" if it was
    // selected). Both input + output combos refresh - the rebuild path
    // touches both banks.
    populateSyncSourceCombo();
    populateSyncOutputCombo();
    populateMcuInputCombo();
    populateMcuOutputCombo();
}

void AudioSettingsPanel::populateSyncSourceCombo()
{
    // ID 1 = "(none)"; IDs 2..N+1 map to engine.midiInputDevices[i-2].
    // Identifier-based selection because indices shift on hot-plug.
    syncSourceCombo.clear (juce::dontSendNotification);
    syncSourceCombo.addItem ("(none)", 1);
    const auto& devices = engine.getMidiInputDevices();
    int matchId = 1;
    for (int i = 0; i < devices.size(); ++i)
    {
        syncSourceCombo.addItem (devices[i].name, i + 2);
        if (devices[i].identifier == session.syncSourceInputIdentifier)
            matchId = i + 2;
    }
    syncSourceCombo.setSelectedId (matchId, juce::dontSendNotification);
}

void AudioSettingsPanel::applySyncSourceChange()
{
    const int id = syncSourceCombo.getSelectedId();
    if (id <= 1)
    {
        session.syncSourceInputIdentifier = juce::String();
        session.syncSourceInputIdx.store (-1, std::memory_order_release);
        return;
    }
    const auto& devices = engine.getMidiInputDevices();
    const int idx = id - 2;
    if (idx < 0 || idx >= devices.size()) return;
    session.syncSourceInputIdentifier = devices[idx].identifier;
    session.syncSourceInputIdx.store (idx, std::memory_order_release);
}

void AudioSettingsPanel::populateSyncOutputCombo()
{
    // Identical shape to populateSyncSourceCombo but reads from the
    // engine's MIDI OUTPUT bank.
    syncOutputCombo.clear (juce::dontSendNotification);
    syncOutputCombo.addItem ("(none)", 1);
    const auto& devices = engine.getMidiOutputDevices();
    int matchId = 1;
    for (int i = 0; i < devices.size(); ++i)
    {
        syncOutputCombo.addItem (devices[i].name, i + 2);
        if (devices[i].identifier == session.syncOutputIdentifier)
            matchId = i + 2;
    }
    syncOutputCombo.setSelectedId (matchId, juce::dontSendNotification);
}

void AudioSettingsPanel::applySyncOutputChange()
{
    const int id = syncOutputCombo.getSelectedId();
    if (id <= 1)
    {
        session.syncOutputIdentifier = juce::String();
        session.syncOutputIdx.store (-1, std::memory_order_release);
        return;
    }
    const auto& devices = engine.getMidiOutputDevices();
    const int idx = id - 2;
    if (idx < 0 || idx >= devices.size()) return;
    session.syncOutputIdentifier = devices[idx].identifier;
    session.syncOutputIdx.store (idx, std::memory_order_release);
    // Eagerly open the port so the first audio-thread emission doesn't
    // race with a synchronous ALSA snd_seq_connect.
    engine.ensureMidiOutputOpen (idx);
}

void AudioSettingsPanel::populateMcuInputCombo()
{
    mcuInputCombo.clear (juce::dontSendNotification);
    mcuInputCombo.addItem ("(none)", 1);
    const auto& devices = engine.getMidiInputDevices();
    int matchId = 1;
    for (int i = 0; i < devices.size(); ++i)
    {
        mcuInputCombo.addItem (devices[i].name, i + 2);
        if (devices[i].identifier == session.mcu.inputIdentifier)
            matchId = i + 2;
    }
    mcuInputCombo.setSelectedId (matchId, juce::dontSendNotification);
}

void AudioSettingsPanel::populateMcuOutputCombo()
{
    mcuOutputCombo.clear (juce::dontSendNotification);
    mcuOutputCombo.addItem ("(none)", 1);
    const auto& devices = engine.getMidiOutputDevices();
    int matchId = 1;
    for (int i = 0; i < devices.size(); ++i)
    {
        mcuOutputCombo.addItem (devices[i].name, i + 2);
        if (devices[i].identifier == session.mcu.outputIdentifier)
            matchId = i + 2;
    }
    mcuOutputCombo.setSelectedId (matchId, juce::dontSendNotification);
}

void AudioSettingsPanel::applyMcuInputChange()
{
    const int id = mcuInputCombo.getSelectedId();
    if (id <= 1)
    {
        session.mcu.inputIdentifier = juce::String();
        session.mcu.resolvedInputIdx.store (-1, std::memory_order_release);
        return;
    }
    const auto& devices = engine.getMidiInputDevices();
    const int idx = id - 2;
    if (idx < 0 || idx >= devices.size()) return;
    session.mcu.inputIdentifier = devices[idx].identifier;
    session.mcu.resolvedInputIdx.store (idx, std::memory_order_release);
}

void AudioSettingsPanel::applyMcuOutputChange()
{
    const int id = mcuOutputCombo.getSelectedId();
    if (id <= 1)
    {
        session.mcu.outputIdentifier = juce::String();
        session.mcu.resolvedOutputIdx.store (-1, std::memory_order_release);
        return;
    }
    const auto& devices = engine.getMidiOutputDevices();
    const int idx = id - 2;
    if (idx < 0 || idx >= devices.size()) return;
    session.mcu.outputIdentifier = devices[idx].identifier;
    session.mcu.resolvedOutputIdx.store (idx, std::memory_order_release);
    // Eagerly open so the first 30 Hz emit doesn't race with ALSA's
    // synchronous snd_seq_connect.
    engine.ensureMidiOutputOpen (idx);
}
} // namespace duskstudio
