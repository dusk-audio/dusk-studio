#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/McuProtocol.h"
#include "engine/McuReceiver.h"
#include "session/Session.h"
#include "session/SessionSerializer.h"
#include "support/DuskMidiTestBridge.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <iterator>

using Catch::Matchers::WithinAbs;
using namespace duskstudio;
using duskstudio::test::toDusk;

namespace
{
// Synthesize a single-event MidiBuffer matching the MCU wire format
// we expect McuReceiver to decode, bridged into the dusk::MidiBuffer the
// receiver takes. Keeps the byte-stuffing in one place.

dusk::MidiBuffer makePitchBend (int channel, int value14)
{
    juce::MidiBuffer mb;
    mb.addEvent (juce::MidiMessage::pitchWheel (channel + 1, value14), 0);
    return toDusk (mb);
}

dusk::MidiBuffer makeNoteOn (int note, int velocity)
{
    juce::MidiBuffer mb;
    mb.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) velocity), 0);
    return toDusk (mb);
}

dusk::MidiBuffer makeCc (int controller, int value)
{
    juce::MidiBuffer mb;
    mb.addEvent (juce::MidiMessage::controllerEvent (1, controller, value), 0);
    return toDusk (mb);
}
} // namespace

TEST_CASE ("McuReceiver: fader pitch-bend writes faderDb on banked strip",
           "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);

    // Bank 0 -> channels 0..7 = tracks 0..7. Pitch-bend on channel 3
    // (zero-indexed = MCU strip 4) at full scale = top of throw = +12 dB.
    r.process (makePitchBend (3, mcu::kPitchBendMaxValue), 0);
    REQUIRE_THAT (s.track (3).strip.faderDb.load (std::memory_order_relaxed),
                  WithinAbs (12.0f, 0.1f));

    // Bottom of fader: 0 -> -100 dB (-inf floor).
    r.process (makePitchBend (3, 0), 0);
    REQUIRE_THAT (s.track (3).strip.faderDb.load (std::memory_order_relaxed),
                  WithinAbs (-100.0f, 0.1f));

    // Bank 1 -> channels 0..7 = tracks 8..15. Strip 0 moves track 8.
    // Half the 14-bit range (8191) sits on the Mackie taper between the
    // -12 dB (7657) and -9 dB (8735) breakpoints -> ~-10.5 dB.
    s.mcu.bank.store (1, std::memory_order_relaxed);
    r.process (makePitchBend (0, mcu::kPitchBendMaxValue / 2), 0);
    REQUIRE_THAT (s.track (8).strip.faderDb.load (std::memory_order_relaxed),
                  WithinAbs (-10.5f, 0.3f));
}

TEST_CASE ("McuReceiver: master fader pitch-bend (channel 8) targets MasterBusParams",
           "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);

    r.process (makePitchBend (mcu::kMasterFaderIndex, mcu::kPitchBendMaxValue), 0);
    REQUIRE_THAT (s.master().faderDb.load (std::memory_order_relaxed),
                  WithinAbs (12.0f, 0.1f));
}

TEST_CASE ("McuReceiver: mute / solo / arm button toggles", "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);

    // Press = velocity 0x7F. Release (vel 0) should be ignored to
    // avoid double-toggle on key-up.
    r.process (makeNoteOn (mcu::btn::MuteBase + 2, 0x7F), 0);
    REQUIRE (s.track (2).strip.mute.load (std::memory_order_relaxed));
    r.process (makeNoteOn (mcu::btn::MuteBase + 2, 0), 0);   // release -> no-op
    REQUIRE (s.track (2).strip.mute.load (std::memory_order_relaxed));
    r.process (makeNoteOn (mcu::btn::MuteBase + 2, 0x7F), 0);
    REQUIRE_FALSE (s.track (2).strip.mute.load (std::memory_order_relaxed));

    r.process (makeNoteOn (mcu::btn::SoloBase + 5, 0x7F), 0);
    REQUIRE (s.track (5).strip.solo.load (std::memory_order_relaxed));

    r.process (makeNoteOn (mcu::btn::RecArmBase + 1, 0x7F), 0);
    REQUIRE (s.track (1).recordArmed.load (std::memory_order_relaxed));
    // anyTrackArmed() reads the counter-backed atom that setTrackArmed
    // bumps, so it's safe to check without a running audio thread.
    // anyTrackSoloed() reads liveSolo (audio-thread mirror of solo),
    // which only updates when AudioEngine's per-block routing runs;
    // a unit test without an engine sees only the raw atom we just
    // wrote, hence the direct-atom assertion above.
    REQUIRE (s.anyTrackArmed());
}

TEST_CASE ("McuReceiver: bank LEFT / RIGHT step session.mcu.bank", "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);

    const int lastBank = Session::kNumBanks - 1;
    REQUIRE (s.mcu.bank.load (std::memory_order_relaxed) == 0);
    r.process (makeNoteOn (mcu::btn::BankRight, 0x7F), 0);
    REQUIRE (s.mcu.bank.load (std::memory_order_relaxed) == 1);
    // Walk RIGHT past the last bank — value clamps to kNumBanks - 1.
    for (int i = 0; i < Session::kNumBanks + 2; ++i)
        r.process (makeNoteOn (mcu::btn::BankRight, 0x7F), 0);
    REQUIRE (s.mcu.bank.load (std::memory_order_relaxed) == lastBank);
    r.process (makeNoteOn (mcu::btn::BankLeft, 0x7F), 0);
    REQUIRE (s.mcu.bank.load (std::memory_order_relaxed) == lastBank - 1);
    // Walk LEFT past 0 -> stays at 0.
    for (int i = 0; i < Session::kNumBanks + 2; ++i)
        r.process (makeNoteOn (mcu::btn::BankLeft, 0x7F), 0);
    REQUIRE (s.mcu.bank.load (std::memory_order_relaxed) == 0);
}

