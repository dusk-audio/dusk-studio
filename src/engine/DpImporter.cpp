#include "DpImporter.h"

#include "../foundation/Text.h"
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace duskstudio::dp
{
namespace
{
// A fragment file is "ZZ<digits>_<1|2>.wav". The device also writes a
// full-song master mixdown (<Song>.WAV / <Song>_z.WAV) and macOS adds
// "._"-prefixed AppleDouble sidecars; neither matches this pattern.
bool parseFragmentName (const juce::String& name, int& zzIndex, int& channel)
{
    if (name.startsWithIgnoreCase ("._")) return false;
    if (! name.startsWithIgnoreCase ("ZZ")) return false;
    if (! name.endsWithIgnoreCase (".wav")) return false;

    const auto stem = name.dropLastCharacters (4);            // strip ".wav"
    const int us = stem.lastIndexOfChar ('_');
    if (us <= 2) return false;                                // need "ZZ" + digits before '_'

    const auto digits  = stem.substring (2, us);
    const auto chanStr = stem.substring (us + 1);
    if (digits.isEmpty() || ! digits.containsOnly ("0123456789")) return false;
    if (chanStr != "1" && chanStr != "2") return false;

    zzIndex = digits.getIntValue();
    channel = chanStr.getIntValue();
    return true;
}

struct HeaderInfo
{
    bool        ok = false;
    double      sampleRate = 0.0;
    std::int64_t lengthSamples = 0;
    int         bitDepth = 0;
    int         numChannels = 0;
};

HeaderInfo readWavHeader (juce::AudioFormatManager& fm, const juce::File& f)
{
    HeaderInfo h;
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));
    if (reader == nullptr) return h;
    h.sampleRate    = reader->sampleRate;
    h.lengthSamples = (std::int64_t) reader->lengthInSamples;
    h.bitDepth      = (int) reader->bitsPerSample;
    h.numChannels   = (int) reader->numChannels;
    h.ok = (h.sampleRate > 0.0 && h.lengthSamples > 0 && h.numChannels > 0);
    return h;
}

// song.sys mixer scene. The first scene block is a 24-entry array of 20-byte
// strip records starting at 0x14; every record ends in a 0x2A sentinel byte.
// Within a record: byte 16 = fader (0..127, power-on default 0x69), byte 18 =
// pan (0..127, centre 0x40). The fader->dB curve is approximate (uncalibrated
// against hardware), so application is opt-in/experimental.
constexpr int    kSongSysSize     = 2996;
constexpr int    kStripBase       = 0x14;
constexpr int    kStripStride     = 20;
constexpr int    kNumStrips       = 24;
constexpr int    kSentinelByte    = 19;
constexpr std::uint8_t kSentinel   = 0x2A;
constexpr int kMuteArrayBase = 0x268;       // 1 byte per channel, 1 = muted (SONG_0005 ch6)

// Channel faders live in their OWN array, NOT in the 0x14 strip records: a byte
// per mixer fader at 0xc4, stride 20 (mirrored in scene block 2). The DP-24 has
// 18 faders - 12 mono tracks + 6 stereo-pair faders for tracks 13..24 - so a
// paired track reads its half's shared fader. Verified against SONG_0002 (24
// faders set to known dB; see kFaderCal). The old strip+16 lookup read unity
// garbage and is gone.
constexpr int kFaderArrayBase   = 0xc4;
constexpr int kFaderArrayStride = 20;
constexpr int kNumFaders        = 18;
constexpr int kPanInRecord      = 2;   // pan byte within a fader record (64 = centre)

// Map a 0-based track index (0..23) to its fader-array entry (0..17).
int faderEntryForTrack (int track) noexcept
{
    if (track < 12) return track;          // tracks 1..12 are mono, one fader each
    return 12 + (track - 12) / 2;          // tracks 13..24 are 6 stereo pairs
}

