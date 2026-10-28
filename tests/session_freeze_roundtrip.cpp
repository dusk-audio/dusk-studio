#include <catch2/catch_test_macros.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"
#include "engine/AudioEngine.h"

#include <juce_core/juce_core.h>
#include <set>
#include <type_traits>

static_assert (! std::is_convertible_v<duskstudio::SessionUndoManager*, juce::UndoManager*>);
static_assert (std::is_same_v<decltype (std::declval<duskstudio::AudioEngine&>().getUndoManager()),
                              duskstudio::SessionUndoManager&>);

namespace
{
juce::File makeTempSessionDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-freeze-roundtrip-"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    // Fail at the source if setup can't create the dirs, not later in the serializer.
    REQUIRE (dir.createDirectory().wasOk());
    REQUIRE (dir.getChildFile ("audio").createDirectory().wasOk());
    return dir;
}
} // namespace

namespace
{
struct HistoryAction final : duskstudio::UndoableAction
{
    HistoryAction (int& v, int b, int a, std::set<const duskstudio::UndoableAction*>& live,
                    bool* locked = nullptr, bool merge = false, bool succeeds = true)
        : value (v), before (b), after (a), alive (live), frozen (locked),
          coalesce (merge), success (succeeds) { alive.insert (this); }
    ~HistoryAction() override { alive.erase (this); }
    bool perform() override { if (success) value = after; return success; }
    bool undo() override { if (undoSuccess) value = before; return undoSuccess; }
    int getSizeInUnits() override { return 1; }
    duskstudio::UndoableAction* createCoalescedAction (duskstudio::UndoableAction* next) override
    {
        const auto* edit = dynamic_cast<HistoryAction*> (next);
        return coalesce && edit != nullptr && edit->coalesce
            ? new HistoryAction (value, before, edit->after, alive, frozen, true) : nullptr;
    }
    int& value;
    int before, after;
    std::set<const duskstudio::UndoableAction*>& alive;
    bool* frozen;
    bool coalesce, success;
    bool undoSuccess = true;
};
} // namespace

TEST_CASE ("Session undo preflights complete transactions and keeps unrelated history usable",
           "[session][undo][freeze]")
{
    std::set<const duskstudio::UndoableAction*> alive;
    bool frozen = false;
    int first = 0, second = 0;
    duskstudio::SessionUndoManager undo ([&alive] (const auto& actions, bool)
    {
        bool allowed = true;
        for (const auto* action : actions)
        {
            REQUIRE (alive.count (action) == 1);
            const auto* edit = dynamic_cast<const HistoryAction*> (action);
            REQUIRE (edit != nullptr);
            allowed = allowed && (edit->frozen == nullptr || ! *edit->frozen);
        }
        return allowed;
    });
    SECTION ("a frozen participant blocks both directions before any action runs")
    {
        undo.beginNewTransaction ("Edit");
        REQUIRE (undo.perform (new HistoryAction (first, 0, 1, alive, &frozen)));
        REQUIRE (undo.perform (new HistoryAction (second, 0, 2, alive)));
        undo.beginNewTransaction();
        frozen = true;
        REQUIRE_FALSE (undo.undo());
        CHECK (first == 1);
        CHECK (second == 2);
        CHECK (undo.getUndoDescription() == "Edit");
        CHECK_FALSE (undo.canRedo());
        frozen = false;
        REQUIRE (undo.undo());
        CHECK (first == 0);
        CHECK (second == 0);
        frozen = true;
        REQUIRE_FALSE (undo.redo());
        CHECK (first == 0);
        CHECK (second == 0);
        CHECK (undo.getRedoDescription() == "Edit");
        frozen = false;
        REQUIRE (undo.redo());
        CHECK (first == 1);
        CHECK (second == 2);
    }
    SECTION ("duplicate names and unrelated intervening transactions keep their identities")
    {
        undo.beginNewTransaction ("Edit");
        REQUIRE (undo.perform (new HistoryAction (first, 0, 1, alive, &frozen)));
        undo.beginNewTransaction ("Edit");
        REQUIRE (undo.perform (new HistoryAction (second, 0, 2, alive), "Edit"));
        frozen = true;
        REQUIRE (undo.undo());
        CHECK (first == 1);
        CHECK (second == 0);
        REQUIRE_FALSE (undo.undo());
        REQUIRE (undo.redo());
        CHECK (second == 2);
        frozen = false;
        REQUIRE (undo.undo());
        REQUIRE (undo.undo());
        CHECK (first == 0);
    }
}

