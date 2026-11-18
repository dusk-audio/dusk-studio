#pragma once

// Included from CompModeMap.h only. The channel compressor editor's map onto
// the strip's old named compressor fields, which the DSP no longer reads and a
// session no longer saves. Nothing but that editor calls it, and it goes when
// the editor moves onto the strip's CompParams.

namespace duskstudio
{
namespace comp
{
constexpr float kOptoGainUnityPct = 50.0f;
constexpr float kOptoGainPctPerDb =  1.25f;
constexpr float kOptoMakeupMaxDb  = 40.0f;
constexpr float kOutMakeupMaxDb   = 20.0f;

constexpr float kOptoPeakRedPctPerDb = 100.0f / 60.0f;
constexpr float kOptoThreshMinDb = -60.0f, kOptoThreshMaxDb =  0.0f;
constexpr float kFetThreshMinDb  = -60.0f, kFetThreshMaxDb  =  0.0f;
constexpr float kVcaThreshMinDb  = -38.0f, kVcaThreshMaxDb  = 12.0f;

constexpr float kFetAttackMinMs   =  0.02f, kFetAttackMaxMs   =   80.0f;
constexpr float kVcaAttackMinMs   =  0.1f,  kVcaAttackMaxMs   =   50.0f;
constexpr float kFetReleaseMinMs  = 50.0f,  kFetReleaseMaxMs  = 1100.0f;
constexpr float kVcaReleaseMinMs  = 10.0f,  kVcaReleaseMaxMs  = 5000.0f;
constexpr float kVcaRatioMin      =  1.0f,  kVcaRatioMax      =  120.0f;
constexpr int   kFetRatioMaxIndex = 4;
constexpr float kFetRatioKnobMin  =  1.0f,  kFetRatioKnobMax  =   20.0f;

constexpr float kFetAttackDefaultMs    =   0.2f;
constexpr float kFetReleaseDefaultMs   = 400.0f;
constexpr float kVcaRatioDefault       =   4.0f;
constexpr float kVcaAttackDefaultMs    =   1.0f;
constexpr float kVcaReleaseDefaultMs   = 100.0f;

namespace legacy
{
inline float makeupDbToOptoGainPct (float db) noexcept
{
    return std::clamp (kOptoGainUnityPct + db * kOptoGainPctPerDb, 0.0f, 100.0f);
}

inline float optoGainPctToMakeupDb (float pct) noexcept
{
    return (std::clamp (pct, 0.0f, 100.0f) - kOptoGainUnityPct) / kOptoGainPctPerDb;
}

inline float thresholdDbToOptoPeakRedPct (float db) noexcept
{
    return std::clamp (-db * kOptoPeakRedPctPerDb, 0.0f, 100.0f);
}

inline float optoPeakRedPctToThresholdDb (float pct) noexcept
{
    return -std::clamp (pct, 0.0f, 100.0f) / kOptoPeakRedPctPerDb;
}

inline Domain makeupDomain (int mode) noexcept
{
    const float span = mode == 0 ? kOptoMakeupMaxDb : kOutMakeupMaxDb;
    return { -span, span };
}

inline Domain thresholdDomain (int mode) noexcept
{
    switch (mode)
    {
        case 0:  return { kOptoThreshMinDb, kOptoThreshMaxDb };
        case 1:  return { kFetThreshMinDb,  kFetThreshMaxDb  };
        default: return { kVcaThreshMinDb,  kVcaThreshMaxDb  };
    }
}

inline void storeThresholdDb (ChannelStripParams& strip, int mode, float db) noexcept
{
    const auto domain = thresholdDomain (mode);
    const float v = std::clamp (db, domain.lo, domain.hi);
    switch (mode)
    {
        case 0:
            strip.compOptoPeakRed.store (thresholdDbToOptoPeakRedPct (v), std::memory_order_relaxed);
            break;
        case 1:  strip.compFetThresholdDb.store (v, std::memory_order_relaxed); break;
        default: strip.compVcaThreshDb.store (v, std::memory_order_relaxed); break;
    }
}
} // namespace legacy

inline Domain makeupDomainFor (const ChannelStripParams& strip) noexcept
{
    return legacy::makeupDomain (compModeOf (strip));
}

inline void applyTrackCompMakeupDb (ChannelStripParams& strip, float db) noexcept
{
    const float outDb = std::clamp (db, -kOutMakeupMaxDb, kOutMakeupMaxDb);
    switch (compModeOf (strip))
    {
        case 0:
            strip.compOptoGain.store (legacy::makeupDbToOptoGainPct (db), std::memory_order_relaxed);
            break;
        case 1:  strip.compFetOutput.store (outDb, std::memory_order_relaxed); break;
        default: strip.compVcaOutput.store (outDb, std::memory_order_relaxed); break;
    }
}

inline float trackCompMakeupDb (const ChannelStripParams& strip) noexcept
{
    switch (compModeOf (strip))
    {
        case 0:
            return legacy::optoGainPctToMakeupDb (strip.compOptoGain.load (std::memory_order_relaxed));
        case 1:
            return std::clamp (strip.compFetOutput.load (std::memory_order_relaxed),
                               -kOutMakeupMaxDb, kOutMakeupMaxDb);
        default:
            return std::clamp (strip.compVcaOutput.load (std::memory_order_relaxed),
                               -kOutMakeupMaxDb, kOutMakeupMaxDb);
    }
}

inline Domain thresholdDomainFor (const ChannelStripParams& strip) noexcept
{
    return legacy::thresholdDomain (compModeOf (strip));
}

inline void applyTrackCompThresholdDb (ChannelStripParams& strip, float db) noexcept
{
    legacy::storeThresholdDb (strip, compModeOf (strip), db);
}

inline float trackCompThresholdDb (const ChannelStripParams& strip) noexcept
{
    const int mode = compModeOf (strip);
    const auto domain = legacy::thresholdDomain (mode);
    switch (mode)
    {
        case 0:
            return std::clamp (legacy::optoPeakRedPctToThresholdDb (
                                   strip.compOptoPeakRed.load (std::memory_order_relaxed)),
                               domain.lo, domain.hi);
        case 1:
            return std::clamp (strip.compFetThresholdDb.load (std::memory_order_relaxed),
                               domain.lo, domain.hi);
        default:
            return std::clamp (strip.compVcaThreshDb.load (std::memory_order_relaxed),
                               domain.lo, domain.hi);
    }
}

inline void resetTrackCompThreshold (ChannelStripParams& strip) noexcept
{
    const int mode = compModeOf (strip);
    legacy::storeThresholdDb (strip, mode, legacy::thresholdDomain (mode).hi);
}

inline Domain attackDomainFor (const ChannelStripParams& strip) noexcept
{
    switch (compModeOf (strip))
    {
        case 1:  return { kFetAttackMinMs, kFetAttackMaxMs };
        case 2:  return { kVcaAttackMinMs, kVcaAttackMaxMs };
        default: return { 0.0f, 0.0f };
    }
}

inline Domain releaseDomainFor (const ChannelStripParams& strip) noexcept
{
    switch (compModeOf (strip))
    {
        case 1:  return { kFetReleaseMinMs, kFetReleaseMaxMs };
        case 2:  return { kVcaReleaseMinMs, kVcaReleaseMaxMs };
        default: return { 0.0f, 0.0f };
    }
}

inline void applyTrackCompAttackMs (ChannelStripParams& strip, float ms) noexcept
{
    switch (compModeOf (strip))
    {
        case 1:
            strip.compFetAttack.store (std::clamp (ms, kFetAttackMinMs, kFetAttackMaxMs),
                                       std::memory_order_relaxed);
            break;
        case 2:
            strip.compVcaAttack.store (std::clamp (ms, kVcaAttackMinMs, kVcaAttackMaxMs),
                                       std::memory_order_relaxed);
            break;
        default: break;
    }
}

inline float trackCompAttackMs (const ChannelStripParams& strip) noexcept
{
    switch (compModeOf (strip))
    {
        case 1:  return std::clamp (strip.compFetAttack.load (std::memory_order_relaxed),
                                    kFetAttackMinMs, kFetAttackMaxMs);
        case 2:  return std::clamp (strip.compVcaAttack.load (std::memory_order_relaxed),
                                    kVcaAttackMinMs, kVcaAttackMaxMs);
        default: return 0.0f;
    }
}

inline void applyTrackCompReleaseMs (ChannelStripParams& strip, float ms) noexcept
{
    switch (compModeOf (strip))
    {
        case 1:
            strip.compFetRelease.store (std::clamp (ms, kFetReleaseMinMs, kFetReleaseMaxMs),
                                        std::memory_order_relaxed);
            break;
        case 2:
            strip.compVcaRelease.store (std::clamp (ms, kVcaReleaseMinMs, kVcaReleaseMaxMs),
                                        std::memory_order_relaxed);
            break;
        default: break;
    }
}

inline float trackCompReleaseMs (const ChannelStripParams& strip) noexcept
{
    switch (compModeOf (strip))
    {
        case 1:  return std::clamp (strip.compFetRelease.load (std::memory_order_relaxed),
                                    kFetReleaseMinMs, kFetReleaseMaxMs);
        case 2:  return std::clamp (strip.compVcaRelease.load (std::memory_order_relaxed),
                                    kVcaReleaseMinMs, kVcaReleaseMaxMs);
        default: return 0.0f;
    }
}

inline Domain ratioKnobDomainFor (const ChannelStripParams& strip) noexcept
{
    switch (compModeOf (strip))
    {
        case 1:  return { kFetRatioKnobMin, kFetRatioKnobMax };
        case 2:  return { kVcaRatioMin,     kVcaRatioMax     };
        default: return { 0.0f, 0.0f };
    }
}
} // namespace comp
} // namespace duskstudio
