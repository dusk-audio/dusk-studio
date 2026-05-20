#include "CrashHandler.h"

#include <atomic>
#include <csignal>
#include <memory>

namespace duskstudio::crash_handler
{
namespace
{
std::atomic<bool>             installed { false };
juce::String                  cachedAppVersion;
juce::File                    cachedCrashDir;
juce::File                    cachedLogFile;
std::unique_ptr<juce::FileLogger> ownedLogger;

juce::File baseDir()
{
    // ~/.local/share/Focal on Linux, ~/Library/Application Support/Focal
    // on macOS, %APPDATA%/Focal on Windows. Same root used by the
    // Patreon support docs so users know where to look.
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
              .getChildFile ("Focal");
}

juce::File makeLogFile()
{
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d");
    return baseDir().getChildFile ("log").getChildFile (juce::String ("dusk-studio-")
                                                              + stamp + ".log");
}

// JUCE invokes this from a signal-handler context (SIGSEGV / SIGABRT /
// SIGFPE). Strictly we should be async-signal-safe here, but the
// process is dying — best effort is to write a paste-able report.
// FileOutputStream allocations may fail under stack corruption; if so,
// the OS still produces a core dump.
void crashCallback (void* /*platformSpecific*/)
{
    juce::File report = cachedCrashDir.getChildFile (
        juce::String ("crash-")
            + juce::Time::getCurrentTime().formatted ("%Y%m%dT%H%M%S")
            + ".txt");
    report.getParentDirectory().createDirectory();

    juce::FileOutputStream stream (report);
    if (! stream.openedOk()) return;

    stream << "Focal crash report\n"
           << "==================\n"
           << "Time:         " << juce::Time::getCurrentTime().toString (true, true) << "\n"
           << "App version:  " << cachedAppVersion << "\n"
           << "JUCE version: " << juce::SystemStats::getJUCEVersion() << "\n"
           << "OS:           " << juce::SystemStats::getOperatingSystemName() << "\n"
           << "CPU:          " << juce::SystemStats::getCpuModel()
           << " (" << juce::SystemStats::getNumCpus() << " cores)\n"
           << "RAM:          " << juce::SystemStats::getMemorySizeInMegabytes() << " MB\n\n"
           << "Backtrace\n"
           << "---------\n"
           << juce::SystemStats::getStackBacktrace() << "\n\n";

    // Tail the current log file so the crash report carries a window
    // of recent activity. Read backward from end-of-file with a bounded
    // 64 KB buffer — loadFileAsString would allocate the entire log,
    // and a long-running session's log can be many MB. Crashing
    // processes are exactly the case where big allocations fail; the
    // smaller bounded read is the most likely to succeed.
    if (cachedLogFile.existsAsFile())
    {
        constexpr juce::int64 kTailBytes = 64 * 1024;
        juce::FileInputStream in (cachedLogFile);
        if (in.openedOk())
        {
            const auto total = in.getTotalLength();
            const auto bytes = juce::jmin (total, kTailBytes);
            in.setPosition (total - bytes);
            juce::MemoryBlock buf;
            in.readIntoMemoryBlock (buf, (int) bytes);
            stream << "Recent log (last " << (int) buf.getSize() << " bytes)\n"
                   << "----------\n";
            stream.write (buf.getData(), buf.getSize());
            stream << "\n";
        }
    }

    stream.flush();

   #if JUCE_LINUX || JUCE_MAC
    // Chain to default signal disposition + re-raise so the OS still
    // produces a core dump for post-mortem debugging. JUCE installs
    // this callback for SIGSEGV / SIGABRT / SIGFPE / SIGILL / SIGBUS;
    // resetting SIGABRT and raising it makes the default abort()
    // semantics fire after our report is on disk. Without this the
    // process exits silently with no core.
    std::signal (SIGABRT, SIG_DFL);
    std::raise  (SIGABRT);
   #endif
}
} // namespace

void install (const juce::String& appVersion)
{
    // Always refresh version — a second install() call from a future
    // hot-reload / test harness updates the crash report header even
    // though the FileLogger + signal handler stay registered as-is.
    cachedAppVersion = appVersion;

    bool expected = false;
    if (! installed.compare_exchange_strong (expected, true)) return;

    baseDir().createDirectory();

    cachedLogFile = makeLogFile();
    cachedLogFile.getParentDirectory().createDirectory();

    // Replace any pre-existing logger (none in normal flow, but tests
    // may have installed one). FileLogger appends; daily rotation comes
    // from the date in the filename — next-day startup picks a new file.
    ownedLogger = std::make_unique<juce::FileLogger> (
        cachedLogFile,
        "Focal " + appVersion + " — "
            + juce::Time::getCurrentTime().toString (true, true));
    juce::Logger::setCurrentLogger (ownedLogger.get());

    cachedCrashDir = baseDir().getChildFile ("crashes");
    cachedCrashDir.createDirectory();

    juce::SystemStats::setApplicationCrashHandler (&crashCallback);
}

void uninstall()
{
    bool expected = true;
    if (! installed.compare_exchange_strong (expected, false)) return;
    juce::Logger::setCurrentLogger (nullptr);
    // Intentionally leak the FileLogger. juce::Logger::setCurrentLogger
    // is not thread-safe — a background thread that loaded the old
    // pointer just before the null-store could still be inside
    // writeToLog and would dereference a destroyed object after .reset().
    // Process is exiting; the OS reclaims the memory either way.
    (void) ownedLogger.release();
}

juce::File getLogDir()     { return baseDir().getChildFile ("log"); }
juce::File getCrashDir()   { return baseDir().getChildFile ("crashes"); }
juce::File getCurrentLogFile() { return cachedLogFile; }
} // namespace duskstudio::crash_handler
