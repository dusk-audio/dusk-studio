#include <catch2/catch_test_macros.hpp>

#include "foundation/UndoManager.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <random>
#include <string>
#include <vector>

// Drives one script through JUCE's UndoManager and dusk::UndoManager and
// compares everything a caller can observe after every step: the actions'
// perform/undo calls in order, the replayed value, return codes, history
// queries, stored units and the change notifications dispatchPendingMessages
// delivers. The GUI initialiser gives both broadcasters a message queue to post
// into; the queue is never pumped, so only the explicit flush delivers.

namespace
{
struct ActionSpec
{
    int delta = 1;
    int size = 1;
    int failPerformOnCall = 0;
    int failUndoOnCall = 0;
    bool coalesce = false;
    bool reenter = false;
};

template <typename Side>
struct ScriptAction final : Side::Action
{
    ScriptAction (Side& owner, std::string actionId, ActionSpec actionSpec)
        : side (owner), id (std::move (actionId)), spec (actionSpec) {}

    bool perform() override
    {
        ++performCalls;
        if (spec.reenter) tryReentry ("P");
        if (performCalls == spec.failPerformOnCall) { side.trace.push_back ("p" + id + "!"); return false; }
        side.trace.push_back ("p" + id);
        side.value += spec.delta;
        return true;
    }

    bool undo() override
    {
        ++undoCalls;
        if (spec.reenter) tryReentry ("U");
        if (undoCalls == spec.failUndoOnCall) { side.trace.push_back ("u" + id + "!"); return false; }
        side.trace.push_back ("u" + id);
        side.value -= spec.delta;
        return true;
    }

    int getSizeInUnits() override { return spec.size; }

    typename Side::Action* createCoalescedAction (typename Side::Action* next) override
    {
        auto* other = dynamic_cast<ScriptAction*> (next);
        if (! spec.coalesce || other == nullptr || ! other->spec.coalesce) return nullptr;
        ActionSpec merged;
        merged.delta = spec.delta + other->spec.delta;
        merged.size = spec.size + other->spec.size;
        merged.coalesce = true;
        side.trace.push_back ("c" + id + "+" + other->id);
        return new ScriptAction (side, id + "+" + other->id, merged);
    }

    void tryReentry (const char* where)
    {
        const bool accepted = side.perform (new ScriptAction (side, id + "/inner", {}));
        side.trace.push_back (std::string ("re") + where + id + (accepted ? ":yes" : ":no"));
    }

    Side& side;
    std::string id;
    ActionSpec spec;
    int performCalls = 0;
    int undoCalls = 0;
};

std::vector<std::string> toStrings (const juce::StringArray& names)
{
    std::vector<std::string> out;
    for (const auto& name : names) out.push_back (name.toStdString());
    return out;
}

struct JuceSide final : private juce::ChangeListener
{
    using Action = juce::UndoableAction;

    JuceSide() { manager.addChangeListener (this); }
    ~JuceSide() override { manager.removeChangeListener (this); }

    bool perform (Action* action) { return manager.perform (action); }
    bool perform (Action* action, const std::string& name) { return manager.perform (action, juce::String (name)); }
    void begin() { manager.beginNewTransaction(); }
    void begin (const std::string& name) { manager.beginNewTransaction (juce::String (name)); }
    bool undo() { return manager.undo(); }
    bool redo() { return manager.redo(); }
    void clear() { manager.clearUndoHistory(); }
    void limit (int units, int transactions) { manager.setMaxNumberOfStoredUnits (units, transactions); }
    void flush() { manager.dispatchPendingMessages(); }

    bool canUndo() const { return manager.canUndo(); }
    bool canRedo() const { return manager.canRedo(); }
    std::string undoName() const { return manager.getUndoDescription().toStdString(); }
    std::string redoName() const { return manager.getRedoDescription().toStdString(); }
    std::vector<std::string> undoNames() const { return toStrings (manager.getUndoDescriptions()); }
    std::vector<std::string> redoNames() const { return toStrings (manager.getRedoDescriptions()); }
    int units() const { return manager.getNumberOfUnitsTakenUpByStoredCommands(); }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { ++notifications; }

    std::vector<std::string> trace;
    int value = 0;
    int notifications = 0;
    juce::UndoManager manager;
};

struct DuskSide
{
    using Action = dusk::UndoableAction;

