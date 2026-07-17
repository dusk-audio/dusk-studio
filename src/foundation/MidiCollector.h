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
class MidiCollector
{
public:
    explicit MidiCollector (std::size_t ringCapacityBytes = 8192)
        : ring (ringCapacityBytes) {}

    // Off-RT only. Resize the backing ring.
    void setCapacity (std::size_t capacityBytes) { ring.reset (capacityBytes); }

    // Off-RT (audioDeviceAboutToStart). Set the sample rate the timestamps
    // convert against and clear any backlog; nowMs seeds lastCallbackTime.
    void reset (double newSampleRate, double nowMs) noexcept
    {
        sampleRate       = newSampleRate;
        lastCallbackTime = nowMs;
        ring.clear();
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
                out.addEvent (bytes, n, std::clamp (pos, 0, numSamples - 1));
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
                out.addEvent (bytes, n, std::clamp (s + startSample, 0, numSamples - 1));
            });
        }
    }

    double getSampleRate() const noexcept { return sampleRate; }

private:
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
    double sampleRate       = 44100.0;
    double lastCallbackTime = 0.0;   // ms; consumer-side only, no atomics needed
};
} // namespace dusk