TEST_CASE ("McuReceiver: SELECT button drives selectedChannel + banking", "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);

    r.process (makeNoteOn (mcu::btn::SelectBase + 4, 0x7F), 0);
    REQUIRE (s.mcu.selectedChannel.load (std::memory_order_relaxed) == 4);

    // Bank 1, strip 2 -> track 10.
    s.mcu.bank.store (1, std::memory_order_relaxed);
    r.process (makeNoteOn (mcu::btn::SelectBase + 2, 0x7F), 0);
    REQUIRE (s.mcu.selectedChannel.load (std::memory_order_relaxed) == 10);
}

TEST_CASE ("McuReceiver: assign-mode buttons set mcu.assignMode", "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);
    REQUIRE (s.mcu.assignMode.load (std::memory_order_relaxed) == 0);  // PAN default

    r.process (makeNoteOn (mcu::btn::AssignEq, 0x7F), 0);
    REQUIRE (s.mcu.assignMode.load (std::memory_order_relaxed) == 5);   // EQ
    r.process (makeNoteOn (mcu::btn::AssignTrack, 0x7F), 0);
    REQUIRE (s.mcu.assignMode.load (std::memory_order_relaxed) == 6);   // COMP
    r.process (makeNoteOn (mcu::btn::AssignPan, 0x7F), 0);
    REQUIRE (s.mcu.assignMode.load (std::memory_order_relaxed) == 0);   // PAN
    // SEND cycles 1 -> 2 -> 3 -> 4 -> 1.
    r.process (makeNoteOn (mcu::btn::AssignSend, 0x7F), 0);
    REQUIRE (s.mcu.assignMode.load (std::memory_order_relaxed) == 1);
    r.process (makeNoteOn (mcu::btn::AssignSend, 0x7F), 0);
    REQUIRE (s.mcu.assignMode.load (std::memory_order_relaxed) == 2);
    r.process (makeNoteOn (mcu::btn::AssignSend, 0x7F), 0);
    r.process (makeNoteOn (mcu::btn::AssignSend, 0x7F), 0);
    REQUIRE (s.mcu.assignMode.load (std::memory_order_relaxed) == 4);
    r.process (makeNoteOn (mcu::btn::AssignSend, 0x7F), 0);
    REQUIRE (s.mcu.assignMode.load (std::memory_order_relaxed) == 1);  // wrap
}

TEST_CASE ("McuReceiver: V-pot rotate (PAN mode) nudges pan", "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);
    REQUIRE_THAT (s.track (0).strip.pan.load (std::memory_order_relaxed),
                  WithinAbs (0.0f, 1e-4f));

    // Right turn: 5 ticks * 0.02 = 0.10.
    r.process (makeCc (mcu::cc::VPotRotateBase + 0, 0x05), 0);
    REQUIRE_THAT (s.track (0).strip.pan.load (std::memory_order_relaxed),
                  WithinAbs (0.10f, 1e-4f));

    // Left turn: bit 6 set = sign negative. 3 ticks -> -0.06 -> pan 0.04.
    r.process (makeCc (mcu::cc::VPotRotateBase + 0, 0x40 | 0x03), 0);
    REQUIRE_THAT (s.track (0).strip.pan.load (std::memory_order_relaxed),
                  WithinAbs (0.04f, 1e-4f));
}

TEST_CASE ("McuReceiver: V-pot push (PAN mode) resets pan to 0", "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);
    s.track (3).strip.pan.store (0.42f, std::memory_order_relaxed);
    r.process (makeNoteOn (mcu::btn::VPotPushBase + 3, 0x7F), 0);
    REQUIRE_THAT (s.track (3).strip.pan.load (std::memory_order_relaxed),
                  WithinAbs (0.0f, 1e-4f));
}

