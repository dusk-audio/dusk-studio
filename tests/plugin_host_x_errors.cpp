#include <catch2/catch_test_macros.hpp>

#include "TestTempDirectory.h"
#include "engine/PluginManager.h"
#include "engine/ScanProcess.h"
#include "engine/ipc/PluginScanProtocol.h"
#include "engine/ipc/RemotePluginConnection.h"

#include <juce_core/juce_core.h>

#include <X11/Xlib.h>
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if DUSKSTUDIO_HAS_OOP_PLUGINS && defined (DUSKSTUDIO_STALE_WINDOW_VST3_FIXTURE_PATH)
using namespace duskstudio;

namespace
{
constexpr const char* kProbeVariable = "DUSKSTUDIO_STALE_WINDOW_PROBE";
constexpr const char* kProbeLogVariable = "DUSKSTUDIO_STALE_WINDOW_PROBE_LOG";

// These cases open windows, so they run only where the ctest entry has started
// a private X server for them, never on the desktop session of whoever runs the
// suite by hand.
bool onPrivateDisplay()
{
    const char* marker = std::getenv ("DUSKSTUDIO_TEST_PRIVATE_DISPLAY");
    return marker != nullptr && std::string (marker) == "1"
        && std::getenv ("WAYLAND_DISPLAY") == nullptr;
}

// Plays the host: a window it created and then destroyed. The connection stays
// open for the whole case, because the server hands a closed connection's
// client slot, and with it the same ID range, to the next client that
// connects, which could be the child.
struct DestroyedHostWindow
{
    DestroyedHostWindow()
    {
        display = XOpenDisplay (nullptr);
        if (display == nullptr) return;
        window = XCreateSimpleWindow (display, DefaultRootWindow (display), 0, 0, 16, 16, 0, 0, 0);
        XDestroyWindow (display, window);
        XSync (display, False);
    }

    ~DestroyedHostWindow()
    {
        if (display != nullptr) XCloseDisplay (display);
    }

    Display* display = nullptr;
    ::Window window = 0;
};

class ScopedEnvironment
{
public:
    ScopedEnvironment (const char* nameIn, const std::string& value) : name (nameIn)
    {
        if (const auto* existing = std::getenv (name)) previous = existing;
        ::setenv (name, value.c_str(), 1);
    }

    ~ScopedEnvironment()
    {
        if (previous) ::setenv (name, previous->c_str(), 1);
        else          ::unsetenv (name);
    }

private:
    const char* name;
    std::optional<std::string> previous;
};

// Scanned before the probe is armed: the scan child loads the fixture too.
std::optional<PluginDescriptor> staleWindowDescriptor()
{
    ScanProcess child;
    if (! child.start ({ DUSKSTUDIO_PLUGIN_HOST_PATH, "--scan", "VST3",
                         DUSKSTUDIO_STALE_WINDOW_VST3_FIXTURE_PATH }))
        return std::nullopt;
    std::string output;
    std::vector<char> chunk (4096);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto read = child.readOutput (chunk.data(), chunk.size(), 50);
        if (read.status == ScanProcess::ReadStatus::Data) output.append (chunk.data(), read.bytes);
        else if (read.status != ScanProcess::ReadStatus::Waiting) break;
    }
    const auto parsed = scanproto::parsePayload (scanproto::extractPayload (output));
    if (! parsed.has_value() || parsed->size() != 1) return std::nullopt;
    return parsed->front();
}

// The child inherits this process's stderr, where both Xlib's default handler
// and the child's own handler print. Catch2 reports on stdout, so an assertion
// that fails while this is held still shows.
class ChildStderr
{
public:
    explicit ChildStderr (const std::filesystem::path& file) : path (file)
    {
        std::fflush (stderr);
        // Close-on-exec, so a child spawned meanwhile gets only the file.
        saved = ::fcntl (STDERR_FILENO, F_DUPFD_CLOEXEC, 0);
        const int fd = ::open (path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (saved >= 0 && fd >= 0) ::dup2 (fd, STDERR_FILENO);
        if (fd >= 0) ::close (fd);
    }

    ~ChildStderr() { restore(); }

    void restore()
    {
        if (saved < 0) return;
        std::fflush (stderr);
        ::dup2 (saved, STDERR_FILENO);
        ::close (saved);
        saved = -1;
    }

    std::string text() const
    {
        std::ifstream in (path);
        return { std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char>() };
    }

    ChildStderr (const ChildStderr&) = delete;
    ChildStderr& operator= (const ChildStderr&) = delete;

private:
    std::filesystem::path path;
    int saved = -1;
};

std::vector<std::string> probeLog (const std::filesystem::path& path)
{
    std::vector<std::string> lines;
    std::ifstream in (path);
    for (std::string line; std::getline (in, line);)
        lines.push_back (line);
    return lines;
}

// Talks to the child directly rather than through a slot, which would retry a
// failed load in this process.
struct Sandbox
{
    bool start (const PluginDescriptor& descriptor)
    {
        std::string error;
        if (! connection.connect (DUSKSTUDIO_PLUGIN_HOST_PATH, "--ipc-host", error)) return false;
        const PluginManager manager;
        int inputs = 0, outputs = 0, latency = 0;
        bool instrument = false;
        return connection.loadPlugin (manager.descriptorToLegacyXml (descriptor).toStdString(),
                                      48000.0, 512, inputs, outputs, latency, instrument, error);
    }

    bool showEditor()
    {
        std::uint64_t windowId = 0;
        int width = 0, height = 0;
        std::string error;
        return connection.showEditor (windowId, width, height, error) && windowId != 0;
    }

    bool hideEditor()
    {
        std::string error;
        return connection.hideEditor (error);
    }

