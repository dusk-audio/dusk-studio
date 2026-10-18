#pragma once

#include "DuskPanelWindow.h"
#include "../../engine/sfz/SfzLibraryService.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace duskstudio::imgui
{
// The service outlives the panel. Host callbacks run after the drawing frame.
std::unique_ptr<DuskPanelView> makeSfzRepositoryView (
    sfz::LibraryService&,
    std::function<void (const sfz::InstalledPack&, const sfz::ProviderInstrument&)> onLoad,
    std::function<std::vector<sfz::ManagedSfzLocator>()> inUse,
    std::function<void (std::string)> openSource,
    std::function<void()> openLocalFolders = {},
    std::optional<sfz::ManagedSfzLocator> offlineLocator = {},
    std::function<void (const sfz::InstalledPack&, const sfz::ProviderInstrument&)> onRelink = {});
} // namespace duskstudio::imgui
