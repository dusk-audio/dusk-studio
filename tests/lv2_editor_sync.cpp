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
}

TEST_CASE ("LV2 editors seed current values and forward later changes",
           "[lv2][editor][regression][issue-355]")
{
    for (const char* path : { "src/engine/lv2/Lv2Editor.cpp",
                              "src/engine/lv2/Lv2Editor_Mac.mm" })
    {
        const auto source = readSource (path);
        const auto syncMethod = source.find ("void syncParameterValues");
        const auto portEvent = source.find ("suil_instance_port_event", syncMethod);
        const auto instantiate = source.find ("suil_instance_new");
        const auto initialSync = source.find ("syncParameterValues (true)", instantiate);
        const auto pump = source.find ("void Lv2Editor::pump");
        const auto ongoingSync = source.find ("syncParameterValues (false)", pump);

        INFO (path);
        REQUIRE (syncMethod != std::string::npos);
        REQUIRE (portEvent != std::string::npos);
        REQUIRE (initialSync != std::string::npos);
        REQUIRE (pump != std::string::npos);
        REQUIRE (ongoingSync != std::string::npos);
    }
}
