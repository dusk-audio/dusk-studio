#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "../src/ui/imgui/GeneralSettingsControls.h"

#include <limits>
#include <optional>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;
using Controls = duskstudio::imgui::GeneralSettingsControls;

namespace
{
struct SettingsHarness
{
    std::vector<bool> savedTape;
    std::vector<float> previews;
    std::vector<float> savedScale;
    Controls controls;

    SettingsHarness (bool tapeDefault = false, float scale = 1.0f)
        : controls (tapeDefault, scale,
                    { [this] (bool value) { savedTape.push_back (value); },
                      [this] (float value) { previews.push_back (value); },
                      [this] (float value) { savedScale.push_back (value); } }) {}
};
}

TEST_CASE ("General settings describe their actual initial values", "[ui][accessibility]")
{
    SettingsHarness settings (true, 1.25f);
    const auto controls = settings.controls.controls();
    REQUIRE (controls[0].id == Controls::Id::tapeDefault);
    REQUIRE (controls[0].role == Controls::Role::checkBox);
    REQUIRE (std::string (controls[0].name) == "Expand tape strip by default");
    REQUIRE (controls[0].checked);
    REQUIRE (controls[0].valueText == "On");
    REQUIRE_FALSE (controls[0].focused);
    REQUIRE (controls[1].id == Controls::Id::uiScale);
    REQUIRE (controls[1].role == Controls::Role::slider);
    REQUIRE (std::string (controls[1].name) == "UI scale");
    REQUIRE_THAT (controls[1].numericValue, WithinAbs (1.25, 1e-6));
    REQUIRE_THAT (controls[1].minimum,
                  WithinAbs (duskstudio::appconfig::kUiScaleMin, 1e-6));
    REQUIRE_THAT (controls[1].maximum,
                  WithinAbs (duskstudio::appconfig::kUiScaleMax, 1e-6));
    REQUIRE_THAT (controls[1].step, WithinAbs (0.01, 1e-6));
    REQUIRE (controls[1].valueText == "1.25x");
    REQUIRE (settings.savedTape.empty());
    REQUIRE (settings.previews.empty());
    REQUIRE (settings.savedScale.empty());
}

TEST_CASE ("Pointer and native checkbox edits share settings state", "[ui][accessibility]")
{
    SettingsHarness settings;
    settings.controls.editTapeDefault (true);
    REQUIRE (settings.controls.tapeDefault());
    REQUIRE (settings.savedTape == std::vector<bool> { true });
    REQUIRE (settings.controls.request ({ Controls::Id::tapeDefault,
                                         Controls::Action::click, std::nullopt }));
    REQUIRE_FALSE (settings.controls.tapeDefault());
    REQUIRE ((settings.savedTape == std::vector<bool> { true, false }));
    settings.controls.editTapeDefault (false);
    REQUIRE (settings.savedTape.size() == 2);
    REQUIRE_FALSE (settings.controls.controls()[0].checked);
    REQUIRE (settings.controls.controls()[0].id == Controls::Id::tapeDefault);
}

TEST_CASE ("Scale preview counts changed frames and release saves latest value", "[ui][accessibility]")
{
    SettingsHarness settings;
    settings.controls.editUiScale (1.1f, true, false);
    settings.controls.editUiScale (1.1f, false, false);
    settings.controls.editUiScale (1.2f, true, false);
    REQUIRE (settings.previews.empty());
    settings.controls.editUiScale (1.3f, true, false);
    REQUIRE (settings.previews.size() == 1);
    REQUIRE_THAT (settings.previews[0], WithinAbs (1.3, 1e-6));
    REQUIRE (settings.savedScale.empty());
    settings.controls.editUiScale (1.4f, true, false);
    REQUIRE (settings.previews.size() == 1);
    settings.controls.editUiScale (1.4f, false, true);
    REQUIRE (settings.previews.size() == 2);
    REQUIRE_THAT (settings.previews[1], WithinAbs (1.4, 1e-6));
    REQUIRE (settings.savedScale.size() == 1);
    REQUIRE_THAT (settings.savedScale[0], WithinAbs (1.4, 1e-6));
    settings.controls.editUiScale (1.4f, false, false);
    REQUIRE (settings.previews.size() == 2);
    REQUIRE (settings.savedScale.size() == 1);
}