// 3-band EQ bytes within a strip record. Band order verified against the
// SONG_0005 calibration song (ch1 LOW +6@70Hz, ch2 HIGH -6@7.5k, ch3 MID -4@1k
// Q2): the device stores HIGH, MID, LOW in that order, NOT low-first.
constexpr int kEqSwByte     = 4;   // 0/1
constexpr int kHighGainByte = 5;   // 0..24, 12 = 0 dB
constexpr int kHighFreqByte = 6;   // index 0..31 -> kFreqTable[32 + idx] (1.7k..18k)
constexpr int kMidGainByte  = 7;
constexpr int kMidFreqByte  = 8;   // index 0..63 into kFreqTable
constexpr int kMidQByte     = 9;   // index 0..6 into kQTable
constexpr int kLowGainByte  = 10;
constexpr int kLowFreqByte  = 11;  // index 0..31 into kFreqTable (32..1.6k)

// Shared 64-entry frequency table (Hz). Low band uses [0..31], Mid [0..63],
// High [32..63]. Transcribed from the manual's Mid Freq list.
constexpr float kFreqTable[64] = {
    32,40,50,60,70,80,90,100,125,150,175,200,225,250,300,350,400,450,500,
    600,700,800,850,900,950,1000,1100,1200,1300,1400,1500,1600,
    1700,1800,1900,2000,2200,2400,2600,2800,3000,3200,3400,3600,3800,4000,
    4500,5000,5500,6000,6500,7000,7500,8000,9000,10000,11000,12000,13000,
    14000,15000,16000,17000,18000 };
constexpr float kQTable[7] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f };

float eqGainDb (std::uint8_t v) noexcept { return (float) juce::jlimit (0, 24, (int) v) - 12.0f; }
float freqAt   (int idx) noexcept { return kFreqTable[(size_t) juce::jlimit (0, 63, idx)]; }
float qAt      (std::uint8_t v) noexcept { return kQTable[(size_t) juce::jlimit (0, 6, (int) v)]; }

// Fader byte -> dB, calibrated from SONG_0002 (24 faders set to known values,
// read off the device display). Monotonic anchor table; linear-interpolate
// between points. Unity (0 dB) = byte 106, full off = 0 -> -100 (session's
// -inf sentinel), max = 127 -> +6 dB.
struct FaderCal { int byte; float db; };
constexpr FaderCal kFaderCal[] = {
    {   0, -100.0f }, {   3, -72.0f }, {  16, -45.3f }, {  26, -34.0f },
    {  40, -20.0f }, {  42, -19.1f }, {  55, -13.2f }, {  65,  -9.3f },
    {  74,  -7.3f }, {  85,  -4.8f }, {  94,  -2.5f }, { 100,  -1.0f },
    { 106,   0.0f }, { 107,   0.3f }, { 112,   1.8f }, { 118,   3.6f },
    { 120,   4.2f }, { 127,   6.0f },
};

float faderByteToDb (std::uint8_t v) noexcept
{
    constexpr int n = (int) (sizeof (kFaderCal) / sizeof (kFaderCal[0]));
    if (v <= kFaderCal[0].byte)     return kFaderCal[0].db;
    if (v >= kFaderCal[n - 1].byte) return kFaderCal[n - 1].db;
    for (int i = 1; i < n; ++i)
        if (v <= kFaderCal[i].byte)
        {
            const float span = (float) (kFaderCal[i].byte - kFaderCal[i - 1].byte);
            const float t    = (float) (v - kFaderCal[i - 1].byte) / span;
            return kFaderCal[i - 1].db + t * (kFaderCal[i].db - kFaderCal[i - 1].db);
        }
    return kFaderCal[n - 1].db;
}

float panByteToNorm (std::uint8_t v) noexcept
{
    return juce::jlimit (-1.0f, 1.0f, ((float) v - 64.0f) / 63.0f);   // 0x40 centre
}

