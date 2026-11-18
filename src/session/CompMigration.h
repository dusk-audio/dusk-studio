#pragma once

#include <nlohmann/json.hpp>

namespace duskstudio
{
// Format 11 stored the compressors in the old donor core's terms. These turn
// each old value into the Multi-Comp 2 host value that plays closest to it.
namespace legacycomp
{
float optoGain (float gainPct) noexcept;
float optoPeakReduction (float peakReductionPct) noexcept;
float fetInput (float inputDb) noexcept;
float fetOutput (float outputDb) noexcept;
float fetAttack (float attackMs, int ratioIndex) noexcept;
float fetRelease (float releaseMs) noexcept;
float fetThreshold (float thresholdDb) noexcept;
float vcaThreshold (float thresholdDb) noexcept;
float vcaCompression (float ratio) noexcept;
float vcaOutput (float outputDb) noexcept;
float busThreshold (float thresholdDb) noexcept;
int   busRatio (float ratio) noexcept;
int   busAttack (float attackMs) noexcept;
int   busRelease (float releaseMs, bool autoRelease) noexcept;
float busMakeup (float makeupDb) noexcept;
} // namespace legacycomp

// The 11 -> 12 step: every track's comp object and the flat comp_* keys of the
// buses, master and mastering become format 12's comp objects.
void migrateCompressorsToFormat12 (nlohmann::json& root);
} // namespace duskstudio
