#include "DafExporterBridge.hpp"

#include "MultiCompAccess.hpp"

// Defined in the plug-in. MultiCompAccess.hpp leaves it out, so this is the
// declaration the plug-in's editor makes for itself.
DUSK_WEAK float multiCompGetParameterValue (void* pluginInstancePointer, uint32_t index) noexcept;

namespace duskstudio::builtin
{
std::unique_ptr<DafPlugin> createMultiComp2 (std::uint32_t faceMask)
{
    auto bridge = std::make_unique<DAF_NAMESPACE::ExporterBridge>();
    multiCompSetHostFaces (bridge->instancePointer(), faceMask);
    return bridge;
}

void* multiComp2Instance (DafPlugin& multiComp2) noexcept
{
    // Each bridge's ExporterBridge lives in its library's DAF namespace, so
    // only a plug-in createMultiComp2 made passes this cast.
    auto* bridge = dynamic_cast<DAF_NAMESPACE::ExporterBridge*> (&multiComp2);
    return bridge != nullptr ? bridge->instancePointer() : nullptr;
}

void setMultiComp2Channels (void* instance, int channels) noexcept
{
    if (instance != nullptr)
        multiCompSetHostChannels (instance, channels);
}

void resetMultiComp2 (void* instance) noexcept
{
    if (instance != nullptr)
        multiCompResetHost (instance);
}

float multiComp2EditorValue (void* instance, std::uint32_t index) noexcept
{
    return instance != nullptr ? multiCompGetParameterValue (instance, index) : 0.0f;
}
} // namespace duskstudio::builtin
