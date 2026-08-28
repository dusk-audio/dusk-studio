#include <catch2/catch_test_macros.hpp>

#include "engine/PluginStateDiagnostics.h"

TEST_CASE ("aux native-state rejection diagnostics identify lane and slot",
           "[session][native][diagnostics]")
{
    for (const char* format : { "CLAP", "LV2", "VST3", "AU" })
    {
        INFO (format);
        REQUIRE (duskstudio::pluginstate::stateRejectedMessage (
                     std::string ("aux ") + format, 1, 4096, 2)
                 == std::string ("[Dusk Studio/session] aux ") + format
                    + " lane 3 slot 2 rejected its saved state (4096 bytes); "
                      "running at defaults");
    }

    REQUIRE (duskstudio::pluginstate::stateRejectedMessage (
                 "track CLAP", 0, 128)
             == "[Dusk Studio/session] track CLAP 1 rejected its saved state "
                "(128 bytes); running at defaults");
}
