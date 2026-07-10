#pragma once

#include "../foundation/MidiBuffer.h"

#include <atomic>
#include <cstdint>

namespace duskstudio
{
// MTC decoder. Audio-thread only. Sibling of MidiSyncReceiver — Clock
// carries tempo, MTC carries absolute SMPTE (HH:MM:SS:FF).
//
// Decode surface:
//   0xF1 Quarter-Frame  — 1 status + 1 data (nibble index 0..7 + 4 bits).
//     8 QFs assemble one SMPTE value spanning 2 frames of wall time, so
//     at nibble-7 commit the assembled value is the time as it was 2
//     frames AGO. We add +2 at the commit so getFrames() is wall-clock
//     aligned with what the master is displaying right now.
//   F0 7F 7F 01 01 hr mn sc fr F7 Full-Frame sysex — instantaneous
//     locate. No +2 offset.
//
// Reverse scrubbing (v1: detect-and-park): masters scrubbing backward
// send 7→0. We freeze frames + rolling on two consecutive QFs with
// strictly decreasing nibbles and wait for a forward sequence resumption.
//
// Rolling watchdog: QF arrival rate, not nibble order. 500 ms covers a
// USB hiccup at 24 fps (96 QF/s) without falsely dropping on cable
// jitter.
class MidiTimeCodeReceiver
{
public:
    enum FrameRate : int { Fps24 = 0, Fps25 = 1, Fps29_97DF = 2, Fps30 = 3 };

    static constexpr FrameRate kDefaultRate = Fps30;
    static constexpr std::int64_t kRollingTimeoutSamplesAt48k = 24000;  // ~500 ms

    void prepare (double sampleRate) noexcept
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        rollingTimeoutSamples =
            (std::int64_t) ((kRollingTimeoutSamplesAt48k * sr) / 48000.0);
        reset();
    }

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

    // blockStartSample is the engine's monotonic sample clock (same as
    // MidiClockEmitter / MidiSyncReceiver).
    void process (const dusk::MidiBuffer& events,
                  std::int64_t blockStartSample,
                  int numSamples) noexcept;

    // Wall-clock-aligned (+2-frame offset already applied).
    std::int64_t getFrames() const noexcept
    {
        return currentFrames.load (std::memory_order_relaxed);
    }

    // False also during reverse scrub.
    bool isRolling() const noexcept
    {
        return rolling.load (std::memory_order_relaxed);
    }

    bool isReversed() const noexcept
    {
        return reversed.load (std::memory_order_relaxed);
    }

    FrameRate getFrameRate() const noexcept
    {
        return (FrameRate) detectedRate.load (std::memory_order_relaxed);
    }

private:
    void onQuarterFrame (std::uint8_t data1, std::int64_t atSample) noexcept;
    void onFullFrameSysex (const std::uint8_t* msg, int sz, std::int64_t atSample) noexcept;
    void commitAssembledFrame (std::int64_t atSample,
                                bool applyTwoFrameOffset) noexcept;

    double sr = 48000.0;
    std::int64_t rollingTimeoutSamples = 12000;

    // QF assembly: 8 nibbles → 32 bits split (frames lo+hi, secs lo+hi,
    // mins lo+hi, hours+rate lo+hi). Outer index picks the SMPTE field.
    std::uint8_t nibbleAccumulator[4] {};

    // Commit only after observing 0..7 in exact order without gaps.
    // Without this, a reverse-scrub stream (7,6,5,4,3,2,1,0,7,...) would
    // wake on nibble 0, partially populate from stale lo/hi bytes, and
    // emit garbage with mm=ss=0.
    int expectedNibble = 0;

    std::int64_t lastEventSample = -1;

    std::atomic<std::int64_t> currentFrames { 0 };
    std::atomic<bool>        rolling       { false };
    std::atomic<bool>        reversed      { false };
    std::atomic<int>         detectedRate  { (int) kDefaultRate };
};
} // namespace duskstudio
