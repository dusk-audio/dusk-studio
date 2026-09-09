#include "WaveformPeaks.h"
#include "../../foundation/PlanarBuffer.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <stdexcept>

namespace dusk::audio
{
namespace
{
WaveformPeaks::Peak merge (WaveformPeaks::Peak a, WaveformPeaks::Peak b) noexcept
{
    return { std::min (a.minimum, b.minimum), std::max (a.maximum, b.maximum) };
}

float finiteSample (float value) noexcept { return std::isfinite (value) ? value : 0.0f; }

bool validInfo (const FileInfo& info) noexcept
{
    return std::isfinite (info.sampleRate) && info.sampleRate > 0.0
        && info.numChannels > 0 && info.numChannels <= 256 && info.numFrames >= 0;
}

bool validWindow (const WaveformDetails::Window& window) noexcept
{
    return window.sourceStart >= 0 && window.sourceLength > 0 && window.fullWidth > 0
        && window.firstColumn >= 0 && window.firstColumn < window.fullWidth
        && window.numColumns > 0 && window.numColumns <= window.fullWidth - window.firstColumn;
}

int64_t columnBoundary (const WaveformDetails::Window& window, int column,
                        bool roundUp, int64_t fileFrames) noexcept
{
    if (window.sourceStart >= fileFrames) return fileFrames;
    const auto remainder = (window.sourceLength % window.fullWidth) * column;
    const auto delta = (window.sourceLength / window.fullWidth) * column
        + remainder / window.fullWidth + (roundUp && remainder % window.fullWidth != 0);
    return window.sourceStart + std::min (delta, fileFrames - window.sourceStart);
}
}

std::shared_ptr<const WaveformPeaks> WaveformPeaks::generate (
    const std::filesystem::path& path, const CancelCheck& cancelled)
{
    if (cancelled && cancelled()) return {};
    auto reader = FileReader::open (path);
    return reader ? generate (*reader, cancelled) : nullptr;
}

std::shared_ptr<const WaveformPeaks> WaveformPeaks::generate (
    FileReader& reader, const CancelCheck& cancelled)
{
    const auto shouldCancel = [&] { return cancelled && cancelled(); };
    const auto info = reader.info();
    if (! validInfo (info)) return {};

    int framesPerBin = 1;
    constexpr int64_t kDetailedPeakCount = 65536;
    while (framesPerBin < kMaxFramesPerPeak && info.numFrames > kDetailedPeakCount * framesPerBin)
        framesPerBin *= 2;
    const auto bins = info.numFrames / framesPerBin + (info.numFrames % framesPerBin != 0);
    const auto channels = (std::size_t) info.numChannels;
    // Account for every level, including the extra parent of odd-sized levels.
    std::size_t entries = 0;
    for (auto n = bins; n > 0; n = n == 1 ? 0 : n / 2 + n % 2)
    {
        const auto remaining = kMaxPeakBytes / sizeof (Peak) - entries;
        if ((uint64_t) n > remaining / channels) return {};
        entries += (std::size_t) n * channels;
    }
    if (shouldCancel()) return {};

    try
    {
        auto result = std::shared_ptr<WaveformPeaks> (new WaveformPeaks);
        result->fileInfo = info;
        result->resolution = framesPerBin;
        if (bins == 0) return result;
        result->levels.emplace_back ((std::size_t) bins * channels);

        constexpr int kReadFrames = 8192;
        PlanarBuffer scratch;
        if (! scratch.setSize (info.numChannels, kReadFrames)) return {};
        for (int64_t first = 0; first < info.numFrames;)
        {
            if (shouldCancel()) return {};
            const auto count = std::min<int64_t> (kReadFrames, info.numFrames - first);
            if (reader.read (scratch.data(), info.numChannels, first, count) != count)
                return {};
            for (int offset = 0; offset < count; offset += framesPerBin)
            {
                const auto index = (std::size_t) ((first + offset) / framesPerBin) * channels;
                const auto end = std::min<int64_t> (count, offset + framesPerBin);
                for (int channel = 0; channel < info.numChannels; ++channel)
                {
                    const auto* samples = scratch.channel (channel);
                    const float initial = finiteSample (samples[offset]);
                    Peak peak { initial, initial };
                    for (auto frame = offset + 1; frame < end; ++frame)
                    {
                        const float value = finiteSample (samples[frame]);
                        peak = merge (peak, { value, value });
                    }
                    result->levels[0][index + (std::size_t) channel] = peak;
                }
            }
            first += count;
        }

        for (auto n = (std::size_t) bins; n > 1; n = n / 2 + n % 2)
        {
            if (shouldCancel()) return {};
            const auto parentBins = n / 2 + n % 2;
            std::vector<Peak> parent (parentBins * channels);
            const auto& child = result->levels.back();
            for (std::size_t bin = 0; bin < parentBins; ++bin)
            {
                if (bin % 4096 == 0 && shouldCancel()) return {};
                for (std::size_t ch = 0; ch < channels; ++ch)
                {
                    const auto a = child[bin * 2 * channels + ch];
                    parent[bin * channels + ch] = bin * 2 + 1 < n
                        ? merge (a, child[(bin * 2 + 1) * channels + ch]) : a;
                }
            }
            result->levels.push_back (std::move (parent));
        }
        if (shouldCancel()) return {};
        return result;
    }
    catch (const std::bad_alloc&) { return {}; }
    catch (const std::length_error&) { return {}; }
}

std::optional<WaveformPeaks::Peak> WaveformPeaks::query (
    int channel, int64_t firstFrame, int64_t endFrame) const noexcept
{
    firstFrame = std::max<int64_t> (0, firstFrame);
    endFrame = std::min (fileInfo.numFrames, endFrame);
    if (channel < 0 || channel >= fileInfo.numChannels || firstFrame >= endFrame)
        return {};

    auto first = (std::size_t) (firstFrame / resolution);
    auto end = (std::size_t) ((endFrame - 1) / resolution + 1);
    std::optional<Peak> peak;
    std::size_t level = 0;
    const auto add = [&] (std::size_t bin)
    {
        const auto value = levels[level][bin * (std::size_t) fileInfo.numChannels + (std::size_t) channel];
        peak = peak ? merge (*peak, value) : value;
    };
    while (first < end)
    {
        if (first % 2 != 0) add (first++);
        if (end % 2 != 0) add (--end);
        first /= 2;
        end /= 2;
        ++level;
    }
    return peak;
}

std::optional<std::pair<int64_t, int64_t>> WaveformDetails::Window::columnRange (
    int column, int64_t fileFrames) const noexcept
{
    if (! validWindow (*this) || column < 0 || column >= numColumns || fileFrames < 0) return {};
    return std::pair<int64_t, int64_t> { columnBoundary (*this, firstColumn + column, false, fileFrames),
                                       columnBoundary (*this, firstColumn + column + 1, true, fileFrames) };
}

bool WaveformDetails::validRequest (const std::vector<Window>& windows, int channels) noexcept
{
    if (channels <= 0 || channels > 256 || windows.size() > kMaxColumns) return false;
    std::size_t columns = 0;
    for (const auto& window : windows)
    {
        if (! validWindow (window)
            || window.sourceLength > (int64_t) window.fullWidth * WaveformPeaks::kMaxFramesPerPeak
            || (std::size_t) window.numColumns > kMaxColumns - columns)
            return false;
        columns += (std::size_t) window.numColumns;
    }
    const auto bookkeeping = sizeof (WaveformDetails) + 2 * sizeof (std::vector<Window>)
        + windows.size() * (3 * sizeof (Window) + sizeof (std::size_t));
    return bookkeeping <= kMaxBytes
        && columns <= (kMaxBytes - bookkeeping) / sizeof (WaveformPeaks::Peak) / (std::size_t) channels;
}

std::shared_ptr<const WaveformDetails> WaveformDetails::generate (
    FileReader& reader, const std::vector<Window>& windows, const WaveformPeaks::CancelCheck& cancelled)
{
    const auto shouldCancel = [&] { return cancelled && cancelled(); };
    const auto info = reader.info();
    if (shouldCancel() || ! validInfo (info) || ! validRequest (windows, info.numChannels)) return {};
    try
    {
        auto result = std::shared_ptr<WaveformDetails> (new WaveformDetails);
        result->fileInfo = info;
        result->viewWindows = windows;
        result->offsets.reserve (windows.size());
        std::size_t entries = 0;
        for (const auto& window : windows)
        {
            result->offsets.push_back (entries);
            entries += (std::size_t) window.numColumns * (std::size_t) info.numChannels;
        }
        result->peaks.resize (entries);
        constexpr int kReadFrames = 8192;
        PlanarBuffer scratch;
        if (! scratch.setSize (info.numChannels, kReadFrames)) return {};
        for (std::size_t index = 0; index < windows.size(); ++index)
        {
            if (shouldCancel()) return {};
            const auto& window = windows[index];
            const auto windowEnd = window.columnRange (window.numColumns - 1, info.numFrames)->second;
            int64_t bufferFirst = -1;
            int bufferCount = 0;
            for (int column = 0; column < window.numColumns; ++column)
            {
                if (column % 256 == 0 && shouldCancel()) return {};
                const auto range = *window.columnRange (column, info.numFrames);
                auto position = range.first;
                const auto end = range.second;
                auto* destination = result->peaks.data() + result->offsets[index]
                    + (std::size_t) column * (std::size_t) info.numChannels;
                bool initialized = false;
                while (position < end)
                {
                    if (position < bufferFirst || position - bufferFirst >= bufferCount)
                    {
                        if (shouldCancel()) return {};
                        bufferFirst = position;
                        bufferCount = (int) std::min<int64_t> (kReadFrames, windowEnd - position);
                        if (reader.read (scratch.data(), info.numChannels, position, bufferCount) != bufferCount)
                            return {};
                    }
                    const int offset = (int) (position - bufferFirst);
                    const int count = (int) std::min<int64_t> (end - position, bufferCount - offset);
                    for (int channel = 0; channel < info.numChannels; ++channel)
                    {
                        const auto* samples = scratch.channel (channel) + offset;
                        if (! initialized)
                        {
                            const auto value = finiteSample (samples[0]);
                            destination[channel] = { value, value };
                        }
                        for (int frame = 0; frame < count; ++frame)
                        {
                            const auto value = finiteSample (samples[frame]);
                            destination[channel] = merge (destination[channel], { value, value });
                        }
                    }
                    initialized = true;
                    position += count;
                }
            }
        }
        if (shouldCancel()) return {};
        return result;
    }
    catch (const std::bad_alloc&) { return {}; }
    catch (const std::length_error&) { return {}; }
}

std::optional<WaveformPeaks::Peak> WaveformDetails::column (
    std::size_t window, int channel, int columnIndex) const noexcept
{
    if (window >= viewWindows.size() || channel < 0 || channel >= fileInfo.numChannels
        || columnIndex < 0 || columnIndex >= viewWindows[window].numColumns)
        return {};
    return peaks[offsets[window] + (std::size_t) columnIndex * (std::size_t) fileInfo.numChannels
                 + (std::size_t) channel];
}

WaveformSource::WaveformSource() : worker ([this] { run(); }) {}

WaveformSource::~WaveformSource()
{
    {
        std::lock_guard<std::mutex> lock (mutex);
        stopping = true;
        generation.fetch_add (1, std::memory_order_release);
        wake.notify_one();
    }
    worker.join();
}

void WaveformSource::setFile (const std::filesystem::path& file)
{
    auto path = file.empty() ? std::shared_ptr<const std::filesystem::path>()
                            : std::make_shared<const std::filesystem::path> (file);
    std::lock_guard<std::mutex> lock (mutex);
    pendingFile = std::move (path);
    ++fileGeneration;
    generation.fetch_add (1, std::memory_order_release);
    filePending = true;
    pending = ! file.empty();
    detailsPending = false;
    requestedWindows.clear();
    current = {};
    current.state = pending ? State::Loading : State::Empty;
    wake.notify_one();
}

bool WaveformSource::setDetailWindows (const std::vector<WaveformDetails::Window>& windows)
{
    std::lock_guard<std::mutex> lock (mutex);
    if (windows == requestedWindows && (! windows.empty() || current.detailState == State::Empty))
        return current.detailState != State::Failed;
    generation.fetch_add (1, std::memory_order_release);
    current.details.reset();
    current.detailState = windows.empty() ? State::Empty : State::Failed;
    detailsPending = false;
    requestedWindows.clear();
    bool valid = windows.empty() || (current.state != State::Empty
        && WaveformDetails::validRequest (windows, current.info ? current.info->numChannels : 1));
    if (valid && ! windows.empty())
    {
        try
        {
            auto replacement = windows;
            requestedWindows.swap (replacement);
            detailsPending = true;
            current.detailState = State::Loading;
        }
        catch (const std::bad_alloc&) { valid = false; }
        catch (const std::length_error&) { valid = false; }
    }
    wake.notify_one();
    return valid;
}

WaveformSource::Snapshot WaveformSource::snapshot() const
{
    std::lock_guard<std::mutex> lock (mutex);
    return current;
}

void WaveformSource::run()
{
    std::unique_lock<std::mutex> lock (mutex);
    std::unique_ptr<FileReader> reader;
    while (true)
    {
        wake.wait (lock, [this] { return stopping || filePending || pending || detailsPending; });
        if (stopping) return;
        if (filePending)
        {
            const auto file = pendingFile;
            const auto fileRequest = fileGeneration;
            filePending = false;
            lock.unlock();
            reader.reset();
            try
            {
                if (file) reader = FileReader::open (*file);
                if (reader && ! validInfo (reader->info())) reader.reset();
            }
            catch (const std::exception&) { reader.reset(); }
            lock.lock();
            if (fileRequest != fileGeneration) continue;
            if (reader) current.info = reader->info();
            else
            {
                pending = false;
                detailsPending = false;
                current.state = file ? State::Failed : State::Empty;
                current.detailState = requestedWindows.empty() ? State::Empty : State::Failed;
            }
        }
        if (! reader)
        {
            detailsPending = false;
            if (! requestedWindows.empty()) current.detailState = State::Failed;
            continue;
        }
        const bool detail = detailsPending;
        std::vector<WaveformDetails::Window> windows;
        if (detail)
        {
            detailsPending = false;
            try { windows = requestedWindows; }
            catch (const std::exception&) { current.detailState = State::Failed; continue; }
        }
        const auto fileRequest = fileGeneration;
        const auto request = generation.load (std::memory_order_acquire);
        lock.unlock();
        std::shared_ptr<const WaveformPeaks> peaks;
        std::shared_ptr<const WaveformDetails> details;
        try
        {
            const auto cancelled = [this, request]
            {
                return generation.load (std::memory_order_acquire) != request;
            };
            if (detail) details = WaveformDetails::generate (*reader, windows, cancelled);
            else peaks = WaveformPeaks::generate (*reader, cancelled);
        }
        catch (const std::exception&) {}
        lock.lock();
        if (fileRequest != fileGeneration || generation.load (std::memory_order_acquire) != request)
            continue;
        if (detail)
        {
            current.detailState = details ? State::Ready : State::Failed;
            current.details = std::move (details);
        }
        else
        {
            pending = false;
            current.state = peaks ? State::Ready : State::Failed;
            current.peaks = std::move (peaks);
        }
    }
}
} // namespace dusk::audio
