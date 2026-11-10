#include "MidiRecoveryWriter.h"
#include "../foundation/AtomicFile.h"

#include <map>
#include <optional>
#include <set>
#include <utility>

namespace duskstudio::midirecovery
{
Writer::Writer (Report reportIn) : report (std::move (reportIn)) {}

Writer::~Writer()
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        stopping = true;
    }
    wake.notify_all();
    if (worker.joinable())
        worker.join();
}

void Writer::queue (PendingFile file) noexcept
{
    try
    {
        const std::lock_guard<std::mutex> lock (mutex);
        if (! worker.joinable())
            worker = std::thread ([this] { run(); });
        // A failed push leaves `file` as it was, for the write below.
        files.push_back (std::move (file));
        ++queued;
        wake.notify_one();
        return;
    }
    catch (...)
    {
    }
    write (file);
}

void Writer::flush()
{
    std::unique_lock<std::mutex> lock (mutex);
    const auto target = queued;
    progress.wait (lock, [this, target] { return finished >= target; });
}

void Writer::run() noexcept
{
    std::unique_lock<std::mutex> lock (mutex);
    for (;;)
    {
        wake.wait (lock, [this] { return stopping || ! files.empty(); });
        if (files.empty()) return;
        auto file = std::move (files.front());
        files.pop_front();
        lock.unlock();
        write (file);
        lock.lock();
        ++finished;
        progress.notify_all();
    }
}

void Writer::write (const PendingFile& file) noexcept
{
    try
    {
        const std::lock_guard<std::mutex> lock (writing);
        const auto result = writeFile (file);
        if (report) report (file, result);
    }
    catch (...)
    {
    }
}

struct Journal::Impl
{
    struct State
    {
        explicit State (Config value) : config (std::move (value)), decoder (std::make_unique<MidiCaptureStream> (1))
        {
            decoder->configure ([this] (std::int64_t sample)
            {
                const auto& timing = config.timing;
                return timing.tempoMap.empty() ? samplesToTicks (sample, timing.sampleRate, timing.bpm)
                    : timing.tempoMap.samplesToTicks (config.timelineStart + sample, timing.sampleRate)
                        - timing.tempoMap.samplesToTicks (config.timelineStart, timing.sampleRate);
            }, config.loopLength);
        }
        PendingFile file() const
        {
            return { config.sessionDir, config.made, config.trackNumber, sanitizeName (config.name),
                     TrackFile::Playing, Folder::Takes, {} };
        }
        Config config;
        std::unique_ptr<MidiCaptureStream> decoder;
        std::map<int, std::filesystem::path> checkpoints;
        std::uint64_t writtenGeneration = 0;
        std::set<int> retired;
        std::map<int, MidiRegion> pending;
        std::array<MidiCc, MidiCc::kLaneCount> controllers {};
        std::array<bool, MidiCc::kLaneCount> controllerValid {};
    };
    struct Batch
    {
        std::uint64_t id = 0, start = 0, dropped = 0;
        std::size_t count = 0;
        MidiCaptureStream::DrainView view;
    };
    struct Job
    {
        std::uint64_t id = 0, batchFence = 0;
        int passOrdinal = 1;
        std::int64_t endSample = 0;
        bool finish = false;
        Handle handle;
        PendingFile file;
    };

    Impl (Writer::Report value, std::size_t capacity)
        : report (std::move (value)), records (std::max<std::size_t> (1, capacity)),
          scratch (records.size()), worker ([this] { run(); }) {}
    ~Impl()
    {
        { const std::lock_guard<std::mutex> lock (mutex); stopping = true; }
        wake.notify_one();
        worker.join();
    }

