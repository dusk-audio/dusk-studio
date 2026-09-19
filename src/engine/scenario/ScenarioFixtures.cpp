#include "ScenarioContext.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace duskstudio::scenario
{
namespace
{
struct FixtureCandidate
{
    const char* logical;
    const char* relative;
};

// Logical name -> where the fixture lands under a DUSKSTUDIO_FIXTURE_DIR root.
// Several rows may share a logical name; they are tried in order. The runner
// exports build-tests/ and tests/fixtures/ as roots, which is where the test
// build writes these.
constexpr FixtureCandidate kCandidates[] = {
    { "multi_bus.clap",   "dusk-studio-multi-bus-clap-fixture.clap" },
    { "multi_bus.clap",   "tests/dusk-studio-multi-bus-clap-fixture.clap" },
    { "relayout.vst3",    "VST3/Release/dusk-studio-runtime-relayout-vst3-fixture.vst3" },
    { "relayout.vst3",    "VST3/dusk-studio-runtime-relayout-vst3-fixture.vst3" },
    { "file_state.lv2",   "tests/file-state-fixture.lv2" },
    { "many_patch.lv2",   "tests/many-patch-fixture.lv2" },
    { "smf.vendor_chunk", "midi/vendor-chunk.mid.hex" },
    { "smf.same_tick",    "midi/same-tick-retrigger.mid.hex" },
    { "smf.vendor_counted", "midi/vendor-chunk-counted.mid.hex" },
    { "panic_probe.clap", "dusk-studio-panic-probe-clap-fixture.clap" },
    { "panic_probe.clap", "tests/dusk-studio-panic-probe-clap-fixture.clap" },
    { "panic_probe.vst3", "VST3/Release/dusk-studio-panic-probe-vst3-fixture.vst3" },
    { "panic_probe.vst3", "VST3/dusk-studio-panic-probe-vst3-fixture.vst3" },
    { "no_window.clap",   "dusk-studio-no-window-clap-fixture.clap" },
    { "no_window.clap",   "tests/dusk-studio-no-window-clap-fixture.clap" },
};

// Same convention as PATH: a Windows root starts with a drive letter, so it
// cannot use ':' as the separator.
#ifdef _WIN32
constexpr char kRootSeparator = ';';
#else
constexpr char kRootSeparator = ':';
#endif

std::vector<std::filesystem::path> fixtureRoots()
{
    std::vector<std::filesystem::path> roots;
    const char* spec = std::getenv ("DUSKSTUDIO_FIXTURE_DIR");
    if (spec == nullptr || *spec == '\0') return roots;

    const std::string text (spec);
    std::size_t start = 0;
    while (start <= text.size())
    {
        const auto end = text.find (kRootSeparator, start);
        const auto piece = text.substr (start, end == std::string::npos ? std::string::npos
                                                                       : end - start);
        if (! piece.empty()) roots.emplace_back (piece);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return roots;
}
} // namespace

std::optional<std::filesystem::path> resolveFixture (const std::string& logical)
{
    const auto roots = fixtureRoots();
    if (roots.empty()) return std::nullopt;

    for (const auto& root : roots)
    {
        for (const auto& candidate : kCandidates)
        {
            if (logical != candidate.logical) continue;

            const auto path = root / candidate.relative;
            std::error_code error;
            if (std::filesystem::exists (path, error))
                return path;
        }
    }
    return std::nullopt;
}
} // namespace duskstudio::scenario
