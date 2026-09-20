#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestTempDirectory.h"
#include "engine/CompModeMap.h"
#include "foundation/Json.h"
#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <algorithm>
#include <fstream>
#include <utility>
#include <vector>

using namespace duskstudio;
using Catch::Matchers::WithinAbs;
using Json = dusk::json::Json;

namespace
{
struct Number
{
    const char* path;
    std::atomic<float>* value;
    float initial, low, high;
};

struct Toggle
{
    const char* path;
    std::atomic<bool>* value;
    bool initial = false;
};

void load (Session& session, const Json& document)
{
    test::TempDirectory dir ("dusk-appendix-");
    const auto file = dir.path() / "session.json";
    {
        std::ofstream stream (file);
        stream << document.dump();
        REQUIRE (stream.good());
    }
    REQUIRE (SessionSerializer::load (session, file));
}

void checkParameters (Session& session, const std::vector<Number>& numbers,
                      const std::vector<Toggle>& toggles)
{
    for (const auto& p : numbers)
    {
        CAPTURE (p.path);
        CHECK_THAT (p.value->load(), WithinAbs (p.initial, 1e-6));
    }
    for (const auto& p : toggles)
    {
        CAPTURE (p.path);
        CHECK (p.value->load() == p.initial);
    }

    // Independent values from Appendix A; both valid endpoints must survive
    // load, and corrupt values must stop at the same bounds.
    for (int pass = 0; pass < 4; ++pass)
    {
        const bool upper = pass % 2 != 0;
        auto document = Json::parse (SessionSerializer::serialize (session).toStdString());
        for (const auto& p : numbers)
            document[Json::json_pointer (p.path)] = pass < 2 ? (upper ? p.high : p.low)
                                                            : (upper ? 1e20f : -1e20f);
        for (const auto& p : toggles)
            document[Json::json_pointer (p.path)] = upper;
        load (session, document);
        for (const auto& p : numbers)
        {
            CAPTURE (p.path, pass);
            CHECK_THAT (p.value->load(), WithinAbs (upper ? p.high : p.low, 1e-6));
        }
        for (const auto& p : toggles)
        {
            CAPTURE (p.path, pass);
            CHECK (p.value->load() == upper);
        }
    }
}
}