    bool append (Handle& handle, const MidiCaptureStream::DrainView& view) noexcept
    {
        auto attempted = std::max (handle.latest.attemptedSequence, view.attemptedSequence);
        const auto* last = view.secondCount != 0 ? &view.second[view.secondCount - 1]
            : view.firstCount != 0 ? &view.first[view.firstCount - 1] : nullptr;
        if (last != nullptr)
            attempted = std::max (attempted, last->event.sequence - (last->progress ? 1u : 0u));
        if (last == nullptr && handle.dropped == 0 && attempted == handle.latest.attemptedSequence
            && view.completedOrdinal == handle.latest.completedOrdinal
            && view.linearCaptureLength == handle.latest.linearCaptureLength) return true;
        handle.latest = view;
        handle.latest.attemptedSequence = attempted;
        handle.latest.first = handle.latest.second = nullptr;
        handle.latest.firstCount = handle.latest.secondCount = 0;
        const auto count = view.firstCount + view.secondCount;
        const auto write = batchWrite.load (std::memory_order_relaxed);
        if (write - batchRead.load (std::memory_order_acquire) == batches.size()
            || count > records.size() - (recordWrite - recordRead.load (std::memory_order_acquire)))
        {
            ++handle.dropped;
            return false;
        }
        auto destination = recordWrite;
        for (std::size_t i = 0; i < view.firstCount; ++i)
            records[(std::size_t) (destination++ % records.size())] = view.first[i];
        for (std::size_t i = 0; i < view.secondCount; ++i)
            records[(std::size_t) (destination++ % records.size())] = view.second[i];
        batches[(std::size_t) (write % batches.size())] = { handle.id, recordWrite, handle.dropped, count, handle.latest };
        handle.dropped = 0;
        recordWrite = destination;
        batchWrite.store (write + 1, std::memory_order_release);
        wake.notify_one();
        return true;
    }

    void notify (const PendingFile& file, const WriteResult& result) noexcept
    {
        try { if (report) report (file, result); } catch (...) {}
    }
    void failure (const PendingFile& file, const std::filesystem::path& path, std::error_code error) noexcept
    {
        try { notify (file, { {}, { { path, error } } }); } catch (...) {}
    }

    MidiRegion regionOf (const State& state, const MidiCapturedPass& pass)
    {
        MidiRegion region;
        region.timelineStart = state.config.timelineStart;
        region.lengthInSamples = pass.lengthInSamples;
        region.lengthInTicks = pass.lengthInTicks;
        region.notes = pass.notes;
        for (std::size_t lane = 0; lane < state.controllers.size(); ++lane)
            if (state.controllerValid[lane])
            {
                auto chased = state.controllers[lane];
                chased.atTick = 0;
                region.ccs.push_back (chased);
            }
        region.ccs.insert (region.ccs.end(), pass.ccs.begin(), pass.ccs.end());
        return region;
    }

    bool checkpoint (State& state, int ordinal, const MidiRegion& region)
    {
        if (state.retired.count (ordinal) != 0 || region.lengthInSamples <= 0
            || (region.notes.empty() && region.ccs.empty())) return true;
        auto file = state.file();
        file.bytes = buildTakeFile (state.config.timing, region, state.config.name);
        if (file.bytes.empty()) return false;
        const auto folder = file.sessionDir / "midi" / "recording";
        std::error_code error;
        if (! std::filesystem::is_directory (dusk::fs::nativeIOPath (file.sessionDir), error))
        {
            failure (file, file.sessionDir, error ? error : std::make_error_code (std::errc::not_a_directory));
            return false;
        }
        for (const auto& level : { folder.parent_path(), folder })
        {
            std::filesystem::create_directory (dusk::fs::nativeIOPath (level), error);
            if (! error) dusk::fs::syncFolder (level.parent_path(), error);
            if (error) { failure (file, level, error); return false; }
        }
        auto known = state.checkpoints.find (ordinal);
        std::filesystem::path path;
        if (known != state.checkpoints.end())
        {
            path = known->second;
            if (! dusk::fs::writeFileAtomically (path, file.bytes.data(), file.bytes.size(), error))
            { failure (file, path, error); return false; }
        }
        else
        {
            for (std::uint64_t copy = 1;; ++copy)
            {
                auto name = std::filesystem::u8path (fileName (file.made, file.trackNumber, file.name)).stem().u8string();
                name += "_pass" + std::to_string (ordinal);
                if (copy > 1) name += "_" + std::to_string (copy);
                name += ".recording.mid";
                path = folder / std::filesystem::u8path (name);
                if (dusk::fs::writeFileAtomically (path, file.bytes.data(), file.bytes.size(), error,
                                                  dusk::fs::AtomicWriteMode::CreateNew)) break;
                if (error != std::errc::file_exists) { failure (file, path, error); return false; }
            }
            state.checkpoints.emplace (ordinal, path);
        }
        notify (file, { { path }, {} });
        return true;
    }

