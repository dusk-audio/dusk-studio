#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/audiofile/BufferedFileReader.h"
#include "engine/audiofile/FileWriter.h"
#include "engine/audiofile/ReaderFillPool.h"
#include "TestTempDirectory.h"

#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

using namespace dusk::audio;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr std::int64_t kFrames = 50000;
constexpr int kChannels = 2;

float sample (int channel, std::int64_t frame)
{
    return (float) frame / 100000.0f + (float) channel * 0.25f;
}

struct SampleFile
{
    duskstudio::test::TempDirectory directory { "reader_fill_pool" };
    std::filesystem::path path = directory.path() / "samples.wav";

    explicit SampleFile (int channelCount = kChannels)
    {
        std::array<std::vector<float>, kChannels> data;
        std::array<const float*, kChannels> pointers;
        for (int c = 0; c < channelCount; ++c)
        {
            data[(std::size_t) c].resize ((std::size_t) kFrames);
            for (std::int64_t f = 0; f < kFrames; ++f)
                data[(std::size_t) c][(std::size_t) f] = sample (c, f);
            pointers[(std::size_t) c] = data[(std::size_t) c].data();
        }
        WriteSpec spec;
        spec.sampleRate = 48000.0;
        spec.numChannels = channelCount;
        spec.bitsPerSample = 32;
        auto writer = FileWriter::create (path, spec);
        REQUIRE (writer != nullptr);
        REQUIRE (writer->write (pointers.data(), channelCount, kFrames));
    }

    std::unique_ptr<BufferedFileReader> reader (std::int64_t window = 20001) const
    {
        auto raw = FileReader::open (path);
        REQUIRE (raw != nullptr);
        return std::make_unique<BufferedFileReader> (std::move (raw), window,
                                                     BufferedFileReader::Fill::Manual);
    }
};

struct Block
{
    std::array<std::array<float, 512>, kChannels> data {};
    std::array<float*, kChannels> pointers { data[0].data(), data[1].data() };

    void check (std::int64_t start, std::int64_t count = 512, int channels = kChannels) const
    {
        for (int c = 0; c < kChannels; ++c)
            for (std::int64_t f = 0; f < 512; ++f)
                REQUIRE_THAT (data[(std::size_t) c][(std::size_t) f],
                              WithinAbs (c < channels && f < count && start + f >= 0 && start + f < kFrames
                                             ? sample (c, start + f) : 0.0f, 1.0e-7f));
    }
};
} // namespace

TEST_CASE ("Manual reader allocates lazily and publishes each bounded fill pass as it reads",
           "[audiofile][buffered][reader-pool]")
{
    SampleFile file;
    auto reader = file.reader();
    CHECK (reader->allocatedBytes() == 0);
    CHECK_FALSE (reader->hasResidency());
    Block block;
    CHECK_FALSE (reader->readRt (block.pointers.data(), kChannels, 0, 512));
    block.check (0, 0);
    CHECK (reader->readMisses() == 1);

    REQUIRE (reader->fillOnce());
    CHECK (reader->allocatedBytes() > 0);
    CHECK (reader->holds (0, BufferedFileReader::kFillChunkFrames));
    CHECK_FALSE (reader->holds (0, BufferedFileReader::kFillChunkFrames + 1));
    REQUIRE (reader->readRt (block.pointers.data(), kChannels, 0, 512));
    block.check (0);
    REQUIRE (reader->fillOnce());
    CHECK (reader->holds (0, 2 * BufferedFileReader::kFillChunkFrames));
    REQUIRE (reader->fillOnce());
    CHECK (reader->holds (0, 20000));
    CHECK_FALSE (reader->holds (20000, 1));
    CHECK_FALSE (reader->fillOnce());
    REQUIRE (reader->readRt (block.pointers.data(), kChannels, 0, 512));
    block.check (0);
}

