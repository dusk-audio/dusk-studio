#include "WindowState.h"

#include "../foundation/AppConfigDir.h"

namespace duskstudio
{
juce::File WindowState::getStorePath()
{
    const auto dir = dusk::fs::appConfigDir();
    if (dir.empty()) return {};
    const juce::File cfgDir (dir.u8string());
    if (! cfgDir.exists()) cfgDir.createDirectory();
    return cfgDir.getChildFile ("window-state.txt");
}

juce::String WindowState::load()
{
    const auto file = getStorePath();
    if (! file.existsAsFile()) return {};
    return file.loadFileAsString().trim();
}

void WindowState::save (const juce::String& stateString)
{
    const auto file = getStorePath();
    if (stateString.isEmpty() || file == juce::File()) return;
    file.replaceWithText (stateString);
}

bool WindowState::rectIsUsable (juce::Rectangle<int> rect)
{
    const auto& displays = juce::Desktop::getInstance().getDisplays();
    for (auto& d : displays.displays)
    {
        const auto inter = d.userArea.getIntersection (rect);
        if (inter.getWidth() >= kMinOnscreenPx && inter.getHeight() >= kMinOnscreenPx)
            return true;
    }
    return false;
}
} // namespace duskstudio
