#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestTempDirectory.h"
#include "engine/PlaybackEngine.h"
#include "engine/Transport.h"
#include "engine/audiofile/FileWriter.h"
#include "foundation/PlanarBuffer.h"
#include "session/RegionJoin.h"
#include "session/Session.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace duskstudio;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr int kFrames = 8192;
constexpr int kTrack  = 2;
constexpr std::int64_t kSeam = 64;

using SessionFile = std::decay_t<decltype (std::declval<const Session&>().getSessionDirectory())>;

// One phase at every level, so two files of it line up wherever their regions
// read them on the same alignment.
std::vector<float> tone (float level)
{
    std::vector<float> samples ((std::size_t) kFrames);
    for (int i = 0; i < kFrames; ++i)
        samples[(std::size_t) i] = level * std::sin (6.283185307f * 220.0f * (float) i / 48000.0f);
    return samples;
}

SessionFile writeMono (const std::filesystem::path& path, const float* samples, int frames)
{
    std::filesystem::create_directories (path.parent_path());
    auto writer = dusk::audio::FileWriter::create (path, { 48000.0, 1, 32 });
    REQUIRE (writer != nullptr);
    const float* channels[] { samples };
    REQUIRE (writer->write (channels, 1, frames));
    writer.reset();
    return SessionFile (path.u8string().c_str());
}

AudioRegion regionOf (const SessionFile& file, std::int64_t start, std::int64_t length, std::int64_t offset)
{
    AudioRegion r;
    r.file            = file;
    r.timelineStart   = start;
    r.lengthInSamples = length;
    r.sourceOffset    = offset;
    return r;
}

// What the track plays over [0, kFrames), read through the loop-start cache,
// which preparePlayback fills on this thread.
std::vector<float> played (Session& session, const std::vector<AudioRegion>& regions)
{
    session.track (kTrack).regions = regions;
    Transport transport;
    transport.setLoopRange (0, kFrames);
    transport.setLoopEnabled (true);
    PlaybackEngine pe (session);
    pe.bindTransport (transport);
    pe.prepare (kFrames);
    pe.preparePlayback();
    std::vector<float> out ((std::size_t) kFrames, 0.0f);
    pe.readForTrack (kTrack, 0, out.data(), nullptr, kFrames, 0, kFrames);
    pe.stopPlayback();
    return out;
}

struct Joined
{
    AudioRegion region;
    std::vector<float> file;
};

// The render half of a join: the mix written out, and the one region that
// plays it with the outer fades the mix left for it.
Joined joined (const std::filesystem::path& dir, const std::string& name, const std::vector<AudioRegion>& regions)
{
    const auto start = regions.front().timelineStart;
    std::int64_t end = start;
    for (const auto& r : regions) end = std::max (end, r.timelineStart + r.lengthInSamples);

    dusk::audio::PlanarBuffer mix;
    REQUIRE (mix.setSize (1, (int) (end - start)));
    JoinedFades outer;
    REQUIRE (mixRegionsAsPlayed (regions, mix, outer));

    Joined j;
    j.file.assign (mix.channel (0), mix.channel (0) + mix.numSamples());
    j.region = regionOf (writeMono (dir / "takes" / (name + ".wav"), mix.channel (0), mix.numSamples()),
                         start, end - start, 0);
    j.region.fadeInSamples  = outer.fadeInSamples;
    j.region.fadeInShape    = outer.fadeInShape;
    j.region.fadeOutSamples = outer.fadeOutSamples;
    j.region.fadeOutShape   = outer.fadeOutShape;
    return j;
}

float furthestApart (const std::vector<float>& a, const std::vector<float>& b)
{
    REQUIRE (a.size() == b.size());
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) worst = std::max (worst, std::abs (a[i] - b[i]));
    return worst;
}

float peakOf (const std::vector<float>& samples, std::int64_t from, std::int64_t to)
{
    float peak = 0.0f;
    for (auto i = from; i < to; ++i) peak = std::max (peak, std::abs (samples[(std::size_t) i]));
    return peak;
}

struct Fixture
{
    test::TempDirectory dir { "dusk-join-" };
    SessionFile loud  = writeMono (dir.path() / "audio" / "loud.wav",  tone (0.5f).data(), kFrames);
    SessionFile quiet = writeMono (dir.path() / "audio" / "quiet.wav", tone (0.3f).data(), kFrames);
    std::unique_ptr<Session> session = std::make_unique<Session>();
};
} // namespace

TEST_CASE ("Regions of one file on one alignment join as one stretch", "[region][join]")
{
    const SessionFile file ("/tmp/one.wav"), other ("/tmp/other.wav");
    auto head   = regionOf (file, 1000, 4832, 200);
    auto middle = regionOf (file, 5768, 4864, 4968);
    auto tail   = regionOf (file, 10568, 3832, 9768);

    CHECK (regionsPlayOneFileStretch ({ head, middle, tail }));
    CHECK (regionsPlayOneFileStretch ({ regionOf (file, 0, 100, 50), regionOf (file, 100, 100, 150) }));
    CHECK (regionsPlayOneFileStretch ({ regionOf (file, 0, 100, 50), regionOf (file, 101, 100, 151) }));

    SECTION ("a region inside a longer one is part of its stretch")
    {
        CHECK (regionsPlayOneFileStretch (
            { regionOf (file, 0, 1000, 0), regionOf (file, 200, 100, 200), regionOf (file, 900, 400, 900) }));
    }
    SECTION ("an overlap that reads the file from somewhere else is not")
    {
        middle.sourceOffset += 10;
        CHECK_FALSE (regionsPlayOneFileStretch ({ head, middle, tail }));
    }
    SECTION ("a gap is not, even on one alignment")
    {
        CHECK_FALSE (regionsPlayOneFileStretch ({ regionOf (file, 0, 100, 0), regionOf (file, 200, 100, 200) }));
    }
    SECTION ("two files are not")
    {
        middle.file = other;
        CHECK_FALSE (regionsPlayOneFileStretch ({ head, middle, tail }));
    }
    SECTION ("no regions are not")
    {
        CHECK_FALSE (regionsPlayOneFileStretch ({}));
    }
}