// Read the device model from edltable.sys. song.sys is NOT reliable - it
// carries a constant "DP-24   " magic on both models - but edltable.sys
// records the real model ("DP-24"/"DP-32" at 0x08, "DP-24_EDL"/"DP-32_EDL"
// at 0x18). Returns the physical track count (18 / 20) or 0 if unknown.
int readDeviceModel (const juce::File& edl, std::string& modelOut)
{
    juce::FileInputStream in (edl);
    if (! in.openedOk()) return 0;
    char buf[16] = {};
    in.setPosition (0x08);
    if (in.read (buf, 8) != 8) return 0;
    const juce::String tag (buf, 8);
    if (tag.startsWith ("DP-32")) { modelOut = "DP-32"; return 20; }
    if (tag.startsWith ("DP-24")) { modelOut = "DP-24"; return 18; }
    return 0;
}

// Decode the scene block. Returns 24 strips with valid=true when the block is
// structurally sound (size, sentinels). Returns empty on any mismatch.
// deviceTrackLimit (18 = DP-24, 20 = DP-32, 0 = unknown) gates the fader/pan
// array layout, which is verified on DP-24 only.
std::vector<MixerStrip> decodeMixerScene (const juce::File& songSys, int deviceTrackLimit)
{
    std::vector<MixerStrip> strips;
    juce::MemoryBlock mb;
    if (! songSys.existsAsFile() || ! songSys.loadFileAsData (mb)) return strips;
    if ((int) mb.getSize() != kSongSysSize) return strips;

    const auto* d = (const std::uint8_t*) mb.getData();
    if (std::memcmp (d, "DP-24", 5) != 0 && std::memcmp (d, "DP-32", 5) != 0)
        return strips;

    for (int c = 0; c < kNumStrips; ++c)
        if (d[kStripBase + c * kStripStride + kSentinelByte] != kSentinel)
            return strips;   // not the layout we know - decode nothing

    // The fader/pan array (0xc4, 18 entries with stereo pairing for tracks
    // 13..24) is verified on DP-24 ONLY. Apply it solely for a confirmed DP-24
    // (deviceTrackLimit == 18). DP-32 (20) has an unmapped layout, and an
    // unknown/unreadable model (0) could be either - in both cases leave the
    // strip defaults (0 dB / centre) rather than read the wrong entries.
    const bool faderPanKnown = (deviceTrackLimit == 18);

    strips.resize ((size_t) kNumStrips);
    for (int c = 0; c < kNumStrips; ++c)
    {
        const auto* rec = d + kStripBase + c * kStripStride;
        MixerStrip s;
        s.valid   = true;
        if (faderPanKnown)
        {
            const int faderRec = kFaderArrayBase + faderEntryForTrack (c) * kFaderArrayStride;
            s.faderDb = faderByteToDb (d[faderRec]);
            s.pan     = panByteToNorm (d[faderRec + kPanInRecord]);
        }
        s.mute    = d[kMuteArrayBase + c] != 0;   // per-channel mute byte (1 = muted)

        s.highGainDb = eqGainDb (rec[kHighGainByte]);
        s.highFreqHz = freqAt   (32 + (int) rec[kHighFreqByte]);
        s.midGainDb  = eqGainDb (rec[kMidGainByte]);
        s.midFreqHz  = freqAt   (rec[kMidFreqByte]);
        s.midQ       = qAt      (rec[kMidQByte]);
        s.lowGainDb  = eqGainDb (rec[kLowGainByte]);
        s.lowFreqHz  = freqAt   (rec[kLowFreqByte]);
        // byte 4 reads 0 even when EQ is set (SONG_0005), so it's not the on/off
        // flag; enable when any band is non-flat.
        s.eqOn = rec[kEqSwByte] != 0
                 || s.lowGainDb != 0.0f || s.midGainDb != 0.0f || s.highGainDb != 0.0f;

        strips[(size_t) c] = s;
    }
    return strips;
}

