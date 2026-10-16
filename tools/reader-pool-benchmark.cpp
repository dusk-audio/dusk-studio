#include "engine/audiofile/BufferedFileReader.h"
#include "engine/audiofile/FileReader.h"
#ifndef DUSK_READER_BENCH_LEGACY
#include "engine/audiofile/ReaderFillPool.h"
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#ifdef __linux__
#include <time.h>
#endif

namespace
{
using Clock = std::chrono::steady_clock;
using Reader = dusk::audio::BufferedFileReader;
constexpr int kTracks = 24;
constexpr int kChannels = 2;
constexpr int kBlockFrames = 256;
constexpr std::int64_t kRate = 48000;
constexpr std::int64_t kClipFrames = kRate * 8;
constexpr std::int64_t kWindowFrames = 96000;
constexpr std::int64_t kLoopStart = kRate * 2;
constexpr std::int64_t kLoopEnd = kRate * 4;
constexpr std::int64_t kLoopCacheFrames = 32768;

struct Options
{
    std::string mode = "plain";
    std::size_t regions = 360;
    std::size_t blocks = 2000;
    std::filesystem::path output;
};

std::size_t countArgument (const char* text)
{
    const std::string value (text);
    std::size_t count = 0;
    const auto parsed = std::from_chars (value.data(), value.data() + value.size(), count);
    if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size() || count == 0)
        throw std::invalid_argument ("counts must be positive integers");
    return count;
}

Options options (int argc, char** argv)
{
    Options result;
    for (int i = 1; i < argc; ++i)
    {
        const std::string option (argv[i]);
        if (option == "--help")
        {
            std::cout << "reader-pool-benchmark --mode plain|locate|loop "
                         "[--regions 360] [--blocks 2000] [--output report.json]\n"
                         "Regions must be a multiple of 24. Runs use 48 kHz/256-frame pacing.\n";
            std::exit (0);
        }
        if (++i == argc) throw std::invalid_argument ("option has no value: " + option);
        if (option == "--mode") result.mode = argv[i];
        else if (option == "--regions") result.regions = countArgument (argv[i]);
        else if (option == "--blocks") result.blocks = countArgument (argv[i]);
        else if (option == "--output") result.output = argv[i];
        else throw std::invalid_argument ("unknown option: " + option);
    }
    if (result.mode != "plain" && result.mode != "locate" && result.mode != "loop")
        throw std::invalid_argument ("mode must be plain, locate or loop");
    if (result.regions < kTracks || result.regions % kTracks != 0
        || result.regions / kTracks > (std::size_t) (std::numeric_limits<std::int64_t>::max() / kClipFrames))
        throw std::invalid_argument ("regions must be a multiple of 24 within timeline limits");
    return result;
}

std::int16_t sourcePcm (int channel, std::int64_t frame) noexcept
{
    const auto code = (frame * (channel == 0 ? 73 : 151) + (channel == 0 ? 101 : 701)) % 60001;
    return (std::int16_t) (code - 30000);
}

float sourceSample (int channel, std::int64_t frame) noexcept
{
    return (float) sourcePcm (channel, frame) / 32768.0f;
}

class SampleWave
{
public:
    SampleWave()
    {
        const auto base = std::filesystem::temp_directory_path();
        std::random_device random;
        for (int attempt = 0; attempt < 16; ++attempt)
        {
            auto candidate = base / ("dusk-reader-bench-" + std::to_string (random())
                                    + "-" + std::to_string (random()));
            std::error_code error;
            if (std::filesystem::create_directory (candidate, error))
            {
                directory = std::move (candidate);
                break;
            }
        }
        if (directory.empty()) throw std::runtime_error ("could not create benchmark sample directory");
        try { write(); }
        catch (...)
        {
            std::error_code ignored;
            std::filesystem::remove_all (directory, ignored);
            throw;
        }
    }

    ~SampleWave()
    {
        std::error_code ignored;
        std::filesystem::remove_all (directory, ignored);
    }

