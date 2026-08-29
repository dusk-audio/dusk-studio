#include <catch2/catch_test_macros.hpp>

#include "engine/ipc/RemotePluginConnection.h"

#include <string>

TEST_CASE ("OOP control replies correlate requests and validate their shape",
           "[ipc][issue-370]")
{
    duskstudio::ipc::RemotePluginConnection connection;
    std::string error;
    REQUIRE (connection.connect (DUSKSTUDIO_PLUGIN_HOST_PATH,
                                 "--ipc-control-reply-stub", error));

    // The child holds A until it receives B, so this timeout establishes the
    // ordering without sleeps or scheduler polling.
    REQUIRE_FALSE (connection.ping (20, error));
    REQUIRE (error == "reply timeout");

    // The child now sends A's failing reply first, followed by B's success.
    // B must ignore A by request ID and consume only its own frame.
    error.clear();
    REQUIRE (connection.ping (500, error));
    REQUIRE (error.empty());

    int numInputs = 0;
    int numOutputs = 0;
    int latency = 0;
    bool isInstrument = false;
    REQUIRE_FALSE (connection.loadPlugin ("", 48000.0, 256,
                                          numInputs, numOutputs, latency,
                                          isInstrument, error));
    REQUIRE (error == "LoadPlugin reply size mismatch");

    error.clear();
    REQUIRE_FALSE (connection.ping (500, error));
    REQUIRE (error == "reply opcode mismatch");

    error.clear();
    REQUIRE_FALSE (connection.ping (500, error));
    REQUIRE (error == "Ping reply payload size mismatch");
}
