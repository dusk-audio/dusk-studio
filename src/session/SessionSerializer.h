#pragma once

#include <juce_core/juce_core.h>
#include "Session.h"

namespace duskstudio
{
// Serialise / restore a Session to/from JSON on disk. Save uses an atomic
// write pattern (write to .tmp, move into place) so a crash mid-save never
// produces a partial session.json. Load is best-effort - missing fields fall
// back to the Session's defaults so older session files still open.
class SessionSerializer
{
public:
    static bool save (const Session& session, const juce::File& target);
    static bool load (Session& session,       const juce::File& source);

    // Build the JSON snapshot without writing it. Lets the autosave path
    // hash the serialised state to skip redundant writes when nothing has
    // changed since the last manual save / autosave tick.
    static juce::String serialize (const Session& session);

    // Atomic write helper: tmp file + fsync + rename. Reused by save() and
    // the autosave path. Returns true on success.
    static bool writeAtomic (const juce::File& target, const juce::String& json);
};
} // namespace duskstudio
