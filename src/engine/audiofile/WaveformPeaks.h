#pragma once

#include "FileReader.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace dusk::audio
{
// Immutable overview data. Generation does disk I/O; queries only read memory.
class WaveformPeaks final
{
public:
    struct Peak { float minimum = 0.0f, maximum = 0.0f; };
    static constexpr int kMaxFramesPerPeak = 512;
    static constexpr std::size_t kMaxPeakBytes = 128u * 1024u * 1024u;
    using CancelCheck = std::function<bool()>;

    // Returns null on cancellation, unreadable/truncated input or memory refusal
    // (including the kMaxPeakBytes ceiling for the complete pyramid).
    // Cancellation is checked between bounded reads and pyramid reductions.
    static std::shared_ptr<const WaveformPeaks> generate (
        const std::filesystem::path&, const CancelCheck& cancelled = {});

    const FileInfo& info() const noexcept { return fileInfo; }
    double durationSeconds() const noexcept { return (double) fileInfo.numFrames / fileInfo.sampleRate; }
    int framesPerPeak() const noexcept { return resolution; }

    // Half-open file-frame range, clipped to the file. Boundary bins are included
    // whole, extending either edge by at most framesPerPeak()-1 frames. Short
    // files use finer power-of-two bins, down to individual samples.
    // Invalid channels and empty ranges return no peak. No allocation or I/O.
    std::optional<Peak> query (int channel, int64_t firstFrame, int64_t endFrame) const noexcept;

private:
    WaveformPeaks() = default;
    FileInfo fileInfo;
    int resolution = 1;
    std::vector<std::vector<Peak>> levels;
};

// One replaceable source for a view. Calls and destruction are non-RT. The worker
// never invokes UI callbacks; a view polls snapshot() using its existing timer.
class WaveformSource final
{
public:
    enum class State { Empty, Loading, Ready, Failed };
    struct Snapshot
    {
        State state = State::Empty;
        std::shared_ptr<const WaveformPeaks> peaks;
    };

    WaveformSource();
    ~WaveformSource();
    WaveformSource (const WaveformSource&) = delete;
    WaveformSource& operator= (const WaveformSource&) = delete;

    // Each call invalidates even the same path, so callers request only on load
    // or explicit reload, not on every repaint/refresh. Empty clears it.
    void setFile (const std::filesystem::path&);
    Snapshot snapshot() const;

private:
    void run();

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::atomic<uint64_t> generation { 0 };
    std::filesystem::path pendingFile;
    Snapshot current;
    bool pending = false;
    bool stopping = false;
    std::thread worker;
};
} // namespace dusk::audio
