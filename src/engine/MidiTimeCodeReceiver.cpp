#include "MidiTimeCodeReceiver.h"

namespace focal
{
namespace
{
// Frame rates as ratios (numerator, denominator) — kept exact for the
// drop-frame math, which counts whole frames per 10-minute block.
struct FrameRateInfo
{
    double fps;       // nominal (24.0 / 25.0 / 29.97 / 30.0)
    bool   dropFrame; // 29.97 DF only
};

constexpr FrameRateInfo kRates[4] =
{
    { 24.0,    false }, // Fps24
    { 25.0,    false }, // Fps25
    { 30000.0 / 1001.0, true }, // Fps29_97DF (29.97 drop-frame)
    { 30.0,    false }, // Fps30
};

// Convert (HH, MM, SS, FF, rate) to an absolute frame count from
// 00:00:00:00 — the model the rest of Focal sees. For non-drop rates
// this is the obvious h*nominalFps*3600 + m*nominalFps*60 + s*nominalFps + f.
// For 29.97 DF we COUNT the drop-frame skips: every minute except every
// 10th drops 2 frame indices (00 and 01) — the wall-clock frame count
// stays continuous because the master also drops them.
juce::int64 smpteToFrames (int hh, int mm, int ss, int ff,
                            MidiTimeCodeReceiver::FrameRate rate) noexcept
{
    if (rate == MidiTimeCodeReceiver::Fps29_97DF)
    {
        // Standard 29.97 DF formula. nominalFps = 30 for arithmetic
        // (the master encodes minutes/seconds using the 30-fps grid
        // and drops 2 frames per minute except every 10th).
        const juce::int64 totalMinutes = (juce::int64) hh * 60 + mm;
        const juce::int64 dropped = 2 * (totalMinutes - totalMinutes / 10);
        const juce::int64 grid30  = ((juce::int64) hh * 3600
                                     + (juce::int64) mm * 60
                                     + ss) * 30 + ff;
        return grid30 - dropped;
    }

    const int fps = (int) kRates[(int) rate].fps;
    return ((juce::int64) hh * 3600
          + (juce::int64) mm * 60
          + ss) * fps + ff;
}
} // namespace

void MidiTimeCodeReceiver::onQuarterFrame (juce::uint8 data1,
                                             juce::int64 atSample) noexcept
{
    // F1 data byte: high nibble = message type (0..7), low nibble = data.
    const int nibbleIdx = (data1 >> 4) & 0x07;
    const int dataBits  = data1 & 0x0F;

    // Strict monotonic-sequence validator. We accept a nibble only if
    // it matches the one we were expecting next (0 → 1 → ... → 7).
    // Any deviation (reverse-scrub, USB drop, duplicate) snaps the
    // expectation back to 0 and parks the receiver until a fresh
    // forward sequence starts. Without this guard, reverse-scrub
    // streams sneak through the nibble-0 wake path and emit garbage
    // SMPTE values with partially-stale accumulator bytes.
    if (nibbleIdx != expectedNibble)
    {
        // Detect reverse-scrub for the UI cue. Two heuristics:
        //   - any nibble strictly less than the one we were expecting
        //     and we'd already started a sequence (expectedNibble > 0)
        //   - OR: nibble 7 arrives when we were expecting 0 (master
        //     just reversed and re-started a sequence at the top)
        if ((expectedNibble > 0 && nibbleIdx < expectedNibble)
            || (expectedNibble == 0 && nibbleIdx == 7))
        {
            reversed.store (true, std::memory_order_relaxed);
            rolling .store (false, std::memory_order_relaxed);
        }
        expectedNibble = 0;  // wait for fresh forward sequence start
        return;
    }

    if (nibbleIdx == 0)
    {
        // Fresh forward sequence — clear accumulator + exit reverse-park.
        reversed.store (false, std::memory_order_relaxed);
        nibbleAccumulator[0] = (juce::uint8) dataBits;
        nibbleAccumulator[1] = nibbleAccumulator[2] = nibbleAccumulator[3] = 0;
    }
    else
    {
        // Each nibble pair (lo, hi) builds one byte of (frames, secs,
        // mins, hr+rate). nibbleIdx 0..1 → frames, 2..3 → seconds,
        // 4..5 → minutes, 6..7 → hours+rate.
        const int byteIdx = nibbleIdx / 2;
        if ((nibbleIdx & 1) == 0)
            nibbleAccumulator[byteIdx] =
                (juce::uint8) ((nibbleAccumulator[byteIdx] & 0xF0) | dataBits);
        else
            nibbleAccumulator[byteIdx] =
                (juce::uint8) ((nibbleAccumulator[byteIdx] & 0x0F)
                                 | (dataBits << 4));
    }

    // Nibble 7 finishes the SMPTE value. Commit + publish, then wrap
    // the expectation back to 0 for the next sequence.
    if (nibbleIdx == 7)
    {
        commitAssembledFrame (atSample, /*applyTwoFrameOffset*/ true);
        expectedNibble = 0;
    }
    else
    {
        expectedNibble = nibbleIdx + 1;
    }
}

void MidiTimeCodeReceiver::commitAssembledFrame (juce::int64 atSample,
                                                   bool applyTwoFrameOffset) noexcept
{
    // Layout: [0] = ff (0..29 or 0..30 depending on rate)
    //         [1] = ss (0..59)
    //         [2] = mm (0..59)
    //         [3] = hh (0..23) | (rate<<5 in top 3 bits)
    const int ff = nibbleAccumulator[0] & 0x1F;
    const int ss = nibbleAccumulator[1] & 0x3F;
    const int mm = nibbleAccumulator[2] & 0x3F;
    const int hh = nibbleAccumulator[3] & 0x1F;
    const int rateBits = (nibbleAccumulator[3] >> 5) & 0x03;

    const FrameRate rate = (FrameRate) rateBits;
    detectedRate.store ((int) rate, std::memory_order_relaxed);

    juce::int64 frames = smpteToFrames (hh, mm, ss, ff, rate);
    if (applyTwoFrameOffset)
        frames += 2;  // 2-frame QF transmission-delay compensation

    currentFrames.store (frames, std::memory_order_relaxed);
    rolling      .store (true,   std::memory_order_relaxed);
    lastEventSample = atSample;
}

void MidiTimeCodeReceiver::onFullFrameSysex (const juce::uint8* msg, int sz,
                                               juce::int64 atSample) noexcept
{
    // F0 7F 7F 01 01 hr mn sc fr F7 — 10 bytes. Indices into msg:
    //   0: F0
    //   1: 7F (universal real time)
    //   2: 7F (broadcast)
    //   3: 01 (MTC sub-id 1)
    //   4: 01 (full-frame)
    //   5: hh | (rate << 5)
    //   6: mm
    //   7: ss
    //   8: ff
    //   9: F7
    if (sz < 10 || msg[0] != 0xF0 || msg[1] != 0x7F || msg[3] != 0x01
        || msg[4] != 0x01)
        return;

    nibbleAccumulator[0] = (juce::uint8) (msg[8] & 0x1F);  // ff
    nibbleAccumulator[1] = (juce::uint8) (msg[7] & 0x3F);  // ss
    nibbleAccumulator[2] = (juce::uint8) (msg[6] & 0x3F);  // mm
    nibbleAccumulator[3] = (juce::uint8) (msg[5] & 0x7F);  // hh + rate
    // No +2 offset — full-frame sysex carries the master's INSTANT
    // playhead. Also exits reverse-park (re-locate is a forward op).
    reversed.store (false, std::memory_order_relaxed);
    commitAssembledFrame (atSample, /*applyTwoFrameOffset*/ false);
}

void MidiTimeCodeReceiver::process (const juce::MidiBuffer& events,
                                      juce::int64 blockStartSample,
                                      int numSamples) noexcept
{
    for (const auto meta : events)
    {
        const auto msg     = meta.getMessage();
        const auto* raw    = msg.getRawData();
        const int   sz     = msg.getRawDataSize();
        if (raw == nullptr || sz < 1) continue;

        const auto status = (juce::uint8) raw[0];
        const juce::int64 atSample = blockStartSample + meta.samplePosition;

        if (status == 0xF1 && sz >= 2)
        {
            onQuarterFrame ((juce::uint8) raw[1], atSample);
        }
        else if (status == 0xF0)
        {
            onFullFrameSysex (raw, sz, atSample);
        }
    }

    // Watchdog: drop rolling if QFs stopped arriving. lastEventSample
    // tracks the most recent QF or full-frame; if blockEnd hasn't seen
    // anything in kRollingTimeoutSamples we declare master stopped.
    const juce::int64 blockEnd = blockStartSample + numSamples;
    if (rolling.load (std::memory_order_relaxed)
        && lastEventSample >= 0
        && blockEnd - lastEventSample > rollingTimeoutSamples)
    {
        rolling.store  (false, std::memory_order_relaxed);
        reversed.store (false, std::memory_order_relaxed);
    }
}
} // namespace focal
