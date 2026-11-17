#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/audiofile/FileReader.h"
#include "engine/audiofile/FileWriter.h"
#include "engine/audiofile/IFileWriteSink.h"
#include "engine/audiofile/ThreadedFileWriter.h"
#include "engine/audiofile/WriterDrainPool.h"
#include "support/ThreadWakeups.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <system_error>
#include <thread>
#include <vector>

using namespace dusk::audio;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr int      kChannels = 2;
constexpr int      kFifo     = 16384;

float sample (int ch, std::int64_t f)
{
    return 0.8f * std::sin ((float) f * 0.003f + (float) ch * 1.1f);
}

std::filesystem::path tmp (const char* name)
{
    return std::filesystem::temp_directory_path() / name;
}

void discard (const std::filesystem::path& p)
{
    std::error_code ec;
    std::filesystem::remove (p, ec);
}

std::unique_ptr<ThreadedFileWriter> makeWavWriter (const std::filesystem::path& path)
{
    WriteSpec spec;
    spec.sampleRate    = 48000.0;
    spec.numChannels   = kChannels;
    spec.bitsPerSample = 32;   // float: bit-exact round-trip
    auto fw = FileWriter::create (path, spec);
    if (fw == nullptr) return nullptr;
    return std::make_unique<ThreadedFileWriter> (std::move (fw), kFifo,
                                                 ThreadedFileWriter::Drain::External);
}

// Pushes a ramp of `frames` to a writer, waiting for ring space (the pool
// drains concurrently) up to a deadline. Returns false if it could not place
// every frame - a genuine overflow the pool never relieved, i.e. a bug.
bool pushRamp (ThreadedFileWriter& tw, std::int64_t frames, std::int64_t block)
{
    std::vector<float> l ((size_t) block), r ((size_t) block);
    for (std::int64_t off = 0; off < frames; off += block)
    {
        const std::int64_t n = std::min (block, frames - off);
        for (std::int64_t f = 0; f < n; ++f)
        {
            l[(size_t) f] = sample (0, off + f);
            r[(size_t) f] = sample (1, off + f);
        }
        const float* p[kChannels] = { l.data(), r.data() };

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (5);
        while (! tw.push (p, kChannels, n))
        {
            if (! tw.isValid() || std::chrono::steady_clock::now() > deadline)
                return false;
            std::this_thread::yield();
        }
    }
    return true;
}

bool fileMatchesRamp (const std::filesystem::path& path, std::int64_t frames)
{
    auto r = FileReader::open (path);
    if (r == nullptr || r->info().numFrames != frames) return false;

    std::vector<std::vector<float>> out ((size_t) kChannels,
                                         std::vector<float> ((size_t) frames, 0.0f));
    std::vector<float*> outP;
    for (auto& v : out) outP.push_back (v.data());
    if (r->read (outP.data(), kChannels, 0, frames) != frames) return false;

    for (int c = 0; c < kChannels; ++c)
        for (std::int64_t f = 0; f < frames; ++f)
            if (std::abs (out[(size_t) c][(size_t) f] - sample (c, f)) > 1.0e-6f)
                return false;
    return true;
}
} // namespace

TEST_CASE ("WriterDrainPool drains several writers to disk bit-exactly",
           "[audiofile][pool]")
{
    constexpr int kWriters = 3;
    constexpr std::int64_t kFrames = 40000;   // several ring-fulls per writer

    WriterDrainPool pool (kWriters);
    std::vector<std::filesystem::path> paths;
    std::vector<std::unique_ptr<ThreadedFileWriter>> tws;

    for (int i = 0; i < kWriters; ++i)
    {
        const auto path = tmp (("dusk_pool_" + std::to_string (i) + ".wav").c_str());
        paths.push_back (path);
        auto tw = makeWavWriter (path);
        REQUIRE (tw != nullptr);
        REQUIRE (pool.add (tw.get()));
        tws.push_back (std::move (tw));
    }

    // The pool's single thread drains concurrently, so pushRamp's ring-space
    // waits resolve without producer threads.
    for (auto& tw : tws)
        REQUIRE (pushRamp (*tw, kFrames, 512));

    for (auto& tw : tws) pool.remove (tw.get());   // drains to empty + flushes
    tws.clear();

    for (const auto& path : paths)
    {
        REQUIRE (fileMatchesRamp (path, kFrames));
        discard (path);
    }
}

