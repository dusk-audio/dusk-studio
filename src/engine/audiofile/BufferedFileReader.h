#pragma once

#include "FileReader.h"
#include "../../foundation/AutoResetEvent.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace dusk::audio
{
class BufferedFileReader
{
public:
    enum class Fill { Background, Manual };

    struct ReadCoverage
    {
        std::int64_t startFrame = 0;
        std::int64_t endFrame = 0;
    };

    static constexpr std::int64_t kDefaultWindowFrames = 96000;
    static constexpr std::int64_t kFillChunkFrames = 8192;

    explicit BufferedFileReader (std::unique_ptr<FileReader> source,
                                 std::int64_t windowFrames = kDefaultWindowFrames,
                                 Fill fill = Fill::Background);
    ~BufferedFileReader();

    BufferedFileReader (const BufferedFileReader&)            = delete;
    BufferedFileReader& operator= (const BufferedFileReader&) = delete;

    const FileInfo& info() const noexcept { return fileInfo; }
    bool isManual() const noexcept { return fillMode == Fill::Manual; }

    // RT: immutable snapshots, silence for missing frames, no disk or wakeup.
    // Coverage describes the file frames copied from that same snapshot.
    bool readRt (float* const* dest, int numDestCh,
                 std::int64_t startFrame, std::int64_t numFrames,
                 ReadCoverage* coverage = nullptr) noexcept;

    void hintRt (std::int64_t startFrame) noexcept;
    std::int64_t hint() const noexcept;
    bool holds (std::int64_t startFrame, std::int64_t numFrames) const noexcept;
    bool holdsSeek (std::int64_t startFrame, std::int64_t numFrames) const noexcept;
    bool needsSeekFill (std::int64_t startFrame, std::int64_t numFrames) const noexcept;

    // Non-RT: prefetch also wakes the owned Background worker.
    void prefetch (std::int64_t startFrame) noexcept;

    // Non-RT, serialized with all external filling and prefetch calls. Switching
    // to Manual joins the owned worker before returning its filling ownership.
    void setFillMode (Fill);

    // Exactly one filling owner. Manual callers/pools serialize these methods.
    // Each fillOnce reads at most kFillChunkFrames; fillNow primes a full window.
    // A window for a frame no published window holds is published with its first
    // chunk and grows in place, so a cold reader plays as soon as it has read.
    bool fillOnce (std::int64_t startFrame = -1);
    // Seek filling preserves current playback; numFrames is the required warm span.
    // The seek window is published with its first chunk and holds the span once
    // enough of it has arrived; playback that commits there keeps it growing.
    bool fillSeekOnce (std::int64_t target, std::int64_t currentFrame, std::int64_t numFrames);
    void fillNow();
    void releaseResidency() noexcept;

    bool needsFill() const noexcept;
    bool hasResidency() const noexcept;
    // Whether the last read from the source came up short or threw, until a
    // later read succeeds: the file has stopped reading, at least for now.
    bool fillFailing() const noexcept;
    std::size_t allocatedBytes() const noexcept;
    std::uint64_t readMisses() const noexcept;

private:
    struct Window
    {
        std::int64_t startFrame = 0;
        std::int64_t stride = 0;
        // Filling owner only: the frames this window grows to.
        std::int64_t planned = 0;
        // Published payload. Only the filling owner raises it, after writing the
        // frames below it, so a reader that loads it may read every frame it covers.
        std::atomic<std::int64_t> filled { 0 };
        std::vector<float> data;
    };

    const Window* acquireWindow() const noexcept;
    void releaseWindow() const noexcept;
    bool needsFillAt (const Window*, std::int64_t) const noexcept;
    static std::int64_t filledFrames (const Window*) noexcept;
    static bool windowHolds (const Window*, std::int64_t, std::int64_t) noexcept;
    static bool grows (const Window*, std::int64_t from, std::int64_t end) noexcept;
    const Window* windowForRange (const Window*, std::int64_t, std::int64_t) const noexcept;
    void retainSeekForPlayback (std::int64_t);
    bool fillAt (std::int64_t startFrame);
    bool fillSeekAt (std::int64_t target, std::int64_t currentFrame, std::int64_t numFrames);
    std::unique_ptr<Window> makeWindow (std::int64_t);
    bool readChunk (Window&);
    void publish (std::unique_ptr<Window>& slot, std::atomic<const Window*>& published,
                  std::unique_ptr<Window> next);
    void updateAllocatedBytes() noexcept;
    void run();

    std::unique_ptr<FileReader> source;
    FileInfo fileInfo;
    const int channels;
    const std::int64_t capacity;
    Fill fillMode;

    // A filling window lives in the slot it serves; staging holds a refill of a
    // current window that still plays, and stays unpublished until it is full.
    std::unique_ptr<Window> currentWindow;
    std::unique_ptr<Window> seekWindow;
    std::unique_ptr<Window> stagingWindow;
    std::vector<float*> fillDest;
    std::chrono::steady_clock::time_point nextFillAttempt {};

    std::atomic<const Window*> publishedWindow { nullptr };
    std::atomic<const Window*> publishedSeekWindow { nullptr };
    mutable std::atomic<unsigned> activeReads { 0 };
    std::atomic<std::int64_t> hintFrame { 0 };
    std::atomic<std::size_t> allocated { 0 };
    std::atomic<std::uint64_t> misses { 0 };
    std::atomic<bool> failing { false };

    std::atomic<bool> stop { false };
    AutoResetEvent    wake;
    std::thread       worker;
};
} // namespace dusk::audio
