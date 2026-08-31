// Cross-platform IPC round-trip regression test. Verifies that:
//   • RemotePluginConnection can spawn the dusk-studio-plugin-host child in
//     --ipc-stub mode (the dependency-light Phase 1 echo loop).
//   • processBlockSync round-trips audio buffers through the platform's
//     shared-memory + cross-process signal pair without timing out.
//   • The stub's echo is byte-exact (output equals input) — catches any
//     SHM offset / channel-stride drift between parent and child.
//
// Platforms: Linux (memfd_create + futex), Windows (CreateFileMapping +
// auto-reset events), macOS 14.4+ (shm_open + os_sync_wait_on_address). The
// transport differences are hidden behind RemotePluginConnection, so this
// file uses only platform-agnostic APIs. tests/CMakeLists.txt gates the
// translation unit on _duskstudio_ipc_test_enabled, which mirrors the
// three platform gates in the top-level CMakeLists' OOP block.

#include <catch2/catch_test_macros.hpp>
#include "engine/ipc/RemotePluginConnection.h"
#include "engine/ipc/platform/IpcProcess.h"
#include "TestTempDirectory.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#if defined (_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace
{
constexpr int  kBlockSize  = 256;
constexpr int  kNumChans   = 2;
constexpr int  kIterations = 32;
constexpr long long kTimeoutNs = 100'000'000LL;  // 100 ms

struct ScopedChannelPair
{
    ~ScopedChannelPair()
    {
        duskstudio::ipc::platform::closeHandle (value.parentEnd);
        duskstudio::ipc::platform::closeHandle (value.childEnd);
    }

    duskstudio::ipc::platform::ChannelPair value;
};
} // namespace

