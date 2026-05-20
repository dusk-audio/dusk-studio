#include "MidiTimeCodeEmitter.h"

namespace focal
{
namespace
{
// Same rate descriptors as the receiver — kept in lock-step so the
// emit-side and decode-side never disagree on what 2 == 29.97 DF.
struct RateInfo
{
    double nominalFps;   // 24 / 25 / 29.97 / 30
    bool   dropFrame;
};

constexpr RateInfo kRates[4] =
{
    { 24.0,                false },
    { 25.0,                false },
    { 30000.0 / 1001.0,    true  },
    { 30.0,                false },
};

constexpr double samplesPerFrame (double sr, MidiTimeCodeEmitter::FrameRate r) noexcept
{
    return sr / kRates[(int) r].nominalFps;
}

constexpr int framesPerSecondGrid (MidiTimeCodeEmitter::FrameRate r) noexcept
{
    // For drop-frame the grid is 30 (master encodes on the 30-fps
    // grid + skips frames 0,1 at minute boundaries except every 10th);
    // for non-drop it's the nominal int rate.
    return (r == MidiTimeCodeEmitter::FrameRate::Fps29_97DF)
           ? 30
           : (int) kRates[(int) r].nominalFps;
}

// frames (absolute count from 00:00:00:00 in drop-aware semantics) →
// (hh, mm, ss, ff) tuple suitable for MTC encoding. Symmetric to
// MidiTimeCodeReceiver::smpteToFrames.
void framesToSmpte (juce::int64 frames,
                     MidiTimeCodeEmitter::FrameRate rate,
                     int& hh, int& mm, int& ss, int& ff) noexcept
{
    if (frames < 0) frames = 0;

    if (rate == MidiTimeCodeEmitter::FrameRate::Fps29_97DF)
    {
        // Inverse of receiver's drop-frame formula. The wall-clock
        // count is `frames`; we need to find (hh, mm, ss, ff) on the
        // 30-fps grid such that grid - 2*(mins - mins/10) == frames.
        // Closed-form via the "frames per 10-minute block" identity:
        //   1 minute  = 30*60 - 2 = 1798 frames (drop)
        //   1 minute  = 30*60     = 1800 frames (every 10th)
        //   10 minutes = 9*1798 + 1800 = 17982 frames
        constexpr juce::int64 perTenMin = 17982;
        constexpr juce::int64 perOneMin = 1798;
        const juce::int64 D = frames / perTenMin;
        const juce::int64 M = frames % perTenMin;
        // Within a 10-minute block: minute 0 carries 1800 frames; minutes
        // 1..9 carry 1798 each. Distribute M accordingly.
        juce::int64 minute, frameInMinute;
        if (M < 1800)
        {
            minute        = 0;
            frameInMinute = M;
        }
        else
        {
            const juce::int64 X = M - 1800;
            minute        = 1 + X / perOneMin;
            frameInMinute = X % perOneMin;
        }
        // Add back the 2 dropped frame indices at the start of every
        // non-10th minute (so the on-wire ff matches the master's display).
        if (minute > 0)
            frameInMinute += 2;

        const juce::int64 totalMinutes = D * 10 + minute;
        hh = (int) (totalMinutes / 60);
        mm = (int) (totalMinutes % 60);
        ss = (int) (frameInMinute / 30);
        ff = (int) (frameInMinute % 30);
    }
    else
    {
        const int fps = framesPerSecondGrid (rate);
        const juce::int64 totalSeconds = frames / fps;
        ff = (int) (frames % fps);
        ss = (int) (totalSeconds % 60);
        const juce::int64 totalMinutes = totalSeconds / 60;
        mm = (int) (totalMinutes % 60);
        hh = (int) (totalMinutes / 60);
    }
}
} // namespace

void MidiTimeCodeEmitter::emitQuarterFrame (juce::int64 atSample,
                                              int nibble,
                                              juce::int64 frames,
                                              FrameRate rate,
                                              juce::MidiBuffer& out) noexcept
{
    int hh = 0, mm = 0, ss = 0, ff = 0;
    framesToSmpte (frames, rate, hh, mm, ss, ff);

    // Nibble layout:
    //   0: ff low  4 bits      (ff & 0x0F)
    //   1: ff high 1 bit       ((ff >> 4) & 0x01)
    //   2: ss low  4 bits      (ss & 0x0F)
    //   3: ss high 2 bits      ((ss >> 4) & 0x03)
    //   4: mm low  4 bits      (mm & 0x0F)
    //   5: mm high 2 bits      ((mm >> 4) & 0x03)
    //   6: hh low  4 bits      (hh & 0x0F)
    //   7: hh high 1 bit + rate (rateBits << 1 | (hh >> 4))
    int data = 0;
    switch (nibble)
    {
        case 0: data = ff & 0x0F; break;
        case 1: data = (ff >> 4) & 0x01; break;
        case 2: data = ss & 0x0F; break;
        case 3: data = (ss >> 4) & 0x03; break;
        case 4: data = mm & 0x0F; break;
        case 5: data = (mm >> 4) & 0x03; break;
        case 6: data = hh & 0x0F; break;
        case 7: data = (((int) rate) << 1) | ((hh >> 4) & 0x01); break;
        default: return;
    }
    const juce::uint8 byte1 = (juce::uint8) (((nibble & 0x07) << 4) | (data & 0x0F));
    out.addEvent (juce::MidiMessage (0xF1, byte1),
                   (int) (atSample - lastBlockStart));
}

void MidiTimeCodeEmitter::emitFullFrameSysex (juce::int64 atSample,
                                                juce::int64 frames,
                                                FrameRate rate,
                                                juce::MidiBuffer& out) noexcept
{
    int hh = 0, mm = 0, ss = 0, ff = 0;
    framesToSmpte (frames, rate, hh, mm, ss, ff);

    // F0 7F 7F 01 01 hr mn sc fr F7 — 10 bytes. hr packs rate in bits 5..6.
    const juce::uint8 hrByte = (juce::uint8) ((((int) rate) << 5) | (hh & 0x1F));
    const juce::uint8 bytes[10] = {
        0xF0, 0x7F, 0x7F, 0x01, 0x01,
        hrByte,
        (juce::uint8) (mm & 0x3F),
        (juce::uint8) (ss & 0x3F),
        (juce::uint8) (ff & 0x1F),
        0xF7
    };
    out.addEvent (juce::MidiMessage (bytes, 10),
                   (int) (atSample - lastBlockStart));
}

void MidiTimeCodeEmitter::generateBlock (juce::int64 blockStartSample,
                                           int numSamples,
                                           juce::int64 playheadSamples,
                                           bool isRolling,
                                           FrameRate rate,
                                           juce::MidiBuffer& out) noexcept
{
    if (numSamples <= 0 || sr <= 0.0) return;
    lastBlockStart = blockStartSample;

    const juce::int64 blockEnd = blockStartSample + (juce::int64) numSamples;
    const double      sPerFrame = samplesPerFrame (sr, rate);
    if (sPerFrame <= 0.0) return;
    const double sPerQF = sPerFrame * 0.25;   // 4 QFs per frame

    // Frame-rate change → restart with a fresh full-frame sysex so
    // slaves learn the new rate from sysex byte 5 instead of waiting
    // for the next nibble-7 round. Same shape as a transport-jump.
    if ((int) rate != lastEmittedRate)
    {
        needFullFrameSysex = true;
        nibbleIdx          = 0;
        lastEmittedRate    = (int) rate;
    }

    // Transport rolling edges. Falling edge → Stop-equivalent (no
    // explicit MTC byte; the slave's QF watchdog times out). Rising
    // edge → arm a full-frame sysex Locate at block offset 0.
    if (isRolling != lastRolling)
    {
        if (isRolling)
        {
            needFullFrameSysex = true;
            nibbleIdx          = 0;
        }
        lastRolling = isRolling;
    }

    if (! isRolling) return;

    // Convert the live playhead to absolute SMPTE frames. The QF
    // sequence encodes (currentFrames - 2) — see header comment on
    // the 2-frame compensation. Sequence value is frozen at nibble 0
    // and held across all 8 nibbles.
    const auto liveFrames = (juce::int64) (playheadSamples / sPerFrame);

    // Detect a transport jump: if our running sequence value differs
    // from the live value by more than kJumpDetectFrames, the user
    // must have locked the playhead to a new position. Emit a fresh
    // sysex + restart the QF cycle so the slave snaps cleanly.
    if (! needFullFrameSysex
        && std::llabs (liveFrames - sequenceFrames - 2) > kJumpDetectFrames)
    {
        needFullFrameSysex = true;
        nibbleIdx          = 0;
    }

    if (needFullFrameSysex)
    {
        emitFullFrameSysex (blockStartSample, liveFrames, rate, out);
        sequenceFrames         = liveFrames - 2;     // pre-roll for QF sequence
        nibbleIdx              = 0;
        nextQuarterFrameSample = blockStartSample;
        needFullFrameSysex     = false;
    }

    // Realign on long idle (large gap since last QF — emitter was
    // disabled or sr changed mid-stream). Mirrors the Clock emitter
    // anti-burst guard.
    if (nextQuarterFrameSample + (juce::int64) (sPerQF * 4.0) < blockStartSample)
    {
        nextQuarterFrameSample = blockStartSample;
        nibbleIdx              = 0;
        sequenceFrames         = liveFrames - 2;
    }

    // Walk the block placing QF bytes at each scheduled sample.
    // sequenceFrames is refreshed at nibble 0 (start of a new 8-QF
    // sequence); each subsequent nibble encodes a slice of the SAME
    // sequenceFrames value (the 2-frame-pre-roll contract).
    //
    // Value encoded = transport playhead at the QF's sample position,
    // minus 2 frames for the QF transmission-delay compensation.
    // Use `playheadSamples` (the caller's transport snapshot, at
    // blockStart) + the QF's offset-into-block to extrapolate the
    // playhead's wall-clock position at the QF instant. Sample clock
    // and transport advance at the same rate (1:1) so the linear
    // extrapolation is exact.
    while (nextQuarterFrameSample < blockEnd)
    {
        if (nibbleIdx == 0)
        {
            const auto offsetInBlock = nextQuarterFrameSample - blockStartSample;
            const auto qfPlayhead    = playheadSamples + offsetInBlock;
            sequenceFrames = (juce::int64) ((double) qfPlayhead / sPerFrame) - 2;
            if (sequenceFrames < 0) sequenceFrames = 0;
        }

        if (nextQuarterFrameSample >= blockStartSample)
        {
            const int offset = (int) (nextQuarterFrameSample - blockStartSample);
            if (offset >= 0 && offset < numSamples)
                emitQuarterFrame (nextQuarterFrameSample, nibbleIdx,
                                    sequenceFrames, rate, out);
        }

        nibbleIdx = (nibbleIdx + 1) & 0x07;   // 0..7 cycling
        nextQuarterFrameSample += (juce::int64) std::llround (sPerQF);
    }
}
} // namespace focal