    void completed (State& state)
    {
        for (const auto& pass : state.decoder->takeCompleted())
        {
            auto& region = state.pending[pass.passOrdinal];
            region = regionOf (state, pass);
            for (const auto& control : pass.ccs)
            {
                const auto lane = (std::size_t) control.laneKey();
                state.controllers[lane] = control;
                state.controllerValid[lane] = true;
            }
            if (checkpoint (state, pass.passOrdinal, region)) state.pending.erase (pass.passOrdinal);
        }
    }
    void checkpoints()
    {
        for (auto& entry : states)
        {
            auto& state = *entry.second;
            for (auto pending = state.pending.begin(); pending != state.pending.end();)
                if (checkpoint (state, pending->first, pending->second)) pending = state.pending.erase (pending);
                else ++pending;
            if (! state.decoder) continue;
            const auto& preview = state.decoder->preview();
            if (state.writtenGeneration != preview.generation
                && checkpoint (state, preview.passOrdinal, regionOf (state, preview)))
                state.writtenGeneration = preview.generation;
        }
    }
    void process (const Job& job)
    {
        const auto found = states.find (job.id);
        if (job.finish)
        {
            if (found == states.end() || ! found->second->decoder) return;
            auto& state = *found->second;
            if (job.handle.dropped != 0)
                failure (state.file(), state.config.sessionDir / "midi" / "recording",
                         std::make_error_code (std::errc::no_buffer_space));
            state.decoder->replay (job.handle.latest);
            completed (state);
            state.decoder->sealFinal (job.passOrdinal, job.endSample);
            completed (state);
            state.decoder.reset();
            if (state.checkpoints.empty() && state.pending.empty()) states.erase (found);
            return;
        }
        auto result = writeFile (job.file);
        if (found != states.end() && ! result.written.empty() && result.failures.empty())
        {
            auto& state = *found->second;
            state.retired.insert (job.passOrdinal);
            state.pending.erase (job.passOrdinal);
            const auto checkpoint = state.checkpoints.find (job.passOrdinal);
            if (checkpoint != state.checkpoints.end())
            {
                std::error_code error;
                std::filesystem::remove (dusk::fs::nativeIOPath (checkpoint->second), error);
                if (! error) dusk::fs::syncFolder (checkpoint->second.parent_path(), error);
                if (error) result.failures.push_back ({ checkpoint->second, error });
                else state.checkpoints.erase (checkpoint);
            }
            if (! state.decoder && state.checkpoints.empty() && state.pending.empty()) states.erase (found);
        }
        notify (job.file, result);
    }

    void run() noexcept
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (1);
        for (;;)
        {
            std::unique_ptr<State> start;
            std::uint64_t id = 0;
            std::optional<Job> job;
            bool force = false, stop = false;
            std::uint64_t flush = 0;
            {
                std::unique_lock<std::mutex> lock (mutex);
                wake.wait_until (lock, deadline, [&]
                {
                    return stopping || ! starts.empty() || ! jobs.empty() || requestedFlush > completedFlush
                        || batchRead.load (std::memory_order_relaxed) != batchWrite.load (std::memory_order_acquire);
                });
                if (! starts.empty())
                {
                    id = starts.front().first;
                    start = std::move (starts.front().second);
                    starts.pop_front();
                }
                else if (! jobs.empty() && batchRead.load (std::memory_order_relaxed) >= jobs.front().batchFence)
                {
                    job = std::move (jobs.front());
                    jobs.pop_front();
                }
                else if (batchRead.load (std::memory_order_relaxed) == batchWrite.load (std::memory_order_acquire))
                {
                    flush = requestedFlush;
                    force = flush > completedFlush || stopping;
                    stop = stopping;
                }
            }
            try
            {
                if (start) states.emplace (id, std::move (start));
                else if (job) process (*job);
                else
                {
                    const auto read = batchRead.load (std::memory_order_relaxed);
                    if (read != batchWrite.load (std::memory_order_acquire))
                    {
                        auto batch = batches[(std::size_t) (read % batches.size())];
                        for (std::size_t i = 0; i < batch.count; ++i)
                            scratch[i] = records[(std::size_t) ((batch.start + i) % records.size())];
                        recordRead.store (batch.start + batch.count, std::memory_order_release);
                        batchRead.store (read + 1, std::memory_order_release);
                        const auto found = states.find (batch.id);
                        if (found != states.end() && found->second->decoder)
                        {
                            auto& state = *found->second;
                            if (batch.dropped != 0)
                                failure (state.file(), state.config.sessionDir / "midi" / "recording",
                                         std::make_error_code (std::errc::no_buffer_space));
                            batch.view.first = scratch.data();
                            batch.view.firstCount = batch.count;
                            state.decoder->replay (batch.view);
                            completed (state);
                        }
                    }
                }
                if (force || std::chrono::steady_clock::now() >= deadline)
                {
                    deadline = std::chrono::steady_clock::now() + std::chrono::seconds (1);
                    checkpoints();
                }
            }
            catch (...)
            {
                if (job) failure (job->file, job->file.sessionDir, std::make_error_code (std::errc::io_error));
            }
            if (force)
            {
                const std::lock_guard<std::mutex> lock (mutex);
                completedFlushReady = std::none_of (states.begin(), states.end(), [&] (const auto& entry)
                {
                    const auto& state = *entry.second;
                    if (! state.decoder && state.pending.empty()) return false;
                    std::error_code error;
                    return flushFolder.empty() || state.config.sessionDir == flushFolder
                        || std::filesystem::equivalent (dusk::fs::nativeIOPath (state.config.sessionDir),
                                                        dusk::fs::nativeIOPath (flushFolder), error);
                });
                completedFlush = flush;
                progress.notify_all();
            }
            if (stop) return;
        }
    }

    Writer::Report report;
    std::vector<MidiCaptureStream::Record> records, scratch;
    std::array<Batch, 128> batches;
    std::uint64_t recordWrite = 0;
    std::atomic<std::uint64_t> recordRead { 0 }, batchWrite { 0 }, batchRead { 0 };
    std::mutex mutex;
    std::condition_variable wake, progress;
    std::deque<std::pair<std::uint64_t, std::unique_ptr<State>>> starts;
    std::deque<Job> jobs;
    std::map<std::uint64_t, std::unique_ptr<State>> states;
    std::uint64_t nextId = 1, requestedFlush = 0, completedFlush = 0;
    std::filesystem::path flushFolder;
    bool completedFlushReady = true;
    bool stopping = false;
    std::thread worker;
};

