#include "MidiRecovery.h"

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

// The writer refuses a gap of more than 0x0FFFFFFF ticks, and a note can end one
// tick after the last place a note-on can take.
constexpr std::int64_t kLastPlacedTick = 0x0ffffffe;
constexpr std::size_t  kMaxNameBytes   = 64;
constexpr std::size_t  kStampLength    = 16;
constexpr int          kMaxCopies      = 1000;
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

std::string utcStamp (std::chrono::system_clock::time_point now)
{
    const auto seconds = (long long) std::chrono::floor<std::chrono::seconds> (now.time_since_epoch()).count();
    const auto days = (seconds >= 0 ? seconds : seconds - 86399) / 86400;
    const auto ofDay = seconds - days * 86400;

    // Howard Hinnant's civil_from_days.
    const auto shifted = days + 719468;
    const auto era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
    const auto dayOfEra = shifted - era * 146097;
    const auto yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const auto dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const auto monthIndex = (5 * dayOfYear + 2) / 153;
    const auto day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
    const auto month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
    const auto year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);

    char stamp[128] {};
    std::snprintf (stamp, sizeof (stamp), "%04lld%02lld%02lld-%02lld%02lld%02lldZ",
                   year, month, day, ofDay / 3600, ofDay / 60 % 60, ofDay % 60);
    return stamp;
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
    if (copy > 1) name += "_" + std::to_string (copy);
    name += extensionOf (kind);
    return name;
}

bool isStamp (std::string_view name) noexcept
{
    if (name.size() < kStampLength) return false;
    for (std::size_t i = 0; i < kStampLength; ++i)
    {
        const char c = name[i];
        if (i == 8 ? c != '-' : i == kStampLength - 1 ? c != 'Z' : (c < '0' || c > '9')) return false;
    }
    return true;
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
    if (rest.size() < 2 || rest.size() > 5 || rest[0] != '_' || rest[1] == '0') return 0;
    int copy = 0;
    for (const char c : rest.substr (1))
    {
        if (c < '0' || c > '9') return 0;
        copy = copy * 10 + (c - '0');
    }
    return copy >= 2 ? copy : 0;
}

// The newest of `names` for this track, sanitized name and kind: the latest
// stamp, and in that second the highest copy.
std::optional<std::string> newestOf (const std::vector<std::string>& names, int trackNumber,
                                     const std::string& sanitized, TrackFile kind)
{
    const auto tagAndName = trackTag (trackNumber) + sanitized;
    const std::string* newest = nullptr;
    int newestCopy = 0;
    for (const auto& name : names)
    {
        const int copy = copyOf (name, tagAndName, kind);
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
    stdfs::create_directories (dusk::fs::nativeIOPath (folder), error);
    if (! error) return true;
    result.failures.push_back ({ folder, error });
    return false;
}

// Writes `bytes` under the first name for this second that nothing in `folder`
// holds yet, so no file there is ever replaced.
void writeNew (WriteResult& result, const stdfs::path& sessionDir, const stdfs::path& folder,
               std::chrono::system_clock::time_point now, int trackNumber, const std::string& sanitized,
               TrackFile kind, const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty())
    {
        result.failures.push_back ({ folder, std::make_error_code (std::errc::invalid_argument) });
        return;
    }
    if (! makeFolder (sessionDir, folder, result)) return;

    const auto stamp = utcStamp (now);
    for (int copy = 1; copy <= kMaxCopies; ++copy)
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
        if (dusk::fs::writeFileAtomically (target, bytes.data(), bytes.size(), error))
            result.written.push_back (target);
        else
            result.failures.push_back ({ target, error });
        return;
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

WriteResult writeTakeFile (const std::filesystem::path& sessionDir, std::chrono::system_clock::time_point now,
                           const Session& session, double sampleRate, int trackIndex,
                           const MidiRegion& pass) noexcept
{
    return guarded (sessionDir, [&] (WriteResult& result)
    {
        const auto folder = sessionDir / "midi" / "takes";
        if (trackIndex < 0 || trackIndex >= Session::kNumTracks)
        {
            result.failures.push_back ({ folder, std::make_error_code (std::errc::invalid_argument) });
            return;
        }
        if (pass.notes.empty() && pass.ccs.empty()) return;

        const auto name = session.track (trackIndex).nameUtf8();
        writeNew (result, sessionDir, folder, now, trackIndex + 1, sanitizeName (name), TrackFile::Playing,
                  buildTakeFile (timingOf (session, sampleRate), pass, name));
    });
}

WriteResult writeChangedTrackFiles (const std::filesystem::path& sessionDir,
                                    std::chrono::system_clock::time_point now,
                                    const Session& session, double sampleRate) noexcept
{
    return guarded (sessionDir, [&] (WriteResult& result)
    {
        const auto folder = sessionDir / "midi" / "tracks";
        const auto timing = timingOf (session, sampleRate);
        std::optional<std::vector<std::string>> existing;
        for (int t = 0; t < Session::kNumTracks; ++t)
        {
            const auto& track = session.track (t);
            const auto& regions = track.midiRegions.current();
            if (regions.empty()) continue;
            if (! existing) existing = fileNamesIn (folder, result);

            const auto name = track.nameUtf8();
            const auto sanitized = sanitizeName (name);
            for (const auto kind : { TrackFile::Playing, TrackFile::Takes })
            {
                const auto tracks = tracksFor (timing, regions, kind);
                if (tracks.empty()) continue;
                const auto bytes = encode (timing, name, tracks);
                const auto newest = newestOf (*existing, t + 1, sanitized, kind);
                if (newest && ! bytes.empty() && holds (folder / stdfs::u8path (*newest), bytes)) continue;
                writeNew (result, sessionDir, folder, now, t + 1, sanitized, kind, bytes);
            }
        }
    });
}
} // namespace duskstudio::midirecovery