    DuskSide() { manager.addChangeListener (this, [this] { ++notifications; }); }
    ~DuskSide() { manager.removeChangeListener (this); }

    bool perform (Action* action) { return manager.perform (action); }
    bool perform (Action* action, const std::string& name) { return manager.perform (action, name); }
    void begin() { manager.beginNewTransaction(); }
    void begin (const std::string& name) { manager.beginNewTransaction (name); }
    bool undo() { return manager.undo(); }
    bool redo() { return manager.redo(); }
    void clear() { manager.clearUndoHistory(); }
    void limit (int units, int transactions) { manager.setMaxNumberOfStoredUnits (units, transactions); }
    void flush() { manager.dispatchPendingMessages(); }

    bool canUndo() const { return manager.canUndo(); }
    bool canRedo() const { return manager.canRedo(); }
    std::string undoName() const { return manager.getUndoDescription(); }
    std::string redoName() const { return manager.getRedoDescription(); }
    std::vector<std::string> undoNames() const { return manager.getUndoDescriptions(); }
    std::vector<std::string> redoNames() const { return manager.getRedoDescriptions(); }
    int units() const { return manager.getNumberOfUnitsTakenUpByStoredCommands(); }

    std::vector<std::string> trace;
    int value = 0;
    int notifications = 0;
    dusk::UndoManager manager;
};

struct Pair
{
    JuceSide reference;
    DuskSide seam;

    void doPerform (const std::string& id, const ActionSpec& spec, const std::string* name = nullptr)
    {
        auto* a = new ScriptAction<JuceSide> (reference, id, spec);
        auto* b = new ScriptAction<DuskSide> (seam, id, spec);
        const bool ra = name != nullptr ? reference.perform (a, *name) : reference.perform (a);
        const bool rb = name != nullptr ? seam.perform (b, *name) : seam.perform (b);
        CHECK (ra == rb);
    }
    void doBegin() { reference.begin(); seam.begin(); }
    void doBegin (const std::string& name) { reference.begin (name); seam.begin (name); }
    void doUndo() { const bool ra = reference.undo(); const bool rb = seam.undo(); CHECK (ra == rb); }
    void doRedo() { const bool ra = reference.redo(); const bool rb = seam.redo(); CHECK (ra == rb); }
    void doClear() { reference.clear(); seam.clear(); }
    void doLimit (int units, int transactions) { reference.limit (units, transactions); seam.limit (units, transactions); }
    void doFlush() { reference.flush(); seam.flush(); }

    void compare()
    {
        CHECK (reference.trace == seam.trace);
        CHECK (reference.value == seam.value);
        CHECK (reference.canUndo() == seam.canUndo());
        CHECK (reference.canRedo() == seam.canRedo());
        CHECK (reference.undoName() == seam.undoName());
        CHECK (reference.redoName() == seam.redoName());
        CHECK (reference.undoNames() == seam.undoNames());
        CHECK (reference.redoNames() == seam.redoNames());
        CHECK (reference.units() == seam.units());
        CHECK (reference.notifications == seam.notifications);
    }
};

ActionSpec plain (int delta, int size = 1)
{
    ActionSpec spec;
    spec.delta = delta;
    spec.size = size;
    return spec;
}
} // namespace

