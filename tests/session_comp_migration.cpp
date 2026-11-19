#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestTempDirectory.h"
#include "foundation/Json.h"
#include "session/CompMigration.h"
#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>
#include <vector>

namespace duskstudio
{
bool migrateSession (nlohmann::json& root, int from);
}

using namespace duskstudio;
using Catch::Matchers::WithinAbs;
using Json = dusk::json::Json;
using Id = CompParamId;

namespace
{
std::filesystem::path fixture (const char* name)
{
    return std::filesystem::u8path (DUSKSTUDIO_SOURCE_DIR) / "tests/fixtures/sessions" / name;
}

Json readJson (const std::filesystem::path& path)
{
    std::ifstream input (path, std::ios::binary);
    REQUIRE (input.is_open());
    return Json::parse (input);
}

void load (Session& session, const Json& document)
{
    test::TempDirectory dir ("dusk-comp-migration-");
    const auto file = dir.path() / "session.json";
    {
        std::ofstream stream (file);
        stream << document.dump();
        REQUIRE (stream.good());
    }
    REQUIRE (SessionSerializer::load (session, file));
}

using Expected = std::vector<std::pair<Id, float>>;

void check (const CompParams& comp, const Expected& expected)
{
    for (const auto& [id, value] : expected)
    {
        CAPTURE (compParamSpec (id).symbol);
        CHECK_THAT (comp.get (id), WithinAbs (value, 1e-4));
    }
}

void checkDefaults (const CompParams& comp, CompStage stage)
{
    for (const auto id : compStageParams (stage))
    {
        CAPTURE (compParamSpec (id).symbol);
        CHECK_THAT (comp.get (id), WithinAbs (compParamSpec (id).hostDefault, 1e-6));
    }
}

// The old strip and bus cores had no control for these, so a converted
// session sets them where the plug-in starts, except the bus side chain:
// the old bus detector's fixed high-pass measured like 27 Hz, host value
// (27 / 500) ^ 0.5.
const Expected kStripFixed { { Id::Mix, 100.0f }, { Id::SidechainHp, 0.0f }, { Id::AutoMakeup, 0.0f },
                             { Id::OptoMeter, 0.0f }, { Id::FetMeter, 0.0f } };
constexpr float kBusSidechainHp = 0.23237900f;
const Expected kBusFixed { { Id::SidechainHp, kBusSidechainHp }, { Id::AutoMakeup, 0.0f }, { Id::BusMix, 100.0f },
                           { Id::BusFadeRate, 0.674074054f }, { Id::BusFade, 0.0f } };
} // namespace

