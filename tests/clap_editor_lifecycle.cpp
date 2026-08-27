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

TEST_CASE ("CLAP editors size the GUI before parenting it",
           "[clap][editor][regression][issue-361]")
{
    for (const char* path : { "src/engine/clap/ClapEditor.cpp",
                              "src/engine/clap/ClapEditor_Win.cpp",
                              "src/engine/clap/ClapEditor_Mac.mm" })
    {
        const auto source = readSource (path);
        const auto embed = source.find ("bool ClapEditor::embed");
        const auto setSize = source.find ("gui->set_size", embed);
        const auto setParent = source.find ("gui->set_parent", embed);
        const auto show = source.find ("gui->show", embed);

        INFO (path);
        REQUIRE (embed != std::string::npos);
        REQUIRE (setSize != std::string::npos);
        REQUIRE (setParent != std::string::npos);
        REQUIRE (show != std::string::npos);
        REQUIRE (setSize < setParent);
        REQUIRE (setParent < show);
    }
}
