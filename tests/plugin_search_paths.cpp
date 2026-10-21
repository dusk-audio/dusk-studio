#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/PluginSearchPaths.h"
#include "engine/JuceCompat.h"
#include "engine/ipc/PluginScanProtocol.h"
#include "engine/ipc/RemotePluginConnection.h"

#if defined(_WIN32)
#include "../cmake/JuceLv2WindowsPaths.h"
#endif

#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>

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

CountingLv2Format& addCountingLv2 (juce::AudioPluginFormatManager& manager)
{
    auto format = std::make_unique<CountingLv2Format>();
    auto& result = *format;
   #if DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS
    manager.addFormat (std::move (format));
   #else
    manager.addFormat (format.release());
   #endif
    return result;
}

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
    duskstudio::juce_compat::primeCustomLv2 (manager);
    REQUIRE (format.visits == 1);
    duskstudio::juce_compat::primeCustomLv2 (manager);
    CHECK (format.visits == 1);

    SECTION ("new manager lifetimes prime independently")
    {
        std::optional<duskstudio::juce_compat::FormatManager> fresh;
        for (int lifetime = 0; lifetime < 2; ++lifetime)
        {
            fresh.emplace();
            auto& other = addCountingLv2 (*fresh);
            duskstudio::juce_compat::primeCustomLv2 (*fresh);
            CHECK (other.visits == 1);
            fresh.reset();
        }
    }
    SECTION ("changed and empty roots invalidate priming")
    {
        REQUIRE (paths::save ({}, error));
        duskstudio::juce_compat::primeCustomLv2 (manager);
        REQUIRE (paths::save (roots, error));
        duskstudio::juce_compat::primeCustomLv2 (manager);
        CHECK (format.visits == 2);
        const paths::Roots other { tree.directory ("other") };
        tree.directory ("other/Second.lv2");
        REQUIRE (paths::save (other, error));
        duskstudio::juce_compat::primeCustomLv2 (manager);
        CHECK (format.visits == 3);
    }
    SECTION ("explicit refresh discovers new bundles under unchanged roots")
    {
        tree.directory ("custom/new/Second.lv2");
        duskstudio::juce_compat::primeCustomLv2 (manager);
        CHECK (format.visits == 1);
        duskstudio::juce_compat::primeCustomLv2 (manager, roots, true);
        CHECK (format.visits == 3);
        duskstudio::juce_compat::primeCustomLv2 (manager);
        CHECK (format.visits == 3);
    }
    SECTION ("reconnected roots retry discovery")
    {
        const auto unplugged = tree.root / "unplugged";
        fs::rename (roots.front(), unplugged);
        duskstudio::juce_compat::primeCustomLv2 (manager);
        CHECK (format.visits == 1);
        fs::rename (unplugged, roots.front());
        duskstudio::juce_compat::primeCustomLv2 (manager);
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
        duskstudio::juce_compat::primeCustomLv2 (manager);
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
        CHECK (std::find (linked.begin(), linked.end(), external) != linked.end());
    }
    std::atomic<bool> abort { true };
    CHECK (paths::findBundles (roots, ".vst3", &abort).empty());
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
    duskstudio::juce_compat::primeCustomLv2 (format, roots);
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
    duskstudio::juce_compat::primeCustomLv2 (fresh, paths::load());
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
    duskstudio::juce_compat::primeCustomLv2 (runtimeFormat, roots);
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
    duskstudio::juce_compat::primeCustomLv2 (restarted, paths::load());
    juce::OwnedArray<juce::PluginDescription> removed;
    restarted.findAllTypesForFile (removed, uri);
    CHECK (removed.isEmpty());
}