TEST_CASE ("Reader keeps the old immutable window while a replacement is staged",
           "[audiofile][buffered][reader-pool]")
{
    SampleFile file;
    auto reader = file.reader();
    reader->fillNow();
    Block block;
    SECTION ("A position no window holds is served by each pass as it is read")
    {
        reader->hintRt (22000);
        REQUIRE (reader->fillOnce());
        CHECK (reader->holds (22000, BufferedFileReader::kFillChunkFrames));
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 22000, 512));
        block.check (22000);
        for (int pass = 0; pass < 2; ++pass) REQUIRE (reader->fillOnce());
        CHECK (reader->holds (22000, 20000));
        CHECK_FALSE (reader->holds (0, 512));
    }
    SECTION ("A one-frame retarget refills the missing end of a full seek span")
    {
        auto seekReader = file.reader (8193);
        REQUIRE (seekReader->fillSeekOnce (1000, 0, 8192));
        REQUIRE (seekReader->holdsSeek (1000, 8192));
        REQUIRE_FALSE (seekReader->holdsSeek (1001, 8192));
        CHECK (seekReader->needsSeekFill (1001, 8192));
        CHECK (seekReader->fillSeekOnce (1001, 0, 8192));
        CHECK (seekReader->holdsSeek (1001, 8192));
    }
    SECTION ("A retarget inside an unfinished seek stage can require a new end")
    {
        auto seekReader = file.reader (16385);
        REQUIRE (seekReader->fillSeekOnce (1000, 0, 16384));
        REQUIRE (seekReader->holdsSeek (1000, 8192));
        REQUIRE_FALSE (seekReader->holdsSeek (1000, 8193));
        REQUIRE (seekReader->fillSeekOnce (1001, 0, 16384));
        CHECK_FALSE (seekReader->holdsSeek (1001, 16384));
        CHECK (seekReader->fillSeekOnce (1001, 0, 16384));
        CHECK (seekReader->holdsSeek (1001, 16384));
    }
    SECTION ("An undersized window still publishes its payload without claiming full readiness")
    {
        auto seekReader = file.reader (16385);
        REQUIRE (seekReader->fillSeekOnce (1000, 0, 20000));
        REQUIRE (seekReader->fillSeekOnce (1000, 0, 20000));
        CHECK (seekReader->holdsSeek (1000, 16384));
        CHECK (seekReader->needsSeekFill (1000, 20000));
    }
    SECTION ("A short read publishes its valid prefix and retains the channel stride")
    {
        const auto headerBytes = std::filesystem::file_size (file.path) - kFrames * kChannels * sizeof (float);
        std::filesystem::resize_file (file.path, headerBytes + 24000 * kChannels * sizeof (float));
        reader->hintRt (22000);
        CHECK_FALSE (reader->fillFailing());
        CHECK_FALSE (reader->fillOnce());
        CHECK (reader->fillFailing());
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 22000, 512));
        block.check (22000);
        CHECK_FALSE (reader->holds (24000, 1));
        CHECK_FALSE (reader->readRt (block.pointers.data(), kChannels, 23800, 512));
        block.check (23800, 200);
    }
    SECTION ("Seek publication preserves current audio through every bounded pass")
    {
        for (int pass = 0; pass < 3; ++pass)
        {
            REQUIRE (reader->fillSeekOnce (22000, 1000, 20000));
            CHECK (reader->holdsSeek (22000, 20000) == (pass == 2));
            REQUIRE (reader->readRt (block.pointers.data(), kChannels, 1000, 512));
            block.check (1000);
        }
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 22000, 512));
        block.check (22000);
        CHECK_FALSE (reader->needsFill());
        const auto bytes = reader->allocatedBytes();
        CHECK_FALSE (reader->needsSeekFill (22001, 8192));
        CHECK_FALSE (reader->fillSeekOnce (22001, 22000, 8192));
        CHECK (reader->allocatedBytes() == bytes);
        REQUIRE (reader->fillOnce (44000));
        CHECK (reader->holds (44000, 6000));
        CHECK_FALSE (reader->holds (1000, 512));
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 22000, 512));
        block.check (22000);
    }
    SECTION ("Replacing a seek retains the whole playing block beyond the old current window")
    {
        for (int pass = 0; pass < 3; ++pass)
            REQUIRE (reader->fillSeekOnce (10000, 0, 20000));
        REQUIRE (reader->fillSeekOnce (32000, 19800, 8192));
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 19800, 512));
        block.check (19800);
        CHECK_FALSE (reader->fillSeekOnce (32000, 19800, 8192));
        CHECK_FALSE (reader->holdsSeek (10000, 20000));
        CHECK (reader->holds (10000, 20000));
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 32000, 512));
        block.check (32000);
    }
    SECTION ("A retarget reuses an unfinished stage when the requested end fits")
    {
        REQUIRE (reader->fillSeekOnce (22000, 0, 8192));
        CHECK_FALSE (reader->holdsSeek (22001, 8192));
        REQUIRE (reader->fillSeekOnce (22001, 0, 8192));
        CHECK (reader->holdsSeek (22001, 8192));
        CHECK (reader->holdsSeek (22000, 2 * BufferedFileReader::kFillChunkFrames));
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 22001, 512));
        block.check (22001);
    }
    SECTION ("A disjoint retarget discards the superseded unfinished seek")
    {
        REQUIRE (reader->fillSeekOnce (22000, 0, 8192));
        REQUIRE (reader->fillSeekOnce (1000, 0, 8192));
        CHECK_FALSE (reader->fillSeekOnce (1000, 0, 8192));
        CHECK_FALSE (reader->holdsSeek (22000, 8192));
        CHECK (reader->holdsSeek (1000, 8192));
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 1000, 512));
        block.check (1000);
    }
    SECTION ("Ordinary filling never lands in the seek window")
    {
        REQUIRE (reader->fillOnce (22000));
        REQUIRE (reader->fillSeekOnce (30000, 0, 8192));
        CHECK_FALSE (reader->holdsSeek (22000, 8192));
        CHECK (reader->holds (22000, 8192));
        CHECK (reader->holdsSeek (30000, 8192));
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 30000, 512));
        block.check (30000);
    }
    SECTION ("Playback committed inside a seek still filling keeps filling it")
    {
        REQUIRE (reader->fillSeekOnce (30000, 0, 8192));
        for (int pass = 0; pass < 2; ++pass)
            REQUIRE (reader->fillOnce (31000));
        CHECK (reader->holdsSeek (30000, 20000));
        CHECK_FALSE (reader->fillOnce (31000));
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 31000, 512));
        block.check (31000);
    }
    SECTION ("A short seek keeps its planar stride and still requests the missing warm span")
    {
        const auto headerBytes = std::filesystem::file_size (file.path) - kFrames * kChannels * sizeof (float);
        std::filesystem::resize_file (file.path, headerBytes + 33000 * kChannels * sizeof (float));
        REQUIRE (reader->fillSeekOnce (22000, 0, 20000));
        CHECK_FALSE (reader->fillSeekOnce (22000, 0, 20000));
        CHECK (reader->holdsSeek (22000, 11000));
        CHECK (reader->needsSeekFill (22000, 20000));
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 22000, 512));
        block.check (22000);
        CHECK_FALSE (reader->readRt (block.pointers.data(), kChannels, 32800, 512));
        block.check (32800, 200);
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 1000, 512));
        block.check (1000);
    }
    SECTION ("Seek readiness clips negative prefixes and EOF suffixes to real file frames")
    {
        CHECK_FALSE (reader->needsSeekFill (-512, 512));
        CHECK_FALSE (reader->fillSeekOnce (-512, 0, 512));
        CHECK_FALSE (reader->needsSeekFill (1000, 0));
        CHECK_FALSE (reader->fillSeekOnce (1000, 0, -1));
        REQUIRE (reader->fillSeekOnce (-256, 0, 512));
        CHECK_FALSE (reader->fillSeekOnce (-256, 0, 512));
        CHECK (reader->holdsSeek (-256, 512));
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, -256, 512));
        block.check (-256);
        REQUIRE (reader->fillSeekOnce (kFrames - 100, 1000, 8192));
        CHECK_FALSE (reader->needsSeekFill (kFrames - 100, 8192));
        CHECK_FALSE (reader->fillSeekOnce (kFrames - 100, 1000, 8192));
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, kFrames - 100, 512));
        block.check (kFrames - 100);
        CHECK_FALSE (reader->needsSeekFill (kFrames, 512));
        CHECK_FALSE (reader->fillSeekOnce (kFrames, 1000, 512));
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, kFrames, 512));
        block.check (kFrames);
        CHECK (reader->readMisses() == 0);
    }
    SECTION ("Seek reads clear extra channels and respect a reduced destination")
    {
        for (int channelCount : { 1, 2 })
        {
            SampleFile sourceFile (channelCount);
            auto seekReader = sourceFile.reader();
            REQUIRE (seekReader->fillSeekOnce (22000, -1, 8192));
            CHECK (seekReader->hasResidency());
            REQUIRE (seekReader->readRt (block.pointers.data(), kChannels, 22000, 512));
            block.check (22000, 512, channelCount);
            block.data[1].fill (-1.0f);
            REQUIRE (seekReader->readRt (block.pointers.data(), 1, 23000, 512));
            for (std::size_t f = 0; f < 512; ++f)
            {
                CHECK_THAT (block.data[0][f], WithinAbs (sample (0, 23000 + (std::int64_t) f), 1.0e-7f));
                CHECK_THAT (block.data[1][f], WithinAbs (-1.0f, 1.0e-7f));
            }
        }
    }
    SECTION ("Eviction clears current seek and partial staging before reuse")
    {
        REQUIRE (reader->fillSeekOnce (22000, 0, 8192));
        reader->hintRt (12000);
        REQUIRE (reader->fillOnce());
        CHECK (reader->holds (0, 20000));
        REQUIRE (reader->allocatedBytes() >= 3 * 20000 * kChannels * sizeof (float));
        reader->releaseResidency();
        CHECK (reader->allocatedBytes() == 0);
        CHECK_FALSE (reader->hasResidency());
        CHECK_FALSE (reader->holds (0, 512));
        CHECK_FALSE (reader->holdsSeek (22000, 512));
        BufferedFileReader::ReadCoverage coverage { 123, 456 };
        CHECK_FALSE (reader->readRt (block.pointers.data(), kChannels, 22000, 512, &coverage));
        block.check (0, 0);
        CHECK (coverage.startFrame == 0);
        CHECK (coverage.endFrame == 0);
        reader->fillNow();
        CHECK_FALSE (reader->holdsSeek (22000, 512));
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 22000, 512, &coverage));
        block.check (22000);
        CHECK (coverage.startFrame == 22000);
        CHECK (coverage.endFrame == 22512);
    }
}

