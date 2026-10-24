#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/PluginSearchPaths.h"
#include "engine/JuceCompat.h"
#include "engine/ipc/PluginScanProtocol.h"
#include "engine/ipc/RemotePluginConnection.h"

#if defined(_WIN32)
#include "../cmake/JuceLv2WindowsPaths.h"
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;
namespace paths = duskstudio::pluginpaths;
struct RegisteredLv2Format
{
    duskstudio::juce_compat::FormatManager manager;
    juce::AudioPluginFormat* format = nullptr;
    RegisteredLv2Format()
    {
        duskstudio::juce_compat::addDefaultFormats (manager);
        for (auto* candidate : manager.getFormats())
            if (candidate->getName() == "LV2") format = candidate;
        REQUIRE (format != nullptr);
    }
};

struct CountingLv2Format : juce::LV2PluginFormat
{
    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>&, const juce::String&) override
    {
        ++visits;
        if (onPrime) onPrime();
    }
    int visits = 0;
    std::function<void()> onPrime;
};

// Searches the way JUCE's VST3 format does: every directory link is followed
// and nothing records what was visited. The depth cap turns a runaway walk into
// a failed check instead of a hung test.
struct LinkFollowingVst3Format : juce::LV2PluginFormat
{
    juce::String getName() const override { return "VST3"; }
    juce::FileSearchPath getDefaultLocationsToSearch() override { return defaults; }
    bool fileMightContainThisPluginType (const juce::String& file) override { return file.endsWithIgnoreCase (".vst3"); }
    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath& paths, bool, bool) override
    {
        juce::StringArray found;
        for (int i = 0; i < paths.getNumPaths(); ++i)
        {
            searched.push_back (std::filesystem::u8path (paths[i].getFullPathName().toStdString()));
            walk (found, paths[i], 0);
        }
        return found;
    }
    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results, const juce::String& file) override
    {
        auto description = std::make_unique<juce::PluginDescription>();
        description->pluginFormatName = getName();
        description->name = juce::File (file).getFileNameWithoutExtension();
        description->fileOrIdentifier = file;
        results.add (description.release());
    }
    void walk (juce::StringArray& found, const juce::File& directory, int depth)
    {
        if (depth > 8) { runaway = true; return; }
        for (const auto& entry : juce::RangedDirectoryIterator (directory, false, "*", juce::File::findFilesAndDirectories))
        {
            const auto file = entry.getFile();
            if (fileMightContainThisPluginType (file.getFullPathName())) found.add (file.getFullPathName());
            else if (file.isDirectory()) walk (found, file, depth + 1);
        }
    }
    juce::FileSearchPath defaults;
    std::vector<std::filesystem::path> searched;
    bool runaway = false;
};

// Loading a bundle path teaches the format the URI "urn:<bundle stem>".
struct RecordingLv2Format : juce::LV2PluginFormat
{
    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results, const juce::String& identifier) override
    {
        const auto text = identifier.toStdString();
        calls.push_back (text);
        const auto path = std::filesystem::u8path (text);
        if (path.extension() == ".lv2") known.insert ("urn:" + path.stem().u8string());
        if (known.count (text) == 0) return;
        auto description = std::make_unique<juce::PluginDescription>();
        description->fileOrIdentifier = identifier;
        results.add (description.release());
    }
    std::vector<std::string> calls;
    std::set<std::string> known;
};

template <typename Format>
Format& addLv2 (juce::AudioPluginFormatManager& manager)
{
    auto format = std::make_unique<Format>();
    auto& result = *format;
   #if DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS
    manager.addFormat (std::move (format));
   #else
    manager.addFormat (format.release());
   #endif
    return result;
}

CountingLv2Format& addCountingLv2 (juce::AudioPluginFormatManager& manager)
{
    return addLv2<CountingLv2Format> (manager);
}

