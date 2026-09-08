#include <catch2/catch_test_macros.hpp>

#include "TestTempDirectory.h"
#include "foundation/Fs.h"
#include "util/LogFile.h"

#include <juce_core/juce_core.h>

#include <filesystem>
#include <string>

namespace
{
juce::File asJuceFile (const std::filesystem::path& path)
{
    return juce::File (juce::String::fromUTF8 (path.u8string().c_str()));
}
} // namespace

TEST_CASE ("application logs append UTF-8 to existing Unicode paths", "[diagnostics][issue-313]")
{
    duskstudio::test::TempDirectory directory { "dusk-log-test-" };
    const auto path = directory.path() / std::filesystem::u8path (u8"Gr\u00fc\u00dfe-\u97f3\u58f0") / "daily.log";
    duskstudio::diagnostics::LogFile log (path);
    REQUIRE (log.prepare());
    REQUIRE (log.append (u8"session: caf\u00e9 \U0001f3b9"));
    REQUIRE (dusk::fs::loadFileAsString (path) == u8"session: caf\u00e9 \U0001f3b9\r\n");

    duskstudio::diagnostics::LogFile reopened (path);
    REQUIRE (reopened.prepare());
    REQUIRE (reopened.append ("second startup\nmore detail"));
    REQUIRE (reopened.append ({}));
    REQUIRE (dusk::fs::loadFileAsString (path)
             == u8"session: caf\u00e9 \U0001f3b9\r\nsecond startup\nmore detail\r\n\r\n");
}

TEST_CASE ("application log startup trimming matches the existing file logger", "[diagnostics][issue-313]")
{
    using duskstudio::diagnostics::LogFile;
    duskstudio::test::TempDirectory directory { "dusk-log-test-" };
    const auto path = directory.path() / "native.log";
    const auto reference = directory.path() / "reference.log";
    std::string original;

    SECTION ("below the limit") { original = "existing\r\nrecords\r\n"; }
    SECTION ("exactly at the limit") { original = std::string (LogFile::kMaxInitialBytes - 1, 'x') + '\n'; }
    SECTION ("LF boundary") { original = std::string (LogFile::kMaxInitialBytes, 'x') + "\nrecent\n"; }
    SECTION ("CRLF boundary") { original = std::string (LogFile::kMaxInitialBytes, 'x') + "\r\nrecent\r\n"; }
    SECTION ("CR boundary") { original = std::string (LogFile::kMaxInitialBytes, 'x') + "\rrecent\r"; }
    SECTION ("no complete line") { original = std::string (LogFile::kMaxInitialBytes + 10, 'x'); }
    SECTION ("embedded zero before the boundary")
    {
        original = std::string (LogFile::kMaxInitialBytes, 'x') + std::string ("\0recent\n", 8);
    }
    REQUIRE (dusk::fs::writeStringToFile (path, original));
    REQUIRE (dusk::fs::writeStringToFile (reference, original));
    juce::FileLogger::trimFileSize (asJuceFile (reference), static_cast<juce::int64> (LogFile::kMaxInitialBytes));

    LogFile log (path);
    REQUIRE (log.prepare());
    REQUIRE (dusk::fs::loadFileAsString (path) == dusk::fs::loadFileAsString (reference));
    REQUIRE (log.append ("new startup"));
    REQUIRE (dusk::fs::loadFileAsString (path) == dusk::fs::loadFileAsString (reference) + "new startup\r\n");
    for (const auto& entry : std::filesystem::directory_iterator (directory.path()))
        REQUIRE (entry.is_regular_file());
}

TEST_CASE ("application logs retain whole recent records during startup trimming", "[diagnostics][issue-313]")
{
    using duskstudio::diagnostics::LogFile;
    duskstudio::test::TempDirectory directory { "dusk-log-test-" };
    const auto path = directory.path() / "daily.log";
    const std::string recent = "\nfirst recent record\nsecond recent record\n";
    REQUIRE (dusk::fs::writeStringToFile (path, std::string (LogFile::kMaxInitialBytes * 2, 'x') + recent));
    LogFile log (path);
    REQUIRE (log.prepare());
    REQUIRE (dusk::fs::loadFileAsString (path) == recent);
    REQUIRE (std::filesystem::file_size (path) <= LogFile::kMaxInitialBytes);

    REQUIRE (log.append (std::string (LogFile::kMaxInitialBytes, 'y')));
    REQUIRE (std::filesystem::file_size (path) > LogFile::kMaxInitialBytes);
}

TEST_CASE ("application log failures preserve existing paths and report failure", "[diagnostics][issue-313]")
{
    using duskstudio::diagnostics::LogFile;
    duskstudio::test::TempDirectory directory { "dusk-log-test-" };
    const auto blocker = directory.path() / "file";
    REQUIRE (dusk::fs::writeStringToFile (blocker, "keep this file"));
    LogFile blocked (blocker / "nested.log");
    REQUIRE_FALSE (blocked.prepare());
    REQUIRE_FALSE (blocked.append ("unwritten"));
    REQUIRE (dusk::fs::loadFileAsString (blocker) == "keep this file");

    LogFile directoryTarget (directory.path());
    REQUIRE_FALSE (directoryTarget.prepare());
    REQUIRE_FALSE (directoryTarget.append ("unwritten"));
    REQUIRE (std::filesystem::is_directory (directory.path()));

    LogFile emptyPath (std::filesystem::path {});
    REQUIRE_FALSE (emptyPath.prepare());
    REQUIRE_FALSE (emptyPath.append ("unwritten"));
}
