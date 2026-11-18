#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestTempDirectory.h"
#include "foundation/Json.h"
#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <cmath>
#include <fstream>
#include <memory>
#include <string>

using namespace duskstudio;
using Catch::Matchers::WithinAbs;
using Json = dusk::json::Json;

namespace
{
void load (Session& session, const Json& document)
{
    test::TempDirectory dir ("dusk-comp-serial-");
    const auto file = dir.path() / "session.json";
    {
        std::ofstream stream (file);
        stream << document.dump();
        REQUIRE (stream.good());
    }
    REQUIRE (SessionSerializer::load (session, file));
}

Json saved (const Session& session)
{
    return Json::parse (SessionSerializer::serialize (session).toStdString());
}

// A value off the default, distinct per salt, inside the range.
float offDefault (CompParamId id, int salt)
{
    const auto& spec = compParamSpec (id);
    if (spec.integer)
    {
        const int span = (int) (spec.hostMax - spec.hostMin);
        const int def = (int) (spec.hostDefault - spec.hostMin);
        return spec.hostMin + (float) ((def + 1 + salt % span) % (span + 1));
    }
    const float frac = 0.13f + 0.07f * (float) (salt % 9);
    const float v = spec.hostMin + frac * (spec.hostMax - spec.hostMin);
    return std::abs (v - spec.hostDefault) < 1e-6f ? spec.hostMin : v;
}

void fill (CompParams& comp, CompStage stage, int salt)
{
    int i = salt;
    for (const auto id : compStageParams (stage))
        comp.set (id, offDefault (id, i++));
}

void checkEqual (const CompParams& a, const CompParams& b, CompStage stage)
{
    for (const auto id : compStageParams (stage))
    {
        CAPTURE (compParamSpec (id).symbol);
        CHECK_THAT (b.get (id), WithinAbs (a.get (id), 1e-6));
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

void checkKeys (const Json& comp, CompStage stage)
{
    REQUIRE (comp.is_object());
    CHECK (comp.contains ("enabled"));
    for (const auto id : compStageParams (stage))
    {
        const auto& spec = compParamSpec (id);
        CAPTURE (spec.symbol);
        REQUIRE (comp.contains (spec.symbol));
        CHECK (comp[spec.symbol].is_number());
        if (spec.integer) CHECK (comp[spec.symbol].is_number_integer());
    }
}

const char* const kOldTrackKeys[] { "opto_peak_red", "fet_threshold_db", "vca_thresh_db", "vca_ratio",
                                    "vca_attack", "vca_release", "vca_overeasy", "vca_detector_classic" };
const char* const kOldFlatKeys[] { "comp_enabled", "comp_thresh_db", "comp_ratio", "comp_attack_ms",
                                   "comp_release_ms", "comp_release_auto", "comp_makeup_db" };
} // namespace

TEST_CASE ("SessionSerializer round-trips every stored compressor parameter", "[session][serializer][comp]")
{
    auto a = std::make_unique<Session>();
    a->track (0).strip.compEnabled.store (true);
    a->track (0).strip.compMode.store (2);
    a->track (0).strip.compModePicked.store (true);
    fill (a->track (0).strip.comp, CompStage::Strip, 1);
    fill (a->track (5).strip.comp, CompStage::Strip, 4);
    a->bus (2).strip.compEnabled.store (true);
    fill (a->bus (2).strip.comp, CompStage::Bus, 2);
    a->master().compEnabled.store (true);
    fill (a->master().comp, CompStage::Bus, 5);
    a->mastering().compEnabled.store (true);
    fill (a->mastering().comp, CompStage::Multiband, 3);

    const auto document = saved (*a);
    CHECK (document["version"].get<int>() == 12);
    const auto& track = document["tracks"][0]["comp"];
    checkKeys (track, CompStage::Strip);
    CHECK (track["mode"].get<int>() == 2);
    CHECK (track["mode_picked"].get<bool>());
    for (const auto* key : kOldTrackKeys) CHECK_FALSE (track.contains (key));
    checkKeys (document["buses"][2]["comp"], CompStage::Bus);
    checkKeys (document["master"]["comp"], CompStage::Bus);
    checkKeys (document["mastering"]["comp"], CompStage::Multiband);
    for (const auto* key : kOldFlatKeys)
    {
        CAPTURE (key);
        CHECK_FALSE (document["buses"][2].contains (key));
        CHECK_FALSE (document["master"].contains (key));
        CHECK_FALSE (document["mastering"].contains (key));
    }

    auto b = std::make_unique<Session>();
    load (*b, document);
    CHECK (b->track (0).strip.compEnabled.load());
    CHECK (b->track (0).strip.compMode.load() == 2);
    CHECK (b->track (0).strip.compModePicked.load());
    checkEqual (a->track (0).strip.comp, b->track (0).strip.comp, CompStage::Strip);
    checkEqual (a->track (5).strip.comp, b->track (5).strip.comp, CompStage::Strip);
    CHECK (b->bus (2).strip.compEnabled.load());
    checkEqual (a->bus (2).strip.comp, b->bus (2).strip.comp, CompStage::Bus);
    CHECK (b->master().compEnabled.load());
    checkEqual (a->master().comp, b->master().comp, CompStage::Bus);
    CHECK (b->mastering().compEnabled.load());
    checkEqual (a->mastering().comp, b->mastering().comp, CompStage::Multiband);
    checkDefaults (b->track (1).strip.comp, CompStage::Strip);
}

TEST_CASE ("SessionSerializer clamps corrupt compressor values on load", "[session][serializer][comp][corruption]")
{
    auto s = std::make_unique<Session>();
    auto document = saved (*s);
    auto& track = document["tracks"][0]["comp"];
    track["vca_threshold"] = 1e30;
    track["fet_input"] = -1e30;
    track["fet_ratio"] = 2.6;
    track["opto_meter"] = 7;
    track["fet_attack"] = "fast";
    track["opto_gain"] = 1e40;
    track["sidechain_hp"] = nullptr;
    auto& bus = document["buses"][0]["comp"];
    bus["bus_threshold"] = -1e30;
    bus["bus_ratio"] = 7;
    bus["bus_mix"] = 1e40;
    bus["bus_fade_rate"] = 3.0;
    auto& master = document["master"]["comp"];
    master["bus_attack"] = "x";
    master["bus_makeup"] = 50.0;
    master["enabled"] = "maybe";
    auto& mastering = document["mastering"]["comp"];
    mastering["mb_ratio_2"] = -4.0;
    mastering["mb_enabled_1"] = 0.4;
    mastering["mb_output"] = true;

    s->track (0).strip.comp.set (CompParamId::FetAttack, 0.9f);
    s->track (0).strip.comp.set (CompParamId::SidechainHp, 0.5f);
    s->master().comp.set (CompParamId::BusAttack, 5.0f);
    s->master().compEnabled.store (true);
    load (*s, document);

    const auto& t = s->track (0).strip.comp;
    CHECK_THAT (t.get (CompParamId::VcaThreshold), WithinAbs (0.0f, 1e-6));
    CHECK_THAT (t.get (CompParamId::FetInput), WithinAbs (-20.0f, 1e-6));
    CHECK_THAT (t.get (CompParamId::FetRatio), WithinAbs (3.0f, 1e-6));
    CHECK_THAT (t.get (CompParamId::OptoMeter), WithinAbs (2.0f, 1e-6));
    CHECK_THAT (t.get (CompParamId::FetAttack), WithinAbs (compParamSpec (CompParamId::FetAttack).hostDefault, 1e-6));
    CHECK_THAT (t.get (CompParamId::OptoGain), WithinAbs (compParamSpec (CompParamId::OptoGain).hostDefault, 1e-6));
    CHECK_THAT (t.get (CompParamId::SidechainHp), WithinAbs (0.0f, 1e-6));

    const auto& b = s->bus (0).strip.comp;
    CHECK_THAT (b.get (CompParamId::BusThreshold), WithinAbs (-30.0f, 1e-6));
    CHECK_THAT (b.get (CompParamId::BusRatio), WithinAbs (2.0f, 1e-6));
    CHECK_THAT (b.get (CompParamId::BusMix), WithinAbs (100.0f, 1e-6));
    CHECK_THAT (b.get (CompParamId::BusFadeRate), WithinAbs (1.0f, 1e-6));

    const auto& m = s->master().comp;
    CHECK_THAT (m.get (CompParamId::BusAttack), WithinAbs (2.0f, 1e-6));
    CHECK_THAT (m.get (CompParamId::BusMakeup), WithinAbs (20.0f, 1e-6));
    CHECK_FALSE (s->master().compEnabled.load());

    const auto& mb = s->mastering().comp;
    CHECK_THAT (mb.get (compBandParam (2, CompBandField::Ratio)), WithinAbs (0.0f, 1e-6));
    CHECK_THAT (mb.get (compBandParam (1, CompBandField::Enabled)), WithinAbs (0.0f, 1e-6));
    CHECK_THAT (mb.get (CompParamId::MbOutput), WithinAbs (0.0f, 1e-6));
}

// Tracks and buses fill what their slot leaves out from the model defaults;
// the master and mastering sections keep the live value for a key they omit
// and reset only when the whole section is gone.
TEST_CASE ("SessionSerializer applies each section's rule to missing compressor keys", "[session][serializer][comp]")
{
    auto s = std::make_unique<Session>();
    fill (s->track (0).strip.comp, CompStage::Strip, 2);
    fill (s->bus (0).strip.comp, CompStage::Bus, 3);
    fill (s->master().comp, CompStage::Bus, 4);
    fill (s->mastering().comp, CompStage::Multiband, 5);
    s->master().compEnabled.store (true);
    s->mastering().compEnabled.store (true);
    const float liveMasterThreshold = s->master().comp.get (CompParamId::BusThreshold);
    const float liveBand = s->mastering().comp.get (compBandParam (0, CompBandField::Makeup));

    SECTION ("keys missing")
    {
        Json document {
            { "version", 12 },
            { "tracks", Json::array ({ Json { { "comp", Json { { "fet_ratio", 3 } } } } }) },
            { "buses", Json::array ({ Json { { "comp", Json { { "bus_ratio", 1 } } } } }) },
            { "master", Json { { "comp", Json { { "bus_ratio", 1 } } } } },
            { "mastering", Json { { "comp", Json { { "mb_mix", 40.0 } } } } } };
        load (*s, document);

        CHECK_THAT (s->track (0).strip.comp.get (CompParamId::FetRatio), WithinAbs (3.0f, 1e-6));
        CHECK_THAT (s->track (0).strip.comp.get (CompParamId::VcaThreshold), WithinAbs (-27.0f, 1e-6));
        CHECK_THAT (s->bus (0).strip.comp.get (CompParamId::BusRatio), WithinAbs (1.0f, 1e-6));
        CHECK_THAT (s->bus (0).strip.comp.get (CompParamId::BusAttack), WithinAbs (2.0f, 1e-6));
        CHECK_THAT (s->master().comp.get (CompParamId::BusRatio), WithinAbs (1.0f, 1e-6));
        CHECK_THAT (s->master().comp.get (CompParamId::BusThreshold), WithinAbs (liveMasterThreshold, 1e-6));
        CHECK (s->master().compEnabled.load());
        CHECK_THAT (s->mastering().comp.get (CompParamId::MbMix), WithinAbs (40.0f, 1e-6));
        CHECK_THAT (s->mastering().comp.get (compBandParam (0, CompBandField::Makeup)), WithinAbs (liveBand, 1e-6));
        CHECK (s->mastering().compEnabled.load());
    }
    SECTION ("comp objects missing")
    {
        Json document {
            { "version", 12 },
            { "tracks", Json::array ({ Json { { "name", "No comp" } } }) },
            { "buses", Json::array ({ Json { { "name", "No comp" } } }) },
            { "master", Json { { "fader_db", -2.0 } } },
            { "mastering", Json { { "eq_enabled", true } } } };
        load (*s, document);

        checkDefaults (s->track (0).strip.comp, CompStage::Strip);
        checkDefaults (s->bus (0).strip.comp, CompStage::Bus);
        CHECK_THAT (s->master().comp.get (CompParamId::BusThreshold), WithinAbs (liveMasterThreshold, 1e-6));
        CHECK (s->master().compEnabled.load());
        CHECK_THAT (s->mastering().comp.get (compBandParam (0, CompBandField::Makeup)), WithinAbs (liveBand, 1e-6));
        CHECK (s->mastering().compEnabled.load());
    }
    SECTION ("sections missing")
    {
        load (*s, Json { { "version", 12 } });

        checkDefaults (s->track (0).strip.comp, CompStage::Strip);
        checkDefaults (s->bus (0).strip.comp, CompStage::Bus);
        checkDefaults (s->master().comp, CompStage::Bus);
        CHECK_FALSE (s->master().compEnabled.load());
        checkDefaults (s->mastering().comp, CompStage::Multiband);
        CHECK_FALSE (s->mastering().compEnabled.load());
    }
}
