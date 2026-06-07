#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include "Session.h"

namespace duskstudio
{
class AudioEngine;

// Replaces a single AudioRegion's fields with new values. Used for the move
// and trim drags, all of which collapse to "region X is now Y". perform()
// applies the new state; undo() restores the original.
class RegionEditAction final : public juce::UndoableAction
{
public:
    RegionEditAction (Session& session, AudioEngine& engine,
                       int trackIdx, int regionIdx,
                       const AudioRegion& before, const AudioRegion& after);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 1; }

private:
    Session& session;
    AudioEngine& engine;
    int trackIdx;
    int regionIdx;
    AudioRegion beforeState;
    AudioRegion afterState;
};

// Splits a region at a timeline-sample position into two regions. The
// original shrinks to [start, splitAt); a new region is inserted at idx+1
// covering [splitAt, originalEnd) with sourceOffset adjusted so the audio
// is continuous across the split.
class SplitRegionAction final : public juce::UndoableAction
{
public:
    SplitRegionAction (Session& session, AudioEngine& engine,
                        int trackIdx, int regionIdx,
                        juce::int64 splitAtTimelineSample);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 2; }

private:
    Session& session;
    AudioEngine& engine;
    int trackIdx;
    int regionIdx;
    juce::int64 splitAt;
    AudioRegion originalState;  // captured at first perform; restored on undo
};

// Inserts a copy of a region at the end of a track's region list. perform()
// records the index it was inserted at so undo() can erase the same slot.
class PasteRegionAction final : public juce::UndoableAction
{
public:
    PasteRegionAction (Session& session, AudioEngine& engine,
                        int trackIdx, const AudioRegion& region);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 1; }

private:
    Session& session;
    AudioEngine& engine;
    int trackIdx;
    AudioRegion regionToInsert;
    int insertedAt = -1;
};

// Adds a fresh empty MidiRegion to a MIDI track at a given timeline
// sample. Used by the TIMELINE-view double-click-to-create gesture so a
// user can hand-author MIDI without first having to record. Undo
// removes the region; redo re-inserts at the same position. Note pile
// stays empty - the user adds notes via the piano roll separately.
class CreateMidiRegionAction final : public juce::UndoableAction
{
public:
    CreateMidiRegionAction (Session& session,
                              int trackIdx,
                              juce::int64 timelineStart,
                              juce::int64 lengthInSamples,
                              juce::int64 lengthInTicks);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 1; }

    // Index of the just-created region in track.midiRegions. Valid only
    // after a successful perform(); -1 otherwise. Callers use this to
    // open the piano roll on the new region.
    int getInsertedIndex() const noexcept { return insertedAt; }

private:
    Session&    session;
    int         trackIdx;
    juce::int64 timelineStart;
    juce::int64 lengthInSamples;
    juce::int64 lengthInTicks;
    int         insertedAt = -1;
};

// Replaces a single MidiRegion's fields with new values. Mirror of
// RegionEditAction for the MIDI side. Used for tape-lane drag-move
// of MIDI regions and any future MIDI-region edits that benefit
// from full before/after capture (trim, label, colour, etc.). The
// notes / ccs vectors come along for free in the snapshot, which is
// what makes this safe even if a recording committed new notes
// between the drag start and finalise.
class MidiRegionEditAction final : public juce::UndoableAction
{
public:
    MidiRegionEditAction (Session& session, AudioEngine& engine,
                            int trackIdx, int regionIdx,
                            const MidiRegion& before, const MidiRegion& after);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 1; }

private:
    Session& session;
    AudioEngine& engine;
    int trackIdx;
    int regionIdx;
    MidiRegion beforeState;
    MidiRegion afterState;
};

// Removes a region; undo re-inserts it at its original index.
class DeleteRegionAction final : public juce::UndoableAction
{
public:
    DeleteRegionAction (Session& session, AudioEngine& engine,
                         int trackIdx, int regionIdx);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 1; }

private:
    Session& session;
    AudioEngine& engine;
    int trackIdx;
    int regionIdx;
    AudioRegion removed;
    bool        haveRemoved = false;
};

// Joins (glues) a set of audio regions on the same track into one.
//   • Fast path: when every selected region references the same source
//     file and the regions abut (or overlap) on the timeline, the join
//     collapses them into a single AudioRegion by extending the leading
//     region's lengthInSamples and erasing the rest. Source data and
//     existing fades at the outer edges are preserved.
//   • Slow path: when sources differ or there are gaps, the join renders
//     a fresh WAV into <session>/takes/ that mixes every selected region
//     across [minStart, maxEnd) and replaces the selection with one
//     region pointing at that file.
// `indices` must list the track-relative region indices the user wants
// joined; ctor sorts a copy by timelineStart so the action records a
// stable order. perform() captures the before-state of every involved
// region so undo() can restore the full pre-join layout.
class JoinRegionsAction final : public juce::UndoableAction
{
public:
    JoinRegionsAction (Session& session, AudioEngine& engine,
                        int trackIdx, const std::vector<int>& indices);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 6; }

