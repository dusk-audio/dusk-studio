#include <catch2/catch_test_macros.hpp>

#include "ui/imgui/SfzArtwork.h"

#include <algorithm>

TEST_CASE ("SFZ category photographs decode to bounded opaque thumbnails", "[sfz][artwork]")
{
    for (std::size_t i = 0; i < duskstudio::sfz::kInstrumentCategories.size(); ++i)
    {
        const auto category = static_cast<duskstudio::sfz::InstrumentCategory> (i);
        INFO (duskstudio::sfz::kInstrumentCategories[i].name);
        const auto bytes = duskstudio::imgui::instrumentArtwork (category);
        REQUIRE_FALSE (bytes.empty());
        const auto pixels = duskstudio::imgui::decodeInstrumentArtwork (bytes);
        REQUIRE (pixels);
        REQUIRE (pixels->width == 512);
        REQUIRE (pixels->height == 320);
        REQUIRE (pixels->rgba.size() == 512 * 320 * 4);
        for (std::size_t alpha = 3; alpha < pixels->rgba.size(); alpha += 4)
            if (pixels->rgba[alpha] != 255) FAIL ("thumbnail lost opacity");
        REQUIRE (std::any_of (pixels->rgba.begin(), pixels->rgba.end(), [] (unsigned char value) { return value > 32 && value < 255; }));
    }
}

TEST_CASE ("SFZ artwork decoder rejects malformed and excessive images", "[sfz][artwork]")
{
    using duskstudio::imgui::decodeInstrumentArtwork;
    CHECK_FALSE (decodeInstrumentArtwork ({}));
    CHECK_FALSE (decodeInstrumentArtwork ("not an image"));
    CHECK_FALSE (decodeInstrumentArtwork (std::string (4 * 1024 * 1024 + 1, 'x')));
    const auto valid = duskstudio::imgui::instrumentArtwork (duskstudio::sfz::InstrumentCategory::Piano);
    CHECK_FALSE (decodeInstrumentArtwork (valid.substr (0, 12)));
}
