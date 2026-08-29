#include <catch2/catch_test_macros.hpp>

#include "engine/ipc/RemotePluginConnection.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace
{
enum class MutationRpc
{
    Prepare,
    Release,
    GetState,
    SetState
};

struct ParkTimeoutResult
{
    bool observedProcessBlock{ false };
    bool mutationSucceeded{ true };
    bool audioSucceeded{ true };
    bool childExited{ false };
    bool connectionCrashed{ false };
    std::string error;
};

ParkTimeoutResult runParkTimeoutCase (MutationRpc rpc)
{
    using namespace std::chrono_literals;

    ParkTimeoutResult result;
    duskstudio::ipc::RemotePluginConnection connection;
    std::string connectError;
    if (!connection.connect (DUSKSTUDIO_PLUGIN_HOST_PATH,
                             "--ipc-park-timeout-stub", connectError))
    {
        result.error = connectError;
        return result;
    }

    float inputSample = 0.25f;
    const float* input[1]{ &inputSample };
    dusk::MidiBuffer midi;
    std::thread audioThread ([&]
                             { result.audioSucceeded = connection.processBlockSync (
                                   input, 1, 1, 1, midi, 1'000'000'000LL); });

    for (int attempt = 0; attempt < 100; ++attempt)
    {
        std::string pingError;
        if (connection.ping (100, pingError))
        {
            result.observedProcessBlock = true;
            break;
        }
        std::this_thread::sleep_for (1ms);
    }

    if (result.observedProcessBlock)
    {
        switch (rpc)
        {
        case MutationRpc::Prepare:
            result.mutationSucceeded = connection.prepareToPlay (48000.0, 256,
                                                                 result.error);
            break;
        case MutationRpc::Release:
            result.mutationSucceeded = connection.release (result.error);
            break;
        case MutationRpc::GetState:
        {
            std::vector<std::uint8_t> state;
            result.mutationSucceeded = connection.getState (state, result.error);
            break;
        }
        case MutationRpc::SetState:
        {
            const std::uint8_t state = 0x42;
            result.mutationSucceeded = connection.setState (&state, 1, result.error);
            break;
        }
        }
    }
    else
    {
        result.error = "test processor never entered processBlock";
        connection.disconnect ();
    }

    audioThread.join ();
    result.connectionCrashed = connection.isCrashed ();

    for (int attempt = 0; attempt < 100; ++attempt)
    {
        if (connection.pollReaper ())
        {
            result.childExited = true;
            break;
        }
        std::this_thread::sleep_for (1ms);
    }
    connection.disconnect ();
    return result;
}

void requireSafeTimeout (MutationRpc rpc, const char* expectedError)
{
    const auto result = runParkTimeoutCase (rpc);
    INFO (result.error);
    REQUIRE (result.observedProcessBlock);
    REQUIRE_FALSE (result.mutationSucceeded);
    REQUIRE_FALSE (result.audioSucceeded);
    REQUIRE (result.error == expectedError);
    REQUIRE (result.connectionCrashed);
    REQUIRE (result.childExited);
}
} // namespace

TEST_CASE ("OOP worker park timeout prevents prepare mutation", "[ipc][issue-371]")
{
    requireSafeTimeout (MutationRpc::Prepare, "PrepareToPlay status != 0");
}

TEST_CASE ("OOP worker park timeout prevents release mutation", "[ipc][issue-371]")
{
    requireSafeTimeout (MutationRpc::Release, "Release status != 0");
}

TEST_CASE ("OOP worker park timeout prevents state read", "[ipc][issue-371]")
{
    requireSafeTimeout (MutationRpc::GetState, "GetState status != 0");
}

TEST_CASE ("OOP worker park timeout prevents state write", "[ipc][issue-371]")
{
    requireSafeTimeout (MutationRpc::SetState, "SetState status != 0");
}
