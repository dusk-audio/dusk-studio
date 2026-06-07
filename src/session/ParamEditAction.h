#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <functional>

namespace duskstudio
{
// Generic undoable mixer / property edit. Each control supplies two closures:
// `applyAfter` (re-apply the new value) and `applyBefore` (restore the old).
// perform() runs applyAfter; undo() runs applyBefore. The live UI already
// applied the value during the drag/click, so perform() re-applying it is a
// harmless no-op the first time and the correct restore on redo.
//
// Continuous controls push ONE action per gesture: capture the value at
// drag-start, push at drag-end (before != after). Toggles push one per click.
// The closures capture the target atomic by pointer — valid for the life of
// the session (the undo history is cleared on session load), and they run on
// the message thread exactly like the normal UI write, so no new RT race.
class ParamEditAction final : public juce::UndoableAction
{
public:
    ParamEditAction (std::function<void()> applyAfter,
                      std::function<void()> applyBefore)
        : after (std::move (applyAfter)), before (std::move (applyBefore)) {}

    bool perform() override { if (after)  after();  return true; }
    bool undo()    override { if (before) before(); return true; }
    int  getSizeInUnits() override { return 1; }

private:
    std::function<void()> after, before;
};
} // namespace duskstudio
