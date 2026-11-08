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
    const auto passB   = makeFakeWav (dirA.getChildFile ("audio/pass_b.wav"));
    const auto freeze  = makeFakeWav (dirA.getChildFile ("audio/freeze/freeze_track02.wav"));
    const auto mixdown = makeFakeWav (dirA.getChildFile ("mixdown.wav"));

    {
        AudioRegion r;
        r.file            = take1;
        r.lengthInSamples = 1000;
        AudioTake live;
        live.id              = s.allocateTakeId();
        live.file            = take1;
        live.lengthInSamples = 1000;
        r.takeId = live.id;
        AudioTake unplayed;
        unplayed.id              = s.allocateTakeId();
        unplayed.file            = passB;
        unplayed.lengthInSamples = 800;
        AudioTake older;
        older.id              = s.allocateTakeId();
        older.file            = take0;
        older.lengthInSamples = 500;
        s.track (0).takes = { live, unplayed, older };
        s.track (0).regions.push_back (r);

        s.track (1).frozen.store (true);
        s.track (1).frozenAudioPath        = freeze.getFullPathName();
        s.track (1).frozenRegion.file      = freeze;
        s.track (1).frozenRegion.lengthInSamples = 1000;

        s.mastering().sourceFile = mixdown;
    }

    const auto res = SessionSerializer::consolidateInto (s, dirB);
    REQUIRE (res.ok);
    REQUIRE (res.filesCopied == 5);
    REQUIRE (res.missingSources.empty());

    // Relative subpaths preserved, including audio/freeze/ and the root mixdown.
    REQUIRE (dirB.getChildFile ("audio/take1.wav").existsAsFile());
    REQUIRE (dirB.getChildFile ("audio/take0.wav").existsAsFile());
    REQUIRE (dirB.getChildFile ("audio/pass_b.wav").existsAsFile());
    REQUIRE (dirB.getChildFile ("audio/freeze/freeze_track02.wav").existsAsFile());
    REQUIRE (dirB.getChildFile ("mixdown.wav").existsAsFile());

    // Model repointed into dirB.
    REQUIRE (s.track (0).regions[0].file == dirB.getChildFile ("audio/take1.wav"));
    REQUIRE (s.track (0).takes[0].file == dirB.getChildFile ("audio/take1.wav"));
    REQUIRE (s.track (0).takes[1].file == dirB.getChildFile ("audio/pass_b.wav"));
    REQUIRE (s.track (0).takes[2].file == dirB.getChildFile ("audio/take0.wav"));
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
    REQUIRE (loaded.track (0).takes.size() == 3);
    REQUIRE (loaded.track (0).takes[1].file == dirB.getChildFile ("audio/pass_b.wav"));
    REQUIRE (loaded.track (0).takes[2].file == dirB.getChildFile ("audio/take0.wav"));
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

    SECTION ("a plugin state folder and MIDI files the session does not own stay behind")
    {
        makeFakeWav (dirA.getChildFile ("state/lv2/slot/blob.ttl"));

        makeFakeWav (dirA.getChildFile ("midi/takes/20261108-143005Z_track01_Keys.mid"));

        const auto res = SessionSerializer::consolidateInto (s, dirB, /*ownsOldFolder*/ false);
        REQUIRE (res.ok);
        REQUIRE_FALSE (dirB.getChildFile ("state").exists());
        REQUIRE (dirA.getChildFile ("state/lv2/slot/blob.ttl").existsAsFile());
        REQUIRE_FALSE (dirB.getChildFile ("midi").exists());
        REQUIRE (res.copiedMidiFiles.empty());
        REQUIRE (dirA.getChildFile ("midi/takes/20261108-143005Z_track01_Keys.mid").existsAsFile());
        REQUIRE (s.track (0).regions[0].file == dirB.getChildFile ("audio/take.wav"));
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
        AudioTake earlier;
        earlier.id = s.allocateTakeId();
        earlier.file = take0;
        earlier.lengthInSamples = 500;
        s.track (0).takes.push_back (earlier);
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
        CHECK (s.track (0).takes[0].file == take0);
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

// A reversed region remembers the render it plays and the file it reversed. Save As
// takes both along and repoints both, so reversing the region again in the new
// folder still finds its source rather than rendering the render in reverse.
TEST_CASE ("consolidateInto carries what a reversed region reversed",
           "[session][serializer][consolidate][reverse]")
{
    const auto dirA = makeTempDir ("dusk-consolidate-rev-a-");
    const auto dirB = makeTempDir ("dusk-consolidate-rev-b-");
    const struct Cleanup
    {
        juce::File a, b;
        ~Cleanup() { a.deleteRecursively(); b.deleteRecursively(); }
    } cleanup { dirA, dirB };

    Session s;
    s.setSessionDirectory (dirA);
    const auto source = makeFakeWav (dirA.getChildFile ("audio/imported.wav"));
    const auto render = makeFakeWav (dirA.getChildFile ("takes/imported-reversed.wav"));
    AudioRegion r;
    r.file = render;
    r.lengthInSamples = 1000;
    r.reversedFrom = AudioRegion::ReverseSource { render, source, 0, 1000, 0 };
    s.track (0).regions.push_back (r);

    const auto res = SessionSerializer::consolidateInto (s, dirB);
    REQUIRE (res.ok);
    CHECK (res.filesCopied == 2);
    const auto& moved = s.track (0).regions[0];
    REQUIRE (moved.reversedFrom.has_value());
    CHECK (moved.file == dirB.getChildFile ("takes/imported-reversed.wav"));
    CHECK (moved.reversedFrom->render == moved.file);
    CHECK (moved.reversedFrom->file == dirB.getChildFile ("audio/imported.wav"));
    CHECK (dirB.getChildFile ("audio/imported.wav").existsAsFile());
    CHECK (forwardOfReversed (moved).has_value());

    SessionSerializer::revertConsolidation (s, res);
    CHECK (s.track (0).regions[0].reversedFrom->file == source);
    CHECK (s.track (0).regions[0].reversedFrom->render == render);
}

namespace
{
std::filesystem::path pathOf (const juce::File& file)
{
    return std::filesystem::u8path (file.getFullPathName().toStdString());
}

std::vector<std::filesystem::path> sourcesOf (const SessionSerializer::ConsolidationResult& res)
{
    std::vector<std::filesystem::path> sources;
    for (const auto& file : res.copiedMidiFiles)
        sources.push_back (file.source);
    return sources;
}
} // namespace

// A session's MIDI recovery files are its own, like its plug-in state, so Save
// As takes midi/ along when the session owns the folder it leaves.
TEST_CASE ("consolidateInto carries the midi folder of a session that owns its old folder",
           "[session][serializer][consolidate][midi]")
{
    namespace fs = std::filesystem;
    const auto dirA   = makeTempDir ("dusk-consolidate-midi-a-");
    const auto parent = makeTempDir ("dusk-consolidate-midi-b-");
    const struct Cleanup
    {
        juce::File a, b;
        ~Cleanup() { a.deleteRecursively(); b.deleteRecursively(); }
    } cleanup { dirA, parent };

    Session s;
    s.setSessionDirectory (dirA);
    const auto take = makeFakeWav (dirA.getChildFile ("audio/take.wav"));
    {
        AudioRegion r;
        r.file = take;
        r.lengthInSamples = 100;
        s.track (0).regions.push_back (r);
    }
    const std::vector<std::string> midiFiles {
        "midi/takes/20261108-143005Z_track01_Keys.mid",
        "midi/takes/20261108-143005Z_track01_Keys_002.mid",
        "midi/tracks/20261108-143010Z_track01_Keys.mid",
        "midi/tracks/20261108-143010Z_track01_Keys.takes.mid",
        "midi/kept by hand/riff.mid",
    };
    for (const auto& name : midiFiles)
    {
        const auto file = dirA.getChildFile (name);
        file.getParentDirectory().createDirectory();
        file.replaceWithText (name);
    }
    const auto listingA = listing (dirA);

    SECTION ("every regular file is copied to the same place, and the copies and their sources are reported")
    {
        const auto dirB = parent.getChildFile ("New Session");
        const auto res = SessionSerializer::consolidateInto (s, dirB);
        REQUIRE (res.ok);
        CHECK (res.filesCopied == 6);
        std::vector<fs::path> sources, copies;
        for (const auto& name : midiFiles)
        {
            CAPTURE (name);
            CHECK (dirB.getChildFile (name).loadFileAsString() == juce::String (name));
            sources.push_back (pathOf (dirA.getChildFile (name)));
            copies.push_back (pathOf (dirB.getChildFile (name)));
        }
        auto reported = sourcesOf (res);
        std::sort (reported.begin(), reported.end());
        std::sort (sources.begin(), sources.end());
        CHECK (reported == sources);
        for (const auto& [source, copy] : res.copiedMidiFiles)
            CHECK (copy == pathOf (dirB) / source.lexically_relative (pathOf (dirA)));
        for (const auto& copy : copies)
            CHECK (std::find (res.copiedFiles.begin(), res.copiedFiles.end(), copy) != res.copiedFiles.end());
        CHECK (listing (dirA) == listingA);

        SECTION ("and a failed save takes them all back")
        {
            SessionSerializer::revertConsolidation (s, res);
            CHECK_FALSE (dirB.exists());
            CHECK (listing (dirA) == listingA);
        }

        SECTION ("and a first save then removes exactly those sources and the folders they leave empty")
        {
            dirA.getChildFile ("midi/tracks/notes.txt").replaceWithText ("not a copy");
            SessionSerializer::removeCopiedMidiSources (res, pathOf (dirA));
            CHECK (listing (dirA) == std::vector<std::string> {
                "audio", "audio/take.wav", "midi", "midi/kept by hand", "midi/tracks", "midi/tracks/notes.txt" });

            dirA.getChildFile ("midi/tracks/notes.txt").deleteFile();
            dirA.getChildFile ("midi/kept by hand").deleteRecursively();
            SessionSerializer::removeCopiedMidiSources (res, pathOf (dirA));
            CHECK (listing (dirA) == std::vector<std::string> { "audio", "audio/take.wav" });
        }

        SECTION ("and a first save keeps a source whose copy cannot reach the disk")
        {
            dirB.getChildFile (midiFiles[1]).deleteFile();
            SessionSerializer::removeCopiedMidiSources (res, pathOf (dirA));
            CHECK (listing (dirA) == std::vector<std::string> {
                "audio", "audio/take.wav", "midi", "midi/kept by hand", "midi/takes",
                "midi/takes/20261108-143005Z_track01_Keys_002.mid" });
            CHECK (dirA.getChildFile (midiFiles[1]).loadFileAsString() == juce::String (midiFiles[1]));
        }
    }

    SECTION ("a link in it is not a regular file and stays behind")
    {
        std::error_code ec;
        fs::create_symlink (pathOf (dirA.getChildFile (midiFiles[0])), pathOf (dirA.getChildFile ("midi/takes/link.mid")),
                            ec);
        if (ec) SKIP ("this filesystem cannot make a link");
        const auto dirB = parent.getChildFile ("Linked");
        const auto res = SessionSerializer::consolidateInto (s, dirB);
        REQUIRE (res.ok);
        CHECK (dirB.getChildFile (midiFiles[0]).existsAsFile());
        CHECK (fs::symlink_status (pathOf (dirB.getChildFile ("midi/takes/link.mid")), ec).type()
               == fs::file_type::not_found);
        CHECK (res.copiedMidiFiles.size() == midiFiles.size());
    }

    SECTION ("a file already in the destination is never written over, and a failed save leaves it")
    {
        const auto dirB = parent.getChildFile ("Shared");
        const auto theirs = dirB.getChildFile (midiFiles[2]);
        theirs.getParentDirectory().createDirectory();
        theirs.replaceWithText ("theirs");
        const auto listingB = listing (dirB);

        const auto res = SessionSerializer::consolidateInto (s, dirB);
        REQUIRE (res.ok);
        CHECK (theirs.loadFileAsString() == "theirs");
        const auto sources = sourcesOf (res);
        CHECK (std::find (sources.begin(), sources.end(), pathOf (dirA.getChildFile (midiFiles[2]))) == sources.end());
        CHECK (sources.size() == midiFiles.size() - 1);
        CHECK (dirB.getChildFile (midiFiles[3]).existsAsFile());

        SessionSerializer::revertConsolidation (s, res);
        CHECK (listing (dirB) == listingB);
        CHECK (theirs.loadFileAsString() == "theirs");
    }

    SECTION ("a copy that fails fails the Save As and takes back what it copied")
    {
        const auto dirB = parent.getChildFile ("Blocked");
        dirB.createDirectory();
        dirB.getChildFile ("midi").replaceWithText ("not a folder");
        const auto listingB = listing (dirB);

        const auto res = SessionSerializer::consolidateInto (s, dirB);
        REQUIRE_FALSE (res.ok);
        CHECK (res.errorMessage.startsWith ("Could not copy \""));
        CHECK (res.errorMessage.contains (dirB.getChildFile ("midi").getFullPathName()));
        CHECK (s.track (0).regions[0].file == take);
        CHECK (listing (dirB) == listingB);
        CHECK (listing (dirA) == listingA);
    }
}

// A session's backups are its own too, so Save As takes backups/ along when the
// session owns the folder it leaves. They are not MIDI recovery files, so a
// first save never moves them out of the old folder.
TEST_CASE ("consolidateInto carries the backups folder of a session that owns its old folder",
           "[session][serializer][consolidate][backups]")
{
    const auto dirA   = makeTempDir ("dusk-consolidate-backups-a-");
    const auto parent = makeTempDir ("dusk-consolidate-backups-b-");
    const struct Cleanup
    {
        juce::File a, b;
        ~Cleanup() { a.deleteRecursively(); b.deleteRecursively(); }
    } cleanup { dirA, parent };

    Session s;
    s.setSessionDirectory (dirA);
    const std::vector<std::string> backups {
        "backups/20261107-091500Z_session.json",
        "backups/20261108-143005Z_session.json",
        "backups/20261108-143005Z_session_002.json",
    };
    for (const auto& name : backups)
    {
        const auto file = dirA.getChildFile (name);
        file.getParentDirectory().createDirectory();
        file.replaceWithText (name);
    }
    const auto listingA = listing (dirA);

    SECTION ("each backup is copied to the same place and is not reported as a MIDI source")
    {
        const auto dirB = parent.getChildFile ("New Session");
        const auto res = SessionSerializer::consolidateInto (s, dirB);
        REQUIRE (res.ok);
        CHECK (res.filesCopied == (int) backups.size());
        for (const auto& name : backups)
        {
            CAPTURE (name);
            CHECK (dirB.getChildFile (name).loadFileAsString() == juce::String (name));
        }
        CHECK (res.copiedMidiFiles.empty());
        CHECK (listing (dirA) == listingA);

        SECTION ("and a failed save removes the backups folder it made")
        {
            SessionSerializer::revertConsolidation (s, res);
            CHECK_FALSE (dirB.exists());
            CHECK (listing (dirA) == listingA);
        }

        SECTION ("and a first save leaves the old folder's backups where they are")
        {
            SessionSerializer::removeCopiedMidiSources (res, pathOf (dirA));
            CHECK (listing (dirA) == listingA);
        }
    }

    SECTION ("a session that does not own its old folder leaves its backups behind")
    {
        const auto dirB = parent.getChildFile ("Not Owned");
        const auto res = SessionSerializer::consolidateInto (s, dirB, /*ownsOldFolder*/ false);
        REQUIRE (res.ok);
        CHECK_FALSE (dirB.getChildFile ("backups").exists());
        CHECK (listing (dirA) == listingA);
    }

    SECTION ("a backup already in the destination is never written over, and a failed save leaves it")
    {
        const auto dirB = parent.getChildFile ("Shared");
        const auto theirs = dirB.getChildFile (backups[1]);
        theirs.getParentDirectory().createDirectory();
        theirs.replaceWithText ("theirs");
        const auto listingB = listing (dirB);

        const auto res = SessionSerializer::consolidateInto (s, dirB);
        REQUIRE (res.ok);
        CHECK (theirs.loadFileAsString() == "theirs");
        CHECK (dirB.getChildFile (backups[0]).loadFileAsString() == juce::String (backups[0]));
        CHECK (dirB.getChildFile (backups[2]).loadFileAsString() == juce::String (backups[2]));

        SessionSerializer::revertConsolidation (s, res);
        CHECK (listing (dirB) == listingB);
        CHECK (theirs.loadFileAsString() == "theirs");
    }
}

// The new folder can be inside the old one's midi/ or backups/. By the time
// those trees are walked, the save has copied the session's audio and plug-in
// state into it, and none of that may be taken for an old MIDI file: a first
// save would delete it as a moved source.
TEST_CASE ("consolidateInto leaves a new folder inside the old midi or backups folder out of the walk",
           "[session][serializer][consolidate][midi]")
{
    const auto dirA = makeTempDir ("dusk-consolidate-nested-");
    const struct Cleanup
    {
        juce::File a;
        ~Cleanup() { a.deleteRecursively(); }
    } cleanup { dirA };

    Session s;
    s.setSessionDirectory (dirA);
    const auto take = makeFakeWav (dirA.getChildFile ("audio/take.wav"));
    {
        AudioRegion r;
        r.file = take;
        r.lengthInSamples = 100;
        s.track (0).regions.push_back (r);
    }
    dirA.getChildFile ("state/lv2/slot/state.ttl").getParentDirectory().createDirectory();
    dirA.getChildFile ("state/lv2/slot/state.ttl").replaceWithText ("state");
    const auto midiFile = dirA.getChildFile ("midi/takes/20261108-143005Z_track01_Keys.mid");
    midiFile.getParentDirectory().createDirectory();
    midiFile.replaceWithText ("midi");
    dirA.getChildFile ("backups/20261108-143005Z_session.json").getParentDirectory().createDirectory();
    dirA.getChildFile ("backups/20261108-143005Z_session.json").replaceWithText ("{}");

    for (const char* inside : { "midi/takes/Inside", "backups/Inside" })
    {
        DYNAMIC_SECTION ("Save As into " << inside)
        {
            const auto dirB = dirA.getChildFile (inside);
            const auto res = SessionSerializer::consolidateInto (s, dirB);
            REQUIRE (res.ok);
            CHECK (dirB.getChildFile ("audio/take.wav").loadFileAsString() == "fake-wav");
            CHECK (dirB.getChildFile ("state/lv2/slot/state.ttl").loadFileAsString() == "state");
            CHECK (dirB.getChildFile ("midi/takes/20261108-143005Z_track01_Keys.mid").loadFileAsString() == "midi");
            CHECK (dirB.getChildFile ("backups/20261108-143005Z_session.json").loadFileAsString() == "{}");
            CHECK (sourcesOf (res) == std::vector<std::filesystem::path> { pathOf (midiFile) });
            CHECK_FALSE (dirB.getChildFile ("midi/takes/Inside").exists());
            CHECK_FALSE (dirB.getChildFile ("backups/Inside").exists());

            SessionSerializer::removeCopiedMidiSources (res, pathOf (dirA));
            CHECK (dirB.getChildFile ("audio/take.wav").loadFileAsString() == "fake-wav");
            CHECK (dirB.getChildFile ("state/lv2/slot/state.ttl").loadFileAsString() == "state");
            CHECK (dirB.getChildFile ("midi/takes/20261108-143005Z_track01_Keys.mid").loadFileAsString() == "midi");
            CHECK_FALSE (midiFile.exists());
        }
    }
}
