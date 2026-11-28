#pragma once

#include "MidiBuffer.h"
#include "MidiRing.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// Retiming MIDI collector built on dusk::MidiRing, replacing
// JUCE's MidiMessageCollector. The backend MIDI thread stamps each incoming
// message with a hi-res ms timestamp and pushes it (addMessage); the audio
// thread pulls a block's worth once per callback (removeNextBlock), converting
// the timestamps into per-sample offsets within the block.
//
// This preserves JUCE's collector semantics but moves the retime arithmetic
// from add-time to drain-time. JUCE computes each event's sample number under a
// mutex in addMessageToQueue; we cannot, because sampleRate / lastCallbackTime
// are consumer-side state (audio thread only) and the producer holds no lock.
// So the producer only records the raw timestamp into the lock-free ring, and
// the consumer computes every sample number at drain time against the same
// lastCallbackTime JUCE would have used (unchanged since the previous drain).
// The observable mapping - right-align when the backlog is short, squeeze +
// window when it is long, drop events older than one second - is identical.
//
// The clock is injected (removeNextBlock takes nowMs) so tests are
// deterministic; the seam passes the real hi-res clock. That injection is why
// no exact A/B against JUCE's MidiMessageCollector is attempted - JUCE reads the
// wall clock internally, which makes bit-exact comparison timing-flaky.
namespace dusk
{
// Where a block sits in a device cycle handed over as several blocks (see
// duskstudio::device::CallbackContext, whose fields these are). All zero for a
// block that is a whole cycle.
struct MidiCyclePlace
{
    int offset = 0;
    int samples = 0;
    std::uint32_t serial = 0;
};

class MidiCollector
{
public:
    explicit MidiCollector (std::size_t ringCapacityBytes = kMidiBlockBytes)
        : ring (ringCapacityBytes)
    {
        cycle.reserveBytes (ringCapacityBytes);
    }

    // Off-RT only. Resize the backing ring.
    void setCapacity (std::size_t capacityBytes)
    {
        ring.reset (capacityBytes);
        cycle.clear();
        cycle.reserveBytes (capacityBytes);
        cycleTaken = heldEnd = 0;
    }

    // Off-RT (audioDeviceAboutToStart). Set the sample rate the timestamps
    // convert against and clear any backlog; nowMs seeds lastCallbackTime.
    void reset (double newSampleRate, double nowMs) noexcept
    {
        sampleRate       = newSampleRate;
        lastCallbackTime = nowMs;
        ring.clear();
        cycle.clear();
        cycleTaken = heldEnd = 0;
    }

    // Producer (backend MIDI thread). timeMs is the hi-res ms timestamp in the
    // same clock domain removeNextBlock is later called with. Returns false if
    // the ring was full and the message was dropped.
    bool addMessage (const std::uint8_t* bytes, int numBytes, double timeMs) noexcept
    {
        return ring.push (bytes, numBytes, timeMs);
    }

    // Consumer (audio thread), once per block. Retimes the pending events into
    // out with offsets in [0, numSamples-1]. nowMs is the current hi-res ms
    // clock. Drains the ring (destructive) - call exactly once per block.
    //
    // out must be EMPTY on entry - an over-cap block is delivered by clearing
    // it, which discards anything the caller left in there.
    //
    // Whole-block-or-nothing: if the block cumulatively overruns out's reserved
    // cap it is delivered empty, because keeping the head would let a note-on
    // through while its note-off fell off the tail, hanging the note. A single
    // record too big for out even when empty is undeliverable at any cut, so it
    // drops alone and the rest of the block survives. Reserve out at the ring's
    // capacity (kMidiBlockBytes) and the cumulative case cannot arise at all.
    void removeNextBlock (dusk::MidiBuffer& out, int numSamples, double nowMs) noexcept
    {
        const double prevLast = lastCallbackTime;
        lastCallbackTime = nowMs;   // advance the clock unconditionally, as JUCE does

        if (numSamples <= 0) { ring.clear(); return; }

        // Snapshot the producer position once so the scan and the drain cover the
        // identical record set: a message pushed mid-block (after this cursor)
        // stays pending for the next block rather than being drained here with a
        // stale-computed backlog.
        const std::size_t end = ring.producerCursor();

        // Pass 1: largest sample number, for the >1 s backlog trim. JUCE trims
        // incrementally at add-time; the equivalent single-shot floor is
        // (maxSampleNumber - sampleRate) when the newest event is over a second
        // past the last drain.
        int  maxS = std::numeric_limits<int>::min();
        bool any  = false;
        ring.forEachUntil (end, [&] (int, double timeMs)
        {
            const int s = toSample (timeMs, prevLast);
            if (s > maxS) maxS = s;
            any = true;
        });
        if (! any) return;

        const int dropBelow = ((double) maxS > sampleRate)
                                ? maxS - (int) sampleRate
                                : std::numeric_limits<int>::min();

        int numSourceSamples = std::max (1, roundToInt ((nowMs - prevLast) * 0.001 * sampleRate));
        int startSample = 0;
        bool overCap = false;

        if (numSourceSamples > numSamples)
        {
            // Backlog longer than the block: squeeze it in, windowing to at most
            // numSamples<<5 source samples so an extreme backlog keeps only the
            // most recent slice.
            const int maxBlockLengthToUse = numSamples << 5;
            if (numSourceSamples > maxBlockLengthToUse)
            {
                startSample      = numSourceSamples - maxBlockLengthToUse;
                numSourceSamples = maxBlockLengthToUse;
            }
            const int scale   = (numSamples << 10) / numSourceSamples;
            const int floorS  = std::max (startSample, dropBelow);
            ring.drainUntil (end, [&] (const std::uint8_t* bytes, int n, double timeMs)
            {
                const int s = toSample (timeMs, prevLast);
                if (s < floorS) return;
                const int pos = ((s - startSample) * scale) >> 10;
                if (! out.addEvent (bytes, n, std::clamp (pos, 0, numSamples - 1))
                      && out.fitsWhenEmpty (n))
                    overCap = true;
            });
        }
        else
        {
            // Backlog shorter than the block: right-align towards the end.
            startSample = numSamples - numSourceSamples;
            ring.drainUntil (end, [&] (const std::uint8_t* bytes, int n, double timeMs)
            {
                const int s = toSample (timeMs, prevLast);
                if (s < dropBelow) return;
                if (! out.addEvent (bytes, n, std::clamp (s + startSample, 0, numSamples - 1))
                      && out.fitsWhenEmpty (n))
                    overCap = true;
            });
        }

        if (overCap) out.clear();
    }