// Clip timeline positions live in song.sys as u32 sample offsets (@ song SR) in
// the metadata region between the two scene blocks. The device stores, per
// placed clip, start and end (= start + length - 480; 480 = one 10ms overview
// column). We don't need the exact record layout: collect every plausible u32
// position in the region, then for each fragment (known length) find a start S
// whose matching end S+length is also present. This pins each clip's start
// without the still-unknown clip->track record fields. Verified on SONG_0002
// (w/ mixdown) and SONG_0003 (no mixdown, same arrangement): identical, exact.
//
// Returns one start (in song-SR samples) per fragment in `lengths`, 0 when no
// confident match (e.g. a take that sits at song start, or undecodable).
constexpr int kPosRegionBegin = 0x340;   // after scene block 1, before the EQ-preset block
constexpr int kPosRegionEnd   = 0x6a0;
constexpr std::int64_t kEndTolerance = 600;   // ~12 ms slack on end = start + length

constexpr int kSongLengthOffset = 0x364;   // u32 song length in samples @ song SR

std::vector<std::int64_t> decodeClipStarts (const juce::File& songSys,
                                           const std::vector<std::int64_t>& lengths)
{
    std::vector<std::int64_t> starts (lengths.size(), 0);
    juce::MemoryBlock mb;
    if (! songSys.existsAsFile() || ! songSys.loadFileAsData (mb)) return starts;
    if ((int) mb.getSize() != kSongSysSize) return starts;
    const auto* d = (const std::uint8_t*) mb.getData();

    const std::int64_t songLen = (std::int64_t) juce::ByteOrder::littleEndianInt (d + kSongLengthOffset);

    // Candidate positions: every 4-byte-aligned u32 in [1, songLength] in the region.
    std::vector<std::int64_t> cand;
    const std::int64_t hi = (songLen > 0 ? songLen : (std::int64_t) 96000 * 60 * 60) + 2000;
    for (int o = kPosRegionBegin; o + 4 <= kPosRegionEnd; o += 4)   // positions are 4-byte aligned
    {
        const std::int64_t v = (std::int64_t) juce::ByteOrder::littleEndianInt (d + o);
        if (v >= 1 && v <= hi) cand.push_back (v);
    }
    std::set<std::int64_t> candSet (cand.begin(), cand.end());

    // A near-full-length take spans the song and sits at 0; never length-match it
    // (its hypothetical end coincides with song-end-ish values and false-matches).
    const std::int64_t fullLenThresh = songLen > 0 ? (songLen * 85) / 100 : 0;

    // Track starts already claimed so two clips of the same length don't both
    // grab the same position (deterministic misplacement). Each start is used once.
    std::set<std::int64_t> usedStarts;
    for (size_t i = 0; i < lengths.size(); ++i)
    {
        const std::int64_t L = lengths[i];
        if (L <= 0) continue;
        if (fullLenThresh > 0 && L >= fullLenThresh) continue;   // full-length take -> stays 0
        std::int64_t best = -1;
        for (const auto S : candSet)
        {
            if (S + L > hi || usedStarts.count (S)) continue;
            // accept if some candidate end is within tolerance of S + L.
            bool endFound = false;
            for (std::int64_t e = S + L - kEndTolerance; e <= S + L && ! endFound; ++e)
                if (candSet.count (e)) endFound = true;
            if (endFound && (best < 0 || S < best)) best = S;
        }
        if (best > 0) { starts[i] = best; usedStarts.insert (best); }
    }
    return starts;
}

// Locate marks (intro/verse/chorus/punch/end) live in song.sys as stride-8
// records {u32 position(samples), u32 index<<24} just past the clip-position
// fields. Position must be in (0, songLen]; the second word's low 24 bits are
// zero and the high byte is the 1-based mark number. Verified on
// /Volumes/DP-24/MUSIC SONG_0002/3 (2 marks) and 003/Whiskey (7 marks @ 21/55/
// 89/140/174/208/242 s).
constexpr int kMarkerRegionBegin = 0x388;
constexpr int kMarkerRegionEnd   = 0x460;
constexpr int kTempoOffset = 0x6d8;   // u8 BPM; verified 107 (SONG_0004) / 93 (SONG_0005)

