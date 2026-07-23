#include <catch2/catch_test_macros.hpp>

#include "engine/audiofile/BufferedFileReader.h"
#include "engine/audiofile/FileWriter.h"

#include <cmath>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

using namespace dusk::audio;

namespace
{
constexpr int      kChannels = 2;
constexpr std::int64_t kFrames = 16384;
constexpr std::int64_t kWindow = 4096;   // small enough that the window has to roll
constexpr std::int64_t kBlock  = 512;

float sample (int ch, std::int64_t f)
{
    return 0.9f * std::sin ((float) f * 0.002f + (float) ch * 1.7f);
}

std::filesystem::path tmp (const char* name)
{
    return std::filesystem::temp_directory_path() / name;
}

// Non-throwing, and only ever called once the readers are released: Windows
// refuses to unlink an open file, and the throwing overload would fail a run
// whose assertions all passed.
void discard (const std::filesystem::path& p)
{
    std::error_code ec;
    std::filesystem::remove (p, ec);
}

// 32-bit float WAV so residency comparisons are exact, not tolerance-based.
bool writeRamp (const std::filesystem::path& path)
{
    std::vector<std::vector<float>> ch ((size_t) kChannels, std::vector<float> ((size_t) kFrames));
    for (int c = 0; c < kChannels; ++c)
        for (std::int64_t f = 0; f < kFrames; ++f)
            ch[(size_t) c][(size_t) f] = sample (c, f);

    std::vector<const float*> src;
    for (auto& v : ch) src.push_back (v.data());

    WriteSpec spec;
    spec.sampleRate    = 48000.0;
    spec.numChannels   = kChannels;
    spec.bitsPerSample = 32;

    auto w = FileWriter::create (path, spec);
    return w != nullptr && w->write (src.data(), kChannels, kFrames);
}

struct Block
{
    std::vector<std::vector<float>> data;
    std::vector<float*>             ptrs;

    explicit Block (std::int64_t frames, float fill = -1.0f)
        : data ((size_t) kChannels, std::vector<float> ((size_t) frames, fill))
    {
        for (auto& v : data) ptrs.push_back (v.data());
    }

    bool isSilent() const
    {
        for (const auto& v : data)
            for (float f : v)
                if (f != 0.0f) return false;
        return true;
    }
};

bool matchesSource (const Block& b, std::int64_t startFrame, std::int64_t numFrames)
{
    for (int c = 0; c < kChannels; ++c)
        for (std::int64_t f = 0; f < numFrames; ++f)
            if (b.data[(size_t) c][(size_t) f] != sample (c, startFrame + f))
                return false;
    return true;
}

std::unique_ptr<BufferedFileReader> manualReader (const std::filesystem::path& path)
{
    auto raw = FileReader::open (path);
    if (raw == nullptr) return nullptr;
    return std::make_unique<BufferedFileReader> (std::move (raw), kWindow,
                                                  BufferedFileReader::Fill::Manual);
}
} // namespace

TEST_CASE ("BufferedFileReader reports silence for a span that is not resident",
           "[audiofile][buffered]")
{
    const auto path = tmp ("dusk_bfr_miss.wav");
    REQUIRE (writeRamp (path));

    auto r = manualReader (path);
    REQUIRE (r != nullptr);

    // Nothing has been filled, so every frame is a miss - and a miss must be
    // silence, not stale scratch.
    Block b (kBlock);
    REQUIRE_FALSE (r->readRt (b.ptrs.data(), kChannels, 0, kBlock));
    REQUIRE (b.isSilent());

    r.reset();
    discard (path);
}

TEST_CASE ("BufferedFileReader serves a resident span bit-exactly",
           "[audiofile][buffered]")
{
    const auto path = tmp ("dusk_bfr_resident.wav");
    REQUIRE (writeRamp (path));

    auto r = manualReader (path);
    REQUIRE (r != nullptr);
    REQUIRE (r->info().numFrames == kFrames);
    r->fillNow();

    Block b (kBlock);
    REQUIRE (r->readRt (b.ptrs.data(), kChannels, 0, kBlock));
    REQUIRE (matchesSource (b, 0, kBlock));

    // Same bytes an unbuffered read of the same span produces.
    auto raw = FileReader::open (path);
    REQUIRE (raw != nullptr);
    Block direct (kBlock);
    REQUIRE (raw->read (direct.ptrs.data(), kChannels, 0, kBlock) == kBlock);
    for (int c = 0; c < kChannels; ++c)
        for (std::int64_t f = 0; f < kBlock; ++f)
            REQUIRE (b.data[(size_t) c][(size_t) f] == direct.data[(size_t) c][(size_t) f]);

    raw.reset();
    r.reset();
    discard (path);
}

