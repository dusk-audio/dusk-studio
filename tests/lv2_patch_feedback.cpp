#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/hosting/InsertAdapter.h"
#include "engine/lv2/Lv2Bundle.h"
#include "engine/lv2/Lv2Instance.h"

#include <array>
#include <string>

namespace
{
constexpr const char* kPluginUri = "urn:duskstudio:test:many-patches";
constexpr int kPropertyCount = 160;
}

TEST_CASE ("LV2 initial patch synchronization retains more than 128 properties",
           "[lv2][params][integration][regression][issue-394]")
{
    duskstudio::lv2::Lv2Bundle bundle;
    std::string error;
    REQUIRE (bundle.load (DUSKSTUDIO_MANY_PATCH_LV2_FIXTURE_PATH, error));

    duskstudio::lv2::Lv2Instance instance;
    REQUIRE (instance.create (bundle, kPluginUri, error));
    REQUIRE (instance.activate (48000.0, 32, error));
    REQUIRE (instance.paramCount() == kPropertyCount);

    instance.requestPatchParameterValuesForUi();
    duskstudio::hosting::InsertAdapter adapter;
    adapter.prepare (instance.portLayout(), 32);
    std::array<float, 32> left {};
    std::array<float, 32> right {};
    adapter.process (instance, left.data(), right.data(), 32);
    instance.drainPatchFeedback();

    for (int i = 0; i < kPropertyCount; ++i)
    {
        const auto* parameter = instance.paramInfo (i);
        REQUIRE (parameter != nullptr);
        REQUIRE (parameter->isPatchProperty);
        REQUIRE (parameter->name.size() > 1);
        REQUIRE (parameter->name.front() == 'p');
        const auto expected = std::stod (parameter->name.substr (1));
        double value = -1.0;
        REQUIRE (instance.getParamValue (parameter->id, value));
        CHECK_THAT (value, Catch::Matchers::WithinAbs (expected, 1.0e-7));
    }
}
