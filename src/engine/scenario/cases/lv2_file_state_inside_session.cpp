#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../foundation/Fs.h"
#include "../../../session/Session.h"
#include "../../../session/SessionSerializer.h"

#include <array>
#include <filesystem>
#include <string>
#include <system_error>

namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_LV2
constexpr int kTrackIndex = 0;
constexpr const char* kPluginUri = "urn:duskstudio:test:file-state";
constexpr const char* kPayload   = "dusk-lv2-file-state-v1";

bool isUnder (const std::filesystem::path& root, const std::filesystem::path& candidate)
{
    std::error_code error;
    const auto relative = std::filesystem::relative (candidate, root, error);
    return ! error && ! relative.empty()
        && relative != std::filesystem::path (".")
        && *relative.begin() != std::filesystem::path ("..");
}

ScenarioResult runFileState (ScenarioContext& ctx)
{
    const auto fixture = *ctx.fixture ("file_state.lv2");
    auto& session = ctx.session();
    auto& engine = ctx.engine();

    const auto& sessionDir = ctx.sessionDir();

    auto& strip = engine.getChannelStrip (kTrackIndex);
    auto& slot = strip.getNativeLv2Slot();
    std::string error;
    if (! ctx.expect (slot.load (fixture, ScenarioContext::kSampleRate,
                             ScenarioContext::kBlockSize, error, kPluginUri),
                  "could not load the file-state fixture"))
    {
        ctx.note ("load error: " + error);
        return ctx.verdict();
    }

    engine.publishPluginStateForSave (true);
    ctx.expect (session.track (kTrackIndex).nativeLv2StateBase64.isNotEmpty(),
            "the plugin published no state");

    const auto stateDir = sessionDir / "state" / "lv2" / "track01";
    std::error_code fsError;
    if (! ctx.expect (std::filesystem::is_directory (stateDir, fsError),
                  "the plugin's file state did not land in the session's state directory"))
        return ctx.verdict();

    // The plugin's stored file has to live inside the session, and the Turtle
    // referencing it has to stay portable: an absolute path baked into the state
    // is what breaks a session the user moves or copies.
    int storedFiles = 0;
    int payloadFiles = 0;
    const auto sessionText = sessionDir.u8string();
    const auto scratchText = ctx.tempDir().u8string();
    for (std::filesystem::recursive_directory_iterator it (stateDir, fsError), end;
         ! fsError && it != end; it.increment (fsError))
    {
        if (! it->is_regular_file()) continue;
        ++storedFiles;
        ctx.expect (isUnder (sessionDir, it->path()),
                "a stored file escaped the session directory: " + it->path().u8string());

        const auto contents = dusk::fs::loadFileAsString (it->path());
        if (it->path().filename() == "payload.txt")
        {
            ++payloadFiles;
            ctx.expect (contents == kPayload, "a stored payload file was not what the plugin wrote");
        }
        if (it->path().extension() != ".ttl") continue;
        ctx.expect (contents.find (sessionText) == std::string::npos
                    && contents.find (scratchText) == std::string::npos,
                "the serialized state baked in an absolute path: " + it->path().u8string());
    }
    ctx.note ("files stored under the session: " + std::to_string (storedFiles));
    ctx.expect (payloadFiles > 0, "the plugin stored no file at all");

    const auto sessionFile = sessionDir / "session.json";
    if (! ctx.expect (SessionSerializer::save (session, sessionFile), "session save failed"))
        return ctx.verdict();

    slot.unload();
    session.track (kTrackIndex).nativeLv2Path.clear();
    session.track (kTrackIndex).nativeLv2PluginId.clear();
    session.track (kTrackIndex).nativeLv2StateBase64.clear();

    if (! ctx.expect (SessionSerializer::load (session, sessionFile), "session load failed"))
        return ctx.verdict();
    engine.consumePluginStateAfterLoad();

    ctx.expect (engine.getLastPluginLoadFailures().empty(),
            "reloading the session reported a plugin load failure");
    if (! ctx.expect (strip.isNativeLv2Loaded(), "the plugin did not come back after a reload"))
        return ctx.verdict();

    // The fixture only passes audio through once it has read its stored file
    // back, so a live signal is the proof the file state actually restored.
    std::array<float, ScenarioContext::kBlockSize> left {};
    std::array<float, ScenarioContext::kBlockSize> right {};
    left.fill (0.25f);
    right.fill (-0.5f);
    strip.getNativeLv2Slot().processStereo (left.data(), right.data(), left.data(),
                                            right.data(), ScenarioContext::kBlockSize);
    ctx.expect (left.front() > 0.2f && right.front() < -0.4f,
            "the restored plugin stayed silent, so it never read its stored file");

    strip.unloadNativeLv2();
    return ctx.verdict();
}
#endif

const ScenarioRegistrar registrar { Scenario {
    "lv2.file_state_inside_session",
    { "lv2", "state", "session" },
    Needs::Engine,
    { "file_state.lv2" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_LV2
        return runFileState (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without the native LV2 host");
       #endif
    }
} };
} // namespace
} // namespace duskstudio::scenario