TEST_CASE ("Reader low-water refill preserves current audio until publication",
           "[audiofile][buffered][reader-pool]")
{
    SampleFile file;
    auto reader = file.reader();
    reader->fillNow();
    reader->hintRt (12000);
    CHECK (reader->needsFill());
    Block block;
    SECTION ("A complete replacement advances the window")
    {
        REQUIRE (reader->fillOnce());
        CHECK (reader->holds (0, 20000));
        reader->fillNow();
        CHECK_FALSE (reader->holds (0, 1));
        CHECK (reader->holds (12000, 20000));
    }
    SECTION ("A short replacement cannot discard longer resident audio")
    {
        const auto headerBytes = std::filesystem::file_size (file.path) - kFrames * kChannels * sizeof (float);
        std::filesystem::resize_file (file.path, headerBytes + 12100 * kChannels * sizeof (float));
        CHECK_FALSE (reader->fillOnce());
        CHECK (reader->holds (0, 20000));
    }
    REQUIRE (reader->readRt (block.pointers.data(), kChannels, 12000, 512));
    block.check (12000);
}

TEST_CASE ("Reader eviction frees sample memory and permits deterministic refill",
           "[audiofile][buffered][reader-pool]")
{
    SampleFile file;
    auto reader = file.reader();
    reader->fillNow();
    CHECK (reader->allocatedBytes() > 0);
    reader->releaseResidency();
    CHECK (reader->allocatedBytes() == 0);
    CHECK_FALSE (reader->hasResidency());
    Block block;
    CHECK_FALSE (reader->readRt (block.pointers.data(), kChannels, 1000, 512));
    block.check (0, 0);
    reader->fillNow();
    REQUIRE (reader->readRt (block.pointers.data(), kChannels, 1000, 512));
    block.check (1000);
}

TEST_CASE ("Reader treats EOF silence as resident without counting a miss",
           "[audiofile][buffered][reader-pool]")
{
    SampleFile file;
    auto reader = file.reader();
    reader->hintRt (kFrames - 100);
    REQUIRE (reader->fillOnce());
    CHECK_FALSE (reader->fillOnce());
    Block block;
    REQUIRE (reader->readRt (block.pointers.data(), kChannels, kFrames - 100, 512));
    block.check (kFrames - 100, 100);
    reader->releaseResidency();
    CHECK (reader->holds (kFrames, 512));
    REQUIRE (reader->readRt (block.pointers.data(), kChannels, kFrames, 512));
    block.check (0, 0);
    CHECK (reader->readMisses() == 0);
}

