#include <catch2/catch_test_macros.hpp>

#include "engine/multisample/NativeMultisampleSlot.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace
{
std::filesystem::path createUniqueTempDirectory()
{
    std::random_device random;
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        const auto nonce = (std::uint64_t (random()) << 32) ^ std::uint64_t (random());
        const auto candidate = std::filesystem::temp_directory_path()
                             / ("dusk-multisample-slot-" + std::to_string (nonce));
        std::error_code error;
        if (std::filesystem::create_directory (candidate, error))
            return candidate;
    }
    return {};
}

struct ScopedPathCleanup
{
    std::filesystem::path path;
    ~ScopedPathCleanup()
    {
        std::error_code ignored;
        std::filesystem::remove_all (path, ignored);
    }
};

float peakAfterNoteOn (duskstudio::NativeMultisampleSlot& slot, int blockSize)
{
    std::vector<float> left ((std::size_t) blockSize), right ((std::size_t) blockSize);
    dusk::MidiBuffer noteOn;
    noteOn.reserveBytes (64);
    const std::uint8_t down[] { 0x90, 60, 100 };
    REQUIRE (noteOn.addEvent (down, 3, 0));
    float peak = 0.0f;
    for (int block = 0; block < 8; ++block)
    {
        std::fill (left.begin(), left.end(), 0.0f);
        std::fill (right.begin(), right.end(), 0.0f);
        slot.processStereo (left.data(), right.data(), left.data(), right.data(), blockSize,
                            block == 0 ? &noteOn : nullptr);
        for (std::size_t i = 0; i < left.size(); ++i)
            peak = std::max ({ peak, std::abs (left[i]), std::abs (right[i]) });
    }
    return peak;
}
} // namespace

// Every soundfont the app loads while audio runs arrives through prime() and
// commit(), so a commit alone has to bring the slot online, with no load() or
// reactivate() before it to publish the frame limit.
TEST_CASE ("A soundfont committed into a fresh multisample slot plays its first note",
           "[multisample][slot]")
{
    const ScopedPathCleanup cleanup { createUniqueTempDirectory() };
    REQUIRE_FALSE (cleanup.path.empty());
    const auto soundfont = cleanup.path / "commit.sfz";
    {
        std::ofstream file (soundfont);
        file << "<region> key=60 sample=*sine\n";
        REQUIRE (file.good());
    }

    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 256;
    std::string error;
    auto primed = duskstudio::NativeMultisampleSlot::prime (soundfont, sampleRate, blockSize, error);
    INFO (error);
    REQUIRE (primed);

    duskstudio::NativeMultisampleSlot slot;
    REQUIRE (slot.commit (primed, blockSize));
    REQUIRE (slot.isLoaded());
    CHECK (peakAfterNoteOn (slot, blockSize) > 1.0e-3f);
}