// Song tempo from song.sys (u8 BPM). Returns 0 if undecodable / out of range.
int readTempoBpm (const juce::File& songSys)
{
    juce::MemoryBlock mb;
    if (! songSys.existsAsFile() || ! songSys.loadFileAsData (mb)) return 0;
    if ((int) mb.getSize() != kSongSysSize) return 0;
    const int bpm = ((const std::uint8_t*) mb.getData())[kTempoOffset];
    return (bpm >= 20 && bpm <= 250) ? bpm : 0;
}

constexpr int kTimeSigOffset = 0x6d9;   // one byte, immediately after tempo

// Song time signature from song.sys. The byte packs both fields:
//   byte = 12 * log2(denominator) + (numerator - 1)
// numerator is 1..12 and denominator is one of 1/2/4/8 (the DP units offer no
// other values), so the valid byte range is 0..47. Verified against four songs
// authored on the device: 1/1=0, 3/2=14, 12/8=47, 8/4=31.
// Sets num + den and returns true on a valid decode; leaves them 0 otherwise.
bool readTimeSignature (const juce::File& songSys, int& num, int& den)
{
    num = 0;
    den = 0;
    juce::MemoryBlock mb;
    if (! songSys.existsAsFile() || ! songSys.loadFileAsData (mb)) return false;
    if ((int) mb.getSize() != kSongSysSize) return false;
    const auto* d = (const std::uint8_t*) mb.getData();
    // Reject non-DP / corrupt files (same magic gate as decodeMixerScene);
    // otherwise byte 0 decodes to a bogus 1/1 for any 2996-byte blob.
    if (std::memcmp (d, "DP-24", 5) != 0 && std::memcmp (d, "DP-32", 5) != 0)
        return false;
    // Same structural sentinel check decodeMixerScene uses: every strip record
    // must end in 0x2A. A blob carrying only the DP magic but not the real
    // song.sys layout fails here, so it can't yield a phantom time signature.
    for (int c = 0; c < kNumStrips; ++c)
        if (d[kStripBase + c * kStripStride + kSentinelByte] != kSentinel)
            return false;
    const int packed = d[kTimeSigOffset];
    const int denExp = packed / 12;       // 0..3 -> 1/2/4/8
    if (denExp > 3) return false;         // out of the device's range -> treat as garbage
    num = (packed % 12) + 1;              // 1..12
    den = 1 << denExp;
    return true;
}

std::vector<DpMarker> decodeMarkers (const juce::File& songSys)
{
    std::vector<DpMarker> out;
    juce::MemoryBlock mb;
    if (! songSys.existsAsFile() || ! songSys.loadFileAsData (mb)) return out;
    if ((int) mb.getSize() != kSongSysSize) return out;
    const auto* d = (const std::uint8_t*) mb.getData();
    const std::int64_t songLen = (std::int64_t) juce::ByteOrder::littleEndianInt (d + kSongLengthOffset);
    if (songLen <= 0) return out;

    for (int o = kMarkerRegionBegin; o + 8 <= kMarkerRegionEnd; o += 8)
    {
        const std::int64_t pos  = (std::int64_t) juce::ByteOrder::littleEndianInt (d + o);
        const std::uint32_t iw  = (std::uint32_t) juce::ByteOrder::littleEndianInt (d + o + 4);
        const int idx = (int) (iw >> 24);
        if (pos > 0 && pos <= songLen && (iw & 0x00FFFFFFu) == 0 && idx >= 1 && idx <= 64)
            out.push_back ({ pos, idx });
    }
    return out;
}
} // namespace