TEST_CASE ("Fill pool prioritizes near readers and evicts outside its horizon",
           "[audiofile][reader-pool]")
{
    SampleFile file;
    auto active = file.reader (129);
    auto future = file.reader (129);
    auto distant = file.reader (129);
    ReaderFillPool pool (3, false);
    pool.configure (1000.0);
    pool.add (*future, 2000, 2500, 0);
    pool.add (*distant, 10000, 11000, 0);
    pool.add (*active, 0, 1000, 0);
    REQUIRE (pool.pumpOnce());
    CHECK (active->hasResidency());
    CHECK_FALSE (future->hasResidency());
    REQUIRE (pool.pumpOnce());
    CHECK (future->hasResidency());
    CHECK (distant->allocatedBytes() == 0);
    CHECK_FALSE (pool.pumpOnce());
    CHECK (pool.stats().workers == 0);
    CHECK (pool.stats().residentReaders == 2);

    pool.setPositionRt (2500);
    CHECK_FALSE (pool.pumpOnce());
    CHECK (active->holds (0, 128));
    CHECK (future->holds (0, 128));

    pool.setPositionRt (10000);
    REQUIRE (pool.pumpOnce());
    REQUIRE (pool.pumpOnce());
    REQUIRE (pool.pumpOnce());
    CHECK (distant->hasResidency());
    CHECK (active->allocatedBytes() == 0);
    CHECK (future->allocatedBytes() == 0);
    CHECK (pool.stats().residentReaders == 1);
    pool.reset();
    CHECK (pool.stats().readers == 0);
    CHECK (distant->allocatedBytes() == 0);
}

TEST_CASE ("Reader minimum window advances without repeatedly refilling the same frame",
           "[audiofile][buffered][reader-pool]")
{
    SampleFile file;
    auto reader = file.reader (2);
    Block block;
    for (std::int64_t at = 0; at < 5; ++at)
    {
        reader->hintRt (at);
        REQUIRE (reader->fillOnce());
        CHECK_FALSE (reader->fillOnce());
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, at, 1));
        for (int c = 0; c < kChannels; ++c)
            REQUIRE_THAT (block.data[(std::size_t) c][0], WithinAbs (sample (c, at), 1.0e-7f));
    }
}

TEST_CASE ("Reader partial residency clears both missing sides of the destination",
           "[audiofile][buffered][reader-pool]")
{
    SampleFile file;
    auto reader = file.reader (129);
    reader->hintRt (1000);
    reader->fillNow();
    Block block;
    for (auto& channel : block.data) channel.fill (-1.0f);
    CHECK_FALSE (reader->readRt (block.pointers.data(), kChannels, 950, 512));
    for (int c = 0; c < kChannels; ++c)
        for (std::int64_t f = 0; f < 512; ++f)
            REQUIRE_THAT (block.data[(std::size_t) c][(std::size_t) f],
                          WithinAbs (f >= 50 && f < 178 ? sample (c, 950 + f) : 0.0f, 1.0e-7f));
    CHECK (reader->readMisses() == 1);
}