// Every value below was worked out from the two donor trees' laws separately
// from CompMigration.cpp; the reasoning for each sits with its converter.
TEST_CASE ("A format-11 session converts every compressor setting to Multi-Comp 2", "[session][serializer][comp][migration]")
{
    const auto original = readJson (fixture ("format11-compressors.json"));
    REQUIRE (original["version"].get<int>() == 11);

    auto session = std::make_unique<Session>();
    SessionLoadError error;
    REQUIRE (SessionSerializer::load (*session, fixture ("format11-compressors.json"), &error));
    CHECK (error.fileVersion == 11);

    const auto& opto = session->track (0).strip;
    CHECK (opto.compEnabled.load());
    CHECK (opto.compModePicked.load());
    CHECK (opto.compMode.load() == 0);
    check (opto.comp, {
        { Id::OptoPeakReduction, 62.5f }, { Id::OptoGain, 53.184524f }, { Id::OptoLimit, 1.0f },
        { Id::FetInput, -8.085974f }, { Id::FetOutput, 6.734586f }, { Id::FetAttack, 0.741900f },
        { Id::FetRelease, 915.0907f }, { Id::FetRatio, 1.0f }, { Id::FetThreshold, -24.0f },
        { Id::VcaThreshold, -17.073446f }, { Id::VcaCompression, 76.902833f }, { Id::VcaOutput, 3.0f } });
    check (opto.comp, kStripFixed);

    const auto& fet = session->track (1).strip;
    CHECK_FALSE (fet.compEnabled.load());
    CHECK (fet.compModePicked.load());
    CHECK (fet.compMode.load() == 1);
    check (fet.comp, {
        { Id::OptoPeakReduction, 20.0f }, { Id::OptoGain, 12.086614f }, { Id::OptoLimit, 0.0f },
        { Id::FetInput, 4.582213f }, { Id::FetOutput, 13.945981f }, { Id::FetAttack, 0.493283f },
        { Id::FetRelease, 417.4357f }, { Id::FetRatio, 4.0f }, { Id::FetThreshold, -18.0f },
        { Id::VcaThreshold, -27.402928f }, { Id::VcaCompression, 18.857447f }, { Id::VcaOutput, -4.0f } });
    check (fet.comp, kStripFixed);

    const auto& vca = session->track (2).strip;
    CHECK (vca.compEnabled.load());
    CHECK_FALSE (vca.compModePicked.load());
    CHECK (vca.compMode.load() == 2);
    check (vca.comp, {
        { Id::OptoPeakReduction, 80.0f }, { Id::OptoGain, 18.903125f }, { Id::OptoLimit, 1.0f },
        { Id::FetInput, -20.0f }, { Id::FetOutput, 20.0f }, { Id::FetAttack, 0.0f },
        { Id::FetRelease, 942.9251f }, { Id::FetRatio, 3.0f }, { Id::FetThreshold, -40.0f },
        { Id::VcaThreshold, -3.0396f }, { Id::VcaCompression, 97.828711f }, { Id::VcaOutput, -12.0f } });
    check (vca.comp, kStripFixed);

    // Bus and master threshold knob: (old threshold + the old ratio's offset
    // 39.85 / 37.60 / 36.61 - (new onset at the new attack and release - its
    // -35.17 at 10 ms and Auto)) / 1.3186, less 4 / 1.3186 per headroom step
    // taken off while it is above +15.

    // Old 10:1, 3 ms, 0.6 s, -18 dB: attack 3 ms, release 1.2 s, onset -37.89;
    // (-18 + 36.61 + 2.72) / 1.3186 = 16.18 is past the top, so headroom 2.
    CHECK (session->bus (0).strip.compEnabled.load());
    check (session->bus (0).strip.comp, {
        { Id::BusThreshold, 13.142727f }, { Id::BusRatio, 0.0f }, { Id::BusAttack, 3.0f },
        { Id::BusRelease, 3.0f }, { Id::BusHeadroom, 2.0f }, { Id::BusMakeup, 4.5f } });
    check (session->bus (0).strip.comp, kBusFixed);

    // Old 5:1 (the 4:1 step), 1 ms, Auto, -30 dB: attack 1 ms, release 0.6 s,
    // onset -37.86.
    CHECK_FALSE (session->bus (1).strip.compEnabled.load());
    check (session->bus (1).strip.comp, {
        { Id::BusThreshold, 7.803731f }, { Id::BusRatio, 0.0f }, { Id::BusAttack, 2.0f },
        { Id::BusRelease, 2.0f }, { Id::BusHeadroom, 3.0f }, { Id::BusMakeup, 2.0f } });
    check (session->bus (1).strip.comp, kBusFixed);

    // Old 2:1, 25 ms, 0.3 s, -45 dB, makeup -4: attack 30 ms, release 1.2 s,
    // onset -34.82; makeup below 0 dB has no counterpart.
    CHECK (session->master().compEnabled.load());
    check (session->master().comp, {
        { Id::BusThreshold, -4.171091f }, { Id::BusRatio, 0.0f }, { Id::BusAttack, 5.0f },
        { Id::BusRelease, 3.0f }, { Id::BusHeadroom, 3.0f }, { Id::BusMakeup, 0.0f } });
    check (session->master().comp, kBusFixed);

    CHECK (session->mastering().compEnabled.load());
    checkDefaults (session->mastering().comp, CompStage::Multiband);

    const auto resaved = Json::parse (SessionSerializer::serialize (*session).toStdString());
    CHECK (resaved["version"].get<int>() == 12);
    CHECK_THAT (resaved["tracks"][0]["comp"]["opto_gain"].get<float>(), WithinAbs (53.184524f, 1e-4));
    CHECK (resaved["buses"][0]["comp"]["bus_headroom"].get<int>() == 2);
}

