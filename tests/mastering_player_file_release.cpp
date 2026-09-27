#include <catch2/catch_test_macros.hpp>

#include "TestOpenHandles.h"
#include "TestTempDirectory.h"
#include "engine/MasteringPlayer.h"
#include "engine/audiofile/FileWriter.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <vector>

using namespace duskstudio;
using test::canCountOpenHandles;
using test::openHandlesTo;
using test::renamedAway;

namespace
{
constexpr int kBlock = 512;

std::filesystem::path writeWav (const std::filesystem::path& path, double sampleRate,
                                std::int64_t frames)
{
    auto writer = dusk::audio::FileWriter::create (path, { sampleRate, 2, 24 });
    REQUIRE (writer != nullptr);
    std::vector<float> left ((size_t) frames, 0.25f), right ((size_t) frames, -0.25f);
    const float* channels[] { left.data(), right.data() };
    REQUIRE (writer->write (channels, 2, frames));
    writer.reset();
    return std::filesystem::canonical (path);
}

juce::File toFile (const std::filesystem::path& path)
{
    return juce::File (juce::String::fromUTF8 (path.u8string().c_str()));
}

void runBlocks (MasteringPlayer& player, int blocks)
{
    std::vector<float> left ((size_t) kBlock), right ((size_t) kBlock);
    for (int b = 0; b < blocks; ++b)
        player.process (left.data(), right.data(), kBlock);
}
} // namespace

TEST_CASE ("MasteringPlayer closes its file as soon as it is unloaded", "[mastering]")
{
    const test::TempDirectory dir ("dusk-mastering-release");
    const auto wav = writeWav (dir.path() / "mix.wav", 48000.0, 48000);

    SECTION ("with no audio callbacks running")
    {
        MasteringPlayer player;
        player.prepare (kBlock, 48000.0);
        REQUIRE (player.loadFile (toFile (wav)));
        if (canCountOpenHandles())
            REQUIRE (openHandlesTo (wav) == 1);

        player.unloadFile();

        CHECK_FALSE (player.isLoaded());
        CHECK (openHandlesTo (wav) == 0);
        CHECK (renamedAway (wav));
    }

    SECTION ("after it played")
    {
        MasteringPlayer player;
        player.prepare (kBlock, 48000.0);
        REQUIRE (player.loadFile (toFile (wav)));
        player.play();
        runBlocks (player, 8);
        REQUIRE (player.isPlaying());

        player.unloadFile();

        CHECK_FALSE (player.isPlaying());
        CHECK (openHandlesTo (wav) == 0);
        CHECK (renamedAway (wav));
    }

    SECTION ("when the player is destroyed while loaded")
    {
        {
            MasteringPlayer player;
            player.prepare (kBlock, 48000.0);
            REQUIRE (player.loadFile (toFile (wav)));
            player.play();
            runBlocks (player, 2);
        }
        CHECK (openHandlesTo (wav) == 0);
        CHECK (renamedAway (wav));
    }
}

TEST_CASE ("MasteringPlayer closes the old file when a load replaces it or fails", "[mastering]")
{
    const test::TempDirectory dir ("dusk-mastering-replace");
    const auto first  = writeWav (dir.path() / "first.wav", 48000.0, 48000);
    const auto second = writeWav (dir.path() / "second.wav", 44100.0, 44100);

    MasteringPlayer player;
    player.prepare (kBlock, 48000.0);
    REQUIRE (player.loadFile (toFile (first)));

    SECTION ("a load of another file")
    {
        REQUIRE (player.loadFile (toFile (second)));
        CHECK (openHandlesTo (first) == 0);
        if (canCountOpenHandles())
            CHECK (openHandlesTo (second) == 1);
        CHECK (renamedAway (first));
    }

    SECTION ("a load that fails")
    {
        CHECK_FALSE (player.loadFile (toFile (dir.path() / "missing.wav")));
        CHECK_FALSE (player.isLoaded());
        CHECK (openHandlesTo (first) == 0);
        CHECK (renamedAway (first));
    }

    SECTION ("rapid unload and load cycles")
    {
        for (int cycle = 0; cycle < 32; ++cycle)
        {
            const auto& current = (cycle % 2 == 0) ? second : first;
            player.unloadFile();
            REQUIRE (openHandlesTo (first) + openHandlesTo (second) == 0);
            REQUIRE (player.loadFile (toFile (current)));
            player.play();
            runBlocks (player, 1);
        }
        player.unloadFile();
        CHECK (openHandlesTo (first) + openHandlesTo (second) == 0);
        CHECK (renamedAway (first));
        CHECK (renamedAway (second));
    }
}

