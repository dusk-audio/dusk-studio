#pragma once

#include "../AppConfig.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace duskstudio::imgui
{
class GeneralSettingsControls
{
public:
    static constexpr const char* kTapeDefaultName = "Expand tape strip by default";
    static constexpr const char* kUiScaleName = "UI scale";

    enum class Id : std::uint32_t { tapeDefault = 2, uiScale = 3 };
    enum class Role { checkBox, slider };
    enum class Action { click, setValue, focus };

    struct Request
    {
        Id control;
        Action action;
        std::optional<double> value;
    };

    struct Control
    {
        Id id;
        Role role;
        const char* name;
        bool focused;
        bool checked;
        double numericValue;
        double minimum;
        double maximum;
        double step;
        std::string valueText;
    };

    struct Callbacks
    {
        std::function<void (bool)> saveTapeDefault;
        std::function<void (float)> previewScale;
        std::function<void (float)> saveScale;
    };

    GeneralSettingsControls (bool initialTapeDefault, float initialScale, Callbacks callbacks)
        : tapeDefaultValue (initialTapeDefault), scaleValue (initialScale),
          effects (std::move (callbacks)) {}

    bool tapeDefault() const noexcept { return tapeDefaultValue; }
    float uiScale() const noexcept { return scaleValue; }

    void editTapeDefault (bool value)
    {
        if (tapeDefaultValue == value)
            return;
        tapeDefaultValue = value;
        if (effects.saveTapeDefault)
            effects.saveTapeDefault (value);
    }

    void editUiScale (float value, bool changed, bool released)
    {
        if (changed)
        {
            scaleValue = value;
            ++changedFrames;
        }
        if (! released && changedFrames < kChangedFramesPerPreview)
            return;
        if (! changed && ! released)
            return;

        changedFrames = 0;
        if (effects.previewScale)
            effects.previewScale (scaleValue);
        if (released && effects.saveScale)
            effects.saveScale (scaleValue);
    }

    bool request (const Request& wanted)
    {
        if (wanted.control != Id::tapeDefault && wanted.control != Id::uiScale)
            return false;
        switch (wanted.action)
        {
            case Action::focus:
                if (wanted.value)
                    return false;
                focusRequest = wanted.control;
                return true;
            case Action::click:
                if (wanted.control != Id::tapeDefault || wanted.value)
                    return false;
                editTapeDefault (! tapeDefaultValue);
                return true;
            case Action::setValue:
                if (wanted.control != Id::uiScale || ! wanted.value
                    || ! std::isfinite (*wanted.value))
                    return false;
                editUiScale (static_cast<float> (std::clamp (
                    *wanted.value, static_cast<double> (appconfig::kUiScaleMin),
                    static_cast<double> (appconfig::kUiScaleMax))), true, true);
                return true;
        }
        return false;
    }

    std::optional<Id> takeFocusRequest() noexcept
    {
        return std::exchange (focusRequest, std::nullopt);
    }

    void reportFocus (std::optional<Id> focused) noexcept { focusedControl = focused; }

    std::array<Control, 2> controls() const
    {
        char scaleText[32];
        std::snprintf (scaleText, sizeof scaleText, "%.2fx", static_cast<double> (scaleValue));
        return {{
            { Id::tapeDefault, Role::checkBox, kTapeDefaultName,
              focusedControl == Id::tapeDefault, tapeDefaultValue, 0.0, 0.0, 0.0, 0.0,
              tapeDefaultValue ? "On" : "Off" },
            { Id::uiScale, Role::slider, kUiScaleName, focusedControl == Id::uiScale, false,
              scaleValue, appconfig::kUiScaleMin, appconfig::kUiScaleMax, 0.01, scaleText }
        }};
    }

private:
    // A preview re-lays out the shell, so throttle it while the slider is dragged.
    static constexpr int kChangedFramesPerPreview = 3;

    bool tapeDefaultValue;
    float scaleValue;
    int changedFrames = 0;
    Callbacks effects;
    std::optional<Id> focusedControl;
    std::optional<Id> focusRequest;
};
} // namespace duskstudio::imgui
