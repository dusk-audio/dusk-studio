#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../builtin/BuiltinRegistry.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../foundation/Base64.h"
#include "../../../session/MidiBindings.h"
#include "../../../session/Session.h"

#if DUSKSTUDIO_HAS_ALSA
#include <alsa/asoundlib.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_ALSA
void checkHotplugWhileRolling (ScenarioContext& ctx, std::shared_ptr<snd_seq_t> portClient,
                               bool recording, std::function<void()> onDone)
{
    auto& engine = ctx.engine();
    if (recording) engine.record();
    else engine.play();
    const std::string portName = recording ? "recording" : "playing";
    const int port = snd_seq_create_simple_port (portClient.get(), portName.c_str(),
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ, SND_SEQ_PORT_TYPE_APPLICATION);
    if (! ctx.expect (port >= 0, "could not create a virtual MIDI port"))
    {
        ctx.complete (ctx.verdict());
        return;
    }
    const std::string suffix = ":" + portName;
    const std::string clientId = "alsa-seq:dusk-hotplug-" + std::to_string (snd_seq_client_id (portClient.get()));
    const auto visible = [&engine, identifier = clientId + suffix]
    {
        const auto& devices = engine.getMidiInputDevices();
        return std::any_of (devices.begin(), devices.end(), [&identifier] (const auto& device)
        { return device.identifier == identifier; });
    };
    ctx.later (1200, [&ctx, &engine, recording, visible, portClient, onDone = std::move (onDone)]
    {
        ctx.expect (recording ? engine.getTransport().isRecording() : engine.getTransport().isPlaying(),
                    "the transport stopped before the hot-plug check");
        ctx.expect (! visible(), "MIDI ports rebuilt while the transport was rolling");
        engine.stop();
        ctx.waitUntil (visible, 4000, std::move (onDone), "the new MIDI port did not appear after Stop");
    });
}

void checkHotplugWhileRendering (ScenarioContext& ctx, std::shared_ptr<snd_seq_t> portClient)
{
    auto& engine = ctx.engine();
    engine.setOfflineRenderActive (true);
    const int port = snd_seq_create_simple_port (portClient.get(), "rendering",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ, SND_SEQ_PORT_TYPE_APPLICATION);
    if (! ctx.expect (port >= 0, "could not create a virtual MIDI port during render"))
    {
        ctx.complete (ctx.verdict());
        return;
    }
    const auto identifier = "alsa-seq:dusk-hotplug-" + std::to_string (snd_seq_client_id (portClient.get()))
                          + ":rendering";
    const auto visible = [&engine, identifier]
    {
        const auto& devices = engine.getMidiInputDevices();
        return std::any_of (devices.begin(), devices.end(), [&identifier] (const auto& device)
        { return device.identifier == identifier; });
    };
    ctx.later (1200, [&ctx, &engine, portClient, visible]
    {
        ctx.expect (! visible(), "MIDI ports rebuilt during an offline render");
        engine.refreshMidiInputs();
        ctx.expect (! visible(), "an explicit MIDI refresh rebuilt ports during an offline render");
        engine.setOfflineRenderActive (false);
        ctx.waitUntil (visible, 4000, [&ctx] { ctx.complete (ctx.verdict()); },
                       "the new MIDI port did not appear after the render");
    });
}