    void release()
    {
        std::string error;
        (void) connection.release (error);
    }

    bool answers()
    {
        std::string error;
        return connection.ping (2000, error);
    }

    bool exited()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (connection.pollReaper()) return true;
            std::this_thread::sleep_for (std::chrono::milliseconds (20));
        }
        return false;
    }

    ipc::RemotePluginConnection connection;
};
} // namespace

TEST_CASE ("A sandbox child survives its editor asking about a destroyed host window",
           "[.][ipc][xvfb][issue-966]")
{
    if (! onPrivateDisplay()) SKIP ("needs the private X server its ctest entry starts");

    const auto descriptor = staleWindowDescriptor();
    REQUIRE (descriptor.has_value());

    DestroyedHostWindow host;
    REQUIRE (host.display != nullptr);
    test::TempDirectory temp ("dusk-host-x-errors-");
    const auto log = temp.path() / "probe.log";
    const ScopedEnvironment probe (kProbeVariable, "editor:" + std::to_string (host.window));
    const ScopedEnvironment probeLogPath (kProbeLogVariable, log.string());

    Sandbox sandbox;
    ChildStderr childStderr (temp.path() / "child-stderr.txt");
    REQUIRE (sandbox.start (*descriptor));

    // Open and close twice: the child must still answer after each error, and
    // a second editor must still open after the first one's teardown hit one.
    for (int round = 0; round < 2; ++round)
    {
        INFO ("round " << round);
        REQUIRE (sandbox.showEditor());
        REQUIRE (sandbox.answers());
        REQUIRE (sandbox.hideEditor());
        REQUIRE (sandbox.answers());
    }
    REQUIRE_FALSE (sandbox.connection.pollReaper());
    childStderr.restore();

    // XQueryTree returns 0 when the error came back, so every query reached the
    // server, failed there, and returned to the plug-in.
    const std::vector<std::string> expected {
        "attached querying", "attached 0", "removed querying", "removed 0",
        "attached querying", "attached 0", "removed querying", "removed 0" };
    REQUIRE (probeLog (log) == expected);

    const auto text = childStderr.text();
    std::ostringstream logged;
    logged << "[dusk-studio-plugin-host/X] ignored stale host window 0x" << std::hex << host.window << ' ';
    std::size_t times = 0;
    for (auto at = text.find (logged.str()); at != std::string::npos; at = text.find (logged.str(), at + 1))
        ++times;
    INFO ("child stderr:\n" << text);
    REQUIRE (times == 4);
}

TEST_CASE ("A sandbox child still ends on an X error that is not a stale host window",
           "[.][ipc][xvfb][issue-966]")
{
    if (! onPrivateDisplay()) SKIP ("needs the private X server its ctest entry starts");

    const auto descriptor = staleWindowDescriptor();
    REQUIRE (descriptor.has_value());

    DestroyedHostWindow host;
    REQUIRE (host.display != nullptr);
    test::TempDirectory temp ("dusk-host-x-errors-");
    const auto log = temp.path() / "probe.log";
    const ScopedEnvironment probeLogPath (kProbeLogVariable, log.string());
    const auto hostWindow = std::to_string (host.window);

    // The error must reach the handler that was there before, Xlib's own, which
    // prints this report and ends the child. A child that died any other way,
    // or a handler that swallowed the error, would not leave it.
    const auto requireXlibDefaultEnding = [] (const std::string& childStderr)
    {
        INFO ("child stderr:\n" << childStderr);
        REQUIRE (childStderr.find ("X Error of failed request") != std::string::npos);
        REQUIRE (childStderr.find ("BadWindow") != std::string::npos);
        REQUIRE (childStderr.find ("ignored stale host window") == std::string::npos);
    };

    SECTION ("a window the child created itself, while its editor is open")
    {
        const ScopedEnvironment probe (kProbeVariable, "own");
        Sandbox sandbox;
        ChildStderr childStderr (temp.path() / "child-stderr.txt");
        const bool loaded = sandbox.start (*descriptor);
        const bool shown = loaded && sandbox.showEditor();
        const bool exited = sandbox.exited();
        childStderr.restore();

        REQUIRE (loaded);
        REQUIRE_FALSE (shown);
        REQUIRE (exited);
        REQUIRE (probeLog (log) == std::vector<std::string> { "attached querying" });
        requireXlibDefaultEnding (childStderr.text());
    }

    SECTION ("a destroyed host window, before any editor exists")
    {
        const ScopedEnvironment probe (kProbeVariable, "load:" + hostWindow);
        Sandbox sandbox;
        ChildStderr childStderr (temp.path() / "child-stderr.txt");
        const bool loaded = sandbox.start (*descriptor);
        const bool exited = sandbox.exited();
        childStderr.restore();

        REQUIRE_FALSE (loaded);
        REQUIRE (exited);
        REQUIRE (probeLog (log) == std::vector<std::string> { "load querying" });
        requireXlibDefaultEnding (childStderr.text());
    }

    SECTION ("a destroyed host window, after the editor has closed")
    {
        const ScopedEnvironment probe (kProbeVariable, "after:" + hostWindow);
        Sandbox sandbox;
        ChildStderr childStderr (temp.path() / "child-stderr.txt");
        const bool loaded = sandbox.start (*descriptor);
        const bool shown = loaded && sandbox.showEditor();
        const bool hidden = shown && sandbox.hideEditor();
        if (hidden) sandbox.release();
        const bool exited = sandbox.exited();
        childStderr.restore();

        REQUIRE (loaded);
        REQUIRE (shown);
        REQUIRE (hidden);
        REQUIRE (exited);
        REQUIRE (probeLog (log) == std::vector<std::string> { "after querying" });
        requireXlibDefaultEnding (childStderr.text());
    }
}
#endif