private:
    Session& session;
    AudioEngine& engine;
    int trackIdx;
    std::vector<int> indices;            // sorted by timelineStart
    std::vector<AudioRegion> beforeRegions;
    int  resultInsertedAt = -1;
    juce::File renderedFile;             // only set on the slow path
    bool firstPerformDone = false;
};

// MIDI counterpart to DeleteRegionAction. Mutates through the
// AtomicSnapshot's currentMutable() since midiRegions is a swap-load
// vector; same race profile every other piano-roll edit accepts.
class DeleteMidiRegionAction final : public juce::UndoableAction
{
public:
    DeleteMidiRegionAction (Session& session, AudioEngine& engine,
                              int trackIdx, int regionIdx);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 1; }

private:
    Session& session;
    AudioEngine& engine;
    int trackIdx;
    int regionIdx;
    MidiRegion removed;
    bool       haveRemoved = false;
};

// Clones a source track's full per-strip state onto a destination slot:
// name, colour, mode, channel-strip params (fader/pan/EQ/comp/sends),
// recording surface settings, regions, MIDI regions, plugin instance
// (description + state). Undo restores the destination's previous state
// captured at first perform().
//
// Plugin replay: copying pluginDescriptionXml / pluginStateBase64 onto
// the destination Track isn't enough on its own - the live PluginSlot
// owned by AudioEngine has to re-instantiate. perform() reads source's
// live slot via getDescriptionXmlForSave / getStateBase64ForSave and
// asks the destination slot to restoreFromSavedState. Undo replays the
// captured before-state through the same path so the dest slot returns
// to whatever plugin (if any) was loaded before.
class CloneTrackAction final : public juce::UndoableAction
{
public:
    CloneTrackAction (Session& session, AudioEngine& engine,
                       int sourceTrackIdx, int destTrackIdx);
    // Out-of-line so the unique_ptr<Impl> deleters are instantiated in the
    // .cpp where Impl is complete, not at every user-site that includes
    // this header with Impl forward-declared.
    ~CloneTrackAction() override;

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 4; }   // medium weight

    // Public so the .cpp's anonymous helpers can take it as a parameter.
    // The struct itself is opaque to callers - they only see unique_ptr<Impl>.
    struct Impl;

private:
    Session& session;
    AudioEngine& engine;
    int srcIdx;
    int dstIdx;

    // Snapshots captured at first perform() so undo / redo are
    // self-contained.
    std::unique_ptr<Impl> beforeState;
    std::unique_ptr<Impl> afterState;
};

// Wraps the per-track regions + midiRegions diff produced by
// RecordManager::stopRecording so a take commit (audio + midi) becomes
// one undo step. perform() applies the after-snapshot; undo() restores
// the before-snapshot. WAV files on disk are NOT deleted on undo —
// the user can redo to re-attach the take, and orphaned files are
// reclaimed via the existing "Clean Out" menu action.
class RecordCommitAction final : public juce::UndoableAction
{
public:
    struct TrackDiff
    {
        int                       trackIndex = -1;
        std::vector<AudioRegion>  audioBefore;
        std::vector<AudioRegion>  audioAfter;
        std::vector<MidiRegion>   midiBefore;
        std::vector<MidiRegion>   midiAfter;
    };

    RecordCommitAction (Session& session, AudioEngine& engine,
                         std::vector<TrackDiff> diffs);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return juce::jmax (1, (int) diffs.size() * 4); }

private:
    Session& session;
    AudioEngine& engine;
    std::vector<TrackDiff> diffs;
    bool firstPerformDone = false;
};

// Replaces the whole tempo map. Every tempo-marker edit (add / change / delete /
// starting tempo) collapses to "the points were X, now Y", so one action type
// covers them all. perform()/undo() publish the after/before set through the
// engine's lock-free tempo snapshot.
class SetTempoMapAction final : public juce::UndoableAction
{
public:
    SetTempoMapAction (AudioEngine& engine,
                        std::vector<TempoPoint> before,
                        std::vector<TempoPoint> after);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 1; }

private:
    AudioEngine& engine;
    std::vector<TempoPoint> before, after;
};
} // namespace duskstudio