std::optional<ScenarioResult> hotplugWaitsForStop (ScenarioContext& ctx)
{
    snd_seq_t* raw = nullptr;
    if (snd_seq_open (&raw, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0)
        return ScenarioResult::skip ("ALSA sequencer is unavailable");
    auto portClient = std::shared_ptr<snd_seq_t> (raw, snd_seq_close);
    const auto name = "dusk-hotplug-" + std::to_string (snd_seq_client_id (raw));
    if (snd_seq_set_client_name (raw, name.c_str()) < 0)
        return ScenarioResult::fail ("could not name the virtual MIDI client");
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    ctx.keep (session.track (0).mode);
    ctx.keep (session.track (0).midiInputIndex);
    session.track (0).mode.store ((int) Track::Mode::Midi);
    session.track (0).midiInputIndex.store (engine.getVirtualKeyboardInputIndex());
    session.setTrackArmed (0, true);
    ctx.cleanup ([&engine, &session, portClient]
    {
        engine.setOfflineRenderActive (false);
        engine.stop();
        session.setTrackArmed (0, false);
    });
    checkHotplugWhileRolling (ctx, portClient, false, [&ctx, portClient]
    {
        checkHotplugWhileRolling (ctx, portClient, true, [&ctx, portClient]
        { checkHotplugWhileRendering (ctx, portClient); });
    });
    return std::nullopt;
}
#endif

constexpr int kInput = 0;

void publish (Session& session, std::vector<MidiBinding> binds)
{
    session.midiBindings.publish (std::make_unique<std::vector<MidiBinding>> (std::move (binds)));
}

MidiBinding ccBinding (int cc, MidiBindingTarget target, int index)
{
    MidiBinding b;
    b.trigger = MidiBindingTrigger::CC;
    b.channel = 0;
    b.dataNumber = cc;
    b.target = target;
    b.targetIndex = index;
    return b;
}

dusk::MidiBuffer cc (int channel, int number, int value)
{
    const std::uint8_t bytes[3] { (std::uint8_t) (0xB0 | (channel - 1)),
                                  (std::uint8_t) number, (std::uint8_t) value };
    dusk::MidiBuffer buffer;
    buffer.addEvent (bytes, 3, 0);
    return buffer;
}

dusk::MidiBuffer noteOn (int channel, int number, int velocity)
{
    const std::uint8_t bytes[3] { (std::uint8_t) (0x90 | (channel - 1)),
                                  (std::uint8_t) number, (std::uint8_t) velocity };
    dusk::MidiBuffer buffer;
    buffer.addEvent (bytes, 3, 0);
    return buffer;
}

dusk::MidiBuffer mmc (int command)
{
    const std::uint8_t bytes[6] { 0xF0, 0x7F, 0x7F, 0x06, (std::uint8_t) command, 0xF7 };
    dusk::MidiBuffer buffer;
    buffer.addEvent (bytes, 6, 0);
    return buffer;
}

// Leaves the session's bindings as they were found, so a scenario's controller
// setup never leaks into the next one.
void restoreBindings (ScenarioContext& ctx)
{
    auto found = ctx.session().midiBindings.current();
    ctx.cleanup ([&ctx, found] { publish (ctx.session(), found); });
}

ScenarioResult runTargets (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    constexpr int kTrack = 5;
    auto& strip = session.track (kTrack).strip;

    ctx.keep (strip.faderDb);
    ctx.keep (strip.pan);
    ctx.keep (strip.mute);
    ctx.keep (session.pendingTransportAction);
    constexpr int kBus = 2;
    auto& bus = session.bus (kBus).strip;
    ctx.keep (bus.hpfEnabled);
    ctx.keep (bus.hpfFreq);
    restoreBindings (ctx);

    MidiBinding mute;
    mute.trigger = MidiBindingTrigger::Note;
    mute.dataNumber = 60;
    mute.target = MidiBindingTarget::TrackMute;
    mute.targetIndex = kTrack;

    MidiBinding play;
    play.trigger = MidiBindingTrigger::MmcCommand;
    play.dataNumber = 2;
    play.target = MidiBindingTarget::TransportPlay;

    publish (session, { ccBinding (23, MidiBindingTarget::TrackFader, kTrack),
                        ccBinding (24, MidiBindingTarget::TrackPan, kTrack),
                        ccBinding (25, MidiBindingTarget::BusHpfFreq, kBus),
                        mute, play });
    ctx.pump (1);

    ctx.pumpWithMidi (kInput, cc (1, 23, 127));
    const float top = strip.faderDb.load (std::memory_order_relaxed);
    ctx.note ("fader at CC 127: " + std::to_string (top) + " dB");
    ctx.expect (std::abs (top - 12.0f) < 0.5f, "a full CC did not take the fader to +12 dB");

    ctx.pumpWithMidi (kInput, cc (1, 23, 0));
    const float bottom = strip.faderDb.load (std::memory_order_relaxed);
    ctx.note ("fader at CC 0: " + std::to_string (bottom) + " dB");
    ctx.expect (bottom < -80.0f, "a zeroed CC did not take the fader to the bottom");

    ctx.pumpWithMidi (kInput, cc (1, 24, 127));
    ctx.expect (std::abs (strip.pan.load (std::memory_order_relaxed) - 1.0f) < 0.02f,
                "a full CC did not pan hard right");
    ctx.pumpWithMidi (kInput, cc (1, 24, 64));
    ctx.expect (std::abs (strip.pan.load (std::memory_order_relaxed)) < 0.02f,
                "a centred CC did not centre the pan");

    // The bus highpass sweeps 20 Hz..3 kHz over the travel; the bottom is OFF.
    ctx.pumpWithMidi (kInput, cc (1, 25, 127));
    ctx.note ("bus highpass at CC 127: " + std::to_string (bus.hpfFreq.load()) + " Hz");
    ctx.expect (bus.hpfEnabled.load (std::memory_order_relaxed)
                    && std::abs (bus.hpfFreq.load (std::memory_order_relaxed) - BusParams::kHpfMaxHz) < 1.0f,
                "a full CC did not take the bus highpass to 3 kHz");
    ctx.pumpWithMidi (kInput, cc (1, 25, 0));
    ctx.expect (! bus.hpfEnabled.load (std::memory_order_relaxed)
                    && std::abs (bus.hpfFreq.load (std::memory_order_relaxed) - BusParams::kHpfOffHz) < 0.01f,
                "a zeroed CC did not turn the bus highpass off");

    const bool wasMuted = strip.mute.load (std::memory_order_relaxed);
    ctx.pumpWithMidi (kInput, noteOn (1, 60, 127));
    ctx.expect (strip.mute.load (std::memory_order_relaxed) != wasMuted,
                "a bound note did not flip mute");

    // A CC on a number nothing is bound to leaves everything alone.
    const float held = strip.pan.load (std::memory_order_relaxed);
    ctx.pumpWithMidi (kInput, cc (1, 99, 0));
    ctx.expect (std::abs (strip.pan.load (std::memory_order_relaxed) - held) < 1.0e-6f,
                "an unbound CC moved a bound target");

    session.pendingTransportAction.store ((int) PendingTransportAction::None,
                                          std::memory_order_relaxed);
    ctx.pumpWithMidi (kInput, mmc (2));
    ctx.expect (session.pendingTransportAction.load (std::memory_order_acquire)
                    == (int) PendingTransportAction::Play,
                "an MMC play command did not queue the transport");

    return ctx.verdict();
}

// Keeps a strip's format-7 EQ dials as they are now, restored after the
// frequencies they pair with: cleanups run latest-registered first, so call
// this before keeping the frequencies.
void keepEqDials (ScenarioContext& ctx, ChannelStripParams& strip)
{
    std::array<std::uint64_t, ChannelStripParams::kNumEqFreqs> words {};
    for (size_t i = 0; i < words.size(); ++i) words[i] = strip.eqFreqDial[i].raw();
    ctx.cleanup ([&strip, words]
    {
        for (size_t i = 0; i < words.size(); ++i) strip.eqFreqDial[i].setRaw (words[i]);
    });
}

// A converted older session's band plays its format-7 dial until something
// moves its frequency. A bound controller does, on the audio thread, and moves
// only the band or filter it is bound to.
ScenarioResult runEqFreqDropsDial (ScenarioContext& ctx)
{
    using EqFreq = ChannelStripParams::EqFreq;
    auto& session = ctx.session();
    constexpr int kTrack = 6;
    auto& strip = session.track (kTrack).strip;
    keepEqDials (ctx, strip);
    for (const auto f : { EqFreq::Hpf, EqFreq::Lf, EqFreq::Lm })
        ctx.keep (strip.eqFreq (f));
    ctx.keep (strip.hpfEnabled);
    restoreBindings (ctx);

    const bool black = strip.eqBlackMode.load();
    const auto dialOf = [&strip, black] (EqFreq f)
    {
        float hz = 0.0f;
        return strip.legacyDial (f).dialFor (strip.eqFreq (f), black, hz);
    };
    strip.legacyDial (EqFreq::Hpf).set (40.0f, strip.hpfFreq.load(), black);
    strip.legacyDial (EqFreq::Lf).set (100.0f, strip.lfFreq.load(), black);
    strip.legacyDial (EqFreq::Lm).set (600.0f, strip.lmFreq.load(), black);

    publish (session, { ccBinding (25, MidiBindingTarget::TrackEqFreq, packTrackEqBand (kTrack, 0)),
                        ccBinding (26, MidiBindingTarget::TrackHpfFreq, kTrack) });
    ctx.pump (1);
    ctx.expect (dialOf (EqFreq::Lf) > 0.0f && dialOf (EqFreq::Lm) > 0.0f && dialOf (EqFreq::Hpf) > 0.0f,
                "the dials did not hold before any controller moved");

    const float lfBefore = strip.lfFreq.load();
    ctx.pumpWithMidi (kInput, cc (1, 25, 90));
    ctx.expect (std::abs (strip.lfFreq.load() - lfBefore) > 1.0f, "the bound CC did not move the LF band");
    ctx.expect (strip.legacyDial (EqFreq::Lf).raw() == 0, "the bound CC kept the LF band's dial");
    ctx.expect (dialOf (EqFreq::Lm) > 0.0f && dialOf (EqFreq::Hpf) > 0.0f,
                "the LF band's CC dropped another band's dial");

    ctx.pumpWithMidi (kInput, cc (1, 26, 64));
    ctx.expect (strip.hpfEnabled.load() && strip.legacyDial (EqFreq::Hpf).raw() == 0,
                "the bound HPF CC kept the filter's dial");
    ctx.expect (dialOf (EqFreq::Lm) > 0.0f, "the HPF's CC dropped the LM band's dial");
    return ctx.verdict();
}

// A bound controller is a move of the control it is bound to, as the knob on
// screen is: a band's gain, frequency or Q engages a bypassed channel EQ, and
// so does the HPF leaving OFF, while turning it back to OFF leaves the EQ as it
// is. The bus EQ's gains and highpass engage the bus EQ, and the compressor's
// threshold and makeup engage the compressor.
ScenarioResult runMovesEngage (ScenarioContext& ctx)
{
    using EqFreq = ChannelStripParams::EqFreq;
    auto& session = ctx.session();
    constexpr int kTrack = 7;   // position 7 of the first bank, for the banked binding
    constexpr int kBus = 1;
    auto& strip = session.track (kTrack).strip;
    auto& bus = session.bus (kBus).strip;
    keepEqDials (ctx, strip);
    for (const auto f : { EqFreq::Hpf, EqFreq::Lm })
        ctx.keep (strip.eqFreq (f));
    for (auto* value : { &strip.lfGainDb, &strip.hfGainDb, &strip.hmQ,
                         &strip.compFetThresholdDb, &strip.compFetOutput })
        ctx.keep (*value);
    for (auto* value : { &strip.eqEnabled, &strip.hpfEnabled, &strip.compEnabled,
                         &bus.eqEnabled, &bus.hpfEnabled })
        ctx.keep (*value);
    for (auto* value : { &bus.hpfFreq, &bus.eqMidGainDb })
        ctx.keep (*value);
    ctx.keep (strip.compMode);
    ctx.keep (session.activeBank);
    restoreBindings (ctx);

    publish (session, { ccBinding (40, MidiBindingTarget::TrackEqGain, packTrackEqBand (kTrack, 0)),
                        ccBinding (41, MidiBindingTarget::TrackEqFreq, packTrackEqBand (kTrack, 1)),
                        ccBinding (42, MidiBindingTarget::TrackEqQ, packTrackEqBand (kTrack, 2)),
                        ccBinding (43, MidiBindingTarget::TrackEqQ, packTrackEqBand (kTrack, 0)),
                        ccBinding (44, MidiBindingTarget::TrackHpfFreq, kTrack),
                        ccBinding (45, MidiBindingTarget::TrackEqGainBank, packTrackEqBand (kTrack, 3)),
                        ccBinding (46, MidiBindingTarget::BusEqGain, packBusEqBand (kBus, 1)),
                        ccBinding (47, MidiBindingTarget::BusHpfFreq, kBus),
                        ccBinding (48, MidiBindingTarget::TrackCompThresh, kTrack),
                        ccBinding (49, MidiBindingTarget::TrackCompMakeup, kTrack) });
    session.activeBank.store (0, std::memory_order_relaxed);
    strip.compMode.store (1, std::memory_order_relaxed);   // FET
    ctx.pump (1);

    const auto engages = [&ctx] (std::atomic<bool>& flag, int number, int value, const std::string& what)
    {
        flag.store (false, std::memory_order_relaxed);
        ctx.pumpWithMidi (kInput, cc (1, number, value));
        ctx.note (what + ": " + (flag.load (std::memory_order_relaxed) ? "engaged" : "left bypassed"));
        return flag.load (std::memory_order_relaxed);
    };

    ctx.expect (engages (strip.eqEnabled, 40, 100, "LF gain"), "a bound LF gain left the EQ bypassed");
    ctx.expect (engages (strip.eqEnabled, 41, 100, "LM frequency"), "a bound LM frequency left the EQ bypassed");
    ctx.expect (engages (strip.eqEnabled, 42, 100, "HM Q"), "a bound HM Q left the EQ bypassed");
    ctx.expect (engages (strip.eqEnabled, 45, 100, "banked HF gain"), "a banked HF gain left the EQ bypassed");
    ctx.expect (! engages (strip.eqEnabled, 43, 100, "LF Q"),
                "a Q binding on the LF shelf, which has no Q, engaged the EQ");

    ctx.expect (! engages (strip.eqEnabled, 44, 0, "HPF at OFF"), "the HPF held at OFF engaged the EQ");
    ctx.expect (engages (strip.eqEnabled, 44, 64, "HPF up off OFF"), "turning the HPF up off OFF left the EQ bypassed");
    ctx.expect (strip.hpfEnabled.load (std::memory_order_relaxed), "turning the HPF up off OFF left it off");
    ctx.pumpWithMidi (kInput, cc (1, 44, 0));
    ctx.expect (! strip.hpfEnabled.load (std::memory_order_relaxed) && strip.eqEnabled.load (std::memory_order_relaxed),
                "turning the HPF down to OFF did not switch it off and leave the EQ engaged");

    ctx.expect (engages (bus.eqEnabled, 46, 100, "bus MID gain"), "a bound bus MID gain left the bus EQ bypassed");
    ctx.expect (! engages (bus.eqEnabled, 47, 0, "bus HPF at OFF"), "the bus highpass held at OFF engaged the bus EQ");
    ctx.expect (engages (bus.eqEnabled, 47, 64, "bus HPF up off OFF"),
                "turning the bus highpass up off OFF left the bus EQ bypassed");
    ctx.pumpWithMidi (kInput, cc (1, 47, 0));
    ctx.expect (! bus.hpfEnabled.load (std::memory_order_relaxed) && bus.eqEnabled.load (std::memory_order_relaxed),
                "turning the bus highpass down to OFF did not switch it off and leave the bus EQ engaged");

    ctx.expect (engages (strip.compEnabled, 48, 40, "comp threshold"), "a bound threshold left the compressor bypassed");
    ctx.expect (engages (strip.compEnabled, 49, 80, "comp makeup"), "a bound makeup left the compressor bypassed");
    return ctx.verdict();
}

ScenarioResult runButtonModes (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& mute = session.track (3).strip.mute;
    auto& other = session.track (4).strip.mute;
    auto& pan = session.track (6).strip.pan;
    ctx.keep (mute); ctx.keep (other); ctx.keep (pan);
    ctx.keep (session.pendingTransportAction);
    ctx.keep (session.midiLearnPending); ctx.keep (session.midiLearnCapture);
    for (int track = 0; track < Session::kNumTracks; ++track)
    { ctx.keep (session.track (track).midiInputIndex); ctx.keep (session.track (track).midiOutputIndex); }
    ctx.keep (session.syncSourceInputIdx); ctx.keep (session.syncOutputIdx);
    ctx.keep (session.mcu.resolvedInputIdx); ctx.keep (session.mcu.resolvedOutputIdx);
    restoreBindings (ctx);
    const bool hardware = engine.setHardwareMidiInputsEnabledForScenario (false);
    const bool takeover = engine.getMidiSoftTakeover();
    engine.setMidiSoftTakeover (false);
    ctx.cleanup ([&engine, hardware, takeover]
    { engine.setHardwareMidiInputsEnabledForScenario (hardware); engine.setMidiSoftTakeover (takeover); });
    session.midiLearnPending.store (-1);
    session.pendingTransportAction.store (static_cast<int> (PendingTransportAction::None));
    int messages = 0, checks = 0;
    const auto check = [&] (bool condition, const std::string& text)
    { ++checks; return ctx.expect (condition, text); };
    const auto send = [&] (dusk::MidiBuffer events)
    {
        for (const auto event : events) { (void) event; ++messages; }
        ctx.pumpWithMidi (kInput, std::move (events));
    };
    const auto sendCc = [&] (int channel, int number, int value) { send (cc (channel, number, value)); };
    const auto raw = [&] (int status, int number, int value)
    {
        const std::uint8_t data[] { static_cast<std::uint8_t> (status),
                                  static_cast<std::uint8_t> (number), static_cast<std::uint8_t> (value) };
        dusk::MidiBuffer events; events.addEvent (data, 3, 0); send (std::move (events));
    };
    auto press = ccBinding (40, MidiBindingTarget::TrackMute, 3);
    press.buttonMode = MidiButtonMode::Press;
    publish (session, { press });
    sendCc (1, 40, 0); mute.store (false);
    const std::array<int, 4> values { 127, 127, 0, 127 };
    const std::array<bool, 4> risingStates { true, true, true, false };
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        sendCc (1, 40, values[i]);
        check (mute.load() == risingStates[i], "CC Press step " + std::to_string (i) + " did not follow rising edges");
    }
    sendCc (1, 40, 0); mute.store (false);
    dusk::MidiBuffer batch;
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        const std::uint8_t data[] { 0xB0, 40, static_cast<std::uint8_t> (values[i]) };
        batch.addEvent (data, 3, static_cast<int> (i));
    }
    send (std::move (batch));
    check (! mute.load(), "multiple presses in one block did not use event order");

    press.dataNumber = 41; publish (session, { press }); mute.store (false);
    const std::array<int, 5> thresholds { 63, 64, 65, 63, 64 };
    const std::array<bool, 5> thresholdStates { false, true, true, true, false };
    for (std::size_t i = 0; i < thresholds.size(); ++i)
    {
        sendCc (1, 41, thresholds[i]);
        check (mute.load() == thresholdStates[i], "CC threshold step " + std::to_string (i) + " was not edge-triggered");
    }
    auto toggle = press; toggle.dataNumber = 40; toggle.buttonMode = MidiButtonMode::Toggle;
    publish (session, { toggle }); mute.store (false);
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        sendCc (1, 40, values[i]);
        check (mute.load() == (i % 2 == 0), "CC Toggle step " + std::to_string (i) + " did not fire on every message");
    }

    auto note = press; note.trigger = MidiBindingTrigger::Note; note.dataNumber = 60;
    publish (session, { note }); raw (0x80, 60, 0); mute.store (false);
    const std::array<std::array<int, 3>, 8> notes {{
        {{0x90, 1, 1}}, {{0x90, 1, 1}}, {{0x80, 64, 1}}, {{0x90, 127, 0}},
        {{0x90, 0, 0}}, {{0x90, 1, 1}}, {{0x80, 0, 1}}, {{0x90, 1, 0}}
    }};
    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        raw (notes[i][0], 60, notes[i][1]);
        check (mute.load() == (notes[i][2] != 0), "Note Press step " + std::to_string (i) + " did not honor positive velocity and releases");
    }
    note.buttonMode = MidiButtonMode::Toggle; publish (session, { note }); mute.store (false);
    raw (0x90, 60, 1); check (mute.load(), "Note Toggle ignored velocity one");
    raw (0x90, 60, 1); check (! mute.load(), "Note Toggle suppressed a repeated positive NoteOn");
    raw (0x80, 60, 127); check (! mute.load(), "Note Toggle fired on NoteOff");
    raw (0x90, 60, 0); check (! mute.load(), "Note Toggle fired on zero-velocity NoteOn");
    raw (0x90, 60, 127); check (mute.load(), "Note Toggle did not rearm after release");
    raw (0x80, 60, 1); check (mute.load(), "Note Toggle release changed mute");

    publish (session, {}); session.midiLearnPending.store (0); session.midiLearnCapture.store (0);
    raw (0x80, 71, 127);
    check (! learnCaptureIsValid (session.midiLearnCapture.load()), "MIDI Learn captured NoteOff");
    raw (0x90, 71, 0);
    check (! learnCaptureIsValid (session.midiLearnCapture.load()), "MIDI Learn captured zero-velocity NoteOn");
    session.midiLearnPending.store (-1);

    auto continuous = ccBinding (90, MidiBindingTarget::TrackPan, 6);
    publish (session, { continuous }); sendCc (1, 90, 127);
    check (std::abs (pan.load() - 1.0f) < 1.0e-6f, "CC continuous high endpoint failed");
    pan.store (0); sendCc (1, 90, 127);
    check (std::abs (pan.load() - 1.0f) < 1.0e-6f, "CC edge tracking suppressed a repeated continuous value");
    sendCc (1, 90, 0);
    check (std::abs (pan.load() + 1.0f) < 1.0e-6f, "CC continuous zero was suppressed");
    continuous.trigger = MidiBindingTrigger::Note; continuous.dataNumber = 62;
    publish (session, { continuous }); raw (0x90, 62, 1);
    check (std::abs (pan.load() - (-1.0f + 2.0f / 127.0f)) < 1.0e-6f, "Note velocity was replaced by a button value");
    raw (0x90, 62, 63);
    check (std::abs (pan.load() - (-1.0f + 126.0f / 127.0f)) < 1.0e-6f, "Note continuous repeat was suppressed");
    raw (0x80, 62, 127);
    check (std::abs (pan.load() - (-1.0f + 126.0f / 127.0f)) < 1.0e-6f, "NoteOff changed a continuous binding");
    raw (0x90, 62, 127); raw (0x90, 62, 0);
    check (std::abs (pan.load() - 1.0f) < 1.0e-6f, "zero-velocity NoteOn changed a continuous binding");
    continuous.trigger = MidiBindingTrigger::PitchBend;
    publish (session, { continuous }); raw (0xE0, 0, 0);
    check (std::abs (pan.load() + 1.0f) < 1.0e-6f, "pitch bend low endpoint changed");
    raw (0xE0, 127, 127);
    check (std::abs (pan.load() - 1.0f) < 1.0e-6f, "pitch bend high endpoint changed");

    const std::array<MidiBindingTarget, 4> targets { MidiBindingTarget::TransportPlay, MidiBindingTarget::TransportStop,
        MidiBindingTarget::TransportRecord, MidiBindingTarget::TransportToggle };
    const std::array<PendingTransportAction, 4> actions { PendingTransportAction::Play, PendingTransportAction::Stop,
        PendingTransportAction::Record, PendingTransportAction::Toggle };
    for (std::size_t t = 0; t < targets.size(); ++t)
    {
        auto button = ccBinding (70 + static_cast<int> (t), targets[t], 0);
        publish (session, { button });
        const std::array<int, 5> presses { 0, 127, 127, 0, 127 };
        for (std::size_t i = 0; i < presses.size(); ++i)
        {
            session.pendingTransportAction.store (static_cast<int> (PendingTransportAction::None));
            sendCc (1, button.dataNumber, presses[i]);
            const auto expected = i == 1 || i == 4 ? actions[t] : PendingTransportAction::None;
            check (session.pendingTransportAction.load() == static_cast<int> (expected),
                   "transport Press target " + std::to_string (t) + " step " + std::to_string (i) + " did not follow edges");
        }
        button.buttonMode = MidiButtonMode::Toggle; publish (session, { button });
        for (int value : { 127, 0, 127 })
        {
            session.pendingTransportAction.store (static_cast<int> (PendingTransportAction::None));
            sendCc (1, button.dataNumber, value);
            check (session.pendingTransportAction.load() == static_cast<int> (actions[t]), "transport Toggle suppressed a message");
        }
    }
    auto machine = ccBinding (2, MidiBindingTarget::TransportPlay, 0); machine.trigger = MidiBindingTrigger::MmcCommand;
    publish (session, { machine });
    for (int i = 0; i < 2; ++i)
    {
        session.pendingTransportAction.store (static_cast<int> (PendingTransportAction::None)); send (mmc (2));
        check (session.pendingTransportAction.load() == static_cast<int> (PendingTransportAction::Play), "repeated MMC one-shot was suppressed");
    }
    session.pendingTransportAction.store (static_cast<int> (PendingTransportAction::None));

    press.dataNumber = 81; publish (session, { press }); mute.store (false);
    sendCc (1, 81, 127); check (mute.load(), "wildcard source did not fire on channel one");
    sendCc (2, 81, 127); check (! mute.load(), "two channels shared their held state");
    sendCc (1, 81, 0); check (! mute.load(), "channel-one release fired");
    sendCc (2, 81, 127); check (! mute.load(), "channel-one release rearmed channel two");
    sendCc (1, 81, 127); check (mute.load(), "channel-one release did not rearm channel one");

    press.dataNumber = 82; auto separate = press; separate.trigger = MidiBindingTrigger::Note; separate.targetIndex = 4;
    publish (session, { press, separate }); mute.store (false); other.store (false);
    sendCc (1, 82, 127); check (mute.load() && ! other.load(), "CC changed the same-numbered note target");
    raw (0x90, 82, 1); check (mute.load() && other.load(), "CC and Note shared their held state");
    raw (0x80, 82, 0); sendCc (1, 82, 0);

    press.dataNumber = 83; auto fanout = press; fanout.targetIndex = 4;
    publish (session, { press, fanout }); mute.store (false); other.store (false);
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        sendCc (1, 83, values[i]);
        check (mute.load() == risingStates[i] && other.load() == risingStates[i], "fanout consumed the edge before every binding saw it");
    }
    press.dataNumber = 84; publish (session, { press }); mute.store (false);
    sendCc (1, 84, 127); check (mute.load(), "empty-bindings setup press failed");
    publish (session, {}); sendCc (1, 84, 0); check (mute.load(), "unbound release changed a control");
    publish (session, { press }); sendCc (1, 84, 127);
    check (! mute.load(), "a release with no bindings was lost");
    for (int status : { 0x80, 0x90 })
    {
        auto key = press; key.trigger = MidiBindingTrigger::Note;
        key.dataNumber = status == 0x80 ? 63 : 64;
        publish (session, { key }); mute.store (false);
        raw (0x90, key.dataNumber, 1); check (mute.load(), "unbound-note release setup failed");
        publish (session, {}); raw (status, key.dataNumber, status == 0x80 ? 127 : 0);
        check (mute.load(), "unbound note release changed a control");
        publish (session, { key }); raw (0x90, key.dataNumber, 1);
        check (! mute.load(), "an unbound note release was lost");
    }
    press.dataNumber = 85; publish (session, { press }); mute.store (false);
    sendCc (1, 85, 127); check (mute.load(), "snapshot setup press failed");
    publish (session, { press }); sendCc (1, 85, 127);
    check (mute.load(), "publishing bindings rearmed a source still held high");

    press.dataNumber = 86; fanout = press; fanout.targetIndex = 4;
    std::vector<MidiBinding> many (512, fanout); many.push_back (press);
    publish (session, std::move (many)); mute.store (false); other.store (false);
    sendCc (1, 86, 127);
    check (mute.load() && ! other.load(), "the binding beyond index 511 was lost or a shared edge was consumed");
    sendCc (1, 86, 127);
    check (mute.load() && ! other.load(), "repeated high fired among 513 bindings");
    sendCc (1, 86, 0); sendCc (1, 86, 127);
    check (! mute.load() && ! other.load(), "513 bindings did not rearm together");

    press.dataNumber = 87; publish (session, { press }); mute.store (false);
    sendCc (1, 87, 127); check (mute.load(), "prepare-reset setup press failed");
    engine.prepareForSelfTest (ScenarioContext::kSampleRate, ScenarioContext::kBlockSize);
    mute.store (false); sendCc (1, 87, 127);
    check (mute.load(), "re-prepare retained a held source from the old device lifecycle");
    press.dataNumber = 88; publish (session, { press }); mute.store (false);
    sendCc (1, 88, 127); check (mute.load(), "MIDI bank rebuild setup press failed");
    engine.refreshMidiInputs();
    mute.store (false); sendCc (1, 88, 127);
    check (mute.load(), "MIDI bank rebuild retained a held source from the old input bank");
    std::printf ("MIDI button modes: %d messages, %d checks\n", messages, checks);
    return ctx.verdict();
}

