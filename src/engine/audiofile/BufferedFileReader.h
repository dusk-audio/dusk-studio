#pragma once

#include "FileReader.h"
#include "../../foundation/AutoResetEvent.h"

#include <atomic>
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
    bool readRt (float* const* dest, int numDestCh,
                 std::int64_t startFrame, std::int64_t numFrames) noexcept;

    void hintRt (std::int64_t startFrame) noexcept;
    std::int64_t hint() const noexcept;
    bool holds (std::int64_t startFrame, std::int64_t numFrames) const noexcept;

    // Non-RT: prefetch also wakes the owned Background worker.
    void prefetch (std::int64_t startFrame) noexcept;

    // Exactly one filling owner. Manual callers/pools serialize these methods.
    // Each fillOnce reads at most kFillChunkFrames; fillNow primes a full window.
    bool fillOnce();
    void fillNow();
    void releaseResidency() noexcept;

    bool needsFill() const noexcept;
    bool hasResidency() const noexcept;
    std::size_t allocatedBytes() const noexcept;
    std::uint64_t readMisses() const noexcept;

private:
    struct Window
    {
        std::int64_t startFrame = 0;
        std::int64_t numFrames = 0;
        std::vector<float> data;
    };

    const Window* acquireWindow() const noexcept;
    void releaseWindow() const noexcept;
    bool needsFillAt (const Window*, std::int64_t) const noexcept;
    void beginFill (std::int64_t);
    void publishStaging();
    void updateAllocatedBytes() noexcept;
    void run();

    std::unique_ptr<FileReader> source;
    FileInfo fileInfo;
    const int channels;
    const std::int64_t capacity;
    const Fill fillMode;

    std::unique_ptr<Window> currentWindow;
    std::unique_ptr<Window> stagingWindow;
    std::vector<float*> fillDest;
    std::int64_t stagingFilled = 0;

    std::atomic<const Window*> publishedWindow { nullptr };
    mutable std::atomic<unsigned> activeReads { 0 };
    std::atomic<std::int64_t> hintFrame { 0 };
    std::atomic<std::int64_t> failedHint { -1 };
    std::atomic<std::size_t> allocated { 0 };
    std::atomic<std::uint64_t> misses { 0 };

    std::atomic<bool> stop { false };
    AutoResetEvent    wake;
    std::thread       worker;
};
} // namespace dusk::audio
