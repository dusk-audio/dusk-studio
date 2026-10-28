#include "PianoRollEdit.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace duskstudio
{
namespace
{
// Duplicated notes may extend past the region, producing inverted drag bounds.
template <typename T>
T limited (T low, T high, T value) noexcept { return std::clamp (value, low, std::max (low, high)); }
}

std::vector<MidiNote> PianoRollEdit::clipboard;

bool PianoRollEdit::isNoteSelected (int idx) const noexcept
{
    return std::binary_search (selectedNotes.begin(), selectedNotes.end(), idx);
}

void PianoRollEdit::clearSelection()
{
    selectedNotes.clear();
    dragAnchor = -1;
}

void PianoRollEdit::selectOnly (int idx)
{
    selectedNotes.clear();
    if (idx >= 0) selectedNotes.push_back (idx);
}

void PianoRollEdit::addToSelection (int idx)
{
    if (idx < 0) return;
    auto it = std::lower_bound (selectedNotes.begin(), selectedNotes.end(), idx);
    if (it == selectedNotes.end() || *it != idx)
        selectedNotes.insert (it, idx);
}

void PianoRollEdit::toggleSelected (int idx)
{
    if (idx < 0) return;
    auto it = std::lower_bound (selectedNotes.begin(), selectedNotes.end(), idx);
    if (it != selectedNotes.end() && *it == idx)
        selectedNotes.erase (it);
    else
        selectedNotes.insert (it, idx);
}

void PianoRollEdit::beginGroupDrag (const std::vector<MidiNote>* notes, int anchorIdx)
{
    dragAnchor = anchorIdx;
    dragSnapshots.clear();
    if (notes == nullptr) return;
    dragSnapshots.reserve (selectedNotes.size());
    for (int idx : selectedNotes)
    {
        if (idx < 0 || idx >= (int) notes->size()) continue;
        const auto& n = (*notes)[(std::size_t) idx];
        dragSnapshots.push_back ({ n.startTick, n.noteNumber, n.lengthInTicks, n.velocity });
    }
}

void PianoRollEdit::applyGroupMove (std::vector<MidiNote>& notes, std::int64_t lengthTicks, std::int64_t deltaTicks, int deltaPitch)
{
    if (selectedNotes.size() != dragSnapshots.size()) return;

    std::int64_t minStart   = std::numeric_limits<std::int64_t>::max();
    std::int64_t maxEnd     = std::numeric_limits<std::int64_t>::min();
    int         minPitch   = 127;
    int         maxPitch   = 0;
    for (const auto& s : dragSnapshots)
    {
        minStart  = std::min (minStart, s.startTick);
        maxEnd    = std::max (maxEnd,   s.startTick + s.lengthInTicks);
        minPitch  = std::min (minPitch, s.noteNumber);
        maxPitch  = std::max (maxPitch, s.noteNumber);
    }
    const std::int64_t maxDeltaTickRight = lengthTicks - maxEnd;
    deltaTicks = limited (-minStart, maxDeltaTickRight, deltaTicks);
    deltaPitch = limited (-minPitch, 127 - maxPitch, deltaPitch);

    for (std::size_t i = 0; i < selectedNotes.size(); ++i)
    {
        const int idx = selectedNotes[i];
        if (idx < 0 || idx >= (int) notes.size()) continue;
        auto& n = notes[(std::size_t) idx];
        n.startTick  = dragSnapshots[i].startTick + deltaTicks;
        n.noteNumber = dragSnapshots[i].noteNumber + deltaPitch;
    }
}

void PianoRollEdit::transposeSelected (std::vector<MidiNote>& notes, int semitones)
{
    if (selectedNotes.empty()) return;
    for (int idx : selectedNotes)
    {
        if (idx < 0 || idx >= (int) notes.size()) continue;
        auto& n = notes[(std::size_t) idx];
        n.noteNumber = limited (0, 127, n.noteNumber + semitones);
    }
}

void PianoRollEdit::quantizeSelected (std::vector<MidiNote>& notes, std::int64_t gridTicks, float strength)
{
    if (gridTicks <= 0) return;
    strength = limited (0.0f, 1.0f, strength);

    auto applyToOne = [gridTicks, strength] (MidiNote& n)
    {
        const auto orig    = n.startTick;
        const auto snapped = ((orig + gridTicks / 2) / gridTicks) * gridTicks;
        const auto blended = orig + (std::int64_t) std::round (
            (double) (snapped - orig) * strength);
        n.startTick = std::max ((std::int64_t) 0, blended);
    };
    if (selectedNotes.empty())
    {
        for (auto& n : notes) applyToOne (n);
    }
    else
    {
        for (int idx : selectedNotes)
        {
            if (idx < 0 || idx >= (int) notes.size()) continue;
            applyToOne (notes[(std::size_t) idx]);
        }
    }
}

void PianoRollEdit::humanizeVelocity (std::vector<MidiNote>& notes, int rangePercent, const std::function<int (int, int)>& nextInt)
{
    rangePercent = limited (1, 100, rangePercent);
    const int range = (int) std::round (127.0 * (double) rangePercent / 100.0);
    auto applyToOne = [&] (MidiNote& n)
    {
        const int delta = nextInt (-range, range + 1);
        n.velocity = limited (1, 127, n.velocity + delta);
    };
    if (selectedNotes.empty()) for (auto& n : notes) applyToOne (n);
    else
        for (int idx : selectedNotes)
            if (idx >= 0 && idx < (int) notes.size())
                applyToOne (notes[(std::size_t) idx]);
}

void PianoRollEdit::setVelocityFor (std::vector<MidiNote>& notes, int value)
{
    const int v = limited (1, 127, value);
    if (selectedNotes.empty()) for (auto& n : notes) n.velocity = v;
    else
        for (int idx : selectedNotes)
            if (idx >= 0 && idx < (int) notes.size())
                notes[(std::size_t) idx].velocity = v;
}

void PianoRollEdit::setChannelForSelected (std::vector<MidiNote>& notes, int channel)
{
    const int ch = limited (1, 16, channel);
    auto applyToOne = [ch] (MidiNote& n) { n.channel = ch; };
    if (selectedNotes.empty()) for (auto& n : notes) applyToOne (n);
    else
        for (int idx : selectedNotes)
            if (idx >= 0 && idx < (int) notes.size())
                applyToOne (notes[(std::size_t) idx]);
}

void PianoRollEdit::setLengthTicksForSelected (std::vector<MidiNote>& notes, std::int64_t lengthTicks, std::int64_t ticks)
{
    if (ticks <= 0) return;
    auto applyToOne = [lengthTicks, ticks] (MidiNote& n)
    {
        const auto maxLen = std::max ((std::int64_t) 1, lengthTicks - n.startTick);
        n.lengthInTicks = limited ((std::int64_t) 1, maxLen, ticks);
    };
    if (selectedNotes.empty()) for (auto& n : notes) applyToOne (n);
    else
        for (int idx : selectedNotes)
            if (idx >= 0 && idx < (int) notes.size())
                applyToOne (notes[(std::size_t) idx]);
}

void PianoRollEdit::glueSelectedNotes (std::vector<MidiNote>& notes)
{
    if (selectedNotes.size() < 2) return;

    std::map<std::pair<int, int>, std::vector<int>> byChannelAndPitch;
    for (int idx : selectedNotes)
    {
        if (idx < 0 || idx >= (int) notes.size()) continue;
        const auto& note = notes[(std::size_t) idx];
        byChannelAndPitch[{ note.channel, note.noteNumber }].push_back (idx);
    }

    std::vector<int> toErase;
    for (auto& group : byChannelAndPitch)
    {
        auto& indices = group.second;
        std::sort (indices.begin(), indices.end(),
            [&] (int a, int b) {
                return notes[(std::size_t) a].startTick < notes[(std::size_t) b].startTick;
            });

        for (std::size_t i = 0; i + 1 < indices.size(); )
        {
            const int aIdx = indices[i];
            const int bIdx = indices[i + 1];
            auto& a = notes[(std::size_t) aIdx];
            auto& b = notes[(std::size_t) bIdx];
            const auto aEnd = a.startTick + a.lengthInTicks;
            const auto bEnd = b.startTick + b.lengthInTicks;

            if (b.startTick <= aEnd)
            {
                a.lengthInTicks = std::max (aEnd, bEnd) - a.startTick;
                toErase.push_back (bIdx);
                indices.erase (indices.begin() + (long) (i + 1));
            }
            else
            {
                ++i;
            }
        }
    }

    if (toErase.empty()) return;
    std::sort (toErase.begin(), toErase.end(), std::greater<int>());
    for (int idx : toErase)
    {
        if (idx >= 0 && idx < (int) notes.size())
            notes.erase (notes.begin() + idx);
    }

    clearSelection();
}

void PianoRollEdit::duplicateSelectedNotes (std::vector<MidiNote>& notes, std::int64_t quarterTicks)
{
    if (selectedNotes.empty()) return;

    std::int64_t minStart = std::numeric_limits<std::int64_t>::max();
    std::int64_t maxEnd   = std::numeric_limits<std::int64_t>::min();
    for (int idx : selectedNotes)
    {
        if (idx < 0 || idx >= (int) notes.size()) continue;
        const auto& n = notes[(std::size_t) idx];
        minStart = std::min (minStart, n.startTick);
        maxEnd   = std::max (maxEnd,   n.startTick + n.lengthInTicks);
    }
    std::int64_t shift = maxEnd - minStart;
    if (shift <= 0) shift = quarterTicks;

    std::vector<int> newSelection;
    newSelection.reserve (selectedNotes.size());
    for (int idx : selectedNotes)
    {
        if (idx < 0 || idx >= (int) notes.size()) continue;
        MidiNote clone = notes[(std::size_t) idx];
        clone.startTick += shift;
        notes.push_back (clone);
        newSelection.push_back ((int) notes.size() - 1);
    }
    selectedNotes = std::move (newSelection);
    std::sort (selectedNotes.begin(), selectedNotes.end());
}

void PianoRollEdit::nudgeSelectedTicks (std::vector<MidiNote>& notes, std::int64_t lengthTicks, std::int64_t deltaTicks)
{
    if (selectedNotes.empty()) return;

    std::int64_t minStart = std::numeric_limits<std::int64_t>::max();
    std::int64_t maxEnd   = std::numeric_limits<std::int64_t>::min();
    for (int idx : selectedNotes)
    {
        if (idx < 0 || idx >= (int) notes.size()) continue;
        const auto& n = notes[(std::size_t) idx];
        minStart = std::min (minStart, n.startTick);
        maxEnd   = std::max (maxEnd,   n.startTick + n.lengthInTicks);
    }
    deltaTicks = limited (-minStart,
                        lengthTicks - maxEnd,
                        deltaTicks);
    if (deltaTicks == 0) return;
    for (int idx : selectedNotes)
    {
        if (idx < 0 || idx >= (int) notes.size()) continue;
        notes[(std::size_t) idx].startTick += deltaTicks;
    }
}

void PianoRollEdit::copySelected (const std::vector<MidiNote>& notes)
{
    clipboard.clear();
    std::int64_t minTick = std::numeric_limits<std::int64_t>::max();
    for (int idx : selectedNotes)
        if (idx >= 0 && idx < (int) notes.size())
            minTick = std::min (minTick, notes[(std::size_t) idx].startTick);
    for (int idx : selectedNotes)
    {
        if (idx < 0 || idx >= (int) notes.size()) continue;
        MidiNote note = notes[(std::size_t) idx];
        note.startTick -= minTick;
        clipboard.push_back (note);
    }
}

void PianoRollEdit::deleteSelected (std::vector<MidiNote>& notes)
{
    for (auto it = selectedNotes.rbegin(); it != selectedNotes.rend(); ++it)
        if (*it >= 0 && *it < (int) notes.size())
            notes.erase (notes.begin() + *it);
    clearSelection();
}

void PianoRollEdit::paste (std::vector<MidiNote>& notes, std::int64_t lengthTicks, std::int64_t atTick)
{
    clearSelection();
    for (const auto& source : clipboard)
    {
        auto note = source;
        note.startTick = atTick + source.startTick;
        if (note.startTick < 0 || note.startTick >= lengthTicks) continue;
        if (note.startTick + note.lengthInTicks > lengthTicks)
            note.lengthInTicks = lengthTicks - note.startTick;
        notes.push_back (note);
        addToSelection ((int) notes.size() - 1);
    }
}
} // namespace duskstudio
