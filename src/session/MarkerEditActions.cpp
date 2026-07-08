#include "MarkerEditActions.h"
#include <algorithm>

namespace duskstudio
{
namespace
{
void sortMarkers (Session& s)
{
    auto& m = s.getMarkers();
    std::sort (m.begin(), m.end(),
        [] (const Marker& a, const Marker& b)
        { return a.timelineSamples < b.timelineSamples; });
}

int findMarkerByNameAt (Session& s, const juce::String& name, std::int64_t atSamples)
{
    const auto& m = s.getMarkers();
    for (int i = 0; i < (int) m.size(); ++i)
        if (m[(size_t) i].timelineSamples == atSamples
            && m[(size_t) i].name == name)
            return i;
    return -1;
}
} // namespace

// ── AddMarkerAction ───────────────────────────────────────────────────────

AddMarkerAction::AddMarkerAction (Session& s, std::int64_t t, juce::String n)
    : session (s), timelineSamples (t), name (std::move (n))
{}

bool AddMarkerAction::perform()
{
    insertedIdx = session.addMarker (timelineSamples, name);
    // Capture the auto-generated name on the FIRST perform so undo knows
    // exactly which marker to remove if the user added more in between.
    if (insertedIdx >= 0 && insertedIdx < (int) session.getMarkers().size()
        && name.isEmpty())
    {
        name = session.getMarkers()[(size_t) insertedIdx].name;
    }
    return insertedIdx >= 0;
}

bool AddMarkerAction::undo()
{
    if (insertedIdx < 0) return false;
    // Markers may have shifted if the user added more before/after this
    // one. Find by (name + samples) instead of trusting the cached idx.
    const int idx = findMarkerByNameAt (session, name, timelineSamples);
    if (idx < 0) return false;
    session.removeMarker (idx);
    return true;
}

// ── RemoveMarkerAction ────────────────────────────────────────────────────

RemoveMarkerAction::RemoveMarkerAction (Session& s, int idx)
    : session (s), markerIdx (idx)
{}

bool RemoveMarkerAction::perform()
{
    auto& markers = session.getMarkers();
    if (markerIdx < 0 || markerIdx >= (int) markers.size()) return false;
    removed = markers[(size_t) markerIdx];
    haveRemoved = true;
    session.removeMarker (markerIdx);
    return true;
}

bool RemoveMarkerAction::undo()
{
    if (! haveRemoved) return false;
    // Re-insert at the marker's original timeline position. addMarker
    // re-sorts by samples, so the index slot may differ — that's fine.
    session.addMarker (removed.timelineSamples, removed.name);
    return true;
}

// ── MoveMarkerAction ──────────────────────────────────────────────────────

MoveMarkerAction::MoveMarkerAction (Session& s, juce::String n,
                                      std::int64_t from, std::int64_t to)
    : session (s), name (std::move (n)), fromSamples (from), toSamples (to)
{}

bool MoveMarkerAction::perform()
{
    // Drag already moved the marker to toSamples; if perform is called
    // for the first time it's a no-op. After a prior undo, the marker
    // sits back at fromSamples, so we'll find it there.
    int idx = findMarkerByNameAt (session, name, toSamples);
    if (idx >= 0) return true;  // already at target
    idx = findMarkerByNameAt (session, name, fromSamples);
    if (idx < 0) return false;
    session.getMarkers()[(size_t) idx].timelineSamples = toSamples;
    sortMarkers (session);
    return true;
}

bool MoveMarkerAction::undo()
{
    int idx = findMarkerByNameAt (session, name, toSamples);
    if (idx < 0) return false;
    session.getMarkers()[(size_t) idx].timelineSamples = fromSamples;
    sortMarkers (session);
    return true;
}
} // namespace duskstudio
