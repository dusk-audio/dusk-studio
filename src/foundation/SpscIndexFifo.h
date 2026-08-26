#pragma once

#include <algorithm>
#include <atomic>

namespace dusk
{
// Index-only SPSC FIFO with the same observable contract as JUCE's
// AbstractFifo: capacity N holds N - 1 usable slots (one sentinel), requests
// clamp to what fits, and spans wrap into two blocks. Callers own the element
// storage. One producer thread uses the write methods, one consumer thread the
// read methods; reset/setCapacity are not thread-safe against either.
class SpscIndexFifo
{
public:
    explicit SpscIndexFifo (int capacity) noexcept : totalSize (capacity) {}

    void setCapacity (int newCapacity) noexcept
    {
        totalSize = newCapacity;
        reset();
    }

    int getFreeSpace() const noexcept
    {
        const int read = readPosition.load (std::memory_order_acquire);
        const int write = writePosition.load (std::memory_order_relaxed);
        return freeSpace (read, write) - 1;
    }

    int getNumReady() const noexcept
    {
        const int read = readPosition.load (std::memory_order_relaxed);
        const int write = writePosition.load (std::memory_order_acquire);
        return distance (read, write);
    }

    void reset() noexcept
    {
        writePosition.store (0, std::memory_order_relaxed);
        readPosition.store (0, std::memory_order_relaxed);
    }

    void prepareToWrite (int numToWrite, int& startIndex1, int& blockSize1,
                         int& startIndex2, int& blockSize2) const noexcept
    {
        const int read = readPosition.load (std::memory_order_acquire);
        const int write = writePosition.load (std::memory_order_relaxed);
        numToWrite = std::min (numToWrite, freeSpace (read, write) - 1);

        if (numToWrite <= 0)
        {
            startIndex1 = 0;
            blockSize1 = 0;
            startIndex2 = 0;
            blockSize2 = 0;
            return;
        }

        startIndex1 = write;
        startIndex2 = 0;
        blockSize1 = std::min (totalSize - write, numToWrite);
        numToWrite -= blockSize1;
        blockSize2 = numToWrite <= 0 ? 0 : std::min (numToWrite, read);
    }

    void finishedWrite (int numWritten) noexcept
    {
        int newWrite = writePosition.load (std::memory_order_relaxed) + numWritten;
        if (newWrite >= totalSize)
            newWrite -= totalSize;
        writePosition.store (newWrite, std::memory_order_release);
    }

    void prepareToRead (int numWanted, int& startIndex1, int& blockSize1,
                        int& startIndex2, int& blockSize2) const noexcept
    {
        const int read = readPosition.load (std::memory_order_relaxed);
        const int write = writePosition.load (std::memory_order_acquire);
        numWanted = std::min (numWanted, distance (read, write));

        if (numWanted <= 0)
        {
            startIndex1 = 0;
            blockSize1 = 0;
            startIndex2 = 0;
            blockSize2 = 0;
            return;
        }

        startIndex1 = read;
        startIndex2 = 0;
        blockSize1 = std::min (totalSize - read, numWanted);
        numWanted -= blockSize1;
        blockSize2 = numWanted <= 0 ? 0 : std::min (numWanted, write);
    }

    void finishedRead (int numRead) noexcept
    {
        int newRead = readPosition.load (std::memory_order_relaxed) + numRead;
        if (newRead >= totalSize)
            newRead -= totalSize;
        readPosition.store (newRead, std::memory_order_release);
    }

private:
    int distance (int start, int end) const noexcept
    {
        return end >= start ? end - start : totalSize - (start - end);
    }

    int freeSpace (int read, int write) const noexcept
    {
        return write >= read ? totalSize - (write - read) : read - write;
    }

    int totalSize;
    std::atomic<int> readPosition { 0 };
    std::atomic<int> writePosition { 0 };
};
} // namespace dusk
