#include <catch2/catch_test_macros.hpp>

#include "foundation/UndoManager.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace
{
struct Log
{
    std::vector<std::string> calls;
    std::set<const dusk::UndoableAction*> alive;
    int value = 0;
};

struct Step final : dusk::UndoableAction
{
    Step (Log& l, std::string stepName, int stepDelta, int units = 1)
        : log (l), name (std::move (stepName)), delta (stepDelta), size (units) { log.alive.insert (this); }
    ~Step() override { log.alive.erase (this); }

    bool perform() override
    {
        log.calls.push_back ("perform " + name);
        if (! performs) return false;
        log.value += delta;
        return true;
    }

    bool undo() override
    {
        log.calls.push_back ("undo " + name);
        if (onUndo) onUndo();
        if (! undoes) return false;
        log.value -= delta;
        return true;
    }

    int getSizeInUnits() override { return size; }

    dusk::UndoableAction* createCoalescedAction (dusk::UndoableAction* next) override
    {
        auto* other = dynamic_cast<Step*> (next);
        if (! merges || other == nullptr || ! other->merges) return nullptr;
        auto* merged = new Step (log, name + other->name, delta + other->delta, size + other->size);
        merged->merges = true;
        return merged;
    }

    Log& log;
    std::string name;
    int delta, size;
    bool performs = true, undoes = true, merges = false;
    std::function<void()> onUndo;
};

Step* failing (Log& log, std::string name)
{
    auto* step = new Step (log, std::move (name), 1);
    step->performs = false;
    return step;
}
} // namespace

TEST_CASE ("dusk::UndoManager replays a transaction as one step", "[foundation][undo]")
{
    Log log;
    dusk::UndoManager undo;
    REQUIRE_FALSE (undo.canUndo());
    REQUIRE_FALSE (undo.undo());

    undo.beginNewTransaction ("Edit");
    REQUIRE (undo.perform (new Step (log, "a", 1)));
    REQUIRE (undo.perform (new Step (log, "b", 10)));
    undo.beginNewTransaction();
    REQUIRE (undo.perform (new Step (log, "c", 100), "Move"));
    CHECK (log.value == 111);
    CHECK (undo.getUndoDescriptions() == std::vector<std::string> { "Move", "Edit" });
    REQUIRE (undo.getUndoActions().size() == 1);

    log.calls.clear();
    REQUIRE (undo.undo());
    REQUIRE (undo.getUndoActions().size() == 2);
    REQUIRE (undo.undo());
    CHECK (log.calls == std::vector<std::string> { "undo c", "undo b", "undo a" });
    CHECK (log.value == 0);
    CHECK_FALSE (undo.canUndo());
    CHECK (undo.getRedoDescription() == "Edit");
    CHECK (undo.getRedoDescriptions() == std::vector<std::string> { "Edit", "Move" });

    log.calls.clear();
    REQUIRE (undo.redo());
    CHECK (log.calls == std::vector<std::string> { "perform a", "perform b" });
    CHECK (undo.getUndoDescription() == "Edit");
    CHECK (undo.getRedoActions().size() == 1);
}

TEST_CASE ("dusk::UndoManager deletes what it refuses or discards", "[foundation][undo]")
{
    Log log;
    dusk::UndoManager undo;

    SECTION ("a failed perform leaves the history alone")
    {
        REQUIRE (undo.perform (new Step (log, "a", 1)));
        REQUIRE_FALSE (undo.perform (failing (log, "f")));
        CHECK (log.alive.size() == 1);
        CHECK (undo.getUndoActions().size() == 1);
        CHECK_FALSE (undo.perform (nullptr));
    }
    SECTION ("perform from inside an undo is refused")
    {
        auto* step = new Step (log, "a", 1);
        bool accepted = true;
        step->onUndo = [&] { accepted = undo.perform (new Step (log, "inner", 5)); };
        REQUIRE (undo.perform (step));
        REQUIRE (undo.undo());
        CHECK_FALSE (accepted);
        CHECK (log.value == 0);
        CHECK (log.alive.size() == 1);
        REQUIRE (undo.perform (new Step (log, "after", 2)));
    }
    SECTION ("a new perform destroys the redo branch")
    {
        undo.beginNewTransaction ("One");
        REQUIRE (undo.perform (new Step (log, "a", 1)));
        undo.beginNewTransaction ("Two");
        REQUIRE (undo.perform (new Step (log, "b", 2, 3)));
        REQUIRE (undo.undo());
        CHECK (undo.getNumberOfUnitsTakenUpByStoredCommands() == 4);
        REQUIRE (undo.perform (new Step (log, "c", 4)));
        CHECK_FALSE (undo.canRedo());
        CHECK (log.alive.size() == 2);
        CHECK (undo.getNumberOfUnitsTakenUpByStoredCommands() == 2);
    }
    SECTION ("a refused undo clears the history and still reports a replay")
    {
        REQUIRE (undo.perform (new Step (log, "a", 1)));
        undo.beginNewTransaction();
        auto* stuck = new Step (log, "b", 2);
        stuck->undoes = false;
        REQUIRE (undo.perform (stuck));
        REQUIRE (undo.perform (new Step (log, "c", 4)));
        REQUIRE (undo.undo());
        CHECK (log.value == 3);
        CHECK_FALSE (undo.canUndo());
        CHECK_FALSE (undo.canRedo());
        CHECK (log.alive.empty());
        CHECK (undo.getNumberOfUnitsTakenUpByStoredCommands() == 0);
    }
    SECTION ("clear deletes every action")
    {
        REQUIRE (undo.perform (new Step (log, "a", 1)));
        REQUIRE (undo.undo());
        undo.clearUndoHistory();
        CHECK (log.alive.empty());
        CHECK_FALSE (undo.canRedo());
    }
}

