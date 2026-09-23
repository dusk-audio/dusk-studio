#include "DafExporterBridge.hpp"

namespace duskstudio::builtin
{
std::unique_ptr<DafPlugin> createTapeMachine2()
{
    return std::make_unique<DAF_NAMESPACE::ExporterBridge>();
}
} // namespace duskstudio::builtin
