#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"
#include "session/TakeComp.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <memory>

using namespace duskstudio;
using Catch::Matchers::WithinAbs;

namespace
{
AudioRegion regionAt (std::int64_t start, std::int64_t length, std::int64_t offset = 0)
{
    AudioRegion r;
    r.timelineStart   = start;
    r.lengthInSamples = length;
    r.sourceOffset    = offset;
    return r;
}

std::int64_t endOf (const AudioRegion& r) { return r.timelineStart + r.lengthInSamples; }

const AudioRegion* startingAt (const std::vector<AudioRegion>& regs, std::int64_t start)
{
    for (const auto& r : regs)
        if (r.timelineStart == start) return &r;
    return nullptr;
}

// Where in which file the track plays timeline sample t, from the last region
// covering it outside every seam (a seam has two sources by design).
struct SourcePoint
{
    juce::File file;
    std::int64_t frame = -1;
};

SourcePoint sourceAt (const std::vector<AudioRegion>& regs, std::int64_t t)
{
    SourcePoint point;
    int covering = 0;
    for (const auto& r : regs)
        if (t >= r.timelineStart && t < endOf (r))
        {
            ++covering;
            point = { r.file, r.sourceOffset + (t - r.timelineStart) };
        }
    return covering == 1 ? point : SourcePoint {};
}

AudioTake takeAt (TakeId id, const juce::File& file, std::int64_t start, std::int64_t length,
                  std::int64_t offset)
{
    AudioTake take;
    take.id              = id;
    take.name            = "Take " + std::to_string (id);
    take.file            = file;
    take.timelineStart   = start;
    take.lengthInSamples = length;
    take.sourceOffset    = offset;
    return take;
}
// A track of its own session, so a carve can give a region a take.
struct CarveTrack
{
    std::unique_ptr<Session> session = std::make_unique<Session>();
    Track& track = session->track (0);
    std::vector<AudioRegion>& regs = track.regions;

    void carve (std::int64_t start, std::int64_t end) { detail::carveRegions (*session, track, start, end); }
};
} // namespace

