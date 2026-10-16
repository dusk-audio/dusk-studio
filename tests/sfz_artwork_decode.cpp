#include <catch2/catch_test_macros.hpp>

#include "ui/imgui/SfzArtwork.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

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
    auto truncatedPng = std::string (valid.substr (0, 33));
    CHECK_FALSE (decodeInstrumentArtwork (truncatedPng));
    const unsigned char jpeg[] {
        0xff, 0xd8, 0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x01, 0x00, 0x01, 0x03,
        0x01, 0x11, 0x00, 0x02, 0x11, 0x00, 0x03, 0x11, 0x00,
        0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00
    };
    const std::string_view malformedJpeg (reinterpret_cast<const char*> (jpeg), sizeof (jpeg));
    for (std::size_t length = 2; length <= malformedJpeg.size(); ++length)
        CHECK_FALSE (decodeInstrumentArtwork (malformedJpeg.substr (0, length)));

    const auto fixture = std::filesystem::u8path (DUSKSTUDIO_SOURCE_DIR)
                       / "tests" / "fixtures" / "artwork" / "overexpanded-idat.png";
    std::ifstream input (fixture, std::ios::binary);
    REQUIRE (input.good());
    const std::string overexpanded (std::istreambuf_iterator<char> { input }, {});
    REQUIRE (overexpanded.size() < 4 * 1024 * 1024);
    CHECK_FALSE (decodeInstrumentArtwork (overexpanded));
    CHECK (decodeInstrumentArtwork (valid));
}
