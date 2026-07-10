#include "MidiSyncReceiver.h"

#include <algorithm>

namespace duskstudio
{
void MidiSyncReceiver::process (const dusk::MidiBuffer& events,
                                  std::int64_t blockStartSample) noexcept
{
    for (const auto meta : events)
    {
        const auto& msg = meta.getMessage();
        const auto* data = msg.getRawData();
        if (msg.getRawDataSize() < 1) continue;
        const std::uint8_t status = data[0];

        // F8 / FA / FB / FC are system-realtime bytes. We test the raw
        // status byte directly rather than relying on message-type helpers.
        if (status == 0xF8)  // Clock
        {
            const std::int64_t sampleAt = blockStartSample + meta.samplePosition;
            if (lastClockSample >= 0)
            {
                const std::int64_t dt = sampleAt - lastClockSample;
                if (dt > 0)
                {
                    // Glitch rejection: if we already have a running
                    // average and this tick's interval drifts more than
                    // the threshold, skip it. Keeps cable noise from
                    // yanking the tempo on a single bad tick.
                    bool keep = true;
                    if (filled >= kAvgWindow / 2)
                    {
                        std::int64_t sum = 0;
                        for (int i = 0; i < filled; ++i) sum += intervals[i];
                        const double avg = (double) sum / (double) filled;
                        if (avg > 0.0)
                        {
                            const double ratio = (double) dt / avg;
                            if (ratio > (double) kJitterRejectFactor
                                || ratio < 1.0 / (double) kJitterRejectFactor)
                                keep = false;
                        }
                    }

                    if (keep)
                    {
                        intervals[writeIdx] = dt;
                        writeIdx = (writeIdx + 1) % kAvgWindow;
                        if (filled < kAvgWindow) ++filled;

                        // Recompute BPM from the average interval. 24
                        // clocks per quarter; interval is samples per
                        // tick, so quarter = 24 * tick, BPM = 60 / qSec.
                        std::int64_t sum = 0;
                        for (int i = 0; i < filled; ++i) sum += intervals[i];
                        const double avgTickSamples = (double) sum / (double) filled;
                        if (avgTickSamples > 0.0 && sr > 0.0)
                        {
                            const double quarterSec = (avgTickSamples * 24.0) / sr;
                            if (quarterSec > 0.0)
                            {
                                const float computed = (float) (60.0 / quarterSec);
                                // Clamp to a sane range so a garbage
                                // stream doesn't push the session into
                                // 0 / inf BPM.
                                bpm.store (std::clamp (computed, 10.0f, 999.0f),
                                             std::memory_order_relaxed);
                            }
                        }
                    }
                }
            }
            lastClockSample = sampleAt;
        }
        else if (status == 0xFA)  // Start
        {
            rolling.store (true, std::memory_order_relaxed);
            // Reset history so the first inter-clock gap after Start
            // isn't measured against pre-Start cable noise.
            writeIdx = 0;
            filled = 0;
            lastClockSample = -1;
        }
        else if (status == 0xFB)  // Continue
        {
            rolling.store (true, std::memory_order_relaxed);
        }
        else if (status == 0xFC)  // Stop
        {
            rolling.store (false, std::memory_order_relaxed);
        }
    }
}
} // namespace duskstudio