// A converted older session can hold a band past its knob's range. Turned
// further toward the end it is already past, the encoder leaves it where it is;
// turned back, it steps in from the end stop, as the knob on screen does when
// dragged. A turn or push that moves a band drops the format-7 dial it played;
// one that leaves it keeps it.
TEST_CASE ("McuReceiver: an EQ encoder leaves a band held past its range until turned back into it",
           "[mcu][receiver]")
{
    using EqFreq = ChannelStripParams::EqFreq;
    Session s;
    McuReceiver r (s);
    s.mcu.assignMode.store (5, std::memory_order_relaxed);       // EQ
    s.mcu.selectedChannel.store (1, std::memory_order_relaxed);
    auto& strip = s.track (1).strip;
    strip.eqBlackMode.store (true, std::memory_order_relaxed);
    const auto hold = [&strip] (EqFreq f, float hz, float dial)
    {
        strip.eqFreq (f).store (hz, std::memory_order_relaxed);
        strip.legacyDial (f).set (dial, hz, true);
    };
    const auto dialOf = [&strip] (EqFreq f)
    {
        float hz = 0.0f;
        return strip.legacyDial (f).dialFor (strip.eqFreq (f), true, hz);
    };
    hold (EqFreq::Hpf, 316.0f, 300.0f);
    hold (EqFreq::Lf, 660.0f, 400.0f);
    hold (EqFreq::Lm, 139.0f, 100.0f);
    hold (EqFreq::Hf, 637.0f, 1000.0f);

    r.process (makeCc (mcu::cc::VPotRotateBase + 1, 0x01), 0);   // LF gain, not frequency
    REQUIRE_THAT (strip.lfFreq.load (std::memory_order_relaxed), WithinAbs (660.0f, 1e-4f));

    r.process (makeCc (mcu::cc::VPotRotateBase + 2, 0x01), 0);   // LF up, past the top already
    REQUIRE_THAT (strip.lfFreq.load (std::memory_order_relaxed), WithinAbs (660.0f, 1e-4f));
    REQUIRE_THAT (dialOf (EqFreq::Lf), WithinAbs (400.0f, 0.0f));
    r.process (makeCc (mcu::cc::VPotRotateBase + 2, 0x40 | 0x01), 0); // LF down: in from the top
    REQUIRE_THAT (strip.lfFreq.load (std::memory_order_relaxed),
                  WithinAbs (ChannelStripParams::kLfFreqMax - 5.0f, 1e-4f));
    REQUIRE (strip.legacyDial (EqFreq::Lf).raw() == 0);

    r.process (makeCc (mcu::cc::VPotRotateBase + 7, 0x40 | 0x01), 0); // HF down, below the bottom already
    REQUIRE_THAT (strip.hfFreq.load (std::memory_order_relaxed), WithinAbs (637.0f, 1e-4f));
    REQUIRE_THAT (dialOf (EqFreq::Hf), WithinAbs (1000.0f, 0.0f));
    r.process (makeCc (mcu::cc::VPotRotateBase + 7, 0x01), 0);   // HF up: in from the bottom
    REQUIRE_THAT (strip.hfFreq.load (std::memory_order_relaxed),
                  WithinAbs (ChannelStripParams::kHfFreqMin + 100.0f, 1e-4f));
    REQUIRE (strip.legacyDial (EqFreq::Hf).raw() == 0);
    r.process (makeCc (mcu::cc::VPotRotateBase + 7, 0x02), 0);   // then moves from there
    REQUIRE_THAT (strip.hfFreq.load (std::memory_order_relaxed),
                  WithinAbs (ChannelStripParams::kHfFreqMin + 300.0f, 1e-4f));

    r.process (makeCc (mcu::cc::VPotRotateBase + 0, 0x01), 0);   // HPF up, past the top already
    REQUIRE_THAT (strip.hpfFreq.load (std::memory_order_relaxed), WithinAbs (316.0f, 1e-4f));
    REQUIRE_THAT (dialOf (EqFreq::Hpf), WithinAbs (300.0f, 0.0f));
    r.process (makeCc (mcu::cc::VPotRotateBase + 0, 0x40 | 0x01), 0);
    REQUIRE_THAT (strip.hpfFreq.load (std::memory_order_relaxed),
                  WithinAbs (ChannelStripParams::kHpfMaxHz - 4.0f, 1e-4f));
    REQUIRE (strip.legacyDial (EqFreq::Hpf).raw() == 0);

    // Pushing an encoder resets its band, and the reset is a move too.
    REQUIRE_THAT (dialOf (EqFreq::Lm), WithinAbs (100.0f, 0.0f));
    r.process (makeNoteOn (mcu::btn::VPotPushBase + 4, 0x7F), 0);
    REQUIRE_THAT (strip.lmFreq.load (std::memory_order_relaxed), WithinAbs (600.0f, 1e-4f));
    REQUIRE (strip.legacyDial (EqFreq::Lm).raw() == 0);
}

// EQ mode's encoders, on the selected channel: 1 HPF, 2 LF gain, 3 LF
// frequency, 4 LM gain, 5 LM frequency, 6 HM gain, 7 HF gain, 8 HF frequency.
// Each detent moves the one parameter its encoder names and nothing else.
TEST_CASE ("McuReceiver: V-pot rotate (EQ mode) turns the parameter each encoder names",
           "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);
    s.mcu.assignMode.store (5, std::memory_order_relaxed);       // EQ
    s.mcu.selectedChannel.store (6, std::memory_order_relaxed);
    auto& strip = s.track (6).strip;
    const std::atomic<float>* params[] {
        &strip.hpfFreq, &strip.lfGainDb, &strip.lfFreq, &strip.lmGainDb,
        &strip.lmFreq, &strip.hmGainDb, &strip.hfGainDb, &strip.hfFreq,
        &strip.hmFreq, &strip.lmQ, &strip.hmQ, &strip.lpfFreq,
    };
    constexpr float kSteps[] { 4.0f, 0.3f, 5.0f, 0.3f, 20.0f, 0.3f, 0.3f, 100.0f };
    for (int encoder = 0; encoder < 8; ++encoder)
    {
        CAPTURE (encoder);
        float before[std::size (params)];
        for (size_t i = 0; i < std::size (params); ++i) before[i] = params[i]->load();
        r.process (makeCc (mcu::cc::VPotRotateBase + encoder, 0x01), 0);
        for (size_t i = 0; i < std::size (params); ++i)
        {
            CAPTURE (i);
            const float moved = (int) i == encoder ? kSteps[encoder] : 0.0f;
            CHECK_THAT (params[i]->load(), WithinAbs (before[i] + moved, 1e-4f));
        }
    }
}

// The HPF encoder switches the filter as its knob and a MIDI binding do: on
// above OFF, off at it. A turn that stores nothing leaves the switch alone.
TEST_CASE ("McuReceiver: the HPF encoder switches the HPF on above OFF and off at it",
           "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);
    s.mcu.assignMode.store (5, std::memory_order_relaxed);       // EQ
    s.mcu.selectedChannel.store (2, std::memory_order_relaxed);
    auto& strip = s.track (2).strip;
    REQUIRE_FALSE (strip.hpfEnabled.load());
    REQUIRE_THAT (strip.hpfFreq.load(), WithinAbs (ChannelStripParams::kHpfOffHz, 0.0f));

    r.process (makeCc (mcu::cc::VPotRotateBase + 0, 0x01), 0);          // up from OFF
    CHECK_THAT (strip.hpfFreq.load(), WithinAbs (ChannelStripParams::kHpfOffHz + 4.0f, 1e-4f));
    CHECK (strip.hpfEnabled.load());

    r.process (makeCc (mcu::cc::VPotRotateBase + 0, 0x05), 0);          // within the range
    CHECK_THAT (strip.hpfFreq.load(), WithinAbs (ChannelStripParams::kHpfOffHz + 24.0f, 1e-4f));
    CHECK (strip.hpfEnabled.load());
    r.process (makeCc (mcu::cc::VPotRotateBase + 0, 0x40 | 0x02), 0);
    CHECK_THAT (strip.hpfFreq.load(), WithinAbs (ChannelStripParams::kHpfOffHz + 16.0f, 1e-4f));
    CHECK (strip.hpfEnabled.load());

    r.process (makeCc (mcu::cc::VPotRotateBase + 0, 0x40 | 0x06), 0);   // down onto OFF
    CHECK_THAT (strip.hpfFreq.load(), WithinAbs (ChannelStripParams::kHpfOffHz, 1e-4f));
    CHECK_FALSE (strip.hpfEnabled.load());

    r.process (makeCc (mcu::cc::VPotRotateBase + 0, 0x0A), 0);
    REQUIRE (strip.hpfEnabled.load());
    r.process (makeNoteOn (mcu::btn::VPotPushBase + 0, 0x7F), 0);       // push: OFF
    CHECK_THAT (strip.hpfFreq.load(), WithinAbs (ChannelStripParams::kHpfOffHz, 0.0f));
    CHECK_FALSE (strip.hpfEnabled.load());

    // Switched on from its label at OFF, turned further down: nothing moves.
    strip.hpfEnabled.store (true);
    r.process (makeCc (mcu::cc::VPotRotateBase + 0, 0x40 | 0x01), 0);
    CHECK (strip.hpfEnabled.load());
}