TEST_CASE ("A native scale edit previews and saves once through shared state", "[ui][accessibility]")
{
    SettingsHarness settings;
    settings.controls.editUiScale (1.1f, true, false);
    REQUIRE (settings.controls.request ({ Controls::Id::uiScale,
                                         Controls::Action::setValue, 1.25 }));
    REQUIRE_THAT (settings.controls.uiScale(), WithinAbs (1.25, 1e-6));
    REQUIRE (settings.previews.size() == 1);
    REQUIRE_THAT (settings.previews[0], WithinAbs (1.25, 1e-6));
    REQUIRE (settings.savedScale.size() == 1);
    REQUIRE_THAT (settings.savedScale[0], WithinAbs (1.25, 1e-6));
    settings.controls.editUiScale (settings.controls.uiScale(), false, false);
    settings.controls.editUiScale (1.3f, true, false);
    settings.controls.editUiScale (1.4f, true, false);
    REQUIRE (settings.previews.size() == 1);
    REQUIRE (settings.savedScale.size() == 1);
    settings.controls.editUiScale (1.5f, true, false);
    REQUIRE (settings.previews.size() == 2);
    REQUIRE_THAT (settings.previews[1], WithinAbs (1.5, 1e-6));
    REQUIRE (settings.controls.controls()[1].id == Controls::Id::uiScale);
}

TEST_CASE ("Native scale values clamp to existing application limits", "[ui][accessibility]")
{
    SettingsHarness settings;
    double requested = -100.0;
    float expected = duskstudio::appconfig::kUiScaleMin;
    SECTION ("lower bound") {}
    SECTION ("upper bound")
    {
        requested = 100.0;
        expected = duskstudio::appconfig::kUiScaleMax;
    }
    SECTION ("fractional input retains text-entry precision")
    {
        requested = 1.234;
        expected = 1.234f;
    }
    REQUIRE (settings.controls.request ({ Controls::Id::uiScale,
                                         Controls::Action::setValue, requested }));
    REQUIRE_THAT (settings.controls.uiScale(), WithinAbs (expected, 1e-6));
    REQUIRE (settings.savedScale.size() == 1);
    REQUIRE_THAT (settings.savedScale[0], WithinAbs (expected, 1e-6));
}

TEST_CASE ("Malformed native settings requests have no effects", "[ui][accessibility]")
{
    SettingsHarness settings;
    const Controls::Request rejected[] = {
        { Controls::Id::uiScale, Controls::Action::setValue, std::nullopt },
        { Controls::Id::uiScale, Controls::Action::setValue,
          std::numeric_limits<double>::quiet_NaN() },
        { Controls::Id::uiScale, Controls::Action::setValue,
          std::numeric_limits<double>::infinity() },
        { Controls::Id::uiScale, Controls::Action::setValue,
          -std::numeric_limits<double>::infinity() },
        { Controls::Id::uiScale, Controls::Action::click, std::nullopt },
        { Controls::Id::tapeDefault, Controls::Action::setValue, 1.25 },
        { Controls::Id::tapeDefault, Controls::Action::click, 1.0 },
        { Controls::Id::uiScale, Controls::Action::focus, 1.0 },
        { static_cast<Controls::Id> (999), Controls::Action::click, std::nullopt },
        { Controls::Id::uiScale, static_cast<Controls::Action> (999), std::nullopt }
    };
    for (const auto& request : rejected)
        REQUIRE_FALSE (settings.controls.request (request));
    REQUIRE_FALSE (settings.controls.tapeDefault());
    REQUIRE_THAT (settings.controls.uiScale(), WithinAbs (1.0, 1e-6));
    REQUIRE (settings.savedTape.empty());
    REQUIRE (settings.previews.empty());
    REQUIRE (settings.savedScale.empty());
    REQUIRE_FALSE (settings.controls.takeFocusRequest());
}

TEST_CASE ("Native focus requests wait for the view to report actual focus", "[ui][accessibility]")
{
    SettingsHarness settings;
    REQUIRE (settings.controls.request ({ Controls::Id::tapeDefault,
                                         Controls::Action::focus, std::nullopt }));
    REQUIRE_FALSE (settings.controls.controls()[0].focused);
    REQUIRE (settings.controls.takeFocusRequest() == Controls::Id::tapeDefault);
    REQUIRE_FALSE (settings.controls.takeFocusRequest());
    settings.controls.reportFocus (Controls::Id::tapeDefault);
    REQUIRE (settings.controls.controls()[0].focused);
    REQUIRE_FALSE (settings.controls.controls()[1].focused);
    settings.controls.reportFocus (std::nullopt);
    REQUIRE_FALSE (settings.controls.controls()[0].focused);
    REQUIRE (settings.savedTape.empty());
    REQUIRE (settings.previews.empty());
    REQUIRE (settings.savedScale.empty());
}