TEST_CASE ("WriterDrainPool rejects registration past its fixed capacity",
           "[audiofile][pool]")
{
    WriterDrainPool pool (2);
    const auto p0 = tmp ("dusk_pool_cap0.wav");
    const auto p1 = tmp ("dusk_pool_cap1.wav");
    const auto p2 = tmp ("dusk_pool_cap2.wav");

    auto a = makeWavWriter (p0);
    auto b = makeWavWriter (p1);
    auto c = makeWavWriter (p2);
    REQUIRE (a); REQUIRE (b); REQUIRE (c);

    REQUIRE (pool.add (a.get()));
    REQUIRE (pool.add (b.get()));
    REQUIRE_FALSE (pool.add (c.get()));   // full

    pool.remove (a.get());
    REQUIRE (pool.add (c.get()));          // a slot freed

    pool.remove (b.get());
    pool.remove (c.get());
    a.reset(); b.reset(); c.reset();
    discard (p0); discard (p1); discard (p2);
}

namespace
{
// Fails once cumulative frames pass a limit; models a disk going full mid-drain.
struct FailingSink final : IFileWriteSink
{
    explicit FailingSink (std::int64_t limitFrames) : limit (limitFrames) {}
    int numChannels() const noexcept override { return kChannels; }
    bool writeInterleaved (const float*, std::int64_t n) noexcept override
    {
        return total.fetch_add (n) + n <= limit;
    }
    bool flush() override { return true; }

    const std::int64_t     limit;
    std::atomic<std::int64_t> total { 0 };
};
} // namespace

TEST_CASE ("WriterDrainPool isolates a failed writer from a healthy one",
           "[audiofile][pool]")
{
    constexpr std::int64_t kFrames = 20000;

    WriterDrainPool pool (2);

    const auto goodPath = tmp ("dusk_pool_good.wav");
    auto good = makeWavWriter (goodPath);
    REQUIRE (good != nullptr);

    // Bad writer fails partway; its ring is large enough to accept every push,
    // so failure comes from the sink, not overflow.
    auto badSink = std::make_unique<FailingSink> (kFrames / 4);
    auto bad = std::make_unique<ThreadedFileWriter> (std::move (badSink), (int) kFrames + 1,
                                                     ThreadedFileWriter::Drain::External);

    REQUIRE (pool.add (good.get()));
    REQUIRE (pool.add (bad.get()));

    REQUIRE (pushRamp (*good, kFrames, 512));
    // The bad writer stops accepting pushes once the drain latches failure;
    // push what fits and move on rather than waiting on a writer that will
    // never free space.
    {
        std::vector<float> l (512, 0.25f), r (512, -0.25f);
        const float* p[kChannels] = { l.data(), r.data() };
        for (std::int64_t off = 0; off < kFrames && bad->push (p, kChannels, 512); off += 512) {}
    }

    pool.remove (good.get());
    pool.remove (bad.get());

    REQUIRE_FALSE (bad->isValid());        // failure latched
    REQUIRE (fileMatchesRamp (goodPath, kFrames));   // sibling unharmed

    good.reset();
    bad.reset();
    discard (goodPath);
}

TEST_CASE ("ThreadedFileWriter external dtor flushes a take a missed remove() left",
           "[audiofile][pool]")
{
    const auto path = tmp ("dusk_pool_dtor.wav");
    constexpr std::int64_t kFrames = 4096;   // fits the ring with no draining

    {
        auto tw = makeWavWriter (path);
        REQUIRE (tw != nullptr);
        // No pool: push into the ring and let the destructor drain it, the
        // insurance path for an owner that forgot to remove().
        std::vector<float> l ((size_t) kFrames), r ((size_t) kFrames);
        for (std::int64_t f = 0; f < kFrames; ++f) { l[(size_t) f] = sample (0, f); r[(size_t) f] = sample (1, f); }
        const float* p[kChannels] = { l.data(), r.data() };
        REQUIRE (tw->push (p, kChannels, kFrames));
    } // dtor drains + flushes

    REQUIRE (fileMatchesRamp (path, kFrames));
    discard (path);
}

