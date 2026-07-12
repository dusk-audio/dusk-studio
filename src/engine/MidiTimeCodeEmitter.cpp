#include "MidiTimeCodeEmitter.h"

#include <cmath>
#include <cstdlib>

namespace duskstudio
{
namespace
{
// Nominal frame rates per enum index, kept in lock-step with the
// receiver so the emit + decode sides never disagree on what 2 ==
// 29.97 DF.
constexpr double kNominalFps[4] = { 24.0, 25.0, 30000.0 / 1001.0, 30.0 };

constexpr double samplesPerFrame (double sr, MidiTimeCodeEmitter::FrameRate r) noexcept
{
    return sr / kNominalFps[(int) r];
}

constexpr int framesPerSecondGrid (MidiTimeCodeEmitter::FrameRate r) noexcept
{
    // For drop-frame the grid is 30 (master encodes on the 30-fps
    // grid + skips frames 0,1 at minute boundaries except every 10th);
    // for non-drop it's the nominal int rate.
    return (r == MidiTimeCodeEmitter::FrameRate::Fps29_97DF)
           ? 30
           : (int) kNominalFps[(int) r];
}

// frames (absolute count from 00:00:00:00 in drop-aware semantics) ->
// (hh, mm, ss, ff) tuple suitable for MTC encoding. Symmetric to
// MidiTimeCodeReceiver::smpteToFrames.
void framesToSmpte (std::int64_t frames,
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
        constexpr std::int64_t perTenMin = 17982;
        constexpr std::int64_t perOneMin = 1798;
        const std::int64_t D = frames / perTenMin;
        const std::int64_t M = frames % perTenMin;
        // Within a 10-minute block: minute 0 carries 1800 frames; minutes
        // 1..9 carry 1798 each. Distribute M accordingly.
        std::int64_t minute, frameInMinute;
        if (M < 1800)
        {
            minute        = 0;
            frameInMinute = M;
        }
        else
        {
            const std::int64_t X = M - 1800;
            minute        = 1 + X / perOneMin;
            frameInMinute = X % perOneMin;
        }
        // Add back the 2 dropped frame indices at the start of every
        // non-10th minute (so the on-wire ff matches the master's display).
        if (minute > 0)
            frameInMinute += 2;

        const std::int64_t totalMinutes = D * 10 + minute;
        hh = (int) (totalMinutes / 60);
        mm = (int) (totalMinutes % 60);
        ss = (int) (frameInMinute / 30);
        ff = (int) (frameInMinute % 30);
    }
    else
    {
        const int fps = framesPerSecondGrid (rate);
        const std::int64_t totalSeconds = frames / fps;
        ff = (int) (frames % fps);
        ss = (int) (totalSeconds % 60);
        const std::int64_t totalMinutes = totalSeconds / 60;
        mm = (int) (totalMinutes % 60);
        hh = (int) (totalMinutes / 60);
    }

    // SMPTE timecode is a 24-hour clock; wrap so the emitted hours field stays
    // in spec (0-23) past the 24h boundary instead of overflowing the field.
    hh %= 24;
}
} // namespace

void MidiTimeCodeEmitter::emitQuarterFrame (std::int64_t atSample,
                                              int nibble,
                                              std::int64_t frames,
                                              FrameRate rate,
                                              dusk::MidiBuffer& out) noexcept
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
    const std::uint8_t qf[2] = { 0xF1, (std::uint8_t) (((nibble & 0x07) << 4) | (data & 0x0F)) };
    out.addEvent (qf, 2, (int) (atSample - lastBlockStart));
}

void MidiTimeCodeEmitter::emitFullFrameSysex (std::int64_t atSample,
                                                std::int64_t frames,
                                                FrameRate rate,
                                                dusk::MidiBuffer& out) noexcept
{
    int hh = 0, mm = 0, ss = 0, ff = 0;
    framesToSmpte (frames, rate, hh, mm, ss, ff);

    // F0 7F 7F 01 01 hr mn sc fr F7 - 10 bytes. hr packs rate in bits 5..6.
    const std::uint8_t hrByte = (std::uint8_t) ((((int) rate) << 5) | (hh & 0x1F));
    const std::uint8_t bytes[10] = {
        0xF0, 0x7F, 0x7F, 0x01, 0x01,
        hrByte,
        (std::uint8_t) (mm & 0x3F),
        (std::uint8_t) (ss & 0x3F),
        (std::uint8_t) (ff & 0x1F),
        0xF7
    };
    out.addEvent (bytes, 10, (int) (atSample - lastBlockStart));
}

void MidiTimeCodeEmitter::generateBlock (std::int64_t blockStartSample,
                                           int numSamples,
                                           std::int64_t playheadSamples,
                                           bool isRolling,
                                           FrameRate rate,
                                           dusk::MidiBuffer& out) noexcept
{
    if (numSamples <= 0 || sr <= 0.0) return;
    lastBlockStart = blockStartSample;

    const std::int64_t blockEnd = blockStartSample + (std::int64_t) numSamples;
    const double      sPerFrame = samplesPerFrame (sr, rate);
    if (sPerFrame <= 0.0) return;
    const double sPerQF = sPerFrame * 0.25;   // 4 QFs per frame

    // Frame-rate change -> restart with a fresh full-frame sysex so
    // slaves learn the new rate from sysex byte 5 instead of waiting
    // for the next nibble-7 round. Same shape as a transport-jump.
    if ((int) rate != lastEmittedRate)
    {
        needFullFrameSysex = true;
        nibbleIdx          = 0;
        lastEmittedRate    = (int) rate;
    }

    // Transport rolling edges. Falling edge -> Stop-equivalent (no
    // explicit MTC byte; the slave's QF watchdog times out). Rising
    // edge -> arm a full-frame sysex Locate at block offset 0.
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
    // sequence encodes (currentFrames - 2) - see header comment on
    // the 2-frame compensation. Sequence value is frozen at nibble 0
    // and held across all 8 nibbles.
    const auto liveFrames = (std::int64_t) (playheadSamples / sPerFrame);

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

    // Realign on long idle (large gap since last QF - emitter was
    // disabled or sr changed mid-stream). Mirrors the Clock emitter
    // anti-burst guard.
    if (nextQuarterFrameSample + (std::int64_t) (sPerQF * 4.0) < blockStartSample)
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
            sequenceFrames = (std::int64_t) ((double) qfPlayhead / sPerFrame) - 2;
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
        nextQuarterFrameSample += (std::int64_t) std::llround (sPerQF);
    }
}
} // namespace duskstudio