#if ! defined(_WIN32)
// A chain of folders whose full path is longer than PATH_MAX, built and removed
// relative to each parent because no absolute path can name the deep end.
struct OverlongFolder
{
    std::vector<int> descriptors;
    std::string name = std::string (240, 'd');
    explicit OverlongFolder (const std::filesystem::path& parent)
    {
        descriptors.push_back (::open (parent.c_str(), O_RDONLY | O_DIRECTORY));
        REQUIRE (descriptors.back() >= 0);
        for (int depth = 0; depth < 20; ++depth)
        {
            REQUIRE (::mkdirat (descriptors.back(), name.c_str(), 0700) == 0);
            descriptors.push_back (::openat (descriptors.back(), name.c_str(), O_RDONLY | O_DIRECTORY));
            REQUIRE (descriptors.back() >= 0);
        }
    }
    ~OverlongFolder()
    {
        for (std::size_t level = descriptors.size(); level-- > 1;)
        {
            ::close (descriptors[level]);
            ::unlinkat (descriptors[level - 1], name.c_str(), AT_REMOVEDIR);
        }
        ::close (descriptors.front());
    }
};
#endif

struct Tree
{
    fs::path root = dusk::fs::createUniqueTempDirectory ("dusk-plugin-paths-");
    Tree()
    {
        REQUIRE (! root.empty());
        root = fs::canonical (root);
    }
    ~Tree() { std::error_code error; fs::remove_all (root, error); }
    fs::path directory (const fs::path& relative)
    {
        const auto path = root / relative;
        REQUIRE (fs::create_directories (path));
        return path;
    }
};

struct ConfigOverride
{
    fs::path::string_type previous;
    bool existed = false;
    explicit ConfigOverride (const fs::path& directory)
    {
       #if defined(_WIN32)
        std::size_t length = 0;
        REQUIRE (::_wgetenv_s (&length, nullptr, 0, L"DUSKSTUDIO_CONFIG_DIR") == 0);
        existed = length > 0;
        if (existed)
        {
            previous.resize (length);
            REQUIRE (::_wgetenv_s (&length, previous.data(), previous.size(), L"DUSKSTUDIO_CONFIG_DIR") == 0);
            previous.resize (length - 1);
        }
        REQUIRE (::_wputenv_s (L"DUSKSTUDIO_CONFIG_DIR", directory.c_str()) == 0);
       #else
        const auto* value = std::getenv ("DUSKSTUDIO_CONFIG_DIR");
        existed = value != nullptr;
        if (value != nullptr) previous = value;
        REQUIRE (::setenv ("DUSKSTUDIO_CONFIG_DIR", directory.c_str(), 1) == 0);
       #endif
    }
    ~ConfigOverride()
    {
       #if defined(_WIN32)
        ::_wputenv_s (L"DUSKSTUDIO_CONFIG_DIR", existed ? previous.c_str() : L"");
       #else
        if (existed) ::setenv ("DUSKSTUDIO_CONFIG_DIR", previous.c_str(), 1);
        else ::unsetenv ("DUSKSTUDIO_CONFIG_DIR");
       #endif
    }
};
}