#if defined (_WIN32)
TEST_CASE ("Windows IPC children inherit only their own shared-memory handles",
           "[ipc][windows][issue-365]")
{
    namespace ipcp = duskstudio::ipc::platform;

    static std::atomic<std::uint64_t> mappingSequence { 0 };
    const auto makeMappingName = [&]
    {
        return std::string ("Local\\dusk-studio-ipc-inherit-test-")
            + std::to_string ((unsigned long) ::GetCurrentProcessId()) + "-"
            + std::to_string (mappingSequence.fetch_add (1, std::memory_order_relaxed));
    };
    const auto mappingAName = makeMappingName();
    const auto mappingBName = makeMappingName();

    SECURITY_ATTRIBUTES attributes {};
    attributes.nLength = sizeof (attributes);
    attributes.bInheritHandle = TRUE;

    ipcp::NativeHandle mappingA;
    ipcp::NativeHandle mappingB;
    mappingA.h = ::CreateFileMappingA (INVALID_HANDLE_VALUE, &attributes,
                                       PAGE_READWRITE, 0, 4096, mappingAName.c_str());
    mappingB.h = ::CreateFileMappingA (INVALID_HANDLE_VALUE, &attributes,
                                       PAGE_READWRITE, 0, 4096, mappingBName.c_str());
    REQUIRE (ipcp::isValid (mappingA));
    REQUIRE (ipcp::isValid (mappingB));

    const auto writeMarker = [] (const ipcp::NativeHandle& mapping,
                                 std::uint8_t marker)
    {
        void* const view = ::MapViewOfFile (reinterpret_cast<HANDLE> (mapping.h),
                                            FILE_MAP_WRITE, 0, 0, 1);
        REQUIRE (view != nullptr);
        *static_cast<std::uint8_t*> (view) = marker;
        REQUIRE (::UnmapViewOfFile (view));
    };
    constexpr std::uint8_t markerA = 0xa5;
    constexpr std::uint8_t markerB = 0x5a;
    writeMarker (mappingA, markerA);
    writeMarker (mappingB, markerB);

    const auto handleArgument = [] (const char* prefix,
                                    const ipcp::NativeHandle& mapping)
    {
        char buffer[96];
        std::snprintf (buffer, sizeof (buffer), "%s0x%llx", prefix,
                       (unsigned long long) (std::uintptr_t) mapping.h);
        return std::string (buffer);
    };
    const std::vector<std::string> probeArguments {
        "--ipc-handle-probe-stub",
        handleArgument ("--ipc-probe-a=", mappingA),
        handleArgument ("--ipc-probe-b=", mappingB)
    };

    ScopedChannelPair channelsA;
    ScopedChannelPair channelsB;
    std::string error;
    REQUIRE (ipcp::createChannelPair (channelsA.value, error));
    REQUIRE (ipcp::createChannelPair (channelsB.value, error));

    ipcp::ChildProcess childA;
    ipcp::ChildProcess childB;
    REQUIRE (childA.spawn (DUSKSTUDIO_PLUGIN_HOST_PATH, probeArguments,
                           channelsA.value.childEnd, { mappingA }, error));
    REQUIRE (childB.spawn (DUSKSTUDIO_PLUGIN_HOST_PATH, probeArguments,
                           channelsB.value.childEnd, { mappingB }, error));

    std::uint8_t childAMarkers[2] {};
    std::uint8_t childBMarkers[2] {};
    REQUIRE (ipcp::readExact (channelsA.value.parentEnd,
                              childAMarkers, sizeof (childAMarkers)));
    REQUIRE (ipcp::readExact (channelsB.value.parentEnd,
                              childBMarkers, sizeof (childBMarkers)));
    REQUIRE (childAMarkers[0] == markerA);
    REQUIRE (childAMarkers[1] == 0);
    REQUIRE (childBMarkers[0] == 0);
    REQUIRE (childBMarkers[1] == markerB);

    DWORD mappingAFlags = 0;
    DWORD mappingBFlags = 0;
    REQUIRE (::GetHandleInformation (reinterpret_cast<HANDLE> (mappingA.h),
                                     &mappingAFlags));
    REQUIRE (::GetHandleInformation (reinterpret_cast<HANDLE> (mappingB.h),
                                     &mappingBFlags));
    REQUIRE ((mappingAFlags & HANDLE_FLAG_INHERIT) == 0);
    REQUIRE ((mappingBFlags & HANDLE_FLAG_INHERIT) == 0);

    ipcp::closeHandle (mappingA);
    ipcp::closeHandle (mappingB);
    const auto mappingExists = [] (const std::string& name)
    {
        HANDLE mapping = ::OpenFileMappingA (FILE_MAP_READ, FALSE, name.c_str());
        if (mapping == nullptr) return false;
        ::CloseHandle (mapping);
        return true;
    };
    REQUIRE (mappingExists (mappingAName));
    REQUIRE (mappingExists (mappingBName));

    ipcp::closeHandle (channelsA.value.parentEnd);
    childA.terminate (1000);
    REQUIRE_FALSE (mappingExists (mappingAName));
    REQUIRE (mappingExists (mappingBName));

    ipcp::closeHandle (channelsB.value.parentEnd);
    childB.terminate (1000);
    REQUIRE_FALSE (mappingExists (mappingBName));
}

TEST_CASE ("Windows IPC launcher preserves Unicode paths and arguments",
           "[ipc][windows][issue-373]")
{
    namespace ipcp = duskstudio::ipc::platform;

    duskstudio::test::TempDirectory temp ("dusk-ipc-unicode-launch-");
    const auto unicodeDirectory = temp.path()
        / std::filesystem::u8path (u8"\u5B89\u88C5 \u8DEF\u5F84");
    std::error_code filesystemError;
    REQUIRE (std::filesystem::create_directory (unicodeDirectory, filesystemError));
    REQUIRE_FALSE (filesystemError);

    const auto sourceHost = std::filesystem::u8path (DUSKSTUDIO_PLUGIN_HOST_PATH);
    const auto copiedHost = unicodeDirectory / sourceHost.filename();
    REQUIRE (std::filesystem::copy_file (
        sourceHost, copiedHost, std::filesystem::copy_options::overwrite_existing,
        filesystemError));
    REQUIRE_FALSE (filesystemError);

    const std::vector<std::string> suppliedArguments {
        "--ipc-argv-stub",
        u8"\u63D2\u4EF6 \u0430\u0440\u0433\u0443\u043C\u0435\u043D\u0442",
        R"(C:\folder with spaces\)",
        R"(say \\"hello" and C:\tail\\)"
    };

    ScopedChannelPair channels;
    std::string error;
    REQUIRE (ipcp::createChannelPair (channels.value, error));

    ipcp::ChildProcess child;
    const bool spawned = child.spawn (copiedHost.u8string(), suppliedArguments,
                                      channels.value.childEnd, {}, error);
    INFO ("spawn error: " << error);
    REQUIRE (spawned);

    std::uint32_t argumentCount = 0;
    REQUIRE (ipcp::readExact (channels.value.parentEnd,
                              &argumentCount, sizeof (argumentCount)));
    REQUIRE (argumentCount == suppliedArguments.size() + 2);

    std::vector<std::string> receivedArguments;
    receivedArguments.reserve (argumentCount);
    for (std::uint32_t i = 0; i < argumentCount; ++i)
    {
        std::uint32_t length = 0;
        REQUIRE (ipcp::readExact (channels.value.parentEnd, &length, sizeof (length)));
        REQUIRE (length < 4096);
        std::string argument (length, '\0');
        if (length > 0)
            REQUIRE (ipcp::readExact (channels.value.parentEnd, argument.data(), length));
        receivedArguments.push_back (std::move (argument));
    }

    REQUIRE (receivedArguments.front() == copiedHost.u8string());
    for (std::size_t i = 0; i < suppliedArguments.size(); ++i)
        REQUIRE (receivedArguments[i + 1] == suppliedArguments[i]);
    REQUIRE (receivedArguments.back().rfind ("--ipc-channel=0x", 0) == 0);
}
#endif

