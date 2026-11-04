#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ui/imgui/PanelText.h"
#include "ui/imgui/PanelTextItems.h"

// LogToBuffer: Dear ImGui logs the text it draws, after hiding a label's "##" part,
// which makes the drawn characters observable without a renderer.
#include <DearImGui/imgui_internal.h>

#include <array>
#include <cstddef>
#include <string>

using duskstudio::imgui::beginTextCombo;
using duskstudio::imgui::comboPreviewText;
using duskstudio::imgui::displayText;
using duskstudio::imgui::textSelectable;

namespace
{
class HeadlessUi
{
public:
    HeadlessUi()
    {
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext (context);
        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2 (640.0f, 480.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.Fonts->AddFontDefault();
        io.Fonts->Build();
        io.Fonts->SetTexID (static_cast<ImTextureID> (1));
    }

    ~HeadlessUi() { ImGui::DestroyContext (context); }

    // Runs one frame and returns the text Dear ImGui drew while `body` ran.
    template <typename Body>
    std::string frame (Body&& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos (ImVec2 (0.0f, 0.0f));
        ImGui::SetNextWindowSize (ImGui::GetIO().DisplaySize);
        ImGui::Begin ("##panel", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                                              | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        body();
        std::string drawn = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return drawn;
    }

    void movePointer (ImVec2 to) { ImGui::GetIO().AddMousePosEvent (to.x, to.y); }
    void pressPointer (bool down) { ImGui::GetIO().AddMouseButtonEvent (ImGuiMouseButton_Left, down); }

private:
    ImGuiContext* context = nullptr;
};

std::size_t occurrences (const std::string& text, const std::string& part)
{
    std::size_t count = 0;
    for (auto at = text.find (part); at != std::string::npos; at = text.find (part, at + 1)) ++count;
    return count;
}

ImVec2 centre (ImVec2 min, ImVec2 max) { return ImVec2 ((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f); }
} // namespace

TEST_CASE ("A text selectable draws a name containing ## in full", "[imgui][sfz]")
{
    HeadlessUi ui;
    const std::string names[] { "Kick ## Room", displayText ("Kick \xEF\xBC\x83\xEF\xBC\x83 Room"), "## Room",
                                "Kick ###Room", "Instruments/Kick ## Room.sfz" };
    for (const auto& name : names)
    {
        CAPTURE (name);
        const auto drawn = ui.frame ([&] { textSelectable ("##choice", name, false); });
        CHECK (occurrences (drawn, name) == 1);
    }
}

TEST_CASE ("A text selectable stays the last item and is picked across its row", "[imgui][sfz]")
{
    HeadlessUi ui;
    const std::string name = "Kick ## Room";
    ImVec2 min {}, max {};
    float textWidth = 0.0f;
    bool picked = false;
    const auto submit = [&]
    {
        picked = textSelectable ("##choice", name, false) || picked;
        min = ImGui::GetItemRectMin();
        max = ImGui::GetItemRectMax();
        textWidth = ImGui::CalcTextSize (name.c_str(), name.c_str() + name.size()).x;
    };
    ui.frame (submit);
    REQUIRE (max.x - min.x > 4.0f * textWidth);

    ui.movePointer (ImVec2 (max.x - 10.0f, (min.y + max.y) * 0.5f));
    ui.frame (submit);
    ui.pressPointer (true);
    ui.frame (submit);
    ui.pressPointer (false);
    ui.frame (submit);
    CHECK (picked);
}

TEST_CASE ("A text combo previews a name containing ## in full, closed and open", "[imgui][sfz]")
{
    HeadlessUi ui;
    const std::array<std::string, 2> names { "Kick ## Room", "Kick ## Hall" };
    std::size_t selected = 0;
    ImVec2 comboMin {}, comboMax {};
    std::array<ImVec2, 2> itemMin {}, itemMax {};
    bool open = false;
    const auto submit = [&]
    {
        ImGui::SetNextItemWidth (300.0f);
        open = beginTextCombo ("##program");
        if (open)
        {
            for (std::size_t i = 0; i < names.size(); ++i)
            {
                ImGui::PushID (static_cast<int> (i));
                if (textSelectable ("##choice", names[i], i == selected)) selected = i;
                itemMin[i] = ImGui::GetItemRectMin();
                itemMax[i] = ImGui::GetItemRectMax();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        comboPreviewText (names[selected]);
        comboMin = ImGui::GetItemRectMin();
        comboMax = ImGui::GetItemRectMax();
    };

    auto drawn = ui.frame (submit);
    CHECK (occurrences (drawn, names[0]) == 1);
    CHECK_THAT (comboMax.x - comboMin.x, Catch::Matchers::WithinAbs (300.0, 0.5));

    ui.movePointer (centre (comboMin, comboMax));
    ui.frame (submit);
    ui.pressPointer (true);
    ui.frame (submit);
    ui.pressPointer (false);
    ui.frame (submit);
    // A popup's first frame sizes it out of sight; its items have their places from the next.
    drawn = ui.frame (submit);
    REQUIRE (open);
    CHECK (occurrences (drawn, names[0]) == 2);
    CHECK (occurrences (drawn, names[1]) == 1);

    ui.movePointer (centre (itemMin[1], itemMax[1]));
    ui.frame (submit);
    ui.pressPointer (true);
    ui.frame (submit);
    ui.pressPointer (false);
    ui.frame (submit);
    CHECK (selected == 1);
    drawn = ui.frame (submit);
    CHECK_FALSE (open);
    CHECK (occurrences (drawn, names[1]) == 1);
    CHECK (occurrences (drawn, names[0]) == 0);
}
