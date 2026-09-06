#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../PluginManager.h"
#include "../../PluginSlot.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../session/Session.h"
#include "../../../session/SessionSerializer.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <type_traits>

namespace duskstudio::scenario
{
namespace
{
constexpr int kTrackIndex = 0;

// Not a check: this mints the session the black-box sandbox legs launch the real
// app against, so the plugin reference it writes has to be one the JUCE-hosted
// slot can reload out of process.
std::optional<ScenarioResult> runMint (ScenarioContext& ctx)
{
    const char* const outSpec = std::getenv ("DUSKSTUDIO_SCENARIO_OUT");
    if (outSpec == nullptr || *outSpec == '\0')
        return ScenarioResult::skip ("DUSKSTUDIO_SCENARIO_OUT is not set");

    const auto fixture = ctx.fixture ("relayout.vst3");
    if (! fixture)
        return ScenarioResult::skip ("missing fixture: relayout.vst3");

    const std::filesystem::path target (outSpec);
    const auto sessionDir = target.parent_path();
    if (sessionDir.empty())
        return ScenarioResult::fail ("DUSKSTUDIO_SCENARIO_OUT has no directory");

    std::error_code error;
    std::filesystem::create_directories (sessionDir, error);
    if (error)
        return ScenarioResult::fail ("could not create " + sessionDir.u8string()
                                     + ": " + error.message());

    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& manager = engine.getPluginManager();

    // Session owns framework file and string types; naming them through its own
    // getters keeps this file free of the framework header.
    using SessionFile   = std::decay_t<decltype (session.getSessionDirectory())>;
    using SessionString = std::decay_t<decltype (session.getSessionDirectory().getFullPathName())>;

    SessionString loadError;
    auto instance = manager.createPluginInstance (
        SessionFile (fixture->u8string().c_str()),
        ScenarioContext::kSampleRate, ScenarioContext::kBlockSize, loadError);
    if (instance == nullptr)
        return ScenarioResult::fail ("the fixture did not load through the JUCE host: "
                                     + loadError.toStdString());

    auto& track = session.track (kTrackIndex);
    track.pluginDescriptor = manager.descriptorForInstance (*instance);
    track.pluginLegacyDescriptionXml.clear();
    track.pluginStateBase64.clear();
    instance.reset();

    session.setSessionDirectory (SessionFile (sessionDir.u8string().c_str()));
    if (! SessionSerializer::save (session, target))
        return ScenarioResult::fail ("could not write " + target.u8string());
    if (! std::filesystem::is_regular_file (target, error))
        return ScenarioResult::fail ("the minted session file is missing");

    // Prove the reference is one the loader can honour before a black-box leg
    // relies on it. In-process here; the leg re-runs it with sandboxing on.
    engine.getChannelStrip (kTrackIndex).getPluginSlot().unload();
    if (! SessionSerializer::load (session, target))
        return ScenarioResult::fail ("the minted session did not load back");
    engine.consumePluginStateAfterLoad();

    if (! engine.getLastPluginLoadFailures().empty())
        return ScenarioResult::fail ("reloading the minted session reported: "
                                     + engine.getLastPluginLoadFailures().front().reason);
    if (! engine.getChannelStrip (kTrackIndex).getPluginSlot().isLoaded())
        return ScenarioResult::fail ("the minted session left the plugin slot empty");

    ctx.note ("minted " + target.u8string());
    engine.getChannelStrip (kTrackIndex).getPluginSlot().unload();
    return ScenarioResult::pass();
}

const ScenarioRegistrar registrar { Scenario {
    "session.mint_oop_fixture",
    { "helper", "oop", "session" },
    Needs::Engine,
    { "relayout.vst3" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runMint (ctx); },
    60000
} };
} // namespace
} // namespace duskstudio::scenario