TEST_CASE ("Plugin-host handshake read timeout bounds a silent live child",
           "[ipc][issue-364]")
{
    namespace ipcp = duskstudio::ipc::platform;
    using namespace std::chrono_literals;

    ipcp::NativeHandle invalid;
    REQUIRE_FALSE (ipcp::setReadTimeout (invalid, 75));

    ScopedChannelPair channels;
    std::string error;
    REQUIRE (ipcp::createChannelPair (channels.value, error));

    ipcp::ChildProcess child;
    REQUIRE (child.spawn (DUSKSTUDIO_PLUGIN_HOST_PATH,
                          { "--ipc-silent-handshake-stub" },
                          channels.value.childEnd, {}, error));
    REQUIRE (child.isAlive());
    REQUIRE (ipcp::setReadTimeout (channels.value.parentEnd, 75));

    char ready = 0;
    const auto readStarted = std::chrono::steady_clock::now();
    REQUIRE_FALSE (ipcp::readExact (channels.value.parentEnd, &ready, sizeof (ready)));
    const auto readElapsed = std::chrono::steady_clock::now() - readStarted;

    // A premature EOF would make this test pass instantly, so require evidence
    // that the live child held the pipe open until the configured deadline.
    REQUIRE (readElapsed >= 25ms);
    REQUIRE (readElapsed < 2s);
    REQUIRE (child.isAlive());

    // Releasing the parent end wakes the silent child's blocking read. The
    // normal process teardown then reaps it without leaving a wedged child.
    ipcp::closeHandle (channels.value.parentEnd);
    const auto teardownStarted = std::chrono::steady_clock::now();
    child.terminate (1000);
    REQUIRE_FALSE (child.isAlive());
    REQUIRE (std::chrono::steady_clock::now() - teardownStarted < 2s);
}

