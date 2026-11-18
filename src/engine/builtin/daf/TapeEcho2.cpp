#include "DafExporterBridge.hpp"

namespace duskstudio::builtin
{
std::unique_ptr<DafPlugin> createTapeEcho2()
{
    // Dry Level and Sync Division are kept for automation that older releases of
    // the plug-in wrote, and it leaves them visible to hosts so those lanes still
    // play. Writing Sync Division overrides the Echo Rate Note detent.
    return std::make_unique<DAF_NAMESPACE::ExporterBridge> (
        std::initializer_list<const char*> { "dry_level", "sync_division" });
}
} // namespace duskstudio::builtin