TEST_CASE ("carveRegions removes, splits and trims what the range covers", "[take][comp]")
{
    CarveTrack t;
    auto& regs = t.regs;

    SECTION ("a region inside the range goes")
    {
        regs.push_back (regionAt (1000, 1000));
        t.carve (500, 2500);
        REQUIRE (regs.empty());
    }

    SECTION ("a region spanning the range splits into a left and a right fragment")
    {
        auto spanning = regionAt (0, 10000, 100);
        spanning.fadeInSamples  = 10;
        spanning.fadeOutSamples = 20;
        regs.push_back (spanning);
        t.carve (4000, 6000);
        REQUIRE (regs.size() == 2);

        const auto* left  = startingAt (regs, 0);
        const auto* right = startingAt (regs, 6000 - kPunchFadeSamples);
        REQUIRE (left != nullptr);
        REQUIRE (right != nullptr);
        CHECK (endOf (*left) == 4000 + kPunchFadeSamples);
        CHECK (left->sourceOffset == 100);
        CHECK (left->fadeInSamples == 10);
        CHECK (left->fadeOutSamples == kPunchFadeSamples);
        CHECK (left->fadeOutShape == FadeShape::RaisedCosine);

        CHECK (endOf (*right) == 10000);
        CHECK (right->sourceOffset == 100 + (6000 - kPunchFadeSamples));
        CHECK (right->fadeInSamples == kPunchFadeSamples);
        CHECK (right->fadeInShape == FadeShape::RaisedCosine);
        CHECK (right->fadeOutSamples == 20);
    }

    SECTION ("a region over the left edge is trimmed to reach one fade into the range")
    {
        regs.push_back (regionAt (0, 5000, 7));
        t.carve (4000, 8000);
        REQUIRE (regs.size() == 1);
        CHECK (regs[0].timelineStart == 0);
        CHECK (regs[0].sourceOffset == 7);
        CHECK (endOf (regs[0]) == 4000 + kPunchFadeSamples);
        CHECK (regs[0].fadeOutSamples == kPunchFadeSamples);
        CHECK (regs[0].fadeOutShape == FadeShape::RaisedCosine);
    }

    SECTION ("a region over the right edge starts one fade before the range end")
    {
        regs.push_back (regionAt (6000, 4000, 50));
        t.carve (4000, 8000);
        REQUIRE (regs.size() == 1);
        CHECK (regs[0].timelineStart == 8000 - kPunchFadeSamples);
        CHECK (endOf (regs[0]) == 10000);
        CHECK (regs[0].sourceOffset == 50 + (8000 - kPunchFadeSamples - 6000));
        CHECK (regs[0].fadeInSamples == kPunchFadeSamples);
        CHECK (regs[0].fadeInShape == FadeShape::RaisedCosine);
    }

    SECTION ("a region ending inside the seam keeps its own end")
    {
        regs.push_back (regionAt (0, 4020, 7));
        t.carve (4000, 8000);
        REQUIRE (regs.size() == 1);
        CHECK (regs[0].timelineStart == 0);
        CHECK (regs[0].sourceOffset == 7);
        CHECK (endOf (regs[0]) == 4020);
        CHECK (regs[0].fadeOutSamples == 20);
        CHECK (regs[0].fadeOutShape == FadeShape::RaisedCosine);
    }

    SECTION ("a region starting inside the seam keeps its own start and source offset")
    {
        regs.push_back (regionAt (7980, 2020, 0));
        t.carve (4000, 8000);
        REQUIRE (regs.size() == 1);
        CHECK (regs[0].timelineStart == 7980);
        CHECK (regs[0].sourceOffset == 0);
        CHECK (endOf (regs[0]) == 10000);
        CHECK (regs[0].fadeInSamples == 20);
        CHECK (regs[0].fadeInShape == FadeShape::RaisedCosine);
    }

    SECTION ("regions that only touch the range are left alone")
    {
        regs.push_back (regionAt (0, 4000));
        regs.push_back (regionAt (8000, 4000, 3));
        t.carve (4000, 8000);
        REQUIRE (regs.size() == 2);
        CHECK (regs[0].timelineStart == 0);
        CHECK (regs[0].lengthInSamples == 4000);
        CHECK (regs[0].fadeOutSamples == 0);
        CHECK (regs[1].timelineStart == 8000);
        CHECK (regs[1].sourceOffset == 3);
        CHECK (regs[1].fadeInSamples == 0);
        CHECK (t.track.takes.empty());
        CHECK (regs[0].takeId == 0);
        CHECK (regs[1].takeId == 0);
    }
}

TEST_CASE ("carveRegions seam fades are 64-sample raised cosines capped at half the range",
           "[take][comp]")
{
    REQUIRE (kPunchFadeSamples == 64);

    CarveTrack t;
    auto& regs = t.regs;

    SECTION ("a short range halves the seam fade")
    {
        regs = { regionAt (0, 10000) };
        t.carve (4000, 4100);
        REQUIRE (regs.size() == 2);
        const auto* left  = startingAt (regs, 0);
        const auto* right = startingAt (regs, 4100 - 50);
        REQUIRE (left != nullptr);
        REQUIRE (right != nullptr);
        CHECK (left->fadeOutSamples == 50);
        CHECK (endOf (*left) == 4050);
        CHECK (right->fadeInSamples == 50);
    }

    SECTION ("a one-sample range cuts without a fade")
    {
        regs = { regionAt (0, 10000) };
        t.carve (4000, 4001);
        REQUIRE (regs.size() == 2);
        CHECK (startingAt (regs, 0)->fadeOutSamples == 0);
        CHECK (endOf (*startingAt (regs, 0)) == 4000);
        CHECK (startingAt (regs, 4001)->fadeInSamples == 0);
    }

    SECTION ("the far fade of a trimmed region shrinks so the two never overlap")
    {
        auto r = regionAt (3900, 200);
        r.fadeInSamples = 150;
        regs = { r };
        t.carve (4000, 8000);
        REQUIRE (regs.size() == 1);
        CHECK (regs[0].lengthInSamples == 100 + kPunchFadeSamples);
        CHECK (regs[0].fadeInSamples + regs[0].fadeOutSamples <= regs[0].lengthInSamples);
    }

    SECTION ("the raised cosine is silent at the seam, full at the far edge, half way between")
    {
        CHECK_THAT (applyFadeShape (0.0f, FadeShape::RaisedCosine), WithinAbs (0.0f, 1e-6));
        CHECK_THAT (applyFadeShape (0.5f, FadeShape::RaisedCosine), WithinAbs (0.5f, 1e-6));
        CHECK_THAT (applyFadeShape (1.0f, FadeShape::RaisedCosine), WithinAbs (1.0f, 1e-6));
        CHECK_THAT (applyFadeShape (0.25f, FadeShape::RaisedCosine),
                    WithinAbs (0.5f * (1.0f - std::cos (0.25f * 3.14159265f)), 1e-6));
    }
}

