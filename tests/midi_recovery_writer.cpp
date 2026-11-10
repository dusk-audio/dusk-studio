#include <catch2/catch_test_macros.hpp>

#include "TestTempDirectory.h"
#include "support/AllocationObserver.h"
#include "foundation/Fs.h"
#include "session/MidiRecovery.h"
#include "session/MidiRecoveryWriter.h"
#include "engine/midi/MidiFileReader.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>

using namespace duskstudio;
namespace recovery = duskstudio::midirecovery;
namespace stdfs = std::filesystem;

namespace
{
using Bytes = std::vector<std::uint8_t>;

constexpr std::int64_t kNovember8th143005 = 1794148205;   // 2026-11-08 14:30:05 UTC

std::chrono::system_clock::time_point november8th()
{
    return std::chrono::system_clock::time_point (std::chrono::seconds (kNovember8th143005));
}

// One pass of a single note at `key`, as a file built for track 7, "Keys".
recovery::PendingFile fileFor (const stdfs::path& sessionDir, int key, recovery::Folder folder)
{
    MidiRegion pass;
    pass.notes.push_back ({ 1, key, 100, 0, 480 });
    return { sessionDir, november8th(), 7, "Keys", recovery::TrackFile::Playing, folder,
             recovery::buildTakeFile (recovery::Timing {}, pass, "Keys") };
}

Bytes contentOf (const stdfs::path& file)
{
    const auto text = dusk::fs::loadFileAsString (file);
    return Bytes (text.begin(), text.end());
}

std::vector<std::string> namesIn (const stdfs::path& folder)
{
    std::vector<std::string> names;
    std::error_code error;
    for (stdfs::directory_iterator it (folder, error), end; ! error && it != end; it.increment (error))
        names.push_back (it->path().filename().u8string());
    std::sort (names.begin(), names.end());
    return names;
}

// What the writer reported, in the order it reported it. Read only after a
// flush or the writer's end, which order the report's writes before the read.
struct Reports
{
    std::vector<recovery::PendingFile> files;
    std::vector<recovery::WriteResult> results;

    recovery::Writer::Report callback()
    {
        return [this] (const recovery::PendingFile& file, const recovery::WriteResult& result)
        {
            files.push_back (file);
            results.push_back (result);
        };
    }
};
} // namespace

TEST_CASE ("The MIDI recovery writer writes each file in the order queued, all of them before flush returns",
           "[midi][recovery]")
{
    const duskstudio::test::TempDirectory temp ("dusk-midi-recovery-writer-order-");
    Reports reports;
    recovery::Writer writer (reports.callback());

    std::vector<Bytes> queued;
    for (int k = 0; k < 12; ++k)
    {
        auto file = fileFor (temp.path(), 60 + k, recovery::Folder::Takes);
        queued.push_back (file.bytes);
        writer.queue (std::move (file));
    }
    writer.flush();

    // Each take of the same second takes the next copy number, so the names,
    // sorted, give the order the files reached the disk in.
    const auto takes = temp.path() / "midi" / "takes";
    std::vector<std::string> names;
    for (int copy = 1; copy <= 12; ++copy)
        names.push_back (recovery::fileName (november8th(), 7, "Keys", recovery::TrackFile::Playing, copy));
    REQUIRE (namesIn (takes) == names);
    REQUIRE (reports.results.size() == names.size());
    for (std::size_t k = 0; k < names.size(); ++k)
    {
        CAPTURE (k);
        CHECK (contentOf (takes / names[k]) == queued[k]);
        CHECK (reports.files[k].bytes == queued[k]);
        CHECK (reports.results[k].failures.empty());
        CHECK (reports.results[k].written == std::vector<stdfs::path> { takes / names[k] });
    }

    // A flush counts only what was queued before it.
    writer.queue (fileFor (temp.path(), 72, recovery::Folder::Takes));
    writer.flush();
    CHECK (namesIn (takes).size() == names.size() + 1);
    CHECK (reports.results.size() == names.size() + 1);
}

