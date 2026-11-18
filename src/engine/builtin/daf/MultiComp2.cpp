#include "DafExporterBridge.hpp"

#include "MultiCompAccess.hpp"

namespace duskstudio::builtin
{
std::unique_ptr<DafPlugin> createMultiComp2 (std::uint32_t faceMask)
{
    auto bridge = std::make_unique<DAF_NAMESPACE::ExporterBridge>();
    multiCompSetHostFaces (bridge->instancePointer(), faceMask);
    return bridge;
}

bool setMultiComp2Channels (DafPlugin& multiComp2, int channels) noexcept
{
    // Each bridge's ExporterBridge lives in its library's DAF namespace, so
    // only a plug-in createMultiComp2 made passes this cast.
    auto* bridge = dynamic_cast<DAF_NAMESPACE::ExporterBridge*> (&multiComp2);
    if (bridge == nullptr)
        return false;
    multiCompSetHostChannels (bridge->instancePointer(), channels);
    return true;
}

bool resetMultiComp2 (DafPlugin& multiComp2) noexcept
{
    auto* bridge = dynamic_cast<DAF_NAMESPACE::ExporterBridge*> (&multiComp2);
    if (bridge == nullptr)
        return false;
    multiCompResetHost (bridge->instancePointer());
    return true;
}
} // namespace duskstudio::builtin
