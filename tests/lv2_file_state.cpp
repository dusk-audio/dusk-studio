#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestTempDirectory.h"
#include "engine/hosting/InsertAdapter.h"
#include "engine/lv2/Lv2Bundle.h"
#include "engine/lv2/Lv2Instance.h"
#include "engine/lv2/Lv2StatePaths.h"
#include "engine/lv2/Lv2UiParameterSync.h"

#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;
constexpr const char* kPluginUri = "urn:duskstudio:test:file-state";
constexpr const char* kControlPluginUri = "urn:duskstudio:test:control-state";
constexpr const char* kRefusedPathPluginUri = "urn:duskstudio:test:refused-paths";
constexpr const char* kPayload = "dusk-lv2-file-state-v1";
constexpr const char* kRestorePayload = "dusk-lv2-restore-make-path-v1";

using TempDirectory = duskstudio::test::TempDirectory;

std::string readFile (const fs::path& path)
{
    std::ifstream input (path, std::ios::binary);
    return { std::istreambuf_iterator<char> (input),
             std::istreambuf_iterator<char>() };
}

struct PortValueMutation
{
    std::string symbol;
    std::string replacement;
};

bool replacePortValue (std::vector<uint8_t>& blob, const PortValueMutation& mutation)
{
    std::string state (blob.begin(), blob.end());
    std::string symbolMarker = "lv2:symbol \"";
    symbolMarker.append (mutation.symbol).append ("\"");
    const auto symbolPos = state.find (symbolMarker);
    if (symbolPos == std::string::npos) return false;
    const std::string valueMarker = "pset:value ";
    const auto markerPos = state.find (valueMarker, symbolPos);
    if (markerPos == std::string::npos) return false;
    const auto valueBegin = markerPos + valueMarker.size();
    const auto valueEnd = state.find_first_of (" \t\r\n;]", valueBegin);
    if (valueEnd == std::string::npos) return false;
    state.replace (valueBegin, valueEnd - valueBegin, mutation.replacement);
    blob.assign (state.begin(), state.end());
    return true;
}

const duskstudio::lv2::Lv2Instance::ParamInfo* findParam (
    const duskstudio::lv2::Lv2Instance& instance, const std::string& name)
{
    for (int i = 0; i < instance.paramCount(); ++i)
        if (const auto* param = instance.paramInfo (i);
            param != nullptr && param->name == name)
            return param;
    return nullptr;
}

double paramValue (const duskstudio::lv2::Lv2Instance& instance,
                   const std::string& name)
{
    const auto* param = findParam (instance, name);
    if (param == nullptr)
    {
        std::string message = "missing LV2 parameter: ";
        throw std::runtime_error (message.append (name));
    }
    double value = 0.0;
    if (! instance.getParamValue (param->id, value))
    {
        std::string message = "unreadable LV2 parameter: ";
        throw std::runtime_error (message.append (name));
    }
    return value;
}

void requireRestoredAudio (duskstudio::lv2::Lv2Instance& instance)
{
    constexpr int frames = 32;
    duskstudio::hosting::InsertAdapter adapter;
    adapter.prepare (instance.portLayout(), frames);
    std::vector<float> left ((size_t) frames, 0.25f);
    std::vector<float> right ((size_t) frames, -0.5f);
    adapter.process (instance, left.data(), right.data(), frames);
    REQUIRE_THAT (left.front(), Catch::Matchers::WithinAbs (0.25f, 1.0e-7f));
    REQUIRE_THAT (right.front(), Catch::Matchers::WithinAbs (-0.5f, 1.0e-7f));
}
}