// The Mastering waveform follows the player through this counter, so it has to
// move on every change a view must redraw for, a reload of the same file too.
TEST_CASE ("MasteringPlayer changes its source generation on every load and unload", "[mastering]")
{
    const test::TempDirectory dir ("dusk-mastering-generation");
    const auto wav = writeWav (dir.path() / "mix.wav", 48000.0, 4800);

    MasteringPlayer player;
    player.prepare (kBlock, 48000.0);
    auto last = player.getSourceGeneration();
    const auto changed = [&player, &last]
    {
        const auto now = player.getSourceGeneration();
        const bool moved = now != last;
        last = now;
        return moved;
    };

    REQUIRE (player.loadFile (toFile (wav)));
    CHECK (changed());
    REQUIRE (player.loadFile (toFile (wav)));
    CHECK (changed());
    player.unloadFile();
    CHECK (changed());
    CHECK_FALSE (player.loadFile (toFile (dir.path() / "missing.wav")));
    CHECK (changed());
    CHECK_FALSE (changed());
}

// The audio thread may be inside process() with the reader latched when the
// message thread unloads, so the close has to wait for that block to leave and
// never happen under it. Under TSan this proves the destruction is ordered after
// every read the audio thread made through the old reader.
//
// Nothing here depends on how fast the audio thread runs. A drain that times out
// (a starved thread under a loaded TSan run) leaves the reader retired, which
// the next successful drain frees, and the load behind it may be refused; the
// test tolerates both and checks the eventual close once the thread is gone.
// That each unload closes at once is covered by the single-threaded cases.
TEST_CASE ("MasteringPlayer closes unloaded files while an audio thread plays", "[mastering]")
{
    const test::TempDirectory dir ("dusk-mastering-concurrent");
    const auto exact     = writeWav (dir.path() / "exact.wav", 48000.0, 480000);
    const auto resampled = writeWav (dir.path() / "resampled.wav", 44100.0, 441000);

    MasteringPlayer player;
    player.prepare (kBlock, 48000.0);

    std::atomic<bool> stopAudio { false };
    // Relaxed on purpose: if this pacing counter ordered the audio thread's
    // reads before the unload, TSan could no longer tell whether the
    // player's own drain does.
    std::atomic<std::uint64_t> blocks { 0 };
    std::thread audioThread ([&]
    {
        std::vector<float> left ((size_t) kBlock), right ((size_t) kBlock);
        while (! stopAudio.load (std::memory_order_acquire))
        {
            player.process (left.data(), right.data(), kBlock);
            blocks.fetch_add (1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    const auto waitForBlocks = [&blocks] (std::uint64_t count)
    {
        const auto target = blocks.load (std::memory_order_relaxed) + count;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (10);
        while (blocks.load (std::memory_order_relaxed) < target)
        {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::yield();
        }
        return true;
    };

    int loadedCycles = 0;
    for (int cycle = 0; cycle < 64; ++cycle)
    {
        const auto& source = (cycle % 2 == 0) ? exact : resampled;
        if (! player.loadFile (toFile (source))) continue;
        ++loadedCycles;
        player.play();
        const bool audioRan = waitForBlocks (2);
        player.unloadFile();
        if (! audioRan) break;
    }

    stopAudio.store (true, std::memory_order_release);
    audioThread.join();
    player.unloadFile();

    CHECK (loadedCycles > 0);
    CHECK (openHandlesTo (exact) + openHandlesTo (resampled) == 0);
    CHECK (renamedAway (exact));
    CHECK (renamedAway (resampled));
}
