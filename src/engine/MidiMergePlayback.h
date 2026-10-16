#pragma once

#include "../session/MidiPlayback.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace duskstudio
{
// Independent of the capture SPSC: both mutation and scheduling belong to
// the audio thread. configure requires the producer to be stopped.
class MidiMergePlayback
{
public:
    using SampleToTick = std::function<std::int64_t (std::int64_t)>;
    static constexpr std::size_t kDefaultCapacity = 65536;

    explicit MidiMergePlayback (std::size_t capacity = kDefaultCapacity)
        : records (std::max<std::size_t> (1, capacity)) {}

    void configure (std::uint64_t gesture, std::int64_t captureLength, SampleToTick conversion)
    {
        gestureId = gesture;
        length = captureLength;
        sampleToTick = std::move (conversion);
        lengthTicks = std::max<std::int64_t> (1, ticksAt (length));
        head = tail = 0;
        held = {};
        heldCount = 0;
        currentPass = 1;
        observedPass = 0;
        available = length > 0;
        overflowed = false;
    }

    void pushEvent (int pass, std::int64_t sample, std::uint8_t status,
                    std::uint8_t first, std::uint8_t second) noexcept
    {
        if (! available || pass < currentPass || sample < 0 || sample >= length) return;
        advanceToPass (pass);
        if (! available) return;
        observedPass = std::max (observedPass, pass);
        const int kind = status & 0xf0;
        const int key = (status & 15) * 128 + (first & 127);
        if (kind == 0x90 && second > 0)
        {
            closeNote (key, sample);
            if (! available) return;
            if (held[(std::size_t) key].velocity == 0)
            {
                auto at = std::lower_bound (heldKeys.begin(), heldKeys.begin() + heldCount, key);
                std::move_backward (at, heldKeys.begin() + heldCount, heldKeys.begin() + heldCount + 1);
                *at = key;
                ++heldCount;
            }
            held[(std::size_t) key] = { sample, second };
        }
        else if (kind == 0x80 || kind == 0x90)
        {
            closeNote (key, sample);
            if (! available || held[(std::size_t) key].velocity == 0) return;
            auto at = std::lower_bound (heldKeys.begin(), heldKeys.begin() + heldCount, key);
            std::move (at + 1, heldKeys.begin() + heldCount, at);
            --heldCount;
            held[(std::size_t) key] = {};
        }
        else
        {
            Record record;
            record.pass = pass;
            record.start = std::min (ticksAt (sample), lengthTicks - 1);
            record.status = status;
            record.first = first;
            record.second = second;
            append (record);
        }
    }

    void publishProgress (int pass, std::int64_t end, bool complete) noexcept
    {
        if (! available || pass < currentPass) return;
        advanceToPass (pass);
        if (end > 0) observedPass = std::max (observedPass, pass);
        if (complete && pass < std::numeric_limits<int>::max()) advanceToPass (pass + 1);
    }

    void discardConsumed (MidiCaptureStamp consumed) noexcept
    {
        const int through = consumedPass (consumed);
        auto first = head, past = tail;
        while (first < past)
        {
            const auto middle = first + (past - first) / 2;
            if (records[(std::size_t) (middle % records.size())].pass <= through)
                first = middle + 1;
            else
                past = middle;
        }
        head = first;
    }

    // Missing raw capture or a full bridge must never leave speculative
    // notes sounding. Committed timelines take over for this gesture.
    void recoverGap() noexcept
    {
        if (! available) return;
        available = false;
        head = tail;
        heldCount = 0;
        overflowed = true;
    }

    bool takeOverflow() noexcept { return std::exchange (overflowed, false); }

    template <typename Visit>
    bool forEachNote (MidiCaptureStamp consumed, int playbackPass, int& scans, Visit&& visit) const noexcept
    {
        if (! available) return true;
        const int through = consumedPass (consumed);
        for (auto i = head; i < tail; ++i)
        {
            if (! takeScan (scans)) return false;
            const auto& record = records[(std::size_t) (i % records.size())];
            if (record.pass > through && record.pass < playbackPass && (record.status & 0xf0) == 0x90
                && ! visit (MidiNote { (record.status & 15) + 1, record.first, record.second,
                                      record.start, record.end - record.start }))
                return false;
        }
        if (currentPass > through && currentPass < playbackPass && currentPass <= observedPass)
            for (int i = 0; i < heldCount; ++i)
            {
                if (! takeScan (scans)) return false;
                const int key = heldKeys[(std::size_t) i];
                const auto record = noteRecord (key, length);
                if (! visit (MidiNote { key / 128 + 1, key % 128, record.second,
                                       record.start, record.end - record.start }))
                    return false;
            }
        return true;
    }

    template <typename Visit>
    bool forEachControl (MidiCaptureStamp consumed, int playbackPass, int& scans, Visit&& visit) const noexcept
    {
        if (! available) return true;
        const int through = consumedPass (consumed);
        for (auto i = head; i < tail; ++i)
        {
            if (! takeScan (scans)) return false;
            const auto& record = records[(std::size_t) (i % records.size())];
            if (record.pass <= through || record.pass >= playbackPass || (record.status & 0xf0) == 0x90) continue;
            const std::uint8_t bytes[] { record.status, record.first, record.second };
            const int kind = record.status & 0xf0;
            MidiCc control;
            if (MidiCc::fromMessage (bytes, kind == 0xc0 || kind == 0xd0 ? 2 : 3, record.start, control)
                && ! visit (control)) return false;
        }
        return true;
    }

private:
    struct Record
    {
        std::int64_t start = 0, end = 0;
        int pass = 1;
        std::uint8_t status = 0, first = 0, second = 0;
    };
    struct Held
    {
        std::int64_t startSample = 0;
        std::uint8_t velocity = 0;
    };

    static bool takeScan (int& scans) noexcept
    {
        if (scans <= 0) return false;
        --scans;
        return true;
    }

    int consumedPass (MidiCaptureStamp consumed) const noexcept
    {
        if (consumed.gestureId > gestureId) return std::numeric_limits<int>::max();
        return consumed.gestureId == gestureId ? consumed.consumedThroughPass : 0;
    }

    std::int64_t ticksAt (std::int64_t sample) const noexcept
    {
        return std::max<std::int64_t> (0, sampleToTick ? sampleToTick (sample) : sample);
    }

    Record noteRecord (int key, std::int64_t endSample) const noexcept
    {
        const auto& note = held[(std::size_t) key];
        Record record;
        record.pass = currentPass;
        record.start = std::min (ticksAt (note.startSample), lengthTicks - 1);
        record.end = std::clamp (ticksAt (std::min (endSample, length)), record.start + 1, lengthTicks);
        record.status = (std::uint8_t) (0x90 | (key / 128));
        record.first = (std::uint8_t) (key % 128);
        record.second = note.velocity;
        return record;
    }

    void append (const Record& record) noexcept
    {
        if (tail - head == records.size())
        {
            recoverGap();
            return;
        }
        records[(std::size_t) (tail % records.size())] = record;
        ++tail;
    }

    void closeNote (int key, std::int64_t endSample) noexcept
    {
        const auto& note = held[(std::size_t) key];
        if (note.velocity > 0 && note.startSample <= endSample) append (noteRecord (key, endSample));
    }

    void advanceToPass (int pass) noexcept
    {
        if (heldCount == 0)
        {
            currentPass = pass;
            return;
        }
        while (available && currentPass < pass)
        {
            for (int i = 0; i < heldCount && available; ++i)
                closeNote (heldKeys[(std::size_t) i], length);
            for (int i = 0; i < heldCount; ++i)
                held[(std::size_t) heldKeys[(std::size_t) i]].startSample = 0;
            ++currentPass;
        }
    }

    std::vector<Record> records;
    std::array<Held, 16 * 128> held {};
    std::array<int, 16 * 128> heldKeys {};
    SampleToTick sampleToTick;
    std::uint64_t gestureId = 0, head = 0, tail = 0;
    std::int64_t length = 0, lengthTicks = 1;
    int heldCount = 0, currentPass = 1, observedPass = 0;
    bool available = false, overflowed = false;
};
} // namespace duskstudio
