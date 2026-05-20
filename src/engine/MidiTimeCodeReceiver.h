#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

namespace focal
{
// MTC (MIDI Time Code) decoder. Companion to MidiSyncReceiver — Clock
// carries musical tempo, MTC carries absolute SMPTE time (HH:MM:SS:FF).
// Audio-thread only; engine feeds per-block MidiBuffer captured from
// the chosen sync input and reads back the decoded frame count +
// rolling state on the same thread.
//
// Decode surface:
//   - 0xF1 Quarter-Frame: 1 status byte + 1 data byte carrying a nibble
//     index (0..7) + 4 data bits. 8 quarter-frames complete one
//     "running SMPTE value" — the time at the START of the QF sequence.
//   - F0 7F 7F 01 01 hr mn sc fr F7 Full-Frame sysex: an instantaneous
//     SMPTE locate (sent on master's transport edges + frame-rate
//     changes). No transmission-delay compensation needed for sysex.
//
// Transmission-delay compensation (the standard "2-frame lag"):
//   It takes 8 QF messages to assemble one SMPTE value, spanning 2
//   frames of wall time. So when nibble 7 arrives, the value just
//   assembled is the time as it was 2 frames AGO. To publish wall-
//   clock-aligned frames, we add +2 to currentFrames at the nibble-7
//   commit point during forward playback. Sysex full-frame skips the
//   +2 (it carries an instantaneous value).
//
// Reverse-QF handling (v1: detect-and-park):
//   Masters scrubbing backward send nibbles 7→0 instead of 0→7. v1
//   does NOT support reverse chase. On detecting two consecutive QFs
//   with strictly decreasing nibble index, the receiver freezes
//   currentFrames + isRolling and stays parked until a forward
//   sequence resumes (nibble 0 arrives). No garbage frames emitted.
//
// Rolling watchdog: QF arrival rate, not nibble order. If we haven't
// seen a QF in kRollingTimeoutSamples, isRolling falls to false. A
// running master pushes 4× framerate QFs/sec; the watchdog at >250 ms
// covers comfortably the slowest legitimate cadence (24 fps = 96
// QF/sec = ~10.4 ms between QFs).
class MidiTimeCodeReceiver
{
public:
    enum FrameRate : int { Fps24 = 0, Fps25 = 1, Fps29_97DF = 2, Fps30 = 3 };

    // Default: 30 fps. The decoded frame rate is recovered from QF
    // nibble 7's two high bits + full-frame sysex byte 5 — both will
    // override this default the moment a real signal arrives.
    static constexpr FrameRate kDefaultRate = Fps30;

    // QF arrival watchdog. If the engine sees no QF / sysex for this
    // long, declare not-rolling. 500 ms covers a few-QF USB-MIDI
    // hiccup at 24 fps (96 QF/sec, ~10.4 ms between QFs) without
    // falsely dropping rolling on real-world cable jitter. Matches
    // Pro Tools' default freewheel timeout magnitude.
    static constexpr juce::int64 kRollingTimeoutSamplesAt48k = 24000; // ~500 ms

    void prepare (double sampleRate) noexcept
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        rollingTimeoutSamples =
            (juce::int64) ((kRollingTimeoutSamplesAt48k * sr) / 48000.0);
        reset();
    }

    // Drop all state — used on source-port change, transport stop, or
    // when caller explicitly wants to re-prime.
    void reset() noexcept
    {
        nibbleAccumulator[0] = nibbleAccumulator[1] =
            nibbleAccumulator[2] = nibbleAccumulator[3] = 0;
        expectedNibble       = 0;
        lastEventSample      = -1;
        currentFrames.store (0, std::memory_order_relaxed);
        rolling.store       (false, std::memory_order_relaxed);
        reversed.store      (false, std::memory_order_relaxed);
        detectedRate.store  ((int) kDefaultRate, std::memory_order_relaxed);
    }

    // Audio-thread entry. blockStartSample is the engine's monotonic
    // sample clock at the start of this block (same clock the
    // MidiClockEmitter / MidiSyncReceiver share). Walks the MidiBuffer
    // for QF + full-frame sysex bytes; everything else is ignored.
    void process (const juce::MidiBuffer& events,
                  juce::int64 blockStartSample,
                  int numSamples) noexcept;

    // Absolute SMPTE frame count from 00:00:00:00. UI converts back to
    // HH:MM:SS:FF via the decoded frame rate. Includes the +2 wall-
    // clock-alignment offset (so this value matches what the master is
    // displaying right now, not what it WAS 2 frames ago).
    juce::int64 getFrames() const noexcept
    {
        return currentFrames.load (std::memory_order_relaxed);
    }

    // QF-arrival watchdog. False also during reverse scrubbing.
    bool isRolling() const noexcept
    {
        return rolling.load (std::memory_order_relaxed);
    }

    // True when the master is scrubbing backward. v1 freezes the
    // decoded frames in this state — chase logic ignores reverse;
    // UI can show a cue.
    bool isReversed() const noexcept
    {
        return reversed.load (std::memory_order_relaxed);
    }

    // Frame rate recovered from QF nibble 7 / full-frame sysex.
    FrameRate getFrameRate() const noexcept
    {
        return (FrameRate) detectedRate.load (std::memory_order_relaxed);
    }

private:
    void onQuarterFrame (juce::uint8 data1, juce::int64 atSample) noexcept;
    void onFullFrameSysex (const juce::uint8* msg, int sz, juce::int64 atSample) noexcept;
    void commitAssembledFrame (juce::int64 atSample,
                                bool applyTwoFrameOffset) noexcept;

    double sr = 48000.0;
    juce::int64 rollingTimeoutSamples = 12000;

    // QF assembly: 8 nibbles → 32 bits split as (frames lo+hi,
    // seconds lo+hi, minutes lo+hi, hours+rate lo+hi). Index in the
    // outer array picks which of the 4 SMPTE fields we're filling.
    juce::uint8 nibbleAccumulator[4] {};

    // Strict monotonic-sequence validator. We commit + publish ONLY
    // after observing nibbles 0,1,2,3,4,5,6,7 in that exact order
    // without gaps. Any out-of-order arrival (reverse-scrub, USB drop,
    // duplicate) resets `expectedNibble` to 0 and waits for a fresh
    // sequence start. Without this, a steady reverse-scrub stream
    // (7,6,5,4,3,2,1,0,7,6,...) would wake the receiver on nibble 0,
    // partially populate the accumulator with stale lo/hi bytes from
    // the previous sequence, and emit garbage SMPTE values with
    // mm=ss=0.
    int expectedNibble = 0;          // next nibble we'll accept (0..7)

    juce::int64 lastEventSample = -1;

    std::atomic<juce::int64> currentFrames { 0 };
    std::atomic<bool>        rolling       { false };
    std::atomic<bool>        reversed      { false };
    std::atomic<int>         detectedRate  { (int) kDefaultRate };
};
} // namespace focal
