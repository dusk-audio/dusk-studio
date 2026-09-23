#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/MasterTape.h"
#include "engine/builtin/DafPlugin.h"
#include "session/Session.h"

#include <cmath>
#include <string>

// The master tape is Tape Machine 2, and the session's TapeParams are its
// parameters: every control the plug-in's editor can move must land in a field
// the session saves, and every field must reach the plug-in.

using namespace duskstudio;
using Catch::Matchers::WithinAbs;

namespace
{
int indexOf (MasterTape& tape, const char* symbol)
{
    const auto& params = tape.plugin().params();
    for (std::size_t i = 0; i < params.size(); ++i)
        if (params[i].symbol == symbol) return (int) i;
    FAIL ("no parameter with symbol " << symbol);
    return -1;
}

// A value away from the default, on the parameter's own grid.
float probeValue (const builtin::DafParamDesc& d)
{
    if (! d.enumValues.empty())
        return d.enumValues.back() != d.defaultValue ? d.enumValues.back() : d.enumValues.front();
    const float v = d.minValue + 0.37f * (d.maxValue - d.minValue);
    return d.isInteger || d.isBoolean ? std::round (v) : v;
}
} // namespace

TEST_CASE ("every Tape Machine 2 control has a home in the session", "[tape][session]")
{
    MasterTape tape;
    MasterBusParams params;

    int engageParams = 0;
    const auto& descs = tape.plugin().params();
    for (std::size_t i = 0; i < descs.size(); ++i)
    {
        const auto& d = descs[i];
        INFO ("parameter " << d.symbol);
        if (d.isOutput || d.symbol == "oversampling")
            continue;

        if (tape.isEngageParam ((int) i))
        {
            ++engageParams;
            tape.setSessionValue (params, (int) i, 1.0f);
            REQUIRE_FALSE (params.tapeEnabled.load());
            REQUIRE_THAT (tape.sessionValue (params, (int) i), WithinAbs (1.0, 1e-6));
            tape.setSessionValue (params, (int) i, 0.0f);
            REQUIRE (params.tapeEnabled.load());
            continue;
        }

        const float v = probeValue (d);
        tape.setSessionValue (params, (int) i, v);
        REQUIRE_THAT (tape.sessionValue (params, (int) i), WithinAbs (v, 1e-4));
    }
    REQUIRE (engageParams == 1);
}

TEST_CASE ("the master tape pushes the session into the plug-in", "[tape][session]")
{
    MasterTape tape;
    MasterBusParams params;
    tape.prepare (48000.0, 256);

    params.tape.inputGainDb.store (5.0f);
    params.tape.headWidth.store (2);
    params.tape.reproHfDb.store (-3.0f);
    tape.pushParameters (params.tape);

    auto& plugin = tape.plugin();
    REQUIRE_THAT (plugin.getParameterValue ((std::uint32_t) indexOf (tape, "inputGain")), WithinAbs (5.0, 1e-6));
    REQUIRE_THAT (plugin.getParameterValue ((std::uint32_t) indexOf (tape, "headWidth")), WithinAbs (2.0, 1e-6));
    REQUIRE_THAT (plugin.getParameterValue ((std::uint32_t) indexOf (tape, "reproHF")),   WithinAbs (-3.0, 1e-6));
    REQUIRE_FALSE (tape.isPassthroughPath());

    params.tape.signalPath.store (3);   // Thru
    tape.pushParameters (params.tape);
    REQUIRE (tape.isPassthroughPath());
}
