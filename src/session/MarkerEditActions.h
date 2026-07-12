#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include "Session.h"

#include <string>

namespace duskstudio
{
// UndoableActions for the marker timeline. Mirrors the shape of
// RegionEditActions: each operation captures whatever it needs at
// perform()-time so undo() can restore the prior state. Markers are kept
// sorted by timelineSamples; the actions re-sort after each mutation.

class AddMarkerAction final : public juce::UndoableAction
{
public:
    AddMarkerAction (Session& session, std::int64_t timelineSamples,
                       std::string name = {});

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 1; }

    // Valid after perform(); -1 if the add failed. Lets the UI open a
    // rename prompt on the just-created marker.
    int insertedIndex() const noexcept { return insertedIdx; }

private:
    Session&     session;
    std::int64_t  timelineSamples;
    std::string  name;
    int          insertedIdx = -1;
};

class RemoveMarkerAction final : public juce::UndoableAction
{
public:
    RemoveMarkerAction (Session& session, int markerIdx);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 1; }

private:
    Session& session;
    int      markerIdx;
    Marker   removed;
    bool     haveRemoved = false;
};

// Marker drag finalisation. The drag mutates the marker in-place for live
// feedback; on drag-end we wrap the swap as a transaction so a subsequent
// perform() / undo() / redo() flips between fromSamples and toSamples.
// Markers are identified by (name + currentExpectedSamples) - the action
// finds the marker at whichever of from/to it currently sits at, which
// stays unambiguous across perform/undo cycles (the marker is always at
// one of those two times).
class MoveMarkerAction final : public juce::UndoableAction
{
public:
    MoveMarkerAction (Session& session, std::string markerName,
                        std::int64_t fromTimelineSamples,
                        std::int64_t toTimelineSamples);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 1; }

private:
    Session&     session;
    std::string  name;
    std::int64_t  fromSamples;
    std::int64_t  toSamples;
};
} // namespace duskstudio
