#include <catch2/catch_test_macros.hpp>

#include "session/Session.h"

#include <filesystem>
#include <string>

using namespace duskstudio;

namespace
{
const std::string kNonAscii = "Gesang \xc3\xa4\xc3\xb6\xc3\xbc\xc3\x9f \xe5\xa3\xb0 \xf0\x9f\x8e\xb8";
} // namespace

TEST_CASE ("AudioRegion label round-trips UTF-8 through its accessors", "[session][accessors]")
{
    AudioRegion region;
    REQUIRE (region.labelUtf8().empty());

    region.setLabelUtf8 (kNonAscii);
    REQUIRE (region.labelUtf8() == kNonAscii);
    REQUIRE (region.label == juce::String::fromUTF8 (kNonAscii.c_str()));

    region.label = juce::String::fromUTF8 ("Take 3 \xe2\x80\x94 Chorus");
    REQUIRE (region.labelUtf8() == "Take 3 \xe2\x80\x94 Chorus");

    region.setLabelUtf8 ({});
    REQUIRE (region.label.isEmpty());
}

TEST_CASE ("AudioRegion colour reads and writes as ARGB, transparent included", "[session][accessors]")
{
    AudioRegion region;
    REQUIRE (region.customArgb() == 0u);
    REQUIRE (argbIsTransparent (region.customArgb()));

    region.setCustomArgb (0xffd05f5fu);
    REQUIRE (region.customColour == juce::Colour (0xffd05f5fu));
    REQUIRE (region.customArgb() == 0xffd05f5fu);
    REQUIRE_FALSE (argbIsTransparent (region.customArgb()));

    region.setCustomArgb (0x00123456u);
    REQUIRE (region.customArgb() == 0x00123456u);
    REQUIRE (region.customColour.isTransparent());
    REQUIRE (argbIsTransparent (region.customArgb()));

    region.setCustomArgb (0x01000000u);
    REQUIRE_FALSE (region.customColour.isTransparent());
    REQUIRE_FALSE (argbIsTransparent (region.customArgb()));

    region.setCustomArgb (0u);
    REQUIRE (region.customColour == juce::Colour());
}

TEST_CASE ("Track name and colour read as UTF-8 and ARGB", "[session][accessors]")
{
    Track track;
    track.name = juce::String::fromUTF8 (kNonAscii.c_str());
    track.colour = juce::Colour (0xff6090d0u);
    REQUIRE (track.nameUtf8() == kNonAscii);
    REQUIRE (track.colourArgb() == 0xff6090d0u);
}

TEST_CASE ("Region and take files read as filesystem paths, non-ASCII included", "[session][accessors]")
{
    const std::string fileName = "Aufnahme \xc3\xbc\xc3\x9f \xe5\xa3\xb0.wav";
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile (juce::String::fromUTF8 ("\xc3\xa9t\xc3\xa9"));
    const auto file = dir.getChildFile (juce::String::fromUTF8 (fileName.c_str()));

    AudioRegion region;
    REQUIRE (region.filePath().empty());

    region.file = file;
    const auto path = region.filePath();
    REQUIRE (path.u8string() == file.getFullPathName().toStdString());
    REQUIRE (path.filename().u8string() == fileName);
    REQUIRE (path.filename().u8string() == file.getFileName().toStdString());
    REQUIRE (juce::File (juce::String::fromUTF8 (path.u8string().c_str())) == file);

    AudioTake take;
    take.file = file;
    REQUIRE (take.filePath() == path);

    AudioRegion same;
    same.file = file;
    REQUIRE (region.sameFile (same));
    same.file = dir.getChildFile ("other.wav");
    REQUIRE_FALSE (region.sameFile (same));
}

TEST_CASE ("formatSamplePosition text is stable across both overloads", "[session][accessors]")
{
    static constexpr double kSr = 48000.0;
    const TempoMap noMap {};

    REQUIRE (formatSamplePosition (0, kSr, 120.0f, 4, TimeDisplayMode::Time) == "00:00.000");
    REQUIRE (formatSamplePosition (-5, kSr, 120.0f, 4, TimeDisplayMode::Time) == "00:00.000");
    REQUIRE (formatSamplePosition (1000, 0.0, 120.0f, 4, TimeDisplayMode::Time) == "00:00.000");
    REQUIRE (formatSamplePosition (62 * 48000 + 24000, kSr, 120.0f, 4, TimeDisplayMode::Time)
             == "01:02.500");
    REQUIRE (formatSamplePosition (47999, kSr, 120.0f, 4, TimeDisplayMode::Time) == "00:00.999");
    REQUIRE (formatSamplePosition (std::int64_t { 125 } * 60 * 48000, kSr, 120.0f, 4, TimeDisplayMode::Time)
             == "125:00.000");

    REQUIRE (formatSamplePosition (0, kSr, 120.0f, 4, TimeDisplayMode::Bars) == "1.1.000");
    REQUIRE (formatSamplePosition (228000, kSr, 120.0f, 4, TimeDisplayMode::Bars) == "3.2.240");
    REQUIRE (formatSamplePosition (24000 + 50, kSr, 120.0f, 4, TimeDisplayMode::Bars) == "1.2.001");
    REQUIRE (formatSamplePosition (228000, 0.0, 120.0f, 4, TimeDisplayMode::Bars) == "1.1.000");
    REQUIRE (formatSamplePosition (228000, kSr, 0.0f, 4, TimeDisplayMode::Bars) == "1.1.000");
    REQUIRE (formatSamplePosition (228000, kSr, 120.0f, 0, TimeDisplayMode::Bars) == "1.1.000");

    REQUIRE (formatSamplePosition (228000, kSr, noMap, 120.0f, 4, TimeDisplayMode::Bars) == "3.2.240");
    REQUIRE (formatSamplePosition (24000, kSr, noMap, 120.0f, 4, TimeDisplayMode::Time) == "00:00.500");

    TempoMap map;
    map.setPoints ({ { 0, 120.0f }, { 48000, 60.0f } });
    REQUIRE (formatSamplePosition (0, kSr, map, 90.0f, 4, TimeDisplayMode::Bars) == "1.1.000");
    REQUIRE (formatSamplePosition (96000 + 24000, kSr, map, 90.0f, 4, TimeDisplayMode::Bars)
             == "1.4.240");
    REQUIRE (formatSamplePosition (96000, 0.0, map, 90.0f, 4, TimeDisplayMode::Bars) == "1.1.000");
    REQUIRE (formatSamplePosition (96000, kSr, map, 90.0f, 0, TimeDisplayMode::Bars) == "1.1.000");
}
