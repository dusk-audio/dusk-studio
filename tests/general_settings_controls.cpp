#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ui/imgui/GeneralSettingsControls.h"

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
    std::vector<int> savedAutosave;
    std::vector<float> previews;
    std::vector<float> savedScale;
    Controls controls;

    explicit SettingsHarness (bool tapeDefault = false, int autosaveSeconds = 30,
                              float scale = 1.0f)
        : controls (tapeDefault, autosaveSeconds, scale,
                    { [this] (bool value) { savedTape.push_back (value); },
                      [this] (int seconds) { savedAutosave.push_back (seconds); },
                      [this] (float value) { previews.push_back (value); },
                      [this] (float value) { savedScale.push_back (value); } }) {}

    bool untouched() const
    {
        return savedTape.empty() && savedAutosave.empty() && previews.empty()
            && savedScale.empty();
    }
};
}

TEST_CASE ("General settings describe their actual initial values", "[ui][accessibility]")
{
    SettingsHarness settings (true, 120, 1.25f);
    const auto controls = settings.controls.controls();

    REQUIRE (controls[0].id == Controls::Id::tapeDefault);
    REQUIRE (controls[0].role == Controls::Role::checkBox);
    REQUIRE (std::string (controls[0].name) == "Expand tape strip by default");
    REQUIRE (std::string (controls[0].help) == Controls::kTapeDefaultHelp);
    REQUIRE (controls[0].checked);
    REQUIRE (controls[0].valueText == "On");
    REQUIRE_FALSE (controls[0].focused);

    REQUIRE (controls[1].id == Controls::Id::autosave);
    REQUIRE (controls[1].role == Controls::Role::comboBox);
    REQUIRE (std::string (controls[1].name) == "Autosave every");
    REQUIRE (std::string (controls[1].help) == Controls::kAutosaveHelp);
    REQUIRE (controls[1].choiceCount == 5);
    const std::vector<std::string> choices (controls[1].choices,
                                            controls[1].choices + controls[1].choiceCount);
    REQUIRE ((choices == std::vector<std::string> { "15 seconds", "30 seconds", "1 minute",
                                                    "2 minutes", "5 minutes" }));
    REQUIRE (controls[1].selected == 3);
    REQUIRE (controls[1].valueText == "2 minutes");

    REQUIRE (controls[2].id == Controls::Id::uiScale);
    REQUIRE (controls[2].role == Controls::Role::slider);
    REQUIRE (std::string (controls[2].name) == "UI scale");
    REQUIRE_THAT (controls[2].numericValue, WithinAbs (1.25, 1e-6));
    REQUIRE_THAT (controls[2].minimum, WithinAbs (duskstudio::appconfig::kUiScaleMin, 1e-6));
    REQUIRE_THAT (controls[2].maximum, WithinAbs (duskstudio::appconfig::kUiScaleMax, 1e-6));
    REQUIRE_THAT (controls[2].step, WithinAbs (0.01, 1e-6));
    REQUIRE (controls[2].valueText == "1.25x");

    REQUIRE (settings.untouched());
}

TEST_CASE ("A stored autosave cadence snaps to the nearest offered choice", "[ui][accessibility]")
{
    int stored = 0;
    int expected = 0;
    SECTION ("below the shortest") { stored = 10; expected = 0; }
    SECTION ("exact") { stored = 60; expected = 2; }
    SECTION ("a tie takes the shorter") { stored = 45; expected = 1; }
    SECTION ("nearer the longer") { stored = 100; expected = 3; }
    SECTION ("above the longest") { stored = 600; expected = 4; }
    SECTION ("extreme values") { stored = std::numeric_limits<int>::min(); expected = 0; }

    SettingsHarness settings (false, stored);
    REQUIRE (settings.controls.autosaveChoice() == expected);
    REQUIRE (settings.untouched());
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
}

