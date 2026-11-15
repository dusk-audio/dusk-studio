#include <catch2/catch_test_macros.hpp>

#include "TestTempDirectory.h"
#include "support/AllocationObserver.h"
#include "foundation/Fs.h"
#include "session/MidiRecovery.h"
#include "session/MidiRecoveryWriter.h"
#include "engine/midi/MidiFileReader.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
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

// Polls rather than waiting on a timer: CI's ThreadSanitizer misreads timed
// condition-variable waits as a double lock.
template <typename Done>
bool eventually (Done done, std::chrono::milliseconds budget = std::chrono::seconds (10))
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (! done())
    {
        if (std::chrono::steady_clock::now() > deadline) return done();
        std::this_thread::yield();
    }
    return true;
}

std::vector<int> noteOnKeysIn (const stdfs::path& file)
{
    std::vector<int> keys;
    midi::MidiFileReader reader;
    if (! reader.readFile (file)) return keys;
    for (const auto& track : reader.tracks())
        for (const auto& event : track)
            if (event.isNoteOn()) keys.push_back (event.noteNumber());
    return keys;
}

// One 10 ms service tick of a recorder at 48 kHz: four 120-sample audio
// blocks, each publishing its progress, with a note-on in the first and its
// note-off in the third.
void playTick (MidiCaptureStream& capture, std::int64_t tick, int key)
{
    const auto start = tick * 480;
    for (int block = 0; block < 4; ++block)
    {
        const auto at = start + block * 120;
        if (block == 0) REQUIRE (capture.pushEvent (1, at, 0x90, (std::uint8_t) key, 100));
        if (block == 2) REQUIRE (capture.pushEvent (1, at, 0x80, (std::uint8_t) key, 0));
        REQUIRE (capture.publishProgress (1, at + 120, false));
    }
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
        // A take still recording into the folder keeps it from being ready.
        REQUIRE_FALSE (journal.flush (config.sessionDir));
        REQUIRE_FALSE (journal.flush (config.sessionDir / "."));
        REQUIRE (journal.finish (handle, 2, 100));
        // Passes whose checkpoints could not be written do not: the session
        // being saved holds them, and they go on retrying where they were.
        REQUIRE (journal.flush (config.sessionDir));
        REQUIRE (journal.flush (config.sessionDir / "."));
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
    SECTION ("a pass holding nothing but controllers chased from earlier passes gets no checkpoint")
    {
        recovery::Journal journal (reports.callback());
        config.loopLength = 100;
        auto handle = journal.begin (config);
        MidiCaptureStream capture;
        capture.configure ({}, 100);
        REQUIRE (capture.pushEvent (1, 10, 0xb0, 64, 127));
        REQUIRE (capture.publishProgress (1, 100, true));
        REQUIRE (capture.publishProgress (2, 100, true));
        REQUIRE (capture.publishProgress (3, 40, false));
        capture.service (-1, [&] (const auto& view) { REQUIRE (journal.append (handle, view)); });
        journal.checkpoint();
        journal.flush();
        CHECK (namesIn (directory) == std::vector<std::string> { "20261108-143005Z_track07_Keys_pass1.recording.mid" });
        REQUIRE (journal.finish (handle, 3, 40));
        journal.flush();
        CHECK (namesIn (directory).size() == 1);
        // Stop gives a final file only to the pass with a controller of its own.
        REQUIRE (journal.commit (fileFor (temp.path(), 60, recovery::Folder::Takes), handle.id, 1));
        journal.flush();
        CHECK (namesIn (directory).empty());
    }
    SECTION ("a checkpoint gives up once every name it may take is taken")
    {
        REQUIRE (stdfs::create_directories (directory));
        const std::string stem = "20261108-143005Z_track07_Keys_pass1";
        for (int copy = 1; copy <= 999; ++copy)
            std::ofstream (directory / (stem + (copy > 1 ? "_" + std::to_string (copy) : std::string()) + ".recording.mid"))
                << "foreign recording";
        recovery::Journal journal (reports.callback());
        auto handle = journal.begin (config);
        MidiCaptureStream capture;
        REQUIRE (capture.pushEvent (1, 0, 0x90, 60, 100));
        REQUIRE (capture.publishProgress (1, 10, false));
        capture.service (-1, [&] (const auto& view) { REQUIRE (journal.append (handle, view)); });
        journal.flush();
        CHECK (namesIn (directory).size() == 999);
        REQUIRE (reports.results.size() == 1);
        REQUIRE (reports.results.front().failures.size() == 1);
        CHECK (reports.results.front().failures.front().error == std::errc::file_exists);
    }
    SECTION ("a checkpoint that keeps failing is reported once, however often it is retried")
    {
        REQUIRE (stdfs::create_directory (directory.parent_path()));
        { std::ofstream blocked (directory); blocked << "blocked recording folder"; }
        recovery::Journal journal (reports.callback());
        auto handle = journal.begin (config);
        MidiCaptureStream capture;
        REQUIRE (capture.pushEvent (1, 0, 0x90, 60, 100));
        REQUIRE (capture.publishProgress (1, 10, false));
        capture.service (-1, [&] (const auto& view) { REQUIRE (journal.append (handle, view)); });
        for (int round = 0; round < 5; ++round)
        {
            journal.checkpoint();
            journal.flush();
        }
        CHECK (reports.results.size() == 1);
        REQUIRE (stdfs::remove (directory));
        journal.flush();
        CHECK (namesIn (directory).size() == 1);
        REQUIRE (reports.results.size() == 2);
        CHECK (reports.results.back().failures.empty());
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

TEST_CASE ("A live MIDI checkpoint is rewritten only when its notes or controllers change", "[midi][recovery][journal]")
{
    const duskstudio::test::TempDirectory temp ("dusk-midi-journal-quiet-");
    Reports reports;
    recovery::Journal journal (reports.callback());
    recovery::Journal::Config config;
    config.sessionDir = temp.path();
    config.made = november8th();
    config.trackNumber = 7;
    config.name = "Keys";
    config.timing.sampleRate = kMidiTicksPerQuarter;
    config.timing.bpm = 60.0f;
    auto handle = journal.begin (config);
    REQUIRE (handle.id != 0);
    MidiCaptureStream capture;
    std::int64_t position = 0;
    int ticks = 0;
    bool accepted = true;
    // Service ticks of four audio blocks each, nothing played in them, and a
    // checkpoint request every hundred ticks as the recorder makes them.
    const auto quiet = [&] (int count)
    {
        for (int i = 0; i < count; ++i)
        {
            for (int block = 0; block < 4; ++block)
                REQUIRE (capture.publishProgress (1, position += 10, false));
            capture.service (-1, [&] (const auto& view) { accepted &= journal.append (handle, view); });
            if (++ticks % 100 == 0) journal.checkpoint();
        }
    };
    const auto checkpointWrites = [&]
    {
        return std::count_if (reports.results.begin(), reports.results.end(), [] (const recovery::WriteResult& result)
        {
            return ! result.written.empty() && result.failures.empty();
        });
    };
    const auto directory = temp.path() / "midi" / "recording";

    SECTION ("a held note")
    {
        REQUIRE (capture.pushEvent (1, 0, 0x90, 60, 100));
        quiet (1);
        journal.checkpoint();
        journal.flush();
        REQUIRE (checkpointWrites() == 1);
        const auto names = namesIn (directory);
        REQUIRE (names.size() == 1);
        const auto path = directory / names.front();
        const auto first = contentOf (path);

        quiet (500);
        journal.flush();
        CHECK (accepted);
        CHECK (checkpointWrites() == 1);
        CHECK (contentOf (path) == first);

        REQUIRE (capture.pushEvent (1, position, 0xb0, 1, 64));
        quiet (1);
        journal.checkpoint();
        journal.flush();
        CHECK (checkpointWrites() == 2);
        midi::MidiFileReader reader;
        REQUIRE (reader.readFile (path));
        bool modulation = false;
        for (const auto& track : reader.tracks())
            for (const auto& event : track)
                modulation |= event.status == 0xb0 && event.data1 == 1 && event.data2 == 64;
        CHECK (modulation);

        // Stop closes the note at the end of the take.
        REQUIRE (journal.finish (handle, 1, position));
        journal.flush();
        CHECK (checkpointWrites() == 3);
        REQUIRE (reader.readFile (path));
        std::vector<std::int64_t> offs;
        for (const auto& track : reader.tracks())
            for (const auto& event : track)
                if (event.isChannelMessage() && (event.status & 0xf0) == 0x80 && event.data1 == 60)
                    offs.push_back (event.tick);
        CHECK (offs == std::vector<std::int64_t> { position });
    }
    SECTION ("nothing played")
    {
        quiet (500);
        REQUIRE (journal.finish (handle, 1, position));
        journal.flush();
        CHECK (accepted);
        CHECK (reports.results.empty());
        CHECK (namesIn (directory).empty());
    }
}

// The recorder's cadence, 100 service ticks and one checkpoint request a
// second, kept up for three seconds by two tracks with a note in every tick,
// while the worker is held in one slow write: the queue has to take all of it.
TEST_CASE ("A MIDI journal worker held in one write through seconds of dense capture drops nothing", "[midi][recovery][journal][ipc]")
{
    const duskstudio::test::TempDirectory temp ("dusk-midi-journal-dense-");
    std::atomic<bool> stalled { false }, release { false };
    Reports reports;
    recovery::Journal journal ([&] (const recovery::PendingFile& file, const recovery::WriteResult& result)
    {
        reports.files.push_back (file);
        reports.results.push_back (result);
        if (! stalled.exchange (true))
            while (! release.load()) std::this_thread::yield();
    });
    struct Unstall
    {
        std::atomic<bool>& release;
        ~Unstall() { release.store (true); }
    } unstall { release };

    recovery::Journal::Config config;
    config.sessionDir = temp.path();
    config.made = november8th();
    config.name = "Keys";
    config.trackNumber = 3;
    config.loopLength = 100;
    auto stall = journal.begin (config);
    config.loopLength = 0;
    config.trackNumber = 1;
    auto first = journal.begin (config);
    config.trackNumber = 2;
    auto second = journal.begin (config);
    REQUIRE (stall.id != 0);
    REQUIRE (first.id != 0);
    REQUIRE (second.id != 0);

    // A loop pass that ends at once takes the worker into its first write.
    MidiCaptureStream loop;
    loop.configure ({}, 100);
    REQUIRE (loop.pushEvent (1, 0, 0x90, 60, 100));
    REQUIRE (loop.publishProgress (1, 100, true));
    loop.service (-1, [&] (const auto& view) { REQUIRE (journal.append (stall, view)); });
    REQUIRE (eventually ([&] { return stalled.load(); }));

    static constexpr int kTicks = 300;
    std::array<MidiCaptureStream, 2> captures;
    const std::array<recovery::Journal::Handle*, 2> handles { &first, &second };
    bool accepted = true;
    for (int tick = 0; tick < kTicks; ++tick)
    {
        for (std::size_t track = 0; track < captures.size(); ++track)
        {
            playTick (captures[track], tick, 36 + (int) track * 24 + tick % 24);
            captures[track].service (-1, [&] (const auto& view) { accepted &= journal.append (*handles[track], view); });
        }
        if (tick % 100 == 99) journal.checkpoint();
    }
    release.store (true);
    CHECK (accepted);

    const auto directory = temp.path() / "midi" / "recording";
    const auto complete = [&] (const char* name, int track)
    {
        const auto keys = noteOnKeysIn (directory / name);
        REQUIRE (keys.size() == (std::size_t) kTicks);
        for (int tick = 0; tick < kTicks; ++tick)
            if (keys[(std::size_t) tick] != 36 + track * 24 + tick % 24) return false;
        return true;
    };
    journal.flush();
    for (const auto& result : reports.results) CHECK (result.failures.empty());
    CHECK (complete ("20261108-143005Z_track01_Keys_pass1.recording.mid", 0));
    CHECK (complete ("20261108-143005Z_track02_Keys_pass1.recording.mid", 1));

    REQUIRE (journal.finish (first, 1, kTicks * 480));
    REQUIRE (journal.finish (second, 1, kTicks * 480));
    journal.flush();
    for (const auto& result : reports.results) CHECK (result.failures.empty());
    CHECK (complete ("20261108-143005Z_track01_Keys_pass1.recording.mid", 0));
    CHECK (complete ("20261108-143005Z_track02_Keys_pass1.recording.mid", 1));
}

// The hook runs on the worker each time it has found nothing to do, just
// before it sleeps. A drain appended there has to wake it by itself: nothing
// else is sent. The half-second bound also fails a worker that would only come
// back on a timer.
TEST_CASE ("A MIDI journal drain appended as its worker goes to sleep still wakes it", "[midi][recovery][journal][ipc]")
{
    const duskstudio::test::TempDirectory temp ("dusk-midi-journal-wake-");
    // Writes into a session folder that is not there fail at once, with no
    // disk sync, so the report times the wake-up and not the disk.
    const auto absent = temp.path() / "absent";
    std::atomic<bool> armed { false }, appended { false }, accepted { false }, reached { false };
    recovery::Journal::Handle handle;
    std::vector<MidiCaptureStream::Record> records;
    MidiCaptureStream::DrainView drain;
    recovery::Journal journal ([&] (const recovery::PendingFile& file, const recovery::WriteResult&)
    {
        if (file.trackNumber == 5) reached.store (true);
    });
    journal.setIdleHookForTest ([&]
    {
        if (! armed.exchange (false)) return;
        accepted.store (journal.append (handle, drain));
        appended.store (true);
    });
    recovery::Journal::Config config;
    config.sessionDir = absent;
    config.made = november8th();
    config.trackNumber = 5;
    config.name = "Keys";
    config.loopLength = 100;
    handle = journal.begin (config);
    REQUIRE (handle.id != 0);
    {
        MidiCaptureStream capture;
        capture.configure ({}, 100);
        REQUIRE (capture.pushEvent (1, 0, 0x90, 60, 100));
        REQUIRE (capture.publishProgress (1, 100, true));
        capture.service (-1, [&] (const auto& view)
        {
            drain = view;
            records.assign (view.first, view.first + view.firstCount);
            records.insert (records.end(), view.second, view.second + view.secondCount);
        });
        drain.first = records.data();
        drain.firstCount = records.size();
        drain.second = nullptr;
        drain.secondCount = 0;
    }
    journal.flush();
    armed.store (true);
    // Wakes the worker for a take file of no journal's, so the hook runs at
    // the idle point after it.
    REQUIRE (journal.commit (fileFor (absent, 60, recovery::Folder::Takes), handle.id + 1000, 1));
    REQUIRE (eventually ([&] { return appended.load(); }));
    REQUIRE (accepted.load());
    CHECK (eventually ([&] { return reached.load(); }, std::chrono::milliseconds (500)));
}

// A take begun and appended to while the worker is deciding to sleep: the
// batch must not be decoded before the take is in hand, nor dropped.
TEST_CASE ("A MIDI take begun and appended as the journal worker goes to sleep is decoded", "[midi][recovery][journal][ipc]")
{
    const duskstudio::test::TempDirectory temp ("dusk-midi-journal-late-take-");
    const auto absent = temp.path() / "absent";
    std::atomic<bool> armed { false }, done { false }, accepted { false }, reached { false };
    std::vector<MidiCaptureStream::Record> records;
    MidiCaptureStream::DrainView drain;
    recovery::Journal::Config config;
    config.sessionDir = absent;
    config.made = november8th();
    config.trackNumber = 6;
    config.name = "Keys";
    config.loopLength = 100;
    recovery::Journal journal ([&] (const recovery::PendingFile& file, const recovery::WriteResult&)
    {
        if (file.trackNumber == 6) reached.store (true);
    });
    journal.setIdleHookForTest ([&]
    {
        if (! armed.exchange (false)) return;
        const auto late = journal.begin (config);
        accepted.store (late.id != 0 && journal.append (late, drain));
        done.store (true);
    });
    {
        MidiCaptureStream capture;
        capture.configure ({}, 100);
        REQUIRE (capture.pushEvent (1, 0, 0x90, 60, 100));
        REQUIRE (capture.publishProgress (1, 100, true));
        capture.service (-1, [&] (const auto& view)
        {
            drain = view;
            records.assign (view.first, view.first + view.firstCount);
            records.insert (records.end(), view.second, view.second + view.secondCount);
        });
        drain.first = records.data();
        drain.firstCount = records.size();
        drain.second = nullptr;
        drain.secondCount = 0;
    }
    auto earlier = config;
    earlier.trackNumber = 2;
    const auto first = journal.begin (earlier);
    REQUIRE (first.id != 0);
    journal.flush();
    armed.store (true);
    REQUIRE (journal.commit (fileFor (absent, 60, recovery::Folder::Takes), first.id + 1000, 1));
    REQUIRE (eventually ([&] { return done.load(); }));
    REQUIRE (accepted.load());
    // Nothing else wakes the worker, and a batch decoded without its take
    // would be dropped without a word, so the pass's report is the proof.
    CHECK (eventually ([&] { return reached.load(); }));
}

TEST_CASE ("A MIDI journal lets a folder it wrote into go only once nothing is left in it", "[midi][recovery][journal]")
{
    const duskstudio::test::TempDirectory temp ("dusk-midi-journal-release-");
    const auto folder = temp.path() / "recordings" / "take";
    const auto directory = folder / "midi" / "recording";
    REQUIRE (stdfs::create_directories (folder));
    Reports reports;
    recovery::Journal::Config config;
    config.sessionDir = folder;
    config.made = november8th();
    config.trackNumber = 7;
    config.name = "Keys";
    const auto playNote = [] (recovery::Journal& journal, const recovery::Journal::Handle& handle)
    {
        MidiCaptureStream capture;
        REQUIRE (capture.pushEvent (1, 0, 0x90, 60, 100));
        REQUIRE (capture.publishProgress (1, 10, false));
        capture.service (-1, [&] (const auto& view) { REQUIRE (journal.append (handle, view)); });
    };

    SECTION ("a folder no take wrote into goes at once, even before any take began")
    {
        recovery::Journal journal (reports.callback());
        journal.release (folder);
        journal.flush();
        CHECK_FALSE (stdfs::exists (folder));
        CHECK (stdfs::is_directory (folder.parent_path()));
    }
    SECTION ("a folder whose take was stopped with nothing played goes")
    {
        recovery::Journal journal (reports.callback());
        auto handle = journal.begin (config);
        REQUIRE (handle.id != 0);
        REQUIRE (journal.finish (handle, 1, 0));
        journal.release (folder);
        journal.flush();
        CHECK_FALSE (stdfs::exists (folder));
    }
    SECTION ("a folder goes once the last checkpoint in it gives way to its take file")
    {
        recovery::Journal journal (reports.callback());
        auto handle = journal.begin (config);
        REQUIRE (handle.id != 0);
        playNote (journal, handle);
        REQUIRE (journal.finish (handle, 1, 10));
        journal.release (folder);
        journal.flush();
        REQUIRE (namesIn (directory).size() == 1);
        const auto elsewhere = temp.path() / "Untitled";
        REQUIRE (stdfs::create_directory (elsewhere));
        REQUIRE (journal.commit (fileFor (elsewhere, 60, recovery::Folder::Takes), handle.id, 1));
        journal.flush();
        CHECK (namesIn (elsewhere / "midi" / "takes").size() == 1);
        CHECK_FALSE (stdfs::exists (folder));
    }
    SECTION ("a checkpoint whose take file was never written keeps its folder")
    {
        recovery::Journal journal (reports.callback());
        auto handle = journal.begin (config);
        REQUIRE (handle.id != 0);
        playNote (journal, handle);
        REQUIRE (journal.finish (handle, 1, 10));
        journal.release (folder);
        journal.flush();
        CHECK (namesIn (directory).size() == 1);
    }
    SECTION ("an empty folder a pass still waits to be written into stays until it is written")
    {
        struct Writable
        {
            stdfs::path folder;
            ~Writable() { std::error_code ignored; stdfs::permissions (folder, stdfs::perms::owner_all, ignored); }
        } writable { folder };
        stdfs::permissions (folder, stdfs::perms::owner_read | stdfs::perms::owner_exec);
        std::error_code blocked;
        if (stdfs::create_directory (folder / "probe", blocked))
        {
            stdfs::remove (folder / "probe", blocked);
            SKIP ("a folder without write permission can still be written in here");
        }
        recovery::Journal journal (reports.callback());
        auto handle = journal.begin (config);
        REQUIRE (handle.id != 0);
        playNote (journal, handle);
        REQUIRE (journal.finish (handle, 1, 10));
        journal.release (folder);
        journal.flush();
        REQUIRE (stdfs::is_directory (folder));
        REQUIRE (stdfs::is_empty (folder));
        stdfs::permissions (folder, stdfs::perms::owner_all);
        journal.flush();
        CHECK (namesIn (directory).size() == 1);
    }
    SECTION ("a file of anyone else's keeps the folder and stays as it is")
    {
        { std::ofstream foreign (folder / "notes.txt"); foreign << "someone's notes"; }
        recovery::Journal journal (reports.callback());
        journal.release (folder);
        journal.flush();
        CHECK (dusk::fs::loadFileAsString (folder / "notes.txt") == "someone's notes");
    }
}
