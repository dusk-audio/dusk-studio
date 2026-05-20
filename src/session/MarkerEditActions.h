#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include "Session.h"

namespace duskstudio
{
// UndoableActions for the marker timeline. Mirrors the shape of
// RegionEditActions: each operation captures whatever it needs at
// perform()-time so undo() can restore the prior state. Markers are kept
// sorted by timelineSamples; the actions re-sort after each mutation.

class AddMarkerAction final : public juce::UndoableAction
{
public:
    AddMarkerAction (Session& session, juce::int64 timelineSamples,
                       juce::String name = {});

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 1; }

private:
    Session&     session;
    juce::int64  timelineSamples;
    juce::String name;
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
// Markers are identified by (name + currentExpectedSamples) — the action
// finds the marker at whichever of from/to it currently sits at, which
// stays unambiguous across perform/undo cycles (the marker is always at
// one of those two times).
class MoveMarkerAction final : public juce::UndoableAction
{
public:
    MoveMarkerAction (Session& session, juce::String markerName,
                        juce::int64 fromTimelineSamples,
                        juce::int64 toTimelineSamples);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 1; }

private:
    Session&     session;
    juce::String name;
    juce::int64  fromSamples;
    juce::int64  toSamples;
};
} // namespace duskstudio
