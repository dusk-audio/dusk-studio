#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>
#include <string>

#ifndef DUSKSTUDIO_SOURCE_DIR
#define DUSKSTUDIO_SOURCE_DIR "."
#endif

TEST_CASE ("aux native-state rejection diagnostics identify lane and slot",
           "[session][native][diagnostics]")
{
    std::ifstream input (std::string (DUSKSTUDIO_SOURCE_DIR)
                         + "/src/engine/AudioEngine.cpp");
    REQUIRE (input.good());
    const std::string source { std::istreambuf_iterator<char> (input),
                               std::istreambuf_iterator<char>() };

    REQUIRE (source.find ("%s lane %d slot %d rejected its saved state")
             != std::string::npos);
    for (const char* format : { "CLAP", "LV2", "VST3", "AU" })
    {
        const auto call = std::string ("noteStateRejected (\"aux ") + format
                        + "\", s, blob.size(), a)";
        INFO (format);
        REQUIRE (source.find (call) != std::string::npos);
    }
}
