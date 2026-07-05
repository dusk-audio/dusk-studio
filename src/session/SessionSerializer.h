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

    // Save As support: copy every session-owned audio file (regions, take
    // history, freeze WAVs, a session-local mastering source) into
    // newSessionDir and repoint the in-memory model, so the subsequent
    // serialize emits paths relative to the new directory. Without this a
    // Save As writes absolute paths into the old folder — deleting it loses
    // all audio. External mastering sources stay absolute (leave-external-
    // media); already-external region/take files are pulled in, which also
    // heals sessions whose refs point into another session's folder.
    //
    // Copies happen before any model mutation: on failure the model is
    // untouched, already-copied files are removed and ok=false. Missing
    // source files are skipped (ref kept as-is) and reported.
    struct ConsolidationResult
    {
        bool ok = true;
        juce::String errorMessage;
        int filesCopied = 0;
        std::vector<juce::String> missingSources;
    };
    static ConsolidationResult consolidateInto (Session& session,
                                                const juce::File& newSessionDir);
};
} // namespace duskstudio
