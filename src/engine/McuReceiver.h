#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace duskstudio
{
class Session;

// Decodes incoming Mackie Control Universal protocol bytes from the
// session's chosen MCU input device. Sibling of MidiSyncReceiver +
// MidiTimeCodeReceiver: audio-thread only, no allocation, no locks.
// AudioEngine drives `process(events, blockStart)` once per audio
// block whenever `Session::mcu.resolvedInputIdx` is non-negative; the
// receiver pulls bytes from the matching `perInputMidi[idx]` buffer
// and writes the decoded events directly to Session atoms.
//
// MCU surface decoded by this class:
//   - Pitch-bend (status 0xE0+N) — channel N is strip N's fader
//     (banked); channel 8 is the master fader. 14-bit position.
//   - Note On 0x00..0x07 — rec arm per strip
//   - Note On 0x08..0x0F — solo per strip
//   - Note On 0x10..0x17 — mute per strip
//   - Note On 0x18..0x1F — channel select per strip (drives the
//     globally-selected channel)
//   - Note On 0x20..0x27 — V-pot push (encoder click; resets the
//     current parameter target to its default)
//   - Note On 0x28..0x2D — assign-mode buttons (PAN / SEND /
//     PLUGIN / EQ / INST; the receiver only maps PAN / SEND
//     / EQ / COMP for now per the v1 plan)
//   - Note On 0x2E / 0x2F — bank LEFT / RIGHT (step by 8)
//   - Note On 0x30 / 0x31 — channel LEFT / RIGHT (step by 1)
//   - Note On 0x56 — loop toggle
//   - Note On 0x5B / 0x5C — rewind / fast-forward (engine.jumpTo*
//     via the existing pendingTransportPlayhead path)
//   - Note On 0x5D / 0x5E / 0x5F — stop / play / record
//   - Note On 0x68..0x70 — fader touch sense (drives the strip's
//     faderTouched latch so write/touch automation behaves)
//   - CC 0x10..0x17 — V-pot rotation (signed 7-bit delta)
//   - CC 0x3C       — jog wheel rotation (signed 7-bit delta;
//     drives the playhead while transport is stopped)
//
// `process` is RT-safe. Writes to Session atoms use release-ordering
// for state flags (mute / solo / arm), relaxed for continuous params
// (fader / pan / send). Transport actions go through the existing
// `Session::pendingTransportAction` queue rather than calling
// engine.play() directly - same pattern MTC chase uses.
class McuReceiver
{
public:
    explicit McuReceiver (Session& sessionRef) noexcept : session (sessionRef) {}

    // Audio-thread reset (e.g. on user switching MCU input device).
    // Forgets any in-flight V-pot accumulator state. No allocations.
    void reset() noexcept;

    // Audio-thread entry. `events` is the perInputMidi[mcuIdx] buffer
    // for the current block; `blockStartSample` is the engine's
    // absolute playhead at the start of the block (used to time
    // jog-wheel scrub steps against a monotonic sample clock).
    void process (const juce::MidiBuffer& events,
                  std::int64_t blockStartSample) noexcept;

private:
    Session& session;

    // Convert a 14-bit MCU pitch-bend value (0..16383) into the
    // session's faderDb range. Linear-in-dB for v1; MCU's hardware
    // fader curve is actually log-derived but matches Logic's
    // calibration only - a future commit can swap in the proper
    // curve when motorized-fader users complain about feel.
    float pitchBendToFaderDb (int pb14) const noexcept;

    // Reverse mapping (used by McuController emit). Kept here so the
    // receiver + controller share the same curve constants.
    int faderDbToPitchBend (float db) const noexcept;

    // Dispatch a V-pot delta (signed -63..+63) through the current
    // assign-mode atom to the right strip parameter.
    void applyVpotDelta (int stripIndex, int delta) noexcept;

    // Reset the V-pot's current target to its default value (called
    // on the encoder's push action). E.g. PAN -> 0, SEND -> -inf dB.
    void resetVpotTarget (int stripIndex) noexcept;

    // Shared note-press handler. `noteNumber` is the MCU button code;
    // `pressed` is true for vel >= 0x40, false for vel 0.
    void handleNotePress (int noteNumber, bool pressed) noexcept;
};
} // namespace duskstudio
