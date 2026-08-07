#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/McuProtocol.h"
#include "engine/McuReceiver.h"
#include "session/Session.h"
#include "session/SessionSerializer.h"
#include "support/DuskMidiTestBridge.h"

#include <juce_audio_basics/juce_audio_basics.h>

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
