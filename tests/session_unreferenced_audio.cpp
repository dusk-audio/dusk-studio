#include <catch2/catch_test_macros.hpp>

#include "engine/RecordManager.h"
#include "session/Session.h"
#include "session/UnreferencedAudio.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <filesystem>
#include <vector>

#if defined (_WIN32)
 #include <windows.h>
#else
 #include <sys/stat.h>
 #include <unistd.h>
#endif

namespace
{
juce::File makeScratch()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-unreferenced-"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    return dir;
}

juce::File writeWav (const juce::File& dir, const juce::String& name, int bytes)
{
    auto file = dir.getChildFile (name);
    file.replaceWithText (juce::String::repeatedString ("x", bytes));
    return file;
}

bool holds (const duskstudio::UnreferencedAudio& found, const juce::File& file)
{
    const auto path = std::filesystem::u8path (file.getFullPathName().toStdString())
                          .lexically_normal();
    return std::find (found.files.begin(), found.files.end(), path) != found.files.end();
}

#if defined (_WIN32)
bool symlinksUnavailable (const std::error_code& error)
{
    return error == std::errc::operation_not_permitted
        || error == std::errc::permission_denied
        || error == std::errc::operation_not_supported
        || error == std::errc::function_not_supported
        || error == std::error_code (ERROR_PRIVILEGE_NOT_HELD, std::system_category());
}
#endif
} // namespace

// Clean out offers to delete what past record passes left behind: WAVs in the
// session's audio directory that nothing points at any more. Anything a region,
// a track take or the loaded mastering source still names has to survive, and so
// does everything outside that one directory level.
TEST_CASE ("Clean out finds only the audio nothing points at", "[session][cleanout]")
{
    using duskstudio::AudioRegion;
    using duskstudio::AudioTake;
    using duskstudio::Session;

    const auto dir = makeScratch();
    const auto audio = dir.getChildFile ("audio");
    audio.createDirectory();

    Session session;
    session.setSessionDirectory (dir);

    const auto live = writeWav (audio, "take_live.wav", 2048);
    const auto older = writeWav (audio, "take_older.wav", 1024);
    const auto unplayed = writeWav (audio, "take_unplayed.wav", 768);
    const auto mastering = writeWav (audio, "mixdown.wav", 512);
    const auto orphanA = writeWav (audio, "orphan_a.wav", 4096);
    const auto orphanB = writeWav (audio, "orphan_b.wav", 256);
    const auto imported = writeWav (audio, "imported.wav", 640);
    // A freeze render and anything hand-dropped live a level down, which the
    // walk never descends into.
    const auto freezeDir = audio.getChildFile ("freeze");
    freezeDir.createDirectory();
    const auto frozen = writeWav (freezeDir, "freeze_track01.wav", 8192);
    // Only .wav files are candidates.
    const auto notes = audio.getChildFile ("notes.txt");
    notes.replaceWithText ("keep me");

    AudioRegion region;
    region.file = live;
    region.timelineStart = 0;
    region.lengthInSamples = 48000;
    session.track (4).regions.push_back (region);
    AudioTake displaced;
    displaced.id = session.allocateTakeId();
    displaced.file = older;
    session.track (4).takes.push_back (displaced);
    AudioTake pass;
    pass.id = session.allocateTakeId();
    pass.file = unplayed;
    session.track (9).takes.push_back (pass);
    session.mastering().sourceFile = mastering;
    // A reversed region plays its render from takes/; reversing it again plays
    // the file the render came from.
    AudioRegion reversed;
    reversed.file = dir.getChildFile ("takes").getChildFile ("imported-reversed.wav");
    reversed.lengthInSamples = 480;
    reversed.reversedFrom = AudioRegion::ReverseSource { reversed.file, imported, 0, 480, 0 };
    session.track (6).regions.push_back (reversed);

    const auto found = duskstudio::findUnreferencedAudio (session);
    CHECK (found.files.size() == 2);
    CHECK (holds (found, orphanA));
    CHECK (holds (found, orphanB));
    CHECK_FALSE (holds (found, live));
    CHECK_FALSE (holds (found, older));
    CHECK_FALSE (holds (found, unplayed));
    CHECK_FALSE (holds (found, mastering));
    CHECK_FALSE (holds (found, imported));
    CHECK_FALSE (holds (found, frozen));
    CHECK_FALSE (holds (found, notes));
    CHECK (found.totalBytes == orphanA.getSize() + orphanB.getSize());

    dir.deleteRecursively();
}

