#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <vector>

// Importer for raw song folders written by a hardware SD-card multitrack
// recorder (the "DP" family). The device stores each song as a folder of
// ZZ####_N.wav audio fragments plus binary side-files (song.sys mixer scene,
// edltable.sys edit/overview table). This module is a pure, message-thread
// parser with no engine/UI coupling: it scans a folder, pairs stereo
// fragments, reads each fragment's true format from its RIFF header, and
// (best-effort) decodes the mixer scene. It never decodes audio and never
// touches the audio thread - the caller drives duskstudio::fileimport to
// bring the fragments in and commits the resulting regions.

namespace duskstudio::dp
{
struct Fragment
{
    int          zzIndex = -1;     // sequential recording-fragment id (NOT a track number)
    juce::File   mono1;            // _1 (mono / left)
    juce::File   mono2;            // _2 (right); empty unless stereo
    bool         stereo = false;
    juce::int64  lengthSamples = 0;
    double       sampleRate = 0.0;
    int          bitDepth = 0;
};

// Per-track mixer settings decoded from song.sys. Values are best-effort:
// the byte layout is solved but the dB/pan calibration is approximate, so
// callers should treat application as experimental and opt-in.
struct MixerStrip
{
    bool   valid = false;     // false => leave Track::strip at defaults
    float  faderDb = 0.0f;
    float  pan = 0.0f;        // -1..+1
    bool   mute = false;

    // 3-band channel EQ, decoded to engineering units (DP-24 manual p.101).
    bool   eqOn = false;
    float  lowGainDb = 0.0f,  lowFreqHz = 900.0f;
    float  midGainDb = 0.0f,  midFreqHz = 1000.0f, midQ = 0.5f;
    float  highGainDb = 0.0f, highFreqHz = 2800.0f;
};

// One importable audio fragment, destined for its own Dusk track. NOTE: a
// fragment is NOT the same as a device track - the recorder has only 18
// (DP-24) / 20 (DP-32) physical tracks, and a song can hold more fragments
// than that (virtual takes, punch-in clips, cut regions). The fragment->track
// grouping lives in the unsolved edltable.sys placement table, so until that
// is decoded each fragment is surfaced separately and named by its ZZ id.
struct ImportedTrack
{
    juce::String name;               // "DP 0006" (the ZZ fragment id)
    Fragment     fragment;
    juce::int64  timelineStart = 0;  // 0 until the placement table is solved
    MixerStrip   mixer;
};

struct SongScan
{
    bool         ok = false;            // at least one importable fragment found
    double       sampleRate = 0.0;
    int          bitDepth = 0;
    std::vector<ImportedTrack> tracks;  // one entry per audio fragment (NOT per device track)
    int          stereoPairs = 0;
    int          discardedTakes = 0;       // on-disk fragments excluded as not-in-arrangement (File-List)
    juce::String deviceModel;              // "DP-24" / "DP-32" from edltable.sys, or empty
    int          deviceTrackLimit = 0;     // physical track faders: 18 (DP-24), 20 (DP-32), 0 unknown
    bool         mixerDecoded = false;     // song.sys parsed; strips look structurally valid
    bool         timelineDecoded = false;  // edltable.sys placement solved (not yet)
    bool         hasMixdown = false;       // an in-folder master WAV is present (enables alignment)
    juce::File   mixdownFile;              // the detected master mixdown, if any
    juce::String warnings;                 // human-readable caveats for the dialog
};

// Scan a song folder. Never throws; bad/short WAV headers and garbage
// side-files are skipped and noted in `warnings`. Returns ok=false only when
// no importable fragment is present.
SongScan scanSongFolder (const juce::File& folder);

// Cheap heuristic for a folder picker: does this folder contain at least one
// ZZ####_N.wav fragment? Does not read headers.
bool looksLikeSongFolder (const juce::File& folder);
} // namespace duskstudio::dp