    std::filesystem::path path() const { return directory / "shared-stereo.wav"; }

private:
    void write()
    {
        std::ofstream stream (path(), std::ios::binary);
        const auto word = [&] (std::uint16_t value)
        {
            stream.put ((char) (value & 255));
            stream.put ((char) (value >> 8));
        };
        const auto dword = [&] (std::uint32_t value)
        {
            word ((std::uint16_t) value);
            word ((std::uint16_t) (value >> 16));
        };
        const auto bytes = (std::uint32_t) (kClipFrames * kChannels * sizeof (std::int16_t));
        stream.write ("RIFF", 4); dword (bytes + 36); stream.write ("WAVEfmt ", 8);
        dword (16); word (1); word (kChannels); dword ((std::uint32_t) kRate);
        dword ((std::uint32_t) (kRate * kChannels * sizeof (std::int16_t)));
        word (kChannels * sizeof (std::int16_t)); word (16);
        stream.write ("data", 4); dword (bytes);
        std::array<char, 8192 * kChannels * sizeof (std::int16_t)> block {};
        for (std::int64_t from = 0; from < kClipFrames; from += 8192)
        {
            const auto frames = std::min<std::int64_t> (8192, kClipFrames - from);
            for (std::int64_t f = 0; f < frames; ++f)
                for (int c = 0; c < kChannels; ++c)
                {
                    const auto value = (std::uint16_t) sourcePcm (c, from + f);
                    const auto offset = (std::size_t) (f * kChannels + c) * sizeof (std::int16_t);
                    block[offset] = (char) (value & 255);
                    block[offset + 1] = (char) (value >> 8);
                }
            stream.write (block.data(), (std::streamsize) (frames * kChannels * sizeof (std::int16_t)));
        }
        stream.flush();
        if (! stream) throw std::runtime_error ("could not write benchmark sample WAV");
    }

    std::filesystem::path directory;
};

struct ReaderDeleter
{
#ifndef DUSK_READER_BENCH_LEGACY
    dusk::audio::ReaderFillPool* pool = nullptr;
#endif
    void operator() (Reader* reader) const noexcept
    {
        if (reader == nullptr) return;
#ifndef DUSK_READER_BENCH_LEGACY
        if (pool != nullptr) pool->remove (*reader);
#endif
        delete reader;
    }
};

struct Region
{
    std::int64_t start = 0;
    std::unique_ptr<Reader, ReaderDeleter> reader;
    std::array<std::vector<float>, kChannels> loopCache;
};

struct Segment
{
    std::int64_t timelineFrame = 0;
    int outputOffset = 0;
    int frames = 0;
};

class Streams
{
public:
    explicit Streams (const Options& settings) : settings (settings)
    {
#ifndef DUSK_READER_BENCH_LEGACY
        pool.configure ((double) kRate);
        pool.setLoopRt (settings.mode == "loop", kLoopStart, kLoopEnd);
#endif
        regions.reserve (settings.regions);
        for (std::size_t i = 0; i < settings.regions; ++i)
        {
            auto raw = dusk::audio::FileReader::open (wave.path());
            if (raw == nullptr) throw std::runtime_error ("could not open shared sample WAV");
            Region region;
            region.start = (std::int64_t) (i / kTracks) * kClipFrames;
#ifdef DUSK_READER_BENCH_LEGACY
            region.reader.reset (new Reader (std::move (raw), kWindowFrames, Reader::Fill::Background));
#else
            region.reader = decltype(region.reader) (new Reader (std::move (raw), kWindowFrames,
                                                                   Reader::Fill::Manual),
                                                     ReaderDeleter { &pool });
            pool.add (*region.reader, region.start, region.start + kClipFrames, 0);
#endif
            regions.push_back (std::move (region));
        }
        const auto initial = settings.mode == "loop" ? kLoopStart : 0;
        if (settings.mode == "loop") prepareLoopCaches();
        warmInitial (initial);
        std::this_thread::sleep_for (std::chrono::milliseconds (150));
    }

    std::int64_t sessionFrames() const noexcept
    {
        return (std::int64_t) (settings.regions / kTracks) * kClipFrames;
    }

    void publishPosition (std::int64_t frame) noexcept
    {
#ifndef DUSK_READER_BENCH_LEGACY
        pool.setPositionRt (frame);
#else
        (void) frame;
#endif
    }