TEST_CASE ("Fill pool priming maps timeline to source offsets and removal fences ownership",
           "[audiofile][reader-pool]")
{
    SampleFile file;
    SECTION ("Explicit priming maps offsets and removal releases the reader")
    {
        auto reader = file.reader (4097);
        ReaderFillPool pool (3, false);
        pool.configure (1000.0);
        pool.add (*reader, 1000, 9000, 12000);
        pool.primeAt (3000);
        CHECK (reader->holds (14000, 4096));
        for (int seek = 0; seek < 10; ++seek)
        {
            pool.primeAt (100000);
            CHECK (reader->allocatedBytes() == 0);
            pool.primeAt (3000);
            while (pool.pumpOnce()) {}
            REQUIRE (reader->holds (14000, 4096));
        }
        Block block;
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 14000, 512));
        block.check (14000);
        pool.remove (*reader);
        CHECK (pool.stats().readers == 0);
        CHECK (reader->allocatedBytes() == 0);
        reader.reset();
        CHECK_FALSE (pool.pumpOnce());
    }
    SECTION ("A required seek span reaches a region beyond the ordinary ahead horizon")
    {
        auto reader = file.reader (2001);
        ReaderFillPool pool (3, false);
        pool.configure (1000.0, 0.0, 0.0);
        pool.add (*reader, 2000, 3000, 1000);
        pool.setSeekTargetRt (1000, 2000);
        CHECK_FALSE (pool.seekReadyRt (1000, 2000));
        REQUIRE (pool.pumpOnce());
        CHECK (pool.seekReadyRt (1000, 2000));
        CHECK_FALSE (pool.pumpOnce());
        Block block;
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 1000, 512));
        block.check (1000);
    }
    SECTION ("A short region requests only its overlapping source span")
    {
        auto reader = file.reader (129);
        ReaderFillPool pool (3, false);
        pool.configure (1000.0);
        pool.add (*reader, 10000, 10100, 1000);
        pool.setSeekTargetRt (10000, 8192);
        REQUIRE (pool.pumpOnce());
        CHECK (pool.seekReadyRt (10000, 8192));
        CHECK_FALSE (pool.pumpOnce());
    }
    SECTION ("A playing callback entering a region retains its previously staged source")
    {
        auto reader = file.reader();
        ReaderFillPool pool (3, false);
        pool.configure (1000.0);
        pool.add (*reader, 1000, 46000, 1000);
        pool.primeAt (30000);
        pool.setSeekTargetRt (1000, 512);
        REQUIRE (pool.pumpOnce());
        REQUIRE (pool.seekReadyRt (1000, 512));
        pool.setPositionRt (900);
        pool.setSeekTargetRt (32000, 512);
        Block block;
        int passes = 0;
        while (pool.pumpOnce())
        {
            ++passes;
            REQUIRE (reader->readRt (block.pointers.data(), kChannels, 1000, 512));
            block.check (1000);
        }
        CHECK (passes >= 2);
        CHECK (pool.seekReadyRt (32000, 512));
    }
    SECTION ("Reset and explicit priming cancel requests from the previous playback")
    {
        auto reader = file.reader();
        ReaderFillPool pool (3, false);
        pool.configure (1000.0);
        pool.add (*reader, 1000, 46000, 1000);
        pool.primeAt (1000);
        pool.setSeekTargetRt (30000, 512);
        REQUIRE (pool.pumpOnce());
        REQUIRE (pool.seekReadyRt (30000, 512));
        SECTION ("Reset")
        {
            pool.reset();
            CHECK_FALSE (pool.seekReadyRt (30000, 512));
            pool.setSeekTargetRt (5000, 8192);
            CHECK (pool.seekReadyRt (5000, 8192));
            pool.clearSeekTargetRt();
        }
        SECTION ("Priming") { pool.primeAt (100000); }
        CHECK_FALSE (pool.seekReadyRt (30000, 512));
        CHECK_FALSE (pool.pumpOnce());
        CHECK (reader->allocatedBytes() == 0);
    }
    SECTION ("Staged seeks preserve current playback through publication and the next seek")
    {
        auto reader = file.reader();
        ReaderFillPool pool (3, false);
        pool.configure (1000.0);
        pool.add (*reader, 1000, 46000, 1000);
        pool.primeAt (1000);
        pool.setSeekTargetRt (30000, 512);
        Block block;
        CHECK_FALSE (pool.seekReadyRt (30000, 512));
        REQUIRE (pool.pumpOnce());
        CHECK (pool.seekReadyRt (30000, 512));
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 1000, 512));
        block.check (1000);
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 30000, 512));
        block.check (30000);
        pool.setPositionRt (30000);
        pool.clearSeekTargetRt();
        CHECK_FALSE (pool.seekReadyRt (30000, 512));
        pool.setSeekTargetRt (5000, 512);
        int passes = 0;
        while (pool.pumpOnce())
        {
            ++passes;
            REQUIRE (reader->readRt (block.pointers.data(), kChannels, 30000, 512));
            block.check (30000);
        }
        CHECK (passes >= 2);
        CHECK (pool.seekReadyRt (5000, 512));
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 5000, 512));
        block.check (5000);
    }
    SECTION ("Retarget cancellation and changed warm counts acknowledge only the new request")
    {
        auto reader = file.reader();
        ReaderFillPool pool (3, false);
        pool.configure (1000.0);
        pool.add (*reader, 1000, 46000, 1000);
        pool.primeAt (1000);
        pool.setSeekTargetRt (30000, 8192);
        REQUIRE (pool.pumpOnce());
        pool.setSeekTargetRt (35000, 8192);
        CHECK_FALSE (pool.seekReadyRt (35000, 8192));
        REQUIRE (pool.pumpOnce());
        REQUIRE (pool.seekReadyRt (35000, 8192));
        CHECK_FALSE (pool.seekReadyRt (30000, 8192));
        CHECK (reader->holdsSeek (30000, 2 * BufferedFileReader::kFillChunkFrames));
        CHECK_FALSE (pool.pumpOnce());
        const auto filled = pool.stats().fillPasses;
        REQUIRE (filled > 0);
        pool.setSeekTargetRt (35000, 8192);
        CHECK (pool.seekReadyRt (35000, 8192));
        pool.clearSeekTargetRt();
        CHECK_FALSE (pool.seekReadyRt (35000, 8192));
        pool.setSeekTargetRt (35000, 8192);
        CHECK_FALSE (pool.seekReadyRt (35000, 8192));
        CHECK_FALSE (pool.pumpOnce());
        REQUIRE (pool.seekReadyRt (35000, 8192));
        pool.setSeekTargetRt (35001, 4096);
        CHECK_FALSE (pool.seekReadyRt (35001, 4096));
        CHECK_FALSE (pool.pumpOnce());
        CHECK (pool.seekReadyRt (35001, 4096));
        CHECK_FALSE (pool.seekReadyRt (35001, 4097));
        pool.setSeekTargetRt (35001, 8192);
        CHECK_FALSE (pool.seekReadyRt (35001, 8192));
        CHECK_FALSE (pool.pumpOnce());
        CHECK (pool.seekReadyRt (35001, 8192));
        CHECK_FALSE (pool.seekReadyRt (35001, 8193));
        CHECK (pool.stats().fillPasses == filled);
    }
    SECTION ("A one-frame retarget warms the last frame of the complete requested span")
    {
        auto reader = file.reader (8193);
        ReaderFillPool pool (3, false);
        pool.configure (1000.0);
        pool.add (*reader, 10000, 40000, 1000);
        pool.setSeekTargetRt (10000, 8192);
        REQUIRE (pool.pumpOnce());
        REQUIRE (pool.seekReadyRt (10000, 8192));
        pool.setSeekTargetRt (10001, 8192);
        CHECK_FALSE (pool.seekReadyRt (10001, 8192));
        REQUIRE (pool.pumpOnce());
        CHECK (pool.seekReadyRt (10001, 8192));
        CHECK_FALSE (pool.pumpOnce());
        Block block;
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 8681, 512));
        block.check (8681);
    }
    SECTION ("A retarget restarts a partial stage whose last required frame is missing")
    {
        auto reader = file.reader (16385);
        ReaderFillPool pool (3, false);
        pool.configure (1000.0, 0.0, 0.0);
        pool.add (*reader, 10000, 40000, 1000);
        pool.setSeekTargetRt (10000, 16384);
        REQUIRE (pool.pumpOnce());
        pool.setSeekTargetRt (10001, 16384);
        REQUIRE (pool.pumpOnce());
        CHECK_FALSE (pool.seekReadyRt (10001, 16384));
        REQUIRE (pool.pumpOnce());
        CHECK (pool.seekReadyRt (10001, 16384));
        Block block;
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 16873, 512));
        block.check (16873);
    }
    SECTION ("A clipped region span reuses a partial stage that already contains its end")
    {
        auto reader = file.reader();
        ReaderFillPool pool (3, false);
        pool.configure (1000.0, 0.0, 0.0);
        pool.add (*reader, 10000, 19000, 1000);
        pool.setSeekTargetRt (10000, 20000);
        REQUIRE (pool.pumpOnce());
        CHECK_FALSE (pool.seekReadyRt (10000, 20000));
        pool.setSeekTargetRt (11000, 20000);
        REQUIRE (pool.pumpOnce());
        CHECK (pool.seekReadyRt (11000, 20000));
        CHECK (reader->holdsSeek (1000, 2 * BufferedFileReader::kFillChunkFrames));
    }
    SECTION ("Seek prefetch keeps nearby future regions inside its ordinary horizon")
    {
        auto required = file.reader (513);
        auto future = file.reader (513);
        auto distant = file.reader (513);
        ReaderFillPool pool (3, false);
        pool.configure (1000.0, 3.0, 0.0);
        pool.add (*distant, 34000, 35000, 20000);
        pool.add (*future, 32000, 32100, 10000);
        pool.add (*required, 30000, 31000, 1000);
        pool.setSeekTargetRt (30000, 512);
        REQUIRE (pool.pumpOnce());
        CHECK (pool.seekReadyRt (30000, 512));
        REQUIRE (pool.pumpOnce());
        CHECK (future->holdsSeek (10000, 100));
        CHECK (distant->allocatedBytes() == 0);
        CHECK_FALSE (pool.pumpOnce());
        Block block;
        REQUIRE (future->readRt (block.pointers.data(), kChannels, 10000, 512));
        block.check (10000);
    }
    SECTION ("A warm span maps each intersecting region to its own source offset")
    {
        auto first = file.reader (513);
        auto second = file.reader (1025);
        ReaderFillPool pool (3, false);
        pool.configure (1000.0, 0.0, 0.0);
        pool.add (*second, 1256, 2024, 30000);
        pool.add (*first, 1000, 1256, 10000);
        pool.setSeekTargetRt (1100, 700);
        REQUIRE (pool.pumpOnce());
        CHECK_FALSE (pool.seekReadyRt (1100, 700));
        CHECK (first->holdsSeek (10100, 156));
        CHECK_FALSE (second->hasResidency());
        REQUIRE (pool.pumpOnce());
        CHECK (pool.seekReadyRt (1100, 700));
        CHECK_FALSE (pool.seekReadyRt (1100, 701));
        CHECK_FALSE (pool.pumpOnce());
        Block block;
        REQUIRE (first->readRt (block.pointers.data(), kChannels, 10100, 512));
        block.check (10100);
        REQUIRE (second->readRt (block.pointers.data(), kChannels, 30000, 512));
        block.check (30000);
    }
    SECTION ("Empty gaps and EOF silence do not require disk fills")
    {
        auto empty = file.reader (129);
        auto tail = file.reader (1025);
        ReaderFillPool pool (3, false);
        pool.configure (1000.0, 0.0, 0.0);
        pool.setSeekTargetRt (1000, 512);
        CHECK (pool.seekReadyRt (1000, 512));
        CHECK_FALSE (pool.pumpOnce());
        pool.add (*empty, 1200, 1200, 1000);
        CHECK_FALSE (pool.seekReadyRt (1000, 512));
        CHECK_FALSE (pool.pumpOnce());
        CHECK (pool.seekReadyRt (1000, 512));
        CHECK (empty->allocatedBytes() == 0);
        pool.add (*tail, 5000, 7000, kFrames - 100);
        pool.setSeekTargetRt (5000, 512);
        REQUIRE (pool.pumpOnce());
        CHECK (pool.seekReadyRt (5000, 512));
        Block block;
        REQUIRE (tail->readRt (block.pointers.data(), kChannels, kFrames - 100, 512));
        block.check (kFrames - 100);
        const auto filled = pool.stats().fillPasses;
        REQUIRE (filled > 0);
        pool.setSeekTargetRt (5100, 512);
        CHECK_FALSE (pool.pumpOnce());
        CHECK (pool.seekReadyRt (5100, 512));
        CHECK (pool.stats().fillPasses == filled);
    }
    SECTION ("Membership changes invalidate acknowledgement and reset permits re-registration")
    {
        auto first = file.reader (1025);
        auto second = file.reader (1025);
        ReaderFillPool pool (3, false);
        pool.configure (1000.0, 0.0, 0.0);
        pool.setSeekTargetRt (1000, 512);
        REQUIRE (pool.seekReadyRt (1000, 512));
        pool.add (*first, 1000, 2000, 1000);
        CHECK_FALSE (pool.seekReadyRt (1000, 512));
        REQUIRE (pool.pumpOnce());
        REQUIRE (pool.seekReadyRt (1000, 512));
        pool.add (*second, 1000, 2000, 30000);
        CHECK_FALSE (pool.seekReadyRt (1000, 512));
        REQUIRE (pool.pumpOnce());
        REQUIRE (pool.seekReadyRt (1000, 512));
        pool.remove (*first);
        CHECK_FALSE (pool.seekReadyRt (1000, 512));
        CHECK (first->allocatedBytes() == 0);
        CHECK_FALSE (pool.pumpOnce());
        REQUIRE (pool.seekReadyRt (1000, 512));
        pool.remove (*first);
        CHECK (pool.seekReadyRt (1000, 512));
        Block block;
        REQUIRE (second->readRt (block.pointers.data(), kChannels, 30000, 512));
        block.check (30000);
        pool.remove (*second);
        CHECK (pool.seekReadyRt (1000, 512));
        CHECK (second->allocatedBytes() == 0);
        pool.reset();
        CHECK_FALSE (pool.seekReadyRt (1000, 512));
        pool.add (*first, 1000, 2000, 1000);
        pool.setSeekTargetRt (1000, 512);
        CHECK_FALSE (pool.seekReadyRt (1000, 512));
        REQUIRE (pool.pumpOnce());
        CHECK (pool.seekReadyRt (1000, 512));
    }
    SECTION ("Current playback is filled before a pending seek")
    {
        auto current = file.reader (513);
        auto target = file.reader (513);
        ReaderFillPool pool (3, false);
        pool.configure (1000.0, 0.0, 0.0);
        pool.add (*target, 30000, 40000, 10000);
        pool.add (*current, 0, 5000, 1000);
        pool.setPositionRt (1000);
        pool.setSeekTargetRt (30000, 512);
        REQUIRE (pool.pumpOnce());
        CHECK (current->hasResidency());
        CHECK_FALSE (target->hasResidency());
        CHECK_FALSE (pool.seekReadyRt (30000, 512));
        REQUIRE (pool.pumpOnce());
        CHECK (pool.seekReadyRt (30000, 512));
        CHECK (pool.stats().fillPasses == 2);
        Block block;
        REQUIRE (current->readRt (block.pointers.data(), kChannels, 2000, 512));
        block.check (2000);
    }
    SECTION ("Loop-head servicing and eviction continue while a seek waits")
    {
        auto loopHead = file.reader (129);
        auto target = file.reader();
        ReaderFillPool pool (3, false);
        pool.configure (1000.0);
        pool.add (*loopHead, 1000, 2000, 7000);
        pool.add (*target, 30000, 40000, 12000);
        pool.setLoopRt (true, 1000, 10000);
        pool.setPositionRt (9500);
        pool.setSeekTargetRt (30000, 512);
        REQUIRE (pool.pumpOnce());
        CHECK (loopHead->holds (7000, 128));
        CHECK_FALSE (pool.seekReadyRt (30000, 512));
        REQUIRE (pool.pumpOnce());
        CHECK (loopHead->holds (7000, 128));
        CHECK (pool.seekReadyRt (30000, 512));
        pool.setPositionRt (5000);
        REQUIRE (pool.pumpOnce());
        CHECK (loopHead->allocatedBytes() == 0);
        CHECK (target->holdsSeek (12000, 512));
        CHECK_FALSE (pool.pumpOnce());
        pool.clearSeekTargetRt();
        REQUIRE (pool.pumpOnce());
        CHECK (target->allocatedBytes() == 0);
    }
    SECTION ("A short seek read preserves current audio without acknowledging missing frames")
    {
        auto reader = file.reader();
        ReaderFillPool pool (3, false);
        pool.configure (1000.0);
        pool.add (*reader, 1000, 46000, 1000);
        pool.primeAt (1000);
        const auto headerBytes = std::filesystem::file_size (file.path) - kFrames * kChannels * sizeof (float);
        std::filesystem::resize_file (file.path, headerBytes + 33000 * kChannels * sizeof (float));
        pool.setSeekTargetRt (30000, 8192);
        REQUIRE (pool.pumpOnce());
        CHECK_FALSE (pool.seekReadyRt (30000, 8192));
        CHECK (reader->holdsSeek (30000, 3000));
        CHECK_FALSE (pool.pumpOnce());
        Block block;
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 1000, 512));
        block.check (1000);
    }
    SECTION ("Seek spans saturate at the timeline limit and normalize empty hints")
    {
        auto reader = file.reader (513);
        auto second = file.reader (513);
        ReaderFillPool pool (3, false);
        pool.configure (1000.0, 0.0, 0.0);
        const auto end = std::numeric_limits<std::int64_t>::max();
        pool.add (*reader, end - 1024, end - 256, 1000);
        pool.add (*second, end - 256, end, 30000);
        pool.setSeekTargetRt (end - 512, 8192);
        REQUIRE (pool.pumpOnce());
        CHECK_FALSE (pool.seekReadyRt (end - 512, 8192));
        REQUIRE (pool.pumpOnce());
        CHECK (pool.seekReadyRt (end - 512, 8192));
        Block block;
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 1512, 512));
        block.check (1512);
        REQUIRE (second->readRt (block.pointers.data(), kChannels, 30000, 512));
        block.check (30000);
        pool.reset();
        pool.add (*reader, 0, 1000, 1000);
        pool.setSeekTargetRt (-100, 0);
        REQUIRE (pool.pumpOnce());
        CHECK_FALSE (pool.seekReadyRt (0, 1));
        REQUIRE (pool.pumpOnce());
        CHECK (pool.seekReadyRt (0, 1));
        CHECK (pool.seekReadyRt (-500, -1));
        CHECK_FALSE (pool.seekReadyRt (0, 2));
        pool.setSeekTargetRt (-100, 0);
        CHECK (pool.seekReadyRt (0, 1));
    }
}

