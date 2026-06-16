#include "DpImporter.h"

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
    juce::int64 lengthSamples = 0;
    int         bitDepth = 0;
    int         numChannels = 0;
};

HeaderInfo readWavHeader (juce::AudioFormatManager& fm, const juce::File& f)
{
    HeaderInfo h;
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));
    if (reader == nullptr) return h;
    h.sampleRate    = reader->sampleRate;
    h.lengthSamples = (juce::int64) reader->lengthInSamples;
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
constexpr int    kFaderByte       = 16;
constexpr int    kPanByte         = 18;
constexpr int    kSentinelByte    = 19;
constexpr juce::uint8 kSentinel   = 0x2A;
constexpr juce::uint8 kFaderUnity = 0x69;   // power-on default == treat as 0 dB

// 3-band EQ bytes within a strip record (DP-24 manual p.101 CC mapping).
constexpr int kEqSwByte    = 4;   // 0/1
constexpr int kLowGainByte = 5;   // 0..24, 12 = 0 dB
constexpr int kLowFreqByte = 6;   // index 0..31 into kFreqTable
constexpr int kMidGainByte = 7;
constexpr int kMidFreqByte = 8;   // index 0..63 into kFreqTable
constexpr int kMidQByte    = 9;   // index 0..6 into kQTable
constexpr int kHighGainByte = 10;
constexpr int kHighFreqByte = 11; // index 0..31 -> kFreqTable[32 + idx]

// Shared 64-entry frequency table (Hz). Low band uses [0..31], Mid [0..63],
// High [32..63]. Transcribed from the manual's Mid Freq list.
constexpr float kFreqTable[64] = {
    32,40,50,60,70,80,90,100,125,150,175,200,225,250,300,350,400,450,500,
    600,700,800,850,900,950,1000,1100,1200,1300,1400,1500,1600,
    1700,1800,1900,2000,2200,2400,2600,2800,3000,3200,3400,3600,3800,4000,
    4500,5000,5500,6000,6500,7000,7500,8000,9000,10000,11000,12000,13000,
    14000,15000,16000,17000,18000 };
constexpr float kQTable[7] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f };

float eqGainDb (juce::uint8 v) noexcept { return (float) juce::jlimit (0, 24, (int) v) - 12.0f; }
float freqAt   (int idx) noexcept { return kFreqTable[(size_t) juce::jlimit (0, 63, idx)]; }
float qAt      (juce::uint8 v) noexcept { return kQTable[(size_t) juce::jlimit (0, 6, (int) v)]; }

float faderByteToDb (juce::uint8 v) noexcept
{
    if (v >= kFaderUnity)
        return (float) (v - kFaderUnity) / (127.0f - (float) kFaderUnity) * 6.0f;   // 0..+6 dB
    if (v == 0)
        return -100.0f;
    // Below unity: perceptual taper down toward silence. Approximate.
    const float db = 40.0f * std::log10 ((float) v / (float) kFaderUnity);
    return juce::jlimit (-100.0f, 0.0f, db);
}

float panByteToNorm (juce::uint8 v) noexcept
{
    return juce::jlimit (-1.0f, 1.0f, ((float) v - 64.0f) / 63.0f);   // 0x40 centre
}

// Read the device model from edltable.sys. song.sys is NOT reliable - it
// carries a constant "DP-24   " magic on both models - but edltable.sys
// records the real model ("DP-24"/"DP-32" at 0x08, "DP-24_EDL"/"DP-32_EDL"
// at 0x18). Returns the physical track count (18 / 20) or 0 if unknown.
int readDeviceModel (const juce::File& edl, juce::String& modelOut)
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
std::vector<MixerStrip> decodeMixerScene (const juce::File& songSys)
{
    std::vector<MixerStrip> strips;
    juce::MemoryBlock mb;
    if (! songSys.existsAsFile() || ! songSys.loadFileAsData (mb)) return strips;
    if ((int) mb.getSize() != kSongSysSize) return strips;

    const auto* d = (const juce::uint8*) mb.getData();
    if (std::memcmp (d, "DP-24", 5) != 0 && std::memcmp (d, "DP-32", 5) != 0)
        return strips;

    for (int c = 0; c < kNumStrips; ++c)
        if (d[kStripBase + c * kStripStride + kSentinelByte] != kSentinel)
            return strips;   // not the layout we know - decode nothing

    strips.resize ((size_t) kNumStrips);
    for (int c = 0; c < kNumStrips; ++c)
    {
        const auto* rec = d + kStripBase + c * kStripStride;
        MixerStrip s;
        s.valid   = true;
        s.faderDb = faderByteToDb (rec[kFaderByte]);
        s.pan     = panByteToNorm (rec[kPanByte]);
        s.mute    = false;   // no confidently-located per-strip mute flag yet

        s.eqOn       = rec[kEqSwByte] != 0;
        s.lowGainDb  = eqGainDb (rec[kLowGainByte]);
        s.lowFreqHz  = freqAt   (rec[kLowFreqByte]);
        s.midGainDb  = eqGainDb (rec[kMidGainByte]);
        s.midFreqHz  = freqAt   (rec[kMidFreqByte]);
        s.midQ       = qAt      (rec[kMidQByte]);
        s.highGainDb = eqGainDb (rec[kHighGainByte]);
        s.highFreqHz = freqAt   (32 + (int) rec[kHighFreqByte]);

        strips[(size_t) c] = s;
    }
    return strips;
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
        t.name     = juce::String::formatted ("DP %04d", idx);   // ZZ fragment id, not a track number
        t.fragment = frag;
        scan.tracks.push_back (std::move (t));
        if (frag.stereo) ++scan.stereoPairs;
    }

    if (scan.tracks.empty())
    {
        scan.warnings = warn.joinIntoString ("\n");
        if (scan.warnings.isEmpty()) scan.warnings = "No importable fragments.";
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
        warn.add (juce::String::formatted (
            "%d audio fragments found, but a %s has only %d tracks. The extra "
            "fragments are virtual takes / punch clips / cut regions. Without the "
            "(undecoded) placement table they can't be grouped, so each fragment "
            "is imported onto its own track for you to sort manually.",
            nFrag, scan.deviceModel.toRawUTF8(), scan.deviceTrackLimit));
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
    const auto strips = decodeMixerScene (folder.getChildFile ("song.sys"));
    if (! strips.empty())
    {
        scan.mixerDecoded = true;
        for (size_t i = 0; i < scan.tracks.size() && i < strips.size(); ++i)
            scan.tracks[i].mixer = strips[i];
        warn.add ("Mixer fader/pan/EQ decoded; mapped to tracks by order "
                  "(channel mapping isn't stored, so re-check assignments).");
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
    scan.timelineDecoded = false;
    if (scan.hasMixdown)
        warn.add ("Timeline can be aligned to the mixdown \"" + scan.mixdownFile.getFileName()
                  + "\" (experimental); otherwise tracks land at song start.");
    else
        warn.add ("No mixdown in folder: timeline not reconstructed; tracks placed at song start.");

    scan.warnings = warn.joinIntoString ("\n");
    scan.ok = true;
    return scan;
}
} // namespace duskstudio::dp
