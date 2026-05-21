#include <catch2/catch_test_macros.hpp>

#include "engine/McuController.h"
#include "engine/McuProtocol.h"
#include "session/Session.h"

#include <juce_audio_basics/juce_audio_basics.h>

using namespace duskstudio;

namespace
{
// Find the first event in `buf` matching status + data1 (e.g. note 0x12,
// pitch-bend channel). Returns the velocity / value byte (data2) or
// -1 if not found. Avoids the test-asserting on event order across the
// buffer.
int findValueForNote (const juce::MidiBuffer& buf, int status, int data1)
{
    for (const auto meta : buf)
    {
        const auto& m = meta.getMessage();
        const auto* raw = m.getRawData();
        const int sz = m.getRawDataSize();
        if (raw == nullptr || sz < 3) continue;
        if ((raw[0] & 0xF0) == (status & 0xF0)
            && (raw[1] & 0x7F) == (data1 & 0x7F))
            return raw[2] & 0x7F;
    }
    return -1;
}

int findPitchBend14 (const juce::MidiBuffer& buf, int channel0Indexed)
{
    for (const auto meta : buf)
    {
        const auto& m = meta.getMessage();
        const auto* raw = m.getRawData();
        const int sz = m.getRawDataSize();
        if (raw == nullptr || sz < 3) continue;
        if ((raw[0] & 0xF0) == 0xE0
            && (raw[0] & 0x0F) == channel0Indexed)
            return (raw[1] & 0x7F) | ((raw[2] & 0x7F) << 7);
    }
    return -1;
}
} // namespace

// buildBufferForTest forces-all so the first call should contain every
// feedback surface the controller knows about. Lets the test verify the
// initial-state-sync that runs whenever a fresh controller plugs in.
TEST_CASE ("McuController: initial resync emits faders + LED state for 8 banked strips",
           "[mcu][controller]")
{
    Session s;
    // No AudioEngine or transport provider: the controller stays in
    // its idle state and the buffer reflects only Session atoms +
    // the fallback "Stop lit" transport state.
    McuController controller (s);

    // Stamp some non-default state on track 0 + track 3 + master.
    s.track (0).strip.faderDb.store (-6.0f, std::memory_order_relaxed);
    s.track (0).strip.liveFaderDb.store (-6.0f, std::memory_order_relaxed);
    s.track (3).strip.mute.store (true, std::memory_order_relaxed);
    s.track (5).strip.solo.store (true, std::memory_order_relaxed);
    s.track (1).recordArmed.store (true, std::memory_order_relaxed);
    s.master().faderDb.store (0.0f, std::memory_order_relaxed);
    s.master().liveFaderDb.store (0.0f, std::memory_order_relaxed);

    auto buf = controller.buildBufferForTest();

    // Track 0 fader: live -6 dB on a -100..+12 range -> norm ~= 0.839
    // -> pb14 ~= 13746. Allow a small rounding band.
    const int pb0 = findPitchBend14 (buf, 0);
    REQUIRE (pb0 >= 13000);
    REQUIRE (pb0 <= 14000);

    // Master fader: 0 dB -> norm = 100/112 -> pb14 ~= 14627.
    const int pbMaster = findPitchBend14 (buf, mcu::kMasterFaderIndex);
    REQUIRE (pbMaster >= 14000);
    REQUIRE (pbMaster <= 15500);

    // Mute LED on track 3 lit, others dark (vel 0x00).
    REQUIRE (findValueForNote (buf, 0x90, mcu::btn::MuteBase + 3) == 0x7F);
    REQUIRE (findValueForNote (buf, 0x90, mcu::btn::MuteBase + 0) == 0x00);

    // Solo LED on track 5.
    REQUIRE (findValueForNote (buf, 0x90, mcu::btn::SoloBase + 5) == 0x7F);

    // Rec arm on track 1.
    REQUIRE (findValueForNote (buf, 0x90, mcu::btn::RecArmBase + 1) == 0x7F);

    // Bank arrow LEDs: bank 0 -> left dim, right lit.
    REQUIRE (findValueForNote (buf, 0x90, mcu::btn::BankLeft)  == 0x00);
    REQUIRE (findValueForNote (buf, 0x90, mcu::btn::BankRight) == 0x7F);

    // Assign mode: default 0 = PAN -> AssignPan lit, others dark.
    REQUIRE (findValueForNote (buf, 0x90, mcu::btn::AssignPan)    == 0x7F);
    REQUIRE (findValueForNote (buf, 0x90, mcu::btn::AssignEq)     == 0x00);
    REQUIRE (findValueForNote (buf, 0x90, mcu::btn::AssignSend)   == 0x00);

    // Transport: default state Stopped -> Stop LED lit, Play / Record dark.
    REQUIRE (findValueForNote (buf, 0x90, mcu::btn::Stop)   == 0x7F);
    REQUIRE (findValueForNote (buf, 0x90, mcu::btn::Play)   == 0x00);
    REQUIRE (findValueForNote (buf, 0x90, mcu::btn::Record) == 0x00);
}

TEST_CASE ("McuController: bank flip relights the eight SELECT LEDs", "[mcu][controller]")
{
    Session s;
    McuController controller (s);

    // First pass: bank 0, selected channel 2. Strip 2's SELECT lights.
    s.mcu.selectedChannel.store (2, std::memory_order_relaxed);
    auto buf0 = controller.buildBufferForTest();
    REQUIRE (findValueForNote (buf0, 0x90, mcu::btn::SelectBase + 2) == 0x7F);
    REQUIRE (findValueForNote (buf0, 0x90, mcu::btn::SelectBase + 0) == 0x00);

    // Second pass: bank flips to 1. The same selectedChannel (2) is
    // now OUTSIDE the visible bank (which is tracks 8..15) so all 8
    // SELECT LEDs should go dark.
    s.mcu.bank.store (1, std::memory_order_relaxed);
    auto buf1 = controller.buildBufferForTest();
    for (int strip = 0; strip < McuController::kStripsPerBank; ++strip)
        REQUIRE (findValueForNote (buf1, 0x90, mcu::btn::SelectBase + strip) == 0x00);

    // Re-select track 10 -> visible at strip 2 in bank 1.
    s.mcu.selectedChannel.store (10, std::memory_order_relaxed);
    auto buf2 = controller.buildBufferForTest();
    REQUIRE (findValueForNote (buf2, 0x90, mcu::btn::SelectBase + 2) == 0x7F);
}