namespace
{
// The edltable.sys File-List enumerates the fragments the song actually uses.
// A fragment on disk whose index is NOT listed is a discarded take (alternate
// take / undone punch) and should be skipped. We just scan the file for the
// ASCII "ZZ####_N.wav" entries (they live only in the tail File-List; the rest
// of the file is binary), and return the set of referenced ZZ indices. Empty
// means undecodable -> caller imports everything.
std::set<int> readActiveFragmentIndices (const juce::File& edltable)
{
    std::set<int> active;
    juce::MemoryBlock mb;
    if (! edltable.existsAsFile() || ! edltable.loadFileAsData (mb)) return active;
    const auto* d = (const char*) mb.getData();
    const int n = (int) mb.getSize();
    // Only scan genuine DP edit tables (magic "TEAC" at 0x00); otherwise a
    // non-DP/corrupt file could coincidentally match ZZ patterns and wrongly
    // filter fragments. Empty result => caller imports everything.
    if (n < 0x20 || std::memcmp (d, "TEAC", 4) != 0) return active;
    auto dig = [] (char c) { return c >= '0' && c <= '9'; };
    for (int i = 0; i + 12 <= n; ++i)
    {
        if (d[i] != 'Z' || d[i + 1] != 'Z') continue;
        if (! (dig (d[i+2]) && dig (d[i+3]) && dig (d[i+4]) && dig (d[i+5]))) continue;
        if (d[i+6] != '_' || ! dig (d[i+7]) || d[i+8] != '.') continue;
        const char w = d[i+9], a = d[i+10], v = d[i+11];
        if ((w == 'w' || w == 'W') && (a == 'a' || a == 'A') && (v == 'v' || v == 'V'))
            active.insert ((d[i+2]-'0')*1000 + (d[i+3]-'0')*100 + (d[i+4]-'0')*10 + (d[i+5]-'0'));
    }
    return active;
}
} // namespace

bool looksLikeSongFolder (const juce::File& folder)
{
    if (! folder.isDirectory()) return false;
    for (const auto& f : folder.findChildFiles (juce::File::findFiles, false, "*.wav"))
    {
        int idx = 0, ch = 0;
        if (parseFragmentName (f.getFileName(), idx, ch)) return true;
    }
    return false;
}

