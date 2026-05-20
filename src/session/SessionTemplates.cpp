#include "SessionTemplates.h"

namespace duskstudio
{
namespace
{
// Track template entry. Mode is stored as int because Track::mode is an
// std::atomic<int> - keeping the same shape avoids a cast at apply time.
struct TrackTemplate
{
    const char* name;
    juce::uint32 argb;            // packed ARGB so the table reads as a static blob
    int          mode;            // Track::Mode value (0=Mono, 1=Stereo, 2=Midi)
};

// Drums = warm red, bass = orange, gtrs = green, keys = teal, vox = violet.
// Same hue family across all templates so the user's eye learns the colour
// language immediately - not literal but evocative.
constexpr juce::uint32 kDrums    = 0xffd05050;
constexpr juce::uint32 kBass     = 0xffd09050;
constexpr juce::uint32 kGuitar   = 0xff70b070;
constexpr juce::uint32 kKeys     = 0xff60b0c0;
constexpr juce::uint32 kVocal    = 0xffb070c0;
constexpr juce::uint32 kPerc     = 0xffe0a050;
constexpr juce::uint32 kSynth    = 0xff7090d0;
constexpr juce::uint32 kPad      = 0xff80c0a0;

const TrackTemplate kBand[] =
{
    { "Kick",     kDrums,  0 },
    { "Snare",    kDrums,  0 },
    { "Drums OH", kDrums,  1 },
    { "Bass",     kBass,   0 },
    { "Gtr 1",    kGuitar, 1 },
    { "Gtr 2",    kGuitar, 1 },
    { "Keys",     kKeys,   1 },
    { "Lead Vox", kVocal,  0 },
    { "BG Vox",   kVocal,  0 },
};

const TrackTemplate kBeats[] =
{
    { "Kick",   kDrums, 0 },
    { "Snare",  kDrums, 0 },
    { "Hat",    kPerc,  0 },
    { "Perc",   kPerc,  1 },
    { "808",    kBass,  0 },
    { "Pad",    kPad,   1 },
    { "Lead",   kSynth, 2 },   // MIDI by default - typical workflow loads a synth
    { "Vox",    kVocal, 0 },
};

const TrackTemplate kSingerSongwriter[] =
{
    { "Vocal",     kVocal,  0 },
    { "Ac Gtr L",  kGuitar, 0 },
    { "Ac Gtr R",  kGuitar, 0 },
    { "Bass",      kBass,   0 },
    { "Synth",     kSynth,  2 },
    { "Drums",     kDrums,  1 },
};

void stampDefaults (Session& s, int firstFallbackTrack)
{
    // Blank-template logic for tracks past the template's coverage. Mirrors
    // Session()'s ctor: numeric name + hue-rotated colour. We stamp these
    // explicitly because the user may apply a template on top of an
    // already-customised session and expect the un-named tracks to reset.
    for (int i = firstFallbackTrack; i < Session::kNumTracks; ++i)
    {
        auto& t = s.track (i);
        t.name = juce::String (i + 1);
        t.colour = juce::Colour::fromHSV (
            i / (float) Session::kNumTracks, 0.45f, 0.75f, 1.0f);
        t.mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    }
}

template <std::size_t N>
void applyTrackTemplate (Session& s, const TrackTemplate (&entries)[N])
{
    const int n = juce::jmin ((int) N, Session::kNumTracks);
    for (int i = 0; i < n; ++i)
    {
        auto& t = s.track (i);
        t.name = entries[(size_t) i].name;
        t.colour = juce::Colour (entries[(size_t) i].argb);
        t.mode.store (entries[(size_t) i].mode, std::memory_order_relaxed);
    }
    stampDefaults (s, n);
}
} // namespace

const char* nameForTemplate (SessionTemplate t) noexcept
{
    switch (t)
    {
        case SessionTemplate::Blank:            return "Blank";
        case SessionTemplate::Band:             return "Band";
        case SessionTemplate::Beats:            return "Beats";
        case SessionTemplate::SingerSongwriter: return "Singer-Songwriter";
        case SessionTemplate::kCount:           break;
    }
    return "?";
}

void applyTemplate (Session& s, SessionTemplate t)
{
    switch (t)
    {
        case SessionTemplate::Blank:            stampDefaults (s, 0); return;
        case SessionTemplate::Band:             applyTrackTemplate (s, kBand); return;
        case SessionTemplate::Beats:            applyTrackTemplate (s, kBeats); return;
        case SessionTemplate::SingerSongwriter: applyTrackTemplate (s, kSingerSongwriter); return;
        case SessionTemplate::kCount:           return;
    }
}
} // namespace duskstudio
