#pragma once

#include "../AppConfig.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace duskstudio::imgui
{
class GeneralSettingsControls
{
public:
    static constexpr const char* kTapeDefaultName = "Expand tape strip by default";
    static constexpr const char* kTapeDefaultHelp =
        "When on, the TIMELINE tape strip starts expanded on every app launch. Saved "
        "per-machine; takes effect on next launch.";
    static constexpr const char* kAutosaveName = "Autosave every";
    static constexpr const char* kAutosaveHelp =
        "How often the session autosaves for crash recovery. Saved per-machine; applies "
        "when this panel closes.";
    static constexpr const char* kUiScaleName = "UI scale";

    static constexpr int kAutosaveChoiceCount = 5;
    static constexpr std::array<const char*, kAutosaveChoiceCount> kAutosaveChoices {
        "15 seconds", "30 seconds", "1 minute", "2 minutes", "5 minutes"
    };
    static constexpr std::array<int, kAutosaveChoiceCount> kAutosaveSeconds {
        15, 30, 60, 120, 300
    };

    enum class Id : std::uint32_t { tapeDefault = 2, autosave = 3, uiScale = 4 };
    enum class Role { checkBox, comboBox, slider };
    enum class Action { click, select, setValue, focus };

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
        const char* help;
        bool focused;
        bool checked;
        int selected;
        const char* const* choices;
        int choiceCount;
        double numericValue;
        double minimum;
        double maximum;
        double step;
        std::string valueText;
    };

    struct Callbacks
    {
        std::function<void (bool)> saveTapeDefault;
        std::function<void (int)> saveAutosaveSeconds;
        std::function<void (float)> previewScale;
        std::function<void (float)> saveScale;
    };

    GeneralSettingsControls (bool initialTapeDefault, int initialAutosaveSeconds,
                             float initialScale, Callbacks callbacks)
        : tapeDefaultValue (initialTapeDefault),
          autosaveValue (nearestAutosaveChoice (initialAutosaveSeconds)),
          scaleValue (initialScale), effects (std::move (callbacks)) {}

    bool tapeDefault() const noexcept { return tapeDefaultValue; }
    int autosaveChoice() const noexcept { return autosaveValue; }
    float uiScale() const noexcept { return scaleValue; }

    void editTapeDefault (bool value)
    {
        if (tapeDefaultValue == value)
            return;
        tapeDefaultValue = value;
        if (effects.saveTapeDefault)
            effects.saveTapeDefault (value);
    }

    void editAutosave (int choice)
    {
        if (choice < 0 || choice >= kAutosaveChoiceCount || choice == autosaveValue)
            return;
        autosaveValue = choice;
        if (effects.saveAutosaveSeconds)
            effects.saveAutosaveSeconds (kAutosaveSeconds[static_cast<std::size_t> (choice)]);
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
        if (wanted.control != Id::tapeDefault && wanted.control != Id::autosave
            && wanted.control != Id::uiScale)
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
            case Action::select:
                if (wanted.control != Id::autosave || ! wanted.value
                    || ! std::isfinite (*wanted.value)
                    || std::islessgreater (std::floor (*wanted.value), *wanted.value)
                    || *wanted.value < 0.0 || *wanted.value >= kAutosaveChoiceCount)
                    return false;
                editAutosave (static_cast<int> (*wanted.value));
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

    std::array<Control, 3> controls() const
    {
        char scaleText[32];
        std::snprintf (scaleText, sizeof scaleText, "%.2fx", static_cast<double> (scaleValue));
        return {{
            { Id::tapeDefault, Role::checkBox, kTapeDefaultName, kTapeDefaultHelp,
              focusedControl == Id::tapeDefault, tapeDefaultValue, -1, nullptr, 0,
              0.0, 0.0, 0.0, 0.0, tapeDefaultValue ? "On" : "Off" },
            { Id::autosave, Role::comboBox, kAutosaveName, kAutosaveHelp,
              focusedControl == Id::autosave, false, autosaveValue, kAutosaveChoices.data(),
              kAutosaveChoiceCount, 0.0, 0.0, 0.0, 0.0,
              kAutosaveChoices[static_cast<std::size_t> (autosaveValue)] },
            { Id::uiScale, Role::slider, kUiScaleName, "", focusedControl == Id::uiScale,
              false, -1, nullptr, 0, scaleValue, appconfig::kUiScaleMin,
              appconfig::kUiScaleMax, 0.01, scaleText }
        }};
    }

private:
    // A preview re-lays out the whole window behind the panel, so a drag previews at
    // about 20 Hz and the slider stays draggable through it.
    static constexpr int kChangedFramesPerPreview = 3;

    // A hand-edited config value snaps to the nearest offered cadence, the shorter one
    // on a tie, rather than leaving the dropdown blank.
    static int nearestAutosaveChoice (int seconds) noexcept
    {
        int best = 0;
        long long bestDistance = std::numeric_limits<long long>::max();
        for (int i = 0; i < kAutosaveChoiceCount; ++i)
        {
            const long long distance = std::llabs (
                static_cast<long long> (kAutosaveSeconds[static_cast<std::size_t> (i)]) - seconds);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = i;
            }
        }
        return best;
    }

    bool tapeDefaultValue;
    int autosaveValue;
    float scaleValue;
    int changedFrames = 0;
    Callbacks effects;
    std::optional<Id> focusedControl;
    std::optional<Id> focusRequest;
};
} // namespace duskstudio::imgui