TEST_CASE ("The format 11 to 12 step leaves none of the old compressor keys", "[session][comp][migration]")
{
    auto root = readJson (fixture ("format11-compressors.json"));
    REQUIRE (migrateSession (root, 11));
    CHECK (root["version"].get<int>() == 12);

    for (const auto& track : root["tracks"])
    {
        const auto& comp = track["comp"];
        for (const auto* key : { "opto_peak_red", "fet_threshold_db", "vca_thresh_db", "vca_ratio", "vca_attack",
                                 "vca_release", "vca_overeasy", "vca_detector_classic" })
            CHECK_FALSE (comp.contains (key));
        for (const auto id : compStageParams (CompStage::Strip))
            CHECK (comp.contains (compParamSpec (id).symbol));
    }
    for (const auto* section : { "master", "mastering" })
        for (const auto* key : { "comp_enabled", "comp_thresh_db", "comp_ratio", "comp_attack_ms",
                                 "comp_release_ms", "comp_release_auto", "comp_makeup_db" })
        {
            CAPTURE (section, key);
            CHECK_FALSE (root[section].contains (key));
            CHECK_FALSE (root["buses"][0].contains (key));
        }
    CHECK (root["buses"][0]["comp"]["enabled"].get<bool>());
    CHECK (root["master"]["comp"]["enabled"].get<bool>());
    CHECK (root["mastering"]["comp"]["enabled"].get<bool>());
}

TEST_CASE ("The released format-10 session loads through both migrations with its compressors converted", "[session][serializer][comp][migration]")
{
    auto session = std::make_unique<Session>();
    SessionLoadError error;
    REQUIRE (SessionSerializer::load (*session, fixture ("v0.15.1-format10.json"), &error));
    CHECK (error.fileVersion == 10);

    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        CAPTURE (t);
        CHECK_FALSE (session->track (t).strip.compEnabled.load());
        check (session->track (t).strip.comp, {
            { Id::OptoPeakReduction, 0.0f }, { Id::OptoGain, 23.409226f }, { Id::OptoLimit, 0.0f },
            { Id::FetInput, -15.479474f }, { Id::FetOutput, 11.659604f }, { Id::FetAttack, 0.696670f },
            { Id::FetRelease, 854.0559f }, { Id::FetRatio, 0.0f }, { Id::FetThreshold, -10.0f },
            { Id::VcaThreshold, 0.0f }, { Id::VcaCompression, 48.451684f }, { Id::VcaOutput, 0.0f } });
    }
    // The old defaults, 4:1, 10 ms, Auto, 0 dB: (0 + 37.60 + 0.35) / 1.3186
    // = 28.78 stays past +15 with every headroom step taken off.
    const Expected oldBusDefaults { { Id::BusThreshold, 15.0f }, { Id::BusRatio, 0.0f }, { Id::BusAttack, 4.0f },
                                    { Id::BusRelease, 2.0f }, { Id::BusHeadroom, 0.0f }, { Id::BusMakeup, 0.0f },
                                    { Id::SidechainHp, kBusSidechainHp } };
    for (int b = 0; b < Session::kNumBuses; ++b)
    {
        CAPTURE (b);
        check (session->bus (b).strip.comp, oldBusDefaults);
    }
    check (session->master().comp, oldBusDefaults);
    checkDefaults (session->mastering().comp, CompStage::Multiband);
    CHECK_FALSE (session->mastering().compEnabled.load());
}