TEST_CASE ("dusk::UndoManager trims by units but keeps the minimum transactions", "[foundation][undo]")
{
    Log log;
    dusk::UndoManager undo;
    undo.setMaxNumberOfStoredUnits (4, 2);
    for (int i = 0; i < 5; ++i)
    {
        undo.beginNewTransaction ("t" + std::to_string (i));
        REQUIRE (undo.perform (new Step (log, std::to_string (i), 1, 3)));
    }
    CHECK (undo.getUndoDescriptions() == std::vector<std::string> { "t4", "t3" });
    CHECK (undo.getNumberOfUnitsTakenUpByStoredCommands() == 6);
    CHECK (log.alive.size() == 2);

    undo.setMaxNumberOfStoredUnits (0, 0);
    undo.beginNewTransaction ("t5");
    REQUIRE (undo.perform (new Step (log, "5", 1, 3)));
    CHECK (undo.getUndoDescriptions() == std::vector<std::string> { "t5" });
}

TEST_CASE ("dusk::UndoManager coalesces into the open transaction", "[foundation][undo]")
{
    Log log;
    dusk::UndoManager undo;
    const auto merging = [&log] (std::string name, int delta)
    {
        auto* step = new Step (log, std::move (name), delta, 2);
        step->merges = true;
        return step;
    };
    undo.beginNewTransaction ("Drag");
    REQUIRE (undo.perform (merging ("a", 1)));
    REQUIRE (undo.perform (merging ("b", 2)));
    CHECK (log.alive.size() == 1);
    CHECK (undo.getNumberOfUnitsTakenUpByStoredCommands() == 4);
    undo.beginNewTransaction ("Drag");
    REQUIRE (undo.perform (merging ("c", 4)));
    CHECK (log.alive.size() == 2);
    REQUIRE (undo.undo());
    REQUIRE (undo.undo());
    CHECK (log.value == 0);
    REQUIRE (undo.redo());
    CHECK (log.value == 3);
}

TEST_CASE ("dusk::UndoManager change notification is posted once and flushable", "[foundation][undo][events]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Log log;
    int first = 0, second = 0;
    auto undo = std::make_unique<dusk::UndoManager>();

    REQUIRE (undo->perform (new Step (log, "unheard", 1)));
    undo->addChangeListener (&first, [&first] { ++first; });
    undo->dispatchPendingMessages();
    CHECK (first == 0);

    REQUIRE (undo->perform (new Step (log, "a", 1)));
    REQUIRE (undo->undo());
    CHECK (first == 0);
    undo->dispatchPendingMessages();
    CHECK (first == 1);
    undo->dispatchPendingMessages();
    CHECK (first == 1);

    undo->removeChangeListener (&first);
    undo->addChangeListener (&second, [&] { ++second; undo->removeChangeListener (&first); });
    undo->addChangeListener (&first, [&first] { ++first; });
    REQUIRE (undo->redo());
    undo->dispatchPendingMessages();
    CHECK (second == 1);
    CHECK (first == 1);

#if ! defined (__APPLE__)
    struct Deadline final : juce::Timer
    {
        void timerCallback() override { stopTimer(); juce::MessageManager::getInstance()->stopDispatchLoop(); }
    };

    int orphaned = 0;
    auto doomed = std::make_unique<dusk::UndoManager>();
    doomed->addChangeListener (&orphaned, [&orphaned] { ++orphaned; });
    REQUIRE (doomed->perform (new Step (log, "doomed", 1)));
    doomed.reset();

    undo->removeChangeListener (&second);
    undo->addChangeListener (&first, [&first] { ++first; juce::MessageManager::getInstance()->stopDispatchLoop(); });
    undo->clearUndoHistory();
    CHECK (first == 1);
    Deadline deadline;
    deadline.startTimer (5000);
    juce::MessageManager::getInstance()->runDispatchLoop();
    CHECK (first == 2);
    CHECK (orphaned == 0);
#endif
}
