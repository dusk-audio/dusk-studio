#pragma once

#include "../../../foundation/Fs.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

extern "C" char** environ;

namespace duskstudio::ipc::platform
{
// A temporary folder of a sandbox child's own, named to it by TMPDIR and
// removed by the parent once it has reaped the child. The parent ends a child
// with a signal, which runs no destructors, and the destructor of JUCE's LV2
// host is the only thing that removes the folder it writes the LV2
// specifications to.
class ChildTempDirectory
{
public:
    ChildTempDirectory() = default;
    ~ChildTempDirectory() { remove(); }
    ChildTempDirectory (const ChildTempDirectory&) = delete;
    ChildTempDirectory& operator= (const ChildTempDirectory&) = delete;

    // Creates the folder and returns this process's environment with TMPDIR
    // naming it, valid until the next call. When the folder cannot be created
    // the child is given the environment unchanged.
    char* const* environment()
    {
        remove();
        auto pattern = (dusk::fs::tempDir() / "dusk-child-XXXXXX").string();
        if (! std::filesystem::path (pattern).is_absolute() || ::mkdtemp (pattern.data()) == nullptr)
            return environ;
        folder = pattern;
        assignment = "TMPDIR=" + pattern;
        variables.clear();
        for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry)
            if (std::strncmp (*entry, "TMPDIR=", 7) != 0) variables.push_back (*entry);
        variables.push_back (assignment.data());
        variables.push_back (nullptr);
        return variables.data();
    }

    void remove() noexcept
    {
        if (folder.empty()) return;
        std::error_code ignored;
        std::filesystem::remove_all (folder, ignored);
        folder.clear();
    }

private:
    std::filesystem::path folder;
    std::string assignment;
    std::vector<char*> variables;
};
} // namespace duskstudio::ipc::platform