TEST_CASE ("LV2 file-backed state survives consecutive save and restore generations",
           "[lv2][state][integration][regression][issue-357][issue-388]")
{
    TempDirectory temp ("dusk-lv2-file-state-integration-");
    const auto stateDir = temp.path() / "state";

    duskstudio::lv2::Lv2Bundle bundle;
    std::string error;
    REQUIRE (bundle.load (DUSKSTUDIO_FILE_STATE_LV2_FIXTURE_PATH, error));

    duskstudio::lv2::Lv2Instance first;
    first.setStateDirectory (stateDir);
    REQUIRE (first.create (bundle, kPluginUri, error));
    REQUIRE (first.activate (48000.0, 32, error));
    std::vector<uint8_t> firstBlob;
    REQUIRE (first.saveState (firstBlob));
    REQUIRE (readFile (stateDir / "cur" / "payload.txt") == kPayload);
    REQUIRE (std::string (firstBlob.begin(), firstBlob.end())
                 .find (stateDir.u8string()) == std::string::npos);

    // Simulate a crash after cur/ moved to prev/ with a complete but
    // uncommitted next/. loadState must select the generation matching the
    // session blob before mapPath resolves any file-backed paths.
    fs::rename (stateDir / "cur", stateDir / "prev");
    fs::copy (stateDir / "prev", stateDir / "next", fs::copy_options::recursive);
    std::ofstream (stateDir / "next" / duskstudio::lv2::statepaths::kStateFileName,
                   std::ios::binary | std::ios::trunc) << "uncommitted";
    std::ofstream (stateDir / "next" / duskstudio::lv2::statepaths::kReadyMarkerName,
                   std::ios::binary | std::ios::trunc) << "ready\n";

    duskstudio::lv2::Lv2Instance second;
    second.setStateDirectory (stateDir);
    REQUIRE (second.create (bundle, kPluginUri, error));
    REQUIRE (second.activate (48000.0, 32, error));
    REQUIRE (second.loadState (firstBlob));
    REQUIRE (readFile (stateDir / "cur" / duskstudio::lv2::statepaths::kStateFileName)
             == std::string (firstBlob.begin(), firstBlob.end()));
    REQUIRE (readFile (stateDir / "cur" / "restore" / "payload.txt")
             == kRestorePayload);
    REQUIRE_FALSE (fs::exists (stateDir / "next"));
    requireRestoredAudio (second);

    const auto stateBeforeReactivate = readFile (
        stateDir / "cur" / duskstudio::lv2::statepaths::kStateFileName);
    REQUIRE (second.reactivate (48000.0, 64, error));
    REQUIRE (readFile (stateDir / "cur"
                       / duskstudio::lv2::statepaths::kStateFileName)
             == stateBeforeReactivate);
    REQUIRE_FALSE (fs::exists (stateDir / "prev"));
    requireRestoredAudio (second);

    std::vector<uint8_t> secondBlob;
    REQUIRE (second.saveState (secondBlob));
    REQUIRE (readFile (stateDir / "cur" / "payload.txt") == kPayload);
    REQUIRE (readFile (stateDir / "prev" / "payload.txt") == kPayload);
    REQUIRE (std::string (secondBlob.begin(), secondBlob.end())
                 .find (stateDir.u8string()) == std::string::npos);

    // cur/ is scratch for the restored instance. Lilv's stable copy directory
    // must contain only one preserved copy after another save, instead of
    // duplicating an unchanged sample bank into every generation.
    size_t payloadCopies = 0;
    for (const auto& entry : fs::recursive_directory_iterator (stateDir / "copy"))
        if (entry.is_regular_file() && readFile (entry.path()) == kPayload)
            ++payloadCopies;
    REQUIRE (payloadCopies == 1);
    std::ofstream (stateDir / "copy" / "orphan.bin", std::ios::binary) << "orphan";
    duskstudio::lv2::statepaths::pruneUnreferencedSharedFiles (stateDir);
    REQUIRE_FALSE (fs::exists (stateDir / "copy" / "orphan.bin"));
    REQUIRE (readFile (stateDir / "cur" / "payload.txt") == kPayload);

    const auto relocatedStateDir = temp.path() / "relocated-state";
    fs::copy (stateDir, relocatedStateDir,
              fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    first.deactivate();
    second.deactivate();
    fs::remove_all (stateDir);

    duskstudio::lv2::Lv2Instance third;
    third.setStateDirectory (relocatedStateDir);
    REQUIRE (third.create (bundle, kPluginUri, error));
    REQUIRE (third.activate (48000.0, 32, error));
    REQUIRE (third.loadState (secondBlob));
    REQUIRE (readFile (relocatedStateDir / "cur" / "restore" / "payload.txt")
             == kRestorePayload);
    requireRestoredAudio (third);
}

TEST_CASE ("LV2 file-backed save replaces an incomplete staging generation",
           "[lv2][state][integration][regression][issue-357]")
{
    TempDirectory temp ("dusk-lv2-file-state-integration-");
    const auto stateDir = temp.path() / "state";
    fs::create_directories (stateDir / "next");
    std::ofstream (stateDir / "next" / "recovery.bin", std::ios::binary)
        << "recoverable";

    duskstudio::lv2::Lv2Bundle bundle;
    std::string error;
    REQUIRE (bundle.load (DUSKSTUDIO_FILE_STATE_LV2_FIXTURE_PATH, error));

    duskstudio::lv2::Lv2Instance instance;
    instance.setStateDirectory (stateDir);
    REQUIRE (instance.create (bundle, kPluginUri, error));
    REQUIRE (instance.activate (48000.0, 32, error));
    REQUIRE_FALSE (instance.loadState ({}));
    std::vector<uint8_t> blob { 1 };
    REQUIRE (instance.saveState (blob));
    REQUIRE_FALSE (blob.empty());
    REQUIRE_FALSE (fs::exists (stateDir / "cur" / "recovery.bin"));
    REQUIRE (readFile (stateDir / "cur" / "payload.txt") == kPayload);
}

TEST_CASE ("LV2 failed file-backed restore cannot overwrite carried generations",
           "[lv2][state][integration][regression][issue-357]")
{
    TempDirectory temp ("dusk-lv2-file-state-integration-");
    const auto stateDir = temp.path() / "state";

    duskstudio::lv2::Lv2Bundle bundle;
    std::string error;
    REQUIRE (bundle.load (DUSKSTUDIO_FILE_STATE_LV2_FIXTURE_PATH, error));

    duskstudio::lv2::Lv2Instance source;
    source.setStateDirectory (stateDir);
    REQUIRE (source.create (bundle, kPluginUri, error));
    REQUIRE (source.activate (48000.0, 32, error));
    std::vector<uint8_t> carriedBlob;
    REQUIRE (source.saveState (carriedBlob));

    // Only prev.old matches the carried session blob. With newer cur/ and prev/
    // both present this is deliberately ambiguous and recovery must refuse it.
    fs::copy (stateDir / "cur", stateDir / "prev.old", fs::copy_options::recursive);
    fs::copy (stateDir / "cur", stateDir / "prev", fs::copy_options::recursive);
    std::ofstream (stateDir / "cur" / duskstudio::lv2::statepaths::kStateFileName,
                   std::ios::binary | std::ios::trunc) << "new current";
    std::ofstream (stateDir / "prev" / duskstudio::lv2::statepaths::kStateFileName,
                   std::ios::binary | std::ios::trunc) << "new previous";

    duskstudio::lv2::Lv2Instance restored;
    restored.setStateDirectory (stateDir);
    REQUIRE (restored.create (bundle, kPluginUri, error));
    REQUIRE (restored.activate (48000.0, 32, error));
    REQUIRE_FALSE (restored.loadState (carriedBlob));

    std::vector<uint8_t> replacement { 1, 2, 3 };
    REQUIRE_FALSE (restored.saveState (replacement));
    REQUIRE (replacement.empty());
    REQUIRE (readFile (stateDir / "prev.old"
                       / duskstudio::lv2::statepaths::kStateFileName)
             == std::string (carriedBlob.begin(), carriedBlob.end()));
    REQUIRE (readFile (stateDir / "cur"
                       / duskstudio::lv2::statepaths::kStateFileName)
             == "new current");
    REQUIRE_FALSE (fs::exists (stateDir / "next"));
}

TEST_CASE ("LV2 patch parameters are exposed in a stable order",
           "[lv2][params][regression][issue-355]")
{
    // Parameter INDEX is the identity MIDI bindings persist, so it has to mean
    // the same parameter on the next launch. lilv defines no iteration order for
    // the property collection, and the fixture declares its two properties in the
    // reverse of their URI order to catch a host that just takes what it is given.
    duskstudio::lv2::Lv2Bundle bundle;
    std::string error;
    REQUIRE (bundle.load (DUSKSTUDIO_FILE_STATE_LV2_FIXTURE_PATH, error));

    duskstudio::lv2::Lv2Instance instance;
    REQUIRE (instance.create (bundle, kPluginUri, error));
    REQUIRE (instance.activate (48000.0, 32, error));

    std::vector<std::string> names;
    for (int i = 0; i < instance.paramCount(); ++i)
        if (const auto* p = instance.paramInfo (i); p != nullptr && p->isPatchProperty)
            names.push_back (p->name);

    auto sorted = names;
    std::sort (sorted.begin(), sorted.end());
    REQUIRE (names.size() == 12);
    REQUIRE (names == sorted);
}

TEST_CASE ("LV2 control-state restore rejects or normalizes corrupt values",
           "[lv2][state][regression][issue-387]")
{
    duskstudio::lv2::Lv2Bundle bundle;
    std::string error;
    REQUIRE (bundle.load (DUSKSTUDIO_FILE_STATE_LV2_FIXTURE_PATH, error));

    duskstudio::lv2::Lv2Instance source;
    REQUIRE (source.create (bundle, kControlPluginUri, error));
    REQUIRE (source.activate (48000.0, 32, error));
    std::vector<uint8_t> cleanState;
    REQUIRE (source.saveState (cleanState));

    SECTION ("finite bounded values clamp and discrete ports normalize")
    {
        auto corruptState = cleanState;
        REQUIRE (replacePortValue (corruptState, { "gain", "\"1e100\"^^xsd:double" }));
        REQUIRE (replacePortValue (corruptState, { "toggle", "\"0.25\"^^xsd:float" }));
        REQUIRE (replacePortValue (corruptState, { "integer", "\"3.6\"^^xsd:float" }));
        REQUIRE (replacePortValue (corruptState, { "enumeration", "\"3.6\"^^xsd:float" }));

        duskstudio::lv2::Lv2Instance restored;
        REQUIRE (restored.create (bundle, kControlPluginUri, error));
        REQUIRE (restored.activate (48000.0, 32, error));
        REQUIRE (restored.loadState (corruptState));
        CHECK_THAT (paramValue (restored, "Gain"),
                    Catch::Matchers::WithinAbs (1.0, 1.0e-7));
        CHECK_THAT (paramValue (restored, "Toggle"),
                    Catch::Matchers::WithinAbs (1.0, 1.0e-7));
        CHECK_THAT (paramValue (restored, "Integer"),
                    Catch::Matchers::WithinAbs (4.0, 1.0e-7));
        CHECK_THAT (paramValue (restored, "Enumeration"),
                    Catch::Matchers::WithinAbs (4.0, 1.0e-7));
    }

    SECTION ("finite unbounded values survive save and restore")
    {
        const auto* unbounded = findParam (source, "Unbounded");
        REQUIRE (unbounded != nullptr);
        source.setParamValue (unbounded->id, 0.75);
        std::vector<uint8_t> unboundedState;
        REQUIRE (source.saveState (unboundedState));

        duskstudio::lv2::Lv2Instance restored;
        REQUIRE (restored.create (bundle, kControlPluginUri, error));
        REQUIRE (restored.activate (48000.0, 32, error));
        REQUIRE (restored.loadState (unboundedState));
        CHECK_THAT (paramValue (restored, "Unbounded"),
                    Catch::Matchers::WithinAbs (0.75, 1.0e-7));
    }

    SECTION ("NaN and infinity cannot publish a non-finite or out-of-range value")
    {
        duskstudio::lv2::Lv2Instance restored;
        REQUIRE (restored.create (bundle, kControlPluginUri, error));
        REQUIRE (restored.activate (48000.0, 32, error));
        const auto* gain = findParam (restored, "Gain");
        REQUIRE (gain != nullptr);
        restored.setParamValue (gain->id, 0.75);

        auto nanState = cleanState;
        REQUIRE (replacePortValue (nanState, { "gain", "\"NaN\"^^xsd:float" }));
        REQUIRE (restored.loadState (nanState));
        const double afterNan = paramValue (restored, "Gain");
        CHECK (std::isfinite (afterNan));
        CHECK (afterNan >= 0.0);
        CHECK (afterNan <= 1.0);

        auto infinityState = cleanState;
        REQUIRE (replacePortValue (infinityState, { "gain", "\"INF\"^^xsd:float" }));
        REQUIRE (restored.loadState (infinityState));
        const double afterInfinity = paramValue (restored, "Gain");
        CHECK (std::isfinite (afterInfinity));
        CHECK (afterInfinity >= 0.0);
        CHECK (afterInfinity <= 1.0);
    }
}

TEST_CASE ("LV2 editor events carry current control and patch values",
           "[lv2][editor][regression][issue-355]")
{
    duskstudio::lv2::Lv2Bundle bundle;
    std::string error;
    REQUIRE (bundle.load (DUSKSTUDIO_FILE_STATE_LV2_FIXTURE_PATH, error));

    duskstudio::lv2::Lv2Instance instance;
    REQUIRE (instance.create (bundle, kPluginUri, error));
    REQUIRE (instance.activate (48000.0, 32, error));
    REQUIRE (instance.uiParameterEventCount() == instance.paramCount());

    int controlIndex = -1;
    int patchIndex = -1;
    for (int i = 0; i < instance.paramCount(); ++i)
    {
        const auto* param = instance.paramInfo (i);
        if (param != nullptr && param->isPatchProperty)
            patchIndex = i;
        else if (param != nullptr)
            controlIndex = i;
    }
    REQUIRE (controlIndex >= 0);
    REQUIRE (patchIndex >= 0);

    const auto* control = instance.paramInfo (controlIndex);
    instance.setParamValue (control->id, 0.75);
    duskstudio::lv2::Lv2Instance::UiParameterEvent event;
    REQUIRE (instance.currentUiParameterEvent (controlIndex, event));
    REQUIRE (event.portIndex == control->id);
    REQUIRE (event.protocol == 0);
    REQUIRE (event.sizeBytes == sizeof (float));
    float controlValue = 0.0f;
    std::memcpy (&controlValue, event.data.data(), sizeof (controlValue));
    REQUIRE_THAT (controlValue, Catch::Matchers::WithinAbs (0.75f, 1.0e-7f));

    const auto* patch = instance.paramInfo (patchIndex);
    instance.setParamValue (patch->id, 0.625);
    REQUIRE (instance.currentUiParameterEvent (patchIndex, event));
    REQUIRE (event.portIndex == 6);
    REQUIRE (event.protocol == instance.uiEventTransferUrid());
    REQUIRE (event.sizeBytes > sizeof (float));
    REQUIRE_THAT (event.value, Catch::Matchers::WithinAbs (0.625f, 1.0e-7f));

    duskstudio::lv2::Lv2UiParameterSync sync;
    std::vector<duskstudio::lv2::Lv2Instance::UiParameterEvent> sent;
    auto capture = [&sent] (const auto& uiEvent) { sent.push_back (uiEvent); };

    sync.sendCurrentValues (instance, true, capture);
    REQUIRE (sent.size() == (size_t) instance.uiParameterEventCount());
    sent.clear();
    sync.sendCurrentValues (instance, false, capture);
    REQUIRE (sent.empty());

    instance.setParamValue (control->id, 0.5);
    sync.sendCurrentValues (instance, false, capture);
    REQUIRE (sent.size() == 1);
    REQUIRE (sent.front().portIndex == control->id);
    sent.clear();

    instance.setParamValue (patch->id, 0.375);
    sync.sendCurrentValues (instance, false, capture);
    REQUIRE (sent.size() == 1);
    REQUIRE (sent.front().protocol == instance.uiEventTransferUrid());
}

TEST_CASE ("LV2 refused state paths stay inside the state root and fail the restore",
           "[lv2][state][integration][regression][issue-458]")
{
    TempDirectory temp ("dusk-lv2-file-state-integration-");
    const auto stateDir = temp.path() / "state";
    const auto canonicalState =
        duskstudio::lv2::statepaths::normalizeStateDirectory (stateDir);

    duskstudio::lv2::Lv2Bundle bundle;
    std::string error;
    REQUIRE (bundle.load (DUSKSTUDIO_FILE_STATE_LV2_FIXTURE_PATH, error));

    duskstudio::lv2::Lv2Instance source;
    source.setStateDirectory (stateDir);
    REQUIRE (source.create (bundle, kRefusedPathPluginUri, error));
    REQUIRE (source.activate (48000.0, 32, error));
    std::vector<uint8_t> blob;
    REQUIRE (source.saveState (blob));

    duskstudio::lv2::Lv2Instance restored;
    restored.setStateDirectory (stateDir);
    REQUIRE (restored.create (bundle, kRefusedPathPluginUri, error));
    REQUIRE (restored.activate (48000.0, 32, error));
    REQUIRE_FALSE (restored.loadState (blob));

    std::ifstream report (stateDir / "cur" / "probe" / "refusals.txt");
    std::vector<std::string> refused;
    for (std::string line; std::getline (report, line);) refused.push_back (line);
    REQUIRE (refused.size() == 4);
    for (const auto& path : refused)
    {
        INFO ("refused path: " << path);
        REQUIRE (fs::path (path).is_absolute());
        REQUIRE (duskstudio::lv2::statepaths::isWithin (canonicalState, fs::path (path)));
        REQUIRE_FALSE (fs::exists (fs::path (path)));
    }

    // A refused restore left the instance at its defaults, so it must not be
    // able to publish those over the generation the session still carries.
    std::vector<uint8_t> replacement { 1, 2, 3 };
    REQUIRE_FALSE (restored.saveState (replacement));
}

TEST_CASE ("LV2 blob-only restore resolves file paths inside cur",
           "[lv2][state][integration][regression][issue-458]")
{
    TempDirectory temp ("dusk-lv2-file-state-integration-");
    const auto stateDir = temp.path() / "state";

    duskstudio::lv2::Lv2Bundle bundle;
    std::string error;
    REQUIRE (bundle.load (DUSKSTUDIO_FILE_STATE_LV2_FIXTURE_PATH, error));

    duskstudio::lv2::Lv2Instance source;
    source.setStateDirectory (stateDir);
    REQUIRE (source.create (bundle, kPluginUri, error));
    REQUIRE (source.activate (48000.0, 32, error));
    std::vector<uint8_t> blob;
    REQUIRE (source.saveState (blob));
    source.deactivate();

    // A session from before generation management: the plugin's files sit in
    // cur/, the blob is the only manifest and no generation matches its bytes.
    const auto legacyDir = temp.path() / "legacy";
    fs::create_directories (legacyDir / "cur");
    fs::copy_file (stateDir / "cur" / "payload.txt", legacyDir / "cur" / "payload.txt");

    duskstudio::lv2::Lv2Instance restored;
    restored.setStateDirectory (legacyDir);
    REQUIRE (restored.create (bundle, kPluginUri, error));
    REQUIRE (restored.activate (48000.0, 32, error));
    REQUIRE (restored.loadState (blob));

    // Adopted into cur/, so lilv parses it with a state directory of its own and
    // the blob's relative paths cannot resolve anywhere else.
    REQUIRE (readFile (legacyDir / "cur" / duskstudio::lv2::statepaths::kStateFileName)
             == std::string (blob.begin(), blob.end()));
    REQUIRE (readFile (legacyDir / "cur" / "restore" / "payload.txt") == kRestorePayload);
    requireRestoredAudio (restored);
}