// A push puts each EQ encoder's parameter where a fresh strip has it, which is
// where the strip's knob returns on a double-click: HPF OFF, 0 dB, LF 100 Hz,
// LM 600 Hz, HF 8 kHz.
TEST_CASE ("McuReceiver: V-pot push (EQ mode) resets each encoder to the strip's default",
           "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);
    s.mcu.assignMode.store (5, std::memory_order_relaxed);       // EQ
    s.mcu.selectedChannel.store (4, std::memory_order_relaxed);
    auto& strip = s.track (4).strip;
    strip.setEqFreq (ChannelStripParams::EqFreq::Hpf, 120.0f);
    strip.hpfEnabled.store (true);
    strip.lfGainDb.store (6.0f);
    strip.setEqFreq (ChannelStripParams::EqFreq::Lf, 250.0f);
    strip.lmGainDb.store (-3.0f);
    strip.setEqFreq (ChannelStripParams::EqFreq::Lm, 1500.0f);
    strip.hmGainDb.store (4.0f);
    strip.hfGainDb.store (-5.0f);
    strip.setEqFreq (ChannelStripParams::EqFreq::Hf, 12000.0f);

    for (int encoder = 0; encoder < 8; ++encoder)
        r.process (makeNoteOn (mcu::btn::VPotPushBase + encoder, 0x7F), 0);

    const ChannelStripParams fresh;
    CHECK_THAT (strip.hpfFreq.load(),  WithinAbs (fresh.hpfFreq.load(),  0.0f));
    CHECK (strip.hpfEnabled.load() == fresh.hpfEnabled.load());
    CHECK_THAT (strip.lfGainDb.load(), WithinAbs (fresh.lfGainDb.load(), 0.0f));
    CHECK_THAT (strip.lfFreq.load(),   WithinAbs (fresh.lfFreq.load(),   0.0f));
    CHECK_THAT (strip.lmGainDb.load(), WithinAbs (fresh.lmGainDb.load(), 0.0f));
    CHECK_THAT (strip.lmFreq.load(),   WithinAbs (fresh.lmFreq.load(),   0.0f));
    CHECK_THAT (strip.hmGainDb.load(), WithinAbs (fresh.hmGainDb.load(), 0.0f));
    CHECK_THAT (strip.hfGainDb.load(), WithinAbs (fresh.hfGainDb.load(), 0.0f));
    CHECK_THAT (strip.hfFreq.load(),   WithinAbs (fresh.hfFreq.load(),   0.0f));
    CHECK_THAT (fresh.hfFreq.load(),   WithinAbs (8000.0f, 0.0f));
}

// A turn is a move of the control its encoder names, as the knob on screen is,
// so it engages a bypassed EQ. The HPF engages it only once it leaves OFF, and
// turning it back to OFF leaves the EQ as it is. A push resets without
// engaging, as the strip's Reset EQ does.
TEST_CASE ("McuReceiver: an EQ encoder turn engages the EQ and a push does not",
           "[mcu][receiver]")
{
    for (int encoder = 1; encoder < 8; ++encoder)
    {
        CAPTURE (encoder);
        Session s;
        McuReceiver r (s);
        s.mcu.assignMode.store (5, std::memory_order_relaxed);   // EQ
        s.mcu.selectedChannel.store (3, std::memory_order_relaxed);
        auto& strip = s.track (3).strip;
        REQUIRE_FALSE (strip.eqEnabled.load());
        r.process (makeCc (mcu::cc::VPotRotateBase + encoder, 0x01), 0);
        CHECK (strip.eqEnabled.load());
    }

    Session s;
    McuReceiver r (s);
    s.mcu.assignMode.store (5, std::memory_order_relaxed);
    s.mcu.selectedChannel.store (3, std::memory_order_relaxed);
    auto& strip = s.track (3).strip;

    r.process (makeCc (mcu::cc::VPotRotateBase + 0, 0x40 | 0x01), 0);   // HPF down at OFF
    CHECK_FALSE (strip.eqEnabled.load());
    r.process (makeCc (mcu::cc::VPotRotateBase + 0, 0x05), 0);          // up off OFF
    CHECK (strip.hpfEnabled.load());
    CHECK (strip.eqEnabled.load());
    r.process (makeCc (mcu::cc::VPotRotateBase + 0, 0x40 | 0x05), 0);   // back to OFF
    CHECK_FALSE (strip.hpfEnabled.load());
    CHECK (strip.eqEnabled.load());

    strip.eqEnabled.store (false);
    strip.setEqFreq (ChannelStripParams::EqFreq::Hpf, 120.0f);
    strip.hpfEnabled.store (true);
    strip.lfGainDb.store (6.0f);
    strip.setEqFreq (ChannelStripParams::EqFreq::Lm, 1500.0f);
    strip.hfGainDb.store (-5.0f);
    for (int encoder = 0; encoder < 8; ++encoder)
        r.process (makeNoteOn (mcu::btn::VPotPushBase + encoder, 0x7F), 0);
    CHECK_THAT (strip.lfGainDb.load(), WithinAbs (0.0f, 0.0f));
    CHECK_FALSE (strip.hpfEnabled.load());
    CHECK_FALSE (strip.eqEnabled.load());
}