ScenarioResult runBankRelative (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    constexpr int kPosition = 1;

    ctx.keep (session.activeBank);
    for (int t = 0; t < Session::kNumTracks; ++t)
        ctx.keep (session.track (t).strip.faderDb);
    restoreBindings (ctx);

    publish (session, { ccBinding (30, MidiBindingTarget::TrackFaderBank, kPosition) });
    ctx.pump (1);

    const auto faderOf = [&session] (int track)
    {
        return session.track (track).strip.faderDb.load (std::memory_order_relaxed);
    };

    for (int bank = 0; bank < 3; ++bank)
    {
        const int track = bank * Session::kBankSize + kPosition;
        for (int t = 0; t < Session::kNumTracks; ++t)
            session.track (t).strip.faderDb.store (0.0f, std::memory_order_relaxed);

        session.activeBank.store (bank, std::memory_order_relaxed);
        ctx.pumpWithMidi (kInput, cc (1, 30, 127));

        ctx.note ("bank " + std::to_string (bank) + " sent position "
                  + std::to_string (kPosition) + " to track " + std::to_string (track + 1)
                  + ": " + std::to_string (faderOf (track)) + " dB");
        ctx.expect (std::abs (faderOf (track) - 12.0f) < 0.5f,
                    "the banked binding did not reach track " + std::to_string (track + 1));
        for (int t = 0; t < Session::kNumTracks; ++t)
            if (t != track)
                ctx.expect (std::abs (faderOf (t)) < 1.0e-6f,
                            "the banked binding also moved track " + std::to_string (t + 1));
    }

    return ctx.verdict();
}

