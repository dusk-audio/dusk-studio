#include "MidiRecoveryWriter.h"
#include "../foundation/AtomicFile.h"
#include "../foundation/AutoResetEvent.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace duskstudio::midirecovery
{
namespace
{
// A checkpoint whose name is taken by a file that is not its own takes the next
// copy number, up to as many as a take file may.
constexpr int kMaxCheckpointCopies = 999;
// A journal whose last write failed sits out this many checkpoint requests at
// most, doubling from one after each failure. Flush and the close try at once.
constexpr unsigned kMaxRetryWait = 32;
} // namespace

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
        std::set<int> retired;
        std::map<int, MidiRegion> pending;
        std::array<MidiCc, MidiCc::kLaneCount> controllers {};
        std::array<bool, MidiCc::kLaneCount> controllerValid {};
        // The preview content the current pass's checkpoint holds.
        std::uint64_t writtenContent = 0;
        // The failure last reported for each pass, so a retry that fails the
        // same way says nothing new.
        std::map<int, Failure> reported;
        bool foldersSynced = false;
        bool due = false;
        unsigned retryWait = 0, retryIn = 0;
    };
    struct Batch
    {
        std::uint64_t id = 0, start = 0, dropped = 0;
        std::size_t count = 0;
        std::int64_t linearCaptureLength = -1;
        std::uint64_t attemptedSequence = 0;
        int completedOrdinal = 0;
    };
    // What a handle has drained and not yet handed to the worker. Message thread only.
    struct Producer
    {
        MidiCaptureStream::DrainView latest;   // the newest drain's bounds, without its records
        std::uint64_t dropped = 0;              // drains that did not fit since the last that did
        MidiCaptureStream::Record held {};      // the newest progress not handed over
        bool holding = false;
    };
    struct Job
    {
        std::uint64_t id = 0, batchFence = 0;
        int passOrdinal = 1;
        std::int64_t endSample = 0;
        bool finish = false;
        Producer producer;
        PendingFile file;
    };

    Impl (Writer::Report value, std::size_t recordCapacity, std::function<void()> hook)
        : report (std::move (value)), records (std::max<std::size_t> (1, recordCapacity)),
          batches (std::max<std::size_t> (64, records.size() / 8)), idleHook (std::move (hook)),
          worker ([this] { run(); }) {}
    ~Impl()
    {
        { const std::lock_guard<std::mutex> lock (mutex); stopping = true; }
        wake.signal();
        worker.join();
    }

    // Two progress markers with no event between them, in one pass, say only
    // how far it reached: the further of them says all of it.
    static bool continues (const MidiCaptureStream::Record& earlier, const MidiCaptureStream::Record& later) noexcept
    {
        return earlier.progress && ! earlier.complete && later.progress && ! later.complete
            && earlier.event.passOrdinal == later.event.passOrdinal && earlier.event.sequence == later.event.sequence;
    }

    bool append (std::uint64_t id, const MidiCaptureStream::DrainView& view) noexcept
    {
        const auto found = producers.find (id);
        if (found == producers.end()) return false;
        auto& producer = found->second;
        auto attempted = std::max (producer.latest.attemptedSequence, view.attemptedSequence);
        const auto* last = view.secondCount != 0 ? &view.second[view.secondCount - 1]
            : view.firstCount != 0 ? &view.first[view.firstCount - 1] : nullptr;
        if (last != nullptr)
            attempted = std::max (attempted, last->event.sequence - (last->progress ? 1u : 0u));
        bool carries = producer.dropped != 0 || attempted != producer.latest.attemptedSequence
            || view.completedOrdinal != producer.latest.completedOrdinal
            || view.linearCaptureLength != producer.latest.linearCaptureLength;
        producer.latest = view;
        producer.latest.attemptedSequence = attempted;
        producer.latest.first = producer.latest.second = nullptr;
        producer.latest.firstCount = producer.latest.secondCount = 0;

        // A drain of nothing but progress stays here, as one marker at its
        // newest position, until a drain with more in it or a checkpoint
        // request takes it along: a batch a tick would fill the queue with
        // positions no checkpoint needs.
        auto held = producer.held;
        auto holding = producer.holding;
        const auto hold = [&] (const MidiCaptureStream::Record& record)
        {
            if (carries) return;
            if (holding && continues (held, record))
                held.event.samplePos = std::max (held.event.samplePos, record.event.samplePos);
            else if (! holding && record.progress && ! record.complete)
            {
                held = record;
                holding = true;
            }
            else carries = true;
        };
        for (std::size_t i = 0; i < view.firstCount; ++i) hold (view.first[i]);
        for (std::size_t i = 0; i < view.secondCount; ++i) hold (view.second[i]);
        if (! carries)
        {
            producer.held = held;
            producer.holding = holding;
            return true;
        }
        if (publish (id, producer, view)) return true;
        ++producer.dropped;
        return false;
    }

    // Hands what the producer holds and the drain's records to the worker as
    // one batch, each run of progress with nothing between as its furthest.
    // Leaves everything as it was when the batch does not fit.
    bool publish (std::uint64_t id, Producer& producer, const MidiCaptureStream::DrainView& view) noexcept
    {
        const auto write = batchWrite.load (std::memory_order_relaxed);
        if (write - batchRead.load (std::memory_order_acquire) == batches.size()) return false;
        const auto limit = recordRead.load (std::memory_order_acquire) + records.size();
        auto end = recordWrite;
        const auto add = [&] (const MidiCaptureStream::Record& record)
        {
            if (end != recordWrite)
            {
                auto& previous = records[(std::size_t) ((end - 1) % records.size())];
                if (continues (previous, record))
                {
                    previous.event.samplePos = std::max (previous.event.samplePos, record.event.samplePos);
                    return true;
                }
            }
            if (end == limit) return false;
            records[(std::size_t) (end++ % records.size())] = record;
            return true;
        };
        if (producer.holding && ! add (producer.held)) return false;
        for (std::size_t i = 0; i < view.firstCount; ++i)
            if (! add (view.first[i])) return false;
        for (std::size_t i = 0; i < view.secondCount; ++i)
            if (! add (view.second[i])) return false;
        const auto& latest = producer.latest;
        batches[(std::size_t) (write % batches.size())] = { id, recordWrite, producer.dropped, (std::size_t) (end - recordWrite),
                                                           latest.linearCaptureLength, latest.attemptedSequence,
                                                           latest.completedOrdinal };
        recordWrite = end;
        producer.dropped = 0;
        producer.holding = false;
        batchWrite.store (write + 1, std::memory_order_release);
        wake.signal();
        return true;
    }

    // What a checkpoint request or a flush needs first: the newest position
    // of every take. One that does not fit waits for the next.
    void sendHeld() noexcept
    {
        for (auto& entry : producers)
            if (entry.second.holding) publish (entry.first, entry.second, {});
    }

    void notify (const PendingFile& file, const WriteResult& result) noexcept
    {
        try { if (report) report (file, result); } catch (...) {}
    }
    void failure (const PendingFile& file, const std::filesystem::path& path, std::error_code error) noexcept
    {
        try { notify (file, { {}, { { path, error } } }); } catch (...) {}
    }
    bool failed (State& state, int ordinal, const PendingFile& file, const std::filesystem::path& path,
                 std::error_code error) noexcept
    {
        state.foldersSynced = false;
        const auto previous = state.reported.find (ordinal);
        if (previous != state.reported.end() && previous->second.path == path && previous->second.error == error)
            return false;
        try { state.reported[ordinal] = { path, error }; } catch (...) {}
        failure (file, path, error);
        return false;
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
        if (state.retired.count (ordinal) != 0 || region.lengthInSamples <= 0) return true;
        auto file = state.file();
        file.bytes = buildTakeFile (state.config.timing, region, state.config.name);
        if (file.bytes.empty()) return false;
        const auto folder = file.sessionDir / "midi" / "recording";
        std::error_code error;
        if (! std::filesystem::is_directory (dusk::fs::nativeIOPath (file.sessionDir), error))
            return failed (state, ordinal, file, file.sessionDir,
                           error ? error : std::make_error_code (std::errc::not_a_directory));
        for (const auto& level : { folder.parent_path(), folder })
        {
            // A folder made here, or one not yet seen to the disk by this
            // journal, gets its entry synced. One already synced needs nothing.
            const bool made = std::filesystem::create_directory (dusk::fs::nativeIOPath (level), error);
            if (! error && (made || ! state.foldersSynced)) dusk::fs::syncFolder (level.parent_path(), error);
            if (error) return failed (state, ordinal, file, level, error);
        }
        state.foldersSynced = true;
        auto known = state.checkpoints.find (ordinal);
        std::filesystem::path path;
        if (known != state.checkpoints.end())
        {
            path = known->second;
            if (! dusk::fs::writeFileAtomically (path, file.bytes.data(), file.bytes.size(), error))
                return failed (state, ordinal, file, path, error);
        }
        else
        {
            const auto stem = std::filesystem::u8path (fileName (file.made, file.trackNumber, file.name)).stem().u8string()
                + "_pass" + std::to_string (ordinal);
            for (int copy = 1;; ++copy)
            {
                if (copy > kMaxCheckpointCopies)
                    return failed (state, ordinal, file, folder, std::make_error_code (std::errc::file_exists));
                auto name = stem;
                if (copy > 1) name += "_" + std::to_string (copy);
                name += ".recording.mid";
                path = folder / std::filesystem::u8path (name);
                // A name already taken is passed over without writing; the
                // create-new write below settles a name taken meanwhile.
                const auto status = std::filesystem::symlink_status (dusk::fs::nativeIOPath (path), error);
                if (status.type() != std::filesystem::file_type::not_found)
                {
                    if (error) return failed (state, ordinal, file, path, error);
                    continue;
                }
                if (dusk::fs::writeFileAtomically (path, file.bytes.data(), file.bytes.size(), error,
                                                  dusk::fs::AtomicWriteMode::CreateNew)) break;
                if (error != std::errc::file_exists) return failed (state, ordinal, file, path, error);
            }
            state.checkpoints.emplace (ordinal, path);
        }
        state.reported.erase (ordinal);
        notify (file, { { path }, {} });
        return true;
    }

    // Only a pass with notes or controllers of its own gets a checkpoint: Stop
    // and MIDI Merge give a pass a final file only then, and a checkpoint with
    // nothing but controllers chased from earlier passes would never be retired.
    void completed (State& state)
    {
        for (const auto& pass : state.decoder->takeCompleted())
        {
            const bool own = ! pass.notes.empty() || ! pass.ccs.empty();
            MidiRegion region;
            if (own) region = regionOf (state, pass);
            for (const auto& control : pass.ccs)
            {
                const auto lane = (std::size_t) control.laneKey();
                state.controllers[lane] = control;
                state.controllerValid[lane] = true;
            }
            if (own && ! checkpoint (state, pass.passOrdinal, region))
                state.pending[pass.passOrdinal] = std::move (region);
        }
    }

    // Retries the passes still unwritten, then writes the current pass when
    // its notes or controllers changed since its checkpoint was written. True
    // when nothing failed.
    bool refresh (State& state)
    {
        bool written = true;
        for (auto pending = state.pending.begin(); pending != state.pending.end();)
            if (checkpoint (state, pending->first, pending->second)) pending = state.pending.erase (pending);
            else
            {
                written = false;
                ++pending;
            }
        if (! state.decoder) return written;
        const auto& preview = state.decoder->preview();
        if (state.writtenContent == preview.contentGeneration) return written;
        if (preview.notes.empty() && preview.ccs.empty())
        {
            state.writtenContent = preview.contentGeneration;
            return written;
        }
        // Content whose pass has no length yet waits for its first position.
        if (preview.lengthInSamples <= 0) return written;
        if (! checkpoint (state, preview.passOrdinal, regionOf (state, preview))) return false;
        state.writtenContent = preview.contentGeneration;
        return written;
    }

    void schedule (State& state, bool forced) noexcept
    {
        if (forced || state.retryIn == 0) state.due = true;
        else --state.retryIn;
    }

    // One journal's checkpoints a turn, so decoding goes on between writes.
    bool refreshNext()
    {
        for (auto& entry : states)
        {
            auto& state = *entry.second;
            if (! state.due) continue;
            state.due = false;
            const bool written = refresh (state);
            state.retryWait = written ? 0 : std::min (kMaxRetryWait, std::max (1u, state.retryWait * 2));
            state.retryIn = state.retryWait;
            return true;
        }
        return false;
    }

    void decode (std::uint64_t read)
    {
        const auto batch = batches[(std::size_t) (read % batches.size())];
        const auto found = states.find (batch.id);
        auto* state = found != states.end() && found->second->decoder ? found->second.get() : nullptr;
        {
            // The batch and its records go back to the producer however the replay ends.
            struct Release
            {
                Impl& impl;
                const Batch& batch;
                std::uint64_t next;
                ~Release()
                {
                    impl.recordRead.store (batch.start + batch.count, std::memory_order_release);
                    impl.batchRead.store (next, std::memory_order_release);
                }
            } release { *this, batch, read + 1 };
            if (state != nullptr)
            {
                const auto start = (std::size_t) (batch.start % records.size());
                const auto first = std::min (batch.count, records.size() - start);
                state->decoder->replay ({ records.data() + start, first, records.data(), batch.count - first,
                                          batch.linearCaptureLength, batch.attemptedSequence, batch.completedOrdinal });
            }
        }
        if (state == nullptr) return;
        if (batch.dropped != 0)
            failure (state->file(), state->config.sessionDir / "midi" / "recording",
                     std::make_error_code (std::errc::no_buffer_space));
        completed (*state);
    }

    void process (const Job& job)
    {
        const auto found = states.find (job.id);
        if (job.finish)
        {
            if (found == states.end() || ! found->second->decoder) return;
            auto& state = *found->second;
            const auto& producer = job.producer;
            if (producer.dropped != 0)
                failure (state.file(), state.config.sessionDir / "midi" / "recording",
                         std::make_error_code (std::errc::no_buffer_space));
            const auto& latest = producer.latest;
            state.decoder->replay ({ producer.holding ? &producer.held : nullptr, producer.holding ? 1u : 0u,
                                     nullptr, 0, latest.linearCaptureLength, latest.attemptedSequence,
                                     latest.completedOrdinal });
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
            state.reported.erase (job.passOrdinal);
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

    // Only a take still recording into the folder keeps it from being ready.
    // A pass whose checkpoint could not be written goes on retrying where it
    // was; the session being saved holds it only while a region keeps it.
    bool readyFor (const std::filesystem::path& folder) const
    {
        return std::none_of (states.begin(), states.end(), [&] (const auto& entry)
        {
            const auto& state = *entry.second;
            if (! state.decoder) return false;
            std::error_code error;
            return folder.empty() || state.config.sessionDir == folder
                || std::filesystem::equivalent (dusk::fs::nativeIOPath (state.config.sessionDir),
                                                dusk::fs::nativeIOPath (folder), error);
        });
    }

    void run() noexcept
    {
        std::uint64_t forced = 0;
        bool closing = false;
        for (;;)
        {
            std::optional<Job> job;
            bool batch = false, round = false, forcedRound = false, stop = false;
            std::uint64_t flush = 0;
            const auto read = batchRead.load (std::memory_order_relaxed);
            {
                const std::lock_guard<std::mutex> lock (mutex);
                // A batch seen here was published after its take's start, so
                // the start is in hand before the batch is decoded.
                started.swap (starts);
                batch = read != batchWrite.load (std::memory_order_acquire);
                if (! jobs.empty() && read >= jobs.front().batchFence)
                {
                    job = std::move (jobs.front());
                    jobs.pop_front();
                }
                if (checkpointRequested && read >= checkpointFence)
                {
                    checkpointRequested = false;
                    round = true;
                }
                if (! batch && ! job && jobs.empty())
                {
                    if (requestedFlush > forced)
                    {
                        forced = requestedFlush;
                        forcedRound = true;
                    }
                    if (stopping && ! closing) closing = forcedRound = true;
                    flush = forced > completedFlush ? forced : 0;
                    stop = stopping;
                }
            }
            try
            {
                for (auto& entry : started) states.emplace (entry.first, std::move (entry.second));
            }
            catch (...)
            {
            }
            started.clear();
            try
            {
                if (round || forcedRound)
                    for (auto& entry : states) schedule (*entry.second, forcedRound);
                if (job)
                {
                    process (*job);
                    continue;
                }
                if (batch)
                {
                    decode (read);
                    continue;
                }
                if (refreshNext()) continue;
            }
            catch (...)
            {
                if (job) failure (job->file, job->file.sessionDir, std::make_error_code (std::errc::io_error));
                continue;
            }
            if (flush != 0)
            {
                std::filesystem::path folder;
                {
                    const std::lock_guard<std::mutex> lock (mutex);
                    folder.swap (flushFolder);
                }
                bool ready = false;
                try { ready = readyFor (folder); } catch (...) {}
                const std::lock_guard<std::mutex> lock (mutex);
                completedFlushReady = ready;
                completedFlush = flush;
                progress.notify_all();
                continue;
            }
            if (stop) return;
            if (idleHook) idleHook();
            wake.wait();
        }
    }

    Writer::Report report;
    std::vector<MidiCaptureStream::Record> records;
    std::vector<Batch> batches;
    std::function<void()> idleHook;
    // The producer's side, on the message thread.
    std::map<std::uint64_t, Producer> producers;
    std::uint64_t recordWrite = 0, nextId = 1;
    std::atomic<std::uint64_t> recordRead { 0 }, batchWrite { 0 }, batchRead { 0 };
    // The worker's.
    std::map<std::uint64_t, std::unique_ptr<State>> states;
    std::deque<std::pair<std::uint64_t, std::unique_ptr<State>>> started;
    // Guarded by mutex.
    std::mutex mutex;
    std::condition_variable progress;
    std::deque<std::pair<std::uint64_t, std::unique_ptr<State>>> starts;
    std::deque<Job> jobs;
    std::uint64_t requestedFlush = 0, completedFlush = 0, checkpointFence = 0;
    std::filesystem::path flushFolder;
    bool completedFlushReady = true, checkpointRequested = false, stopping = false;
    // Every hand-over signals it after it is made, and the worker checks for
    // work before each wait, so a signal sent while it was deciding to sleep
    // ends the wait at once. The wait has no timeout.
    dusk::AutoResetEvent wake;
    std::thread worker;
};

Journal::Journal (Writer::Report value, std::size_t size) : report (std::move (value)), capacity (size) {}
Journal::~Journal() = default;

Journal::Handle Journal::begin (Config config) noexcept
{
    if (config.sessionDir.empty()) return {};
    try
    {
        if (! impl) impl = std::make_unique<Impl> (report, capacity, idleHook);
        auto state = std::make_unique<Impl::State> (std::move (config));
        const auto id = impl->nextId++;
        impl->producers.emplace (id, Impl::Producer {});
        try
        {
            const std::lock_guard<std::mutex> lock (impl->mutex);
            impl->starts.emplace_back (id, std::move (state));
        }
        catch (...)
        {
            impl->producers.erase (id);
            throw;
        }
        impl->wake.signal();
        return { id };
    }
    catch (...) { return {}; }
}

bool Journal::append (const Handle& handle, const MidiCaptureStream::DrainView& drain) noexcept
{
    return impl && handle.id != 0 && impl->append (handle.id, drain);
}

void Journal::checkpoint() noexcept
{
    if (! impl) return;
    impl->sendHeld();
    {
        const std::lock_guard<std::mutex> lock (impl->mutex);
        impl->checkpointRequested = true;
        impl->checkpointFence = impl->batchWrite.load (std::memory_order_relaxed);
    }
    impl->wake.signal();
}

bool Journal::finish (const Handle& handle, int passOrdinal, std::int64_t endSample) noexcept
{
    if (! impl || handle.id == 0) return false;
    const auto found = impl->producers.find (handle.id);
    if (found == impl->producers.end()) return false;
    try
    {
        Impl::Job job;
        job.batchFence = impl->batchWrite.load (std::memory_order_relaxed);
        job.id = handle.id;
        job.passOrdinal = passOrdinal;
        job.endSample = endSample;
        job.finish = true;
        job.producer = found->second;
        {
            const std::lock_guard<std::mutex> lock (impl->mutex);
            impl->jobs.push_back (std::move (job));
        }
        impl->producers.erase (found);
        impl->wake.signal();
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
        job.batchFence = impl->batchWrite.load (std::memory_order_relaxed);
        job.id = id;
        job.passOrdinal = passOrdinal;
        job.file = file;
        {
            const std::lock_guard<std::mutex> lock (impl->mutex);
            impl->jobs.push_back (std::move (job));
        }
        impl->wake.signal();
        return true;
    }
    catch (...) { return false; }
}

bool Journal::flush (const std::filesystem::path& folder)
{
    if (! impl) return true;
    impl->sendHeld();
    std::unique_lock<std::mutex> lock (impl->mutex);
    impl->flushFolder = folder;
    const auto target = ++impl->requestedFlush;
    lock.unlock();
    impl->wake.signal();
    lock.lock();
    impl->progress.wait (lock, [&] { return impl->completedFlush >= target; });
    return impl->completedFlushReady;
}
} // namespace duskstudio::midirecovery
