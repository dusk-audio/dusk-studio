#pragma once

#include "Session.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Standard MIDI Files that keep a session's MIDI outside session.json, so a
// damaged session.json, or one saved with wrong data, does not take the MIDI
// with it. Under <session>/midi:
//
//   takes/   one file per recorded pass, written once when the pass commits;
//   tracks/  per track, a playing file with the regions that play, as they
//            play now, and a takes file with the muted regions and every
//            region's previous takes. Each is written whenever it has something
//            to hold and differs from the newest file of its kind there for the
//            same track number and sanitized name.
//
// The importer merges every track of a file into one region, so a file holding
// all of a track's MIDI would re-import with its muted regions and old takes
// over the notes that play. The playing file re-imports as the performance
// heard.
//
// Writing never replaces or removes a file in either folder. A file's events
// sit at the ticks (kMidiTicksPerQuarter to the quarter) where playback puts
// them, counted on the session tempo map from the timeline's start, and its
// conductor carries that map up to the file's last event and no further, so a
// re-import neither moves nor stretches anything. A file holds what playback
// reads from a region: events the MIDI wire cannot carry are left out, as
// playback leaves them out, but nothing is cut at the region's end.
//
// Building a file is split from writing it, so the disk never holds up the
// message thread. The builders run on the message thread, which owns the
// regions they read; writeFile runs on any thread.
namespace duskstudio::midirecovery
{
// What places an event on the timeline, as playback reads it.
struct Timing
{
    TempoMap tempoMap;                 // empty: the constant bpm below
    float    bpm         = 120.0f;
    double   sampleRate  = 48000.0;    // the timeline's, as AudioEngine::getTimelineSampleRate gives it
    int      beatsPerBar = 4;
    int      beatUnit    = 4;
};

Timing timingOf (const Session& session, double sampleRate);

// The pass's notes and controllers as one track named trackName, placed from
// pass.timelineStart. Only the pass itself: its previous takes, if any, are
// passes of their own. Empty when the writer refuses the result.
std::vector<std::uint8_t> buildTakeFile (const Timing& timing, const MidiRegion& pass,
                                         const std::string& trackName);

// Each region that plays, in timeline order, as a track named "Region N" (with
// " - " and its label when it has one) holding what the region plays now. N
// counts every region of the track, muted ones too, so it names the same region
// in both files. The conductor carries trackName. Empty when the writer refuses
// the result.
std::vector<std::uint8_t> buildPlayingFile (const Timing& timing, const std::vector<MidiRegion>& regions,
                                            const std::string& trackName);

// What does not play, in timeline order: a track for what each muted region
// holds now and one for each previous take of every region, each placed where
// its region starts. A track is named as in the playing file, then ", take K"
// when its region has previous takes, numbered as the region's take menu
// numbers them (1 is the take it plays, its previous takes 2 and on), then
// " (muted)" when its region is muted. The conductor carries trackName. Empty
// when the writer refuses the result.
std::vector<std::uint8_t> buildTakesFile (const Timing& timing, const std::vector<MidiRegion>& regions,
                                          const std::string& trackName);

// A track name as one part of a file name every supported system accepts: each
// of / \ : * ? " < > |, each control character and each byte that is not UTF-8
// becomes '_', the name stops at 64 bytes without splitting a character, and a
// trailing dot or space goes. Nothing left reads "untitled".
std::string sanitizeName (std::string_view name);

// Which of a track's two files in tracks/ a name is for. A take file in takes/
// is named as a playing file.
enum class TrackFile
{
    Playing,
    Takes,
};

// "20261108-143005Z_track07_Keys.mid": the UTC second, the 1-based track number
// and the sanitized name; a takes file ends ".takes.mid" instead. Copies 2 to
// 999, for further files of a kind in the same second, put "_002" to "_999"
// before that ending. '_' sorts after '.' and every copy number has three
// digits, so a plain sort keeps a second's files in the order they were made.
std::string fileName (std::chrono::system_clock::time_point now, int trackNumber,
                      std::string_view trackName, TrackFile kind = TrackFile::Playing, int copy = 1);

struct Failure
{
    std::filesystem::path path;   // the file or folder the step was on
    std::error_code       error;
};

struct WriteResult
{
    std::vector<std::filesystem::path> written;
    std::vector<Failure>               failures;
};

// Which folder under midi/ a file goes to: takes/ always under a new name,
// tracks/ only when it differs from the newest file there of its kind for the
// same track number and sanitized name.
enum class Folder
{
    Takes,
    Tracks,
};

// One file built and waiting to be written, with everything its write
// needs. `made` stamps its name: when the take committed or the save ran,
// not when the disk got to it.
struct PendingFile
{
    std::filesystem::path                 sessionDir;
    std::chrono::system_clock::time_point made;
    int                                   trackNumber = 0;   // 1-based
    std::string                           name;              // as sanitizeName gives it
    TrackFile                             kind   = TrackFile::Playing;
    Folder                                folder = Folder::Takes;
    std::vector<std::uint8_t>             bytes;             // empty when the file writer refused the events
};

// The take file for the pass recorded on track trackIndex (0-based), or
// nothing for a pass with no notes and no controllers or a track out of range.
std::optional<PendingFile> pendingTakeFile (const std::filesystem::path& sessionDir,
                                            std::chrono::system_clock::time_point made,
                                            const Session& session, double sampleRate, int trackIndex,
                                            const MidiRegion& pass);

// For each track that has MIDI regions, in any track mode as session.json keeps
// them, each of its two files for tracks/ that has something to hold: the
// playing file when a region plays, the takes file when a region is muted or
// has a previous take. Track order, the playing file first.
std::vector<PendingFile> pendingTrackFiles (const std::filesystem::path& sessionDir,
                                            std::chrono::system_clock::time_point made,
                                            const Session& session, double sampleRate);

// Writes one file under a name no file in its folder has yet, or, for tracks/,
// nothing when the newest file of its kind there already holds these bytes;
// tracks/ is listed for each file, so the second of two identical files
// written in turn writes nothing. The session folder must exist; the midi
// folders are made when needed.
WriteResult writeFile (const PendingFile& file) noexcept;
} // namespace duskstudio::midirecovery