ScenarioResult runLearn (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    const bool hardwareInputs = engine.setHardwareMidiInputsEnabledForScenario (false);
    ctx.cleanup ([&engine, hardwareInputs] { engine.setHardwareMidiInputsEnabledForScenario (hardwareInputs); });
    constexpr int kTrack = 2;

    ctx.keep (session.midiLearnPending);
    ctx.keep (session.midiLearnCapture);
    restoreBindings (ctx);

    // Learn has to work with no bindings at all, or the first one could never
    // be made.
    publish (session, {});
    session.midiLearnCapture.store (0, std::memory_order_relaxed);
    session.midiLearnPending.store (
        packLearnTarget (MidiBindingTarget::TrackFader, kTrack), std::memory_order_relaxed);
    ctx.pump (1);

    ctx.pumpWithMidi (kInput, cc (4, 71, 90));
    const auto capture = session.midiLearnCapture.load (std::memory_order_relaxed);
    if (! ctx.expect (learnCaptureIsValid (capture), "the armed learn captured nothing"))
        return ctx.verdict();

    ctx.expect (unpackLearnCaptureTrigger (capture) == MidiBindingTrigger::CC,
                "the capture did not record a CC");
    ctx.expect (unpackLearnCaptureChannel (capture) == 4,
                "the capture recorded the wrong channel");
    ctx.expect (unpackLearnCaptureDataNumber (capture) == 71,
                "the capture recorded the wrong CC number");

    // The next control moved is the one that binds: a later message must not
    // steal a capture the panel has not read yet.
    ctx.pumpWithMidi (kInput, cc (1, 12, 20));
    const auto second = session.midiLearnCapture.load (std::memory_order_relaxed);
    ctx.expect (unpackLearnCaptureDataNumber (second) == 71,
                "a second control overwrote the captured one");

    // Nothing is captured while learn is not armed.
    session.midiLearnPending.store (-1, std::memory_order_relaxed);
    session.midiLearnCapture.store (0, std::memory_order_relaxed);
    ctx.pumpWithMidi (kInput, cc (1, 12, 20));
    ctx.expect (! learnCaptureIsValid (session.midiLearnCapture.load (std::memory_order_relaxed)),
                "a control was captured with no learn armed");

    // A note and a pitch bend learn as themselves, not as a CC.
    session.midiLearnPending.store (
        packLearnTarget (MidiBindingTarget::TrackMute, kTrack), std::memory_order_relaxed);
    ctx.pumpWithMidi (kInput, noteOn (2, 48, 100));
    const auto note = session.midiLearnCapture.load (std::memory_order_relaxed);
    ctx.expect (learnCaptureIsValid (note)
                    && unpackLearnCaptureTrigger (note) == MidiBindingTrigger::Note
                    && unpackLearnCaptureDataNumber (note) == 48,
                "a note did not learn as a note");

    return ctx.verdict();
}