// The compressor follows the strip's comp knobs and threshold handle: a turn
// engages it, a push resets without engaging, as a double-click on the handle
// does. Opto has no ratio, attack or release, so those encoders move nothing
// there and engage nothing.
TEST_CASE ("McuReceiver: a COMP encoder turn engages the compressor and a push does not",
           "[mcu][receiver]")
{
    for (const int mode : { 0, 1, 2 })
        for (int encoder = 0; encoder < 5; ++encoder)
        {
            CAPTURE (mode, encoder);
            Session s;
            McuReceiver r (s);
            s.mcu.assignMode.store (6, std::memory_order_relaxed);   // COMP
            s.mcu.selectedChannel.store (2, std::memory_order_relaxed);
            auto& strip = s.track (2).strip;
            strip.compMode.store (mode, std::memory_order_relaxed);
            REQUIRE_FALSE (strip.compEnabled.load());

            r.process (makeNoteOn (mcu::btn::VPotPushBase + encoder, 0x7F), 0);
            CHECK_FALSE (strip.compEnabled.load());
            r.process (makeCc (mcu::cc::VPotRotateBase + encoder, 0x40 | 0x02), 0);
            const bool moves = mode != 0 || encoder == 0 || encoder == 4;
            CHECK (strip.compEnabled.load() == moves);
        }
}

TEST_CASE ("McuReceiver: V-pot rotate (COMP mode) makeup moves the audible param",
           "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);
    s.mcu.assignMode.store (6, std::memory_order_relaxed);       // COMP
    s.mcu.selectedChannel.store (2, std::memory_order_relaxed);
    auto& strip = s.track (2).strip;

    // Encoder 5 (index 4) is MAKEUP, 0.3 dB per tick. The value the DSP
    // reads is the active mode's output param, and a tick is worth the
    // same real dB in every mode.
    strip.compMode.store (2, std::memory_order_relaxed);         // VCA
    r.process (makeCc (mcu::cc::VPotRotateBase + 4, 0x05), 0);   // +5 ticks
    REQUIRE_THAT (strip.compVcaOutput.load (std::memory_order_relaxed),
                  WithinAbs (1.5f, 1e-4f));

    // Opto's makeup is the donor's 0..100 dial at 0.8 dB per unit, so the
    // same +1.5 dB from unity is 51.875, not a raw +1.5 on the dial.
    strip.compMode.store (0, std::memory_order_relaxed);
    r.process (makeCc (mcu::cc::VPotRotateBase + 4, 0x05), 0);
    REQUIRE_THAT (strip.compOptoGain.load (std::memory_order_relaxed),
                  WithinAbs (51.875f, 1e-3f));

    // Left turn: bit 6 set = negative. 2 ticks = -0.6 dB, so +0.9 dB total.
    r.process (makeCc (mcu::cc::VPotRotateBase + 4, 0x40 | 0x02), 0);
    REQUIRE_THAT (strip.compOptoGain.load (std::memory_order_relaxed),
                  WithinAbs (51.125f, 1e-3f));
}

// A strip parked at the bottom of its makeup range - a CC binding driven to
// zero, or a session that stored the floor - must step by one tick from
// there. Clamping the base into some other range first would fling the
// strip several dB on the very first tick.
TEST_CASE ("McuReceiver: makeup nudge steps from the mode's floor",
           "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);
    s.mcu.assignMode.store (6, std::memory_order_relaxed);
    auto& strip = s.track (0).strip;
    strip.compMode.store (1, std::memory_order_relaxed);          // FET
    strip.compFetOutput.store (-20.0f, std::memory_order_relaxed);

    r.process (makeCc (mcu::cc::VPotRotateBase + 4, 0x01), 0);
    REQUIRE_THAT (strip.compFetOutput.load (std::memory_order_relaxed),
                  WithinAbs (-19.7f, 1e-4f));

    // Down from the floor holds at the floor rather than drifting below it.
    r.process (makeCc (mcu::cc::VPotRotateBase + 4, 0x40 | 0x0A), 0);
    REQUIRE_THAT (strip.compFetOutput.load (std::memory_order_relaxed),
                  WithinAbs (-20.0f, 1e-4f));
}

TEST_CASE ("McuReceiver: V-pot push (COMP mode) returns makeup to unity",
           "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);
    s.mcu.assignMode.store (6, std::memory_order_relaxed);
    s.mcu.selectedChannel.store (1, std::memory_order_relaxed);
    auto& strip = s.track (1).strip;
    strip.compMode.store (1, std::memory_order_relaxed);         // FET
    strip.compFetOutput.store (7.5f, std::memory_order_relaxed);

    r.process (makeNoteOn (mcu::btn::VPotPushBase + 4, 0x7F), 0);
    REQUIRE_THAT (strip.compFetOutput.load (std::memory_order_relaxed),
                  WithinAbs (0.0f, 1e-4f));

    strip.compMode.store (0, std::memory_order_relaxed);         // Opto
    strip.compOptoGain.store (80.0f, std::memory_order_relaxed);
    r.process (makeNoteOn (mcu::btn::VPotPushBase + 4, 0x7F), 0);
    REQUIRE_THAT (strip.compOptoGain.load (std::memory_order_relaxed),
                  WithinAbs (50.0f, 1e-4f));  // unity, not silence
}

