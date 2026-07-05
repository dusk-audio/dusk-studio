#include "FileImporter.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace duskstudio::fileimport
{
namespace
{
// Generated filename pattern - mirrors RecordManager::createFilename's
// "track{NN}_{timestamp}.wav" so imports sit next to recordings in the
// session's audio directory and aren't visually distinct in the file
// listing. "import_" prefix is the only differentiator.
juce::String makeImportedFilename (int trackIndex, const juce::String& extension = ".wav")
{
    const auto now = juce::Time::getCurrentTime();
    const auto stamp = juce::String::formatted ("%04d%02d%02d-%02d%02d%02d",
                                                  now.getYear(), now.getMonth() + 1, now.getDayOfMonth(),
                                                  now.getHours(), now.getMinutes(), now.getSeconds());
    // No %s through String::formatted: MSVC's wide printf reads a char* as
    // wchar_t* and garbles the name into invalid filename characters.
    return "import_track" + juce::String (trackIndex + 1).paddedLeft ('0', 2)
             + "_" + stamp + extension;
}

// Channel-conform the first `numSamples` of `src` into `dst` (both pre-sized
// to at least numSamples). Handles 1->2 duplicate, 2->1 sum-and-halve, and
// pass-through. No allocation; caller owns both buffers.
void conformChunk (const juce::AudioBuffer<float>& src,
                    juce::AudioBuffer<float>& dst,
                    int targetChannels,
                    int numSamples)
{
    const int srcCh = src.getNumChannels();
    const int n     = numSamples;
    jassert (dst.getNumChannels() == targetChannels);
    jassert (dst.getNumSamples()  >= n && src.getNumSamples() >= n);

    if (srcCh == 1 && targetChannels == 2)
    {
        dst.copyFrom (0, 0, src, 0, 0, n);
        dst.copyFrom (1, 0, src, 0, 0, n);
        return;
    }
    if (srcCh >= 2 && targetChannels == 1)
    {
        // Mix L+R at 0.5 each. juce::AudioBuffer::copyFrom doesn't take
        // a gain when the source is another AudioBuffer, so we clear +
        // accumulate via addFrom (which DOES have the gain overload).
        dst.clear();
        dst.addFrom (0, 0, src, 0, 0, n, 0.5f);
        dst.addFrom (0, 0, src, 1, 0, n, 0.5f);
        return;
    }
    // Pass-through: copy as many channels as both sides have.
    const int common = juce::jmin (srcCh, targetChannels);
    for (int c = 0; c < common; ++c)
        dst.copyFrom (c, 0, src, c, 0, n);
}
} // namespace

AudioImportResult importAudio (const AudioImportRequest& req)
{
    AudioImportResult result;

    if (! req.source.existsAsFile())
    {
        result.errorMessage = "Source file does not exist: " + req.source.getFullPathName();
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
            result.errorMessage = "Could not create audio directory: "
                                + created.getErrorMessage();
            return result;
        }
    }

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    // Bounded retry: on Windows a file that was just written/downloaded can be
    // transiently locked by the indexer or AV real-time scan, so the first open
    // returns null even though the file is valid. Retry briefly before treating
    // it as unreadable; genuine unsupported files just exhaust the attempts.
    std::unique_ptr<juce::AudioFormatReader> reader;
    for (int attempt = 0; attempt < 5 && reader == nullptr; ++attempt)
    {
        reader.reset (fm.createReaderFor (req.source));
        if (reader == nullptr && attempt < 4)
            juce::Thread::sleep (20);
    }
    if (reader == nullptr)
    {
        result.errorMessage = "Unsupported or unreadable audio file: "
                            + req.source.getFileName();
        return result;
    }

    const auto srcSampleRate = reader->sampleRate;
    const auto srcLength     = (juce::int64) reader->lengthInSamples;
    const auto srcChannels   = (int) reader->numChannels;

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
    // rate AND the requested channel layout, copy it in verbatim — no decode,
    // no resample, no bit-depth change — so an import never alters audio the
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

        bool copied = false;
        for (int attempt = 0; attempt < 5 && ! copied; ++attempt)
        {
            copied = req.source.copyFileTo (outFile);
            if (! copied && attempt < 4)
                juce::Thread::sleep (20);
        }
        if (! copied)
        {
            outFile.deleteFile();   // drop any partial copy (matches the slow path)
            result.errorMessage = "Could not copy the file into the session audio folder: "
                                + outFile.getFullPathName();
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
    juce::int64 outLength = srcLength;
    if (needsResample)
    {
        outLength = (juce::int64) std::llround ((double) srcLength
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
    std::unique_ptr<juce::FileOutputStream> stream;
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        stream = outFile.createOutputStream();
        if (stream != nullptr && stream->openedOk())
            break;
        stream.reset();
        if (attempt < 4)
            juce::Thread::sleep (20);
    }
    if (stream == nullptr || ! stream->openedOk())
    {
        result.errorMessage = "Could not open output file for writing: "
                            + outFile.getFullPathName();
        return result;
    }

    juce::WavAudioFormat wav;
    constexpr int kBitsPerSample = 24;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (stream.get(),
                              sessionSr,
                              (unsigned int) req.targetChannels,
                              kBitsPerSample,
                              {},
                              0));
    if (writer == nullptr)
    {
        result.errorMessage = "WAV writer construction failed (unsupported configuration)";
        return result;
    }
    // createWriterFor takes ownership of the stream on success.
    stream.release();

    // Stream decode → conform → (sinc-)resample → write in bounded chunks.
    // The old whole-file path allocated three full-length buffers (a 30-min
    // 96 kHz stereo stem needed ~1.4 GB before the writer even opened) and
    // froze the message thread on the allocation; this loop peaks at a few
    // hundred KB regardless of source length.
    constexpr int kGrain = 65536;
    juce::AudioBuffer<float> srcChunk  (srcChannels,      kGrain);
    juce::AudioBuffer<float> outChunk  (req.targetChannels, kGrain);
    bool wrote = true;

    if (! needsResample)
    {
        juce::AudioBuffer<float> confChunk (req.targetChannels, kGrain);
        juce::int64 pos = 0;
        while (pos < srcLength && wrote)
        {
            const int n = (int) juce::jmin ((juce::int64) kGrain, srcLength - pos);
            srcChunk.clear();
            if (! reader->read (&srcChunk, 0, n, pos, true, srcChannels > 1))
            {
                wrote = false;
                break;
            }
            conformChunk (srcChunk, confChunk, req.targetChannels, n);
            wrote = writer->writeFromAudioSampleBuffer (confChunk, 0, n);
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
                               + (int) juce::WindowedSincInterpolator::getBaseLatency() + 8;
        juce::AudioBuffer<float> confChunk (req.targetChannels, kGrain);
        juce::AudioBuffer<float> carry     (req.targetChannels, needIn + kGrain);
        std::array<juce::WindowedSincInterpolator, 2> interp;
        for (auto& i : interp) i.reset();

        int         carryLen = 0;
        juce::int64 srcPos   = 0;
        juce::int64 produced = 0;
        while (produced < outLength && wrote)
        {
            // Top up the carry from the source.
            while (carryLen < needIn && srcPos < srcLength)
            {
                const int n = (int) juce::jmin ((juce::int64) kGrain, srcLength - srcPos);
                srcChunk.clear();
                if (! reader->read (&srcChunk, 0, n, srcPos, true, srcChannels > 1))
                {
                    wrote = false;
                    break;
                }
                conformChunk (srcChunk, confChunk, req.targetChannels, n);
                const int room = juce::jmin (n, carry.getNumSamples() - carryLen);
                for (int c = 0; c < req.targetChannels; ++c)
                    carry.copyFrom (c, carryLen, confChunk, c, 0, room);
                carryLen += room;
                srcPos   += room;
            }
            if (! wrote) break;
            if (carryLen < needIn)   // EOF: silence-pad so the tail flushes
            {
                for (int c = 0; c < req.targetChannels; ++c)
                    juce::FloatVectorOperations::clear (
                        carry.getWritePointer (c) + carryLen, needIn - carryLen);
                carryLen = needIn;
            }

            const int nOut = (int) juce::jmin ((juce::int64) kGrain, outLength - produced);
            int consumed = 0;
            for (int c = 0; c < req.targetChannels; ++c)
                consumed = interp[(size_t) c].process (ratio,
                                                        carry.getReadPointer (c),
                                                        outChunk.getWritePointer (c),
                                                        nOut);
            wrote = writer->writeFromAudioSampleBuffer (outChunk, 0, nOut);
            produced += nOut;

            consumed = juce::jlimit (0, carryLen, consumed);
            for (int c = 0; c < req.targetChannels; ++c)
            {
                auto* p = carry.getWritePointer (c);
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
juce::int64 rescaleTicks (juce::int64 srcTicks, int srcPPQ) noexcept
{
    if (srcPPQ <= 0) return srcTicks;
    if (srcPPQ == kMidiTicksPerQuarter) return srcTicks;
    return (juce::int64) std::llround ((double) srcTicks
                                          * (double) kMidiTicksPerQuarter
                                          / (double) srcPPQ);
}
} // namespace

MidiImportResult importMidi (const MidiImportRequest& req)
{
    MidiImportResult result;

    if (! req.source.existsAsFile())
    {
        result.errorMessage = "MIDI file does not exist: " + req.source.getFullPathName();
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

    juce::FileInputStream in (req.source);
    if (! in.openedOk())
    {
        result.errorMessage = "Could not open MIDI file for reading";
        return result;
    }

    juce::MidiFile mf;
    if (! mf.readFrom (in))
    {
        result.errorMessage = "Failed to parse MIDI file";
        return result;
    }

    const auto timeFormat = mf.getTimeFormat();
    const bool isSmpte    = (timeFormat < 0);

    // For SMPTE-formatted files, convert to seconds first then rebuild
    // tick positions at session BPM. PPQ files use a direct rescale.
    if (isSmpte)
        mf.convertTimestampTicksToSeconds();

    auto timestampToProjectTicks = [&] (double rawTime) -> juce::int64
    {
        if (isSmpte)
        {
            // rawTime is in seconds.
            const double samples = rawTime * sessionSr;
            return duskstudio::samplesToTicks ((juce::int64) std::llround (samples),
                                            sessionSr,
                                            req.sessionBpm);
        }
        // rawTime is in source-PPQ ticks.
        return rescaleTicks ((juce::int64) std::llround (rawTime),
                              (int) timeFormat);
    };

    // Merge all tracks into one flat event list. Skip meta events; we
    // don't import tempo / time-sig maps in v1.
    struct ActiveNote
    {
        juce::int64 startTick;
        int velocity;
    };
    // (channel, note) -> stack of open note-ons. MIDI spec allows multiple
    // overlapping note-ons of the same pitch on the same channel.
    std::map<std::pair<int, int>, std::vector<ActiveNote>> open;

    std::vector<MidiNote> notes;
    std::vector<MidiCc>   ccs;
    juce::int64 maxTick = 0;

    const int numTracks = mf.getNumTracks();
    for (int t = 0; t < numTracks; ++t)
    {
        const auto* track = mf.getTrack (t);
        if (track == nullptr) continue;

        for (int i = 0; i < track->getNumEvents(); ++i)
        {
            const auto* ev = track->getEventPointer (i);
            const auto& msg = ev->message;

            const auto tick = timestampToProjectTicks (msg.getTimeStamp());
            if (tick > maxTick) maxTick = tick;

            if (msg.isNoteOn())
            {
                const int ch   = msg.getChannel();
                const int note = msg.getNoteNumber();
                const int vel  = msg.getVelocity();
                open[{ ch, note }].push_back ({ tick, vel });
            }
            else if (msg.isNoteOff() || (msg.isNoteOn() && msg.getVelocity() == 0))
            {
                const int ch   = msg.getChannel();
                const int note = msg.getNoteNumber();
                auto it = open.find ({ ch, note });
                if (it != open.end() && ! it->second.empty())
                {
                    const auto open_ = it->second.front();
                    it->second.erase (it->second.begin());
                    MidiNote n;
                    n.channel       = ch;
                    n.noteNumber    = note;
                    n.velocity      = juce::jmax (1, open_.velocity);
                    n.startTick     = open_.startTick;
                    n.lengthInTicks = juce::jmax<juce::int64> (1, tick - open_.startTick);
                    notes.push_back (n);
                }
            }
            else if (msg.isController())
            {
                MidiCc cc;
                cc.channel    = msg.getChannel();
                cc.controller = msg.getControllerNumber();
                cc.value      = msg.getControllerValue();
                cc.atTick     = tick;
                ccs.push_back (cc);
                if (tick > maxTick) maxTick = tick;
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
            n.velocity      = juce::jmax (1, a.velocity);
            n.startTick     = a.startTick;
            n.lengthInTicks = juce::jmax<juce::int64> (1, maxTick - a.startTick);
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
    result.region.lengthInTicks   = juce::jmax<juce::int64> (1, maxTick);
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