TEST_CASE ("BufferedFileReader window rolls forward past its own capacity",
           "[audiofile][buffered]")
{
    const auto path = tmp ("dusk_bfr_roll.wav");
    REQUIRE (writeRamp (path));

    auto r = manualReader (path);
    REQUIRE (r != nullptr);

    // Play the whole file in blocks, topping the window up between them the way
    // the worker would. Every block must be fully resident even though the file
    // is four windows long.
    for (std::int64_t pos = 0; pos + kBlock <= kFrames; pos += kBlock)
    {
        r->prefetch (pos);
        r->fillNow();

        Block b (kBlock);
        REQUIRE (r->readRt (b.ptrs.data(), kChannels, pos, kBlock));
        REQUIRE (matchesSource (b, pos, kBlock));
    }

    // Having played to the end, a jump back to the top is a miss again until
    // the window is refilled - forward prefetch cannot cover a backward seek,
    // which is why the loop pre-cache in PlaybackEngine exists.
    Block back (kBlock);
    REQUIRE_FALSE (r->readRt (back.ptrs.data(), kChannels, 0, kBlock));
    REQUIRE (back.isSilent());

    r.reset();
    discard (path);
}

TEST_CASE ("BufferedFileReader prefetch warms a span the audio thread has not asked for",
           "[audiofile][buffered]")
{
    const auto path = tmp ("dusk_bfr_prefetch.wav");
    REQUIRE (writeRamp (path));

    auto r = manualReader (path);
    REQUIRE (r != nullptr);

    const std::int64_t far = kFrames - kWindow;
    r->prefetch (far);
    r->fillNow();

    Block warm (kBlock);
    REQUIRE (r->readRt (warm.ptrs.data(), kChannels, far, kBlock));
    REQUIRE (matchesSource (warm, far, kBlock));

    // Backwards, over a span the window has already left behind.
    r->prefetch (0);
    r->fillNow();

    Block rewound (kBlock);
    REQUIRE (r->readRt (rewound.ptrs.data(), kChannels, 0, kBlock));
    REQUIRE (matchesSource (rewound, 0, kBlock));

    r.reset();
    discard (path);
}

TEST_CASE ("BufferedFileReader treats frames past the end of the file as silence",
           "[audiofile][buffered]")
{
    const auto path = tmp ("dusk_bfr_eof.wav");
    REQUIRE (writeRamp (path));

    auto r = manualReader (path);
    REQUIRE (r != nullptr);

    const std::int64_t tail = kFrames - 100;
    r->prefetch (tail);
    r->fillNow();

    // Straddling EOF: the file's last 100 frames, then silence, and not a miss.
    Block b (kBlock);
    REQUIRE (r->readRt (b.ptrs.data(), kChannels, tail, kBlock));
    REQUIRE (matchesSource (b, tail, 100));
    for (int c = 0; c < kChannels; ++c)
        for (std::int64_t f = 100; f < kBlock; ++f)
            REQUIRE (b.data[(size_t) c][(size_t) f] == 0.0f);

    Block past (kBlock);
    REQUIRE (r->readRt (past.ptrs.data(), kChannels, kFrames + 1000, kBlock));
    REQUIRE (past.isSilent());

    r.reset();
    discard (path);
}

TEST_CASE ("BufferedFileReader zero-fills destination channels beyond the file",
           "[audiofile][buffered]")
{
    const auto path = tmp ("dusk_bfr_mono.wav");
    std::vector<float> mono ((size_t) kFrames);
    for (std::int64_t f = 0; f < kFrames; ++f) mono[(size_t) f] = sample (0, f);
    const float* src = mono.data();

    WriteSpec spec;
    spec.sampleRate    = 48000.0;
    spec.numChannels   = 1;
    spec.bitsPerSample = 32;
    {
        auto w = FileWriter::create (path, spec);
        REQUIRE (w != nullptr);
        REQUIRE (w->write (&src, 1, kFrames));
    }

    auto r = manualReader (path);
    REQUIRE (r != nullptr);
    r->fillNow();

    Block b (kBlock);
    REQUIRE (r->readRt (b.ptrs.data(), kChannels, 0, kBlock));
    for (std::int64_t f = 0; f < kBlock; ++f)
    {
        REQUIRE (b.data[0][(size_t) f] == sample (0, f));
        REQUIRE (b.data[1][(size_t) f] == 0.0f);
    }

    r.reset();
    discard (path);
}

TEST_CASE ("BufferedFileReader background worker fills and joins",
           "[audiofile][buffered]")
{
    const auto path = tmp ("dusk_bfr_worker.wav");
    REQUIRE (writeRamp (path));

    auto raw = FileReader::open (path);
    REQUIRE (raw != nullptr);

    // Residency is a race here by construction, so assert only what holds
    // either way: the read is silent-or-correct and the worker joins cleanly.
    auto r = std::make_unique<BufferedFileReader> (std::move (raw), kWindow);
    Block b (kBlock);
    if (r->readRt (b.ptrs.data(), kChannels, 0, kBlock))
        REQUIRE (matchesSource (b, 0, kBlock));

    r.reset();
    discard (path);
}