    void locate (std::int64_t frame)
    {
#ifndef DUSK_READER_BENCH_LEGACY
        pool.primeAt (frame);
#else
        const auto clip = (std::size_t) (frame / kClipFrames);
        for (int track = 0; track < kTracks; ++track)
            regions[clip * kTracks + (std::size_t) track].reader->prefetch (frame % kClipFrames);
#endif
    }

    Reader& reader (int track, std::int64_t timelineFrame)
    {
        return *regions[(std::size_t) (timelineFrame / kClipFrames) * kTracks
                      + (std::size_t) track].reader;
    }

    void applyLoopCache (int track, const Segment& segment, float* const* destination) const noexcept
    {
        if (settings.mode != "loop" || segment.timelineFrame >= kLoopStart + kLoopCacheFrames)
            return;
        const auto from = std::max (segment.timelineFrame, kLoopStart);
        const auto end = std::min (segment.timelineFrame + segment.frames, kLoopStart + kLoopCacheFrames);
        if (end <= from) return;
        const auto& cache = regions[(std::size_t) track].loopCache;
        for (int c = 0; c < kChannels; ++c)
            std::copy_n (cache[(std::size_t) c].data() + (from - kLoopStart), end - from,
                         destination[c] + (from - segment.timelineFrame));
    }

    std::uint64_t wakeups() const
    {
#ifdef DUSK_READER_BENCH_LEGACY
        std::uint64_t total = 0;
        for (const auto& region : regions) total += region.reader->workerWakeups();
        return total;
#else
        return pool.stats().workerWakeups;
#endif
    }

    std::size_t residentBytes() const
    {
#ifdef DUSK_READER_BENCH_LEGACY
        std::size_t total = 0;
        for (const auto& region : regions) total += region.reader->windowAllocatedBytes();
        // The archived FileReader has no scratch-capacity API. Every warm
        // reader requested an 8192-frame chunk; report that scratch estimate.
        return total + regions.size() * 8192 * kChannels * sizeof (float);
#else
        return pool.stats().allocatedBytes;
#endif
    }