// With nothing recorded yet there is no audio directory, and Clean out says so
// rather than offering an empty delete.
TEST_CASE ("Clean out reports nothing for a session with no audio directory",
           "[session][cleanout]")
{
    using duskstudio::Session;

    const auto dir = makeScratch();
    Session session;
    session.setSessionDirectory (dir);
    dir.getChildFile ("audio").deleteRecursively();

    const auto found = duskstudio::findUnreferencedAudio (session);
    CHECK (found.files.empty());
    CHECK (found.totalBytes == 0);

    dir.deleteRecursively();
}

TEST_CASE ("Clean out keeps legacy freeze renders in the audio directory",
           "[session][cleanout][freeze]")
{
    const auto dir = makeScratch();
    duskstudio::Session session;
    session.setSessionDirectory (dir);
    const auto audio = session.getAudioDirectory();
    REQUIRE (audio.createDirectory().wasOk());
    const auto frozen = writeWav (audio, "freeze_track01.wav", 512);
    const auto orphan = writeWav (audio, "orphan.wav", 128);
    auto& track = session.track (0);
    track.frozen.store (true, std::memory_order_relaxed);
    track.frozenAudioPath = frozen.getFullPathName();
    track.frozenRegion.file = frozen;
    track.frozenRegion.lengthInSamples = 48000;
    track.frozenRegion.numChannels = 2;

    SECTION ("the complete saved freeze state protects its render") {}
    SECTION ("the persisted path protects its render") { track.frozenRegion = {}; }
    SECTION ("the playback region protects its render") { track.frozenAudioPath.clear(); }

    const auto found = duskstudio::findUnreferencedAudio (session);
    CHECK_FALSE (found.scanFailed);
    CHECK_FALSE (holds (found, frozen));
    CHECK (found.files.size() == 1);
    CHECK (holds (found, orphan));
    CHECK (found.totalBytes == orphan.getSize());
    dir.deleteRecursively();
}

