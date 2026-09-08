#pragma once

#include "FileReader.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

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
    static std::shared_ptr<const WaveformPeaks> generate (FileReader&, const CancelCheck& cancelled = {});

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

// Exact sample extrema for the visible columns of one or more source slices.
class WaveformDetails final
{
public:
    struct Window
    {
        int64_t sourceStart = 0, sourceLength = 0;
        int fullWidth = 0, firstColumn = 0, numColumns = 0;

        // Shared by detail and overview rendering. Invalid geometry returns no
        // range; an off-file column returns an empty clipped range.
        std::optional<std::pair<int64_t, int64_t>> columnRange (int column, int64_t fileFrames) const noexcept;

        bool operator== (const Window& other) const noexcept
        {
            return sourceStart == other.sourceStart && sourceLength == other.sourceLength
                && fullWidth == other.fullWidth && firstColumn == other.firstColumn
                && numColumns == other.numColumns;
        }
    };
    static constexpr std::size_t kMaxColumns = 65536;
    static constexpr std::size_t kMaxBytes = 16u * 1024u * 1024u;

    // Includes pending/worker window copies and result indexing in the budget.
    // Windows retain the full slice mapping, even when only a clipped subset
    // of its columns is visible. Coarser than 512 frames/column uses overview.
    static bool validRequest (const std::vector<Window>&, int channels) noexcept;
    static std::shared_ptr<const WaveformDetails> generate (
        FileReader&, const std::vector<Window>&, const WaveformPeaks::CancelCheck& cancelled = {});

    const std::vector<Window>& windows() const noexcept { return viewWindows; }
    const FileInfo& info() const noexcept { return fileInfo; }
    // Column is relative to firstColumn. Samples outside the file are clipped;
    // a column wholly outside the file has a zero peak.
    std::optional<WaveformPeaks::Peak> column (std::size_t window, int channel, int column) const noexcept;

private:
    WaveformDetails() = default;
    FileInfo fileInfo;
    std::vector<Window> viewWindows;
    std::vector<std::size_t> offsets;
    std::vector<WaveformPeaks::Peak> peaks;
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
        std::optional<FileInfo> info;
        State detailState = State::Empty;
        std::shared_ptr<const WaveformDetails> details;
    };

    WaveformSource();
    ~WaveformSource();
    WaveformSource (const WaveformSource&) = delete;
    WaveformSource& operator= (const WaveformSource&) = delete;

    // Each call invalidates even the same path, so callers request only on load
    // or explicit reload, not on every repaint/refresh. Empty clears it.
    void setFile (const std::filesystem::path&);
    // Set after setFile. True accepts async validation: geometry and known-channel
    // budgets are checked immediately, unknown channels after opening the file.
    // A changed request clears prior detail. Identical requests (including Failed)
    // are deduplicated; clear detail or change file/windows to retry.
    // Visible detail preempts an unfinished overview, which restarts after requests
    // settle. Completed overview survives. Empty clears only detail.
    bool setDetailWindows (const std::vector<WaveformDetails::Window>&);
    Snapshot snapshot() const;

private:
    void run();

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::atomic<uint64_t> generation { 0 };
    uint64_t fileGeneration = 0;
    std::shared_ptr<const std::filesystem::path> pendingFile;
    std::vector<WaveformDetails::Window> requestedWindows;
    Snapshot current;
    bool filePending = false;
    bool pending = false;
    bool detailsPending = false;
    bool stopping = false;
    std::thread worker;
};
} // namespace dusk::audio