// The makeup encoder is relative, so the value it nudges from has to still
// be there after a reload: one tick on a freshly-loaded session continues
// from the saved makeup instead of restarting near unity.
TEST_CASE ("McuReceiver: makeup nudge base survives a session reload",
           "[mcu][receiver]")
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dusk-studio-mcu-makeup-"
                                          + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    const auto target = dir.getChildFile ("session.json");

    Session a;
    McuReceiver ra (a);
    a.mcu.assignMode.store (6, std::memory_order_relaxed);

    SECTION ("VCA")
    {
        a.track (0).strip.compMode.store (2, std::memory_order_relaxed);
        ra.process (makeCc (mcu::cc::VPotRotateBase + 4, 0x14), 0);   // +20 ticks = +6 dB
        REQUIRE_THAT (a.track (0).strip.compVcaOutput.load (std::memory_order_relaxed),
                      WithinAbs (6.0f, 1e-4f));
        REQUIRE (SessionSerializer::save (a, target));

        Session b;
        REQUIRE (SessionSerializer::load (b, target));
        REQUIRE (b.mcu.assignMode.load (std::memory_order_relaxed) == 6);
        REQUIRE_THAT (b.track (0).strip.compVcaOutput.load (std::memory_order_relaxed),
                      WithinAbs (6.0f, 1e-4f));

        McuReceiver rb (b);
        rb.process (makeCc (mcu::cc::VPotRotateBase + 4, 0x01), 0);
        REQUIRE_THAT (b.track (0).strip.compVcaOutput.load (std::memory_order_relaxed),
                      WithinAbs (6.3f, 1e-4f));
    }

    SECTION ("Opto")
    {
        // +6 dB on the dial is 57.5; one more tick is +6.3 dB = 57.875.
        a.track (0).strip.compMode.store (0, std::memory_order_relaxed);
        ra.process (makeCc (mcu::cc::VPotRotateBase + 4, 0x14), 0);
        REQUIRE_THAT (a.track (0).strip.compOptoGain.load (std::memory_order_relaxed),
                      WithinAbs (57.5f, 1e-3f));
        REQUIRE (SessionSerializer::save (a, target));

        Session b;
        REQUIRE (SessionSerializer::load (b, target));
        REQUIRE_THAT (b.track (0).strip.compOptoGain.load (std::memory_order_relaxed),
                      WithinAbs (57.5f, 1e-3f));

        McuReceiver rb (b);
        rb.process (makeCc (mcu::cc::VPotRotateBase + 4, 0x01), 0);
        REQUIRE_THAT (b.track (0).strip.compOptoGain.load (std::memory_order_relaxed),
                      WithinAbs (57.875f, 1e-3f));
    }

    dir.deleteRecursively();
}

// Encoder 1 (index 0) is THRESHOLD, 0.5 dB per tick. The audible value is
// the active mode's threshold-shaped param, and every mode has to move -
// the encoder used to nudge a unified atom the DSP never read, so the whole
// control was silent in all three.
TEST_CASE ("McuReceiver: V-pot rotate (COMP mode) threshold moves the audible param",
           "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);
    s.mcu.assignMode.store (6, std::memory_order_relaxed);       // COMP
    s.mcu.selectedChannel.store (3, std::memory_order_relaxed);
    auto& strip = s.track (3).strip;

    // VCA sits at its +12 dB no-compression ceiling, so it can only go down.
    strip.compMode.store (2, std::memory_order_relaxed);
    r.process (makeCc (mcu::cc::VPotRotateBase + 0, 0x40 | 0x08), 0);  // -8 ticks
    REQUIRE_THAT (strip.compVcaThreshDb.load (std::memory_order_relaxed),
                  WithinAbs (8.0f, 1e-4f));

    // FET's threshold is the donor's own param, not its input drive.
    strip.compMode.store (1, std::memory_order_relaxed);
    r.process (makeCc (mcu::cc::VPotRotateBase + 0, 0x40 | 0x04), 0);  // -4 ticks
    REQUIRE_THAT (strip.compFetThresholdDb.load (std::memory_order_relaxed),
                  WithinAbs (-12.0f, 1e-4f));
    REQUIRE_THAT (strip.compFetInput.load (std::memory_order_relaxed),
                  WithinAbs (0.0f, 1e-4f));

    // Opto's dial runs the other way: 6 ticks down = -3 dB = 5 % reduction.
    strip.compMode.store (0, std::memory_order_relaxed);
    r.process (makeCc (mcu::cc::VPotRotateBase + 0, 0x40 | 0x06), 0);
    REQUIRE_THAT (strip.compOptoPeakRed.load (std::memory_order_relaxed),
                  WithinAbs (5.0f, 1e-3f));
}

// Encoders 2 / 3 / 4 wrote the VCA ratio / attack / release atoms whatever
// the mode, so all three were silent in Opto and FET. Opto has no parameter
// behind any of them and must stay untouched.
TEST_CASE ("McuReceiver: V-pot rotate (COMP mode) ratio / attack / release follow the mode",
           "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);
    s.mcu.assignMode.store (6, std::memory_order_relaxed);
    auto& strip = s.track (0).strip;

    SECTION ("FET drives the FET params")
    {
        strip.compMode.store (1, std::memory_order_relaxed);
        r.process (makeCc (mcu::cc::VPotRotateBase + 1, 0x02), 0);   // ratio: one rung
        REQUIRE (strip.compFetRatio.load (std::memory_order_relaxed) == 1);
        // Timings scale 6 % per detent: 4 ticks on the 0.2 ms default.
        r.process (makeCc (mcu::cc::VPotRotateBase + 2, 0x04), 0);
        REQUIRE_THAT (strip.compFetAttack.load (std::memory_order_relaxed),
                      WithinAbs (0.2524954f, 1e-5f));
        r.process (makeCc (mcu::cc::VPotRotateBase + 3, 0x0A), 0);   // 10 ticks on 400 ms
        REQUIRE_THAT (strip.compFetRelease.load (std::memory_order_relaxed),
                      WithinAbs (716.3391f, 1e-2f));

        REQUIRE_THAT (strip.compVcaRatio.load (std::memory_order_relaxed),
                      WithinAbs (4.0f, 1e-4f));
        REQUIRE_THAT (strip.compVcaAttack.load (std::memory_order_relaxed),
                      WithinAbs (1.0f, 1e-4f));
    }

    SECTION ("VCA drives the VCA params")
    {
        strip.compMode.store (2, std::memory_order_relaxed);
        r.process (makeCc (mcu::cc::VPotRotateBase + 1, 0x0A), 0);   // 10 ticks on 4:1
        REQUIRE_THAT (strip.compVcaRatio.load (std::memory_order_relaxed),
                      WithinAbs (7.1633908f, 1e-4f));
        r.process (makeCc (mcu::cc::VPotRotateBase + 3, 0x14), 0);   // 20 ticks on 100 ms
        REQUIRE_THAT (strip.compVcaRelease.load (std::memory_order_relaxed),
                      WithinAbs (320.7135f, 1e-2f));
        REQUIRE (strip.compFetRatio.load (std::memory_order_relaxed) == 0);
    }

    SECTION ("Opto leaves every one of them alone")
    {
        strip.compMode.store (0, std::memory_order_relaxed);
        r.process (makeCc (mcu::cc::VPotRotateBase + 1, 0x0A), 0);
        r.process (makeCc (mcu::cc::VPotRotateBase + 2, 0x0A), 0);
        r.process (makeCc (mcu::cc::VPotRotateBase + 3, 0x0A), 0);
        REQUIRE_THAT (strip.compVcaRatio.load (std::memory_order_relaxed),
                      WithinAbs (4.0f, 1e-4f));
        REQUIRE_THAT (strip.compVcaAttack.load (std::memory_order_relaxed),
                      WithinAbs (1.0f, 1e-4f));
        REQUIRE_THAT (strip.compVcaRelease.load (std::memory_order_relaxed),
                      WithinAbs (100.0f, 1e-4f));
        REQUIRE (strip.compFetRatio.load (std::memory_order_relaxed) == 0);
        REQUIRE_THAT (strip.compFetAttack.load (std::memory_order_relaxed),
                      WithinAbs (0.2f, 1e-4f));
    }
}

