#include <catch2/catch_test_macros.hpp>

#include "engine/sfz/SfzArtworkCache.h"
#include "engine/sfz/SfzInstrumentCategory.h"
#include "foundation/Fs.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace
{
using namespace duskstudio::sfz;
struct Tree
{
    std::filesystem::path root = dusk::fs::createUniqueTempDirectory ("dusk-sfz-artwork-");
    ~Tree() { std::error_code error; std::filesystem::remove_all (root, error); }
};

std::shared_ptr<const ArtworkPixels> decode (std::string_view bytes)
{
    if (bytes.empty() || bytes == "invalid") return {};
    auto pixels = std::make_shared<ArtworkPixels>();
    pixels->width = pixels->height = 1;
    pixels->rgba = { static_cast<unsigned char> (bytes.front()), 20, 30, 255 };
    return pixels;
}

struct Transport final : HttpTransport
{
    std::atomic<unsigned> calls { 0 };
    std::string bytes = "source";
    int status = 200;
    std::string location;
    std::shared_future<void> barrier;
    OperationResult perform (const HttpRequest& request, const HttpHeadersCallback& headers,
                             const HttpBodyCallback& body, const OperationControl& control) override
    {
        ++calls;
        while (barrier.valid() && barrier.wait_for (std::chrono::seconds (0)) != std::future_status::ready)
        {
            if (control.status() != OperationStatus::Complete) return { control.status(), {} };
            std::this_thread::yield();
        }
        if (control.status() != OperationStatus::Complete) return { control.status(), {} };
        if (! headers ({ status, request.url, location.empty() ? HttpHeaders {} : HttpHeaders {{ "location", location }} }) || ! body (bytes)) return { OperationStatus::Failed, "rejected" };
        return { OperationStatus::Complete, {} };
    }
};

template <typename Predicate>
void until (Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (3);
    while (! predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    REQUIRE (predicate());
}

constexpr std::string_view fallback = "fallback";
constexpr const char* url = "https://raw.githubusercontent.com/piano.jpg";
} // namespace

TEST_CASE ("SFZ categories recognize instrument names before collection context", "[sfz][categories]")
{
    using C = InstrumentCategory;
    CHECK (instrumentCategory ("karoryfer.big-rusty-drums") == C::Drums);
    CHECK (instrumentCategory ("HeadroomPiano") == C::Piano);
    CHECK (instrumentCategory ("Bass Drum", "VCSL") == C::Drums);
    CHECK (instrumentCategory ("Clarinets", "Orchestral Collection") == C::Woodwinds);
    CHECK (instrumentCategory ("Bongos") == C::Percussion);
    CHECK (instrumentCategory ("Rhodes Mark V") == C::ElectricPiano);
    CHECK (instrumentCategory ("Bassoon Staccato", "VSCO strings and piano") == C::Woodwinds);
    CHECK (instrumentCategory ("Cello", "VCSL piano and percussion") == C::Strings);
    CHECK (instrumentCategory ("Electric Guitar") == C::ElectricGuitar);
    CHECK (instrumentCategory ("Acoustic Guitar") == C::AcousticGuitar);
    CHECK (instrumentCategory ("Pizzicato", "Piano") == C::Strings);
    CHECK (instrumentCategory ("Program 01", "Woodwinds/Flute.sfz") == C::Woodwinds);
    CHECK (instrumentCategory ("VCSL", "piano and woodwinds") == C::Collection);
    CHECK (instrumentCategory ("Koto") == C::World);
    CHECK (instrumentCategory ("Unknown") == C::Other);
}

TEST_CASE ("SFZ artwork cache survives restart and never fetches for offline selections", "[sfz][artwork][ipc]")
{
    Tree tree;
    auto online = std::make_shared<Transport>();
    {
        ArtworkCache cache (tree.root, online);
        cache.request ("piano", url, fallback, decode, true, "sfzinstruments");
        until ([&] { return cache.snapshot().sourceImage; });
        REQUIRE (cache.snapshot().pixels->rgba.front() == 's');
        REQUIRE (online->calls == 1);
    }
    auto offline = std::make_shared<Transport>();
    ArtworkCache cached (tree.root, offline);
    cached.request ("installed-piano", url, fallback, decode, false, "sfzinstruments");
    until ([&] { return cached.snapshot().sourceImage; });
    CHECK (cached.snapshot().pixels->rgba.front() == 's');
    CHECK (offline->calls == 0);
    cached.request ("uncached", "https://raw.githubusercontent.com/other.jpg", fallback, decode, false, "sfzinstruments");
    until ([&] { return cached.snapshot().pixels != nullptr; });
    CHECK_FALSE (cached.snapshot().sourceImage);
    CHECK (cached.snapshot().pixels->rgba.front() == 'f');
    CHECK (offline->calls == 0);
    for (const auto& file : std::filesystem::directory_iterator (tree.root))
        if (file.path().extension() == ".image") REQUIRE (dusk::fs::writeStringToFile (file.path(), "invalid"));
    auto repair = std::make_shared<Transport>();
    ArtworkCache repaired (tree.root, repair);
    repaired.request ("repaired", url, fallback, decode, true, "sfzinstruments");
    until ([&] { const auto state = repaired.snapshot(); return state.sourceImage && ! state.busy; });
    CHECK (repair->calls == 1);
    ArtworkCache reloaded (tree.root, offline);
    reloaded.request ("reloaded", url, fallback, decode, false, "sfzinstruments");
    until ([&] { const auto state = reloaded.snapshot(); return state.pixels && ! state.busy; });
    CHECK (reloaded.snapshot().sourceImage);
    CHECK (offline->calls == 0);
}

TEST_CASE ("SFZ artwork failures retain the category image and do not fetch unsafe URLs", "[sfz][artwork][ipc]")
{
    Tree tree;
    auto transport = std::make_shared<Transport>();
    SECTION ("invalid image") { transport->bytes = "invalid"; }
    SECTION ("oversized image") { transport->bytes.assign (4 * 1024 * 1024 + 1, 'x'); }
    SECTION ("HTTP error") { transport->status = 503; }
    ArtworkCache cache (tree.root, transport);
    cache.request ("piano", url, fallback, decode, true, "sfzinstruments");
    until ([&] { const auto state = cache.snapshot(); return transport->calls == 1 && state.pixels && ! state.busy; });
    CHECK_FALSE (cache.snapshot().sourceImage);
    CHECK (cache.snapshot().pixels->rgba.front() == 'f');
    cache.cancel();
    const auto before = transport->calls.load();
    for (const auto* unsafe : { "http://raw.githubusercontent.com/a.jpg", "file:///tmp/a.jpg", "https://user@raw.githubusercontent.com/a.jpg", "https://",
                                "https://raw.githubusercontent.com.evil.invalid/a.jpg", "https://raw.githubusercontent.com:444/a.jpg",
                                "https://elsewhere.example/a.jpg", "https://musical-artifacts.com/a.jpg" })
    {
        cache.request (unsafe, unsafe, fallback, decode, true, "sfzinstruments");
        until ([&] { const auto state = cache.snapshot(); return state.pixels && ! state.busy; });
        CHECK_FALSE (cache.snapshot().sourceImage);
    }
    CHECK (transport->calls == before);
}

TEST_CASE ("SFZ artwork restricts every redirect to its provider hosts", "[sfz][artwork][ipc]")
{
    Tree tree;
    auto transport = std::make_shared<Transport>();
    transport->status = 302;
    transport->location = "https://untrusted.example/cover.jpg";
    ArtworkCache cache (tree.root, transport);
    cache.request ("redirect", url, fallback, decode, true, "sfzinstruments");
    until ([&] { const auto state = cache.snapshot(); return state.pixels && ! state.busy; });
    CHECK_FALSE (cache.snapshot().sourceImage);
    CHECK (transport->calls == 1);
}

TEST_CASE ("SFZ artwork uses only hosts belonging to the selected provider", "[sfz][artwork][ipc]")
{
    Tree tree;
    auto transport = std::make_shared<Transport>();
    ArtworkCache cache (tree.root, transport);
    cache.request ("artifact", "https://musical-artifacts.com/cover.jpg", fallback, decode, true, "musical-artifacts");
    until ([&] { const auto state = cache.snapshot(); return state.pixels && ! state.busy; });
    REQUIRE (cache.snapshot().sourceImage);
    REQUIRE (transport->calls == 1);
    cache.request ("wrong-provider", "https://musical-artifacts.com/cover.jpg", fallback, decode, false, "freepats");
    until ([&] { const auto state = cache.snapshot(); return state.pixels && ! state.busy; });
    CHECK_FALSE (cache.snapshot().sourceImage);
    for (const auto* source : { "sfzinstruments", "freepats", "versilian" })
    {
        cache.request (source, url, fallback, decode, true, source);
        until ([&] { const auto state = cache.snapshot(); return state.pixels && ! state.busy; });
        CHECK (cache.snapshot().sourceImage);
    }
    CHECK (transport->calls == 2);
}

TEST_CASE ("SFZ artwork ignores stale selections and pauses network work during recording", "[sfz][artwork][ipc]")
{
    Tree tree;
    auto transport = std::make_shared<Transport>();
    std::promise<void> release;
    transport->barrier = release.get_future().share();
    ArtworkCache cache (tree.root, transport);
    cache.request ("old", url, fallback, decode, true, "sfzinstruments");
    until ([&] { return transport->calls == 1; });
    cache.request ("current", {}, fallback, decode, false, "sfzinstruments");
    release.set_value();
    until ([&] { const auto state = cache.snapshot(); return state.key == "current" && state.pixels; });
    CHECK_FALSE (cache.snapshot().sourceImage);
    CHECK (cache.snapshot().pixels->rgba.front() == 'f');
    cache.setHeavyWork (true);
    cache.request ("recording", "https://raw.githubusercontent.com/recording.jpg", fallback, decode, true, "sfzinstruments");
    until ([&] { const auto state = cache.snapshot(); return state.pixels && ! state.busy; });
    CHECK (transport->calls == 1);
    cache.setHeavyWork (false);
    CHECK (transport->calls == 1);
    cache.request ("resumed", "https://raw.githubusercontent.com/recording.jpg", fallback, decode, true, "sfzinstruments");
    until ([&] { return cache.snapshot().sourceImage; });
    CHECK (transport->calls == 2);
}