    // Consumer (audio thread), once per block, for a device that hands one
    // cycle over as several blocks run back to back (see
    // device::CallbackContext). Every block of the cycle runs at the same
    // moment, so draining each by the clock would put the whole cycle's input
    // in the first. The first block (offset 0) retimes the pending events over
    // the whole cycle instead, exactly as removeNextBlock would for one block
    // that long, and each block takes the events that fall inside it.
    // A cycle at or under numSamples long is a whole cycle: removeNextBlock.
    //
    // The callback can sit blocks out (the engine is suspended while a device
    // is reconfigured), which leaves held events whose block never ran. They
    // go out late, at the start of the next block that does run, in this cycle
    // or a later one - all but the note-ons. A late note-off or pedal-up ends
    // what an earlier block started; a note-on that late would only start a
    // note whose end may already have gone by.
    void removeNextSubBlock (dusk::MidiBuffer& out, int numSamples,
                             const MidiCyclePlace& place, double nowMs) noexcept
    {
        if (place.samples <= numSamples)
        {
            releaseHeld (out, heldEnd);
            removeNextBlock (out, numSamples, nowMs);
            return;
        }
        if (place.offset <= 0 || place.serial != heldCycle)
            releaseHeld (out, heldEnd);
        if (place.offset <= 0)
        {
            removeNextBlock (cycle, place.samples, nowMs);
            heldCycle = place.serial;
            heldEnd = place.samples;
        }
        if (numSamples <= 0) return;

        // This cycle's blocks that never ran, then this block's own events.
        const int taken = cycleTaken;
        releaseHeld (out, place.offset, false);
        bool overCap = false;
        for (const auto meta : cycle)
        {
            const int pos = meta.samplePosition - place.offset;
            if (meta.samplePosition < taken || pos < 0 || pos >= numSamples) continue;
            if (! out.addEvent (meta.data, meta.numBytes, pos) && out.fitsWhenEmpty (meta.numBytes))
                overCap = true;
        }
        if (overCap) out.clear();
        cycleTaken = std::max (taken, place.offset + numSamples);
    }

private:
    // Hands out, at the start of the block, the held events from cycleTaken up
    // to `before` that are not note-ons, and counts them taken. With `drop`
    // the held cycle is over: nothing of it is kept.
    void releaseHeld (dusk::MidiBuffer& out, int before, bool drop = true) noexcept
    {
        for (const auto meta : cycle)
        {
            if (meta.samplePosition < cycleTaken || meta.samplePosition >= before) continue;
            const bool noteOn = meta.numBytes >= 3 && (meta.data[0] & 0xF0) == 0x90 && meta.data[2] != 0;
            if (! noteOn)
                out.addEvent (meta.data, meta.numBytes, 0);
        }
        cycleTaken = std::max (cycleTaken, before);
        if (drop)
        {
            cycle.clear();
            cycleTaken = 0;
            heldEnd = 0;
        }
    }

    // Event sample number relative to the last drain, matching JUCE's
    // (timeStamp - 0.001*lastCallbackTime)*sampleRate with timeStamp in seconds;
    // here both timestamps are ms. Truncates toward zero, as JUCE's (int) cast.
    int toSample (double timeMs, double refMs) const noexcept
    {
        return (int) ((timeMs - refMs) * 0.001 * sampleRate);
    }

    // Round to nearest (ties to even, matching JUCE's roundToInt under the
    // default FPU rounding mode) - used only for numSourceSamples.
    static int roundToInt (double v) noexcept { return (int) std::lrint (v); }

    dusk::MidiRing ring;
    // A split cycle's events at their offsets in the cycle; audio thread only.
    dusk::MidiBuffer cycle;
    std::uint32_t heldCycle = 0;   // the serial of the cycle those are from
    int heldEnd = 0;               // that cycle's length
    int cycleTaken = 0;            // its samples already handed out
    double sampleRate       = 44100.0;
    double lastCallbackTime = 0.0;   // ms; consumer-side only, no atomics needed
};
} // namespace dusk
