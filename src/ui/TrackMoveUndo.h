#pragma once

#include "DuskAlerts.h"
#include "../session/RegionEditActions.h"

#include <string>

namespace duskstudio
{
constexpr const char* kCantMoveTracksTitle = "Can't move tracks";

// The "Can't move tracks" alert, over the window host sits in rather than over
// host: a render's dialog, which can be the reason, sits on the window too.
template <typename Host>
void explainTrackMoveRefused (Host& host, const std::string& message)
{
    auto* window = host.getTopLevelComponent();
    showDuskAlert (window != nullptr ? *window : host, kCantMoveTracksTitle, message);
}

// The engine's undo (redo when redo is true). A track move that can't be undone
// or redone now leaves the history as it is and says why.
template <typename Host>
bool undoOrExplain (AudioEngine& engine, Host& host, bool redo)
{
    const auto refusal = trackMoveUndoRefusal (engine, redo);
    if (refusal.kind == TrackMoveRefusal::Kind::None)
        return redo ? redoTransaction (engine) : undoTransaction (engine);
    explainTrackMoveRefused (host, trackMoveRefusalMessage (refusal, redo ? TrackMoveStep::Redo : TrackMoveStep::Undo));
    return false;
}
} // namespace duskstudio