TEST_CASE ("Clean out keeps audio referenced through filesystem aliases",
           "[session][cleanout]")
{
    enum class Alias { symbolicLink, hardLink, caseVariant };
    auto kind = Alias::symbolicLink;
    SECTION ("symbolic links") { kind = Alias::symbolicLink; }
    SECTION ("hard links") { kind = Alias::hardLink; }
    SECTION ("case aliases on a case insensitive filesystem") { kind = Alias::caseVariant; }

    const auto dir = makeScratch();
    duskstudio::Session session;
    session.setSessionDirectory (dir);
    const auto audio = session.getAudioDirectory();
    REQUIRE (audio.createDirectory().wasOk());
    const auto aliases = dir.getChildFile ("aliases");
    REQUIRE (aliases.createDirectory().wasOk());
    const auto live = writeWav (audio, "live.wav", 512);
    const auto take = writeWav (audio, "take.wav", 256);
    const auto mastering = writeWav (audio, "mix.wav", 128);
    const auto orphan = writeWav (audio, "orphan.wav", 64);
    if (kind == Alias::caseVariant)
    {
        const auto upper = live.getSiblingFile (live.getFileName().toUpperCase());
        std::error_code error;
        const bool supported = std::filesystem::equivalent (
            std::filesystem::u8path (live.getFullPathName().toStdString()),
            std::filesystem::u8path (upper.getFullPathName().toStdString()), error);
        REQUIRE ((! error || error == std::errc::no_such_file_or_directory));
        if (! supported)
        {
            WARN ("Case-insensitive alias coverage is unavailable on this case-sensitive filesystem");
            dir.deleteRecursively();
            return;
        }
    }
   #if defined (_WIN32)
    if (kind == Alias::symbolicLink)
    {
        const auto probe = std::filesystem::u8path (
            aliases.getChildFile ("symlink-probe.wav").getFullPathName().toStdString());
        std::error_code error;
        std::filesystem::create_symlink (
            std::filesystem::u8path (live.getFullPathName().toStdString()), probe, error);
        if (symlinksUnavailable (error))
        {
            WARN ("Symbolic-link alias coverage is unavailable: " << error.message());
            dir.deleteRecursively();
            return;
        }
        REQUIRE_FALSE (error);
        REQUIRE (std::filesystem::remove (probe, error));
        REQUIRE_FALSE (error);
    }
   #endif
    const auto aliasOf = [&] (const juce::File& file)
    {
        auto alias = aliases.getChildFile (file.getFileName());
        std::error_code error;
        const auto source = std::filesystem::u8path (file.getFullPathName().toStdString());
        if (kind == Alias::caseVariant)
        {
            alias = file.getSiblingFile (file.getFileName().toUpperCase());
            REQUIRE (std::filesystem::equivalent (
                source, std::filesystem::u8path (alias.getFullPathName().toStdString()), error));
            REQUIRE_FALSE (error);
        }
        else
        {
            const auto target = std::filesystem::u8path (alias.getFullPathName().toStdString());
            if (kind == Alias::symbolicLink)
                std::filesystem::create_symlink (source, target, error);
            else
                std::filesystem::create_hard_link (source, target, error);
            REQUIRE_FALSE (error);
        }
        return alias;
    };

    duskstudio::AudioRegion region;
    region.file = aliasOf (live);
    region.lengthInSamples = 48000;
    session.track (0).regions.push_back (region);
    duskstudio::AudioTake oldTake;
    oldTake.id = session.allocateTakeId();
    oldTake.file = aliasOf (take);
    session.track (1).takes.push_back (oldTake);
    session.mastering().sourceFile = aliasOf (mastering);

    const auto found = duskstudio::findUnreferencedAudio (session);
    CHECK_FALSE (found.scanFailed);
    CHECK_FALSE (holds (found, live));
    CHECK_FALSE (holds (found, take));
    CHECK_FALSE (holds (found, mastering));
    CHECK (found.files.size() == 1);
    CHECK (holds (found, orphan));
    CHECK (found.totalBytes == orphan.getSize());
    dir.deleteRecursively();
}

TEST_CASE ("Clean out offers no files when a reference cannot be inspected",
           "[session][cleanout]")
{
    const auto dir = makeScratch();
    duskstudio::Session session;
    session.setSessionDirectory (dir);
    const auto audio = session.getAudioDirectory();
    REQUIRE (audio.createDirectory().wasOk());
    writeWav (audio, "orphan.wav", 128);
    const auto alias = dir.getChildFile ("loop.wav");
    const auto path = std::filesystem::u8path (alias.getFullPathName().toStdString());
    std::error_code error;
    std::filesystem::create_symlink (path, path, error);
   #if defined (_WIN32)
    if (symlinksUnavailable (error))
    {
        WARN ("Uninspectable-symbolic-link coverage is unavailable: " << error.message());
        dir.deleteRecursively();
        return;
    }
   #endif
    REQUIRE_FALSE (error);
    session.mastering().sourceFile = alias;

    const auto found = duskstudio::findUnreferencedAudio (session);
    CHECK (found.referenceUnreadable);
    CHECK_FALSE (found.scanFailed);
    CHECK (found.files.empty());
    CHECK (found.totalBytes == 0);
    dir.deleteRecursively();
}

