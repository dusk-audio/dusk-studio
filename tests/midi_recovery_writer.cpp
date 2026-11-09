#include <catch2/catch_test_macros.hpp>

#include "TestTempDirectory.h"
#include "foundation/Fs.h"
#include "session/MidiRecovery.h"
#include "session/MidiRecoveryWriter.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
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