TEST_CASE ("Pointer and native autosave selections share settings state", "[ui][accessibility]")
{
    SettingsHarness settings;
    settings.controls.editAutosave (0);
    REQUIRE (settings.controls.autosaveChoice() == 0);
    REQUIRE (settings.savedAutosave == std::vector<int> { 15 });
    REQUIRE (settings.controls.request ({ Controls::Id::autosave,
                                         Controls::Action::select, 4.0 }));
    REQUIRE (settings.controls.autosaveChoice() == 4);
    REQUIRE ((settings.savedAutosave == std::vector<int> { 15, 300 }));
    REQUIRE (settings.controls.controls()[1].valueText == "5 minutes");

    settings.controls.editAutosave (4);
    REQUIRE (settings.controls.request ({ Controls::Id::autosave,
                                         Controls::Action::select, 4.0 }));
    settings.controls.editAutosave (-1);
    settings.controls.editAutosave (5);
    REQUIRE (settings.controls.autosaveChoice() == 4);
    REQUIRE (settings.savedAutosave.size() == 2);
    REQUIRE (settings.savedTape.empty());
    REQUIRE (settings.previews.empty());
    REQUIRE (settings.savedScale.empty());
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
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const Controls::Request rejected[] = {
        { Controls::Id::uiScale, Controls::Action::setValue, std::nullopt },
        { Controls::Id::uiScale, Controls::Action::setValue, nan },
        { Controls::Id::uiScale, Controls::Action::setValue, inf },
        { Controls::Id::uiScale, Controls::Action::setValue, -inf },
        { Controls::Id::uiScale, Controls::Action::click, std::nullopt },
        { Controls::Id::uiScale, Controls::Action::select, 1.0 },
        { Controls::Id::tapeDefault, Controls::Action::setValue, 1.25 },
        { Controls::Id::tapeDefault, Controls::Action::click, 1.0 },
        { Controls::Id::tapeDefault, Controls::Action::select, 1.0 },
        { Controls::Id::autosave, Controls::Action::select, std::nullopt },
        { Controls::Id::autosave, Controls::Action::select, nan },
        { Controls::Id::autosave, Controls::Action::select, inf },
        { Controls::Id::autosave, Controls::Action::select, 2.5 },
        { Controls::Id::autosave, Controls::Action::select, -1.0 },
        { Controls::Id::autosave, Controls::Action::select, 5.0 },
        { Controls::Id::autosave, Controls::Action::click, std::nullopt },
        { Controls::Id::autosave, Controls::Action::setValue, 60.0 },
        { Controls::Id::uiScale, Controls::Action::focus, 1.0 },
        { static_cast<Controls::Id> (999), Controls::Action::click, std::nullopt },
        { Controls::Id::uiScale, static_cast<Controls::Action> (999), std::nullopt }
    };
    for (const auto& request : rejected)
        REQUIRE_FALSE (settings.controls.request (request));
    REQUIRE_FALSE (settings.controls.tapeDefault());
    REQUIRE (settings.controls.autosaveChoice() == 1);
    REQUIRE_THAT (settings.controls.uiScale(), WithinAbs (1.0, 1e-6));
    REQUIRE (settings.untouched());
    REQUIRE_FALSE (settings.controls.takeFocusRequest());
}

TEST_CASE ("Native focus requests wait for the view to report actual focus", "[ui][accessibility]")
{
    SettingsHarness settings;
    REQUIRE (settings.controls.request ({ Controls::Id::autosave,
                                         Controls::Action::focus, std::nullopt }));
    REQUIRE_FALSE (settings.controls.controls()[1].focused);
    REQUIRE (settings.controls.takeFocusRequest() == Controls::Id::autosave);
    REQUIRE_FALSE (settings.controls.takeFocusRequest());
    settings.controls.reportFocus (Controls::Id::autosave);
    const auto focused = settings.controls.controls();
    REQUIRE_FALSE (focused[0].focused);
    REQUIRE (focused[1].focused);
    REQUIRE_FALSE (focused[2].focused);
    settings.controls.reportFocus (std::nullopt);
    REQUIRE_FALSE (settings.controls.controls()[1].focused);
    REQUIRE (settings.untouched());
}