TEST_CASE ("ipc-stub: connect, round-trip 32 blocks, byte-exact echo",
            "[ipc]")
{
    duskstudio::ipc::RemotePluginConnection conn;

    std::string err;
    REQUIRE (conn.connect (DUSKSTUDIO_PLUGIN_HOST_PATH, "--ipc-stub", err));
    REQUIRE (err.empty());

    std::vector<float> bufL ((std::size_t) kBlockSize);
    std::vector<float> bufR ((std::size_t) kBlockSize);
    const float* in[kNumChans] { bufL.data(), bufR.data() };
    dusk::MidiBuffer midi;

    for (int it = 0; it < kIterations; ++it)
    {
        // Vary content per iteration so a buggy stub returning a stale
        // SHM region would fail the byte-exact check below on iter 1+.
        for (int i = 0; i < kBlockSize; ++i)
        {
            bufL[(std::size_t) i] = 0.5f * std::sin (((float) (i + it)) * 0.1f);
            bufR[(std::size_t) i] = 0.5f * std::cos (((float) (i + it)) * 0.1f);
        }

        // The stub echoes midiIn -> midiOut, so a fed note-on must round-trip
        // back byte-exact through the dusk serialise (in) / deserialise (out).
        const std::uint8_t noteOn[3] { 0x90, (std::uint8_t) (0x40 + it % 8), 0x7F };
        const int notePos = it % kBlockSize;
        midi.clear();
        midi.addEvent (noteOn, 3, notePos);

        REQUIRE (conn.processBlockSync (in, kNumChans, kNumChans, kBlockSize, midi, kTimeoutNs));

        int midiEvents = 0;
        for (const auto meta : midi)
        {
            REQUIRE (meta.numBytes       == 3);
            REQUIRE (meta.samplePosition == notePos);
            REQUIRE (meta.data[0]        == noteOn[0]);
            REQUIRE (meta.data[1]        == noteOn[1]);
            REQUIRE (meta.data[2]        == noteOn[2]);
            ++midiEvents;
        }
        REQUIRE (midiEvents == 1);

        for (int c = 0; c < kNumChans; ++c)
        {
            const float* out = conn.readOutChannel (c);
            const float* expected = (c == 0) ? bufL.data() : bufR.data();
            for (int i = 0; i < kBlockSize; ++i)
            {
                // Stub mode is a memcpy echo — bit-exact equality is the
                // right check. Floating-point tolerance is wrong here:
                // any drift would mask a real SHM corruption bug.
                REQUIRE (out[i] == expected[i]);
            }
        }
    }

    REQUIRE (conn.getRoundTripCount() == (std::uint64_t) kIterations);
    REQUIRE_FALSE (conn.isCrashed());
}

TEST_CASE ("ipc-stub: rejects oversize block", "[ipc]")
{
    duskstudio::ipc::RemotePluginConnection conn;

    std::string err;
    REQUIRE (conn.connect (DUSKSTUDIO_PLUGIN_HOST_PATH, "--ipc-stub", err));

    // PluginIpc.h hardcodes kMaxBlock = 1024. processBlockSync must
    // return false rather than overrun the SHM audio region.
    std::vector<float> oversize (4096, 0.0f);
    const float* in[1] { oversize.data() };
    dusk::MidiBuffer midi;
    REQUIRE_FALSE (conn.processBlockSync (in, 1, 1, 4096, midi, 1'000'000LL));
    REQUIRE_FALSE (conn.isCrashed());  // bad-input rejection isn't a crash
}

TEST_CASE ("ipc-stub: timeout is bounded and marks the connection crashed", "[ipc]")
{
    duskstudio::ipc::RemotePluginConnection conn;

    std::string err;
    REQUIRE (conn.connect (DUSKSTUDIO_PLUGIN_HOST_PATH, "--ipc-stub-timeout", err));

    std::vector<float> input ((std::size_t) kBlockSize, 0.25f);
    const float* in[1] { input.data() };
    dusk::MidiBuffer midi;

    const auto started = std::chrono::steady_clock::now();
    REQUIRE_FALSE (conn.processBlockSync (in, 1, 1, kBlockSize, midi, 20'000'000LL));
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE (conn.isCrashed());
    // Wide enough for scheduling slack over the 20 ms deadline, tight enough
    // that a build ignoring the deadline still fails.
    REQUIRE (elapsed < std::chrono::milliseconds (250));

    // Explicit repeated teardown must wake the stalled child and remain
    // idempotent; the destructor calls it once more after this scope.
    conn.disconnect();
    conn.disconnect();
}

TEST_CASE ("ipc-stub: repeated connect, block, and teardown", "[ipc]")
{
    for (int run = 0; run < 4; ++run)
    {
        duskstudio::ipc::RemotePluginConnection conn;
        std::string err;
        REQUIRE (conn.connect (DUSKSTUDIO_PLUGIN_HOST_PATH, "--ipc-stub", err));

        float sample = (float) run + 0.125f;
        const float* in[1] { &sample };
        dusk::MidiBuffer midi;
        REQUIRE (conn.processBlockSync (in, 1, 1, 1, midi, kTimeoutNs));
        REQUIRE (conn.readOutChannel (0)[0] == sample);
        REQUIRE (conn.getRoundTripCount() == 1);

        conn.disconnect();
        conn.disconnect();
    }
}
