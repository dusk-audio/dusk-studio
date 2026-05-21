#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>
#include <array>
#include <atomic>
#include <functional>

namespace duskstudio
{
class Session;

// Message-thread-side feedback emitter for the Mackie Control surface.
// Sibling of McuReceiver - the receiver decodes hardware->host events on
// the audio thread; this class polls the resulting Session state at 30
// Hz, diffs vs the last-sent snapshot, and pushes only the deltas to
// the MCU's MIDI output. Wraps the entire bidirectional half of the
// MCU bridge in one juce::Timer subclass so the AudioEngine owns it
// alongside McuReceiver via unique_ptr.
//
// Wire format (per-feedback):
//   * Channel faders: pitch-bend ch 0..7 (14-bit)
//   * Master fader:   pitch-bend ch 8
//   * Mute / Solo / Arm / Select LEDs: note-on 0x10+N / 0x08+N /
//     0x00+N / 0x18+N (vel 0x7F = lit, 0x00 = dark)
//   * Transport LEDs: note-on 0x5E (PLAY) / 0x5D (STOP) / 0x5F (REC) /
//     0x56 (LOOP)
//   * Bank arrow LEDs: 0x2E (left) / 0x2F (right) - lit when steppable
//   * Assign-mode LEDs: 0x28..0x2D - exactly one lit at a time
//   * Selected-channel LED: 0x18 + (selectedChannel - bank*8) when the
//     selected channel falls inside the visible bank
//
// LCD + timecode + meter + V-pot rings ship in steps 4 / 5; this class
// reserves slots for their lastSent caches so the diff path stays the
// same shape across all the feedback surfaces.
class McuController : public juce::Timer
{
public:
    static constexpr int kStripsPerBank = 8;
    static constexpr int kRefreshHz     = 30;

    // SinkFn is called on the message thread with the deltas the
    // controller wants pushed to the MCU output port. Empty buffer is
    // skipped before the callback fires. AudioEngine wires this up to
    // its `sendMidiToOutput(resolvedOutputIdx, buf)` in the engine's
    // ctor; tests can leave it empty and pull buffers via
    // buildBufferForTest instead.
    using SinkFn = std::function<void (const juce::MidiBuffer&)>;

    explicit McuController (Session& sessionRef) noexcept;
    ~McuController() override;

    // Provide a sink for the timer-driven emit. McuController stays
    // valid without one (it just builds buffers and drops them).
    void setSink (SinkFn fn) noexcept { sink = std::move (fn); }

    // External hook so callers can give the controller a way to read
    // transport state without an AudioEngine reference. Wired in
    // AudioEngine's ctor to `[this]() { return this->getTransport(); }`.
    // Returning a Transport* lets the controller poll loopEnabled +
    // state. Null is acceptable - controller skips transport feedback
    // until it's set.
    void setTransportProvider (std::function<class Transport* ()> fn) noexcept
    {
        transportProvider = std::move (fn);
    }

    // Re-emit every cached field on the next tick, even if it matches
    // the last-sent value. Called when the user picks a new MCU output
    // device (so a freshly-connected controller doesn't start dark).
    void forceResync() noexcept { resyncRequested.store (true, std::memory_order_release); }

    // Push a single 1-block buffer right now (skip the 30 Hz cadence).
    // Used by the on-screen transport buttons so PLAY's LED lights
    // before the timer's next tick. Cheap; same code path the timer
    // uses. Message-thread only.
    void notifyTransportChanged() noexcept { transportEdgeRequested.store (true, std::memory_order_release); }

    // Test entry. Builds the buffer the next timer tick would send
    // and returns it without touching the engine's MIDI output bank.
    // The caller passes the test's assertion target. Doesn't update
    // the lastSent caches - leaves them as-is so the same diff can be
    // re-checked across multiple state changes.
    juce::MidiBuffer buildBufferForTest();

private:
    void timerCallback() override;

    // Build a buffer of deltas from the current Session state, updating
    // lastSent* caches in place. Returns the buffer (possibly empty if
    // nothing changed). `forceAll` re-emits every field, used by
    // forceResync on a device reconnect.
    juce::MidiBuffer buildEmitBuffer (bool forceAll);

    // Mapping helpers shared with McuReceiver. Linear-in-dB for v1.
    int faderDbToPitchBend (float db) const noexcept;

    Session&     session;
    SinkFn       sink;
    std::function<class Transport* ()> transportProvider;

    // Cached last-sent state. -1 sentinels for "never sent". Bank +
    // selected channel + assign mode write the same byte regardless of
    // freshness - we still cache so the diff is a single int compare.
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

    std::atomic<bool> resyncRequested        { true };  // first tick re-emits
    std::atomic<bool> transportEdgeRequested { false };
};
} // namespace duskstudio
