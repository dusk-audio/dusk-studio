#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace duskstudio
{
// What a take's capture kept of its input, in input time: samples of the take's
// gesture counted from its first block. A track recorded from its input is
// written on exactly these samples; a track recorded from its strip's output
// (PRINT) receives each input sample that output's delay later, so it reads the
// schedule that much behind and keeps the same input samples, in the same loop
// passes and timeline positions, as the input capture did.
//
// Audio thread only. Fixed capacity: add, forgetBefore and forEachIn never
// allocate or lock.
class CaptureSchedule
{
public:
    struct Span
    {
        std::int64_t inputStart = 0;
        int length = 0;
        // The loop pass, 1-based; 0 in a linear take.
        int passOrdinal = 0;
        std::int64_t timelineStart = 0;
        bool startsPass = false;
        bool endsPass = false;

        std::int64_t inputEnd() const noexcept { return inputStart + length; }
    };

    // The longest output delay the schedule serves. A loop pass lasts at least
    // the shortest loop the recorder accepts (128 samples), so a horizon's worth
    // of passes fits in the capacity with room for a seek in each.
    static constexpr std::int64_t kHorizonSamples = 40960;
    static constexpr int kCapacity = 1024;

    void reset() noexcept
    {
        oldest = 0;
        count = 0;
    }

    // Spans arrive in input order. One that carries straight on from the newest
    // (the next input sample, the next timeline sample, the same pass) extends
    // it. A full schedule drops its oldest span.
    void add (const Span& span) noexcept
    {
        if (span.length <= 0) return;
        if (count > 0)
        {
            auto& last = at (count - 1);
            if (span.inputStart == last.inputEnd()
                && span.passOrdinal == last.passOrdinal
                && span.timelineStart == last.timelineStart + last.length
                && ! last.endsPass && ! span.startsPass)
            {
                last.length += span.length;
                last.endsPass = span.endsPass;
                return;
            }
        }
        if (count == kCapacity)
        {
            oldest = (oldest + 1) % kCapacity;
            --count;
        }
        at (count) = span;
        ++count;
    }

    // Drops the spans that end at or before inputTime.
    void forgetBefore (std::int64_t inputTime) noexcept
    {
        while (count > 0 && at (0).inputEnd() <= inputTime)
        {
            oldest = (oldest + 1) % kCapacity;
            --count;
        }
    }

    // Calls fn (const Span&) with each stored span's part inside [from, to), in
    // input order. A part keeps startsPass or endsPass only when it keeps that
    // end of its span.
    template <typename Fn>
    void forEachIn (std::int64_t from, std::int64_t to, Fn&& fn) const
    {
        for (int i = 0; i < count && from < to; ++i)
        {
            const auto& span = at (i);
            if (span.inputEnd() <= from) continue;
            if (span.inputStart >= to) break;
            const auto start = std::max (from, span.inputStart);
            const auto end = std::min (to, span.inputEnd());
            Span part = span;
            part.inputStart = start;
            part.length = (int) (end - start);
            part.timelineStart = span.timelineStart + (start - span.inputStart);
            part.startsPass = span.startsPass && start == span.inputStart;
            part.endsPass = span.endsPass && end == span.inputEnd();
            fn (part);
        }
    }

    int size() const noexcept { return count; }

private:
    Span& at (int index) noexcept { return spans[(std::size_t) ((oldest + index) % kCapacity)]; }
    const Span& at (int index) const noexcept { return spans[(std::size_t) ((oldest + index) % kCapacity)]; }

    std::array<Span, kCapacity> spans {};
    int oldest = 0;
    int count = 0;
};
} // namespace duskstudio
