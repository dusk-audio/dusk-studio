// The Windows half of the single-instance gate compiles only on Windows, so
// these read its source. Each invariant guards a launch outcome that cannot be
// reproduced from a Linux or macOS run: a session path handed to whoever
// squatted a machine-global pipe name, and a second desktop session that
// reports itself primary while its listener never started.

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>
#include <string>

#ifndef DUSKSTUDIO_SOURCE_DIR
#define DUSKSTUDIO_SOURCE_DIR "."
#endif

namespace
{
std::string readSource (const char* relativePath)
{
    std::ifstream input (std::string (DUSKSTUDIO_SOURCE_DIR) + "/" + relativePath);
    REQUIRE (input.good());
    return { std::istreambuf_iterator<char> (input),
             std::istreambuf_iterator<char>() };
}

std::size_t occurrences (const std::string& text, const std::string& needle)
{
    std::size_t found = 0;
    for (std::size_t at = text.find (needle); at != std::string::npos;
         at = text.find (needle, at + needle.size()))
        ++found;
    return found;
}
} // namespace

TEST_CASE ("Windows single-instance client verifies the process it hands the session to",
           "[single-instance][windows][issue-451]")
{
    const auto source = readSource ("src/util/SingleInstance.cpp");

    // \\.\pipe\ is machine-global, so any local account can create the name
    // first and collect the absolute session path of every launch this user
    // makes. The server already checks the client SID; the client has to check
    // the server's the same way before a byte goes out.
    REQUIRE (source.find ("processIsThisUser") != std::string::npos);
    REQUIRE (source.find ("serverIsThisUser") != std::string::npos);
    REQUIRE (source.find ("GetNamedPipeServerProcessId") != std::string::npos);
    REQUIRE (source.find ("PROCESS_QUERY_LIMITED_INFORMATION") != std::string::npos);
    REQUIRE (source.find ("EqualSid") != std::string::npos);
    REQUIRE (source.find ("handOver (candidate->pipeName, candidate->security.userSid")
             != std::string::npos);

    // A stranger on the name is not a primary: the launch reports it and runs
    // unattached rather than quitting on a reply it cannot trust.
    REQUIRE (source.find ("Handoff::Foreign") != std::string::npos);
}

TEST_CASE ("Windows single-instance mutex and pipe share one namespace and a failure sticks",
           "[single-instance][windows][issue-451]")
{
    const auto source = readSource ("src/util/SingleInstance.cpp");

    // A session-scoped mutex against a machine-global pipe lets a second RDP or
    // fast-user-switch session pass the mutex, fail CreateNamedPipe on the
    // first session's name, and start anyway. Both names carry the Terminal
    // Services session, so the slot is one per user per desktop session.
    REQUIRE (source.find ("kSlotNamespace") != std::string::npos);
    REQUIRE (source.find ("ProcessIdToSessionId") != std::string::npos);
    REQUIRE (source.find ("hashBytes (&sessionId, sizeof sessionId, hash)") != std::string::npos);
    REQUIRE (source.find ("mutexName = L\"Local") == std::string::npos);

    // A listener that never started must keep the mutex, or every later launch
    // in the session repeats the failure and calls itself primary as well.
    REQUIRE (occurrences (source, "listener = candidate.release();") == 2);
    REQUIRE (source.find ("the single-instance listener could ") != std::string::npos);
}