TEST_CASE ("Custom LV2 priming caches roots for each format lifetime", "[plugins][paths]")
{
    Tree tree;
    ConfigOverride config (tree.directory ("config"));
    const paths::Roots roots { tree.directory ("custom") };
    tree.directory ("custom/nested/First.lv2");
    std::string error;
    REQUIRE (paths::save (roots, error));
    duskstudio::juce_compat::FormatManager manager;
    auto& format = addCountingLv2 (manager);
    duskstudio::juce_compat::primeCustomLv2 (manager, paths::load());
    REQUIRE (format.visits == 1);
    duskstudio::juce_compat::primeCustomLv2 (manager, paths::load());
    CHECK (format.visits == 1);

    SECTION ("new manager lifetimes prime independently")
    {
        std::optional<duskstudio::juce_compat::FormatManager> fresh;
        for (int lifetime = 0; lifetime < 2; ++lifetime)
        {
            fresh.emplace();
            auto& other = addCountingLv2 (*fresh);
            duskstudio::juce_compat::primeCustomLv2 (*fresh, paths::load());
            CHECK (other.visits == 1);
            fresh.reset();
        }
    }
    SECTION ("changed and empty roots invalidate priming")
    {
        REQUIRE (paths::save ({}, error));
        duskstudio::juce_compat::primeCustomLv2 (manager, paths::load());
        REQUIRE (paths::save (roots, error));
        duskstudio::juce_compat::primeCustomLv2 (manager, paths::load());
        CHECK (format.visits == 2);
        const paths::Roots other { tree.directory ("other") };
        tree.directory ("other/Second.lv2");
        REQUIRE (paths::save (other, error));
        duskstudio::juce_compat::primeCustomLv2 (manager, paths::load());
        CHECK (format.visits == 3);
    }
    SECTION ("explicit refresh discovers new bundles under unchanged roots")
    {
        tree.directory ("custom/new/Second.lv2");
        duskstudio::juce_compat::primeCustomLv2 (manager, paths::load());
        CHECK (format.visits == 1);
        duskstudio::juce_compat::primeCustomLv2 (manager, roots, true);
        CHECK (format.visits == 3);
        duskstudio::juce_compat::primeCustomLv2 (manager, paths::load());
        CHECK (format.visits == 3);
    }
    SECTION ("reconnected roots retry discovery")
    {
        const auto unplugged = tree.root / "unplugged";
        fs::rename (roots.front(), unplugged);
        duskstudio::juce_compat::primeCustomLv2 (manager, paths::load());
        CHECK (format.visits == 1);
        fs::rename (unplugged, roots.front());
        duskstudio::juce_compat::primeCustomLv2 (manager, paths::load());
        CHECK (format.visits == 2);
    }
    SECTION ("cancellation does not commit partial priming")
    {
        std::atomic<bool> abort { true };
        duskstudio::juce_compat::primeCustomLv2 (manager, roots, true, &abort);
        CHECK (format.visits == 1);
        abort.store (false);
        tree.directory ("custom/new/Second.lv2");
        format.onPrime = [&] { abort.store (true); };
        duskstudio::juce_compat::primeCustomLv2 (manager, roots, true, &abort);
        CHECK (format.visits == 2);
        format.onPrime = {};
        abort.store (false);
        duskstudio::juce_compat::primeCustomLv2 (manager, roots, false, &abort);
        CHECK (format.visits == 4);
    }
    SECTION ("exceptions leave priming retryable")
    {
        format.onPrime = [] { throw std::runtime_error ("metadata failure"); };
        CHECK_THROWS_AS (duskstudio::juce_compat::primeCustomLv2 (manager, roots, true), std::runtime_error);
        CHECK (format.visits == 2);
        format.onPrime = {};
        duskstudio::juce_compat::primeCustomLv2 (manager, paths::load());
        CHECK (format.visits == 3);
    }
}

TEST_CASE ("Custom plugin folders persist Unicode and unavailable locations", "[plugins][paths]")
{
    Tree tree;
    const auto file = tree.root / "settings" / "plugin-search-paths.json";
    const paths::Roots roots { tree.root / fs::u8path ("Plug-ins \xc3\xa9 % = [one]"), tree.root / "offline" };
    std::string error;
    REQUIRE (paths::save (roots, error, file));
    CHECK (error.empty());
    CHECK (paths::load (file) == roots);
    REQUIRE (paths::save ({ roots.front() }, error, file));
    CHECK (paths::load (file) == paths::Roots { roots.front() });
    CHECK_FALSE (fs::exists (roots.front()));
}

TEST_CASE ("Custom plugin roots extend defaults and discard only equivalent aliases", "[plugins][paths]")
{
    Tree tree;
    const auto standard = tree.directory ("standard");
    const auto custom = tree.directory ("custom");
    const auto offline = tree.root / "offline";
    CHECK (paths::extend ({ standard }, { standard / ".", custom, custom, offline }) == paths::Roots { standard, custom, offline });
    std::error_code error;
    const auto alias = tree.root / "alias";
    fs::create_directory_symlink (custom, alias, error);
    if (! error) CHECK (paths::normalize ({ custom, alias }) == paths::Roots { custom });
   #if defined(_WIN32)
    CHECK (paths::normalize ({ tree.root / "Offline", tree.root / "offline" }).size() == 1);
   #else
    CHECK (paths::normalize ({ tree.root / "Offline", tree.root / "offline" }).size() == 2);
   #endif
}

