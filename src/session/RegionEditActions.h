#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include "Session.h"
#include <algorithm>

namespace duskstudio
{
class AudioEngine;
using UndoableAction = juce::UndoableAction;

// The plugin portion of a clone snapshot is one indivisible replay unit.
// Keeping the structured descriptor, malformed legacy fallback, and state
// together prevents perform/undo/redo from dropping one representation.
struct ClonePluginSnapshot
{
    std::optional<PluginDescriptor> descriptor;
    juce::String legacyDescriptionXml;
    juce::String stateBase64;

    static ClonePluginSnapshot fromTrack (const Track& track)
    {
        return { track.pluginDescriptor,
                 track.pluginLegacyDescriptionXml,
                 track.pluginStateBase64 };
    }

    void publishTo (Track& track) const
    {
        track.pluginDescriptor = descriptor;
        track.pluginLegacyDescriptionXml = legacyDescriptionXml;
        track.pluginStateBase64 = stateBase64;
    }

    bool operator== (const ClonePluginSnapshot& other) const
    {
        return descriptor == other.descriptor
            && legacyDescriptionXml == other.legacyDescriptionXml
            && stateBase64 == other.stateBase64;
    }
};

// Replaces a single AudioRegion's fields with new values. Used for the move
// and trim drags, all of which collapse to "region X is now Y". perform()
// applies the new state; undo() restores the original.
class RegionEditAction final : public UndoableAction
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
class SplitRegionAction final : public UndoableAction
{
public:
    SplitRegionAction (Session& session, AudioEngine& engine,
                        int trackIdx, int regionIdx,
                        std::int64_t splitAtTimelineSample);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 2; }

private:
    Session& session;
    AudioEngine& engine;
    int trackIdx;
    int regionIdx;
    std::int64_t splitAt;
    AudioRegion originalState;  // captured at first perform; restored on undo
};

// Inserts a copy of a region at the end of a track's region list. perform()
// records the index it was inserted at so undo() can erase the same slot. The
// copy keeps its takeId only while that take is on the track and the copy
// reads the take's file.
class PasteRegionAction final : public UndoableAction
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
class CreateMidiRegionAction final : public UndoableAction
{
public:
    CreateMidiRegionAction (Session& session,
                              int trackIdx,
                              std::int64_t timelineStart,
                              std::int64_t lengthInSamples,
                              std::int64_t lengthInTicks);

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
    std::int64_t timelineStart;
    std::int64_t lengthInSamples;
    std::int64_t lengthInTicks;
    int         insertedAt = -1;
};

// Replaces a single MidiRegion's fields with new values. Mirror of
// RegionEditAction for the MIDI side. Used for tape-lane drag-move
// of MIDI regions and any future MIDI-region edits that benefit
// from full before/after capture (trim, label, colour, etc.). The
// notes / ccs vectors come along for free in the snapshot, which is
// what makes this safe even if a recording committed new notes
// between the drag start and finalise.
class MidiRegionEditAction final : public UndoableAction
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
class DeleteRegionAction final : public UndoableAction
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

// Puts the take's audio on the timeline over [start, end), clamped to the take,
// carving what the track played there (TakeComp's promoteTakeRange). The
// track's regions and takes are snapshotted whole before and after, so undo
// also removes a take the carve made of a region naming none, and ends an
// audition of it. Refused when a locked region lies in the range, since
// carving would split or trim it.
class PromoteTakeRangeAction final : public UndoableAction
{
public:
    PromoteTakeRangeAction (Session& session, AudioEngine& engine, int trackIdx,
                             TakeId takeId, std::int64_t start, std::int64_t end);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 2; }

private:
    Session& session;
    AudioEngine& engine;
    int trackIdx;
    TakeId takeId;
    std::int64_t start, end;
    std::vector<AudioRegion> beforeRegions, afterRegions;
    std::vector<AudioTake> beforeTakes, afterTakes;
    bool firstPerformDone = false;
};

// Removes a take and every region cut from it, and ends an audition of it.
// Undo puts the take and regions back with the same ids, the take at its place
// in recording order; the audition stays off. Refused while a region cut from
// the take is locked.
class DeleteTakeAction final : public UndoableAction
{
public:
    DeleteTakeAction (Session& session, AudioEngine& engine, int trackIdx, TakeId takeId);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 2; }

private:
    Session& session;
    AudioEngine& engine;
    int trackIdx;
    TakeId takeId;
    AudioTake removedTake;
    std::size_t takeIndex = 0;
    std::vector<AudioRegion> beforeRegions, afterRegions;
    bool firstPerformDone = false;
};

class RenameTakeAction final : public UndoableAction
{
public:
    RenameTakeAction (Session& session, int trackIdx, TakeId takeId, std::string newName);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 1; }

private:
    bool apply (const std::string& name);

    Session& session;
    int trackIdx;
    TakeId takeId;
    std::string newName;
    std::string oldName;
    bool firstPerformDone = false;
};

