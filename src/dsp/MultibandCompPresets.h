#pragma once

namespace duskstudio
{
class CompParams;
class MultiCompStage;
}

namespace duskstudio::mbpresets
{
// Three-band mastering compressor presets transcribed from the Tascam
// DP-24/32/SD compression pre-set chart, in the chart's plain units. The
// mastering chain's Multi-Comp 2 has four bands (low / lowmid / highmid /
// high); a preset plays Low on low, Mid on lowmid and High on high, and
// disables highmid, which Multi-Comp 2 merges into its neighbours, so the
// split between lowmid and high is crossover 3. The chart's Knee column has
// no equivalent in Multi-Comp 2's multiband mode and is dropped; Auto Makeup
// is Off for every preset.
struct Band
{
    float thresholdDb;   // mb_threshold_<band>  [-60, 0]
    float ratio;         // mb_ratio_<band>      [1, 20]
    float makeupDb;      // mb_makeup_<band>     [-12, +12]  (chart "Gain")
    float attackMs;      // mb_attack_<band>     [0.1, 100]
    float releaseMs;     // mb_release_<band>    [10, 1000]
};

struct Preset
{
    const char* name;
    Band  low;           // -> band 0, low
    Band  mid;           // -> band 1, lowmid
    Band  high;          // -> band 3, high
    float lowXoverHz;    // Low/Mid split   -> mb_crossover_1 [20, 500]
    float hiXoverHz;     // Mid/High split  -> mb_crossover_3 [2000, 16000]
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

// Writes preset `index` into the multiband values of `comp`, each plain number
// turned into Multi-Comp 2's host value by `stage` reading it as typed text, so
// the plug-in's tapers stay its own. Crossover 2, which the disabled highmid
// band leaves without a band edge, goes to the log midpoint of the other two,
// where Multi-Comp 2's 1.5x crossover spacing leaves the chart's splits alone.
// Bus-wide Mix and Output keep their values. False, and comp untouched, for an
// index out of range or a value the plug-in does not read. Message thread.
bool apply (int index, const MultiCompStage& stage, CompParams& comp);
} // namespace duskstudio::mbpresets
