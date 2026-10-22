#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/audiofile/BufferedFileReader.h"
#include "engine/audiofile/FileWriter.h"
#include "engine/audiofile/ReaderFillPool.h"
#include "TestTempDirectory.h"

#include <array>
#include <filesystem>
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

    SampleFile()
    {
        std::array<std::vector<float>, kChannels> data;
        std::array<const float*, kChannels> pointers;
        for (int c = 0; c < kChannels; ++c)
        {
            data[(std::size_t) c].resize ((std::size_t) kFrames);
            for (std::int64_t f = 0; f < kFrames; ++f)
                data[(std::size_t) c][(std::size_t) f] = sample (c, f);
            pointers[(std::size_t) c] = data[(std::size_t) c].data();
        }
        WriteSpec spec;
        spec.sampleRate = 48000.0;
        spec.numChannels = kChannels;
        spec.bitsPerSample = 32;
        auto writer = FileWriter::create (path, spec);
        REQUIRE (writer != nullptr);
        REQUIRE (writer->write (pointers.data(), kChannels, kFrames));
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

    void check (std::int64_t start, std::int64_t count = 512) const
    {
        for (int c = 0; c < kChannels; ++c)
            for (std::int64_t f = 0; f < 512; ++f)
                REQUIRE_THAT (data[(std::size_t) c][(std::size_t) f],
                              WithinAbs (f < count ? sample (c, start + f) : 0.0f, 1.0e-7f));
    }
};
} // namespace

TEST_CASE ("Manual reader allocates lazily and publishes after bounded fill passes",
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
    CHECK_FALSE (reader->holds (0, 512));
    REQUIRE (reader->fillOnce());
    CHECK_FALSE (reader->hasResidency());
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
    SECTION ("A complete replacement stays unpublished until filled")
    {
        for (int pass = 0; pass < 2; ++pass)
        {
            reader->hintRt (22000);
            REQUIRE (reader->fillOnce());
            CHECK_FALSE (reader->holds (22000, 512));
            REQUIRE (reader->readRt (block.pointers.data(), kChannels, 0, 512));
            block.check (0);
        }
        reader->hintRt (22000);
        REQUIRE (reader->fillOnce());
        CHECK_FALSE (reader->holds (0, 512));
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 22000, 512));
        block.check (22000);
    }
    SECTION ("A short read publishes its valid prefix and retains the channel stride")
    {
        const auto headerBytes = std::filesystem::file_size (file.path) - kFrames * kChannels * sizeof (float);
        std::filesystem::resize_file (file.path, headerBytes + 24000 * kChannels * sizeof (float));
        reader->hintRt (22000);
        CHECK_FALSE (reader->fillOnce());
        REQUIRE (reader->readRt (block.pointers.data(), kChannels, 22000, 512));
        block.check (22000);
        CHECK_FALSE (reader->holds (24000, 1));
        CHECK_FALSE (reader->readRt (block.pointers.data(), kChannels, 23800, 512));
        block.check (23800, 200);
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
    CHECK_FALSE (pool.seekReadyRt (30000, 512));
    checkCurrent();
    SECTION ("the target becomes ready only after its complete window is published")
    {
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