// Joins (glues) a set of audio regions on the same track into one.
//   - Fast path: when every selected region references the same source
//     file and the regions abut (or overlap) on the timeline, the join
//     collapses them into a single AudioRegion by extending the leading
//     region's lengthInSamples and erasing the rest. Source data and
//     existing fades at the outer edges are preserved, and so is the
//     takeId when every region names the same take.
//   - Slow path: when sources differ or there are gaps, the join renders
//     a fresh WAV into <session>/takes/ that mixes every selected region
//     across [minStart, maxEnd) and replaces the selection with one
//     region pointing at that file, which names no take.
// `indices` must list the track-relative region indices the user wants
// joined; ctor sorts a copy by timelineStart so the action records a
// stable order. perform() captures the before-state of every involved
// region so undo() can restore the full pre-join layout.
class JoinRegionsAction final : public UndoableAction
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
    bool firstPerformDone = false;
};

// Reverses a single audio region's content non-destructively: reads the
// region's source samples, reverses each channel, renders a fresh WAV into
// <session>/takes/, and repoints the region at it (sourceOffset 0, fades
// swapped so the envelope stays on the same material). The rendered file is no
// take's, so the region names none. The original file is untouched; undo
// restores the pre-reverse region. Frozen tracks are edit-locked (perform
// bails). Single region.
class ReverseRegionAction final : public UndoableAction
{
public:
    ReverseRegionAction (Session& session, AudioEngine& engine,
                          int trackIdx, int regionIdx);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 4; }

private:
    Session& session;
    AudioEngine& engine;
    int trackIdx;
    int regionIdx;
    AudioRegion beforeState;
    AudioRegion afterState;
    bool firstPerformDone = false;
};

// MIDI counterpart to DeleteRegionAction. Erase/insert reshape the
// vector, so both go through mutate() (copy + publish) - never
// currentMutable() while the audio thread iterates the snapshot.
class DeleteMidiRegionAction final : public UndoableAction
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
// everything SessionSerializer writes for a track (the scenario
// session.clone_track_carries_every_saved_field holds the two in step) plus
// the strip's live insert mode and the live plugin state. Undo restores the
// destination's previous state captured at first perform().
//
// Plugin replay: copying the descriptor / legacy fallback / state onto
// the destination Track isn't enough on its own - the live PluginSlot
// owned by AudioEngine has to re-instantiate. perform() reads source's
// live slot via getDescriptorForSave / getStateBase64ForSave and
// asks the destination slot to restoreFromSavedState. Undo replays the
// captured before-state through the same path so the dest slot returns
// to whatever plugin (if any) was loaded before.
class CloneTrackAction final : public UndoableAction
{
public:
    CloneTrackAction (Session& session, AudioEngine& engine,
                       int sourceTrackIdx, int destTrackIdx);
    // Out-of-line so the unique_ptr<Impl> deleters are instantiated in the
    // .cpp where Impl is complete, not at every user-site that includes
    // this header with Impl forward-declared.
    ~CloneTrackAction() override;

    // Why a fresh clone would refuse, so the menu can say so. A clone waits
    // for a stopped transport with no automation pass open on either track: a
    // pass records its ride on the side, so the source's lanes lack it and the
    // destination's pass would be dropped when the clone replaces its lanes.
    // Undo and redo do not wait - a refused one makes the undo manager discard
    // the whole history - and republish mid-playback like any lane undo.
    enum class Refusal { None, Frozen, Playing };
    static Refusal refusalFor (const Session& session, AudioEngine& engine,
                               int sourceTrackIdx, int destTrackIdx);

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

// Wraps the per-track regions + midiRegions + takes diff produced by
// RecordManager::stopRecording so a take commit (audio + midi) becomes
// one undo step. perform() applies the after-snapshot; undo() restores
// the before-snapshot and ends an audition of a take it removes, which
// redo does not bring back. WAV files on disk are NOT deleted on undo -
// the user can redo to re-attach the take, and orphaned files are
// reclaimed via the existing "Clean Out" menu action.
class RecordCommitAction final : public UndoableAction
{
public:
    struct TrackDiff
    {
        int                       trackIndex = -1;
        std::vector<AudioRegion>  audioBefore;
        std::vector<AudioRegion>  audioAfter;
        std::vector<MidiRegion>   midiBefore;
        std::vector<MidiRegion>   midiAfter;
        std::vector<AudioTake>    takesBefore;
        std::vector<AudioTake>    takesAfter;
    };

    RecordCommitAction (Session& session, AudioEngine& engine,
                         std::vector<TrackDiff> diffs);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return std::max (1, (int) diffs.size() * 4); }

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
class SetTempoMapAction final : public UndoableAction
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

// Replaces one track's automation-lane point set. Add / drag / delete of a
// breakpoint all collapse to "the lane was X, now Y". perform()/undo() swap
// the point vector and republish it to the audio thread via a release-store
// on the track's automationMode (the Session.h lane-sync contract). Lane
// editing is gated on transport=Stopped, same as the live edit path.
class AutomationLaneEditAction final : public UndoableAction
{
public:
    AutomationLaneEditAction (Session& session, int trackIdx, int paramIdx,
                               std::vector<AutomationPoint> before,
                               std::vector<AutomationPoint> after);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 1; }

private:
    bool apply (const std::vector<AutomationPoint>& pts);

    Session& session;
    int trackIdx, paramIdx;
    std::vector<AutomationPoint> before, after;
};
} // namespace duskstudio
