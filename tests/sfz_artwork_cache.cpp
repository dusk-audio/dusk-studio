#include <catch2/catch_test_macros.hpp>

#include "engine/sfz/SfzArtworkCache.h"
#include "engine/sfz/SfzDirectoryLease.h"
#include "engine/sfz/SfzInstrumentCategory.h"
#include "foundation/Fs.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

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

TEST_CASE ("SFZ artwork startup removes only abandoned owned image temporaries", "[sfz][artwork][ipc]")
{
    namespace fs = std::filesystem;
    Tree tree;
    const auto root = fs::canonical (tree.root);
    const std::vector<std::string> owned {
        std::string (64, 'a') + ".42.part", std::string (64, 'b') + ".4294967295.part", std::string (64, 'c') + ".0.part"
    };
    const std::vector<std::string> unrelated {
        "other.part", std::string (64, 'a') + ".0042.part", std::string (64, 'b') + ".4294967296.part",
        std::string (64, 'A') + ".42.part", ".cache-" + std::string (32, 'a')
    };
    for (const auto& name : owned) REQUIRE (dusk::fs::writeStringToFile (root / name, "unfinished"));
    for (const auto& name : unrelated) REQUIRE (dusk::fs::writeStringToFile (root / name, "keep"));
    const auto directory = root / (std::string (64, 'd') + ".7.part");
    REQUIRE (fs::create_directory (directory));
    const auto target = root / "unrelated-target";
    REQUIRE (dusk::fs::writeStringToFile (target, "keep"));
    const auto symbolic = root / (std::string (64, 'e') + ".8.part");
    std::error_code error;
    fs::create_symlink (target, symbolic, error);
    const bool madeSymbolic = ! error;
    const auto linked = root / (std::string (64, 'f') + ".9.part");
    fs::create_hard_link (target, linked, error);
    const bool madeLink = ! error;
    auto transport = std::make_shared<Transport>();
    ArtworkCache cache (root, transport);
    cache.request ("startup", {}, fallback, decode, false, "sfzinstruments");
    until ([&] { const auto state = cache.snapshot(); return state.pixels && ! state.busy; });
    for (const auto& name : owned) CHECK_FALSE (fs::exists (root / name));
    for (const auto& name : unrelated) CHECK (dusk::fs::loadFileAsString (root / name) == "keep");
    CHECK (fs::is_directory (directory));
    CHECK (dusk::fs::loadFileAsString (target) == "keep");
    if (madeSymbolic) CHECK (fs::is_symlink (fs::symlink_status (symbolic)));
    if (madeLink) CHECK (dusk::fs::loadFileAsString (linked) == "keep");
    CHECK (transport->calls == 0);
}

TEST_CASE ("SFZ artwork maintenance waits for the last active cache owner", "[sfz][artwork][ipc]")
{
    namespace fs = std::filesystem;
    Tree tree;
    const auto root = fs::canonical (tree.root);
    const auto cacheRoot = root / "images";
    REQUIRE (fs::create_directory (cacheRoot));
    auto input = cacheRoot;
    SECTION ("Plain cache parent") {}
    SECTION ("Symbolic cache parent")
    {
        const auto alias = root / "alias";
        std::error_code error;
        fs::create_directory_symlink (root, alias, error);
        if (error) SKIP ("The filesystem cannot create a symbolic-directory fixture");
        input = alias / "images";
    }
    const auto temporary = cacheRoot / (std::string (64, 'a') + ".17.part");
    auto transport = std::make_shared<Transport>();
    {
        DirectoryLease active;
        std::string error;
        REQUIRE (active.acquire (cacheRoot, DirectoryLease::Access::Shared, error));
        REQUIRE (dusk::fs::writeStringToFile (temporary, "active"));
        ArtworkCache first (input, transport);
        first.request ("first", {}, fallback, decode, false, "sfzinstruments");
        until ([&] { const auto state = first.snapshot(); return state.pixels && ! state.busy; });
        active = {};
        ArtworkCache second (input, transport);
        second.request ("second", {}, fallback, decode, false, "sfzinstruments");
        until ([&] { const auto state = second.snapshot(); return state.pixels && ! state.busy; });
        CHECK (dusk::fs::loadFileAsString (temporary) == "active");
        CHECK (transport->calls == 0);
    }
    ArtworkCache restarted (input, transport);
    restarted.request ("restarted", {}, fallback, decode, false, "sfzinstruments");
    until ([&] { const auto state = restarted.snapshot(); return state.pixels && ! state.busy; });
    CHECK_FALSE (fs::exists (temporary));
    CHECK (transport->calls == 0);
   #if ! defined(_WIN32)
    fs::rename (cacheRoot, root / "old-images");
    REQUIRE (fs::create_directory (cacheRoot));
    restarted.request ("new-root", url, fallback, decode, true, "sfzinstruments");
    until ([&] { const auto state = restarted.snapshot(); return state.sourceImage && ! state.busy; });
    DirectoryLease maintenance;
    std::string error;
    CHECK_FALSE (maintenance.acquire (cacheRoot, DirectoryLease::Access::Exclusive, error));
    CHECK (error.empty());
    CHECK (transport->calls == 1);
   #endif
}

