#pragma once

#include <array>

// Multi-Comp 2's own constants that the format 11 -> 12 migration needs, copied
// so the session layer builds without the donor. tests/comp_donor_guards.cpp
// holds each copy to the donor's value.
namespace duskstudio::legacycomp::mirror
{
struct Point { float x, y; };

// Mirrors core/MultiCompParams.hpp kOptoGainTaper, kOptoGainSilentKnobMax and
// kOptoGainMuteDb (Multi-Comp 2).
constexpr float kOptoGainSilentKnobMax = 5.416667f;
constexpr float kOptoGainMuteDb = -462.5f;
constexpr std::array<Point, 22> kOptoGainTaper {{
    { 5.416667f, -462.5f }, { 9.875000f, -22.3f }, { 14.333333f, -9.6f }, { 18.791667f, -4.1f },
    { 23.250000f, -0.1f },  { 27.708333f, 2.7f },  { 32.166667f, 5.6f },  { 36.625000f, 9.1f },
    { 41.083333f, 11.1f },  { 45.541667f, 12.7f }, { 50.000000f, 14.5f }, { 54.458333f, 16.6f },
    { 58.916667f, 18.4f },  { 63.375000f, 20.1f }, { 67.833333f, 24.8f }, { 72.291667f, 28.6f },
    { 76.750000f, 31.3f },  { 81.208333f, 34.0f }, { 85.666667f, 36.2f }, { 90.125000f, 37.3f },
    { 94.583333f, 37.7f },  { 99.041667f, 37.7f } }};

// Mirrors core/MultiCompModes.hpp kFetBlackVoice: inputRelativeDb,
// outputRelativeDb, inputTrimDb, outputTrimDb, reductionSlopes and
// attackScales (Multi-Comp 2).
constexpr std::array<float, 17> kFetInputRelativeDb {{
    -41.949972f, -40.349972f, -37.919972f, -35.199972f, -27.579973f, -25.519973f,
    -23.209974f, -17.159975f, -13.389977f, -9.519981f, -8.019983f, -6.429985f,
    -4.549989f, -0.419998f, -0.090000f, -0.030000f, 0.000000f }};
constexpr std::array<float, 17> kFetOutputRelativeDb {{
    -99.990001f, -91.940000f, -66.070001f, -56.780000f, -47.990000f, -39.920000f,
    -33.170000f, -27.200001f, -21.810001f, -16.860000f, -12.900000f, -9.240000f,
    -6.060000f, -3.500000f, -1.529999f, -0.150000f, 0.000000f }};
constexpr float kFetInputTrimDb  = 39.850671f;
constexpr float kFetOutputTrimDb = 4.360566f;
constexpr std::array<float, 4> kFetReductionSlopes { 0.82857823f, 0.90319133f, 0.93707197f, 0.96619657f };
constexpr std::array<float, 4> kFetAttackScales    { 1.0f, 0.65f, 0.45f, 0.25f };

// Mirrors core/MultiCompModes.hpp kFetDynamicsDefaults [38] fbAttackSeconds,
// [39] fbAttackSlope, [40] fbAttackCurve, [41] fbRatioExponent,
// [60] fbReleasePotFixed, [61] fbReleasePotStart, [62] fbReleasePotEnd
// (Multi-Comp 2, FET BLACK).
constexpr double kFbAttackSeconds  = 0.00010914402;
constexpr double kFbAttackSlope    = 1.1925094;
constexpr double kFbAttackCurve    = -1.5139126;
constexpr double kFbRatioExponent  = 1.3603765;
constexpr double kFbReleasePotFixed = 0.069821596;
constexpr double kFbReleasePotStart = 0.12;
constexpr double kFbReleasePotEnd   = 0.85;

// Mirrors core/MultiCompVcaLaw.hpp kCompressLaw (Multi-Comp 2).
constexpr std::array<Point, 22> kVcaCompressLaw {{
    { 0.0f, 1.000f },   { 5.0f, 1.255f },   { 10.0f, 1.518f },  { 15.0f, 1.790f },
    { 20.0f, 2.072f },  { 25.0f, 2.366f },  { 30.0f, 2.675f },  { 35.0f, 3.001f },
    { 40.0f, 3.349f },  { 45.0f, 3.725f },  { 50.0f, 4.137f },  { 55.0f, 4.597f },
    { 60.0f, 5.124f },  { 65.0f, 5.746f },  { 70.0f, 6.512f },  { 75.0f, 7.515f },
    { 80.0f, 8.939f },  { 85.0f, 11.243f }, { 90.0f, 15.932f }, { 95.0f, 32.424f },
    { 97.5f, 87.972f }, { 100.0f, -85.418f } }};

// Mirrors core/MultiCompVcaLaw.hpp kDetectorCalibration followed by
// kHotDetectorCalibration without its repeated 0 dB point (Multi-Comp 2).
constexpr std::array<Point, 23> kVcaDetectorCalibration {{
    { -56.0f, 0.0000f },  { -55.0f, 0.4237f },  { -54.0f, 0.1705f },  { -52.0f, -0.2395f },
    { -50.0f, -0.5287f }, { -48.0f, -0.7115f }, { -46.0f, -0.8128f }, { -44.0f, -0.8555f },
    { -42.0f, -0.8425f }, { -40.0f, -0.7701f }, { -36.0f, -0.6011f }, { -32.0f, -0.3873f },
    { -28.0f, -0.1744f }, { -24.0f, 0.0282f },  { -20.0f, 0.1538f },  { -16.0f, 0.2235f },
    { -14.0f, 0.2401f },  { 0.0f, 0.2401f },
    { 3.0f, 0.3871333165921654f },  { 6.0f, 0.6474051045335686f },  { 9.0f, 1.0665004629175474f },
    { 12.0f, 1.6975348429495067f }, { 15.0f, 2.602034570012348f } }};
} // namespace duskstudio::legacycomp::mirror
