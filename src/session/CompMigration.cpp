#include "CompMigration.h"
#include "CompMigrationTables.h"
#include "CompParams.h"
#include "../foundation/Json.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>

namespace duskstudio
{
namespace
{
namespace json = ::dusk::json;
using namespace legacycomp::mirror;

template <std::size_t N>
float interpolate (const std::array<Point, N>& table, float x) noexcept
{
    if (x <= table.front().x) return table.front().y;
    for (std::size_t i = 1; i < N; ++i)
        if (x <= table[i].x)
        {
            const float t = (x - table[i - 1].x) / (table[i].x - table[i - 1].x);
            return table[i - 1].y + t * (table[i].y - table[i - 1].y);
        }
    return table.back().y;
}

// Position 0..1 on a 17-point control law, k / 16, whose values rise with it.
float inverseControlLaw (const std::array<float, 17>& values, float target) noexcept
{
    if (! (target > values.front())) return 0.0f;
    for (std::size_t i = 1; i < values.size(); ++i)
        if (target <= values[i])
        {
            const float t = (target - values[i - 1]) / (values[i] - values[i - 1]);
            return ((float) (i - 1) + t) / 16.0f;
        }
    return 1.0f;
}

// The old core prepared every mode at twice the rate it then ran at, so each
// time constant it computed lasted twice as long as written
// (core/UniversalCompressorDSP.cpp kModePrepareOversampleFactor).
constexpr float kOldTimeScale = 2.0f;

// The reference's time to release half way after a 1 s tone at 4:1 with the
// Release knob at 0.5, which the release law is fitted to
// (core/MultiCompModes.hpp, the fbReleasePotFixed note).
constexpr double kFetHalfReleaseAtMidSeconds = 0.337;

// Where the reference VCA's onset sits for a THRESHOLD reading: the RMS of a
// sine 3.0103 dB under its peak, less the 0.29 dB the measured knee needs
// (core/MultiCompModes.hpp vcaThresholdDb).
constexpr float kVcaPeakToRmsDb = 3.0103f;
constexpr float kVcaOnsetTrimDb = 0.29f;

float clampOld (const nlohmann::json& obj, const char* key, float fallback, float lo, float hi)
{
    return std::clamp (json::getFiniteFloat (obj, key, fallback), lo, hi);
}

int oldIndex (const nlohmann::json& obj, const char* key, int fallback, int lo, int hi)
{
    return std::clamp (json::getInt (obj, key, fallback), lo, hi);
}

void put (nlohmann::json& comp, CompParamId id, float value)
{
    const auto& spec = compParamSpec (id);
    const float v = clampCompParam (id, value);
    if (spec.integer) comp[spec.symbol] = (int) v;
    else              comp[spec.symbol] = v;
}

// The old cores had no control for these; they ran at the plug-in's defaults.
void putFixedDefaults (nlohmann::json& comp, std::initializer_list<CompParamId> ids)
{
    for (const auto id : ids)
        put (comp, id, compParamSpec (id).hostDefault);
}

void migrateTrack (nlohmann::json& track)
{
    const auto found = track.find ("comp");
    const nlohmann::json old = found != track.end() && found->is_object() ? *found : nlohmann::json::object();

    nlohmann::json comp = nlohmann::json::object();
    for (const auto* key : { "enabled", "mode_picked", "mode" })
        if (json::has (old, key)) comp[key] = old[key];

    put (comp, CompParamId::OptoPeakReduction,
         legacycomp::optoPeakReduction (clampOld (old, "opto_peak_red", 0.0f, 0.0f, 100.0f)));
    put (comp, CompParamId::OptoGain, legacycomp::optoGain (clampOld (old, "opto_gain", 50.0f, 0.0f, 100.0f)));
    put (comp, CompParamId::OptoLimit, json::getBool (old, "opto_limit", false) ? 1.0f : 0.0f);

    const int fetRatio = oldIndex (old, "fet_ratio", 0, 0, 4);
    put (comp, CompParamId::FetInput, legacycomp::fetInput (clampOld (old, "fet_input", 0.0f, -20.0f, 40.0f)));
    put (comp, CompParamId::FetOutput, legacycomp::fetOutput (clampOld (old, "fet_output", 0.0f, -20.0f, 20.0f)));
    put (comp, CompParamId::FetAttack,
         legacycomp::fetAttack (clampOld (old, "fet_attack", 0.2f, 0.02f, 80.0f), fetRatio));
    put (comp, CompParamId::FetRelease,
         legacycomp::fetRelease (clampOld (old, "fet_release", 400.0f, 50.0f, 1100.0f)));
    put (comp, CompParamId::FetRatio, (float) fetRatio);
    put (comp, CompParamId::FetThreshold,
         legacycomp::fetThreshold (clampOld (old, "fet_threshold_db", -10.0f, -60.0f, 0.0f)));

    put (comp, CompParamId::VcaThreshold,
         legacycomp::vcaThreshold (clampOld (old, "vca_thresh_db", 12.0f, -38.0f, 12.0f)));
    put (comp, CompParamId::VcaCompression,
         legacycomp::vcaCompression (clampOld (old, "vca_ratio", 4.0f, 1.0f, 120.0f)));
    put (comp, CompParamId::VcaOutput, legacycomp::vcaOutput (clampOld (old, "vca_output", 0.0f, -20.0f, 20.0f)));

    // The old strip high-passed VCA's side chain at 60 Hz. The new VCA reads
    // this control as its PULL/SC switch, a tilt that also lifts the treble
    // into the detector (core/MultiCompVcaLaw.hpp kSidechainTiltMeasured), so
    // a flat side chain is the nearer match.
    putFixedDefaults (comp, { CompParamId::Mix, CompParamId::SidechainHp, CompParamId::AutoMakeup,
                              CompParamId::OptoMeter, CompParamId::FetMeter });
    track["comp"] = std::move (comp);
}

struct OldBusComp
{
    float thresholdDb, ratio, attackMs, releaseMs, makeupDb;
    bool  releaseAuto;
};

constexpr OldBusComp kOldBusDefaults { 0.0f, 4.0f, 10.0f, 100.0f, 0.0f, true };

constexpr const char* kOldBusKeys[] { "comp_enabled", "comp_thresh_db", "comp_ratio", "comp_attack_ms",
                                      "comp_release_ms", "comp_release_auto", "comp_makeup_db" };

// Buses fall back to the old defaults for a key they lack, as the old loader
// filled them; the master kept its live value, so its absent keys stay absent.
void migrateBusComp (nlohmann::json& stage, bool keepAbsentKeys)
{
    const auto present = [&] (const char* key) { return ! keepAbsentKeys || json::has (stage, key); };
    nlohmann::json comp = nlohmann::json::object();
    if (json::has (stage, "comp_enabled"))
        comp["enabled"] = json::getBool (stage, "comp_enabled", false);

    const auto& d = kOldBusDefaults;
    if (present ("comp_thresh_db"))
        put (comp, CompParamId::BusThreshold,
             legacycomp::busThreshold (clampOld (stage, "comp_thresh_db", d.thresholdDb, -60.0f, 0.0f)));
    if (present ("comp_ratio"))
        put (comp, CompParamId::BusRatio,
             (float) legacycomp::busRatio (clampOld (stage, "comp_ratio", d.ratio, 1.0f, 10.0f)));
    if (present ("comp_attack_ms"))
        put (comp, CompParamId::BusAttack,
             (float) legacycomp::busAttack (clampOld (stage, "comp_attack_ms", d.attackMs, 0.1f, 50.0f)));
    if (present ("comp_release_ms") || present ("comp_release_auto"))
        put (comp, CompParamId::BusRelease,
             (float) legacycomp::busRelease (clampOld (stage, "comp_release_ms", d.releaseMs, 50.0f, 1000.0f),
                                             json::getBool (stage, "comp_release_auto", d.releaseAuto)));
    if (present ("comp_makeup_db"))
        put (comp, CompParamId::BusMakeup,
             legacycomp::busMakeup (clampOld (stage, "comp_makeup_db", d.makeupDb, -10.0f, 20.0f)));

    putFixedDefaults (comp, { CompParamId::SidechainHp, CompParamId::AutoMakeup, CompParamId::BusMix,
                              CompParamId::BusHeadroom, CompParamId::BusFadeRate, CompParamId::BusFade });
    for (const auto* key : kOldBusKeys) stage.erase (key);
    stage["comp"] = std::move (comp);
}

// The mastering comp_* floats never reached the multiband compressor and its
// band settings were never saved, so it played at the plug-in's defaults.
void migrateMasteringComp (nlohmann::json& mastering)
{
    nlohmann::json comp = nlohmann::json::object();
    if (json::has (mastering, "comp_enabled"))
        comp["enabled"] = json::getBool (mastering, "comp_enabled", false);
    for (const auto id : compStageParams (CompStage::Multiband))
        put (comp, id, compParamSpec (id).hostDefault);
    for (const auto* key : kOldBusKeys) mastering.erase (key);
    mastering["comp"] = std::move (comp);
}
} // namespace

namespace legacycomp
{
// Old Opto: (knob - 50) * 0.8 dB of makeup (core/UniversalCompressorDSP.cpp,
// the Opto parameter switch). The new knob runs through the measured taper, so
// the same decibels land on its inverse: exact from -40 to its +37.7 dB top.
float optoGain (float gainPct) noexcept
{
    const float db = std::clamp ((std::clamp (gainPct, 0.0f, 100.0f) - 50.0f) * 0.8f, -40.0f, 40.0f);
    const float clamped = std::clamp (db, kOptoGainMuteDb, kOptoGainTaper.back().y);
    if (clamped <= kOptoGainMuteDb) return 0.0f;
    for (std::size_t i = 1; i < kOptoGainTaper.size(); ++i)
    {
        if (clamped > kOptoGainTaper[i].y) continue;
        const auto& lo = kOptoGainTaper[i - 1];
        const auto& hi = kOptoGainTaper[i];
        if (hi.y - lo.y <= 0.0f) return hi.x;
        return lo.x + (clamped - lo.y) / (hi.y - lo.y) * (hi.x - lo.x);
    }
    return 100.0f;
}

// The old cell's side-chain gain was the cube of the knob (core/OptoCompressor.hpp,
// stage 5) and the new one is a measured table into a different cell model
// (core/MultiCompOptoCell.hpp sideChainGain); no reading of one matches the
// other, so the knob keeps its place.
float optoPeakReduction (float peakReductionPct) noexcept
{
    return std::clamp (peakReductionPct, 0.0f, 100.0f);
}

// The old Input was plain decibels into the detector (core/FETCompressor.hpp);
// the new value sets a measured pot whose gain is its trim plus the pot law
// (core/MultiCompModes.hpp fetInputGainDb). The position playing the old gain
// keeps the drive into the threshold, exact from -2.1 to +39.85 dB.
float fetInput (float inputDb) noexcept
{
    const float position = inverseControlLaw (kFetInputRelativeDb, inputDb - kFetInputTrimDb);
    return -20.0f + 60.0f * position;
}

// Same for the Output pot (core/MultiCompModes.hpp fetOutputGainDb), exact up
// to its +4.36 dB top.
float fetOutput (float outputDb) noexcept
{
    const float position = inverseControlLaw (kFetOutputRelativeDb, outputDb - kFetOutputTrimDb);
    return -20.0f + 40.0f * position;
}

// The old attack was a one-pole on the gain, its time the written ms, doubled
// for All (core/FETCompressor.hpp process). The new feedback loop charges at
// overshoot / attack with attack = fbAttackSeconds * 10^(curve u^2 - slope u),
// u = position - 0.5, times the ratio's attack scale; near its settled point
// that is a one-pole of 20 / ln 10 * reductionSlope * attack seconds, which
// at the slow stop gives the 1.2-1.5 ms the reference shows (the turn-on note
// in core/MultiCompModes.hpp). All keeps the feed-forward arm there, so it is
// placed on the 4:1 law. A time slower than the law reaches takes the slow
// stop, where the turn-on law starts slowest.
float fetAttack (float attackMs, int ratioIndex) noexcept
{
    float attackSeconds = std::max (1.0e-4f, attackMs * 0.001f);
    if (ratioIndex >= 4) attackSeconds = std::max (2.0e-4f, attackSeconds * 2.0f);
    const double tau = (double) (attackSeconds * kOldTimeScale);

    const auto r = (std::size_t) (ratioIndex >= 4 ? 0 : std::clamp (ratioIndex, 0, 3));
    const double perAttackSecond = 20.0 / std::log (10.0) * (double) kFetReductionSlopes[r]
                                 * std::pow ((double) kFetAttackScales[r], kFbRatioExponent);
    const double x = std::log10 (tau / perAttackSecond / kFbAttackSeconds);
    const double c = -kFbAttackCurve;
    const double s = kFbAttackSlope;
    if (x >= s * s / (4.0 * c)) return 0.0f;
    if (x <= -0.25 * c - 0.5 * s) return 1.0f;
    const double u = (-s + std::sqrt (s * s - 4.0 * c * x)) / (2.0 * c);
    return (float) std::clamp (0.5 + u, 0.0, 1.0);
}

// The old release was a one-pole on the gain of 0.05 * 22^(ms / 1100) s, the
// longest time at the largest value (core/FETCompressor.hpp). The new value
// runs the other way: (value - 50) / 1050 is the Release position, 1 the
// fast stop, and the release time follows the pot law fixed + travel. The two
// meet on the time to release half way from 14 dB, the old one-pole's
// ln((1 - g14) / (1 - g7)) of its time constant against the reference's
// measured 337 ms at position 0.5.
float fetRelease (float releaseMs) noexcept
{
    const double tau = 0.05 * std::pow (22.0, (double) std::clamp (releaseMs / 1100.0f, 0.0f, 1.0f))
                     * (double) kOldTimeScale;
    const double g14 = std::pow (10.0, -14.0 / 20.0);
    const double g7  = std::pow (10.0, -7.0 / 20.0);
    const double halfRelease = tau * std::log ((1.0 - g14) / (1.0 - g7));

    const double span = kFbReleasePotEnd - kFbReleasePotStart;
    const double travelAtMid = (kFbReleasePotEnd - 0.5) / span;
    const double travel = halfRelease / kFetHalfReleaseAtMidSeconds * (kFbReleasePotFixed + travelAtMid)
                        - kFbReleasePotFixed;
    const double position = std::clamp (kFbReleasePotEnd - travel * span, 0.0, 1.0);
    return (float) (50.0 + 1050.0 * position);
}

// Both cores treat -10 dB as the unit's own setting: the old one hard-wired it
// before the control existed, the new one measured its law there
// (core/MultiCompModes.hpp fetReferenceReductionDb). The law around it is a
// different shape, so the setting keeps its value.
float fetThreshold (float thresholdDb) noexcept
{
    return std::clamp (thresholdDb, -60.0f, 0.0f);
}

// The old VCA compressed once its true RMS passed the threshold
// (core/VCACompressor.hpp). The new one reads THRESHOLD as a sine's peak and
// adds a measured correction to its RMS detector before comparing; the value
// that starts it at the old RMS level is exact down the whole new range.
// Old settings above about -3 dB have no counterpart and take the 0 dB top.
float vcaThreshold (float thresholdDb) noexcept
{
    const float correction = interpolate (kVcaDetectorCalibration, thresholdDb + kVcaPeakToRmsDb);
    return std::clamp (thresholdDb + kVcaPeakToRmsDb - kVcaOnsetTrimDb + correction, -55.0f, 0.0f);
}

// The new COMPRESSION knob applies a measured ratio, interpolated in 1 / ratio
// (core/MultiCompVcaLaw.hpp compressPosition), which reaches every old ratio.
float vcaCompression (float ratio) noexcept
{
    if (! (ratio > 1.0f)) return 0.0f;
    const float slope = 1.0f / ratio;
    for (std::size_t i = 1; i < kVcaCompressLaw.size(); ++i)
    {
        const float sa = 1.0f / kVcaCompressLaw[i - 1].y;
        const float sb = 1.0f / kVcaCompressLaw[i].y;
        if (slope <= sa && slope >= sb)
        {
            const float t = sa - sb > 0.0f ? (sa - slope) / (sa - sb) : 0.0f;
            return kVcaCompressLaw[i - 1].x + (kVcaCompressLaw[i].x - kVcaCompressLaw[i - 1].x) * t;
        }
    }
    return 100.0f;
}

float vcaOutput (float outputDb) noexcept
{
    return std::clamp (outputDb, -20.0f, 20.0f);
}

// The new threshold is the reference console's measured knob law
// (core/MultiCompBusLaw.hpp drive), not decibels at the detector, so the
// setting keeps its value within the knob's range.
float busThreshold (float thresholdDb) noexcept
{
    return std::clamp (thresholdDb, -30.0f, 15.0f);
}

// The stepped indices the old bus and master strips played (BusStrip and
// MasterBus updateCompParameters).
int busRatio (float ratio) noexcept
{
    return ratio < 3.0f ? 0 : (ratio < 7.0f ? 1 : 2);
}

int busAttack (float attackMs) noexcept
{
    return attackMs < 0.2f ? 0
         : attackMs < 0.65f ? 1
         : attackMs < 2.0f ? 2
         : attackMs < 6.5f ? 3
         : attackMs < 20.0f ? 4 : 5;
}

int busRelease (float releaseMs, bool autoRelease) noexcept
{
    if (autoRelease) return 4;
    return releaseMs < 200.0f ? 0 : (releaseMs < 450.0f ? 1 : (releaseMs < 900.0f ? 2 : 3));
}

float busMakeup (float makeupDb) noexcept
{
    return std::clamp (makeupDb, 0.0f, 20.0f);
}
} // namespace legacycomp

void migrateCompressorsToFormat12 (nlohmann::json& root)
{
    if (! root.is_object()) return;

    if (const auto tracks = root.find ("tracks"); tracks != root.end() && tracks->is_array())
        for (auto& track : *tracks)
            if (track.is_object()) migrateTrack (track);

    if (const auto buses = root.find ("buses"); buses != root.end() && buses->is_array())
        for (auto& bus : *buses)
            if (bus.is_object()) migrateBusComp (bus, false);

    if (const auto master = root.find ("master"); master != root.end() && master->is_object())
        migrateBusComp (*master, true);

    if (const auto mastering = root.find ("mastering"); mastering != root.end() && mastering->is_object())
        migrateMasteringComp (*mastering);
}
} // namespace duskstudio