TEST_CASE ("SFZ artwork quotas evict only owned plain cache images", "[sfz][artwork][ipc]")
{
    namespace fs = std::filesystem;
    Tree tree;
    unsigned count = 256;
    bool oversized = false;
    SECTION ("Image count") {}
    SECTION ("Byte limit") { count = 1; oversized = true; }
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours (2);
    std::vector<fs::path> images;
    for (unsigned index = 0; index < count; ++index)
    {
        const auto token = std::to_string (index);
        images.push_back (tree.root / (std::string (64 - token.size(), '0') + token + ".image"));
        REQUIRE (dusk::fs::writeStringToFile (images.back(), "image"));
        if (oversized) fs::resize_file (images.back(), 64 * 1024 * 1024);
        fs::last_write_time (images.back(), old);
    }
    const auto unrelated = tree.root / "notes.image";
    const auto uppercase = tree.root / (std::string (64, 'A') + ".image");
    for (const auto& path : { unrelated, uppercase })
    {
        REQUIRE (dusk::fs::writeStringToFile (path, "keep"));
        fs::last_write_time (path, old - std::chrono::hours (1));
    }
    const auto linked = tree.root / (std::string (64, 'f') + ".image");
    std::error_code error;
    fs::create_hard_link (unrelated, linked, error);
    const bool madeLink = ! error;
    const auto symbolic = tree.root / (std::string (64, 'e') + ".image");
    fs::create_symlink (unrelated, symbolic, error);
    const bool madeSymbolic = ! error;
    const auto directory = tree.root / (std::string (64, 'd') + ".image");
    REQUIRE (fs::create_directory (directory));
    auto transport = std::make_shared<Transport>();
    ArtworkCache cache (tree.root, transport);
    cache.request ("source", url, fallback, decode, true, "sfzinstruments");
    until ([&] { const auto state = cache.snapshot(); return state.sourceImage && ! state.busy; });
    CHECK_FALSE (fs::exists (images.front()));
    for (unsigned index = 1; index < count; ++index) CHECK (fs::exists (images[index]));
    CHECK (dusk::fs::loadFileAsString (unrelated) == "keep");
    CHECK (dusk::fs::loadFileAsString (uppercase) == "keep");
    if (madeLink) CHECK (dusk::fs::loadFileAsString (linked) == "keep");
    if (madeSymbolic) CHECK (fs::is_symlink (fs::symlink_status (symbolic)));
    CHECK (fs::is_directory (directory));
    CHECK (transport->calls == 1);
}

TEST_CASE ("SFZ artwork remains available when disk ownership or publication fails", "[sfz][artwork][ipc]")
{
    namespace fs = std::filesystem;
    Tree tree;
    auto transport = std::make_shared<Transport>();
    fs::path blockedImage;
    SECTION ("Unavailable ownership leaves the disk cache untouched")
    {
        REQUIRE (fs::create_directory (tree.root / ".dusk-sfz.lock"));
    }
    SECTION ("A failed rename removes the temporary image")
    {
        {
            ArtworkCache seed (tree.root, transport);
            seed.request ("seed", url, fallback, decode, true, "sfzinstruments");
            until ([&] { const auto state = seed.snapshot(); return state.sourceImage && ! state.busy; });
        }
        for (const auto& entry : fs::directory_iterator (tree.root))
            if (entry.path().extension() == ".image") blockedImage = entry.path();
        REQUIRE_FALSE (blockedImage.empty());
        REQUIRE (fs::remove (blockedImage));
        REQUIRE (fs::create_directory (blockedImage));
    }
    const auto before = transport->calls.load();
    ArtworkCache cache (tree.root, transport);
    cache.request ("image", url, fallback, decode, true, "sfzinstruments");
    until ([&] { const auto state = cache.snapshot(); return state.sourceImage && ! state.busy; });
    CHECK (cache.snapshot().pixels->rgba.front() == 's');
    CHECK (transport->calls == before + 1);
    for (const auto& entry : fs::directory_iterator (tree.root))
    {
        CHECK (entry.path().extension() != ".part");
        if (entry.path().extension() == ".image") CHECK (entry.path() == blockedImage);
    }
    if (! blockedImage.empty()) CHECK (fs::is_directory (blockedImage));
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
