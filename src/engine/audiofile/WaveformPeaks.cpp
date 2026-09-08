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
}

std::shared_ptr<const WaveformPeaks> WaveformPeaks::generate (
    const std::filesystem::path& path, const CancelCheck& cancelled)
{
    const auto shouldCancel = [&] { return cancelled && cancelled(); };
    if (shouldCancel()) return {};

    auto reader = FileReader::open (path);
    if (reader == nullptr) return {};
    const auto info = reader->info();
    if (! std::isfinite (info.sampleRate) || info.sampleRate <= 0.0
        || info.numChannels <= 0 || info.numChannels > 256 || info.numFrames < 0)
        return {};

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
            if (reader->read (scratch.data(), info.numChannels, first, count) != count)
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
    std::lock_guard<std::mutex> lock (mutex);
    pendingFile = file;
    generation.fetch_add (1, std::memory_order_release);
    pending = ! file.empty();
    current = { pending ? State::Loading : State::Empty, {} };
    wake.notify_one();
}

WaveformSource::Snapshot WaveformSource::snapshot() const
{
    std::lock_guard<std::mutex> lock (mutex);
    return current;
}

void WaveformSource::run()
{
    std::unique_lock<std::mutex> lock (mutex);
    while (true)
    {
        wake.wait (lock, [this] { return stopping || pending; });
        if (stopping) return;
        const auto file = std::move (pendingFile);
        const auto request = generation.load (std::memory_order_acquire);
        pending = false;
        lock.unlock();
        std::shared_ptr<const WaveformPeaks> peaks;
        try
        {
            peaks = WaveformPeaks::generate (file, [this, request]
            {
                return generation.load (std::memory_order_acquire) != request;
            });
        }
        catch (const std::exception&) {}
        lock.lock();
        if (generation.load (std::memory_order_acquire) == request)
            current = { peaks ? State::Ready : State::Failed, std::move (peaks) };
    }
}
} // namespace dusk::audio
