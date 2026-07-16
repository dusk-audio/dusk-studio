#pragma once

#include "../foundation/MessageThread.h"
#include "../foundation/MidiBuffer.h"
#include <array>
#include <atomic>
#include <functional>

namespace duskstudio
{
class Session;

// Mackie Control feedback emitter. Polls Session state at 30 Hz, diffs
// vs the last-sent snapshot, pushes deltas to the MCU MIDI output.
//
// Wire format:
//   Channel faders : pitch-bend ch 0..7 (14-bit)
//   Master fader   : pitch-bend ch 8
//   Mute/Solo/Arm/Select LEDs : note-on 0x10+N / 0x08+N / 0x00+N / 0x18+N
//                               (vel 0x7F = lit, 0x00 = dark)
//   Transport LEDs : 0x5E PLAY / 0x5D STOP / 0x5F REC / 0x56 LOOP
//   Bank arrows    : 0x2E left / 0x2F right (lit when steppable)
//   Assign modes   : 0x28..0x2D (exactly one lit)
//   Selected ch    : 0x18 + (selected - bank*8) when inside visible bank
class McuController : public dusk::Timer
{
public:
    static constexpr int kStripsPerBank = 8;
    static constexpr int kRefreshHz     = 30;

    // Called on the message thread with the deltas to push. Empty
    // buffers are skipped before invocation.
    using SinkFn = std::function<void (const dusk::MidiBuffer&)>;

    explicit McuController (Session& sessionRef) noexcept;
    ~McuController() override;

    void setSink (SinkFn fn) noexcept { sink = std::move (fn); }

    // Provided so the controller can poll transport without an
    // AudioEngine reference. nullptr is OK - transport feedback skipped.
    void setTransportProvider (std::function<class Transport* ()> fn) noexcept
    {
        transportProvider = std::move (fn);
    }

    // Live device sample rate for the BBT timecode readout. Unset falls
    // back to 48 kHz (readout only - nothing audible depends on it).
    void setSampleRateProvider (std::function<double()> fn) noexcept
    {
        sampleRateProvider = std::move (fn);
    }

    // Re-emit every cached field next tick. Used when the user picks a
    // new MCU output so the controller doesn't start dark.
    void forceResync() noexcept { resyncRequested.store (true, std::memory_order_release); }

    // Skip the 30 Hz cadence and emit immediately - so on-screen PLAY
    // lights its LED before the next tick.
    void notifyTransportChanged() noexcept { transportEdgeRequested.store (true, std::memory_order_release); }

    // Test: build what the next tick would send without touching the
    // engine's output bank or updating lastSent caches.
    dusk::MidiBuffer buildBufferForTest();

private:
    void timerCallback() override;

    dusk::MidiBuffer buildEmitBuffer (bool forceAll);

    Session&     session;
    SinkFn       sink;
    std::function<class Transport* ()> transportProvider;
    std::function<double()>            sampleRateProvider;

    // -1 sentinel = never sent.
    std::array<int, kStripsPerBank> lastFader { -1, -1, -1, -1, -1, -1, -1, -1 };
    int lastMasterFader = -1;
    std::array<bool, kStripsPerBank> lastMute = {};
    std::array<bool, kStripsPerBank> lastSolo = {};
    std::array<bool, kStripsPerBank> lastArm  = {};
    int  lastBank            = -1;
    int  lastAssignMode      = -1;
    int  lastSelectedChannel = -1;
    int  lastTransportState  = -1;
    bool lastLoopOn          = false;

    // LCD 56 cols x 2 rows. Init to spaces so first diff fires non-blank.
    std::array<char, 56> lastLcdRow0 {};
    std::array<char, 56> lastLcdRow1 {};

    // 10 ASCII bytes - MCU's number panel adds the dots.
    std::array<char, 10> lastTimecode {};

    // Rolling: emit timecode every tick. Stopped: throttle to ~1 Hz so
    // static positions don't flood the bus.
    int  ticksSinceTimecode = 0;

    // 0..14 + clip-15. Throttled to every other tick (~15 Hz) - meters
    // are the highest-traffic MCU feedback.
    std::array<int, kStripsPerBank> lastMeter = { -1, -1, -1, -1, -1, -1, -1, -1 };
    bool emitMeterOnThisTick = false;

    std::array<int, kStripsPerBank> lastVpotRing = { -1, -1, -1, -1, -1, -1, -1, -1 };

    std::atomic<bool> resyncRequested        { true };  // first tick = full resync
    std::atomic<bool> transportEdgeRequested { false };
};
} // namespace duskstudio