TEST_CASE ("carveRegions makes a region naming no take a take of its own before cutting it",
           "[take][comp][regression]")
{
    using Span = std::pair<std::int64_t, std::int64_t>;
    CarveTrack t;
    const juce::File importedFile ("/tmp/imported.wav"), recordedFile ("/tmp/recorded.wav");
    t.track.takes.push_back (takeAt (5, recordedFile, 20000, 4000, 0));
    t.session->seedTakeIdAllocator (5);

    auto plain = regionAt (0, 10000, 100);
    plain.file = importedFile;
    plain.numChannels = 2;
    plain.provenance = { 0, 0, false };
    auto recorded = regionAt (20000, 4000, 0);
    recorded.file = recordedFile;
    recorded.takeId = 5;
    auto elsewhere = regionAt (30000, 1000, 0);
    elsewhere.file = importedFile;
    t.regs = { plain, recorded, elsewhere };

    t.carve (4000, 22000);

    REQUIRE (t.track.takes.size() == 2);
    const auto& adopted = t.track.takes[1];
    CHECK (adopted.id == 6);
    CHECK (adopted.name == "Take 6");
    CHECK (adopted.file == importedFile);
    CHECK (adopted.timelineStart == 0);
    CHECK (adopted.lengthInSamples == 10000);
    CHECK (adopted.sourceOffset == 100);
    CHECK (adopted.numChannels == 2);
    CHECK (t.track.takes[0].id == 5);
    CHECK (takeCoverage (t.track, 6) == std::vector<Span> { { 0, 4000 + kPunchFadeSamples } });
    const auto* left = startingAt (t.regs, 0);
    REQUIRE (left != nullptr);
    CHECK (left->takeId == 6);
    const auto* after = startingAt (t.regs, 30000);
    REQUIRE (after != nullptr);
    CHECK (after->takeId == 0);
    CHECK (startingAt (t.regs, 22000 - kPunchFadeSamples)->takeId == 5);
}

TEST_CASE ("nextTakeName numbers past the highest take still named Take N", "[take][comp]")
{
    auto session = std::make_unique<Session>();
    auto& track = session->track (0);
    CHECK (nextTakeName (track) == "Take 1");
    for (const char* name : { "Take 2", "Take 10", "Take 11 keeper", "Lead", "Take", "Take -4",
                              "take 40", "Take 1234567890", "Take 3" })
    {
        auto take = takeAt (1, juce::File(), 0, 10, 0);
        take.name = name;
        track.takes.push_back (take);
    }
    CHECK (nextTakeName (track) == "Take 11");
}