TEST_CASE ("A join render holds the crossfade playback gives a comp seam", "[region][join][playback]")
{
    Fixture f;
    const std::int64_t overlapStart = 4000 - kSeam / 2, overlapEnd = 4000 + kSeam / 2;
    auto left = regionOf (f.loud, 0, overlapEnd, 0);
    left.fadeInSamples  = 480;
    left.fadeInShape    = FadeShape::Exp;
    left.fadeOutSamples = kSeam;
    left.fadeOutShape   = FadeShape::RaisedCosine;
    auto right = regionOf (f.quiet, overlapStart, kFrames - overlapStart, overlapStart);
    right.fadeInSamples  = kSeam;
    right.fadeInShape    = FadeShape::RaisedCosine;
    right.fadeOutSamples = 960;
    right.fadeOutShape   = FadeShape::Log;

    const auto before = played (*f.session, { left, right });
    const auto join = joined (f.dir.path(), "seam", { left, right });

    CHECK_THAT (furthestApart (played (*f.session, { join.region }), before), WithinAbs (0.0f, 1.0e-4f));
    CHECK (peakOf (join.file, overlapStart, overlapEnd) <= 0.5f + 1.0e-4f);

    // The outer fades ride on the region, so the file is unfaded under them.
    CHECK (join.region.fadeInSamples == 480);
    CHECK (join.region.fadeInShape == FadeShape::Exp);
    CHECK (join.region.fadeOutSamples == 960);
    CHECK (join.region.fadeOutShape == FadeShape::Log);
    const auto loud = tone (0.5f), quiet = tone (0.3f);
    CHECK_THAT (join.file[100], WithinAbs (loud[100], 1.0e-6f));
    CHECK_THAT (join.file[(std::size_t) kFrames - 100], WithinAbs (quiet[(std::size_t) kFrames - 100], 1.0e-6f));
}

TEST_CASE ("A join render plays as its regions did however they overlap", "[region][join][playback]")
{
    Fixture f;
    std::vector<AudioRegion> regions;

    SECTION ("an overlap with no fades crossfades at equal power")
    {
        regions = { regionOf (f.loud, 0, 4100, 0), regionOf (f.quiet, 3900, kFrames - 3900, 3900) };
    }
    SECTION ("a fade shorter than the overlap gives way to it")
    {
        auto a = regionOf (f.loud, 0, 4100, 0);
        a.fadeOutSamples = 50;
        a.fadeOutShape = FadeShape::Sigmoid;
        auto b = regionOf (f.quiet, 3900, kFrames - 3900, 3900);
        b.fadeInSamples = 600;
        b.fadeInShape = FadeShape::Linear;
        regions = { a, b };
    }
    SECTION ("each region keeps its own gain, and a muted one is silent")
    {
        auto a = regionOf (f.loud, 0, 3000, 0);
        a.gainDb = -6.0f;
        auto b = regionOf (f.quiet, 2900, 2000, 2900);
        b.muted = true;
        auto c = regionOf (f.loud, 4800, kFrames - 4800, 100);
        c.gainDb = 3.0f;
        c.fadeInSamples = 200;
        regions = { a, b, c };
    }
    SECTION ("a region inside a longer one")
    {
        auto a = regionOf (f.loud, 0, kFrames, 0);
        a.fadeInSamples = 300;
        a.fadeOutSamples = 100;
        regions = { a, regionOf (f.quiet, 2000, 1000, 500) };
    }
    SECTION ("a second region under the first one's fade-in")
    {
        auto a = regionOf (f.loud, 0, 4000, 0);
        a.fadeInSamples = 2000;
        a.fadeInShape = FadeShape::Log;
        regions = { a, regionOf (f.quiet, 1000, kFrames - 1000, 0) };
    }
    SECTION ("fades longer than their region")
    {
        auto a = regionOf (f.loud, 0, 1000, 0);
        a.fadeInSamples = 900;
        a.fadeOutSamples = 900;
        auto b = regionOf (f.quiet, 2000, 1000, 0);
        b.fadeInSamples = 5000;
        regions = { a, b };
    }
    SECTION ("a gap stays silent")
    {
        regions = { regionOf (f.loud, 0, 2000, 0), regionOf (f.loud, 3000, 2000, 2000) };
        const auto join = joined (f.dir.path(), "gap", regions);
        CHECK_THAT (peakOf (join.file, 2000, 3000), WithinAbs (0.0f, 1.0e-9f));
        CHECK (peakOf (join.file, 0, 2000) > 0.4f);
        CHECK (peakOf (join.file, 3000, 5000) > 0.4f);
    }

    const auto before = played (*f.session, regions);
    auto joinedPlay = played (*f.session, { joined (f.dir.path(), "any", regions).region });
    CHECK_THAT (furthestApart (joinedPlay, before), WithinAbs (0.0f, 1.0e-4f));
}

TEST_CASE ("A join render refuses a region its file cannot fill", "[region][join]")
{
    Fixture f;
    dusk::audio::PlanarBuffer mix;
    REQUIRE (mix.setSize (1, kFrames + 1000));
    JoinedFades outer;
    CHECK_FALSE (mixRegionsAsPlayed ({ regionOf (f.loud, 0, 1000, 0), regionOf (f.quiet, 1000, kFrames, 100) },
                                     mix, outer));
}