TEST_CASE ("The MIDI recovery writer writes what is still queued before it goes", "[midi][recovery]")
{
    const duskstudio::test::TempDirectory temp ("dusk-midi-recovery-writer-drain-");
    Reports reports;
    {
        recovery::Writer writer (reports.callback());
        for (int k = 0; k < 3; ++k)
            writer.queue (fileFor (temp.path(), 60 + k, recovery::Folder::Takes));
    }
    CHECK (namesIn (temp.path() / "midi" / "takes") == std::vector<std::string> {
        "20261108-143005Z_track07_Keys.mid",
        "20261108-143005Z_track07_Keys_002.mid",
        "20261108-143005Z_track07_Keys_003.mid",
    });
    CHECK (reports.results.size() == 3);
}

TEST_CASE ("A MIDI recovery file that cannot be written is reported, and the files after it are still written",
           "[midi][recovery]")
{
    const duskstudio::test::TempDirectory temp ("dusk-midi-recovery-writer-failure-");
    const auto missing = temp.path() / "gone";
    Reports reports;
    recovery::Writer writer (reports.callback());
    writer.queue (fileFor (missing, 60, recovery::Folder::Takes));
    writer.queue (fileFor (temp.path(), 61, recovery::Folder::Tracks));
    writer.flush();

    REQUIRE (reports.results.size() == 2);
    CHECK (reports.files[0].sessionDir == missing);
    CHECK (reports.results[0].written.empty());
    REQUIRE (reports.results[0].failures.size() == 1);
    CHECK (reports.results[0].failures.front().path == missing);
    CHECK_FALSE (stdfs::exists (missing));

    CHECK (reports.results[1].failures.empty());
    CHECK (reports.results[1].written == std::vector<stdfs::path> {
        temp.path() / "midi" / "tracks" / "20261108-143005Z_track07_Keys.mid" });
}

TEST_CASE ("Two identical MIDI track files queued in turn write one file, and two identical take files two",
           "[midi][recovery]")
{
    const duskstudio::test::TempDirectory temp ("dusk-midi-recovery-writer-same-");
    Reports reports;
    recovery::Writer writer (reports.callback());
    for (const auto folder : { recovery::Folder::Tracks, recovery::Folder::Takes })
        for (int k = 0; k < 2; ++k)
            writer.queue (fileFor (temp.path(), 60, folder));
    writer.flush();

    CHECK (namesIn (temp.path() / "midi" / "tracks") == std::vector<std::string> {
        "20261108-143005Z_track07_Keys.mid" });
    CHECK (namesIn (temp.path() / "midi" / "takes") == std::vector<std::string> {
        "20261108-143005Z_track07_Keys.mid",
        "20261108-143005Z_track07_Keys_002.mid",
    });
    REQUIRE (reports.results.size() == 4);
    CHECK (reports.results[1].written.empty());
    CHECK (reports.results[1].failures.empty());
}

TEST_CASE ("A MIDI recovery writer with nothing queued flushes at once", "[midi][recovery]")
{
    recovery::Writer writer;
    writer.flush();
    writer.flush();
    SUCCEED ("flush returned");
}

TEST_CASE ("A MIDI recovery writer whose report throws goes on writing", "[midi][recovery]")
{
    const duskstudio::test::TempDirectory temp ("dusk-midi-recovery-writer-throws-");
    int reported = 0;
    recovery::Writer writer ([&reported] (const recovery::PendingFile&, const recovery::WriteResult&)
    {
        ++reported;
        throw std::runtime_error ("the report failed");
    });
    for (int k = 0; k < 2; ++k)
        writer.queue (fileFor (temp.path(), 60 + k, recovery::Folder::Takes));
    writer.flush();
    CHECK (reported == 2);
    CHECK (namesIn (temp.path() / "midi" / "takes").size() == 2);
}