TEST_CASE ("Custom plugin discovery reaches nested vendors and stops at bundle boundaries", "[plugins][paths]")
{
    Tree tree;
    const auto vendor = tree.directory ("custom/vendor/nested/products");
    const auto vst3 = tree.directory ("custom/vendor/nested/products/Effect.vst3/Contents").parent_path();
    const auto lv2 = tree.directory ("custom/vendor/nested/products/Synth.lv2");
    REQUIRE (dusk::fs::writeStringToFile (vendor / "Effect.clap", "fixture"));
    REQUIRE (dusk::fs::writeStringToFile (vst3 / "Contents" / "Hidden.clap", "not a top-level plugin"));
    REQUIRE (dusk::fs::writeStringToFile (lv2 / "Hidden.vst3", "not a top-level plugin"));
    const paths::Roots roots { tree.root / "custom", vendor, lv2 };
    CHECK (paths::findBundles (roots, ".vst3") == paths::Roots { vst3 });
    CHECK (paths::findBundles (roots, ".clap") == paths::Roots { vendor / "Effect.clap" });
    CHECK (paths::findBundles (roots, ".lv2") == paths::Roots { lv2 });
    const auto external = tree.directory ("external/Vendor/Linked.vst3");
    std::error_code linkError;
    fs::create_directory_symlink (tree.root / "external", vendor / "linked-vendor", linkError);
    if (! linkError)
    {
        fs::create_directory_symlink (tree.root / "custom", tree.root / "external" / "cycle", linkError);
        const auto linked = paths::findBundles (roots, ".vst3");
        CHECK (linked.size() == 2);
        CHECK (std::find (linked.begin(), linked.end(), vendor / "linked-vendor" / "Vendor" / "Linked.vst3") != linked.end());
        CHECK (std::find (linked.begin(), linked.end(), external) == linked.end());
    }
    std::atomic<bool> abort { true };
    CHECK (paths::findBundles (roots, ".vst3", &abort).empty());
}

TEST_CASE ("Custom plugin roots and bundles keep the spelling that reached them through links", "[plugins][paths]")
{
    Tree tree;
    const auto versioned = tree.directory ("vendor/Diva-1.4");
    REQUIRE (dusk::fs::writeStringToFile (versioned / "Diva.64.so", "fixture"));
    const auto custom = tree.directory ("custom");
    const auto vendorLink = custom / "u-he";
    std::error_code linkError;
    fs::create_directory_symlink (tree.root / "vendor", vendorLink, linkError);
    if (! linkError) fs::create_symlink (versioned / "Diva.64.so", custom / "Diva.clap", linkError);
    if (linkError) SKIP ("symbolic links are unavailable: " << linkError.message());
    CHECK (paths::normalize ({ vendorLink, tree.root / "vendor" }) == paths::Roots { vendorLink });
    CHECK (paths::findBundles ({ custom }, ".clap") == paths::Roots { custom / "Diva.clap" });
    const auto file = tree.root / "settings.json";
    std::string error;
    REQUIRE (paths::save ({ vendorLink / "Diva-1.4" / "" }, error, file));
    CHECK (paths::stored (file) == paths::Roots { vendorLink / "Diva-1.4" });
    CHECK (paths::load (file) == paths::Roots { vendorLink / "Diva-1.4" });
}

TEST_CASE ("A folder that cannot be walked reports an error and keeps its siblings", "[plugins][paths]")
{
   #if defined(_WIN32)
    SKIP ("long paths are read through the extended-length prefix on Windows");
   #else
    Tree tree;
    const auto custom = tree.directory ("custom");
    const auto before = tree.directory ("custom/a/Before.lv2");
    const auto after = tree.directory ("custom/z/After.lv2");
    {
        bool walkError = true;
        CHECK (paths::findBundles ({ custom }, ".lv2", nullptr, &walkError) == paths::Roots { before, after });
        CHECK_FALSE (walkError);
    }
    const OverlongFolder overlong (tree.directory ("custom/m"));
    bool walkError = false;
    CHECK (paths::findBundles ({ custom }, ".lv2", nullptr, &walkError) == paths::Roots { before, after });
    CHECK (walkError);

    duskstudio::juce_compat::FormatManager manager;
    auto& format = addLv2<RecordingLv2Format> (manager);
    fs::rename (tree.root / "custom" / "m", tree.root / "parked");
    CHECK (duskstudio::juce_compat::primeCustomLv2 (manager, { custom }, true));
    CHECK (duskstudio::juce_compat::customLv2Bundles (manager) == paths::Roots { before, after });
    fs::rename (tree.root / "parked", tree.root / "custom" / "m");
    fs::remove_all (after);
    CHECK_FALSE (duskstudio::juce_compat::primeCustomLv2 (manager, { custom }, true));
    CHECK (duskstudio::juce_compat::customLv2Bundles (manager) == paths::Roots { before, after });
    fs::rename (tree.root / "custom" / "m", tree.root / "parked");
    CHECK (format.calls.size() == 3);
   #endif
}

