#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../../session/MidiBindings.h"
#include "../../../session/Session.h"

#if DUSKSTUDIO_HAS_ALSA
#include <alsa/asoundlib.h>
#endif

#include <algorithm>
#include <cmath>
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
        engine.stop();
        session.setTrackArmed (0, false);
    });
    checkHotplugWhileRolling (ctx, portClient, false, [&ctx, portClient]
    {
        checkHotplugWhileRolling (ctx, portClient, true, [&ctx] { ctx.complete (ctx.verdict()); });
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

ScenarioResult runButtonModes (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    constexpr int kTrack = 3;
    auto& mute = session.track (kTrack).strip.mute;

    ctx.keep (mute);
    restoreBindings (ctx);

    MidiBinding press;
    press.trigger = MidiBindingTrigger::CC;
    press.dataNumber = 40;
    press.target = MidiBindingTarget::TrackMute;
    press.targetIndex = kTrack;
    press.buttonMode = MidiButtonMode::Press;

    // A momentary button sends 127 then 0 for one press: only the rising edge
    // is the press.
    publish (session, { press });
    mute.store (false, std::memory_order_relaxed);
    ctx.pump (1);

    ctx.pumpWithMidi (kInput, cc (1, 40, 127));
    ctx.expect (mute.load (std::memory_order_relaxed),
                "a momentary press did not reach mute");
    ctx.pumpWithMidi (kInput, cc (1, 40, 0));
    ctx.expect (mute.load (std::memory_order_relaxed),
                "the release of a momentary press flipped mute a second time");

    // A latching button alternates 127 and 0 across presses, so every message
    // has to count or the user gets one flip per two clicks.
    auto toggle = press;
    toggle.buttonMode = MidiButtonMode::Toggle;
    publish (session, { toggle });
    mute.store (false, std::memory_order_relaxed);
    ctx.pump (1);

    ctx.pumpWithMidi (kInput, cc (1, 40, 127));
    ctx.expect (mute.load (std::memory_order_relaxed),
                "the first click of a latching button did not flip mute");
    ctx.pumpWithMidi (kInput, cc (1, 40, 0));
    ctx.expect (! mute.load (std::memory_order_relaxed),
                "the second click of a latching button did not flip mute back");

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

const ScenarioRegistrar targets { Scenario {
    "midi.bindings_reach_their_targets",
    { "midi", "bindings" },
    Needs::Engine,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runTargets (ctx); }
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
    [] (ScenarioContext& ctx) { return hotplugWaitsForStop (ctx); }, 15000
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