// A track or bus filled a missing or unreadable key from the old model's
// default, so the conversion starts from that default; the master and mastering
// kept their live value, so the converted key stays absent and the new loader
// keeps the live value as well.
TEST_CASE ("The format 11 to 12 step converts what the old loader would have read", "[session][comp][migration]")
{
    Json root {
        { "version", 11 },
        { "tracks", Json::array ({
            Json { { "comp", Json { { "vca_ratio", "x" }, { "fet_attack", 1e40 }, { "opto_gain", 500.0 } } } },
            Json { { "name", "No comp" } },
            Json { { "comp", 42 } } }) },
        { "buses", Json::array ({ Json { { "comp_ratio", 99.0 }, { "comp_makeup_db", -8.0 } } }) },
        { "master", Json { { "comp_release_auto", false } } },
        { "mastering", Json { { "eq_enabled", true } } } };
    REQUIRE (migrateSession (root, 11));

    const auto& first = root["tracks"][0]["comp"];
    CHECK_THAT (first["vca_compression"].get<float>(), WithinAbs (legacycomp::vcaCompression (4.0f), 1e-6));
    CHECK_THAT (first["fet_attack"].get<float>(), WithinAbs (legacycomp::fetAttack (0.2f, 0), 1e-6));
    CHECK_THAT (first["opto_gain"].get<float>(), WithinAbs (legacycomp::optoGain (100.0f), 1e-6));
    for (const std::size_t t : { 1u, 2u })
    {
        const auto& comp = root["tracks"][t]["comp"];
        CAPTURE (t);
        CHECK_FALSE (comp.contains ("enabled"));
        CHECK_THAT (comp["vca_threshold"].get<float>(), WithinAbs (legacycomp::vcaThreshold (12.0f), 1e-6));
        CHECK_THAT (comp["opto_gain"].get<float>(), WithinAbs (compParamSpec (Id::OptoGain).hostDefault, 1e-4));
    }

    const auto& bus = root["buses"][0]["comp"];
    CHECK (bus["bus_ratio"].get<int>() == 0);
    CHECK_THAT (bus["bus_makeup"].get<float>(), WithinAbs (0.0f, 1e-6));
    CHECK_THAT (bus["bus_threshold"].get<float>(),
                WithinAbs (legacycomp::busThreshold (0.0f, 10.0f, 4, 2).threshold, 1e-6));
    CHECK (bus["bus_headroom"].get<int>() == legacycomp::busThreshold (0.0f, 10.0f, 4, 2).headroom);
    CHECK (bus["bus_attack"].get<int>() == 4);
    CHECK (bus["bus_release"].get<int>() == 2);
    CHECK_FALSE (bus.contains ("enabled"));

    const auto& master = root["master"]["comp"];
    CHECK (master["bus_release"].get<int>() == 2);
    for (const auto* key : { "enabled", "bus_threshold", "bus_ratio", "bus_attack", "bus_makeup" })
        CHECK_FALSE (master.contains (key));
    CHECK (master["bus_headroom"].get<int>() == 3);

    const auto& mastering = root["mastering"]["comp"];
    CHECK_FALSE (mastering.contains ("enabled"));
    CHECK (mastering["mb_enabled_3"].get<int>() == 1);

    auto session = std::make_unique<Session>();
    session->master().comp.set (Id::BusThreshold, -9.0f);
    load (*session, root);
    CHECK_THAT (session->master().comp.get (Id::BusThreshold), WithinAbs (-9.0f, 1e-6));
    CHECK_THAT (session->master().comp.get (Id::BusRelease), WithinAbs (2.0f, 1e-6));
}

