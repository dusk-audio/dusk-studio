#include <catch2/catch_test_macros.hpp>

#include "engine/ipc/PluginIpc.h"
#include "engine/ipc/RemotePluginConnection.h"

#include <string>

// A Release leaves the child alive with no instance. Answering the next
// PrepareToPlay with success let the parent republish the connection and route
// audio into a child with nothing to process, which reads as a working slot
// that is simply silent.
TEST_CASE ("PrepareToPlay is refused when the child holds no plug-in")
{
    duskstudio::ipc::RemotePluginConnection connection;

    std::string error;
    REQUIRE (connection.connect (DUSKSTUDIO_PLUGIN_HOST_PATH, "--ipc-host", error));

    CHECK_FALSE (connection.prepareToPlay (48000.0, 64, error));
    CHECK_FALSE (error.empty());
}
