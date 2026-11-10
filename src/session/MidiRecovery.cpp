#include "MidiRecovery.h"

#include "RecoveryStamp.h"
#include "../engine/midi/MidiFileWriter.h"
#include "../foundation/AtomicFile.h"
#include "../foundation/Fs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <new>
#include <optional>
#include <utility>

namespace duskstudio::midirecovery
{
namespace
{
namespace stdfs = std::filesystem;
using midi::MidiFileEvent;
using midi::MidiFileTrack;
using recoverystamp::isStamp;
using recoverystamp::kStampLength;
using recoverystamp::utcStamp;

// The writer refuses a gap of more than 0x0FFFFFFF ticks, and a note can end one
// tick after the last place a note-on can take.
constexpr std::int64_t kLastPlacedTick = 0x0ffffffe;
constexpr std::size_t  kMaxNameBytes   = 64;
constexpr int          kMaxCopies      = 999;
// Every pass of a loop take handed over at one Stop shares its second, and a
// loop take keeps up to 1,024 of them.
constexpr int          kMaxSeconds     = 8;
constexpr char         kUnnamed[]      = "untitled";

// Where a region starts, rounded to a tick on the session tempo map, as
// RegionClock places it for playback; the region's own ticks count from there.
// With no map, playback counts the region's ticks in samples from its start,
// which a file's constant tempo reproduces to within half a tick.
std::int64_t regionStartTick (const Timing& timing, std::int64_t timelineStart) noexcept
{
    return timing.tempoMap.empty()
        ? ::duskstudio::samplesToTicks (timelineStart, timing.sampleRate, timing.bpm)
        : timing.tempoMap.samplesToTicks (timelineStart, timing.sampleRate);
}

// A region's tick on the file's timeline, or -1 past the last one a file can
// hold. A tick before the timeline's start lands on it, as the tempo map plays it.
std::int64_t placedTick (std::int64_t startTick, std::int64_t tick) noexcept
{
    if (tick > 0 && startTick > std::numeric_limits<std::int64_t>::max() - tick) return -1;
    if (tick < 0 && startTick < std::numeric_limits<std::int64_t>::min() - tick) return 0;
    const auto at = startTick + tick;
    return at > kLastPlacedTick ? -1 : std::max<std::int64_t> (0, at);
}

void appendPass (MidiFileTrack& track, std::int64_t startTick,
                 const std::vector<MidiNote>& notes, const std::vector<MidiCc>& ccs)
{
    const auto playback = buildMidiPlayback (notes, ccs);
    for (const auto& controller : playback.controllers)
    {
        const auto at = placedTick (startTick, controller.tick);
        if (at < 0) continue;
        const auto message = controller.message();
        track.events.push_back ({ at, message.statusByte(), message.dataByte1(), message.dataByte2() });
    }
    for (const auto& note : playback.notes)
    {
        const auto on = placedTick (startTick, note.startTick);
        const auto off = placedTick (startTick, std::max (note.endTick, note.startTick));
        if (on < 0 || off < 0) continue;
        // A note of no length plays its note-off right behind its note-on
        // (scheduleSpan's onsets). At one tick the file would put the off first
        // and the note would hang, so it ends one tick after it starts.
        track.events.push_back ({ on, (std::uint8_t) (0x90 | note.channel), note.key, note.velocity });
        track.events.push_back ({ std::max (off, on + 1), (std::uint8_t) (0x80 | note.channel), note.key, 0 });
    }
}

// A tempo too slow or too fast for the format goes in as the nearest one it
// carries, and no tempo at all (not above zero, or not a number) as 120 bpm.
int microsecondsPerQuarter (float bpm) noexcept
{
    if (! std::isfinite (bpm) || bpm <= 0.0f) return 500000;
    return (int) std::clamp (std::round (60.0e6 / (double) bpm), 1.0, (double) 0xffffff);
}

midi::MidiFileConductor conductorFor (const Timing& timing, std::int64_t lastTick, std::string name)
{
    midi::MidiFileConductor conductor;
    conductor.name = std::move (name);
    // A beat unit that is no power of two cannot go in the file: it goes in as
    // quarters, the fallback the transport bar applies to one.
    const int unit = timing.beatUnit;
    conductor.timeSignatureNumerator   = timing.beatsPerBar >= 1 && timing.beatsPerBar <= 255 ? timing.beatsPerBar : 4;
    conductor.timeSignatureDenominator = unit >= 1 && unit <= 64 && (unit & (unit - 1)) == 0 ? unit : 4;

    // The importer counts meta events toward a region's length, so a tempo change
    // past the last event would stretch the region a re-import makes.
    const auto& points = timing.tempoMap.points();
    conductor.tempos.front().microsecondsPerQuarter
        = microsecondsPerQuarter (points.empty() ? timing.bpm : points.front().bpm);
    for (std::size_t i = 1; i < points.size(); ++i)
    {
        const auto tick = timing.tempoMap.samplesToTicks (points[i].timelineSamples, timing.sampleRate);
        if (tick > lastTick) break;
        conductor.tempos.push_back ({ tick, microsecondsPerQuarter (points[i].bpm) });
    }
    return conductor;
}

std::vector<std::uint8_t> encode (const Timing& timing, std::string conductorName,
                                  const std::vector<MidiFileTrack>& tracks)
{
    std::int64_t lastTick = 0;
    for (const auto& track : tracks)
        for (const auto& event : track.events)
            lastTick = std::max (lastTick, event.tick);
    return midi::writeMidiFileData (kMidiTicksPerQuarter,
                                    conductorFor (timing, lastTick, std::move (conductorName)), tracks);
}

std::string regionTrackName (std::size_t number, const MidiRegion& region, std::size_t take)
{
    auto name = "Region " + std::to_string (number);
    const auto label = region.label.toStdString();
    if (! label.empty()) name += " - " + label;
    if (take > 0) name += ", take " + std::to_string (take);
    if (region.muted) name += " (muted)";
    return name;
}

std::vector<MidiFileTrack> tracksFor (const Timing& timing, const std::vector<MidiRegion>& regions, TrackFile kind)
{
    std::vector<const MidiRegion*> order;
    order.reserve (regions.size());
    for (const auto& region : regions)
        order.push_back (&region);
    std::stable_sort (order.begin(), order.end(), [] (const MidiRegion* a, const MidiRegion* b)
                      { return a->timelineStart < b->timelineStart; });

    std::vector<MidiFileTrack> tracks;
    const auto add = [&tracks] (std::string name, std::int64_t startTick,
                                const std::vector<MidiNote>& notes, const std::vector<MidiCc>& ccs)
    {
        tracks.push_back ({ std::move (name), {} });
        appendPass (tracks.back(), startTick, notes, ccs);
    };
    for (std::size_t r = 0; r < order.size(); ++r)
    {
        const auto& region = *order[r];
        const auto startTick = regionStartTick (timing, region.timelineStart);
        if (kind == TrackFile::Playing)
        {
            if (! region.muted) add (regionTrackName (r + 1, region, 0), startTick, region.notes, region.ccs);
            continue;
        }
        if (region.muted)
            add (regionTrackName (r + 1, region, region.previousTakes.empty() ? 0 : 1), startTick,
                 region.notes, region.ccs);
        for (std::size_t k = 0; k < region.previousTakes.size(); ++k)
            add (regionTrackName (r + 1, region, k + 2), startTick,
                 region.previousTakes[k].notes, region.previousTakes[k].ccs);
    }
    return tracks;
}

// The length of the well-formed UTF-8 sequence at `at`, 0 when none starts there.
std::size_t sequenceLength (std::string_view text, std::size_t at) noexcept
{
    const auto byteAt = [&] (std::size_t k) -> unsigned { return static_cast<unsigned char> (text[at + k]); };
    const unsigned lead = byteAt (0);
    if (lead < 0x80) return 1;

    std::size_t length = 0;
    unsigned low = 0x80, high = 0xbf;
    if (lead >= 0xc2 && lead <= 0xdf)      length = 2;
    else if (lead == 0xe0)                 { length = 3; low = 0xa0; }
    else if (lead == 0xed)                 { length = 3; high = 0x9f; }
    else if (lead >= 0xe1 && lead <= 0xef) length = 3;
    else if (lead == 0xf0)                 { length = 4; low = 0x90; }
    else if (lead == 0xf4)                 { length = 4; high = 0x8f; }
    else if (lead >= 0xf1 && lead <= 0xf3) length = 4;
    else return 0;

    if (text.size() - at < length || byteAt (1) < low || byteAt (1) > high) return 0;
    for (std::size_t k = 2; k < length; ++k)
        if (byteAt (k) < 0x80 || byteAt (k) > 0xbf) return 0;
    return length;
}

std::string trackTag (int trackNumber)
{
    char tag[32] {};
    std::snprintf (tag, sizeof (tag), "_track%02d_", trackNumber);
    return tag;
}

std::string_view extensionOf (TrackFile kind) noexcept
{
    return kind == TrackFile::Takes ? std::string_view { ".takes.mid" } : std::string_view { ".mid" };
}

std::string fileNameFor (const std::string& stamp, int trackNumber, const std::string& sanitized,
                         TrackFile kind, int copy)
{
    auto name = stamp + trackTag (trackNumber) + sanitized;
    if (copy > 1)
    {
        char suffix[16] {};
        std::snprintf (suffix, sizeof (suffix), "_%03d", copy);
        name += suffix;
    }
    name += extensionOf (kind);
    return name;
}

// The copy number fileNameFor gave `name` for a stamp, this track tag, this
// sanitized name and this kind; 0 for any other file. A playing file of a track
// named "X.takes" reads as a takes file of a track "X" with the same number, and
// the other way round. Their conductors carry the two track names, so their
// bytes differ, and taking one for the other costs one extra file, never a
// missed one.
int copyOf (std::string_view name, std::string_view tagAndName, TrackFile kind) noexcept
{
    const auto extension = extensionOf (kind);
    if (! isStamp (name) || name.size() < kStampLength + tagAndName.size() + extension.size()
        || name.substr (kStampLength, tagAndName.size()) != tagAndName
        || name.substr (name.size() - extension.size()) != extension)
        return 0;

    const auto rest = name.substr (kStampLength + tagAndName.size(),
                                   name.size() - kStampLength - tagAndName.size() - extension.size());
    if (rest.empty()) return 1;
    if (rest.size() != 4 || rest[0] != '_') return 0;
    int copy = 0;
    for (const char c : rest.substr (1))
    {
        if (c < '0' || c > '9') return 0;
        copy = copy * 10 + (c - '0');
    }
    return copy >= 2 ? copy : 0;
}

int copyOfFile (const stdfs::path& folder, const std::string& name, int trackNumber,
                const std::string& sanitized, TrackFile kind)
{
    if (const int copy = copyOf (name, trackTag (trackNumber) + sanitized, kind); copy != 0) return copy;
    const auto ascii = [] (const std::string& text)
    {
        return std::all_of (text.begin(), text.end(), [] (unsigned char c) { return c < 0x80; });
    };
    if (ascii (name) && ascii (sanitized)) return 0;
    const auto extension = extensionOf (kind);
    if (! isStamp (name) || name.size() < kStampLength + extension.size()
        || name.compare (name.size() - extension.size(), extension.size(), extension) != 0)
        return 0;

    // HFS+ lists decomposed names even when the requested track name is NFC.
    // Let the filesystem decide equivalence, retaining its actual spelling.
    const auto matches = [&] (int copy)
    {
        std::error_code error;
        const auto requested = folder / stdfs::u8path (fileNameFor (name.substr (0, kStampLength),
                                                                  trackNumber, sanitized, kind, copy));
        return stdfs::equivalent (dusk::fs::nativeIOPath (folder / stdfs::u8path (name)),
                                  dusk::fs::nativeIOPath (requested), error);
    };
    if (matches (1)) return 1;
    const auto end = name.size() - extension.size();
    if (end < kStampLength + 4 || name[end - 4] != '_') return 0;
    int copy = 0;
    for (std::size_t i = end - 3; i < end; ++i)
    {
        if (name[i] < '0' || name[i] > '9') return 0;
        copy = copy * 10 + name[i] - '0';
    }
    return copy >= 2 && matches (copy) ? copy : 0;
}

// The newest of `names` for this track, sanitized name and kind: the latest
// stamp, and in that second the highest copy.
std::optional<std::string> newestOf (const stdfs::path& folder, const std::vector<std::string>& names,
                                     int trackNumber, const std::string& sanitized, TrackFile kind)
{
    const std::string* newest = nullptr;
    int newestCopy = 0;
    for (const auto& name : names)
    {
        const int copy = copyOfFile (folder, name, trackNumber, sanitized, kind);
        if (copy == 0) continue;
        const int order = newest == nullptr ? 1 : name.compare (0, kStampLength, *newest, 0, kStampLength);
        if (order > 0 || (order == 0 && copy > newestCopy))
        {
            newest = &name;
            newestCopy = copy;
        }
    }
    if (newest == nullptr) return std::nullopt;
    return *newest;
}

std::vector<std::string> fileNamesIn (const stdfs::path& folder, WriteResult& result)
{
    std::vector<std::string> names;
    std::error_code error;
    stdfs::directory_iterator it (dusk::fs::nativeIOPath (folder), error);
    if (error == std::errc::no_such_file_or_directory) return names;
    for (; ! error && it != stdfs::directory_iterator(); it.increment (error))
        names.push_back (it->path().filename().u8string());
    if (error) result.failures.push_back ({ folder, error });
    return names;
}

bool holds (const stdfs::path& file, const std::vector<std::uint8_t>& bytes)
{
    if (dusk::fs::fileSize (file) != (std::int64_t) bytes.size()) return false;
    const auto content = dusk::fs::loadFileAsString (file);
    return content.size() == bytes.size()
        && std::equal (bytes.begin(), bytes.end(), content.begin(),
                       [] (std::uint8_t a, char b) { return a == static_cast<std::uint8_t> (b); });
}

bool makeFolder (const stdfs::path& sessionDir, const stdfs::path& folder, WriteResult& result)
{
    std::error_code error;
    if (! stdfs::is_directory (dusk::fs::nativeIOPath (sessionDir), error))
    {
        result.failures.push_back ({ sessionDir, error ? error : std::make_error_code (std::errc::not_a_directory) });
        return false;
    }
    // One level at a time: a session folder removed while a file waited to be
    // written must not come back as the parent of its midi folder.
    for (const auto& level : { folder.parent_path(), folder })
    {
        stdfs::create_directory (dusk::fs::nativeIOPath (level), error);
        if (! error) continue;
        result.failures.push_back ({ level, error });
        return false;
    }
    return true;
}

// Preserve filename order after a clock rollback or exhausted copy suffix.
void writeNew (WriteResult& result, const stdfs::path& sessionDir, const stdfs::path& folder,
               std::chrono::system_clock::time_point now, int trackNumber, const std::string& sanitized,
               TrackFile kind, const std::vector<std::uint8_t>& bytes, const std::optional<std::string>& newest)
{
    if (bytes.empty())
    {
        result.failures.push_back ({ folder, std::make_error_code (std::errc::invalid_argument) });
        return;
    }
    if (! makeFolder (sessionDir, folder, result)) return;

    auto filenameTime = std::chrono::floor<std::chrono::seconds> (now);
    int firstCopy = 1;
    if (newest)
        if (const auto newestTime = recoverystamp::timeOfStamp (*newest);
            newestTime && *newestTime >= filenameTime)
        {
            filenameTime = *newestTime;
            firstCopy = copyOfFile (folder, *newest, trackNumber, sanitized, kind) + 1;
        }
    for (int second = 0; second < kMaxSeconds; ++second)
    {
        const auto stamp = utcStamp (filenameTime + std::chrono::seconds (second));
        for (int copy = second == 0 ? firstCopy : 1; copy <= kMaxCopies; ++copy)
        {
            const auto target = folder / stdfs::u8path (fileNameFor (stamp, trackNumber, sanitized, kind, copy));
            std::error_code error;
            const auto status = stdfs::symlink_status (dusk::fs::nativeIOPath (target), error);
            if (status.type() != stdfs::file_type::not_found)
            {
                if (! error) continue;
                result.failures.push_back ({ target, error });
                return;
            }
            if (dusk::fs::writeFileAtomically (target, bytes.data(), bytes.size(), error, dusk::fs::AtomicWriteMode::CreateNew))
                result.written.push_back (target);
            else if (error == std::errc::file_exists)
                continue;
            else
                result.failures.push_back ({ target, error });
            return;
        }
    }
    result.failures.push_back ({ folder, std::make_error_code (std::errc::file_exists) });
}

template <typename Work>
WriteResult guarded (const stdfs::path& sessionDir, Work&& work) noexcept
{
    WriteResult result;
    try
    {
        work (result);
    }
    catch (const stdfs::filesystem_error& failure)
    {
        result.failures.push_back ({ failure.path1().empty() ? sessionDir : failure.path1(), failure.code() });
    }
    catch (const std::bad_alloc&)
    {
        result.failures.push_back ({ sessionDir, std::make_error_code (std::errc::not_enough_memory) });
    }
    catch (...)
    {
        result.failures.push_back ({ sessionDir, std::make_error_code (std::errc::io_error) });
    }
    return result;
}
} // namespace

Timing timingOf (const Session& session, double sampleRate)
{
    Timing timing;
    timing.tempoMap    = session.tempoMap;
    timing.bpm         = session.tempoBpm.load (std::memory_order_relaxed);
    timing.sampleRate  = sampleRate;
    timing.beatsPerBar = session.beatsPerBar.load (std::memory_order_relaxed);
    timing.beatUnit    = session.beatUnit.load (std::memory_order_relaxed);
    return timing;
}

std::vector<std::uint8_t> buildTakeFile (const Timing& timing, const MidiRegion& pass,
                                         const std::string& trackName)
{
    std::vector<MidiFileTrack> tracks (1);
    tracks.front().name = trackName;
    appendPass (tracks.front(), regionStartTick (timing, pass.timelineStart), pass.notes, pass.ccs);
    return encode (timing, {}, tracks);
}

std::vector<std::uint8_t> buildPlayingFile (const Timing& timing, const std::vector<MidiRegion>& regions,
                                            const std::string& trackName)
{
    return encode (timing, trackName, tracksFor (timing, regions, TrackFile::Playing));
}

std::vector<std::uint8_t> buildTakesFile (const Timing& timing, const std::vector<MidiRegion>& regions,
                                          const std::string& trackName)
{
    return encode (timing, trackName, tracksFor (timing, regions, TrackFile::Takes));
}

std::string sanitizeName (std::string_view name)
{
    static constexpr std::string_view kForbidden { "/\\:*?\"<>|" };
    std::string out;
    out.reserve (name.size());
    for (std::size_t at = 0; at < name.size();)
    {
        const auto length = sequenceLength (name, at);
        const auto lead = static_cast<unsigned char> (name[at]);
        const bool control = lead < 0x20 || lead == 0x7f
            || (length == 2 && lead == 0xc2 && static_cast<unsigned char> (name[at + 1]) < 0xa0);
        if (length == 0 || control || (length == 1 && kForbidden.find (name[at]) != std::string_view::npos))
            out += '_';
        else
            out.append (name.substr (at, length));
        at += std::max<std::size_t> (length, 1);
    }

    if (out.size() > kMaxNameBytes)
    {
        auto cut = kMaxNameBytes;
        while (cut > 0 && (static_cast<unsigned char> (out[cut]) & 0xc0) == 0x80)
            --cut;
        out.resize (cut);
    }
    while (! out.empty() && (out.back() == '.' || out.back() == ' '))
        out.pop_back();
    return out.empty() ? std::string (kUnnamed) : out;
}

std::string fileName (std::chrono::system_clock::time_point now, int trackNumber,
                      std::string_view trackName, TrackFile kind, int copy)
{
    return fileNameFor (utcStamp (now), trackNumber, sanitizeName (trackName), kind, copy);
}

std::optional<PendingFile> pendingTakeFile (const std::filesystem::path& sessionDir,
                                            std::chrono::system_clock::time_point made,
                                            const Session& session, double sampleRate, int trackIndex,
                                            const MidiRegion& pass)
{
    if (trackIndex < 0 || trackIndex >= Session::kNumTracks || (pass.notes.empty() && pass.ccs.empty()))
        return std::nullopt;
    const auto name = session.track (trackIndex).nameUtf8();
    return PendingFile { sessionDir, made, trackIndex + 1, sanitizeName (name), TrackFile::Playing, Folder::Takes,
                         buildTakeFile (timingOf (session, sampleRate), pass, name) };
}

std::vector<PendingFile> pendingTrackFiles (const std::filesystem::path& sessionDir,
                                            std::chrono::system_clock::time_point made,
                                            const Session& session, double sampleRate)
{
    std::vector<PendingFile> files;
    const auto timing = timingOf (session, sampleRate);
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        const auto& track = session.track (t);
        const auto& regions = track.midiRegions.current();
        if (regions.empty()) continue;

        const auto name = track.nameUtf8();
        for (const auto kind : { TrackFile::Playing, TrackFile::Takes })
        {
            const auto tracks = tracksFor (timing, regions, kind);
            if (tracks.empty()) continue;
            files.push_back ({ sessionDir, made, t + 1, sanitizeName (name), kind, Folder::Tracks,
                               encode (timing, name, tracks) });
        }
    }
    return files;
}

WriteResult writeFile (const PendingFile& file) noexcept
{
    return guarded (file.sessionDir, [&] (WriteResult& result)
    {
        const auto folder = file.sessionDir / "midi" / (file.folder == Folder::Takes ? "takes" : "tracks");
        const auto newest = newestOf (folder, fileNamesIn (folder, result), file.trackNumber, file.name, file.kind);
        if (file.folder == Folder::Tracks && ! file.bytes.empty()
            && newest && holds (folder / stdfs::u8path (*newest), file.bytes)) return;
        writeNew (result, file.sessionDir, folder, file.made, file.trackNumber, file.name, file.kind, file.bytes, newest);
    });
}
} // namespace duskstudio::midirecovery
