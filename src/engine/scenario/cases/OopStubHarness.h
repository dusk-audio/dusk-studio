#pragma once

#include "../../PluginDescriptor.h"
#include "../../PluginManager.h"
#include "../../PluginSlot.h"
#include "../../../foundation/Fs.h"

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

// Shared rigging for the sandboxed-plugin scenarios: they all need the real
// plugin-host child in one of its stub modes, a descriptor the stub will answer
// for, and a MIDI buffer to hand the slot's process entry.
namespace duskstudio::scenario::oopstub
{
#if DUSKSTUDIO_HAS_OOP_PLUGINS
// The child ships beside the app; a build tree without it (or a platform that
// does not build it) turns these scenarios into skips rather than failures.
inline std::optional<std::filesystem::path> hostBinary()
{
    const auto executable = dusk::fs::currentExecutablePath();
    if (executable.empty()) return std::nullopt;

   #if defined(_WIN32)
    const auto child = executable.parent_path() / "dusk-studio-plugin-host.exe";
   #else
    const auto child = executable.parent_path() / "dusk-studio-plugin-host";
   #endif

    std::error_code error;
    if (! std::filesystem::is_regular_file (child, error)) return std::nullopt;
    return child;
}

// The stub children answer without opening anything, so the location only has to
// be a plausible one the slot will hand to the child verbatim.
inline PluginDescriptor stubDescriptor()
{
    PluginDescriptor descriptor;
    descriptor.name = "sandbox stub";
    descriptor.formatName = "VST3";
    descriptor.location = "/nonexistent/sandbox-stub.vst3";
    return descriptor;
}

inline void useStub (PluginManager& manager, const std::filesystem::path& host,
                     const char* modeArg)
{
    manager.setOopEnabled (true);
    manager.setHostExecutableOverride (host.u8string(), modeArg);
}

// The slot's process entry takes the framework's MIDI buffer by reference;
// deducing the type from the member function keeps these cases free of the
// framework header.
template <typename Ret, typename Class, typename A0, typename A1, typename A2, typename A3>
A3 midiBufferArgOf (Ret (Class::*) (A0, A1, A2, A3&) noexcept);

using SlotMidiBuffer = decltype (midiBufferArgOf (&PluginSlot::processStereoBlock));
#endif
} // namespace duskstudio::scenario::oopstub