// A session saved while Sunset was the knob unit binds MIDI to the knob unit's
// parameter indices. Restored into the plug-in, each binding drives the
// parameter it was learned on, not whatever the plug-in keeps at that index.
ScenarioResult runKnobSynthBindings (ScenarioContext& ctx)
{
    static constexpr const char* kSynth = "dusk.builtin.synth";
    if (builtin::findUnit (kSynth) == nullptr)
        return ScenarioResult::skip ("this build has no Sunset");

    auto& engine = ctx.engine();
    auto& session = ctx.session();
    static constexpr int kTrack = 9;
    static constexpr int kCutoff = 15, kUnison = 5;   // the knob unit's indices
    auto& track = session.track (kTrack);
    auto& strip = engine.getChannelStrip (kTrack);

    restoreBindings (ctx);
    ctx.keep (strip.insertMode);
    engine.publishPluginStateForSave (true);
    ctx.cleanup ([&strip, &track, unitWas = track.builtinUnitId,
                  stateWas = track.builtinStateBase64]
    {
        strip.unloadBuiltin();
        track.builtinUnitId = unitWas;
        track.builtinStateBase64 = stateWas;
    });

    const std::string blob = R"({"id":"dusk.builtin.synth","version":1,"params":{"cutoff":1500}})";
    track.builtinUnitId = kSynth;
    track.builtinStateBase64 = dusk::base64::encode ((const std::uint8_t*) blob.data(), blob.size());
    auto cutoff = ccBinding (74, MidiBindingTarget::TrackPluginParam, kTrack);
    cutoff.paramIndex = kCutoff;
    auto unison = ccBinding (75, MidiBindingTarget::TrackPluginParam, kTrack);
    unison.paramIndex = kUnison;
    publish (session, { cutoff, unison });

    engine.consumePluginStateAfterLoad();
    if (! ctx.expect (strip.isBuiltinLoaded(), "Sunset did not restore from the knob unit's state"))
        return ctx.verdict();

    const auto& slot = strip.getBuiltinSlot();
    const auto boundTo = [&session, &slot] (int number) -> std::string
    {
        for (const auto& b : session.midiBindings.current())
            if (b.dataNumber == number)
            {
                const auto* info = slot.paramInfo (b.paramIndex);
                return info != nullptr && info->id != nullptr ? info->id : "(none)";
            }
        return "(unbound)";
    };
    ctx.note ("CC 74 drives " + boundTo (74) + ", CC 75 drives " + boundTo (75));
    ctx.expect (boundTo (74) == "filterCutoff", "the knob unit's Cutoff binding moved off the cutoff");
    ctx.expect (boundTo (75) == "unisonVoices", "the knob unit's Unison binding moved off the unison count");
    return ctx.verdict();
}

