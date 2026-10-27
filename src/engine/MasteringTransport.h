#pragma once

#include "Transport.h"
#include <atomic>
#include <cstdint>
#include <limits>

namespace duskstudio
{
class MasteringTransport
{
public:
    struct Block
    {
        std::int64_t position = 0;
        std::uint32_t locateSerial = 0;
        std::uint64_t command = 0;
        bool playing = false;
    };

    struct Seek
    {
        std::uint64_t command;
        bool playing;
    };

    void play() noexcept { publishRun (true); }
    void stop() noexcept { publishRun (false); }
    bool isPlaying() const noexcept { return playingFor (runCommand.load (std::memory_order_acquire)); }
    std::int64_t getPlayhead() const noexcept { return cursor.getPlayhead(); }

    Seek beginSeek (std::int64_t position) noexcept
    {
        const auto command = runCommand.load (std::memory_order_acquire);
        const Seek seek { command, playingFor (command) };
        cursor.requestLocate (position);
        return seek;
    }

    void finishSeek (const Seek& seek) noexcept
    {
        auto expected = seek.command;
        for (;;)
        {
            // Preserve a newer explicit Play/Stop, while an older EOF cannot
            // change the run intent captured before this seek was published.
            const bool playing = expected == seek.command ? seek.playing : (expected & kPlaying) != 0;
            const auto desired = nextCommand (expected, playing);
            if (runCommand.compare_exchange_weak (expected, desired,
                                                  std::memory_order_acq_rel, std::memory_order_acquire))
                return;
        }
    }

    Block beginBlock() noexcept
    {
        const auto command = runCommand.load (std::memory_order_acquire);
        const bool playing = playingFor (command);
        const auto position = playing ? cursor.beginAudioBlock() : cursor.getCommittedPlayhead();
        return { position, cursor.getLocateCount(), command, playing };
    }

    void finishBlock (const Block& block, std::int64_t next, bool ended) noexcept
    {
        cursor.finishAudioBlock (next);
        if (ended) endedCommand.store (block.command, std::memory_order_release);
    }

private:
    static constexpr std::uint64_t kPlaying = 1;
    static std::uint64_t nextCommand (std::uint64_t command, bool playing) noexcept
    {
        return ((command + 2) & ~kPlaying) | (playing ? kPlaying : 0);
    }

    bool playingFor (std::uint64_t command) const noexcept
    {
        return (command & kPlaying) != 0
            && endedCommand.load (std::memory_order_acquire) != command;
    }

    void publishRun (bool playing) noexcept
    {
        auto expected = runCommand.load (std::memory_order_acquire);
        while (! runCommand.compare_exchange_weak (expected, nextCommand (expected, playing),
                                                   std::memory_order_acq_rel, std::memory_order_acquire)) {}
    }

    Transport cursor;
    std::atomic<std::uint64_t> runCommand { 0 };
    std::atomic<std::uint64_t> endedCommand { std::numeric_limits<std::uint64_t>::max() };
};
} // namespace duskstudio