namespace
{
using duskstudio::test::eventually;
using duskstudio::test::returnsPromptly;
using duskstudio::test::staysAt;

constexpr std::int64_t kBlock = 512;

struct CountingSink final : IFileWriteSink
{
    int numChannels() const noexcept override { return kChannels; }
    bool writeInterleaved (const float*, std::int64_t n) noexcept override
    {
        frames.fetch_add (n);
        return true;
    }
    bool flush() override { return true; }

    std::atomic<std::int64_t> frames { 0 };
};

// A writer over a CountingSink with one block already in its ring.
struct CountedWriter
{
    CountedWriter()
    {
        auto owned = std::make_unique<CountingSink>();
        sink = owned.get();
        writer = std::make_unique<ThreadedFileWriter> (std::move (owned), 4096,
                                                       ThreadedFileWriter::Drain::External);
        const std::vector<float> l ((size_t) kBlock, 0.5f), r ((size_t) kBlock, -0.5f);
        const float* p[kChannels] = { l.data(), r.data() };
        REQUIRE (writer->push (p, kChannels, kBlock));
    }

    bool drained() const { return sink->frames.load() == kBlock; }

    CountingSink* sink = nullptr;
    std::unique_ptr<ThreadedFileWriter> writer;
};
} // namespace

TEST_CASE ("WriterDrainPool makes no timer wake-ups while no writer is registered", "[audiofile][pool]")
{
    std::atomic<int> idle { 0 };
    WriterDrainPool pool (2, [&] { idle.fetch_add (1); });

    REQUIRE (eventually ([&] { return idle.load() > 0; }));
    CHECK (staysAt (idle, idle.load()));
}

TEST_CASE ("WriterDrainPool drains a writer added while it sleeps", "[audiofile][pool]")
{
    CountedWriter counted;
    std::atomic<int> idle { 0 };
    WriterDrainPool pool (2, [&] { idle.fetch_add (1); });
    REQUIRE (eventually ([&] { return idle.load() > 0; }));

    REQUIRE (pool.add (counted.writer.get()));
    CHECK (eventually ([&] { return counted.drained(); }));
    pool.remove (counted.writer.get());
}

// The add lands after the pool has seen no writer and before it waits, and
// nothing else wakes it: the add alone has to end that wait. The first take
// brings the pool back to that point once the hook is armed.
TEST_CASE ("WriterDrainPool drains a writer added as it goes to sleep", "[audiofile][pool]")
{
    CountedWriter first, late;
    std::atomic<bool> armed { false }, added { false };
    std::atomic<WriterDrainPool*> self { nullptr };
    WriterDrainPool pool (2, [&]
    {
        auto* p = self.load();
        if (p == nullptr || ! armed.exchange (false)) return;
        added.store (p->add (late.writer.get()));
    });
    self.store (&pool);

    REQUIRE (pool.add (first.writer.get()));
    REQUIRE (eventually ([&] { return first.drained(); }));
    armed.store (true);
    pool.remove (first.writer.get());

    CHECK (eventually ([&] { return late.drained(); }));
    CHECK (added.load());
    pool.remove (late.writer.get());
}

TEST_CASE ("WriterDrainPool goes back to sleep when its last writer is removed", "[audiofile][pool]")
{
    CountedWriter counted;
    std::atomic<int> idle { 0 };
    WriterDrainPool pool (2, [&] { idle.fetch_add (1); });

    // Added to a sleeping pool, so the add's wake-up is spent ending that
    // sleep and none is left over to end the next one.
    REQUIRE (eventually ([&] { return idle.load() > 0; }));
    REQUIRE (pool.add (counted.writer.get()));
    REQUIRE (eventually ([&] { return counted.drained(); }));
    const int beforeRemove = idle.load();
    pool.remove (counted.writer.get());

    REQUIRE (eventually ([&] { return idle.load() > beforeRemove; }));
    CHECK (staysAt (idle, idle.load()));
}

TEST_CASE ("Destroying a sleeping WriterDrainPool wakes and joins its thread", "[audiofile][pool]")
{
    auto idle = std::make_shared<std::atomic<int>> (0);
    auto pool = std::make_unique<WriterDrainPool> (1, [idle] { idle->fetch_add (1); });
    REQUIRE (eventually ([&] { return idle->load() > 0; }));

    auto* sleeping = pool.release();
    // A destructor that never returns still holds the pool, so it is left alive.
    CHECK (returnsPromptly ([sleeping] { delete sleeping; }));
}