// A percentage step has to keep moving at the ends of the range: the FET's
// attack floor is 0.02 ms, where half a millisecond per detent was the whole
// useful 1176 region in two clicks and any fixed step from the floor would
// either overshoot it or, going the other way, stick.
TEST_CASE ("McuReceiver: attack / release stepping still moves at the domain edges",
           "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);
    s.mcu.assignMode.store (6, std::memory_order_relaxed);
    auto& strip = s.track (0).strip;
    strip.compMode.store (1, std::memory_order_relaxed);          // FET

    strip.compFetAttack.store (0.02f, std::memory_order_relaxed);
    r.process (makeCc (mcu::cc::VPotRotateBase + 2, 0x01), 0);
    REQUIRE_THAT (strip.compFetAttack.load (std::memory_order_relaxed),
                  WithinAbs (0.0212f, 1e-6f));

    // Down from the floor holds at the floor rather than drifting below it.
    r.process (makeCc (mcu::cc::VPotRotateBase + 2, 0x40 | 0x05), 0);
    REQUIRE_THAT (strip.compFetAttack.load (std::memory_order_relaxed),
                  WithinAbs (0.02f, 1e-6f));

    // And a spin past the ceiling saturates there instead of overshooting.
    r.process (makeCc (mcu::cc::VPotRotateBase + 3, 0x3F), 0);
    REQUIRE_THAT (strip.compFetRelease.load (std::memory_order_relaxed),
                  WithinAbs (1100.0f, 1e-3f));
}

TEST_CASE ("McuReceiver: V-pot push (COMP mode) resets to the mode's own default",
           "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);
    s.mcu.assignMode.store (6, std::memory_order_relaxed);
    auto& strip = s.track (0).strip;

    // Threshold parks at no compression, which is +12 dB on VCA and 0 dB
    // (0 % reduction) on the other two.
    strip.compMode.store (2, std::memory_order_relaxed);
    strip.compVcaThreshDb.store (-20.0f, std::memory_order_relaxed);
    strip.compVcaAttack.store (30.0f, std::memory_order_relaxed);
    r.process (makeNoteOn (mcu::btn::VPotPushBase + 0, 0x7F), 0);
    r.process (makeNoteOn (mcu::btn::VPotPushBase + 2, 0x7F), 0);
    REQUIRE_THAT (strip.compVcaThreshDb.load (std::memory_order_relaxed),
                  WithinAbs (12.0f, 1e-4f));
    REQUIRE_THAT (strip.compVcaAttack.load (std::memory_order_relaxed),
                  WithinAbs (1.0f, 1e-4f));

    strip.compMode.store (1, std::memory_order_relaxed);
    strip.compFetThresholdDb.store (-45.0f, std::memory_order_relaxed);
    strip.compFetRelease.store (900.0f, std::memory_order_relaxed);
    r.process (makeNoteOn (mcu::btn::VPotPushBase + 0, 0x7F), 0);
    r.process (makeNoteOn (mcu::btn::VPotPushBase + 3, 0x7F), 0);
    REQUIRE_THAT (strip.compFetThresholdDb.load (std::memory_order_relaxed),
                  WithinAbs (0.0f, 1e-4f));
    REQUIRE_THAT (strip.compFetRelease.load (std::memory_order_relaxed),
                  WithinAbs (400.0f, 1e-4f));

    strip.compMode.store (0, std::memory_order_relaxed);
    strip.compOptoPeakRed.store (65.0f, std::memory_order_relaxed);
    r.process (makeNoteOn (mcu::btn::VPotPushBase + 0, 0x7F), 0);
    REQUIRE_THAT (strip.compOptoPeakRed.load (std::memory_order_relaxed),
                  WithinAbs (0.0f, 1e-4f));
}

