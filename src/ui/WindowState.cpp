#include "WindowState.h"

namespace duskstudio
{
juce::File WindowState::getStorePath()
{
    auto cfgDir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                      .getChildFile ("Dusk Studio");
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
    if (stateString.isEmpty()) return;
    getStorePath().replaceWithText (stateString);
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
