#pragma once

#include "../session/MidiEvents.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace duskstudio
{
class PianoRollEdit
{
public:
    struct DragSnapshot
    {
        std::int64_t startTick = 0;
        int noteNumber = 0;
        std::int64_t lengthInTicks = 0;
        int velocity = 100;
    };

    // Sorted indices permit toggling and descending erasure without remapping.
    std::vector<int> selectedNotes;
    int dragAnchor = -1;
    std::vector<DragSnapshot> dragSnapshots;
    // Process-wide; ticks are relative to the earliest copied note.
    static std::vector<MidiNote> clipboard;

    bool isNoteSelected (int index) const noexcept;
    void clearSelection();
    void selectOnly (int index);
    void addToSelection (int index);
    void toggleSelected (int index);
    void beginGroupDrag (const std::vector<MidiNote>* notes, int anchor);
    void applyGroupMove (std::vector<MidiNote>& notes, std::int64_t lengthTicks,
                         std::int64_t deltaTicks, int deltaPitch);
    void transposeSelected (std::vector<MidiNote>& notes, int semitones);
    void quantizeSelected (std::vector<MidiNote>& notes, std::int64_t gridTicks, float strength);
    void humanizeVelocity (std::vector<MidiNote>& notes, int rangePercent,
                           const std::function<int (int, int)>& nextInt);
    void setVelocityFor (std::vector<MidiNote>& notes, int value);
    void setChannelForSelected (std::vector<MidiNote>& notes, int channel);
    void setLengthTicksForSelected (std::vector<MidiNote>& notes, std::int64_t lengthTicks, std::int64_t ticks);
    void glueSelectedNotes (std::vector<MidiNote>& notes);
    void duplicateSelectedNotes (std::vector<MidiNote>& notes, std::int64_t quarterTicks);
    void nudgeSelectedTicks (std::vector<MidiNote>& notes, std::int64_t lengthTicks, std::int64_t deltaTicks);
    void copySelected (const std::vector<MidiNote>& notes);
    void deleteSelected (std::vector<MidiNote>& notes);
    void paste (std::vector<MidiNote>& notes, std::int64_t lengthTicks, std::int64_t atTick);
};
} // namespace duskstudio