TEST_CASE ("Live MIDI journals preserve unfinished passes until their final files are durable", "[midi][recovery][journal]")
{
    const duskstudio::test::TempDirectory temp ("dusk-midi-journal-");
    Reports reports;
    recovery::Journal::Config config;
    config.sessionDir = temp.path();
    config.made = november8th();
    config.trackNumber = 7;
    config.name = "Keys";
    config.timing.sampleRate = kMidiTicksPerQuarter;
    config.timing.bpm = 60.0f;
    config.timelineStart = 1000;
    const auto directory = temp.path() / "midi" / "recording";
    const auto noteOffs = [&] (const stdfs::path& path)
    {
        const auto bytes = contentOf (path);
        midi::MidiFileReader reader;
        REQUIRE (reader.readData (bytes.data(), bytes.size()));
        REQUIRE (reader.timeFormat() == kMidiTicksPerQuarter);
        std::vector<std::int64_t> offs;
        for (const auto& track : reader.tracks())
            for (const auto& event : track)
                if (event.isChannelMessage() && (event.status & 0xf0) == 0x80 && event.data1 == 60)
                    offs.push_back (event.tick);
        return offs;
    };

    SECTION ("live checkpoints retain completed loops and only successful finals retire them")
    {
        recovery::Journal journal (reports.callback());
        config.loopLength = 100;
        auto handle = journal.begin (config);
        REQUIRE (handle.id != 0);
        MidiCaptureStream capture;
        capture.configure ({}, 100);
        const auto drain = [&] (const auto& view) { REQUIRE (journal.append (handle, view)); };
        REQUIRE (capture.pushEvent (1, 10, 0x90, 60, 100));
        REQUIRE (capture.pushEvent (1, 11, 0xb0, 64, 127));
        REQUIRE (capture.publishProgress (1, 30, false));
        capture.service (-1, drain);
        journal.flush();
        auto names = namesIn (directory);
        REQUIRE (names.size() == 1);
        const auto first = directory / names.front();
        REQUIRE (noteOffs (first) == std::vector<std::int64_t> { 1030 });

        REQUIRE (capture.publishProgress (1, 100, true));
        REQUIRE (capture.publishProgress (2, 100, true));
        REQUIRE (capture.publishProgress (3, 40, false));
        capture.service (-1, drain);
        journal.flush();
        REQUIRE (namesIn (directory).size() == 3);
        REQUIRE (noteOffs (first) == std::vector<std::int64_t> { 1100 });
        const auto second = directory / "20261108-143005Z_track07_Keys_pass2.recording.mid";
        midi::MidiFileReader secondReader;
        REQUIRE (secondReader.readFile (second));
        bool chasedSustain = false;
        for (const auto& track : secondReader.tracks())
            for (const auto& event : track)
                chasedSustain |= event.tick == 1000 && event.status == 0xb0
                    && event.data1 == 64 && event.data2 == 127;
        REQUIRE (chasedSustain);
        const auto before = contentOf (first);
        REQUIRE (journal.finish (handle, 3, 40));
        journal.flush();

        const auto blocked = temp.path() / "midi" / "takes";
        { std::ofstream file (blocked); file << "foreign file"; }
        REQUIRE (journal.commit (fileFor (temp.path(), 60, recovery::Folder::Takes), handle.id, 1));
        journal.flush();
        REQUIRE (contentOf (first) == before);
        REQUIRE_FALSE (reports.results.back().failures.empty());
        REQUIRE (journal.flush (config.sessionDir));
        REQUIRE (stdfs::remove (blocked));
        REQUIRE (journal.commit (fileFor (temp.path(), 60, recovery::Folder::Takes), handle.id, 1));
        journal.flush();
        REQUIRE_FALSE (stdfs::exists (first));
        REQUIRE (namesIn (directory).size() == 2);
        REQUIRE (namesIn (blocked).size() == 1);
        REQUIRE (reports.results.back().failures.empty());
    }
    SECTION ("a full journal queue reports a gap instead of sustaining a lost release")
    {
        recovery::Journal journal (reports.callback(), 2);
        auto handle = journal.begin (config);
        REQUIRE (handle.id != 0);
        MidiCaptureStream capture (8);
        REQUIRE (capture.pushEvent (1, 0, 0x90, 60, 100));
        REQUIRE (capture.publishProgress (1, 10, false));
        capture.service (-1, [&] (const auto& view) { REQUIRE (journal.append (handle, view)); });
        journal.flush();
        REQUIRE (capture.pushEvent (1, 15, 0x80, 60, 0));
        REQUIRE (capture.pushEvent (1, 16, 0xb0, 1, 100));
        REQUIRE (capture.publishProgress (1, 20, false));
        capture.service (-1, [&] (const auto& view) { REQUIRE_FALSE (journal.append (handle, view)); });
        REQUIRE (capture.publishProgress (1, 30, false));
        capture.service (-1, [&] (const auto& view) { REQUIRE (journal.append (handle, view)); });
        journal.flush();
        const auto names = namesIn (directory);
        REQUIRE (names.size() == 1);
        REQUIRE (noteOffs (directory / names.front()) == std::vector<std::int64_t> { 1010 });
    }
    SECTION ("a rejected final drain retains payload newer than its watermark")
    {
        recovery::Journal journal (reports.callback(), 2);
        auto handle = journal.begin (config);
        MidiCaptureStream capture (8);
        REQUIRE (capture.pushEvent (1, 0, 0x90, 60, 100));
        REQUIRE (capture.publishProgress (1, 10, false));
        capture.service (-1, [&] (const auto& view) { REQUIRE (journal.append (handle, view)); });
        journal.flush();
        REQUIRE (capture.pushEvent (1, 15, 0x80, 60, 0));
        REQUIRE (capture.pushEvent (1, 16, 0xb0, 1, 100));
        REQUIRE (capture.publishProgress (1, 20, false));
        capture.service (-1, [&] (auto view)
        {
            view.attemptedSequence = 1;
            REQUIRE_FALSE (journal.append (handle, view));
        });
        MidiCaptureStream::DrainView stale;
        stale.attemptedSequence = 1;
        REQUIRE (journal.append (handle, stale));
        REQUIRE (journal.finish (handle, 1, 30));
        journal.flush();
        const auto names = namesIn (directory);
        REQUIRE (names.size() == 1);
        REQUIRE (noteOffs (directory / names.front()) == std::vector<std::int64_t> { 1010 });
    }
    SECTION ("failed completed checkpoints retry after Stop and later successful finals")
    {
        REQUIRE (stdfs::create_directory (directory.parent_path()));
        { std::ofstream blocked (directory); blocked << "blocked recording folder"; }
        recovery::Journal journal (reports.callback());
        config.loopLength = 100;
        auto handle = journal.begin (config);
        MidiCaptureStream capture;
        capture.configure ({}, 100);
        REQUIRE (capture.pushEvent (1, 10, 0x90, 60, 100));
        REQUIRE (capture.publishProgress (1, 100, true));
        REQUIRE (capture.publishProgress (2, 100, true));
        capture.service (-1, [&] (const auto& view) { REQUIRE (journal.append (handle, view)); });
        REQUIRE (journal.finish (handle, 2, 100));
        REQUIRE_FALSE (journal.flush (config.sessionDir));
        REQUIRE_FALSE (journal.flush (config.sessionDir / "."));
        const auto takes = directory.parent_path() / "takes";
        { std::ofstream blocked (takes); blocked << "blocked final folder"; }
        REQUIRE (journal.commit (fileFor (temp.path(), 60, recovery::Folder::Takes), handle.id, 1));
        journal.flush();
        REQUIRE (stdfs::remove (takes));
        REQUIRE (journal.commit (fileFor (temp.path(), 60, recovery::Folder::Takes), handle.id, 2));
        journal.flush();
        REQUIRE (stdfs::remove (directory));
        REQUIRE (journal.flush (config.sessionDir));
        REQUIRE (namesIn (directory) == std::vector<std::string> { "20261108-143005Z_track07_Keys_pass1.recording.mid" });
        REQUIRE (noteOffs (directory / namesIn (directory).front()) == std::vector<std::int64_t> { 1100 });
    }
    SECTION ("a checkpoint never claims an existing recording file")
    {
        REQUIRE (stdfs::create_directories (directory));
        const auto foreign = directory / "20261108-143005Z_track07_Keys_pass1.recording.mid";
        { std::ofstream file (foreign); file << "foreign recording"; }
        const auto original = contentOf (foreign);
        recovery::Journal journal (reports.callback());
        auto handle = journal.begin (config);
        REQUIRE (handle.id != 0);
        MidiCaptureStream capture;
        REQUIRE (capture.pushEvent (1, 0, 0x90, 60, 100));
        REQUIRE (capture.publishProgress (1, 10, false));
        capture.service (-1, [&] (const auto& view) { REQUIRE (journal.append (handle, view)); });
        journal.flush();
        REQUIRE (contentOf (foreign) == original);
        REQUIRE (namesIn (directory).size() == 2);
        REQUIRE (noteOffs (directory / "20261108-143005Z_track07_Keys_pass1_2.recording.mid")
                 == std::vector<std::int64_t> { 1010 });
    }
    SECTION ("removing a session does not cause the journal to recreate it")
    {
        config.sessionDir = temp.path() / "removed-session";
        REQUIRE (stdfs::create_directory (config.sessionDir));
        recovery::Journal journal (reports.callback());
        auto handle = journal.begin (config);
        REQUIRE (handle.id != 0);
        REQUIRE (stdfs::remove (config.sessionDir));
        MidiCaptureStream capture;
        REQUIRE (capture.pushEvent (1, 0, 0x90, 60, 100));
        REQUIRE (capture.publishProgress (1, 10, false));
        capture.service (-1, [&] (const auto& view) { REQUIRE (journal.append (handle, view)); });
        journal.flush();
        REQUIRE_FALSE (stdfs::exists (config.sessionDir));
        REQUIRE_FALSE (reports.results.empty());
        REQUIRE_FALSE (reports.results.back().failures.empty());
    }
}

