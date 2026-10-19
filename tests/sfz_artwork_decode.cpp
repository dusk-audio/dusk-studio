#include <catch2/catch_test_macros.hpp>

#include "ui/imgui/SfzArtwork.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

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
    for (const auto* name : { "Upright Bass", "Harp", "Cello", "Banjo", "Mandolin", "Ukulele", "Accordion", "Harmonica",
                              "Saxophone", "Clarinet", "Bassoon", "Oboe", "Trumpet", "Trombone", "Tuba", "Harpsichord",
                              "Vibraphone", "Glockenspiel", "Steelpan", "Handpan", "Koto", "Tubular Bells", "Bells" })
    {
        INFO (name);
        const auto category = duskstudio::sfz::instrumentCategory (name);
        const auto bytes = duskstudio::imgui::instrumentArtwork (category, name);
        REQUIRE_FALSE (bytes.empty());
        const auto pixels = duskstudio::imgui::decodeInstrumentArtwork (bytes);
        REQUIRE (pixels);
        CHECK (pixels->width == 512);
        CHECK (pixels->height == 320);
        CHECK (bytes != duskstudio::imgui::instrumentArtwork (category));
    }
    using C = duskstudio::sfz::InstrumentCategory;
    using duskstudio::imgui::instrumentArtworkResource;
    CHECK (std::string (instrumentArtworkResource (C::Strings, "Cello")) == "sfz_cello_png");
    CHECK (std::string (instrumentArtworkResource (C::Brass, "Trumpet")) == "sfz_trumpet_png");
    CHECK (std::string (instrumentArtworkResource (C::Mallets, "Bells")) == "sfz_handbell_png");
    CHECK (std::string (instrumentArtworkResource (C::Mallets, "Tubular Bells")) == "sfz_tubular_bells_png");
    CHECK (std::string (instrumentArtworkResource (C::Woodwinds, "Default", "Woodwinds/Clarinet.sfz")) == "sfz_clarinet_png");
    CHECK (std::string (instrumentArtworkResource (C::Synth, "Synth Bass")) == "sfz_synth_png");
    CHECK (std::string (instrumentArtworkResource (C::World, "Duduk")) == "sfz_collection_png");
    CHECK (std::string (instrumentArtworkResource (C::Strings, "Violin", "Strings/Harp ensemble/Violin.sfz")) == "sfz_strings_png");
    CHECK (std::string (instrumentArtworkResource (C::Brass, "French Horn", "Brass/Trumpet and Horn/Horn.sfz")) == "sfz_brass_png");
    CHECK (std::string (instrumentArtworkResource (C::Strings, "Winter Gift", "solo cello")) == "sfz_cello_png");
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
    {
        CHECK_FALSE (decodeInstrumentArtwork (malformedJpeg.substr (0, length)));
        CHECK_FALSE (decodeInstrumentArtwork (std::string ("\xff", 1) + std::string (malformedJpeg.substr (0, length))));
    }

    CHECK_FALSE (decodeInstrumentArtwork (std::string (malformedJpeg.substr (0, 21)) + std::string ("\xff\xd9", 2)));
    CHECK_FALSE (decodeInstrumentArtwork (std::string (malformedJpeg.substr (0, 21))
        + std::string ("\xff\xdd\x00\x02", 4) + std::string (malformedJpeg.substr (21)) + std::string ("\xff\xd9", 2)));
    const std::string embeddedEnd ("\xff\xd8\xff\xe0\x00\x04\xff\xd9", 8);
    CHECK_FALSE (decodeInstrumentArtwork (embeddedEnd + std::string (malformedJpeg.substr (2))));
    const auto artworkDirectory = std::filesystem::u8path (DUSKSTUDIO_SOURCE_DIR) / "tests" / "fixtures" / "artwork";
    for (const auto* filename : { "single-pixel.jpg", "single-pixel-progressive.jpg" })
    {
        std::ifstream jpegInput (artworkDirectory / filename, std::ios::binary);
        REQUIRE (jpegInput.good());
        const std::string jpegBytes (std::istreambuf_iterator<char> { jpegInput }, {});
        REQUIRE (decodeInstrumentArtwork (jpegBytes));
        CHECK (decodeInstrumentArtwork (std::string ("\xff", 1) + jpegBytes));
        CHECK (decodeInstrumentArtwork (jpegBytes + "trailing bytes"));
        CHECK (decodeInstrumentArtwork (embeddedEnd + jpegBytes.substr (2)));
        const auto scan = jpegBytes.find (std::string ("\xff\xda", 2));
        REQUIRE (scan != std::string::npos);
        REQUIRE (static_cast<unsigned char> (jpegBytes[scan + 4]) == 3);
        auto missingComponents = jpegBytes;
        missingComponents[scan + 3] = 8;
        missingComponents[scan + 4] = 1;
        missingComponents.erase (scan + 7, 4);
        CHECK_FALSE (decodeInstrumentArtwork (missingComponents));
        auto restartBoundary = jpegBytes;
        restartBoundary.insert (scan, std::string ("\xff\xdd\x00\x04\x00\x01", 6));
        CHECK (decodeInstrumentArtwork (restartBoundary));
        auto incompleteScan = restartBoundary;
        const auto frame = incompleteScan.find (std::string ("\xff\xc0", 2));
        const auto progressiveFrame = incompleteScan.find (std::string ("\xff\xc2", 2));
        const auto header = frame != std::string::npos ? frame : progressiveFrame;
        REQUIRE (header != std::string::npos);
        incompleteScan[header + 6] = 33;
        CHECK_FALSE (decodeInstrumentArtwork (incompleteScan));
        for (std::size_t length = 2; length < jpegBytes.size(); ++length)
            CHECK_FALSE (decodeInstrumentArtwork (std::string_view (jpegBytes).substr (0, length)));
    }

    const auto fixture = std::filesystem::u8path (DUSKSTUDIO_SOURCE_DIR)
                       / "tests" / "fixtures" / "artwork" / "overexpanded-idat.png";
    std::ifstream input (fixture, std::ios::binary);
    REQUIRE (input.good());
    const std::string overexpanded (std::istreambuf_iterator<char> { input }, {});
    REQUIRE (overexpanded.size() < 4 * 1024 * 1024);
    CHECK_FALSE (decodeInstrumentArtwork (overexpanded));
    CHECK (decodeInstrumentArtwork (valid));
}