TEST_CASE ("Appendix A: channel defaults and load ranges", "[session][appendix]")
{
    Session session;
    auto& p = session.track (0).strip;
    CHECK_FALSE (p.eqBlackMode.load());
    CHECK (p.compMode.load() == 0);
    CHECK (p.compFetRatio.load() == 0);
    for (size_t i = 0; i < 4; ++i)
    {
        CHECK_FALSE (p.busAssign[i].load());
        CHECK_FALSE (p.auxSendPreFader[i].load());
        CHECK_THAT (p.auxSendDb[i].load(), WithinAbs (-100, 1e-6));
    }
    checkParameters (session, {
        { "/tracks/0/fader_db", &p.faderDb, 0, -100, 12 },
        { "/tracks/0/pan", &p.pan, 0, -1, 1 },
        { "/tracks/0/hpf/freq", &p.hpfFreq, 20, 20, 300 },
        { "/tracks/0/lpf/freq", &p.lpfFreq, 20000, 3000, 20000 },
        { "/tracks/0/eq/lf/freq", &p.lfFreq, 100, 20, 400 },
        { "/tracks/0/eq/lf/gain", &p.lfGainDb, 0, -15, 15 },
        { "/tracks/0/eq/lm/freq", &p.lmFreq, 600, 100, 4000 },
        { "/tracks/0/eq/lm/gain", &p.lmGainDb, 0, -15, 15 },
        { "/tracks/0/eq/lm/q", &p.lmQ, 0.7f, 0.4f, 4 },
        { "/tracks/0/eq/hm/freq", &p.hmFreq, 2000, 600, 13000 },
        { "/tracks/0/eq/hm/gain", &p.hmGainDb, 0, -15, 15 },
        { "/tracks/0/eq/hm/q", &p.hmQ, 0.7f, 0.4f, 4 },
        { "/tracks/0/eq/hf/freq", &p.hfFreq, 8000, 1000, 20000 },
        { "/tracks/0/eq/hf/gain", &p.hfGainDb, 0, -15, 15 },
        { "/tracks/0/comp/opto_gain", &p.compOptoGain, 50, 0, 100 },
        { "/tracks/0/comp/fet_threshold_db", &p.compFetThresholdDb, -10, -60, 0 },
        { "/tracks/0/comp/fet_attack", &p.compFetAttack, 0.2f, 0.02f, 80 },
        { "/tracks/0/comp/fet_release", &p.compFetRelease, 400, 50, 1100 },
        { "/tracks/0/comp/fet_output", &p.compFetOutput, 0, -20, 20 },
        { "/tracks/0/comp/vca_thresh_db", &p.compVcaThreshDb, 12, -38, 12 },
        { "/tracks/0/comp/vca_ratio", &p.compVcaRatio, 4, 1, 120 },
        { "/tracks/0/comp/vca_attack", &p.compVcaAttack, 1, 0.1f, 50 },
        { "/tracks/0/comp/vca_release", &p.compVcaRelease, 100, 10, 5000 },
        { "/tracks/0/comp/vca_output", &p.compVcaOutput, 0, -20, 20 },
    }, {
        { "/tracks/0/mute", &p.mute }, { "/tracks/0/solo", &p.solo },
        { "/tracks/0/phase_invert", &p.phaseInvert },
        { "/tracks/0/hpf/enabled", &p.hpfEnabled },
        { "/tracks/0/lpf/enabled", &p.lpfEnabled },
        { "/tracks/0/eq/enabled", &p.eqEnabled },
        { "/tracks/0/comp/enabled", &p.compEnabled },
        { "/tracks/0/comp/vca_overeasy", &p.compVcaOverEasy },
        { "/tracks/0/comp/vca_detector_classic", &p.compVcaDetectorClassic },
    });
    CHECK_THAT (comp::optoGainPctToMakeupDb (0), WithinAbs (-40, 1e-6));
    CHECK_THAT (comp::optoGainPctToMakeupDb (50), WithinAbs (0, 1e-6));
    CHECK_THAT (comp::optoGainPctToMakeupDb (100), WithinAbs (40, 1e-6));

    for (int choice = 0; choice < 5; ++choice)
    {
        auto document = Json::parse (SessionSerializer::serialize (session).toStdString());
        document["tracks"][0]["eq"]["type"] = choice % 2 ? "black" : "brown";
        document["tracks"][0]["comp"]["mode"] = choice % 3;
        document["tracks"][0]["comp"]["fet_ratio"] = choice;
        load (session, document);
        CHECK (p.eqBlackMode.load() == (choice % 2 != 0));
        CHECK (p.compMode.load() == choice % 3);
        CHECK (p.compFetRatio.load() == choice);
    }
    for (float send : { -100.0f, -60.0f, -59.9f, 6.0f, 100.0f })
    {
        auto document = Json::parse (SessionSerializer::serialize (session).toStdString());
        for (size_t i = 0; i < 4; ++i)
        {
            document["tracks"][0]["aux_send_db"][i] = send;
            document["tracks"][0]["aux_send_pre_fader"][i] = send > 0;
            document["tracks"][0]["bus_assign"][i] = send > 0;
        }
        load (session, document);
        for (size_t i = 0; i < 4; ++i)
        {
            CAPTURE (i, send);
            CHECK_THAT (p.auxSendDb[i].load(),
                        WithinAbs (send <= -60 ? -100 : std::min (send, 6.0f), 1e-6));
            CHECK (p.auxSendPreFader[i].load() == (send > 0));
            CHECK (p.busAssign[i].load() == (send > 0));
        }
    }
}

TEST_CASE ("Appendix A: bus defaults and load ranges", "[session][appendix]")
{
    Session session;
    auto& p = session.bus (0).strip;
    checkParameters (session, {
        { "/buses/0/fader_db", &p.faderDb, 0, -100, 12 },
        { "/buses/0/pan", &p.pan, 0, -1, 1 },
        { "/buses/0/eq_lf_db", &p.eqLfGainDb, 0, -9, 9 },
        { "/buses/0/eq_mid_db", &p.eqMidGainDb, 0, -9, 9 },
        { "/buses/0/eq_hf_db", &p.eqHfGainDb, 0, -9, 9 },
        { "/buses/0/comp_thresh_db", &p.compThreshDb, 0, -60, 0 },
        { "/buses/0/comp_ratio", &p.compRatio, 4, 1, 10 },
        { "/buses/0/comp_attack_ms", &p.compAttackMs, 10, 0.1f, 50 },
        { "/buses/0/comp_release_ms", &p.compReleaseMs, 100, 50, 1000 },
        { "/buses/0/comp_makeup_db", &p.compMakeupDb, 0, -10, 20 },
    }, {
        { "/buses/0/mute", &p.mute }, { "/buses/0/solo", &p.solo },
        { "/buses/0/eq_enabled", &p.eqEnabled },
        { "/buses/0/comp_enabled", &p.compEnabled },
        { "/buses/0/comp_release_auto", &p.compReleaseAuto, true },
    });
}

TEST_CASE ("Appendix A: aux return defaults and load ranges", "[session][appendix]")
{
    Session session;
    auto& p = session.auxLane (0).params;
    checkParameters (session, {
        { "/aux_lanes/0/return_level_db", &p.returnLevelDb, 0, -100, 12 },
    }, { { "/aux_lanes/0/mute", &p.mute } });
}