TEST_CASE ("dusk::UndoManager matches JUCE's UndoManager on scripted edits", "[foundation][undo][parity]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Pair pair;
    const std::string edit = "Edit", move = "Move";

    SECTION ("grouping, naming and whole-transaction replay")
    {
        pair.doBegin (edit);
        pair.doPerform ("a", plain (1));
        pair.doPerform ("b", plain (10));
        pair.doBegin();
        pair.doPerform ("c", plain (100), &move);
        pair.doPerform ("d", plain (1000), &edit);
        pair.doFlush();
        pair.compare();
        pair.doUndo();
        pair.doUndo();
        pair.doUndo();
        pair.doFlush();
        pair.compare();
        pair.doRedo();
        pair.doRedo();
        pair.compare();
    }
    SECTION ("failed performs, refused undos and refused redos")
    {
        auto failing = plain (5);
        failing.failPerformOnCall = 1;
        pair.doPerform ("a", plain (1));
        pair.doPerform ("f", failing);
        pair.compare();
        auto redoFails = plain (7);
        redoFails.failPerformOnCall = 2;
        pair.doBegin();
        pair.doPerform ("r", redoFails);
        pair.doPerform ("s", plain (3));
        pair.doUndo();
        pair.doRedo();
        pair.doFlush();
        pair.compare();
        auto undoFails = plain (9);
        undoFails.failUndoOnCall = 1;
        pair.doPerform ("u", plain (2));
        pair.doBegin();
        pair.doPerform ("v", undoFails);
        pair.doPerform ("w", plain (4));
        pair.doUndo();
        pair.doFlush();
        pair.compare();
    }
    SECTION ("perform from inside a replay is refused")
    {
        auto reentrant = plain (2);
        reentrant.reenter = true;
        pair.doPerform ("x", reentrant);
        pair.doUndo();
        pair.doRedo();
        pair.compare();
    }
    SECTION ("a new perform discards redo")
    {
        pair.doPerform ("a", plain (1));
        pair.doBegin();
        pair.doPerform ("b", plain (2));
        pair.doUndo();
        pair.doBegin (move);
        pair.doPerform ("c", plain (3));
        pair.doRedo();
        pair.compare();
    }
    SECTION ("the unit ceiling trims old transactions down to the minimum")
    {
        pair.doLimit (5, 2);
        for (int i = 0; i < 6; ++i)
        {
            pair.doBegin ("t" + std::to_string (i));
            pair.doPerform ("a" + std::to_string (i), plain (1, 2));
            pair.compare();
        }
        pair.doUndo();
        pair.doUndo();
        pair.doLimit (1, 1);
        pair.doBegin();
        pair.doPerform ("z", plain (1, 3));
        pair.compare();
    }
    SECTION ("coalescing merges into the open transaction only")
    {
        auto merging = plain (1, 2);
        merging.coalesce = true;
        pair.doBegin (edit);
        pair.doPerform ("a", merging);
        pair.doPerform ("b", merging);
        pair.doPerform ("c", plain (5));
        pair.doPerform ("d", merging);
        pair.doBegin();
        pair.doPerform ("e", merging);
        pair.compare();
        pair.doUndo();
        pair.doUndo();
        pair.doRedo();
        pair.compare();
    }
    SECTION ("clear keeps the pending transaction state and notifies")
    {
        pair.doBegin (edit);
        pair.doPerform ("a", plain (1));
        pair.doClear();
        pair.doPerform ("b", plain (2));
        pair.doFlush();
        pair.compare();
        pair.doBegin (move);
        pair.doClear();
        pair.doPerform ("c", plain (3));
        pair.doFlush();
        pair.compare();
    }
}

TEST_CASE ("dusk::UndoManager matches JUCE's UndoManager on random scripts", "[foundation][undo][parity]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const std::vector<std::string> names { "", "Edit", "Move", "Edit" };

    for (const unsigned seed : { 1u, 7u, 42u, 1234u, 99991u })
    {
        INFO ("seed " << seed);
        Pair pair;
        std::mt19937 rng (seed);
        const auto roll = [&rng] (int n) { return (int) (rng() % (unsigned) n); };

        for (int step = 0; step < 1500; ++step)
        {
            INFO ("step " << step);
            const int op = roll (100);
            if (op < 50)
            {
                ActionSpec spec;
                spec.delta = 1 + roll (50);
                spec.size = roll (4);
                spec.coalesce = roll (4) == 0;
                spec.reenter = roll (25) == 0;
                const int failure = roll (20);
                if (failure == 0) spec.failPerformOnCall = 1;
                else if (failure == 1) spec.failPerformOnCall = 2;
                else if (failure == 2) spec.failUndoOnCall = 1;
                else if (failure == 3) spec.failUndoOnCall = 2;
                const auto id = std::to_string (step);
                if (roll (3) == 0)
                {
                    const auto& name = names[(std::size_t) roll ((int) names.size())];
                    pair.doPerform (id, spec, &name);
                }
                else
                {
                    pair.doPerform (id, spec);
                }
            }
            else if (op < 63)
            {
                const int which = roll ((int) names.size() + 1);
                if (which == 0) pair.doBegin();
                else pair.doBegin (names[(std::size_t) which - 1]);
            }
            else if (op < 78) pair.doUndo();
            else if (op < 90) pair.doRedo();
            else if (op < 92) pair.doClear();
            else if (op < 95) pair.doLimit (1 + roll (40), 1 + roll (6));
            else pair.doFlush();

            pair.compare();
        }
        pair.doFlush();
        pair.compare();
    }
}