// A Cut region lives only on the region clipboard until it is pasted. An
// imported file is no take, so nothing else names it, and deleting it would
// leave the paste pointing at a missing file.
TEST_CASE ("Clean out keeps the audio a region on the clipboard plays",
           "[session][cleanout]")
{
    const auto dir = makeScratch();
    duskstudio::Session session;
    session.setSessionDirectory (dir);
    const auto audio = session.getAudioDirectory();
    REQUIRE (audio.createDirectory().wasOk());
    const auto imported = writeWav (audio, "import_track01.wav", 512);
    const auto reversedSource = writeWav (audio, "import_track02.wav", 256);
    const auto orphan = writeWav (audio, "orphan.wav", 128);

    duskstudio::AudioRegion cut;
    cut.file = imported;
    cut.lengthInSamples = 480;
    SECTION ("the clipboard region plays the file")
    {
        CHECK (holds (duskstudio::findUnreferencedAudio (session), imported));
        const auto found = duskstudio::findUnreferencedAudio (session, &cut);
        CHECK_FALSE (found.scanFailed);
        CHECK_FALSE (holds (found, imported));
        CHECK (holds (found, reversedSource));
        CHECK (holds (found, orphan));
    }
    SECTION ("the clipboard region reverses the file")
    {
        cut.file = dir.getChildFile ("takes").getChildFile ("import_track02-reversed.wav");
        cut.reversedFrom = duskstudio::AudioRegion::ReverseSource { cut.file, reversedSource, 0, 480, 0 };
        const auto found = duskstudio::findUnreferencedAudio (session, &cut);
        CHECK_FALSE (found.scanFailed);
        CHECK_FALSE (holds (found, reversedSource));
        CHECK (holds (found, imported));
        CHECK (holds (found, orphan));
    }
    dir.deleteRecursively();
}

// A take's WAV is in the audio directory from the moment recording starts, but
// no region names it until Stop commits one, so the scan alone offers it for
// deletion. Clean out refuses while the recorder is active, which has to cover
// the whole of that window and end once the region exists.
TEST_CASE ("The recorder holds a take open for as long as its file is unreferenced",
           "[session][cleanout][recordmanager]")
{
    using duskstudio::RecordManager;
    using duskstudio::Session;
    using duskstudio::Track;

    constexpr int kBlock = 256;
    constexpr int kBlocks = 8;
    const auto dir = makeScratch();
    Session session;
    session.setSessionDirectory (dir);
    session.track (0).mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    session.setTrackArmed (0, true);

    RecordManager recorder (session);
    CHECK_FALSE (recorder.isActive());
    REQUIRE (recorder.startRecording (48000.0, 0));
    const std::vector<float> block ((size_t) kBlock, 0.1f);
    for (int i = 0; i < kBlocks; ++i)
        recorder.writeInputBlock (0, block.data(), nullptr, kBlock);

    const auto midTake = duskstudio::findUnreferencedAudio (session);
    REQUIRE (midTake.files.size() == 1);
    const auto take = midTake.files.front();
    CHECK (recorder.isActive());

    recorder.stopRecording (kBlock * kBlocks);
    CHECK_FALSE (recorder.isActive());
    const auto& regions = session.track (0).regions;
    REQUIRE (regions.size() == 1);
    CHECK (std::filesystem::u8path (regions.front().file.getFullPathName().toStdString())
               .lexically_normal() == take);
    CHECK (std::filesystem::exists (take));
    CHECK (duskstudio::findUnreferencedAudio (session).files.empty());

    dir.deleteRecursively();
}

#if ! defined (_WIN32)
// A directory that cannot be read is not an empty one: saying "already clean"
// there would tell the user their session holds no stale takes when nothing
// was actually looked at.
TEST_CASE ("Clean out reports a directory it cannot read", "[session][cleanout]")
{
    using duskstudio::Session;

    if (::geteuid() == 0)
        SKIP ("root reads a directory whatever its permissions say");

    const auto dir = makeScratch();
    const auto audio = dir.getChildFile ("audio");
    audio.createDirectory();
    writeWav (audio, "orphan.wav", 128);

    Session session;
    session.setSessionDirectory (dir);
    REQUIRE (::chmod (audio.getFullPathName().toRawUTF8(), 0) == 0);

    const auto found = duskstudio::findUnreferencedAudio (session);
    CHECK (found.scanFailed);
    CHECK_FALSE (found.referenceUnreadable);
    CHECK (found.files.empty());

    ::chmod (audio.getFullPathName().toRawUTF8(), 0700);
    dir.deleteRecursively();
}
#endif