SongScan scanSongFolder (const juce::File& folder)
{
    SongScan scan;
    if (! folder.isDirectory())
    {
        scan.warnings = "Folder does not exist.";
        return scan;
    }

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    // Group fragment files by ZZ index.
    struct Pair { juce::File f1, f2; };
    std::map<int, Pair> byIndex;
    for (const auto& f : folder.findChildFiles (juce::File::findFiles, false))
    {
        int idx = 0, ch = 0;
        if (! parseFragmentName (f.getFileName(), idx, ch)) continue;
        if (ch == 1) byIndex[idx].f1 = f;
        else         byIndex[idx].f2 = f;
    }

    if (byIndex.empty())
    {
        scan.warnings = "No DP audio fragments (ZZ####_N.wav) found in this folder.";
        return scan;
    }

    juce::StringArray warn;

    // Drop discarded takes: keep only fragments the edltable File-List references.
    const auto active = readActiveFragmentIndices (folder.getChildFile ("edltable.sys"));
    if (! active.empty())
    {
        for (auto it = byIndex.begin(); it != byIndex.end();)
        {
            if (active.find (it->first) == active.end())
            {
                it = byIndex.erase (it);
                ++scan.discardedTakes;
            }
            else ++it;
        }
        if (scan.discardedTakes > 0)
            warn.add (juce::String::formatted (
                "%d discarded take(s) on disk were skipped (not used in the session).",
                scan.discardedTakes));
    }

    if (byIndex.empty())
    {
        scan.warnings = "All fragments were discarded takes; nothing to import.";
        return scan;
    }

    // std::map iterates in ascending key order -> tracks ordered by ZZ index.
    for (auto& [idx, pair] : byIndex)
    {
        const bool have1 = pair.f1 != juce::File();
        const bool have2 = pair.f2 != juce::File();

        Fragment frag;
        frag.zzIndex = idx;

        if (have1 && have2)
        {
            const auto h1 = readWavHeader (fm, pair.f1);
            const auto h2 = readWavHeader (fm, pair.f2);
            if (! h1.ok || ! h2.ok)
            {
                warn.add (juce::String::formatted ("ZZ%04d: unreadable WAV header; skipped.", idx));
                continue;
            }
            // A real stereo pair is two equal-format mono files. Reject halves
            // that disagree on sample rate / bit depth, or aren't single-channel.
            if (std::abs (h1.sampleRate - h2.sampleRate) > 0.5
                || h1.bitDepth != h2.bitDepth
                || h1.numChannels != 1 || h2.numChannels != 1)
            {
                warn.add (juce::String::formatted (
                    "ZZ%04d: stereo halves have mismatched format; skipped.", idx));
                continue;
            }
            if (h1.lengthSamples != h2.lengthSamples)
                warn.add (juce::String::formatted ("ZZ%04d: stereo halves differ in length; using left length.", idx));
            frag.stereo        = true;
            frag.mono1         = pair.f1;
            frag.mono2         = pair.f2;
            frag.lengthSamples = h1.lengthSamples;
            frag.sampleRate    = h1.sampleRate;
            frag.bitDepth      = h1.bitDepth;
        }
        else
        {
            const auto src = have1 ? pair.f1 : pair.f2;
            if (have2 && ! have1)
                warn.add (juce::String::formatted ("ZZ%04d: right channel without left; imported as mono.", idx));
            const auto h = readWavHeader (fm, src);
            if (! h.ok)
            {
                warn.add (juce::String::formatted ("ZZ%04d: unreadable WAV header; skipped.", idx));
                continue;
            }
            frag.stereo        = false;
            frag.mono1         = src;
            frag.lengthSamples = h.lengthSamples;
            frag.sampleRate    = h.sampleRate;
            frag.bitDepth      = h.bitDepth;
        }

        ImportedTrack t;
        t.name     = dusk::text::format ("DP %04d", idx);   // ZZ fragment id, not a track number
        t.fragment = frag;
        scan.tracks.push_back (std::move (t));
        if (frag.stereo) ++scan.stereoPairs;
    }

    if (scan.tracks.empty())
    {
        scan.warnings = warn.joinIntoString ("\n").toStdString();
        if (scan.warnings.empty()) scan.warnings = "No importable fragments.";
        return scan;
    }

    // Song format = the first track's; flag any divergence (importAudio
    // resamples each fragment to the session rate individually).
    scan.sampleRate = scan.tracks.front().fragment.sampleRate;
    scan.bitDepth   = scan.tracks.front().fragment.bitDepth;

    // Device model + physical track count, so we can be honest that not every
    // fragment is a distinct track.
    scan.deviceTrackLimit = readDeviceModel (folder.getChildFile ("edltable.sys"),
                                             scan.deviceModel);
    const int nFrag = (int) scan.tracks.size();
    if (scan.deviceTrackLimit > 0 && nFrag > scan.deviceTrackLimit)
        warn.add (dusk::text::format (
            "%d audio fragments found, but a %s has only %d tracks. The extra "
            "fragments are virtual takes / punch clips / cut regions. Without the "
            "(undecoded) placement table they can't be grouped, so each fragment "
            "is imported onto its own track for you to sort manually.",
            nFrag, scan.deviceModel.c_str(), scan.deviceTrackLimit));
    else
        warn.add ("Each audio fragment is imported onto its own track; fragments "
                  "are not grouped into their original device tracks.");
    for (const auto& t : scan.tracks)
        if (std::abs (t.fragment.sampleRate - scan.sampleRate) > 0.5
            || t.fragment.bitDepth != scan.bitDepth)
        {
            warn.add ("Fragments have mixed sample-rate/bit-depth; each is conformed to the session.");
            break;
        }

    // Mixer scene (fader/pan/EQ calibrated from the DP-24 manual). Strips are
    // indexed by device input channel; the fragment->channel mapping is part of
    // the unsolved placement table, so we apply strips positionally (track order).
    const auto strips = decodeMixerScene (folder.getChildFile ("song.sys"), scan.deviceTrackLimit);
    if (! strips.empty())
    {
        scan.mixerDecoded = true;
        for (size_t i = 0; i < scan.tracks.size() && i < strips.size(); ++i)
            scan.tracks[i].mixer = strips[i];
        if (scan.deviceTrackLimit == 18)
            warn.add ("Mixer fader/pan/EQ decoded; mapped to tracks by order "
                      "(channel mapping isn't stored, so re-check assignments).");
        else if (scan.deviceTrackLimit == 20)
            warn.add ("Mixer EQ decoded; fader/pan recall is skipped on DP-32 "
                      "(its fader layout isn't mapped yet). Re-check levels.");
        else
            warn.add ("Mixer EQ decoded; fader/pan recall skipped (device model "
                      "not identified, so the fader layout can't be trusted). "
                      "Re-check levels.");
    }

    // Timeline placement: not in the .sys files. If an in-folder master mixdown
    // exists we can recover positions by aligning fragments to it; otherwise
    // everything lands at song start (correct for full-length takes).
    for (const auto& f : folder.findChildFiles (juce::File::findFiles, false))
    {
        const auto name = f.getFileName();
        if (name.startsWithIgnoreCase ("._")) continue;
        if (! name.endsWithIgnoreCase (".wav")) continue;
        int idx = 0, ch = 0;
        if (parseFragmentName (name, idx, ch)) continue;            // a fragment, not the master
        const auto stem = name.dropLastCharacters (4);
        if (stem.endsWithIgnoreCase ("_z") || stem.endsWithIgnoreCase ("_zz")) continue;  // undo backup
        scan.hasMixdown = true;
        scan.mixdownFile = f;
        if (stem.equalsIgnoreCase (folder.getFileName())) break;    // prefer <SongName>.WAV
    }
    // Clip start positions from song.sys (device's own data; works with no mixdown).
    std::vector<std::int64_t> lengths;
    lengths.reserve (scan.tracks.size());
    for (const auto& t : scan.tracks) lengths.push_back (t.fragment.lengthSamples);
    const auto starts = decodeClipStarts (folder.getChildFile ("song.sys"), lengths);
    int placedFromSys = 0;
    for (size_t i = 0; i < scan.tracks.size() && i < starts.size(); ++i)
    {
        scan.tracks[i].timelineStart = starts[i];   // song-SR samples
        if (starts[i] > 0) ++placedFromSys;
    }
    scan.markers = decodeMarkers (folder.getChildFile ("song.sys"));
    if (! scan.markers.empty())
        warn.add (juce::String::formatted ("%d song marker(s) will be imported.",
                                           (int) scan.markers.size()));

    scan.tempoBpm = readTempoBpm (folder.getChildFile ("song.sys"));
    if (scan.tempoBpm > 0)
        warn.add (juce::String::formatted ("Song tempo %d BPM will be applied.", scan.tempoBpm));

    if (readTimeSignature (folder.getChildFile ("song.sys"), scan.timeSigNum, scan.timeSigDen))
        warn.add (juce::String::formatted ("Time signature %d/%d will be applied.",
                                            scan.timeSigNum, scan.timeSigDen));

    scan.timelineDecoded = (placedFromSys > 0);
    if (placedFromSys > 0)
        warn.add (juce::String::formatted (
            "Timeline positions recovered from song.sys for %d clip(s); the rest start at 0.",
            placedFromSys));
    else if (scan.hasMixdown)
        warn.add ("No clip offsets in song.sys (all at song start); a mixdown is present for "
                  "optional onset-alignment.");
    else
        warn.add ("All clips start at song start (no stored offsets).");

    scan.warnings = warn.joinIntoString ("\n").toStdString();
    scan.ok = true;
    return scan;
}
} // namespace duskstudio::dp
