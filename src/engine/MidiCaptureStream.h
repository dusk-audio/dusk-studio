#pragma once

#include "../session/MidiEvents.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace duskstudio
{
struct MidiCapturedPass
{
    int passOrdinal = 1;
    std::int64_t lengthInSamples = 0;
    std::int64_t lengthInTicks = 0;
    std::vector<MidiNote> notes;
    std::vector<MidiCc> ccs;
};

struct MidiCapturePreview : MidiCapturedPass
{
    std::uint64_t generation = 0;
};

// One audio producer and one message-thread consumer. configure and sealFinal
// require the producer to be stopped; all decoding and vector growth is consumer-only.
class MidiCaptureStream
{
public:
    struct RawEvent
    {
        std::int64_t samplePos = 0;
        int passOrdinal = 1;
        std::uint8_t status = 0;
        std::uint8_t data1 = 0;
        std::uint8_t data2 = 0;
        std::uint64_t sequence = 0;
    };

    using SampleToTick = std::function<std::int64_t (std::int64_t)>;
    static constexpr std::size_t kDefaultCapacity = 65536;

    explicit MidiCaptureStream (std::size_t capacity = kDefaultCapacity);
    void configure (SampleToTick sampleToTick,
                    std::int64_t loopCaptureLengthSamples = 0);

    bool pushEvent (int passOrdinal, std::int64_t samplePos,
                    std::uint8_t status, std::uint8_t data1,
                    std::uint8_t data2) noexcept;
    bool publishProgress (int passOrdinal, std::int64_t endSample,
                          bool complete) noexcept;

    bool service();
    void sealFinal (int passOrdinal, std::int64_t endSample);
    std::vector<MidiCapturedPass> takeCompleted();
    const MidiCapturePreview& preview() const noexcept { return previewSnapshot; }
    std::uint64_t overflowCount() const noexcept
    {
        return droppedWrites.load (std::memory_order_relaxed);
    }

private:
    struct Record
    {
        RawEvent event;
        bool progress = false;
        bool complete = false;
    };
    struct HeldNote
    {
        bool active = false;
        int velocity = 0;
        std::int64_t startSample = 0;
    };
    struct NoteBounds
    {
        std::int64_t start = 0, end = 0;
    };

    bool push (Record record) noexcept;
    void consume (const Record& record);
    void advanceToPass (int passOrdinal);
    void finishPass (std::int64_t endSample, bool carryState);
    void recoverGap();
    void appendHeldNote (int key, std::int64_t endSample,
                         std::vector<MidiNote>& destination,
                         bool closeAtOnset = false,
                         std::vector<NoteBounds>* bounds = nullptr) const;
    std::int64_t ticksAt (std::int64_t sample) const;
    void rebuildPreview();

    std::vector<Record> records;
    std::atomic<std::uint64_t> readPosition { 0 };
    std::atomic<std::uint64_t> writePosition { 0 };
    std::atomic<std::uint64_t> attemptedSequence { 0 };
    std::atomic<std::uint64_t> droppedWrites { 0 };
    std::atomic<int> completedOrdinal { 0 };
    std::uint64_t nextSequence = 1;

    SampleToTick sampleToTick;
    std::int64_t loopLength = 0;
    MidiCapturedPass current;
    std::vector<NoteBounds> noteBounds;
    std::vector<std::int64_t> controlSamples;
    std::array<HeldNote, 16 * 128> heldNotes {};
    std::vector<MidiCapturedPass> completed;
    MidiCapturePreview previewSnapshot;
    std::uint64_t expectedSequence = 1;
    std::uint64_t generation = 0;
    int lastCompletedOrdinal = 0;
    bool sealed = false;
};
} // namespace duskstudio
