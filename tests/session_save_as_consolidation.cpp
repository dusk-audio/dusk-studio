#include <catch2/catch_test_macros.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

using namespace duskstudio;

namespace
{
juce::File makeTempDir (const char* tag)
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile (juce::String (tag)
                                   + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    return dir;
}

juce::File makeFakeWav (const juce::File& at)
{
    at.getParentDirectory().createDirectory();
    at.replaceWithText ("fake-wav");
    return at;
}
} // namespace

// Save As must take the session's audio along: consolidateInto copies every
// session-owned file into the new directory and repoints the model, so the
// subsequent serialize emits relative paths. Before this existed, Save As
// wrote absolute paths into the OLD folder — deleting it lost all audio.
TEST_CASE ("consolidateInto copies session audio and repoints the model",
           "[session][serializer][consolidate]")
{
    const auto dirA = makeTempDir ("dusk-consolidate-a-");
    const auto dirB = makeTempDir ("dusk-consolidate-b-");
    const struct Cleanup
    {
        juce::File a, b;
        ~Cleanup() { a.deleteRecursively(); b.deleteRecursively(); }
    } cleanup { dirA, dirB };

    Session s;
    s.setSessionDirectory (dirA);

    const auto take1   = makeFakeWav (dirA.getChildFile ("audio/take1.wav"));
    const auto take0   = makeFakeWav (dirA.getChildFile ("audio/take0.wav"));
    const auto freeze  = makeFakeWav (dirA.getChildFile ("audio/freeze/freeze_track02.wav"));
    const auto mixdown = makeFakeWav (dirA.getChildFile ("mixdown.wav"));

    {
        AudioRegion r;
        r.file            = take1;
        r.lengthInSamples = 1000;
        TakeRef prior;
        prior.file            = take0;
        prior.lengthInSamples = 500;
        r.previousTakes.push_back (prior);
        s.track (0).regions.push_back (r);

        s.track (1).frozen.store (true);
        s.track (1).frozenAudioPath        = freeze.getFullPathName();
        s.track (1).frozenRegion.file      = freeze;
        s.track (1).frozenRegion.lengthInSamples = 1000;

        s.mastering().sourceFile = mixdown;
    }

    const auto res = SessionSerializer::consolidateInto (s, dirB);
    REQUIRE (res.ok);
    REQUIRE (res.filesCopied == 4);
    REQUIRE (res.missingSources.empty());

    // Relative subpaths preserved, including audio/freeze/ and the root mixdown.
    REQUIRE (dirB.getChildFile ("audio/take1.wav").existsAsFile());
    REQUIRE (dirB.getChildFile ("audio/take0.wav").existsAsFile());
    REQUIRE (dirB.getChildFile ("audio/freeze/freeze_track02.wav").existsAsFile());
    REQUIRE (dirB.getChildFile ("mixdown.wav").existsAsFile());

    // Model repointed into dirB.
    REQUIRE (s.track (0).regions[0].file == dirB.getChildFile ("audio/take1.wav"));
    REQUIRE (s.track (0).regions[0].previousTakes[0].file
                 == dirB.getChildFile ("audio/take0.wav"));
    REQUIRE (s.track (1).frozenAudioPath
                 == dirB.getChildFile ("audio/freeze/freeze_track02.wav").getFullPathName());
    REQUIRE (s.track (1).frozenRegion.file
                 == dirB.getChildFile ("audio/freeze/freeze_track02.wav"));
    REQUIRE (s.mastering().sourceFile == dirB.getChildFile ("mixdown.wav"));

    // The serialized session must not reference dirA anywhere, and a fresh
    // load from dirB must resolve every file.
    s.setSessionDirectory (dirB);
    const auto json = SessionSerializer::serialize (s);
    REQUIRE (! json.contains (dirA.getFullPathName()));

    const auto target = dirB.getChildFile ("session.json");
    REQUIRE (SessionSerializer::writeAtomic (target, json));
    Session loaded;
    loaded.setSessionDirectory (dirB);
    REQUIRE (SessionSerializer::load (loaded, target));
    REQUIRE (loaded.missingAudioFilesAfterLoad.empty());
    REQUIRE (loaded.track (0).regions[0].file == dirB.getChildFile ("audio/take1.wav"));
}

