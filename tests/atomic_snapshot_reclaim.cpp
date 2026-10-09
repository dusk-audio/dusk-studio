#include <catch2/catch_test_macros.hpp>

#include "session/AtomicSnapshot.h"
#include "session/Session.h"

#include <memory>
#include <optional>
#include <vector>

// A published value is freed once no read scope can still hold it: at once
// while none is open, as with no device running, and otherwise once every scope
// open when it was retired has closed, however many publishes came between.

using duskstudio::AtomicSnapshot;
using duskstudio::MidiRegion;
using duskstudio::MidiRegionSnapshot;
using duskstudio::SnapshotReadScope;

namespace
{
void publish (AtomicSnapshot<std::vector<int>>& snapshot, int value)
{
    snapshot.publish (std::make_unique<std::vector<int>> (std::vector<int> (64, value)));
}
} // namespace

TEST_CASE ("AtomicSnapshot frees a retired value at once when no reader is open", "[snapshot][reclaim]")
{
    AtomicSnapshot<std::vector<int>> snapshot;
    for (int i = 0; i < 100; ++i)
    {
        publish (snapshot, i);
        REQUIRE (snapshot.retiredCount() == 0);
    }
    CHECK (snapshot.read()->front() == 99);
}

TEST_CASE ("AtomicSnapshot keeps what an open reader read, however fast it publishes", "[snapshot][reclaim]")
{
    AtomicSnapshot<std::vector<int>> snapshot;
    publish (snapshot, 1);

    std::optional<SnapshotReadScope> callback;
    callback.emplace();
    const auto* held = snapshot.read();
    const auto* second = held;

    // Many publishes inside one block, with a second read part-way.
    for (int i = 2; i <= 50; ++i)
    {
        publish (snapshot, i);
        if (i == 25) second = snapshot.read();
    }
    CHECK (snapshot.retiredCount() == 49);
    CHECK (held->size() == 64);
    CHECK (held->front() == 1);
    CHECK (second->front() == 25);
    CHECK (snapshot.read()->front() == 50);

    callback.reset();
    snapshot.reclaim();
    CHECK (snapshot.retiredCount() == 0);
    CHECK (snapshot.read()->front() == 50);
}

TEST_CASE ("AtomicSnapshot holds back only what was retired before the oldest reader opened",
           "[snapshot][reclaim]")
{
    AtomicSnapshot<std::vector<int>> snapshot;
    publish (snapshot, 1);

    std::optional<SnapshotReadScope> first;
    first.emplace();
    publish (snapshot, 2);   // retires 1 under the first reader
    REQUIRE (snapshot.retiredCount() == 1);

    // A second reader opens before the first closes, and finds 2 published.
    SnapshotReadScope second;
    first.reset();
    publish (snapshot, 3);   // retires 2 under the second reader

    // 1 was retired before the second reader opened, so only 2 waits.
    CHECK (snapshot.retiredCount() == 1);
    CHECK (snapshot.read()->front() == 3);
}

TEST_CASE ("AtomicSnapshot: one reader holds every snapshot it may have read", "[snapshot][reclaim]")
{
    AtomicSnapshot<std::vector<int>> a, b;
    {
        const SnapshotReadScope callback;
        const auto* fromA = a.read();
        const auto* fromB = b.read();
        publish (a, 1);
        publish (b, 2);
        CHECK (a.retiredCount() == 1);
        CHECK (b.retiredCount() == 1);
        CHECK (fromA->empty());
        CHECK (fromB->empty());
    }
    a.reclaim();
    b.reclaim();
    CHECK (a.retiredCount() == 0);
    CHECK (b.retiredCount() == 0);
}

TEST_CASE ("AtomicSnapshot frees nothing while more readers are open than it has slots",
           "[snapshot][reclaim]")
{
    AtomicSnapshot<std::vector<int>> snapshot;
    std::vector<std::unique_ptr<SnapshotReadScope>> readers;
    for (int i = 0; i < duskstudio::snapshot_epoch::kReaderSlots + 1; ++i)
        readers.push_back (std::make_unique<SnapshotReadScope>());

    const auto* held = snapshot.read();
    publish (snapshot, 1);
    publish (snapshot, 2);
    CHECK (snapshot.retiredCount() == 2);
    CHECK (held->empty());

    // The reader without a slot is the newest; closing every slotted one
    // still leaves it holding everything back.
    readers.erase (readers.begin(), readers.end() - 1);
    snapshot.reclaim();
    CHECK (snapshot.retiredCount() == 2);

    readers.clear();
    snapshot.reclaim();
    CHECK (snapshot.retiredCount() == 0);
}

TEST_CASE ("MidiRegionSnapshot: the audio side never reads the regions the message thread edits",
           "[snapshot][midi]")
{
    MidiRegionSnapshot snapshot;
    REQUIRE (snapshot.read() != nullptr);
    CHECK (snapshot.read()->size() == 0);

    MidiRegion region;
    region.timelineStart = 1000;
    region.lengthInSamples = 4800;
    region.lengthInTicks = 48;
    region.notes.push_back ({ 1, 60, 100, 0, 10 });
    snapshot.publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { region, region }));
    const auto* timeline = snapshot.read();
    REQUIRE (timeline->size() == 2);
    CHECK (timeline->placement[1].load().timelineStart == 1000);

    // Edits to the message thread's regions reach the timeline only when
    // handed over: placements through editedInPlace(), the rest by a publish.
    auto& regions = snapshot.currentMutable();
    regions[1].timelineStart = 9000;
    regions[1].lengthInSamples = 2400;
    regions[1].muted = true;
    regions[1].notes.clear();
    CHECK (timeline->placement[1].load().timelineStart == 1000);
    CHECK (timeline->playback[1].notes.size() == 1);

    snapshot.editedInPlace();
    const auto moved = timeline->placement[1].load();
    CHECK (moved.timelineStart == 9000);
    CHECK (moved.lengthInSamples == 2400);
    CHECK (moved.muted);
    CHECK (timeline->placement[0].load().timelineStart == 1000);
    CHECK (timeline->playback[1].notes.size() == 1);
    CHECK (snapshot.read() == timeline);

    snapshot.mutate ([] (std::vector<MidiRegion>&) {});
    REQUIRE (snapshot.read() != timeline);
    CHECK (snapshot.read()->playback[1].notes.empty());
    CHECK (snapshot.read()->placement[1].load().timelineStart == 9000);
    CHECK (snapshot.generation() == 2);
}