TEST_CASE ("Session undo snapshots follow pruning coalescing and redo branches",
           "[session][undo][freeze]")
{
    std::set<const duskstudio::UndoableAction*> alive;
    bool frozen = false;
    int value = 0;
    duskstudio::SessionUndoManager undo ([&alive] (const auto& actions, bool)
    {
        bool allowed = true;
        for (const auto* action : actions)
        {
            REQUIRE (alive.count (action) == 1);
            const auto* edit = dynamic_cast<const HistoryAction*> (action);
            REQUIRE (edit != nullptr);
            allowed = allowed && (edit->frozen == nullptr || ! *edit->frozen);
        }
        return allowed;
    });
    SECTION ("pruning retires the oldest action metadata")
    {
        undo.setMaxNumberOfStoredUnits (1, 1);
        undo.beginNewTransaction ("Old");
        REQUIRE (undo.perform (new HistoryAction (value, 0, 1, alive, &frozen)));
        undo.beginNewTransaction ("Retained");
        REQUIRE (undo.perform (new HistoryAction (value, 1, 2, alive)));
        REQUIRE (alive.size() == 1);
        frozen = true;
        REQUIRE (undo.undo());
        CHECK (value == 1);
        CHECK_FALSE (undo.canUndo());
        REQUIRE (undo.redo());
        CHECK (value == 2);
    }
    SECTION ("coalescing replaces deleted action pointers in the transaction snapshot")
    {
        undo.beginNewTransaction ("Gesture");
        REQUIRE (undo.perform (new HistoryAction (value, 0, 1, alive, &frozen, true)));
        REQUIRE (undo.perform (new HistoryAction (value, 1, 2, alive, &frozen, true)));
        REQUIRE (alive.size() == 1);
        frozen = true;
        REQUIRE_FALSE (undo.undo());
        CHECK (value == 2);
        frozen = false;
        REQUIRE (undo.undo());
        CHECK (value == 0);
        REQUIRE (undo.redo());
        CHECK (value == 2);
    }
    SECTION ("a fresh branch invalidates redo even when the retired target is frozen")
    {
        undo.beginNewTransaction ("First");
        REQUIRE (undo.perform (new HistoryAction (value, 0, 1, alive)));
        undo.beginNewTransaction ("Retired");
        REQUIRE (undo.perform (new HistoryAction (value, 1, 2, alive, &frozen)));
        REQUIRE (undo.undo());
        undo.beginNewTransaction ("Branch");
        REQUIRE (undo.perform (new HistoryAction (value, 1, 3, alive)));
        CHECK_FALSE (undo.canRedo());
        frozen = true;
        REQUIRE (undo.undo());
        CHECK (value == 1);
        REQUIRE (undo.redo());
        CHECK (value == 3);
    }
    SECTION ("clearing history after branching cannot revive stashed action metadata")
    {
        REQUIRE (undo.perform (new HistoryAction (value, 0, 1, alive, &frozen)));
        REQUIRE (undo.undo());
        REQUIRE (undo.perform (new HistoryAction (value, 0, 2, alive)));
        undo.clearUndoHistory();
        frozen = true;
        REQUIRE (undo.perform (new HistoryAction (value, 2, 3, alive)));
        REQUIRE (undo.undo());
        CHECK (value == 2);
        REQUIRE (undo.redo());
        CHECK (value == 3);
    }
    SECTION ("repeated branches and pruning preserve only the visible redo transaction")
    {
        undo.setMaxNumberOfStoredUnits (1, 1);
        REQUIRE (undo.perform (new HistoryAction (value, 0, 1, alive, &frozen)));
        REQUIRE (undo.undo());
        REQUIRE (undo.perform (new HistoryAction (value, 0, 2, alive)));
        REQUIRE (undo.undo());
        REQUIRE (undo.perform (new HistoryAction (value, 0, 3, alive)));
        undo.beginNewTransaction();
        REQUIRE (undo.perform (new HistoryAction (value, 3, 4, alive)));
        frozen = true;
        REQUIRE (undo.undo());
        CHECK (value == 3);
        CHECK_FALSE (undo.canUndo());
        REQUIRE (undo.redo());
        CHECK (value == 4);
    }
    SECTION ("failed perform and clear cannot replace metadata with stale actions")
    {
        undo.beginNewTransaction ("First");
        REQUIRE (undo.perform (new HistoryAction (value, 0, 1, alive, &frozen)));
        REQUIRE_FALSE (undo.perform (new HistoryAction (value, 1, 2, alive, nullptr, false, false)));
        REQUIRE (alive.size() == 1);
        undo.clearUndoHistory();
        REQUIRE (alive.empty());
        undo.beginNewTransaction ("Replacement");
        REQUIRE (undo.perform (new HistoryAction (value, 1, 3, alive)));
        frozen = true;
        REQUIRE (undo.undo());
        CHECK (value == 1);
        CHECK_FALSE (undo.canUndo());
    }
    SECTION ("an action refusing actual undo permits subsequent fresh edits")
    {
        for (const bool multiple : { false, true })
        {
            undo.clearUndoHistory();
            value = 0;
            if (multiple)
            {
                REQUIRE (undo.perform (new HistoryAction (value, 0, 10, alive)));
                undo.beginNewTransaction();
            }
            auto* action = new HistoryAction (value, value, 1, alive);
            action->undoSuccess = false;
            REQUIRE (undo.perform (action));
            if (multiple) REQUIRE (undo.perform (new HistoryAction (value, 1, 2, alive)));
            REQUIRE_FALSE (undo.undo());
            CHECK_FALSE (undo.canUndo());
            CHECK_FALSE (undo.canRedo());
            REQUIRE (alive.empty());
            REQUIRE (undo.perform (new HistoryAction (value, 1, 3, alive)));
            REQUIRE (undo.undo());
            CHECK (value == 1);
        }
    }
    SECTION ("an action refusing actual redo permits subsequent fresh edits")
    {
        for (const bool multiple : { false, true })
        {
            undo.clearUndoHistory();
            value = 0;
            if (multiple) REQUIRE (undo.perform (new HistoryAction (value, 0, 1, alive)));
            auto* action = new HistoryAction (value, value, 2, alive);
            REQUIRE (undo.perform (action));
            REQUIRE (undo.undo());
            action->success = false;
            REQUIRE_FALSE (undo.redo());
            CHECK_FALSE (undo.canUndo());
            CHECK_FALSE (undo.canRedo());
            REQUIRE (alive.empty());
            const int before = value;
            REQUIRE (undo.perform (new HistoryAction (value, before, 3, alive)));
            REQUIRE (undo.undo());
            CHECK (value == before);
        }
    }

}