const ScenarioRegistrar knobSynthBindings { Scenario {
    "midi.knob_synth_bindings_follow_sunset",
    { "midi", "bindings", "builtin" },
    Needs::Engine,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runKnobSynthBindings (ctx); }
} };

const ScenarioRegistrar targets { Scenario {
    "midi.bindings_reach_their_targets",
    { "midi", "bindings" },
    Needs::Engine,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runTargets (ctx); }
} };

const ScenarioRegistrar eqFreqDropsDial { Scenario {
    "midi.eq_frequency_drops_a_converted_dial",
    { "midi", "bindings", "eq" },
    Needs::Engine,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runEqFreqDropsDial (ctx); }
} };

const ScenarioRegistrar movesEngage { Scenario {
    "midi.eq_and_comp_moves_engage_their_section",
    { "midi", "bindings", "eq" },
    Needs::Engine,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runMovesEngage (ctx); }
} };

const ScenarioRegistrar buttons { Scenario {
    "midi.button_modes",
    { "midi", "bindings" },
    Needs::Engine,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runButtonModes (ctx); }
} };

const ScenarioRegistrar banked { Scenario {
    "midi.banked_binding_follows_the_surface_bank",
    { "midi", "bindings" },
    Needs::Engine,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runBankRelative (ctx); }
} };
#if DUSKSTUDIO_HAS_ALSA
const ScenarioRegistrar hotplug { Scenario {
    "midi.hotplug_waits_for_stop", { "midi", "hardware" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return hotplugWaitsForStop (ctx); }, 20000
} };
#endif
const ScenarioRegistrar learn { Scenario {
    "midi.learn_captures_the_next_control",
    { "midi", "bindings" },
    Needs::Engine,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runLearn (ctx); }
} };
} // namespace
} // namespace duskstudio::scenario
