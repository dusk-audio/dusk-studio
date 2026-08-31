#include "FileImporter.h"

#include "../foundation/Text.h"
#include "audiofile/FileReader.h"
#include "audiofile/FileWriter.h"
#include "midi/MidiFileReader.h"
#include "../foundation/PlanarBuffer.h"
#include "../foundation/WindowedSincInterpolator.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <thread>
#include <utility>
#include <vector>

namespace duskstudio::fileimport
{
namespace
{
using namespace std::chrono_literals;

void waitForTransientFileLock()
{
    std::this_thread::sleep_for (20ms);
}

template <typename FileType>
std::filesystem::path toStdPath (const FileType& file)
{
    return std::filesystem::u8path (file.getFullPathName().toStdString());
}

// Generated filename pattern - mirrors RecordManager::createFilename's
// "track{NN}_{timestamp}.wav" so imports sit next to recordings in the
// session's audio directory and aren't visually distinct in the file
// listing. "import_" prefix is the only differentiator.
std::string makeImportedFilename (int trackIndex, const juce::String& extension = ".wav")
{
    const auto now = juce::Time::getCurrentTime();
    return dusk::text::format ("import_track%02d_%04d%02d%02d-%02d%02d%02d%s",
                               trackIndex + 1,
                               now.getYear(), now.getMonth() + 1, now.getDayOfMonth(),
                               now.getHours(), now.getMinutes(), now.getSeconds(),
                               extension.toRawUTF8());
}

// Channel-conform the first `numSamples` of `src` into `dst` (both pre-sized
// to at least numSamples). Handles 1->2 duplicate, 2->1 sum-and-halve, and
// pass-through. No allocation; caller owns both buffers.
void conformChunk (const dusk::audio::PlanarBuffer& src,
                    dusk::audio::PlanarBuffer& dst,
                    int targetChannels,
                    int numSamples)
{
    const int srcCh = src.numChannels();
    const int n     = numSamples;
    jassert (dst.numChannels() == targetChannels);
    jassert (dst.numSamples()  >= n && src.numSamples() >= n);

    if (srcCh == 1 && targetChannels == 2)
    {
        dusk::audio::vecCopy (dst.channel (0), src.channel (0), n);
        dusk::audio::vecCopy (dst.channel (1), src.channel (0), n);
        return;
    }
    if (srcCh >= 2 && targetChannels == 1)
    {
        const float* l = src.channel (0);
        const float* r = src.channel (1);
        float*       d = dst.channel (0);
        for (int i = 0; i < n; ++i)
            d[i] = 0.5f * (l[i] + r[i]);
        return;
    }
    // Pass-through: copy as many channels as both sides have.
    const int common = std::min (srcCh, targetChannels);
    for (int c = 0; c < common; ++c)
        dusk::audio::vecCopy (dst.channel (c), src.channel (c), n);
}
} // namespace

AudioImportResult importAudio (const AudioImportRequest& req)
{
    AudioImportResult result;

    if (! req.source.existsAsFile())
    {
        result.errorMessage = ("Source file does not exist: " + req.source.getFullPathName()).toStdString();
        return result;
    }
    // Session sample rate can legitimately be zero at import time when
    // the user opened the project before an audio device finished
    // initialising. Fall back to 48 kHz so the resample target is still
    // a sane value - the device-open path will recompute downstream
    // lengthInSamples when the live SR is known.
    double sessionSr = req.sessionSampleRate;
    if (! std::isfinite (sessionSr) || sessionSr <= 0.0) sessionSr = 48000.0;
    if (req.targetChannels < 1 || req.targetChannels > 2)
    {
        result.errorMessage = "Target channel count must be 1 or 2";
        return result;
    }
    if (! req.audioDir.isDirectory())
    {
        const auto created = req.audioDir.createDirectory();
        if (created.failed())
        {
            result.errorMessage = ("Could not create audio directory: "
                                + created.getErrorMessage()).toStdString();
            return result;
        }
    }

    // Bounded retry: on Windows a file that was just written/downloaded can be
    // transiently locked by the indexer or AV real-time scan, so the first open
    // returns null even though the file is valid. Retry briefly before treating
    // it as unreadable; genuine unsupported files just exhaust the attempts.
    auto reader = detail::retryTransientFileOperation (
        [&] { return dusk::audio::FileReader::open (toStdPath (req.source)); },
        waitForTransientFileLock);
    if (reader == nullptr)
    {
        result.errorMessage = ("Unsupported or unreadable audio file: "
                            + req.source.getFileName()).toStdString();
        return result;
    }

    const auto srcSampleRate = reader->info().sampleRate;
    const auto srcLength     = reader->info().numFrames;
    const auto srcChannels   = reader->info().numChannels;

    if (srcSampleRate <= 0.0 || srcLength <= 0 || srcChannels <= 0)
    {
        result.errorMessage = "Audio file reports an empty or invalid stream";
        return result;
    }
    if (srcLength > kMaxImportSamplesPerChannel)
    {
        result.errorMessage = "Audio file too long for import";
        return result;
    }

    // Faithful fast path: when the source already matches the session's sample
    // rate AND the requested channel layout, copy it in verbatim - no decode,
    // no resample, no bit-depth change - so an import never alters audio the
    // user didn't ask to change (a 16-bit or 32-float source is preserved
    // exactly, in its original format). Only an actual rate or channel conform
    // falls through to the decode + re-encode path below.
    if (std::abs (srcSampleRate - sessionSr) <= 0.001
        && srcChannels == req.targetChannels
        && srcChannels >= 1 && srcChannels <= 2
        && req.source.getFileExtension().isNotEmpty())   // no extension -> can't safely
    {                                                     // name a verbatim copy; re-encode below
        reader.reset();   // release the read handle before copying the bytes

        const auto ext = req.source.getFileExtension();
        // The stamp has one-second resolution: two imports to the same track
        // within a second would silently overwrite, leaving both regions
        // pointing at one file. Suffix like RecordManager does.
        auto outFile = req.audioDir.getChildFile (
            makeImportedFilename (req.trackIndex, ext));
        if (outFile.exists())
            outFile = req.audioDir.getNonexistentChildFile (
                outFile.getFileNameWithoutExtension(), ext);

        const bool copied = detail::retryTransientFileOperation (
            [&] { return req.source.copyFileTo (outFile); },
            waitForTransientFileLock);
        if (! copied)
        {
            outFile.deleteFile();   // drop any partial copy (matches the slow path)
            result.errorMessage = ("Could not copy the file into the session audio folder: "
                                + outFile.getFullPathName()).toStdString();
            return result;
        }

        result.ok = true;
        result.region.file            = outFile;
        result.region.timelineStart   = req.timelineStart;
        result.region.lengthInSamples = srcLength;
        result.region.sourceOffset    = 0;
        result.region.numChannels     = srcChannels;
        return result;
    }

    const bool needsResample = std::abs (srcSampleRate - sessionSr) > 0.001;
    std::int64_t outLength = srcLength;
    if (needsResample)
    {
        outLength = (std::int64_t) std::llround ((double) srcLength
                                                 * sessionSr / srcSampleRate);
        if (outLength <= 0)
        {
            result.errorMessage = "Resample produced empty output";
            return result;
        }
        if (outLength > kMaxImportSamplesPerChannel)
        {
            result.errorMessage = "Resampled output too long for import";
            return result;
        }
    }

    // Write the normalised WAV. Same transient-lock retry as the source open:
    // a freshly-created file in the audio dir can be briefly held by the
    // Windows indexer / AV before the stream opens.
    // Same second-resolution collision guard as the verbatim-copy path.
    auto outFile = req.audioDir.getChildFile (makeImportedFilename (req.trackIndex));
    if (outFile.exists())
        outFile = req.audioDir.getNonexistentChildFile (
            outFile.getFileNameWithoutExtension(), ".wav");
    dusk::audio::WriteSpec writeSpec;
    writeSpec.sampleRate    = sessionSr;
    writeSpec.numChannels   = req.targetChannels;
    writeSpec.bitsPerSample = 24;
    writeSpec.format        = dusk::audio::WriteSpec::Format::Wav;
    auto writer = detail::retryTransientFileOperation (
        [&] { return dusk::audio::FileWriter::create (toStdPath (outFile), writeSpec); },
        waitForTransientFileLock);
    if (writer == nullptr)
    {
        result.errorMessage = ("Could not open output file for writing: "
                            + outFile.getFullPathName()).toStdString();
        return result;
    }

    auto readChunk = [&reader, srcChannels] (dusk::audio::PlanarBuffer& buffer,
                                              std::int64_t start, int frames)
    {
        return reader->read (buffer.data(), srcChannels, start, frames) == frames;
    };
    auto writeChunk = [&writer] (const dusk::audio::PlanarBuffer& buffer,
                                  int start, int frames)
    {
        std::array<const float*, 2> channels {};
        for (int c = 0; c < buffer.numChannels(); ++c)
            channels[(size_t) c] = buffer.channel (c) + start;
        return writer->write (channels.data(), buffer.numChannels(), frames);
    };

    // Stream decode -> conform -> (sinc-)resample -> write in bounded chunks.
    // The old whole-file path allocated three full-length buffers (a 30-min
    // 96 kHz stereo stem needed ~1.4 GB before the writer even opened) and
    // froze the message thread on the allocation; this loop peaks at a few
    // hundred KB regardless of source length.
    constexpr int kGrain = 65536;
    dusk::audio::PlanarBuffer srcChunk, outChunk;
    srcChunk.setSize (srcChannels,        kGrain);
    outChunk.setSize (req.targetChannels, kGrain);
    bool wrote = true;

    if (! needsResample)
    {
        dusk::audio::PlanarBuffer confChunk;
        confChunk.setSize (req.targetChannels, kGrain);
        std::int64_t pos = 0;
        while (pos < srcLength && wrote)
        {
            const int n = (int) std::min ((std::int64_t) kGrain, srcLength - pos);
            srcChunk.clear();
            if (! readChunk (srcChunk, pos, n))
            {
                wrote = false;
                break;
            }
            conformChunk (srcChunk, confChunk, req.targetChannels, n);
            wrote = writeChunk (confChunk, 0, n);
            pos += n;
        }
    }
    else
    {
        // Streaming windowed-sinc resample. `carry` holds conformed source
        // samples not yet consumed by the interpolators; each pass tops it up
        // to the worst-case input need for one output grain, pads with
        // silence past EOF so the sinc tail flushes, then drops what the
        // interpolators consumed.
        const double ratio = srcSampleRate / sessionSr;
        const int needIn   = (int) std::ceil ((double) kGrain * ratio)
                               + (int) dusk::audio::WindowedSincInterpolator::getBaseLatency() + 8;
        dusk::audio::PlanarBuffer confChunk, carry;
        confChunk.setSize (req.targetChannels, kGrain);
        carry.setSize     (req.targetChannels, needIn + kGrain);
        std::array<dusk::audio::WindowedSincInterpolator, 2> interp;
        for (auto& i : interp) i.reset();

        // The sinc kernel delays its output by getBaseLatency() INPUT samples;
        // without discarding that from the head, every resampled import lands
        // ~2 ms late on the timeline (and the tail gets truncated by the same
        // amount). Discard the equivalent output-domain prefix; EOF zero-pad
        // above supplies the extra input the tail needs.
        std::int64_t discard = (std::int64_t) std::llround (
            (double) dusk::audio::WindowedSincInterpolator::getBaseLatency() / ratio);

        int         carryLen = 0;
        std::int64_t srcPos   = 0;
        std::int64_t produced = 0;
        while (produced < outLength && wrote)
        {
            // Top up the carry from the source.
            while (carryLen < needIn && srcPos < srcLength)
            {
                const int n = (int) std::min ((std::int64_t) kGrain, srcLength - srcPos);
                srcChunk.clear();
                if (! readChunk (srcChunk, srcPos, n))
                {
                    wrote = false;
                    break;
                }
                conformChunk (srcChunk, confChunk, req.targetChannels, n);
                const int room = std::min (n, carry.numSamples() - carryLen);
                for (int c = 0; c < req.targetChannels; ++c)
                    dusk::audio::vecCopy (carry.channel (c) + carryLen,
                                           confChunk.channel (c), room);
                carryLen += room;
                srcPos   += room;
            }
            if (! wrote) break;
            if (carryLen < needIn)   // EOF: silence-pad so the tail flushes
            {
                for (int c = 0; c < req.targetChannels; ++c)
                    dusk::audio::vecClear (carry.channel (c) + carryLen,
                                            needIn - carryLen);
                carryLen = needIn;
            }

            const int nOut = (int) std::min ((std::int64_t) kGrain,
                                              (outLength - produced) + discard);
            int consumed = 0;
            for (int c = 0; c < req.targetChannels; ++c)
                consumed = interp[(size_t) c].process (ratio,
                                                        carry.channel (c),
                                                        outChunk.channel (c),
                                                        nOut);
            const int skip       = (int) std::min ((std::int64_t) nOut, discard);
            const int writeCount = nOut - skip;
            if (writeCount > 0)
                wrote = writeChunk (outChunk, skip, writeCount);
            discard  -= skip;
            produced += writeCount;

            consumed = std::clamp (consumed, 0, carryLen);
            for (int c = 0; c < req.targetChannels; ++c)
            {
                auto* p = carry.channel (c);
                std::memmove (p, p + consumed,
                               sizeof (float) * (size_t) (carryLen - consumed));
            }
            carryLen -= consumed;
        }
    }

    writer.reset();   // flush + close before we read the file back
    if (! wrote)
    {
        outFile.deleteFile();
        result.errorMessage = "Audio decode or write failed";
        return result;
    }

    result.ok = true;
    result.region.file            = outFile;
    result.region.timelineStart   = req.timelineStart;
    result.region.lengthInSamples = outLength;
    result.region.sourceOffset    = 0;
    result.region.numChannels     = req.targetChannels;
    return result;
}

namespace
{
// Rescale a tick value from one PPQ resolution to Dusk Studio's canonical
// kMidiTicksPerQuarter. Rounds rather than truncates so the cumulative
// drift across a long region stays bounded.
std::int64_t rescaleTicks (std::int64_t srcTicks, int srcPPQ) noexcept
{
    if (srcPPQ <= 0) return srcTicks;
    if (srcPPQ == kMidiTicksPerQuarter) return srcTicks;
    return (std::int64_t) std::llround ((double) srcTicks
                                          * (double) kMidiTicksPerQuarter
                                          / (double) srcPPQ);
}
} // namespace

MidiImportResult importMidi (const MidiImportRequest& req)
{
    MidiImportResult result;

    if (! req.source.existsAsFile())
    {
        result.errorMessage = ("MIDI file does not exist: " + req.source.getFullPathName()).toStdString();
        return result;
    }
    // MIDI has no inherent sample rate - the importer only needs one to
    // cache the rendered lengthInSamples on the resulting MidiRegion.
    // PlaybackEngine recomputes that cache when the live session SR is
    // known. Fall back to 48 kHz when the session hasn't opened a device
    // yet so a fresh-session import doesn't fail just for asking too
    // early.
    double sessionSr = req.sessionSampleRate;
    if (! std::isfinite (sessionSr) || sessionSr <= 0.0) sessionSr = 48000.0;
    // Upper bound for BPM picked well above anything musically plausible
    // - rejects NaN/inf as well as nonsense values from a hand-edited
    // session.json that would otherwise turn into ridiculous tick-to-
    // sample conversions inside the importer's scheduler math.
    constexpr float kMaxBpm = 999.0f;
    if (! std::isfinite (req.sessionBpm) || req.sessionBpm <= 0.0f
        || req.sessionBpm > kMaxBpm)
    {
        result.errorMessage = "Invalid session tempo";
        return result;
    }

    midi::MidiFileReader mf;
    if (! mf.readFile (toStdPath (req.source)))
    {
        result.errorMessage = "Failed to parse MIDI file";
        return result;
    }

    const int    timeFormat     = mf.timeFormat();
    const bool   isSmpte        = mf.isSmpteTimeFormat();
    const double ticksPerSecond = mf.smpteTicksPerSecond();
    if (isSmpte && ticksPerSecond <= 0.0)
    {
        result.errorMessage = "Unsupported MIDI time format";
        return result;
    }

    // An SMPTE file's ticks are absolute time, so they become project ticks via
    // the session tempo; a PPQ file's ticks are already musical and only need a
    // resolution rescale.
    auto timestampToProjectTicks = [&] (std::int64_t rawTick) -> std::int64_t
    {
        if (isSmpte)
        {
            const double samples = ((double) rawTick / ticksPerSecond) * sessionSr;
            return duskstudio::samplesToTicks ((std::int64_t) std::llround (samples),
                                            sessionSr,
                                            req.sessionBpm);
        }
        return rescaleTicks (rawTick, timeFormat);
    };

    // Merge all tracks into one flat event list. Skip meta events; we
    // don't import tempo / time-sig maps in v1.
    struct ActiveNote
    {
        std::int64_t startTick;
        int velocity;
    };
    // (channel, note) -> stack of open note-ons. MIDI spec allows multiple
    // overlapping note-ons of the same pitch on the same channel.
    std::map<std::pair<int, int>, std::vector<ActiveNote>> open;

    std::vector<MidiNote> notes;
    std::vector<MidiCc>   ccs;
    std::int64_t maxTick = 0;

    for (const auto& track : mf.tracks())
    {
        for (const auto& msg : track)
        {
            const auto tick = timestampToProjectTicks (msg.tick);
            if (tick > maxTick) maxTick = tick;

            if (msg.isNoteOn())
            {
                open[{ msg.channel(), msg.noteNumber() }].push_back ({ tick, msg.velocity() });
            }
            else if (msg.isNoteOff())
            {
                const int ch   = msg.channel();
                const int note = msg.noteNumber();
                auto it = open.find ({ ch, note });
                if (it != open.end() && ! it->second.empty())
                {
                    const auto open_ = it->second.front();
                    it->second.erase (it->second.begin());
                    MidiNote n;
                    n.channel       = ch;
                    n.noteNumber    = note;
                    n.velocity      = std::max (1, open_.velocity);
                    n.startTick     = open_.startTick;
                    n.lengthInTicks = std::max<std::int64_t> (1, tick - open_.startTick);
                    notes.push_back (n);
                }
            }
            else if (msg.isController())
            {
                MidiCc cc;
                cc.channel    = msg.channel();
                cc.controller = msg.controllerNumber();
                cc.value      = msg.controllerValue();
                cc.atTick     = tick;
                ccs.push_back (cc);
            }
            // Meta events / sysex / tempo / time-sig: skipped.
        }
    }

    // Flush any dangling note-ons (missing matching note-off) - synthesise
    // a note-off at maxTick so the region's range still captures them.
    for (auto& [key, stack] : open)
    {
        const auto [ch, note] = key;
        for (const auto& a : stack)
        {
            MidiNote n;
            n.channel       = ch;
            n.noteNumber    = note;
            n.velocity      = std::max (1, a.velocity);
            n.startTick     = a.startTick;
            n.lengthInTicks = std::max<std::int64_t> (1, maxTick - a.startTick);
            notes.push_back (n);
        }
    }

    if (notes.empty() && ccs.empty())
    {
        result.errorMessage = "MIDI file contains no notes or CC events";
        return result;
    }

    result.ok = true;
    result.region.timelineStart   = req.timelineStart;
    result.region.lengthInTicks   = std::max<std::int64_t> (1, maxTick);
    result.region.lengthInSamples = duskstudio::ticksToSamples (result.region.lengthInTicks,
                                                              sessionSr,
                                                              req.sessionBpm);
    // Anchor BPM for tempo-locked retime. Without this, an imported MIDI
    // file defaults to the struct's 120 BPM and a subsequent BPM change
    // in applyTempoChange would scale positions by 120/newBpm instead of
    // sessionBpm/newBpm - silently mis-timing the take.
    result.region.recordedAtBPM   = (double) req.sessionBpm;
    result.region.notes = std::move (notes);
    result.region.ccs   = std::move (ccs);
    return result;
}
} // namespace duskstudio::fileimport
