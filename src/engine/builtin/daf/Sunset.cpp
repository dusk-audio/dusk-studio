#include "DafExporterBridge.hpp"

namespace duskstudio::builtin
{
std::unique_ptr<DafPlugin> createSunset()
{
    return std::make_unique<DAF_NAMESPACE::ExporterBridge>();
}
} // namespace duskstudio::builtin