TEST_CASE ("Loading an LV2 plug-in primes only bundles the last folder walk found", "[plugins][paths]")
{
    Tree tree;
    const auto custom = tree.directory ("custom");
    const auto first = tree.directory ("custom/vendor/First.lv2");
    const auto second = tree.directory ("custom/vendor/Second.lv2");
    duskstudio::juce_compat::FormatManager scanned;
    auto& scanFormat = addLv2<RecordingLv2Format> (scanned);
    REQUIRE (duskstudio::juce_compat::primeCustomLv2 (scanned, { custom }, true));
    CHECK (scanFormat.calls == std::vector<std::string> { first.u8string(), second.u8string() });
    scanFormat.calls.clear();
    fs::rename (custom, tree.root / "unplugged");
    duskstudio::juce_compat::primeLv2Plugin (scanned, "urn:Second");
    CHECK (scanFormat.calls == std::vector<std::string> { "urn:Second" });

    duskstudio::juce_compat::FormatManager restarted;
    auto& format = addLv2<RecordingLv2Format> (restarted);
    duskstudio::juce_compat::restoreCustomLv2Bundles (restarted, duskstudio::juce_compat::customLv2Bundles (scanned));
    duskstudio::juce_compat::primeLv2Plugin (restarted, "urn:First");
    CHECK (format.calls == std::vector<std::string> { "urn:First", first.u8string(), "urn:First" });
    format.calls.clear();
    duskstudio::juce_compat::primeLv2Plugin (restarted, "urn:Missing");
    CHECK (format.calls == std::vector<std::string> { "urn:Missing", second.u8string(), "urn:Missing" });
    format.calls.clear();
    duskstudio::juce_compat::primeLv2Plugin (restarted, "urn:Missing");
    CHECK (format.calls == std::vector<std::string> { "urn:Missing" });
}