// The threshold encoder is relative, so its base has to survive a reload -
// and it now comes from the mode's own param rather than a saved dial that
// nothing plays.
TEST_CASE ("McuReceiver: threshold nudge base survives a session reload",
           "[mcu][receiver]")
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dusk-studio-mcu-thresh-"
                                          + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    const auto target = dir.getChildFile ("session.json");

    Session a;
    McuReceiver ra (a);
    a.mcu.assignMode.store (6, std::memory_order_relaxed);
    a.track (0).strip.compMode.store (1, std::memory_order_relaxed);   // FET
    ra.process (makeCc (mcu::cc::VPotRotateBase + 0, 0x40 | 0x14), 0); // -20 ticks = -10 dB
    REQUIRE_THAT (a.track (0).strip.compFetThresholdDb.load (std::memory_order_relaxed),
                  WithinAbs (-20.0f, 1e-4f));
    REQUIRE (SessionSerializer::save (a, target));

    Session b;
    REQUIRE (SessionSerializer::load (b, target));
    REQUIRE_THAT (b.track (0).strip.compFetThresholdDb.load (std::memory_order_relaxed),
                  WithinAbs (-20.0f, 1e-4f));

    McuReceiver rb (b);
    rb.process (makeCc (mcu::cc::VPotRotateBase + 0, 0x02), 0);
    REQUIRE_THAT (b.track (0).strip.compFetThresholdDb.load (std::memory_order_relaxed),
                  WithinAbs (-19.0f, 1e-4f));

    dir.deleteRecursively();
}

TEST_CASE ("McuReceiver: PLAY / STOP / RECORD buttons enqueue pendingTransportAction",
           "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);

    r.process (makeNoteOn (mcu::btn::Play, 0x7F), 0);
    REQUIRE (s.pendingTransportAction.load (std::memory_order_relaxed)
             == (int) PendingTransportAction::Play);

    s.pendingTransportAction.store ((int) PendingTransportAction::None,
                                      std::memory_order_relaxed);
    r.process (makeNoteOn (mcu::btn::Stop, 0x7F), 0);
    REQUIRE (s.pendingTransportAction.load (std::memory_order_relaxed)
             == (int) PendingTransportAction::Stop);

    s.pendingTransportAction.store ((int) PendingTransportAction::None,
                                      std::memory_order_relaxed);
    r.process (makeNoteOn (mcu::btn::Record, 0x7F), 0);
    REQUIRE (s.pendingTransportAction.load (std::memory_order_relaxed)
             == (int) PendingTransportAction::Record);
}

TEST_CASE ("McuReceiver: REWIND / FAST-FORWARD publish held-state on press and release",
           "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);

    r.process (makeNoteOn (mcu::btn::Rewind, 0x7F), 0);   // press
    REQUIRE (s.mcu.rewHeld.load (std::memory_order_relaxed));
    r.process (makeNoteOn (mcu::btn::Rewind, 0x00), 0);   // release
    REQUIRE (! s.mcu.rewHeld.load (std::memory_order_relaxed));

    r.process (makeNoteOn (mcu::btn::FastForward, 0x7F), 0);
    REQUIRE (s.mcu.ffwdHeld.load (std::memory_order_relaxed));
    r.process (makeNoteOn (mcu::btn::FastForward, 0x00), 0);
    REQUIRE (! s.mcu.ffwdHeld.load (std::memory_order_relaxed));
}

TEST_CASE ("McuReceiver: fader touch sense flips faderTouched latch", "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);

    r.process (makeNoteOn (mcu::btn::FaderTouchBase + 4, 0x7F), 0);
    REQUIRE (s.track (4).strip.faderTouched.load (std::memory_order_relaxed));
    r.process (makeNoteOn (mcu::btn::FaderTouchBase + 4, 0), 0);
    REQUIRE_FALSE (s.track (4).strip.faderTouched.load (std::memory_order_relaxed));

    r.process (makeNoteOn (mcu::btn::FaderTouchMaster, 0x7F), 0);
    REQUIRE (s.master().faderTouched.load (std::memory_order_relaxed));
}

// The jog wheel scrubs the playhead. setPlayhead is not RT-safe, so the
// receiver queues the destination on the same atom Rewind / FFwd use and the
// message thread moves the transport; a queued value is what "scrubbed" looks
// like from here.
TEST_CASE ("McuReceiver: jog wheel scrubs the playhead", "[mcu][receiver]")
{
    Session s;
    McuReceiver r (s);
    constexpr std::int64_t kPerDetent = 2400;

    SECTION ("a detent each way moves from where the block started")
    {
        r.process (makeCc (mcu::cc::JogWheel, 1), 48000);
        CHECK (s.pendingTransportPlayhead.load (std::memory_order_relaxed)
                   == 48000 + kPerDetent);

        s.pendingTransportPlayhead.store (-1, std::memory_order_relaxed);
        r.process (makeCc (mcu::cc::JogWheel, 0x40 | 1), 48000);
        CHECK (s.pendingTransportPlayhead.load (std::memory_order_relaxed)
                   == 48000 - kPerDetent);
    }

    SECTION ("a spin back past the start stops at zero")
    {
        r.process (makeCc (mcu::cc::JogWheel, 0x40 | 20), 1000);
        CHECK (s.pendingTransportPlayhead.load (std::memory_order_relaxed) == 0);
    }

    SECTION ("detents in one block accumulate instead of overwriting")
    {
        juce::MidiBuffer mb;
        mb.addEvent (juce::MidiMessage::controllerEvent (1, mcu::cc::JogWheel, 2), 0);
        mb.addEvent (juce::MidiMessage::controllerEvent (1, mcu::cc::JogWheel, 3), 64);
        r.process (toDusk (mb), 0);
        CHECK (s.pendingTransportPlayhead.load (std::memory_order_relaxed)
                   == 5 * kPerDetent);
    }

    SECTION ("a detent of zero queues nothing")
    {
        r.process (makeCc (mcu::cc::JogWheel, 0), 48000);
        CHECK (s.pendingTransportPlayhead.load (std::memory_order_relaxed) == -1);

        r.process (makeCc (mcu::cc::JogWheel, 1), 48000);
        const auto queued = s.pendingTransportPlayhead.load (std::memory_order_relaxed);
        REQUIRE (queued == 48000 + kPerDetent);
        r.process (makeCc (mcu::cc::JogWheel, 0), 48000);
        CHECK (s.pendingTransportPlayhead.load (std::memory_order_relaxed) == queued);
    }
}