TEST_CASE ("A take made of a region naming no take joins the track in recording order",
           "[take][comp][regression]")
{
    auto session = std::make_unique<Session>();
    auto& track = session->track (0);
    const juce::File plainFile ("/tmp/plain.wav");
    auto older = takeAt (1, juce::File ("/tmp/older.wav"), 0, 48000, 0);
    older.provenance.capturedAtMs = 100;
    auto newer = takeAt (2, juce::File ("/tmp/newer.wav"), 0, 48000, 0);
    newer.provenance = { 300, 2, false };
    track.takes = { older, newer };
    session->seedTakeIdAllocator (2);
    auto plain = regionAt (1000, 2000, 0);
    plain.file = plainFile;
    const auto idsInOrder = [&track]
    {
        std::vector<TakeId> ids;
        for (const auto& take : track.takes) ids.push_back (take.id);
        return ids;
    };

    SECTION ("without a capture time it goes just before the take being placed")
    {
        track.regions = { plain };
        promoteTakeRange (*session, track, track.takes[1], 0, 48000);
        CHECK (idsInOrder() == std::vector<TakeId> { 1, 3, 2 });
        CHECK (track.takes[1].file == plainFile);
        CHECK (track.takes[1].name == "Take 3");
    }

    SECTION ("with a capture time it goes before the first take captured after it")
    {
        plain.provenance.capturedAtMs = 200;
        track.regions = { plain };
        promoteTakeRange (*session, track, track.takes[0], 0, 48000);
        CHECK (idsInOrder() == std::vector<TakeId> { 1, 3, 2 });
    }

    SECTION ("a capture time shared with a take is ordered by loop pass")
    {
        plain.provenance = { 300, 1, false };
        track.regions = { plain };
        promoteTakeRange (*session, track, track.takes[0], 0, 48000);
        CHECK (idsInOrder() == std::vector<TakeId> { 1, 3, 2 });
    }

    SECTION ("captured after every take it goes last")
    {
        plain.provenance.capturedAtMs = 400;
        track.regions = { plain };
        promoteTakeRange (*session, track, track.takes[0], 0, 48000);
        CHECK (idsInOrder() == std::vector<TakeId> { 1, 2, 3 });
    }

    SECTION ("placing a take the track does not hold yet puts it last, several in timeline order")
    {
        auto later = plain;
        later.timelineStart = 5000;
        later.file = juce::File ("/tmp/plain-later.wav");
        track.regions = { later, plain };
        auto incoming = takeAt (9, juce::File ("/tmp/incoming.wav"), 0, 48000, 0);
        session->seedTakeIdAllocator (9);
        promoteTakeRange (*session, track, incoming, 0, 48000);
        CHECK (idsInOrder() == std::vector<TakeId> { 1, 2, 10, 11 });
        CHECK (track.takes[2].file == plainFile);
        CHECK (track.takes[3].file == later.file);
        CHECK (track.takes[2].name == "Take 3");
        CHECK (track.takes[3].name == "Take 4");
        REQUIRE (track.regions.size() == 1);
        CHECK (track.regions[0].takeId == 9);
    }
}

TEST_CASE ("regionFromTake cuts the take where it plays and clamps to its span", "[take][comp]")
{
    const juce::File file ("/tmp/take-b.wav");
    auto take = takeAt (7, file, 10000, 5000, 200);
    take.numChannels = 2;
    take.provenance = { 1234, 3, true };

    SECTION ("a range inside the take")
    {
        const auto r = regionFromTake (take, 12000, 13000);
        REQUIRE (r.has_value());
        CHECK (r->file == file);
        CHECK (r->timelineStart == 12000);
        CHECK (r->lengthInSamples == 1000);
        CHECK (r->sourceOffset == 200 + 2000);
        CHECK (r->numChannels == 2);
        CHECK (r->takeId == 7);
        CHECK (r->provenance.capturedAtMs == 1234);
        CHECK (r->provenance.loopPassOrdinal == 3);
        CHECK (r->provenance.partialPass);
        CHECK_THAT (r->gainDb, WithinAbs (0.0f, 1e-9));
        CHECK (r->fadeInSamples == 0);
        CHECK (r->fadeOutSamples == 0);
    }

    SECTION ("a range starting before the take starts where the take does")
    {
        const auto r = regionFromTake (take, 5000, 12000);
        REQUIRE (r.has_value());
        CHECK (r->timelineStart == 10000);
        CHECK (r->lengthInSamples == 2000);
        CHECK (r->sourceOffset == 200);
    }

    SECTION ("a range running past the take ends where the take does")
    {
        const auto r = regionFromTake (take, 14000, 99999);
        REQUIRE (r.has_value());
        CHECK (r->timelineStart == 14000);
        CHECK (endOf (*r) == 15000);
        CHECK (r->sourceOffset == 200 + 4000);
    }

    SECTION ("a range outside the take or empty gives nothing")
    {
        CHECK_FALSE (regionFromTake (take, 20000, 30000).has_value());
        CHECK_FALSE (regionFromTake (take, 0, 10000).has_value());
        CHECK_FALSE (regionFromTake (take, 12000, 12000).has_value());
        CHECK_FALSE (regionFromTake (take, 13000, 12000).has_value());
    }
}