TEST_CASE ("A stalled MIDI journal worker cannot block or allocate in capture submission", "[midi][recovery][journal][ipc]")
{
    const duskstudio::test::TempDirectory temp ("dusk-midi-journal-stall-");
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto released = release.get_future().share();
    bool firstReport = true;
    recovery::Journal journal ([&] (const auto&, const auto&)
    {
        if (firstReport)
        {
            firstReport = false;
            entered.set_value();
            released.wait();
        }
    }, 2);
    struct Unblock
    {
        std::promise<void>& release;
        ~Unblock() { release.set_value(); }
    } unblock { release };
    recovery::Journal::Config config;
    config.sessionDir = temp.path();
    config.made = november8th();
    config.trackNumber = 1;
    config.name = "Keys";
    config.loopLength = 100;
    auto first = journal.begin (config);
    auto second = journal.begin (config);
    REQUIRE (first.id != 0);
    REQUIRE (second.id != 0);
    MidiCaptureStream capture;
    capture.configure ({}, 100);
    REQUIRE (capture.pushEvent (1, 0, 0x90, 60, 100));
    REQUIRE (capture.publishProgress (1, 100, true));
    capture.service (-1, [&] (const auto& view) { REQUIRE (journal.append (first, view)); });
    REQUIRE (enteredFuture.wait_for (std::chrono::seconds (10)) == std::future_status::ready);
    MidiCaptureStream other;
    REQUIRE (other.pushEvent (1, 0, 0x90, 61, 100));
    REQUIRE (other.publishProgress (1, 10, false));
    other.service (-1, [&] (const auto& view)
    {
        bool accepted = false, rejected = false;
        std::size_t allocations = 0;
        {
            duskstudio::test::AllocationObserver observer;
            accepted = journal.append (second, view);
            rejected = ! journal.append (second, view);
            allocations = observer.count();
        }
        CHECK (accepted);
        CHECK (rejected);
        CHECK (allocations == 0);
        MidiCaptureStream::DrainView empty = view;
        empty.first = empty.second = nullptr;
        empty.firstCount = empty.secondCount = 0;
        bool idleAccepted = true;
        for (int i = 0; i < 1000; ++i) idleAccepted &= journal.append (second, empty);
        CHECK (idleAccepted);
    });
}
