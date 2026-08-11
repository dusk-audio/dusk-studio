#include <catch2/catch_test_macros.hpp>

#include "engine/sfz/SfzInstallWorker.h"
#include "sfz_pack_fixture.h"

#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace
{
using duskstudio::sfz::InstallActivityGate;
using duskstudio::sfz::InstallJobProgress;
using duskstudio::sfz::InstallJobState;
using duskstudio::sfz::InstallStatus;
using duskstudio::sfz::InstallWorker;
using sfzfixture::PackFixture;

// Every wait in these tests is on a state the worker actually published, never
// on elapsed time.
struct StateRecorder
{
    void record (const InstallJobProgress& progress)
    {
        {
            std::lock_guard<std::mutex> held (lock);
            records.push_back (progress);
        }
        changed.notify_all();
    }

    template <typename Predicate>
    std::vector<InstallJobProgress> waitFor (Predicate predicate)
    {
        std::unique_lock<std::mutex> held (lock);
        changed.wait (held, [this, &predicate] { return predicate (records); });
        return records;
    }

    std::vector<InstallJobProgress> snapshot()
    {
        std::lock_guard<std::mutex> held (lock);
        return records;
    }

    std::mutex lock;
    std::condition_variable changed;
    std::vector<InstallJobProgress> records;
};

auto finishedCount (std::size_t expected)
{
    return [expected] (const std::vector<InstallJobProgress>& records)
    {
        std::size_t finished = 0;
        for (const auto& record : records)
            if (record.state == InstallJobState::finished)
                ++finished;
        return finished >= expected;
    };
}

auto sawState (InstallJobState state)
{
    return [state] (const std::vector<InstallJobProgress>& records)
    {
        for (const auto& record : records)
            if (record.state == state)
                return true;
        return false;
    };
}

struct FakeGate final : InstallActivityGate
{
    bool shouldPauseBackgroundWork() override
    {
        std::lock_guard<std::mutex> held (lock);
        return closed;
    }

    void open()
    {
        std::lock_guard<std::mutex> held (lock);
        closed = false;
    }

    std::mutex lock;
    bool closed { true };
};

const InstallJobProgress& finalRecord (const std::vector<InstallJobProgress>& records)
{
    for (auto it = records.rbegin(); it != records.rend(); ++it)
        if (it->state == InstallJobState::finished)
            return *it;
    throw std::runtime_error ("no finished record");
}
} // namespace

TEST_CASE ("SFZ install worker installs a queued pack", "[sfz][worker]")
{
    PackFixture fixture;
    StateRecorder recorder;

    InstallWorker worker (fixture.transport, fixture.layout);
    worker.setListener ([&recorder] (const InstallJobProgress& progress)
                        { recorder.record (progress); });

    const auto id = worker.enqueue (fixture.pack());
    const auto records = recorder.waitFor (finishedCount (1));

    const auto& completed = finalRecord (records);
    CHECK (completed.id == id);
    CHECK (completed.packId == "pack");
    CHECK (completed.status == InstallStatus::installed);
    CHECK (std::filesystem::is_regular_file (
        fixture.layout.packDirectory ("pack", "v1.0.0") / "Kit.sfz"));
    CHECK (worker.snapshot().empty());
}

TEST_CASE ("SFZ install worker drains the queue in order", "[sfz][worker]")
{
    PackFixture fixture;
    StateRecorder recorder;

    auto first = fixture.pack();
    auto second = first;
    second.releaseId = "v1.1.0";

    InstallWorker worker (fixture.transport, fixture.layout);
    worker.setListener ([&recorder] (const InstallJobProgress& progress)
                        { recorder.record (progress); });

    const auto firstId = worker.enqueue (first);
    const auto secondId = worker.enqueue (second);
    const auto records = recorder.waitFor (finishedCount (2));

    std::vector<std::uint64_t> finishOrder;
    for (const auto& record : records)
        if (record.state == InstallJobState::finished)
        {
            finishOrder.push_back (record.id);
            CHECK (record.status == InstallStatus::installed);
        }
    CHECK (finishOrder == std::vector<std::uint64_t> { firstId, secondId });
}

TEST_CASE ("SFZ install worker holds work while the studio is busy", "[sfz][worker]")
{
    PackFixture fixture;
    StateRecorder recorder;
    FakeGate gate;

    InstallWorker worker (fixture.transport, fixture.layout, {}, &gate);
    worker.setListener ([&recorder] (const InstallJobProgress& progress)
                        { recorder.record (progress); });

    worker.enqueue (fixture.pack());
    recorder.waitFor (sawState (InstallJobState::paused));

    // The gate is checked before the transfer starts, so nothing has been
    // fetched while the job sits paused.
    CHECK (fixture.transport.attempts == 0);
    CHECK_FALSE (std::filesystem::exists (fixture.layout.packsDirectory()));

    gate.open();
    worker.notifyActivityChanged();

    const auto records = recorder.waitFor (finishedCount (1));
    CHECK (finalRecord (records).status == InstallStatus::installed);
}

TEST_CASE ("SFZ install worker cancels a job that has not started", "[sfz][worker]")
{
    PackFixture fixture;
    StateRecorder recorder;
    FakeGate gate;

    InstallWorker worker (fixture.transport, fixture.layout, {}, &gate);
    worker.setListener ([&recorder] (const InstallJobProgress& progress)
                        { recorder.record (progress); });

    const auto id = worker.enqueue (fixture.pack());
    recorder.waitFor (sawState (InstallJobState::paused));

    worker.cancel (id);
    const auto records = recorder.waitFor (finishedCount (1));

    CHECK (finalRecord (records).status == InstallStatus::cancelled);
    CHECK (fixture.transport.attempts == 0);
    CHECK_FALSE (std::filesystem::exists (fixture.layout.packsDirectory()));
}

TEST_CASE ("SFZ install worker cancels a transfer in flight", "[sfz][worker]")
{
    PackFixture fixture;
    StateRecorder recorder;

    std::mutex lock;
    std::condition_variable started;
    bool transferring = false;

    fixture.transport.chunkBytes = 8;
    fixture.transport.onChunkDelivered = [&] (std::uint64_t)
    {
        {
            std::lock_guard<std::mutex> held (lock);
            transferring = true;
        }
        started.notify_all();
    };

    InstallWorker worker (fixture.transport, fixture.layout);
    worker.setListener ([&recorder] (const InstallJobProgress& progress)
                        { recorder.record (progress); });

    const auto id = worker.enqueue (fixture.pack());
    {
        std::unique_lock<std::mutex> held (lock);
        started.wait (held, [&transferring] { return transferring; });
    }

    worker.cancel (id);
    const auto records = recorder.waitFor (finishedCount (1));

    const auto& completed = finalRecord (records);
    // The transfer may have finished before the cancel landed; either way the
    // job must reach a final state and never publish a half-written pack.
    CHECK ((completed.status == InstallStatus::cancelled
            || completed.status == InstallStatus::installed));
    CHECK (fixture.stagingIsClean());
}

TEST_CASE ("SFZ install worker abandons its queue on shutdown", "[sfz][worker]")
{
    PackFixture fixture;
    FakeGate gate;

    {
        InstallWorker worker (fixture.transport, fixture.layout, {}, &gate);
        worker.enqueue (fixture.pack());
    }

    CHECK_FALSE (std::filesystem::exists (fixture.layout.packsDirectory()));
}
