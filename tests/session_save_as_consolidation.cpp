#include <catch2/catch_test_macros.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

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

    const auto ext1  = makeFakeWav (ext1Dir.getChildFile ("loop.wav"));
    const auto ext2  = makeFakeWav (ext2Dir.getChildFile ("loop.wav"));
    // Session-local file with the same basename, planned AFTER the externals:
    // the flattened externals claim audio/loop.wav first, so the relative-path
    // branch must suffix too instead of double-mapping the name.
    const auto local = makeFakeWav (dirA.getChildFile ("audio/loop.wav"));
    {
        AudioRegion r1; r1.file = ext1;  r1.lengthInSamples = 100;
        AudioRegion r2; r2.file = ext2;  r2.lengthInSamples = 100;
        AudioRegion r3; r3.file = local; r3.lengthInSamples = 100;
        s.track (0).regions.push_back (r1);
        s.track (0).regions.push_back (r2);
        s.track (0).regions.push_back (r3);
    }

    const auto res = SessionSerializer::consolidateInto (s, dirB);
    REQUIRE (res.ok);
    REQUIRE (res.filesCopied == 3);

    REQUIRE (dirB.getChildFile ("audio/loop.wav").existsAsFile());
    REQUIRE (dirB.getChildFile ("audio/loop_2.wav").existsAsFile());
    REQUIRE (dirB.getChildFile ("audio/loop_3.wav").existsAsFile());
    auto& regs = s.track (0).regions;
    for (auto& r : regs)
        REQUIRE (r.file.isAChildOf (dirB));
    REQUIRE (regs[0].file != regs[1].file);
    REQUIRE (regs[0].file != regs[2].file);
    REQUIRE (regs[1].file != regs[2].file);
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

    SECTION ("the same directory through a link is a no-op and deletes nothing")
    {
        std::error_code ec;
        const auto link = std::filesystem::u8path (dirB.getFullPathName().toStdString()) / "link-to-a";
        std::filesystem::create_directory_symlink (
            std::filesystem::u8path (dirA.getFullPathName().toStdString()), link, ec);
        if (ec)
            SKIP ("this filesystem cannot make a directory link");

        const auto res = SessionSerializer::consolidateInto (s, juce::File (link.u8string()));
        REQUIRE (res.ok);
        REQUIRE (res.filesCopied == 0);
        REQUIRE (take.existsAsFile());
        REQUIRE (take.loadFileAsString() == "fake-wav");
        REQUIRE (s.track (0).regions[0].file == take);
    }

    SECTION ("a file already in the destination is kept and the copy takes a suffix")
    {
        const auto theirs = dirB.getChildFile ("audio/take.wav");
        theirs.getParentDirectory().createDirectory();
        theirs.replaceWithText ("theirs");

        const auto res = SessionSerializer::consolidateInto (s, dirB);
        REQUIRE (res.ok);
        REQUIRE (theirs.loadFileAsString() == "theirs");
        REQUIRE (s.track (0).regions[0].file == dirB.getChildFile ("audio/take_2.wav"));
        REQUIRE (s.track (0).regions[0].file.loadFileAsString() == "fake-wav");
    }

    SECTION ("an existing plugin state folder in the destination refuses and is left alone")
    {
        makeFakeWav (dirA.getChildFile ("state/lv2/slot/blob.ttl"));
        const auto theirs = dirB.getChildFile ("state/lv2/other/blob.ttl");
        theirs.getParentDirectory().createDirectory();
        theirs.replaceWithText ("theirs");

        const auto res = SessionSerializer::consolidateInto (s, dirB);
        REQUIRE_FALSE (res.ok);
        REQUIRE (theirs.loadFileAsString() == "theirs");
        REQUIRE_FALSE (dirB.getChildFile ("state/lv2/slot/blob.ttl").exists());
        REQUIRE_FALSE (dirB.getChildFile ("audio/take.wav").exists());
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
        // A plain file where the copy must create the audio/ subdir makes
        // createDirectory fail on every platform. A read-only flag on the dir
        // can't gate this portably — Windows ignores it for content creation.
        REQUIRE (dirB.getChildFile ("audio").create().wasOk());

        const auto res = SessionSerializer::consolidateInto (s, dirB);
        REQUIRE (! res.ok);
        REQUIRE (res.errorMessage.isNotEmpty());
        REQUIRE (s.track (0).regions[0].file == take);
    }
}

namespace
{
std::vector<std::string> listing (const juce::File& dir)
{
    namespace fs = std::filesystem;
    const auto root = fs::u8path (dir.getFullPathName().toStdString());
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::recursive_directory_iterator it (root, ec), end; ! ec && it != end; it.increment (ec))
        names.push_back (it->path().lexically_relative (root).generic_string());
    std::sort (names.begin(), names.end());
    return names;
}
} // namespace

