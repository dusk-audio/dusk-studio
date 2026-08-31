#pragma once

#include <juce_core/juce_core.h>
#include "../session/Session.h"
#include <string>

namespace duskstudio::fileimport
{
namespace detail
{
// Windows indexers and antivirus scanners can briefly deny access to a newly
// created file. Keep the retry policy in one testable place so every importer
// file operation remains bounded and waits only between failed attempts.
inline constexpr int kTransientFileAttempts = 5;

template <typename Operation, typename Wait>
auto retryTransientFileOperation (Operation&& operation, Wait&& wait)
{
    auto result = operation();
    for (int attempt = 1; ! result && attempt < kTransientFileAttempts; ++attempt)
    {
        wait();
        result = operation();
    }
    return result;
}
} // namespace detail

// Message-thread orchestrator for bringing user-supplied audio / MIDI
// files into a Dusk Studio session, always copying into the session's
// audio directory so the session stays self-contained.
//
// Audio: when the source already matches the session sample-rate AND the
// requested channel layout, it is copied in verbatim (no decode / resample
// / bit-depth change) so the import is bit-faithful. Only an actual rate or
// channel conform decodes, channel-conforms, resamples, and writes a
// 24-bit WAV.
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
    std::int64_t  timelineStart = 0;   // samples
};

struct AudioImportResult
{
    bool         ok = false;
    std::string  errorMessage;
    AudioRegion  region;
};

AudioImportResult importAudio (const AudioImportRequest&);

struct MidiImportRequest
{
    juce::File   source;
    double       sessionSampleRate;   // required - importer rejects <= 0
    float        sessionBpm = 120.0f;
    std::int64_t  timelineStart = 0;
};

struct MidiImportResult
{
    bool         ok = false;
    std::string  errorMessage;
    MidiRegion   region;
};

MidiImportResult importMidi (const MidiImportRequest&);

// Maximum samples per channel accepted by the audio importer. ~30 min at
// 96 kHz; rejects bigger files with a clear error so we don't OOM trying
// to load a multi-hour stem in one allocation.
constexpr std::int64_t kMaxImportSamplesPerChannel = 96000ll * 60ll * 30ll;
} // namespace duskstudio::fileimport
