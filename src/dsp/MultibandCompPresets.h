#pragma once

namespace duskstudio::mbpresets
{
// Three-band mastering compressor presets transcribed from the Tascam
// DP-24/32/SD compression pre-set chart. The donor multiband compressor has
// four bands (low / lowmid / highmid / high); these map Low->low, Mid->lowmid,
// High->high and disable the highmid band so the engine behaves as 3-band.
// The chart's Knee column has no equivalent in the donor's multiband mode and
// is dropped; AutoMake is Off for every multiband preset.
struct Band
{
    float thresholdDb;   // mb_<band>_threshold  [-60, 0]
    float ratio;         // mb_<band>_ratio      [1, 20]
    float makeupDb;      // mb_<band>_makeup     [-12, +12]  (chart "Gain")
    float attackMs;      // mb_<band>_attack     [0.1, 100]
    float releaseMs;     // mb_<band>_release    [10, 1000]
};

struct Preset
{
    const char* name;
    Band  low;           // -> donor "low"     band
    Band  mid;           // -> donor "lowmid"  band
    Band  high;          // -> donor "high"    band
    float lowXoverHz;    // Low/Mid split   -> mb_crossover_1
    float hiXoverHz;     // Mid/High split  -> mb_crossover_2/3 (highmid collapsed)
};

constexpr Preset kPresets[] =
{
    { "Basic CD",
      { -15.0f, 3.0f, 6.0f, 20.0f, 700.0f },
      { -11.0f, 3.0f, 5.0f, 20.0f, 500.0f },
      { -16.0f, 3.0f, 6.0f, 20.0f, 300.0f }, 315.0f, 3170.0f },
    { "Pop",
      { -11.0f, 4.0f, 6.0f,  2.0f, 500.0f },
      { -13.0f, 4.0f, 2.0f,  2.0f, 210.0f },
      { -13.0f, 4.0f, 8.0f, 10.0f, 100.0f }, 210.0f, 2670.0f },
    { "Pop Rock 1",
      { -11.0f, 4.0f, 6.0f, 12.0f, 700.0f },
      { -13.0f, 4.0f, 2.0f, 30.0f, 360.0f },
      { -13.0f, 4.0f, 7.0f, 20.0f, 200.0f }, 315.0f, 3170.0f },
    { "Pop Rock 2",
      { -11.0f, 6.0f, 5.0f, 12.0f, 610.0f },
      { -13.0f, 6.0f, 2.0f, 30.0f, 360.0f },
      { -15.0f, 6.0f, 7.0f, 12.0f, 280.0f }, 315.0f, 3170.0f },
    { "Rock 1",
      { -11.0f, 3.0f, 3.0f, 20.0f, 700.0f },
      { -13.0f, 2.0f, 2.0f, 30.0f, 500.0f },
      { -17.0f, 2.0f, 4.0f, 20.0f, 280.0f }, 315.0f, 3000.0f },
    { "Rock 2",
      { -11.0f, 4.0f, 3.0f, 18.0f, 610.0f },
      { -13.0f, 2.0f, 2.0f, 30.0f, 530.0f },
      { -17.0f, 2.0f, 8.0f, 18.0f, 280.0f }, 223.0f, 5990.0f },
    { "Classic",
      { -15.0f, 1.5f, 3.0f, 18.0f, 500.0f },
      { -13.0f, 1.5f, 5.0f, 22.0f, 300.0f },
      { -15.0f, 1.5f, 3.0f, 20.0f, 220.0f }, 125.0f, 7130.0f },
    { "Dance",
      { -16.0f, 4.0f, 5.0f, 24.0f, 440.0f },
      { -13.0f, 1.5f, 2.0f, 22.0f, 300.0f },
      { -14.0f, 4.0f, 8.0f, 52.0f, 300.0f }, 140.0f, 7130.0f },
    { "R&B Hip Hop",
      { -19.0f, 6.0f,  8.0f, 98.0f, 400.0f },
      { -13.0f, 2.0f,  1.0f, 24.0f, 300.0f },
      { -20.0f, 8.0f, 11.0f, 50.0f, 220.0f }, 132.0f, 5340.0f },
};

constexpr int kNumPresets = (int) (sizeof (kPresets) / sizeof (kPresets[0]));
} // namespace duskstudio::mbpresets