TEST_CASE ("Fill pool readiness reads only what the committed position plays",
           "[audiofile][reader-pool][seek]")
{
    SampleFile file;
    Block block;
    SECTION ("A seek committed before it was warm keeps filling the window it began")
    {
        auto reader = file.reader();
        ReaderFillPool pool (3, false);
        pool.configure (1000.0);
        pool.add (*reader, 1000, 46000, 1000);
        pool.primeAt (1000);
        pool.setSeekTargetRt (30000, 16384);
        REQUIRE (pool.pumpOnce());
        CHECK_FALSE (pool.seekReadyRt (30000, 16384));
        REQUIRE (reader->holdsSeek (30000, BufferedFileReader::kFillChunkFrames));
        // The locate limit commits it anyway.
        pool.setPositionRt (30000);
        pool.clearSeekTargetRt();
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 30000, 512));
        block.check (30000);
        const auto passes = pool.stats().fillPasses;
        REQUIRE (pool.pumpOnce());
        CHECK (pool.stats().fillPasses == passes + 1);
        CHECK (reader->holdsSeek (30000, 2 * BufferedFileReader::kFillChunkFrames));
        // Playback running on toward its end keeps it growing to a whole window.
        pool.setPositionRt (38000);
        while (pool.pumpOnce()) {}
        CHECK (reader->holdsSeek (30000, 20000));
    }
    SECTION ("A region that ends before the target is not filled for it")
    {
        auto behind = file.reader();
        auto required = file.reader();
        ReaderFillPool pool (3, false);
        pool.configure (1000.0, 3.0, 2.0);
        pool.add (*behind, 28500, 29500, 5000);
        pool.add (*required, 30000, 31000, 1000);
        pool.setPositionRt (100000);
        pool.setSeekTargetRt (30000, 512);
        REQUIRE (pool.pumpOnce());
        CHECK (pool.seekReadyRt (30000, 512));
        CHECK_FALSE (pool.pumpOnce());
        CHECK (behind->allocatedBytes() == 0);
        // Committed, the region is near but behind the playhead, and stays unfilled.
        pool.setPositionRt (30000);
        pool.clearSeekTargetRt();
        while (pool.pumpOnce()) {}
        CHECK (behind->allocatedBytes() == 0);
        CHECK (required->holds (1000, 1000));
    }
}