TEST_CASE ("Appendix A: master defaults and load ranges", "[session][appendix]")
{
    Session session;
    auto& p = session.master();
    checkParameters (session, {
        { "/master/fader_db", &p.faderDb, 0, -100, 12 },
        { "/master/eq_lf_boost", &p.eqLfBoost, 0, 0, 10 },
        { "/master/eq_lf_atten", &p.eqLfAtten, 0, 0, 10 },
        { "/master/eq_lf_freq", &p.eqLfFreq, 60, 20, 100 },
        { "/master/eq_hf_boost", &p.eqHfBoost, 0, 0, 10 },
        { "/master/eq_hf_boost_freq", &p.eqHfBoostFreq, 8000, 3000, 16000 },
        { "/master/eq_hf_boost_bandwidth", &p.eqHfBoostBandwidth, 0.5f, 0, 10 },
        { "/master/eq_hf_atten", &p.eqHfAtten, 0, 0, 10 },
        { "/master/eq_hf_atten_freq", &p.eqHfAttenFreq, 10000, 5000, 20000 },
        { "/master/comp_thresh_db", &p.compThreshDb, 0, -60, 0 },
        { "/master/comp_ratio", &p.compRatio, 4, 1, 10 },
        { "/master/comp_attack_ms", &p.compAttackMs, 10, 0.1f, 50 },
        { "/master/comp_release_ms", &p.compReleaseMs, 100, 50, 1000 },
        { "/master/comp_makeup_db", &p.compMakeupDb, 0, -10, 20 },
    }, {
        { "/master/tape_enabled", &p.tapeEnabled },
        { "/master/eq_enabled", &p.eqEnabled },
        { "/master/comp_enabled", &p.compEnabled },
        { "/master/comp_release_auto", &p.compReleaseAuto, true },
        { "/master/mono_sum", &p.monoSum },
    });
    for (const auto& choice : std::vector<std::pair<const char*, std::vector<float>>> {
             { "eq_lf_freq", { 20, 30, 60, 100 } },
             { "eq_hf_boost_freq", { 3000, 4000, 5000, 8000, 10000, 12000, 16000 } },
             { "eq_hf_atten_freq", { 5000, 10000, 20000 } } })
        for (float value : choice.second)
        {
            CAPTURE (choice.first, value);
            auto document = Json::parse (SessionSerializer::serialize (session).toStdString());
            document["master"][choice.first] = value;
            load (session, document);
            const auto saved = Json::parse (SessionSerializer::serialize (session).toStdString());
            CHECK_THAT (saved["master"][choice.first].get<float>(), WithinAbs (value, 1e-6));
        }
}

TEST_CASE ("Appendix A: mastering defaults and load ranges", "[session][appendix]")
{
    Session session;
    auto& p = session.mastering();
    checkParameters (session, {
        { "/mastering/eq_band_0_freq", &p.eqBandFreq[0], 50, 20, 400 },
        { "/mastering/eq_band_1_freq", &p.eqBandFreq[1], 250, 60, 1500 },
        { "/mastering/eq_band_2_freq", &p.eqBandFreq[2], 1000, 200, 6000 },
        { "/mastering/eq_band_3_freq", &p.eqBandFreq[3], 4000, 800, 12000 },
        { "/mastering/eq_band_4_freq", &p.eqBandFreq[4], 12000, 2000, 20000 },
        { "/mastering/eq_band_0_gain_db", &p.eqBandGainDb[0], 0, -12, 12 },
        { "/mastering/eq_band_1_gain_db", &p.eqBandGainDb[1], 0, -12, 12 },
        { "/mastering/eq_band_2_gain_db", &p.eqBandGainDb[2], 0, -12, 12 },
        { "/mastering/eq_band_3_gain_db", &p.eqBandGainDb[3], 0, -12, 12 },
        { "/mastering/eq_band_4_gain_db", &p.eqBandGainDb[4], 0, -12, 12 },
        { "/mastering/eq_band_1_q", &p.eqBandQ[1], 1, 0.3f, 6 },
        { "/mastering/eq_band_2_q", &p.eqBandQ[2], 1, 0.3f, 6 },
        { "/mastering/eq_band_3_q", &p.eqBandQ[3], 1, 0.3f, 6 },
        { "/mastering/limiter_ceiling_db", &p.limiterCeilingDb, -0.3f, -12, 0 },
        { "/mastering/limiter_drive_db", &p.limiterDriveDb, 0, 0, 20 },
        { "/mastering/limiter_release_ms", &p.limiterReleaseMs, 100, 10, 1000 },
    }, {
        { "/mastering/eq_enabled", &p.eqEnabled },
        { "/mastering/comp_enabled", &p.compEnabled },
        { "/mastering/limiter_enabled", &p.limiterEnabled, true },
    });
}
