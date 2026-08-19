#include <catch2/catch_test_macros.hpp>

#include "../src/foundation/SpscIndexFifo.h"

#include <array>
#include <atomic>
#include <thread>
#include <vector>

TEST_CASE ("SpscIndexFifo uses a sentinel slot and clamps requests", "[foundation][spsc]")
{
    dusk::SpscIndexFifo fifo (4);
    REQUIRE (fifo.getFreeSpace() == 3);
    REQUIRE (fifo.getNumReady() == 0);

    int start1 = -1, size1 = -1, start2 = -1, size2 = -1;
    fifo.prepareToWrite (0, start1, size1, start2, size2);
    REQUIRE (start1 == 0);
    REQUIRE (size1 == 0);
    REQUIRE (start2 == 0);
    REQUIRE (size2 == 0);

    fifo.prepareToWrite (99, start1, size1, start2, size2);
    REQUIRE (size1 + size2 == 3);
    fifo.finishedWrite (size1 + size2);
    REQUIRE (fifo.getFreeSpace() == 0);
    REQUIRE (fifo.getNumReady() == 3);
    REQUIRE (fifo.getFreeSpace() + fifo.getNumReady() == 3);

    fifo.prepareToWrite (1, start1, size1, start2, size2);
    REQUIRE (size1 == 0);
    REQUIRE (size2 == 0);
    fifo.prepareToRead (99, start1, size1, start2, size2);
    REQUIRE (size1 + size2 == 3);
    fifo.finishedRead (size1 + size2);
    REQUIRE (fifo.getFreeSpace() == 3);
    REQUIRE (fifo.getNumReady() == 0);
}

TEST_CASE ("SpscIndexFifo splits wrapped writes and reads", "[foundation][spsc]")
{
    dusk::SpscIndexFifo fifo (5);
    std::array<int, 5> values {};

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToWrite (3, start1, size1, start2, size2);
    REQUIRE (start1 == 0);
    REQUIRE (size1 == 3);
    REQUIRE (size2 == 0);
    for (int i = 0; i < size1; ++i) values[(size_t) (start1 + i)] = i;
    fifo.finishedWrite (3);

    fifo.prepareToRead (2, start1, size1, start2, size2);
    REQUIRE (start1 == 0);
    REQUIRE (size1 == 2);
    REQUIRE (size2 == 0);
    fifo.finishedRead (2);

    fifo.prepareToWrite (3, start1, size1, start2, size2);
    REQUIRE (start1 == 3);
    REQUIRE (size1 == 2);
    REQUIRE (start2 == 0);
    REQUIRE (size2 == 1);
    values[3] = 3;
    values[4] = 4;
    values[0] = 5;
    fifo.finishedWrite (3);

    fifo.prepareToRead (4, start1, size1, start2, size2);
    REQUIRE (start1 == 2);
    REQUIRE (size1 == 3);
    REQUIRE (start2 == 0);
    REQUIRE (size2 == 1);
    REQUIRE (values[2] == 2);
    REQUIRE (values[3] == 3);
    REQUIRE (values[4] == 4);
    REQUIRE (values[0] == 5);
    fifo.finishedRead (4);
    REQUIRE (fifo.getFreeSpace() == 4);
    REQUIRE (fifo.getNumReady() == 0);
}

TEST_CASE ("SpscIndexFifo supports interleaved operations and reset", "[foundation][spsc]")
{
    dusk::SpscIndexFifo fifo (3);
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;

    for (int i = 0; i < 100; ++i)
    {
        fifo.prepareToWrite (1, start1, size1, start2, size2);
        REQUIRE (size1 + size2 == 1);
        fifo.finishedWrite (1);
        REQUIRE (fifo.getFreeSpace() + fifo.getNumReady() == 2);

        fifo.prepareToRead (1, start1, size1, start2, size2);
        REQUIRE (size1 + size2 == 1);
        fifo.finishedRead (1);
        REQUIRE (fifo.getFreeSpace() + fifo.getNumReady() == 2);
    }

    fifo.prepareToWrite (2, start1, size1, start2, size2);
    REQUIRE (size1 + size2 == 2);
    fifo.finishedWrite (2);
    fifo.reset();
    REQUIRE (fifo.getFreeSpace() == 2);
    REQUIRE (fifo.getNumReady() == 0);
    fifo.prepareToRead (1, start1, size1, start2, size2);
    REQUIRE (size1 == 0);
    REQUIRE (size2 == 0);
}

TEST_CASE ("SpscIndexFifo preserves order under SPSC stress", "[foundation][spsc]")
{
    SECTION ("bounded producer and consumer")
    {
        constexpr int kItemCount = 100000;
        constexpr int kCapacity = 1024;
        constexpr int kMaxIdleSpins = 5000000;
        dusk::SpscIndexFifo fifo (kCapacity);
        std::vector<int> ring ((size_t) kCapacity);
        std::vector<int> consumed ((size_t) kItemCount);
        std::atomic<bool> failed { false };

        std::thread producer ([&]
        {
            int next = 0;
            int idleSpins = 0;
            while (next < kItemCount && ! failed.load (std::memory_order_relaxed))
            {
                int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
                fifo.prepareToWrite (1, start1, size1, start2, size2);
                if (size1 + size2 == 0)
                {
                    if (++idleSpins > kMaxIdleSpins)
                    {
                        failed.store (true, std::memory_order_relaxed);
                        break;
                    }
                    std::this_thread::yield();
                    continue;
                }

                const int slot = size1 > 0 ? start1 : start2;
                ring[(size_t) slot] = next++;
                fifo.finishedWrite (1);
                idleSpins = 0;
            }
        });

        std::thread consumer ([&]
        {
            int count = 0;
            int idleSpins = 0;
            while (count < kItemCount && ! failed.load (std::memory_order_relaxed))
            {
                int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
                fifo.prepareToRead (1, start1, size1, start2, size2);
                if (size1 + size2 == 0)
                {
                    if (++idleSpins > kMaxIdleSpins)
                    {
                        failed.store (true, std::memory_order_relaxed);
                        break;
                    }
                    std::this_thread::yield();
                    continue;
                }

                const int slot = size1 > 0 ? start1 : start2;
                consumed[(size_t) count++] = ring[(size_t) slot];
                fifo.finishedRead (1);
                idleSpins = 0;
            }
        });

        producer.join();
        consumer.join();
        REQUIRE_FALSE (failed.load (std::memory_order_relaxed));
        REQUIRE (fifo.getNumReady() == 0);
        REQUIRE (consumed.size() == (size_t) kItemCount);
        for (int i = 0; i < kItemCount; ++i)
            REQUIRE (consumed[(size_t) i] == i);
    }
}
