#include <catch2/catch_test_macros.hpp>

#include "ui/AppConfig.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
namespace stdfs = std::filesystem;

// The store is located from the environment, so a test can point it at a
// scratch directory instead of the developer's real configuration.
const char* storeEnvVar()
{
   #if defined(_WIN32)
    return "APPDATA";
   #else
    return "HOME";
   #endif
}

void setEnv (const char* name, const char* value)
{
   #if defined(_WIN32)
    _putenv_s (name, value != nullptr ? value : "");
   #else
    if (value != nullptr) ::setenv (name, value, 1);
    else                  ::unsetenv (name);
   #endif
}

// Redirects the store into a scratch directory and puts the environment back
// afterwards. XDG_CONFIG_HOME is redirected too: left alone it outranks HOME on
// Linux, and the store would land in the real configuration directory.
class ScopedStore
{
public:
    explicit ScopedStore (const char* name)
    {
        if (const char* existing = std::getenv (storeEnvVar()))
            previousStore = existing;
        if (const char* existing = std::getenv ("XDG_CONFIG_HOME"))
            previousXdg = existing;

        root = stdfs::temp_directory_path() / "dusk-appconfig" / name;
        std::error_code ec;
        stdfs::remove_all (root, ec);
        stdfs::create_directories (root, ec);
        setEnv (storeEnvVar(), root.string().c_str());
        setEnv ("XDG_CONFIG_HOME", (root / ".config").string().c_str());
    }

    ~ScopedStore()
    {
        setEnv (storeEnvVar(), previousStore.empty() ? nullptr : previousStore.c_str());
        setEnv ("XDG_CONFIG_HOME", previousXdg.empty() ? nullptr : previousXdg.c_str());
    }

    // Located rather than constructed: the directory layout under the
    // configuration root differs per platform, and the test cares that the
    // store round-trips, not where it sits.
    stdfs::path file() const
    {
        std::error_code ec;
        for (const auto& entry : stdfs::recursive_directory_iterator (root, ec))
            if (entry.path().filename() == "app-config.properties")
                return entry.path();
        return {};
    }

    // Forces the store into existence so a test can seed its contents.
    void create() const { duskstudio::appconfig::setScanPluginsOnStartup (false); }

    std::string contents() const
    {
        std::ifstream in (file(), std::ios::binary);
        return { std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char>() };
    }

    void write (const std::string& raw) const
    {
        std::ofstream out (file(), std::ios::binary | std::ios::trunc);
        out << raw;
    }

private:
    stdfs::path root;
    std::string previousStore;
    std::string previousXdg;
};
} // namespace

TEST_CASE ("An absent notepad switch leaves the notepad available", "[appconfig]")
{
    const ScopedStore store { "default" };
    CHECK (duskstudio::appconfig::getNotepadEnabled());
}

TEST_CASE ("The notepad switch round-trips", "[appconfig]")
{
    const ScopedStore store { "roundtrip" };
    duskstudio::appconfig::setNotepadEnabled (false);
    CHECK_FALSE (duskstudio::appconfig::getNotepadEnabled());
    duskstudio::appconfig::setNotepadEnabled (true);
    CHECK (duskstudio::appconfig::getNotepadEnabled());
}

// The manual tells users to set keys in this file by hand. A store whose last
// line has no terminator silently glues an appended key onto the previous one,
// which reads back as a corrupted value for that previous key and as no value
// at all for the appended one.
TEST_CASE ("The store leaves its last line terminated", "[appconfig]")
{
    const ScopedStore store { "terminated" };
    duskstudio::appconfig::setNotepadEnabled (false);
    REQUIRE_FALSE (store.file().empty());

    const auto raw = store.contents();
    REQUIRE_FALSE (raw.empty());
    CHECK (raw.back() == '\n');
}

TEST_CASE ("A key appended by hand is read back", "[appconfig]")
{
    const ScopedStore store { "appended" };
    duskstudio::appconfig::setScanPluginsOnStartup (true);
    REQUIRE_FALSE (store.file().empty());
    // Exactly what a user or a shell redirection does to the file.
    {
        std::ofstream out (store.file(), std::ios::binary | std::ios::app);
        out << "notepad_enabled=0\n";
    }

    CHECK_FALSE (duskstudio::appconfig::getNotepadEnabled());
    CHECK (duskstudio::appconfig::getScanPluginsOnStartup());
}

TEST_CASE ("Writing one key preserves the others", "[appconfig]")
{
    const ScopedStore store { "preserve" };
    store.create();
    store.write ("# a comment\nscan_plugins_on_startup=1\nnotepad_enabled=0\n");

    duskstudio::appconfig::setNotepadEnabled (true);

    CHECK (duskstudio::appconfig::getNotepadEnabled());
    CHECK (duskstudio::appconfig::getScanPluginsOnStartup());
    CHECK (store.contents().find ("# a comment") != std::string::npos);
}