TEST_CASE ("promoteTakeRange plays the take over the range and leaves the rest as it was",
           "[take][comp]")
{
    auto session = std::make_unique<Session>();
    auto& track = session->track (0);
    const juce::File fileA ("/tmp/take-a.wav"), fileB ("/tmp/take-b.wav");
    track.takes.push_back (takeAt (1, fileA, 0, 48000, 0));
    track.takes.push_back (takeAt (2, fileB, 0, 48000, 300));
    auto live = regionAt (0, 48000);
    live.file = fileA;
    live.takeId = 1;
    track.regions.push_back (live);

    const auto before = track.regions;
    promoteTakeRange (*session, track, track.takes[1], 10000, 20000);

    REQUIRE (track.regions.size() == 3);
    const auto* promoted = startingAt (track.regions, 10000);
    REQUIRE (promoted != nullptr);
    CHECK (promoted->takeId == 2);
    CHECK (promoted->file == fileB);
    CHECK (promoted->lengthInSamples == 10000);
    CHECK (promoted->sourceOffset == 300 + 10000);
    CHECK_THAT (promoted->gainDb, WithinAbs (0.0f, 1e-9));
    CHECK (promoted->fadeInSamples == kPunchFadeSamples);
    CHECK (promoted->fadeInShape == FadeShape::RaisedCosine);
    CHECK (promoted->fadeOutSamples == kPunchFadeSamples);
    CHECK (promoted->fadeOutShape == FadeShape::RaisedCosine);

    for (const std::int64_t t : { (std::int64_t) 0, (std::int64_t) 5000, (std::int64_t) 9999,
                                  (std::int64_t) 20000 + kPunchFadeSamples, (std::int64_t) 47999 })
    {
        const auto was = sourceAt (before, t);
        const auto now = sourceAt (track.regions, t);
        INFO ("timeline sample " << t);
        CHECK (now.file == was.file);
        CHECK (now.frame == was.frame);
    }
    for (const std::int64_t t : { (std::int64_t) 10000 + kPunchFadeSamples, (std::int64_t) 15000,
                                  (std::int64_t) 19999 - kPunchFadeSamples })
    {
        const auto now = sourceAt (track.regions, t);
        INFO ("timeline sample " << t);
        CHECK (now.file == fileB);
        CHECK (now.frame == 300 + t);
    }
    for (const auto& r : track.regions)
        if (r.file == fileA) CHECK (r.takeId == 1);

    SECTION ("the range is clamped to the take")
    {
        track.takes.push_back (takeAt (3, fileB, 30000, 5000, 0));
        promoteTakeRange (*session, track, track.takes[2], 25000, 60000);
        const auto* clamped = startingAt (track.regions, 30000);
        REQUIRE (clamped != nullptr);
        CHECK (clamped->takeId == 3);
        CHECK (endOf (*clamped) == 35000);
        CHECK (sourceAt (track.regions, 36000).file == fileA);
        CHECK (sourceAt (track.regions, 36000).frame == 36000);
    }

    SECTION ("a range the take does not reach changes nothing")
    {
        track.takes.push_back (takeAt (3, fileB, 30000, 5000, 0));
        const auto untouched = track.regions.size();
        promoteTakeRange (*session, track, track.takes[2], 40000, 45000);
        CHECK (track.regions.size() == untouched);
    }
}