TEST_CASE ("A custom folder whose links loop finishes discovery in every format that sees it", "[plugins][paths]")
{
    Tree tree;
    const auto custom = tree.directory ("custom");
    std::error_code linkError;
    fs::create_directory_symlink (".", custom / "a", linkError);
    if (! linkError) fs::create_directory_symlink (".", custom / "b", linkError);
    INFO ("directory links: " << (linkError ? linkError.message() : std::string ("a -> . and b -> .")));
    const paths::Roots roots { custom };
    juce::KnownPluginList known;
    juce::String scanned;

    SECTION ("VST3 keeps JUCE's walk to the default folders and finds custom bundles natively")
    {
        const auto standard = tree.directory ("standard/Default.vst3").parent_path();
        const auto looped = tree.directory ("custom/Vendor/Looped.vst3");
        LinkFollowingVst3Format format;
        format.defaults.add (juce::File (standard.u8string()));
        {
            juce::PluginDirectoryScanner scanner (known, format,
                duskstudio::juce_compat::pluginSearchLocations (format, roots), true, {}, false);
            while (scanner.scanNextFile (false, scanned)) {}
        }
        CHECK_FALSE (format.runaway);
        CHECK (format.searched == std::vector<fs::path> { standard });
        REQUIRE (known.getNumTypes() == 1);
        CHECK (fs::u8path (known.getTypes()[0].fileOrIdentifier.toStdString()) == standard / "Default.vst3");

        const auto nativeRoots = paths::extend ({ standard }, roots);
        auto expected = paths::Roots { standard / "Default.vst3", looped };
        std::sort (expected.begin(), expected.end());
        CHECK (paths::findBundles (nativeRoots, ".vst3") == expected);
        std::atomic<bool> abort { true };
        CHECK (paths::findBundles (nativeRoots, ".vst3", &abort).empty());
    }
    SECTION ("LV2 lists each custom folder once and primes nested bundles")
    {
        const auto bundle = tree.directory ("custom/Vendor/Looped.lv2");
        REQUIRE (dusk::fs::writeStringToFile (bundle / "manifest.ttl", R"ttl(
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix doap: <http://usefulinc.com/ns/doap#> .
<urn:duskstudio:test:looped-folder> a lv2:Plugin ;
 doap:name "Looped folder fixture" ; lv2:binary <missing.so> ;
 lv2:port [ a lv2:InputPort, lv2:AudioPort ; lv2:index 0 ; lv2:symbol "in" ; lv2:name "Input" ],
          [ a lv2:OutputPort, lv2:AudioPort ; lv2:index 1 ; lv2:symbol "out" ; lv2:name "Output" ] .
)ttl"));
        RegisteredLv2Format registered;
        auto& format = *registered.format;
        duskstudio::juce_compat::primeCustomLv2 (registered.manager, roots);
        {
            juce::PluginDirectoryScanner scanner (known, format,
                duskstudio::juce_compat::pluginSearchLocations (format, roots), true, {}, false);
            while (scanner.scanNextFile (false, scanned)) {}
        }
        const auto found = known.getTypes();
        CHECK (std::any_of (found.begin(), found.end(), [] (const auto& item)
        { return item.fileOrIdentifier == "urn:duskstudio:test:looped-folder"; }));
    }
}

TEST_CASE ("Invalid plugin folder settings do not create search roots", "[plugins][paths]")
{
    Tree tree;
    const auto file = tree.root / "plugin-search-paths.json";
    CHECK (paths::load (file).empty());
    for (const auto* text : { "{broken", "{}", "[null,12,true]", "[\"bad\\u0000path\"]" })
    {
        CAPTURE (text);
        REQUIRE (dusk::fs::writeStringToFile (file, text));
        CHECK (paths::load (file).empty());
    }
    const auto blocking = tree.root / "file";
    REQUIRE (dusk::fs::writeStringToFile (blocking, "keep"));
    std::string error;
    CHECK_FALSE (paths::save ({ tree.root }, error, blocking / "settings.json"));
    CHECK_FALSE (error.empty());
    CHECK (dusk::fs::loadFileAsString (blocking) == "keep");
    const auto destination = tree.directory ("existing.json");
    REQUIRE (dusk::fs::writeStringToFile (destination / "keep", "unchanged"));
    CHECK_FALSE (paths::save ({ tree.root }, error, destination));
    CHECK (dusk::fs::loadFileAsString (destination / "keep") == "unchanged");
    for (const auto& entry : fs::directory_iterator (tree.root))
        CHECK (entry.path().extension() != ".tmp");
}

TEST_CASE ("Nested custom LV2 bundles prime the parent scanner and fresh scan child", "[plugins][paths][ipc]")
{
    Tree tree;
   #if defined(_WIN32)
    const auto configDirectory = tree.root / fs::u8path ("config \xc3\xa9 \xe9\x9f\xb3\xe8\x89\xb2");
    const ConfigOverride config (configDirectory);
   #else
    const ConfigOverride config (tree.root / "config");
   #endif
   #if defined(_WIN32)
    const auto custom = fs::u8path ("Plug-ins \xc3\xa9; collection");
   #else
    const auto custom = fs::u8path ("Plug-ins \xc3\xa9: collection");
   #endif
    const auto bundle = tree.directory (custom / "Vendor" / "Nested" / "Fixture.lv2");
   #if defined(_WIN32)
    namespace wide = duskstudio::lv2WindowsPaths;
    std::unique_ptr<char, decltype (&std::free)> expanded (
        wide::expandEnvironment ("%DUSKSTUDIO_CONFIG_DIR%\\LV2"), &std::free);
    REQUIRE (expanded != nullptr);
    CHECK (fs::u8path (expanded.get()) == configDirectory / "LV2");
    const auto sample = bundle / fs::u8path ("sample \xe9\x9f\xb3\xe8\x89\xb2.bin");
    const auto sampleName = sample.u8string();
    std::unique_ptr<FILE, decltype (&std::fclose)> file (wide::openFile (sampleName.c_str(), "wb"), &std::fclose);
    REQUIRE (file != nullptr);
    REQUIRE (std::fwrite ("sample", 1, 6, file.get()) == 6);
    file.reset();
    struct _stat64 info {};
    REQUIRE (wide::fileInfo (sampleName.c_str(), &info) == 0);
    CHECK (info.st_size == 6);
    CHECK (wide::pathAccess (sampleName.c_str(), 0) == 0);
    std::vector<std::string> names;
    const auto bundleName = bundle.u8string();
    wide::forEachDirectory (bundleName.c_str(), &names, [] (const char*, const char* name, void* data)
    { static_cast<std::vector<std::string>*> (data)->emplace_back (name); });
    CHECK (std::find (names.begin(), names.end(), sample.filename().u8string()) != names.end());
    CHECK (wide::removeFile (sampleName.c_str()) == 0);
    const auto directoryName = (bundle / fs::u8path ("folder \xe9\x9f\xb3\xe8\x89\xb2")).u8string();
    REQUIRE (wide::makeDirectory (directoryName.c_str()) == 0);
    CHECK (wide::removeDirectory (directoryName.c_str()) != FALSE);
   #endif
    const std::string uri = "urn:duskstudio:test:custom-paths";
    REQUIRE (dusk::fs::writeStringToFile (bundle / "manifest.ttl", R"ttl(
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix doap: <http://usefulinc.com/ns/doap#> .
<urn:duskstudio:test:custom-paths> a lv2:Plugin ;
 doap:name "Custom paths fixture" ; lv2:binary <missing.so> ;
 lv2:port [ a lv2:InputPort, lv2:AudioPort ; lv2:index 0 ; lv2:symbol "in" ; lv2:name "Input" ],
          [ a lv2:OutputPort, lv2:AudioPort ; lv2:index 1 ; lv2:symbol "out" ; lv2:name "Output" ] .
)ttl"));
    const paths::Roots roots { tree.root / custom };
    std::string error;
    REQUIRE (paths::save (roots, error));
    RegisteredLv2Format registered;
    auto& format = *registered.format;
    duskstudio::juce_compat::primeCustomLv2 (registered.manager, roots);
    juce::KnownPluginList known;
    juce::PluginDirectoryScanner scanner (known, format,
        duskstudio::juce_compat::pluginSearchLocations (format, roots), true, {}, false);
    juce::String scanned;
    while (scanner.scanNextFile (false, scanned)) {}
    const auto found = known.getTypes();
    const auto fixture = std::find_if (found.begin(), found.end(), [&] (const auto& item)
    { return item.fileOrIdentifier.toStdString() == uri; });
    REQUIRE (fixture != found.end());
    CHECK (fixture->numInputChannels == 1);
    CHECK (fixture->numOutputChannels == 1);
    RegisteredLv2Format freshRegistered;
    auto& fresh = *freshRegistered.format;
    duskstudio::juce_compat::primeCustomLv2 (freshRegistered.manager, paths::load());
    juce::OwnedArray<juce::PluginDescription> descriptions;
    fresh.findAllTypesForFile (descriptions, uri);
    REQUIRE (descriptions.size() == 1);
    CHECK (descriptions[0]->fileOrIdentifier.toStdString() == uri);
    juce::ChildProcess child;
    REQUIRE (child.start (juce::StringArray { DUSKSTUDIO_PLUGIN_HOST_PATH, "--scan", "LV2", uri }));
    auto reading = std::async (std::launch::async, [&child]
    {
        std::string output;
        char buffer[4096];
        for (int count = 0; (count = child.readProcessOutput (buffer, sizeof (buffer))) > 0;)
        {
            output.append (buffer, static_cast<std::size_t> (count));
            if (output.size() > 1024 * 1024) output.erase (0, output.size() - 1024 * 1024);
        }
        return output;
    });
    const bool stopped = child.waitForProcessToFinish (15000);
    if (! stopped) child.kill();
    const auto output = reading.get();
    REQUIRE (stopped);
    INFO (output);
    REQUIRE (child.getExitCode() == 0);
    const auto parsed = duskstudio::scanproto::parsePayload (duskstudio::scanproto::extractPayload (output));
    REQUIRE (parsed);
    REQUIRE (parsed->size() == 1);
    CHECK (parsed->front().location == uri);
   #if defined(DUSKSTUDIO_MIDI_PROBE_LV2_FIXTURE_PATH)
    fs::copy (fs::u8path (DUSKSTUDIO_MIDI_PROBE_LV2_FIXTURE_PATH),
              bundle.parent_path() / "Runtime.lv2", fs::copy_options::recursive);
    REQUIRE (dusk::fs::writeStringToFile (
        bundle.parent_path() / "Runtime.lv2" / "require-creator-thread", "1"));
    RegisteredLv2Format runtimeRegistered;
    auto& runtimeFormat = *runtimeRegistered.format;
    duskstudio::juce_compat::primeCustomLv2 (runtimeRegistered.manager, roots);
    juce::OwnedArray<juce::PluginDescription> runtimeDescriptions;
    runtimeFormat.findAllTypesForFile (runtimeDescriptions, "urn:duskstudio:test:midi-probe");
    REQUIRE (runtimeDescriptions.size() == 1);
    CHECK (runtimeDescriptions[0]->isInstrument);
    duskstudio::ipc::RemotePluginConnection connection;
    REQUIRE (connection.connect (DUSKSTUDIO_PLUGIN_HOST_PATH, "--ipc-host", error));
    int inputs = 0, outputs = 0, latency = 0;
    bool instrument = false;
    const auto xml = runtimeDescriptions[0]->createXml()->toString().toStdString();
    REQUIRE (connection.loadPlugin (xml, 48000.0, 64,
                                     inputs, outputs, latency, instrument, error));
    CHECK (outputs == 2);
    CHECK (instrument);
    for (const double rate : { 44100.0, 96000.0, 48000.0 })
    {
        REQUIRE (connection.prepareToPlay (rate, 64, error));
        std::vector<std::uint8_t> state;
        REQUIRE (connection.getState (state, error));
        REQUIRE_FALSE (state.empty());
        REQUIRE (connection.setState (state.data(), static_cast<std::uint32_t> (state.size()), error));
        dusk::MidiBuffer midi;
        const std::uint8_t note[] { 0x90, 60, 100 };
        midi.addEvent (note, sizeof (note), 0);
        REQUIRE (connection.processBlockSync (nullptr, 0, 2, 64, midi, 500'000'000LL));
        CHECK_THAT (connection.readOutChannel (0)[0], Catch::Matchers::WithinAbs (0.1, 1.0e-6));
    }
    REQUIRE (connection.loadPlugin (xml, 48000.0, 64,
                                    inputs, outputs, latency, instrument, error));
    REQUIRE (connection.release (error));
    CHECK_FALSE (connection.prepareToPlay (48000.0, 64, error));
    REQUIRE (connection.loadPlugin (xml, 48000.0, 64,
                                    inputs, outputs, latency, instrument, error));
    dusk::MidiBuffer reloadedMidi;
    const std::uint8_t reloadedNote[] { 0x90, 64, 100 };
    reloadedMidi.addEvent (reloadedNote, sizeof (reloadedNote), 0);
    REQUIRE (connection.processBlockSync (nullptr, 0, 2, 64, reloadedMidi, 500'000'000LL));
    CHECK_THAT (connection.readOutChannel (0)[0], Catch::Matchers::WithinAbs (0.1, 1.0e-6));
    REQUIRE (connection.release (error));
    REQUIRE (connection.release (error));
   #endif
    REQUIRE (paths::save ({}, error));
    juce::OwnedArray<juce::PluginDescription> retained;
    format.findAllTypesForFile (retained, uri);
    CHECK (retained.size() == 1);
    RegisteredLv2Format restartedRegistered;
    auto& restarted = *restartedRegistered.format;
    duskstudio::juce_compat::primeCustomLv2 (restartedRegistered.manager, paths::load());
    juce::OwnedArray<juce::PluginDescription> removed;
    restarted.findAllTypesForFile (removed, uri);
    CHECK (removed.isEmpty());
}
