#include "ReverbUnit.h"

#include "../../foundation/ScopedNoDenormals.h"

#include <DuskVerbEngine.h>

#include <algorithm>
#include <cstring>

namespace duskstudio::builtin
{
namespace
{
const ParamInfo kReverbParams[] =
{
    { "mix",       "Mix",       0.0f,    1.0f,     0.0f    },
    { "algorithm", "Algorithm", 0.0f,   15.0f,    10.0f    },
    { "decay",     "Decay",     0.2f,   30.0f,     2.0f    },
    { "size",      "Size",      0.0f,    1.0f,     0.5f    },
    { "predelay",  "Pre-Delay", 0.0f,  250.0f,    20.0f    },
    { "damping",   "Damping",   0.1f,    1.5f,     0.7f    },
    { "width",     "Width",     0.0f,    2.0f,     1.0f    },
    { "lo_cut",    "Lo Cut",    5.0f,  500.0f,    20.0f    },
    { "hi_cut",    "Hi Cut", 1000.0f, 20000.0f, 12000.0f   },
};

constexpr double kMixRampSeconds = 0.02;
} // namespace

ReverbUnit::ReverbUnit()
    : BuiltinUnit (kReverbParams, kNumParams),
      engine (std::make_unique<DuskVerbEngine>())
{
}

ReverbUnit::~ReverbUnit() = default;

void ReverbUnit::prepare (double sampleRate, int maxBlockFrames)
{
    maxFrames = std::max (0, maxBlockFrames);
    dryL.assign ((size_t) maxFrames, 0.0f);
    dryR.assign ((size_t) maxFrames, 0.0f);

    engine->prepare (sampleRate, maxFrames);

    mix.reset (sampleRate, kMixRampSeconds);
    mix.setCurrentAndTargetValue (getParam (kMix));

    // Force every cached value to miss so prepare() republishes the whole set
    // onto the freshly prepared engine.
    lastAlgorithm = -1;
    lastDecay = lastSize = lastPreDelay = -1.0f;
    lastDamping = lastWidth = lastLoCut = lastHiCut = -1.0f;
    pushChangedParams();
}

void ReverbUnit::pushChangedParams() noexcept
{
    const int algorithm = std::clamp ((int) (paramValue (kAlgorithm) + 0.5f), 0, 15);
    if (algorithm != lastAlgorithm)
    {
        lastAlgorithm = algorithm;
        engine->setAlgorithm (algorithm);
    }

    // The inequality pair rather than != so a NaN from a corrupt session
    // leaves the cached value and the engine alone.
    auto push = [] (float value, float& cached, auto&& apply)
    {
        if (value < cached || value > cached)
        {
            cached = value;
            apply (value);
        }
    };

    push (paramValue (kDecaySeconds), lastDecay,
          [this] (float v) { engine->setDecayTime (v); });
    push (paramValue (kSize), lastSize,
          [this] (float v) { engine->setSize (v); });
    push (paramValue (kPreDelayMs), lastPreDelay,
          [this] (float v) { engine->setPreDelay (v); });
    push (paramValue (kDamping), lastDamping,
          [this] (float v) { engine->setTrebleMultiply (v); engine->setAirTrebleMultiply (v); });
    push (paramValue (kWidth), lastWidth,
          [this] (float v) { engine->setWidth (v); });
    push (paramValue (kLoCutHz), lastLoCut,
          [this] (float v) { engine->setLoCut (v); });
    push (paramValue (kHiCutHz), lastHiCut,
          [this] (float v) { engine->setHiCut (v); });
}

void ReverbUnit::process (float* left, float* right, int numFrames,
                          const dusk::MidiBuffer*) noexcept
{
    dusk::audio::ScopedNoDenormals noDenormals;

    if (left == nullptr || right == nullptr || numFrames <= 0 || numFrames > maxFrames)
        return;

    pushChangedParams();
    mix.setTargetValue (paramValue (kMix));

    const auto bytes = sizeof (float) * (size_t) numFrames;
    std::memcpy (dryL.data(), left, bytes);
    std::memcpy (dryR.data(), right, bytes);

    engine->process (left, right, numFrames);

    for (int i = 0; i < numFrames; ++i)
    {
        const float m = mix.getNextValue();
        left[i]  = dryL[(size_t) i] + m * (left[i]  - dryL[(size_t) i]);
        right[i] = dryR[(size_t) i] + m * (right[i] - dryR[(size_t) i]);
    }
}
} // namespace duskstudio::builtin