// Freeze persistence contract: a frozen MIDI track must reload frozen, with
// frozenRegion rebuilt (file + length + channels) so PlaybackEngine can open
// the baked WAV. The loader also has to DROP a frozen flag whose WAV is gone,
// so a moved / cleaned session falls back to the live instrument instead of
// playing silence. Mirrors the engine-side AudioEngine::freezeTrack writes
// without needing an audio device.
TEST_CASE ("SessionSerializer round-trips track freeze state", "[session][serializer][freeze]")
{
    using duskstudio::Session;
    using duskstudio::Track;
    using duskstudio::SessionSerializer;

    const auto dir    = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");
    const auto wav    = dir.getChildFile ("audio").getChildFile ("freeze_track01.wav");

    SECTION ("frozen track with a present WAV reloads frozen + region intact")
    {
        REQUIRE (wav.replaceWithText ("not really audio, but existsAsFile() is all the loader checks"));

        Session a;
        auto& t0 = a.track (0);
        t0.name = "Synth";
        t0.mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
        t0.frozen.store (true, std::memory_order_relaxed);
        t0.frozenAudioPath = wav.getFullPathName();
        t0.frozenRegion.file            = wav;
        t0.frozenRegion.lengthInSamples = 480000;
        t0.frozenRegion.numChannels     = 2;

        REQUIRE (SessionSerializer::save (a, target));

        Session b;
        REQUIRE (SessionSerializer::load (b, target));

        auto& r = b.track (0);
        REQUIRE (r.frozen.load (std::memory_order_relaxed));
        REQUIRE (juce::File (r.frozenAudioPath) == wav);
        REQUIRE (r.frozenRegion.file == wav);
        REQUIRE (r.frozenRegion.lengthInSamples == 480000);
        REQUIRE (r.frozenRegion.numChannels == 2);
        REQUIRE (r.frozenRegion.timelineStart == 0);
    }

    SECTION ("frozen flag is dropped when the baked WAV is missing")
    {
        // Path recorded, file never created (simulates a moved / cleaned session).
        Session a;
        auto& t0 = a.track (0);
        t0.mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
        t0.frozen.store (true, std::memory_order_relaxed);
        t0.frozenAudioPath = wav.getFullPathName();
        t0.frozenRegion.file            = wav;
        t0.frozenRegion.lengthInSamples = 480000;
        t0.frozenRegion.numChannels     = 2;

        REQUIRE (SessionSerializer::save (a, target));
        REQUIRE_FALSE (wav.existsAsFile());

        Session b;
        REQUIRE (SessionSerializer::load (b, target));
        auto& r = b.track (0);
        REQUIRE_FALSE (r.frozen.load (std::memory_order_relaxed));
        // Dropping the flag isn't enough — the stale baked-WAV payload must be reset too,
        // or a later re-freeze / save could resurrect the missing file.
        REQUIRE (r.frozenAudioPath.isEmpty());
        REQUIRE (r.frozenRegion.file == juce::File());
        REQUIRE (r.frozenRegion.lengthInSamples == 0);
    }

    dir.deleteRecursively();
}