Journal::Journal (Writer::Report value, std::size_t size) : report (std::move (value)), capacity (size) {}
Journal::~Journal() = default;

Journal::Handle Journal::begin (Config config) noexcept
{
    if (config.sessionDir.empty()) return {};
    try
    {
        if (! impl) impl = std::make_unique<Impl> (report, capacity);
        auto state = std::make_unique<Impl::State> (std::move (config));
        const std::lock_guard<std::mutex> lock (impl->mutex);
        const auto id = impl->nextId++;
        impl->starts.emplace_back (id, std::move (state));
        impl->wake.notify_one();
        return { id, {}, 0 };
    }
    catch (...) { return {}; }
}

bool Journal::append (Handle& handle, const MidiCaptureStream::DrainView& drain) noexcept
{
    return impl && handle.id != 0 && impl->append (handle, drain);
}

bool Journal::finish (const Handle& handle, int passOrdinal, std::int64_t endSample) noexcept
{
    if (! impl || handle.id == 0) return false;
    try
    {
        Impl::Job job;
        job.batchFence = impl->batchWrite.load (std::memory_order_acquire);
        job.id = handle.id;
        job.passOrdinal = passOrdinal;
        job.endSample = endSample;
        job.finish = true;
        job.handle = handle;
        const std::lock_guard<std::mutex> lock (impl->mutex);
        impl->jobs.push_back (std::move (job));
        impl->wake.notify_one();
        return true;
    }
    catch (...) { return false; }
}

bool Journal::commit (const PendingFile& file, std::uint64_t id, int passOrdinal) noexcept
{
    if (! impl) return false;
    try
    {
        Impl::Job job;
        job.batchFence = impl->batchWrite.load (std::memory_order_acquire);
        job.id = id;
        job.passOrdinal = passOrdinal;
        job.file = file;
        const std::lock_guard<std::mutex> lock (impl->mutex);
        impl->jobs.push_back (std::move (job));
        impl->wake.notify_one();
        return true;
    }
    catch (...) { return false; }
}

bool Journal::flush (const std::filesystem::path& folder)
{
    if (! impl) return true;
    std::unique_lock<std::mutex> lock (impl->mutex);
    impl->flushFolder = folder;
    const auto target = ++impl->requestedFlush;
    impl->wake.notify_one();
    impl->progress.wait (lock, [&] { return impl->completedFlush >= target; });
    return impl->completedFlushReady;
}
} // namespace duskstudio::midirecovery