// A Save As that fails after consolidation (the notepad or session.json write)
// must leave the session exactly where it was: the Save failed alert says so.
TEST_CASE ("revertConsolidation restores every path and removes what the Save As made",
           "[session][serializer][consolidate]")
{
    const auto dirA   = makeTempDir ("dusk-revert-a-");
    const auto parent = makeTempDir ("dusk-revert-b-");
    const auto extDir = makeTempDir ("dusk-revert-x-");
    const struct Cleanup
    {
        juce::File a, b, x;
        ~Cleanup() { a.deleteRecursively(); b.deleteRecursively(); x.deleteRecursively(); }
    } cleanup { dirA, parent, extDir };

    Session s;
    s.setSessionDirectory (dirA);
    const auto take1   = makeFakeWav (dirA.getChildFile ("audio/take1.wav"));
    const auto take0   = makeFakeWav (dirA.getChildFile ("audio/take0.wav"));
    const auto freeze  = makeFakeWav (dirA.getChildFile ("audio/freeze/freeze_track02.wav"));
    const auto mixdown = makeFakeWav (dirA.getChildFile ("mixdown.wav"));
    const auto ext     = makeFakeWav (extDir.getChildFile ("loop.wav"));
    const auto gone    = dirA.getChildFile ("audio/deleted.wav");
    makeFakeWav (dirA.getChildFile ("state/lv2/track01/cur/bank.bin"));
    {
        AudioRegion r;
        r.file = take1;
        r.lengthInSamples = 1000;
        TakeRef prior;
        prior.file = take0;
        prior.lengthInSamples = 500;
        r.previousTakes.push_back (prior);
        s.track (0).regions.push_back (r);
        AudioRegion e;
        e.file = ext;
        e.lengthInSamples = 100;
        s.track (0).regions.push_back (e);
        AudioRegion m;
        m.file = gone;
        m.lengthInSamples = 100;
        s.track (2).regions.push_back (m);
        s.track (1).frozen.store (true);
        s.track (1).frozenAudioPath = freeze.getFullPathName();
        s.track (1).frozenRegion.file = freeze;
        s.track (1).frozenRegion.lengthInSamples = 1000;
        s.mastering().sourceFile = mixdown;
    }
    const auto listingA = listing (dirA);

    const auto failedSaveAs = [&s, dirA] (const juce::File& dirB)
    {
        const auto res = SessionSerializer::consolidateInto (s, dirB);
        REQUIRE (res.ok);
        REQUIRE (res.filesCopied == 5);
        REQUIRE (s.track (0).regions[0].file.isAChildOf (dirB));
        // The rest of a Save As up to a failed session.json write.
        REQUIRE (SessionSerializer::saveNotepad (dirB, "notes"));
        s.setSessionDirectory (dirB);
        makeFakeWav (dirB.getChildFile ("state/lv2/track01/next/state.ttl"));
        dirB.getChildFile ("session.json.tmp").replaceWithText ("{}");

        SessionSerializer::revertConsolidation (s, res);
        s.setSessionDirectory (dirA);
    };
    const auto restored = [&]
    {
        CHECK (s.getSessionDirectory() == dirA);
        CHECK (s.track (0).regions[0].file == take1);
        CHECK (s.track (0).regions[0].previousTakes[0].file == take0);
        CHECK (s.track (0).regions[1].file == ext);
        CHECK (s.track (2).regions[0].file == gone);
        CHECK (s.track (1).frozenAudioPath == freeze.getFullPathName());
        CHECK (s.track (1).frozenRegion.file == freeze);
        CHECK (s.mastering().sourceFile == mixdown);
        CHECK (listing (dirA) == listingA);
    };

    SECTION ("into a folder that did not exist, which is removed")
    {
        const auto dirB = parent.getChildFile ("New Session");
        failedSaveAs (dirB);
        restored();
        CHECK_FALSE (dirB.exists());
    }

    SECTION ("into a folder of someone else's files, which are all that is left")
    {
        const auto dirB = parent.getChildFile ("Shared");
        dirB.getChildFile ("audio").createDirectory();
        dirB.getChildFile ("audio/take1.wav").replaceWithText ("theirs");
        dirB.getChildFile ("readme.txt").replaceWithText ("theirs");
        const auto listingB = listing (dirB);
        failedSaveAs (dirB);
        restored();
        CHECK (listing (dirB) == listingB);
        CHECK (dirB.getChildFile ("audio/take1.wav").loadFileAsString() == "theirs");
    }
}