TEST_CASE ("Fill pool retains loop-start readers near a wrap only",
           "[audiofile][reader-pool]")
{
    SampleFile file;
    auto reader = file.reader (129);
    ReaderFillPool pool (3, false);
    pool.configure (1000.0);
    int regionLength = 1000;
    SECTION ("A full loop-head region") {}
    SECTION ("A one-frame loop-head region") { regionLength = 1; }
    pool.add (*reader, 1000, 1000 + regionLength, 7000);
    pool.setLoopRt (true, 1000, 10000);
    pool.setPositionRt (9500);
    REQUIRE (pool.pumpOnce());
    CHECK (reader->holds (7000, 128));
    pool.setPositionRt (5000);
    REQUIRE (pool.pumpOnce());
    CHECK (reader->allocatedBytes() == 0);
    pool.setLoopRt (false, 1000, 10000);
    pool.setPositionRt (9500);
    CHECK_FALSE (pool.pumpOnce());
}

TEST_CASE ("Fill pool stages rolling seeks without replacing current playback",
           "[audiofile][reader-pool][seek]")
{
    SampleFile file;
    auto reader = file.reader (20001);
    ReaderFillPool pool (3, false);
    pool.configure (1000.0);
    pool.add (*reader, 1000, 46000, 1000);
    pool.primeAt (1000);
    Block block;
    const auto checkCurrent = [&]
    {
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 1000, 512));
        block.check (1000);
    };
    checkCurrent();
    pool.setSeekTargetRt (30000, 512);
    CHECK_FALSE (pool.seekReadyRt (30000, 512));
    REQUIRE (pool.pumpOnce());
    checkCurrent();
    SECTION ("the target becomes ready once its warm span is published")
    {
        CHECK (pool.seekReadyRt (30000, 512));
        CHECK_FALSE (reader->holdsSeek (30000, BufferedFileReader::kFillChunkFrames + 1));
        while (pool.pumpOnce()) checkCurrent();
        REQUIRE (pool.seekReadyRt (30000, 512));
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 30000, 512));
        block.check (30000);
        pool.setPositionRt (30000);
        pool.clearSeekTargetRt();
        pool.setSeekTargetRt (5000, 512);
        while (pool.pumpOnce())
        {
            REQUIRE (reader->readRt (block.pointers.data(), kChannels, 30000, 512));
            block.check (30000);
        }
        REQUIRE (pool.seekReadyRt (5000, 512));
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 5000, 512));
        block.check (5000);
    }
    SECTION ("superseded targets keep only current and the newest pending window")
    {
        for (const auto target : { 35000, 26000, 32000 })
        {
            pool.setSeekTargetRt (target, 512);
            CHECK_FALSE (pool.seekReadyRt (target, 512));
            while (pool.pumpOnce()) checkCurrent();
            REQUIRE (pool.seekReadyRt (target, 512));
            CHECK_FALSE (pool.seekReadyRt (30000, 512));
            CHECK (reader->allocatedBytes() <= 2 * 20000 * kChannels * sizeof (float)
                                                + 8192 * kChannels * sizeof (float) + 1024);
        }
    }
    SECTION ("an overlapping seek preserves the whole current callback, not just its first frame")
    {
        pool.setSeekTargetRt (20200, 512);
        while (pool.pumpOnce()) {}
        REQUIRE (pool.seekReadyRt (20200, 512));
        pool.setPositionRt (20200);
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 20200, 512));
        block.check (20200);
        pool.setPositionRt (20712);
        pool.setSeekTargetRt (45000, 512);
        while (pool.pumpOnce())
        {
            REQUIRE (reader->readRt (block.pointers.data(), kChannels, 20712, 512));
            block.check (20712);
        }
        REQUIRE (pool.seekReadyRt (45000, 512));
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 20712, 512));
        block.check (20712);
    }
    SECTION ("cancelled targets lose their readiness acknowledgement")
    {
        while (pool.pumpOnce()) checkCurrent();
        REQUIRE (pool.seekReadyRt (30000, 512));
        pool.clearSeekTargetRt();
        CHECK_FALSE (pool.seekReadyRt (30000, 512));
        while (pool.pumpOnce()) checkCurrent();
    }
    SECTION ("an empty destination range can commit without reading audio")
    {
        pool.setSeekTargetRt (100000, 512);
        while (pool.pumpOnce()) checkCurrent();
        CHECK (pool.seekReadyRt (100000, 512));
    }
    SECTION ("explicit priming invalidates a seek whose residency it retires")
    {
        while (pool.pumpOnce()) checkCurrent();
        REQUIRE (pool.seekReadyRt (30000, 512));
        pool.primeAt (100000);
        CHECK_FALSE (reader->holdsSeek (30000, 512));
        CHECK_FALSE (pool.seekReadyRt (30000, 512));
    }
    SECTION ("an empty pool is ready without waking a worker")
    {
        pool.remove (*reader);
        CHECK (pool.seekReadyRt (30000, 512));
        pool.clearSeekTargetRt();
        pool.setSeekTargetRt (5000, 8192);
        CHECK (pool.seekReadyRt (5000, 8192));
        CHECK (reader->allocatedBytes() == 0);
        return;
    }
    pool.remove (*reader);
    CHECK (pool.stats().readers == 0);
    CHECK (reader->allocatedBytes() == 0);
    CHECK_FALSE (pool.pumpOnce());
}