TEST_CASE ("promoteTakeRange onto empty timeline adds no seam fades", "[take][comp]")
{
    auto session = std::make_unique<Session>();
    auto& track = session->track (0);
    track.takes.push_back (takeAt (4, juce::File ("/tmp/take-c.wav"), 1000, 2000, 0));
    promoteTakeRange (*session, track, track.takes[0], 0, 99999);
    REQUIRE (track.regions.size() == 1);
    CHECK (track.regions[0].timelineStart == 1000);
    CHECK (track.regions[0].lengthInSamples == 2000);
    CHECK (track.regions[0].fadeInSamples == 0);
    CHECK (track.regions[0].fadeOutSamples == 0);
}

TEST_CASE ("takeCoverage reports the parts of a take the track plays, sorted and merged",
           "[take][comp]")
{
    using Span = std::pair<std::int64_t, std::int64_t>;
    auto session = std::make_unique<Session>();
    auto& track = session->track (0);
    const juce::File fileA ("/tmp/take-a.wav"), fileB ("/tmp/take-b.wav");
    track.takes.push_back (takeAt (5, fileA, 0, 5000, 1000));
    track.takes.push_back (takeAt (9, fileB, 0, 5000, 0));

    // Take 5 holds timeline sample t at source frame 1000 + t.
    auto named = [&track] (TakeId id, const juce::File& file, std::int64_t start,
                           std::int64_t length, std::int64_t offset)
    {
        auto r = regionAt (start, length, offset);
        r.file   = file;
        r.takeId = id;
        track.regions.push_back (r);
    };
    named (5, fileA, 1000, 100, 2000);
    named (5, fileA, 300, 100, 1300);
    named (9, fileB, 400, 600, 400);
    named (5, fileA, 100, 200, 1100);
    named (5, fileA, 1050, 200, 2050);
    named (5, fileA, 2000, 0, 3000);

    const std::vector<Span> unmoved { { 100, 400 }, { 1000, 1250 } };
    CHECK (takeCoverage (track, 5) == unmoved);
    CHECK (takeCoverage (track, 9) == std::vector<Span> { { 400, 1000 } });
    CHECK (takeCoverage (track, 6).empty());
    CHECK (takeCoverage (track, 0).empty());

    SECTION ("a moved region counts where its audio lies in the take")
    {
        named (5, fileA, 40000, 500, 1000 + 3000);
        CHECK (takeCoverage (track, 5) == std::vector<Span> { { 100, 400 }, { 1000, 1250 }, { 3000, 3500 } });
    }

    SECTION ("a region naming the take but reading another file does not count")
    {
        named (5, fileB, 3000, 500, 3000);
        CHECK (takeCoverage (track, 5) == unmoved);
    }

    SECTION ("a region reaching past the take counts only up to the take's end")
    {
        named (5, fileA, 4800, 400, 1000 + 4800);
        CHECK (takeCoverage (track, 5).back() == Span { 4800, 5000 });
    }

    SECTION ("a region naming a take the track lacks does not count")
    {
        track.takes.erase (track.takes.begin());
        CHECK (takeCoverage (track, 5).empty());
    }
}

TEST_CASE ("findTake finds a take by id and never by the none id", "[take][comp]")
{
    auto session = std::make_unique<Session>();
    auto& track = session->track (0);
    track.takes.push_back (takeAt (3, juce::File(), 0, 10, 0));
    track.takes.push_back (takeAt (8, juce::File(), 0, 10, 0));
    REQUIRE (findTake (track, 8) == &track.takes[1]);
    CHECK (findTake (track, 3) == &track.takes[0]);
    CHECK (findTake (track, 4) == nullptr);
    CHECK (findTake (track, 0) == nullptr);
}