    std::size_t loopCacheBytes() const noexcept
    {
        std::size_t total = 0;
        for (const auto& region : regions)
            for (const auto& channel : region.loopCache) total += channel.capacity() * sizeof (float);
        return total;
    }

private:
    void warmInitial (std::int64_t initial)
    {
#ifndef DUSK_READER_BENCH_LEGACY
        pool.primeAt (initial);
#else
        for (std::size_t i = 0; i < regions.size(); ++i)
            regions[i].reader->prefetch (i < kTracks ? initial : 0);
        const auto deadline = Clock::now() + std::chrono::seconds (10);
        for (;;)
        {
            bool ready = true;
            for (std::size_t i = 0; i < regions.size(); ++i)
                ready = ready && regions[i].reader->holds (i < kTracks ? initial : 0, kWindowFrames - 1);
            if (ready) break;
            if (Clock::now() >= deadline) throw std::runtime_error ("legacy readers did not warm within 10 seconds");
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
#endif
        for (int track = 0; track < kTracks; ++track)
            if (! reader (track, initial).holds (initial % kClipFrames, kBlockFrames))
                throw std::runtime_error ("initial playback position is not resident");
    }

    void prepareLoopCaches()
    {
        auto direct = dusk::audio::FileReader::open (wave.path());
        if (direct == nullptr) throw std::runtime_error ("could not open loop-cache source");
        for (int track = 0; track < kTracks; ++track)
        {
            auto& cache = regions[(std::size_t) track].loopCache;
            std::array<float*, kChannels> destination;
            for (int c = 0; c < kChannels; ++c)
            {
                cache[(std::size_t) c].resize ((std::size_t) kLoopCacheFrames);
                destination[(std::size_t) c] = cache[(std::size_t) c].data();
            }
            if (direct->read (destination.data(), kChannels, kLoopStart, kLoopCacheFrames) != kLoopCacheFrames)
                throw std::runtime_error ("could not fill loop-start cache");
        }
    }

    const Options& settings;
    SampleWave wave;
#ifndef DUSK_READER_BENCH_LEGACY
    dusk::audio::ReaderFillPool pool { 3 };
#endif
    std::vector<Region> regions;
};

struct ProcessSample
{
    std::optional<std::uint64_t> rssBytes;
    std::optional<std::uint64_t> highWaterRssBytes;
    std::optional<std::uint64_t> threads;
};

ProcessSample processSample()
{
    ProcessSample result;
#ifdef __linux__
    std::ifstream status ("/proc/self/status");
    std::string line;
    while (std::getline (status, line))
    {
        std::uint64_t value = 0;
        if (line.compare (0, 6, "VmRSS:") == 0)
        {
            std::istringstream (line.substr (6)) >> value;
            result.rssBytes = value * 1024;
        }
        else if (line.compare (0, 6, "VmHWM:") == 0)
        {
            std::istringstream (line.substr (6)) >> value;
            result.highWaterRssBytes = value * 1024;
        }
        else if (line.compare (0, 8, "Threads:") == 0)
        {
            std::istringstream (line.substr (8)) >> value;
            result.threads = value;
        }
    }
#endif
    return result;
}

std::optional<std::uint64_t> threadCpuNanoseconds() noexcept
{
#ifdef __linux__
    timespec value {};
    if (clock_gettime (CLOCK_THREAD_CPUTIME_ID, &value) == 0)
        return (std::uint64_t) value.tv_sec * 1000000000 + (std::uint64_t) value.tv_nsec;
#endif
    return std::nullopt;
}

void keepMaximum (std::optional<std::uint64_t>& target, std::optional<std::uint64_t> value)
{
    if (value && (! target || *value > *target)) target = value;
}

struct Measurements
{
    std::vector<double> callbackWallUs;
    std::vector<double> callbackCpuUs;
    std::vector<double> locatePrimeMs;
    std::uint64_t reads = 0;
    std::uint64_t rawReadMisses = 0;
    std::uint64_t uncoveredReadMisses = 0;
    std::uint64_t uncoveredBlocks = 0;
    std::uint64_t wrongSourceFrames = 0;
    std::uint64_t unexpectedWrongFrames = 0;
    std::uint64_t nonzeroSamples = 0;
    std::uint64_t loopWraps = 0;
    std::uint64_t wakeups = 0;
    std::size_t peakResidentBytes = 0;
    std::size_t loopCacheBytes = 0;
    ProcessSample peaks;
    double checksum = 0.0;
    double referenceChecksum = 0.0;
    double outputEnergy = 0.0;
    double elapsedSeconds = 0.0;
};

Measurements measure (const Options& settings, Streams& streams)
{
    Measurements result;
    result.callbackWallUs.reserve (settings.blocks);
    result.callbackCpuUs.reserve (settings.blocks);
    result.locatePrimeMs.reserve (3);
    result.loopCacheBytes = streams.loopCacheBytes();
    const auto observe = [&]
    {
        result.peakResidentBytes = std::max (result.peakResidentBytes, streams.residentBytes());
        const auto sample = processSample();
        keepMaximum (result.peaks.rssBytes, sample.rssBytes);
        keepMaximum (result.peaks.highWaterRssBytes, sample.highWaterRssBytes);
        keepMaximum (result.peaks.threads, sample.threads);
    };
    observe();
    const auto wakeStart = streams.wakeups();
    auto timeline = settings.mode == "loop" ? kLoopStart : 0;
    using TrackData = std::array<std::array<float, kBlockFrames>, kChannels>;
    std::array<TrackData, kTracks> tracks {};
    TrackData mix {};
    const auto started = Clock::now();
    auto deadline = started;
    const auto period = std::chrono::duration_cast<Clock::duration> (
                           std::chrono::duration<double> ((double) kBlockFrames / kRate));
    std::size_t nextLocate = 0;
    const auto clips = settings.regions / kTracks;
    const std::array<std::size_t, 3> locateClips { std::min<std::size_t> (1, clips - 1),
                                                clips / 2, clips - 1 };
    for (std::size_t block = 0; block < settings.blocks; ++block)
    {
        if (settings.mode == "locate" && nextLocate < 3
            && block >= settings.blocks * (nextLocate + 1) / 4)
        {
            const auto target = (std::int64_t) locateClips[nextLocate] * kClipFrames + kRate * 3;
            const auto primeStart = Clock::now();
            streams.locate (target);
            const auto primeEnd = Clock::now();
            result.locatePrimeMs.push_back (std::chrono::duration<double, std::milli> (primeEnd - primeStart).count());
            deadline += primeEnd - primeStart;
            timeline = target;
            ++nextLocate;
        }
        if (settings.mode != "loop" && streams.sessionFrames() - timeline < kBlockFrames) break;
        std::array<Segment, 2> segments {};
        int segmentCount = 0;
        auto nextTimeline = timeline;
        for (int offset = 0; offset < kBlockFrames; )
        {
            if (settings.mode == "loop" && nextTimeline == kLoopEnd)
            {
                nextTimeline = kLoopStart;
                ++result.loopWraps;
            }
            const auto untilClip = kClipFrames - nextTimeline % kClipFrames;
            auto frames = std::min<std::int64_t> (kBlockFrames - offset, untilClip);
            if (settings.mode == "loop") frames = std::min (frames, kLoopEnd - nextTimeline);
            frames = std::min (frames, streams.sessionFrames() - nextTimeline);
            if (frames <= 0 || segmentCount == (int) segments.size())
                throw std::runtime_error ("invalid callback timeline plan");
            segments[(std::size_t) segmentCount++] = { nextTimeline, offset, (int) frames };
            nextTimeline += frames;
            offset += (int) frames;
        }
        std::array<std::array<bool, 2>, kTracks> readSucceeded {};
        const auto cpuStart = threadCpuNanoseconds();
        const auto callbackStart = Clock::now();
        streams.publishPosition (timeline);
        for (auto& channel : mix) channel.fill (0.0f);
        for (int track = 0; track < kTracks; ++track)
        {
            auto& data = tracks[(std::size_t) track];
            for (int s = 0; s < segmentCount; ++s)
            {
                const auto& segment = segments[(std::size_t) s];
                std::array<float*, kChannels> destination { data[0].data() + segment.outputOffset,
                                                          data[1].data() + segment.outputOffset };
                const bool complete = streams.reader (track, segment.timelineFrame).readRt (
                    destination.data(), kChannels, segment.timelineFrame % kClipFrames, segment.frames);
                readSucceeded[(std::size_t) track][(std::size_t) s] = complete;
                ++result.reads;
                result.rawReadMisses += complete ? 0 : 1;
                streams.applyLoopCache (track, segment, destination.data());
            }
            for (int c = 0; c < kChannels; ++c)
                for (int f = 0; f < kBlockFrames; ++f)
                    mix[(std::size_t) c][(std::size_t) f] += data[(std::size_t) c][(std::size_t) f];
        }
        const auto callbackEnd = Clock::now();
        const auto cpuEnd = threadCpuNanoseconds();
        result.callbackWallUs.push_back (std::chrono::duration<double, std::micro> (callbackEnd - callbackStart).count());
        if (cpuStart && cpuEnd) result.callbackCpuUs.push_back ((double) (*cpuEnd - *cpuStart) / 1000.0);

        bool blockWrong = false;
        for (int track = 0; track < kTracks; ++track)
            for (int s = 0; s < segmentCount; ++s)
            {
                const auto& segment = segments[(std::size_t) s];
                bool segmentWrong = false;
                for (int f = 0; f < segment.frames; ++f)
                {
                    bool frameWrong = false;
                    for (int c = 0; c < kChannels; ++c)
                    {
                        const auto value = tracks[(std::size_t) track][(std::size_t) c]
                                                  [(std::size_t) (segment.outputOffset + f)];
                        const auto expected = sourceSample (c, segment.timelineFrame % kClipFrames + f);
                        frameWrong = frameWrong || ! std::isfinite (value) || std::abs (value - expected) > 1.0e-7f;
                    }
                    if (frameWrong)
                    {
                        ++result.wrongSourceFrames;
                        if (readSucceeded[(std::size_t) track][(std::size_t) s]) ++result.unexpectedWrongFrames;
                        segmentWrong = true;
                    }
                }
                if (segmentWrong && ! readSucceeded[(std::size_t) track][(std::size_t) s])
                    ++result.uncoveredReadMisses;
                blockWrong = blockWrong || segmentWrong;
            }
        result.uncoveredBlocks += blockWrong ? 1 : 0;
        for (int s = 0; s < segmentCount; ++s)
            for (int f = 0; f < segments[(std::size_t) s].frames; ++f)
                for (int c = 0; c < kChannels; ++c)
                {
                    const auto& segment = segments[(std::size_t) s];
                    const auto value = mix[(std::size_t) c][(std::size_t) (segment.outputOffset + f)];
                    const auto expected = (double) sourceSample (c, segment.timelineFrame % kClipFrames + f) * kTracks;
                    const auto weight = 1.0 + (double) ((block + (std::size_t) f + (std::size_t) c) % 17) / 17.0;
                    result.checksum += (double) value * weight;
                    result.referenceChecksum += expected * weight;
                    result.outputEnergy += (double) value * value;
                    result.nonzeroSamples += value != 0.0f ? 1 : 0;
                }
        // Buffer statistics are sampled outside callback timing every block;
        // /proc sampling is less frequent and also outside callback timing.
        result.peakResidentBytes = std::max (result.peakResidentBytes, streams.residentBytes());
        if (block % 20 == 0) observe();
        timeline = nextTimeline;
        if (settings.mode == "loop" && timeline == kLoopEnd)
        {
            timeline = kLoopStart;
            ++result.loopWraps;
        }
        deadline += period;
        std::this_thread::sleep_until (deadline);
    }
    result.elapsedSeconds = std::chrono::duration<double> (Clock::now() - started).count();
    result.wakeups = streams.wakeups() - wakeStart;
    observe();
    return result;
}

std::string timingJson (std::vector<double> values)
{
    if (values.empty()) return "null";
    const auto mean = std::accumulate (values.begin(), values.end(), 0.0) / (double) values.size();
    std::sort (values.begin(), values.end());
    const auto p95 = (std::size_t) std::ceil ((double) values.size() * 0.95) - 1;
    std::ostringstream json;
    json << std::setprecision (12) << "{\"mean\":" << mean << ",\"p95\":" << values[p95]
         << ",\"max\":" << values.back() << "}";
    return json.str();
}

std::string optionalJson (std::optional<std::uint64_t> value)
{
    return value ? std::to_string (*value) : "null";
}

std::string numberJson (double value)
{
    if (! std::isfinite (value)) return "null";
    std::ostringstream json;
    json << std::setprecision (17) << value;
    return json.str();
}

std::string report (const Options& settings, const Measurements& result)
{
    const bool nonzero = result.nonzeroSamples > 0 && std::isfinite (result.outputEnergy)
                      && result.outputEnergy > 0.0 && std::isfinite (result.checksum);
    std::ostringstream json;
    json << std::setprecision (17) << "{\n";
#ifdef DUSK_READER_BENCH_LEGACY
    json << "  \"implementation\":\"legacy_background\",\n"
            "  \"memory_metric\":\"observed_ring_and_pointer_capacity_plus_8192_frame_scratch_estimate\",\n"
            "  \"baseline_instrumentation\":\"one_relaxed_atomic_increment_after_each_disk_worker_wait\",\n"
         << "  \"configured_disk_workers\":" << settings.regions << ",\n";
#else
    json << "  \"implementation\":\"shared_fill_pool\",\n"
            "  \"memory_metric\":\"observed_window_pointer_and_FileReader_scratch_capacity\",\n"
            "  \"baseline_instrumentation\":null,\n"
            "  \"configured_disk_workers\":3,\n";
#endif
    json << "  \"mode\":\"" << settings.mode << "\",\n"
         << "  \"regions\":" << settings.regions << ",\n"
         << "  \"tracks\":" << kTracks << ",\n"
         << "  \"sample_rate\":" << kRate << ",\n"
         << "  \"block_frames\":" << kBlockFrames << ",\n"
         << "  \"window_frames\":" << kWindowFrames << ",\n"
         << "  \"warmup_settle_ms\":150,\n"
         << "  \"requested_blocks\":" << settings.blocks << ",\n"
         << "  \"measured_blocks\":" << result.callbackWallUs.size() << ",\n"
         << "  \"elapsed_seconds\":" << result.elapsedSeconds << ",\n"
         << "  \"callback_wall_us\":" << timingJson (result.callbackWallUs) << ",\n"
         << "  \"callback_thread_cpu_us\":" << timingJson (result.callbackCpuUs) << ",\n"
         << "  \"locate_priming_ms\":" << timingJson (result.locatePrimeMs) << ",\n"
         << "  \"locates\":" << result.locatePrimeMs.size() << ",\n"
         << "  \"loop_wraps\":" << result.loopWraps << ",\n"
         << "  \"reads\":" << result.reads << ",\n"
         << "  \"raw_read_misses\":" << result.rawReadMisses << ",\n"
         << "  \"uncovered_read_misses\":" << result.uncoveredReadMisses << ",\n"
         << "  \"uncovered_blocks\":" << result.uncoveredBlocks << ",\n"
         << "  \"wrong_source_frames\":" << result.wrongSourceFrames << ",\n"
         << "  \"unexpected_wrong_source_frames\":" << result.unexpectedWrongFrames << ",\n"
         << "  \"source_phase_checked\":true,\n"
         << "  \"nonzero_output\":" << (nonzero ? "true" : "false") << ",\n"
         << "  \"gap_free\":" << (result.wrongSourceFrames == 0 ? "true" : "false") << ",\n"
         << "  \"checksum_matches_reference\":"
         << (std::isfinite (result.checksum)
                 && std::abs (result.checksum - result.referenceChecksum)
                       <= 1.0e-9 * (1.0 + std::abs (result.referenceChecksum)) ? "true" : "false") << ",\n"
         << "  \"output_checksum\":" << numberJson (result.checksum) << ",\n"
         << "  \"reference_checksum\":" << numberJson (result.referenceChecksum) << ",\n"
         << "  \"output_energy\":" << numberJson (result.outputEnergy) << ",\n"
         << "  \"sampled_peak_resident_audio_buffer_bytes\":" << result.peakResidentBytes << ",\n"
         << "  \"resident_memory_sample_period_frames\":" << kBlockFrames << ",\n"
         << "  \"loop_cache_bytes\":" << result.loopCacheBytes << ",\n"
         << "  \"sampled_peak_audio_buffer_and_loop_cache_bytes\":" << result.peakResidentBytes + result.loopCacheBytes << ",\n"
         << "  \"sampled_peak_process_rss_bytes\":" << optionalJson (result.peaks.rssBytes) << ",\n"
         << "  \"observed_process_rss_high_water_bytes\":" << optionalJson (result.peaks.highWaterRssBytes) << ",\n"
         << "  \"sampled_peak_process_threads\":" << optionalJson (result.peaks.threads) << ",\n"
         << "  \"sampled_peak_worker_threads\":"
         << optionalJson (result.peaks.threads && *result.peaks.threads > 0
                              ? std::optional<std::uint64_t> (*result.peaks.threads - 1) : std::nullopt) << ",\n"
         << "  \"disk_worker_wait_returns\":" << result.wakeups << ",\n"
         << "  \"disk_worker_wait_returns_per_second\":" << (double) result.wakeups / result.elapsedSeconds << ",\n"
         << "  \"notes\":\"Shared warm-page-cache PCM16 input; paced standalone reader and mix workload, no device or DSP. "
            "Callback timing excludes phase checks, statistics, pacing and locate priming. "
            "Wait-return counters include timeout and notification returns, not OS scheduler switches. "
            "Memory is sampled, not an allocation high-water counter; RSS includes harness overhead.\"\n}\n";
    return json.str();
}
} // namespace

int main (int argc, char** argv)
{
    try
    {
        const auto settings = options (argc, argv);
        Streams streams (settings);
        const auto result = measure (settings, streams);
        const auto json = report (settings, result);
        std::cout << json;
        if (! settings.output.empty())
        {
            std::ofstream output (settings.output);
            output << json;
            output.flush();
            if (! output) throw std::runtime_error ("could not write JSON report");
        }
        if (result.callbackWallUs.empty() || result.nonzeroSamples == 0
            || ! std::isfinite (result.outputEnergy) || result.outputEnergy <= 0.0
            || ! std::isfinite (result.checksum) || result.unexpectedWrongFrames != 0
            || (result.wrongSourceFrames == 0
                && std::abs (result.checksum - result.referenceChecksum)
                      > 1.0e-9 * (1.0 + std::abs (result.referenceChecksum))))
            return 2;
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "reader-pool-benchmark: " << error.what() << '\n';
        return 1;
    }
}
