#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include "../session/Session.h"

namespace duskstudio::fileimport
{
// Message-thread orchestrator for bringing user-supplied audio / MIDI
// files into a Focal session. Audio imports are decoded, channel-
// conformed to a target mono/stereo layout, resampled to the session
// sample-rate if needed, then written as a 24-bit WAV into the
// session's audio directory - matching RecordManager's on-disk
// convention so the session stays self-contained.
//
// Strictly single-threaded (message thread). The audio thread never
// touches FileImporter; the caller is responsible for committing the
// returned region onto Track::regions (audio) or Track::midiRegions
// (MIDI) on the message thread, then signalling whatever
// regions-changed mechanism the engine uses.

struct AudioImportRequest
{
    juce::File   source;
    juce::File   audioDir;          // Session::getAudioDirectory()
    int          trackIndex = 0;    // for the generated filename ("import_track{NN}_...")
    double       sessionSampleRate;   // required - importer rejects <= 0
    int          targetChannels = 1;  // 1 = mono, 2 = stereo
    juce::int64  timelineStart = 0;   // samples
};

struct AudioImportResult
{
    bool         ok = false;
    juce::String errorMessage;
    AudioRegion  region;
};

AudioImportResult importAudio (const AudioImportRequest&);

struct MidiImportRequest
{
    juce::File   source;
    double       sessionSampleRate;   // required - importer rejects <= 0
    float        sessionBpm = 120.0f;
    juce::int64  timelineStart = 0;
};

struct MidiImportResult
{
    bool         ok = false;
    juce::String errorMessage;
    MidiRegion   region;
};

MidiImportResult importMidi (const MidiImportRequest&);

// Maximum samples per channel accepted by the audio importer. ~30 min at
// 96 kHz; rejects bigger files with a clear error so we don't OOM trying
// to load a multi-hour stem in one allocation.
constexpr juce::int64 kMaxImportSamplesPerChannel = 96000ll * 60ll * 30ll;
} // namespace duskstudio::fileimport