TEST_CASE ("consolidateInto pulls external files into audio/ with collision suffixes",
           "[session][serializer][consolidate]")
{
    const auto dirA = makeTempDir ("dusk-consolidate-a-");
    const auto dirB = makeTempDir ("dusk-consolidate-b-");
    const auto ext1Dir = makeTempDir ("dusk-consolidate-x1-");
    const auto ext2Dir = makeTempDir ("dusk-consolidate-x2-");
    const struct Cleanup
    {
        juce::File a, b, x1, x2;
        ~Cleanup()
        {
            a.deleteRecursively(); b.deleteRecursively();
            x1.deleteRecursively(); x2.deleteRecursively();
        }
    } cleanup { dirA, dirB, ext1Dir, ext2Dir };

    Session s;
    s.setSessionDirectory (dirA);

    const auto ext1 = makeFakeWav (ext1Dir.getChildFile ("loop.wav"));
    const auto ext2 = makeFakeWav (ext2Dir.getChildFile ("loop.wav"));
    {
        AudioRegion r1; r1.file = ext1; r1.lengthInSamples = 100;
        AudioRegion r2; r2.file = ext2; r2.lengthInSamples = 100;
        s.track (0).regions.push_back (r1);
        s.track (0).regions.push_back (r2);
    }

    const auto res = SessionSerializer::consolidateInto (s, dirB);
    REQUIRE (res.ok);
    REQUIRE (res.filesCopied == 2);

    REQUIRE (dirB.getChildFile ("audio/loop.wav").existsAsFile());
    REQUIRE (dirB.getChildFile ("audio/loop_2.wav").existsAsFile());
    REQUIRE (s.track (0).regions[0].file != s.track (0).regions[1].file);
    REQUIRE (s.track (0).regions[0].file.isAChildOf (dirB));
    REQUIRE (s.track (0).regions[1].file.isAChildOf (dirB));
}

TEST_CASE ("consolidateInto edge cases", "[session][serializer][consolidate]")
{
    const auto dirA = makeTempDir ("dusk-consolidate-a-");
    const auto dirB = makeTempDir ("dusk-consolidate-b-");
    const auto extDir = makeTempDir ("dusk-consolidate-x-");
    const struct Cleanup
    {
        juce::File a, b, x;
        ~Cleanup()
        {
            b.setReadOnly (false);
            a.deleteRecursively(); b.deleteRecursively(); x.deleteRecursively();
        }
    } cleanup { dirA, dirB, extDir };

    Session s;
    s.setSessionDirectory (dirA);
    const auto take = makeFakeWav (dirA.getChildFile ("audio/take.wav"));
    {
        AudioRegion r;
        r.file            = take;
        r.lengthInSamples = 100;
        s.track (0).regions.push_back (r);
    }

    SECTION ("same directory is a no-op")
    {
        const auto res = SessionSerializer::consolidateInto (s, dirA);
        REQUIRE (res.ok);
        REQUIRE (res.filesCopied == 0);
        REQUIRE (s.track (0).regions[0].file == take);
    }

    SECTION ("external mastering source stays absolute and is not copied")
    {
        const auto extMix = makeFakeWav (extDir.getChildFile ("master-source.wav"));
        s.mastering().sourceFile = extMix;

        const auto res = SessionSerializer::consolidateInto (s, dirB);
        REQUIRE (res.ok);
        REQUIRE (s.mastering().sourceFile == extMix);
        REQUIRE (! dirB.getChildFile ("audio/master-source.wav").existsAsFile());
    }

    SECTION ("missing source is skipped and reported, ref kept")
    {
        AudioRegion gone;
        gone.file            = dirA.getChildFile ("audio/deleted.wav");
        gone.lengthInSamples = 100;
        s.track (1).regions.push_back (gone);

        const auto res = SessionSerializer::consolidateInto (s, dirB);
        REQUIRE (res.ok);
        REQUIRE (res.filesCopied == 1);
        REQUIRE (res.missingSources.size() == 1);
        REQUIRE (s.track (1).regions[0].file == dirA.getChildFile ("audio/deleted.wav"));
    }

    SECTION ("copy failure leaves the model untouched")
    {
        REQUIRE (dirB.setReadOnly (true));

        const auto res = SessionSerializer::consolidateInto (s, dirB);
        REQUIRE (! res.ok);
        REQUIRE (res.errorMessage.isNotEmpty());
        REQUIRE (s.track (0).regions[0].file == take);
    }
}
