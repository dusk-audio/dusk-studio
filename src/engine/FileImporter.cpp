#include "FileImporter.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <cmath>
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
juce::String makeImportedFilename (int trackIndex)
{
    const auto now = juce::Time::getCurrentTime();
    const auto stamp = juce::String::formatted ("%04d%02d%02d-%02d%02d%02d",
                                                  now.getYear(), now.getMonth() + 1, now.getDayOfMonth(),
                                                  now.getHours(), now.getMinutes(), now.getSeconds());
    return juce::String::formatted ("import_track%02d_%s.wav",
                                       trackIndex + 1,
                                       stamp.toRawUTF8());
}

// Channel-conform `src` into `dst` (dst is pre-sized to (targetChannels,
// src.getNumSamples())). Handles 1->2 duplicate, 2->1 sum-and-halve, and
// pass-through. No allocation; caller owns both buffers.
void conformChannels (const juce::AudioBuffer<float>& src,
                       juce::AudioBuffer<float>& dst,
                       int targetChannels)
{
    const int srcCh = src.getNumChannels();
    const int n     = src.getNumSamples();
    jassert (dst.getNumChannels() == targetChannels);
    jassert (dst.getNumSamples()  == n);

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

// Resample one channel via CatmullRomInterpolator. Returns the number of
// output samples written. Speed ratio = inSR / outSR (JUCE convention:
// values > 1 mean "consume input faster than output", i.e. down-sample
// the time axis -> lower output rate -> shorter output buffer).
int resampleChannel (const float* in,
                      int   inNumSamples,
                      double inSampleRate,
                      float* out,
                      int   outNumSamples,
                      double outSampleRate) noexcept
{
    juce::CatmullRomInterpolator interp;
    interp.reset();
    const double ratio = inSampleRate / outSampleRate;
    return interp.process (ratio, in, out, outNumSamples,
                             inNumSamples, /*wrapAt*/ 0);
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

    // Decode entire source. juce::AudioBuffer::setSize calls allocate;
    // we're on the message thread so that's fine.
    juce::AudioBuffer<float> srcBuf (srcChannels, (int) srcLength);
    srcBuf.clear();
    const bool readOk = reader->read (&srcBuf, 0, (int) srcLength, 0, true, srcChannels > 1);
    if (! readOk)
    {
        result.errorMessage = "Failed to decode audio file";
        return result;
    }

    // Channel conform.
    juce::AudioBuffer<float> conformed (req.targetChannels, (int) srcLength);
    conformed.clear();
    conformChannels (srcBuf, conformed, req.targetChannels);

    // Resample if necessary.
    const bool needsResample = std::abs (srcSampleRate - sessionSr) > 0.001;
    juce::int64 outLength = srcLength;
    juce::AudioBuffer<float> outBuf;

    if (! needsResample)
    {
        outBuf.makeCopyOf (conformed);
    }
    else
    {
        const double ratioOut = sessionSr / srcSampleRate;
        outLength = (juce::int64) std::llround ((double) srcLength * ratioOut);
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

        outBuf.setSize (req.targetChannels, (int) outLength, false, true, false);
        outBuf.clear();
        for (int c = 0; c < req.targetChannels; ++c)
        {
            // CatmullRomInterpolator fully fills all `outLength` output samples
            // (outLength was derived as srcLength/ratio, so the input is exactly
            // enough). Its return value is the number of INPUT samples CONSUMED
            // (~= srcLength), NOT the output count — for any upsample that's
            // always < outLength, so the old "pad the tail" path overwrote the
            // last ~8% of correctly-resampled audio with a held sample (imports
            // went silent at ~92% of their length). Discard the return; any
            // <=2-sample tail boundary stays as the pre-cleared zeros.
            resampleChannel (conformed.getReadPointer (c),
                             (int) srcLength, srcSampleRate,
                             outBuf.getWritePointer (c),
                             (int) outLength, sessionSr);
        }
    }

    // Write the normalised WAV. Same transient-lock retry as the source open:
    // a freshly-created file in the audio dir can be briefly held by the
    // Windows indexer / AV before the stream opens.
    const auto outFile = req.audioDir.getChildFile (makeImportedFilename (req.trackIndex));
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

    const bool wrote = writer->writeFromAudioSampleBuffer (outBuf, 0, (int) outLength);
    writer.reset();   // flush + close before we read the file back
    if (! wrote)
    {
        outFile.deleteFile();
        result.errorMessage = "Audio write failed";
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
