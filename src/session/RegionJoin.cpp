#include "RegionJoin.h"
#include "RegionEnvelope.h"
#include "../engine/audiofile/FileReader.h"
#include "../foundation/Decibels.h"

#include <algorithm>
#include <limits>
#include <memory>

namespace duskstudio
{
namespace
{
struct Voice
{
    const AudioRegion* region = nullptr;
    std::unique_ptr<dusk::audio::FileReader> reader;
    std::int64_t fadeIn      = 0;
    std::int64_t fadeOut     = 0;
    std::int64_t overlapPrev = 0;
    std::int64_t overlapNext = 0;
};

std::int64_t endOf (const AudioRegion& r) noexcept { return r.timelineStart + r.lengthInSamples; }
} // namespace

bool regionsPlayOneFileStretch (const std::vector<AudioRegion>& regions)
{
    if (regions.empty()) return false;

    // A run of splits snapped to slightly different sub-sample boundaries can
    // leave neighbours a sample apart; they still read as one stretch.
    static constexpr std::int64_t kAbutTolerance = 1;
    const auto within = [] (std::int64_t v) noexcept { return v >= -kAbutTolerance && v <= kAbutTolerance; };

    auto reach = endOf (regions.front());
    for (std::size_t i = 1; i < regions.size(); ++i)
    {
        const auto& prev = regions[i - 1];
        const auto& cur  = regions[i];
        if (cur.file != prev.file) return false;

        const bool carriesOn = within (cur.timelineStart - endOf (prev))
                            && within (cur.sourceOffset - (prev.sourceOffset + prev.lengthInSamples));
        const bool sameAlignment = cur.sourceOffset - cur.timelineStart
                                == prev.sourceOffset - prev.timelineStart;
        if (! carriesOn && ! (sameAlignment && cur.timelineStart <= reach))
            return false;
        reach = std::max (reach, endOf (cur));
    }
    return true;
}

bool mixRegionsAsPlayed (const std::vector<AudioRegion>& regions,
                         dusk::audio::PlanarBuffer& mix, JoinedFades& outer)
{
    outer = {};
    if (regions.empty()) return true;

    const auto firstStart = regions.front().timelineStart;
    const AudioRegion* latestEnding = &regions.front();
    for (const auto& r : regions)
        if (endOf (r) > endOf (*latestEnding))
            latestEnding = &r;

    // Playback drops a region whose file will not open before it pairs
    // neighbours, so such a region shapes no overlap here either.
    std::vector<Voice> voices;
    voices.reserve (regions.size());
    for (const auto& reg : regions)
    {
        Voice v;
        v.region = &reg;
        v.reader = dusk::audio::FileReader::open (reg.filePath());
        if (v.reader == nullptr) continue;
        v.fadeIn  = reg.fadeInSamples;
        v.fadeOut = reg.fadeOutSamples;
        fitFadesToLength (v.fadeIn, v.fadeOut, reg.lengthInSamples);
        voices.push_back (std::move (v));
    }
    for (std::size_t i = 1; i < voices.size(); ++i)
    {
        const auto& a = *voices[i - 1].region;
        const auto& b = *voices[i].region;
        const auto overlap = overlapWithNext (a.timelineStart, a.lengthInSamples,
                                              b.timelineStart, b.lengthInSamples);
        voices[i - 1].overlapNext = overlap;
        voices[i].overlapPrev     = overlap;
    }

    const auto soundsAlone = [&voices] (const Voice& self, std::int64_t from, std::int64_t to)
    {
        for (const auto& other : voices)
            if (&other != &self && ! other.region->muted
                && other.region->timelineStart < to && endOf (*other.region) > from)
                return false;
        return true;
    };

    const int chs = mix.numChannels();
    for (auto& v : voices)
    {
        const auto& reg = *v.region;
        auto env = regionEnvelope (v.fadeIn, reg.fadeInShape, v.fadeOut, reg.fadeOutShape,
                                   v.overlapPrev, v.overlapNext);

        if (&reg == &regions.front() && v.fadeIn >= v.overlapPrev
            && soundsAlone (v, reg.timelineStart, reg.timelineStart + env.fadeIn))
        {
            outer.fadeInSamples = env.fadeIn;
            outer.fadeInShape   = env.fadeInShape;
            outer.fadeInAuto    = reg.fadeInAuto;
            env.fadeIn = 0;
        }
        if (&reg == latestEnding && v.fadeOut >= v.overlapNext
            && soundsAlone (v, endOf (reg) - env.fadeOut, endOf (reg)))
        {
            outer.fadeOutSamples = env.fadeOut;
            outer.fadeOutShape   = env.fadeOutShape;
            outer.fadeOutAuto    = reg.fadeOutAuto;
            env.fadeOut = 0;
        }

        if (reg.muted) continue;
        const int regSamples = (int) std::clamp<std::int64_t> (
            reg.lengthInSamples, 0, std::numeric_limits<int>::max());
        if (regSamples == 0) continue;
        const auto destOffset = reg.timelineStart - firstStart;
        if (destOffset < 0 || destOffset + regSamples > mix.numSamples())
            return false;

        dusk::audio::PlanarBuffer tmp;
        if (! tmp.setSize (chs, regSamples))
            return false;
        // A mono file plays in the centre of a stereo track, so it fills every
        // channel of the mix; the reader alone would leave the others silent.
        const int fileChs = std::clamp (v.reader->info().numChannels, 1, chs);
        if (v.reader->read (tmp.data(), fileChs, reg.sourceOffset, regSamples) != regSamples)
            return false;
        for (int c = fileChs; c < chs; ++c)
            std::copy_n (tmp.channel (0), regSamples, tmp.channel (c));

        const float regionGain = dusk::audio::decibelsToGain (
            std::clamp (reg.gainDb, -60.0f, 24.0f), -60.0f);
        for (int i = 0; i < regSamples; ++i)
        {
            const float gain = env.gainAt (regionGain, i, regSamples - i);
            for (int c = 0; c < chs; ++c)
                mix.channel (c)[destOffset + i] += tmp.channel (c)[i] * gain;
        }
    }
    return true;
}

int channelsAsPlayed (const std::vector<AudioRegion>& regions)
{
    int channels = 1;
    for (const auto& reg : regions)
        if (const auto reader = dusk::audio::FileReader::open (reg.filePath()))
            channels = std::max (channels, std::clamp (reader->info().numChannels, 1, 2));
    return channels;
}
} // namespace duskstudio