TEST_CASE ("Legacy compressor conversions keep the old settings' sound", "[session][comp][migration]")
{
    SECTION ("Opto gain lands on the same decibels")
    {
        CHECK_THAT (legacycomp::optoGain (50.0f), WithinAbs (23.409226f, 1e-4));
        CHECK_THAT (legacycomp::optoGain (0.0f), WithinAbs (9.695735f, 1e-4));
        CHECK_THAT (legacycomp::optoGain (100.0f), WithinAbs (94.583336f, 1e-4));
        // Rising until the new knob's +37.7 dB top, which old settings past
        // 97.1 % reach.
        float previous = -1.0f;
        for (float pct = 0.0f; pct <= 95.0f; pct += 2.5f)
        {
            const float knob = legacycomp::optoGain (pct);
            CHECK (knob > previous);
            previous = knob;
        }
        CHECK_THAT (legacycomp::optoGain (98.0f), WithinAbs (legacycomp::optoGain (100.0f), 1e-6));
    }
    SECTION ("FET input and output keep their applied gain")
    {
        CHECK_THAT (legacycomp::fetInput (40.0f), WithinAbs (40.0f, 1e-6));
        CHECK_THAT (legacycomp::fetInput (-20.0f), WithinAbs (-20.0f, 1e-6));
        CHECK_THAT (legacycomp::fetInput (0.0f), WithinAbs (-15.479474f, 1e-4));
        CHECK_THAT (legacycomp::fetOutput (20.0f), WithinAbs (20.0f, 1e-6));
        CHECK_THAT (legacycomp::fetOutput (0.0f), WithinAbs (11.659604f, 1e-4));
    }
    SECTION ("FET release runs the other way")
    {
        CHECK_THAT (legacycomp::fetRelease (1100.0f), WithinAbs (50.0f, 1e-6));
        float previous = 1101.0f;
        for (float ms = 50.0f; ms <= 1100.0f; ms += 50.0f)
        {
            const float value = legacycomp::fetRelease (ms);
            CHECK (value <= previous);
            CHECK (value >= 50.0f);
            CHECK (value <= 1100.0f);
            previous = value;
        }
    }
    SECTION ("FET attack slower than the new law takes the slow stop")
    {
        CHECK_THAT (legacycomp::fetAttack (80.0f, 0), WithinAbs (0.0f, 1e-6));
        CHECK_THAT (legacycomp::fetAttack (1.0f, 0), WithinAbs (0.0f, 1e-6));
        CHECK_THAT (legacycomp::fetAttack (0.2f, 0), WithinAbs (0.696670f, 1e-4));
        CHECK_THAT (legacycomp::fetAttack (0.02f, 0), WithinAbs (legacycomp::fetAttack (0.1f, 0), 1e-6));
        CHECK (legacycomp::fetAttack (0.2f, 3) < legacycomp::fetAttack (0.2f, 0));
    }
    SECTION ("VCA starts compressing at the old RMS level")
    {
        // -27.0103 dB RMS is a -24 dB sine peak, a calibration knot (+0.0282 dB).
        CHECK_THAT (legacycomp::vcaThreshold (-27.0103f), WithinAbs (-27.0103f + 3.0103f - 0.29f + 0.0282f, 1e-4));
        CHECK_THAT (legacycomp::vcaThreshold (-2.0f), WithinAbs (0.0f, 1e-6));
        CHECK_THAT (legacycomp::vcaCompression (87.972f), WithinAbs (97.5f, 1e-3));
        CHECK_THAT (legacycomp::vcaCompression (1.0f), WithinAbs (0.0f, 1e-6));
        CHECK_THAT (legacycomp::vcaCompression (4.137f), WithinAbs (50.0f, 1e-3));
    }
    SECTION ("bus steps go to the new steps that play closest at 2:1")
    {
        // The old steps 0.1 / 0.3 / 1 ms all played about as fast as the new 1 ms.
        CHECK (legacycomp::busAttack (0.1f) == 2);
        CHECK (legacycomp::busAttack (0.3f) == 2);
        CHECK (legacycomp::busAttack (1.0f) == 2);
        CHECK (legacycomp::busAttack (3.0f) == 3);
        CHECK (legacycomp::busAttack (10.0f) == 4);
        CHECK (legacycomp::busAttack (30.0f) == 5);
        // Old releases ran four times their label; old Auto was a fixed 0.6 s.
        CHECK (legacycomp::busRelease (100.0f, false) == 2);
        CHECK (legacycomp::busRelease (300.0f, false) == 3);
        CHECK (legacycomp::busRelease (600.0f, false) == 3);
        CHECK (legacycomp::busRelease (1000.0f, false) == 3);
        CHECK (legacycomp::busRelease (100.0f, true) == 2);
        CHECK_THAT (legacycomp::busMakeup (-10.0f), WithinAbs (0.0f, 1e-6));
        CHECK_THAT (legacycomp::busSidechainHp(), WithinAbs (kBusSidechainHp, 1e-7));
    }
    SECTION ("bus threshold starts the new 2:1 where the old step started")
    {
        // At 10 ms and Auto the onset table adds nothing: (thr + offset) / 1.3186.
        const auto twoToOne = legacycomp::busThreshold (-40.0f, 2.0f, 4, 4);
        CHECK_THAT (twoToOne.threshold, WithinAbs ((-40.0f + 39.85f) / 1.3186f, 1e-5));
        CHECK (twoToOne.headroom == 3);
        // 0.1 ms and 0.1 s set in 1.70 dB lower than 10 ms and Auto, so the
        // knob sits 1.70 dB higher to start at the same level.
        const auto fast = legacycomp::busThreshold (-40.0f, 2.0f, 0, 0);
        CHECK_THAT (fast.threshold, WithinAbs ((-40.0f + 39.85f + 1.70f) / 1.3186f, 1e-5));
        // Old 4:1 at -15 dB: 17.14 is past +15, one headroom step brings it in.
        const auto hot = legacycomp::busThreshold (-15.0f, 4.0f, 4, 4);
        CHECK (hot.headroom == 2);
        CHECK_THAT (hot.threshold, WithinAbs ((-15.0f + 37.60f - 4.0f) / 1.3186f, 1e-5));
        // Out of reach at headroom 0: the knob holds at its top.
        const auto top = legacycomp::busThreshold (0.0f, 2.0f, 5, 3);
        CHECK (top.headroom == 0);
        CHECK_THAT (top.threshold, WithinAbs (15.0f, 1e-6));
    }
}
