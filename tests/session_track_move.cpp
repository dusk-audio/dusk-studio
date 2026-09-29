#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"
#include "session/TrackMove.h"

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace duskstudio;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr int kN = Session::kNumTracks;

bool odd (int k) { return (k % 2) != 0; }
float step (int k, float base, float by) { return base + by * (float) k; }

juce::Colour colourFor (int k, int salt)
{
    return juce::Colour ((juce::uint8) (10 + k + salt), (juce::uint8) (200 - k),
                         (juce::uint8) (3 * k + salt), (juce::uint8) 255);
}

TakeProvenance provenanceFor (int k)
{
    return { 1000 + k, k + 1, odd (k) };
}

// Moves every field a save writes off its default, to a value that differs
// from the neighbouring k on both sides and from k + 17, so any field the move
// leaves behind shows up as the old occupant's value.
void fillTrack (Track& t, int k)
{
    const auto text = [k] (const char* what) { return juce::String (what) + "-" + juce::String (k); };
    t.name = text ("Track");
    t.colour = colourFor (k, 0);

    if (odd (k))
    {
        t.pluginDescriptor.reset();
        t.pluginLegacyDescriptionXml = text ("<PLUGIN/>");
    }
    else
    {
        PluginDescriptor d;
        d.name = text ("name").toStdString();
        d.descriptiveName = text ("descriptive").toStdString();
        d.manufacturer = text ("maker").toStdString();
        d.category = text ("category").toStdString();
        d.version = text ("version").toStdString();
        d.formatName = text ("format").toStdString();
        d.backend = (k % 4) == 0 ? PluginBackend::Native : PluginBackend::JuceLegacy;
        d.location = text ("/plugins/location").toStdString();
        d.pluginId = text ("id").toStdString();
        d.uniqueId = 100 + k;
        d.deprecatedUid = 200 + k;
        d.numInputChannels = 1 + k;
        d.numOutputChannels = 2 + k;
        d.lastFileModificationMs = 3000 + k;
        d.lastInfoUpdateMs = 4000 + k;
        d.isInstrument = (k % 4) == 0;
        d.hasSharedContainer = (k % 4) != 0;
        d.hasAraExtension = (k % 4) == 0;
        t.pluginDescriptor = d;
        t.pluginLegacyDescriptionXml = {};
    }
    t.pluginStateBase64 = text ("plugin-state");
    t.nativeClapPath = text ("/clap/path");
    t.nativeClapPluginId = text ("clap-id");
    t.nativeClapStateBase64 = text ("clap-state");
    t.nativeLv2Path = text ("/lv2/path");
    t.nativeLv2PluginId = text ("lv2-id");
    t.nativeLv2StateBase64 = text ("lv2-state");
    t.nativeVst3Path = text ("/vst3/path");
    t.nativeVst3PluginId = text ("vst3-id");
    t.nativeVst3StateBase64 = text ("vst3-state");
    t.nativeAuIdentifier = text ("au-id");
    t.nativeAuStateBase64 = text ("au-state");
    t.nativeMultisamplePath = text ("/sfz/path");
    t.nativeMultisampleStateBase64 = text ("sfz-state");
    t.builtinUnitId = text ("builtin").toStdString();
    t.builtinStateBase64 = text ("builtin-state").toStdString();

    auto& s = t.strip;
    s.faderDb = step (k, -20.0f, 0.5f);
    s.pan = step (k, -0.9f, 0.07f);
    s.mute = odd (k);
    s.solo = ! odd (k);
    s.phaseInvert = odd (k);
    s.insertBypassed = ! odd (k);
    s.faderGroupId = k % 5;
    t.inputMonitor = odd (k);
    t.printEffects = ! odd (k);
    t.inputSource = odd (k) ? k : kInputFollowsTrack;
    t.inputSourceR = odd (k) ? kInputFollowsTrack : 30 + k;
    t.midiInputIndex = k;
    t.midiInputIdentifier = text ("midi-in");
    t.midiOutputIndex = k + 1;
    t.midiOutputIdentifier = text ("midi-out");
    t.midiChannel = (k % 16) + 1;
    t.mode = k % 3;

    t.frozen = true;
    t.frozenAudioPath = text ("/freeze/track") + ".wav";
    t.frozenRegion = {};
    t.frozenRegion.file = juce::File (t.frozenAudioPath);
    t.frozenRegion.timelineStart = k;
    t.frozenRegion.lengthInSamples = 1000 + k;
    t.frozenRegion.numChannels = odd (k) ? 1 : 2;
    t.frozenRegion.gainDb = step (k, -2.0f, 0.1f);
    t.frozenPluginBypass = odd (k);

    for (int i = 0; i < ChannelStripParams::kNumBuses; ++i)
        s.busAssign[(size_t) i] = odd (k + i);
    s.auxSendsBypassed = odd (k);
    for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
    {
        s.auxSendDb[(size_t) i] = step (k, -30.0f + 0.25f * (float) i, 1.0f);
        s.auxSendPreFader[(size_t) i] = odd (k + i);
    }

    s.hpfEnabled = odd (k);
    s.hpfFreq = step (k, 30.0f, 1.0f);
    s.lpfEnabled = ! odd (k);
    s.lpfFreq = step (k, 5000.0f, 100.0f);
    s.eqEnabled = odd (k);
    s.eqBlackMode = ! odd (k);
    s.lfGainDb = step (k, -10.0f, 0.5f);
    s.lfFreq = step (k, 40.0f, 1.0f);
    s.lmGainDb = step (k, -9.0f, 0.5f);
    s.lmFreq = step (k, 300.0f, 10.0f);
    s.lmQ = step (k, 0.5f, 0.1f);
    s.hmGainDb = step (k, -8.0f, 0.5f);
    s.hmFreq = step (k, 1000.0f, 20.0f);
    s.hmQ = step (k, 0.6f, 0.1f);
    s.hfGainDb = step (k, -7.0f, 0.5f);
    s.hfFreq = step (k, 5000.0f, 50.0f);
    for (int f = 0; f < ChannelStripParams::kNumEqFreqs; ++f)
    {
        const auto which = (ChannelStripParams::EqFreq) f;
        s.legacyDial (which).set (step (k, 1.0f + 0.01f * (float) f, 0.1f),
                                  s.eqFreq (which).load(), odd (k + f));
    }

    s.compEnabled = odd (k);
    s.compModePicked = ! odd (k);
    s.compMode = k % 3;
    s.compOptoPeakRed = step (k, 10.0f, 1.0f);
    s.compOptoGain = step (k, 20.0f, 1.0f);
    s.compOptoLimit = odd (k);
    s.compFetInput = step (k, 1.0f, 0.5f);
    s.compFetOutput = step (k, -5.0f, 0.25f);
    s.compFetAttack = step (k, 0.1f, 0.01f);
    s.compFetRelease = step (k, 100.0f, 1.0f);
    s.compFetRatio = k % 5;
    s.compFetThresholdDb = step (k, -30.0f, 0.5f);
    s.compVcaThreshDb = step (k, -20.0f, 0.5f);
    s.compVcaRatio = step (k, 2.0f, 0.1f);
    s.compVcaAttack = step (k, 1.0f, 0.1f);
    s.compVcaRelease = step (k, 50.0f, 1.0f);
    s.compVcaOutput = step (k, -3.0f, 0.2f);
    s.compVcaOverEasy = odd (k);
    s.compVcaDetectorClassic = ! odd (k);

    auto& hw = t.hardwareInsert;
    hw.enabled = odd (k);
    {
        HardwareInsertRouting routing;
        routing.outputChL = k;
        routing.outputChR = k + 1;
        routing.inputChL = k + 2;
        routing.inputChR = k + 3;
        routing.latencySamples = 100 + k;
        routing.format = odd (k) ? 1 : 0;
        hw.routing.publish (std::make_unique<HardwareInsertRouting> (routing));
    }
    hw.outputGainDb = step (k, -6.0f, 0.25f);
    hw.inputGainDb = step (k, -3.0f, 0.25f);
    hw.dryWet = step (k, 0.1f, 0.03f);

    {
        AudioRegion r;
        r.file = juce::File (text ("/audio/region") + ".wav");
        r.timelineStart = 1000 * k + 1;
        r.lengthInSamples = 500 + k;
        r.sourceOffset = 10 + k;
        r.numChannels = odd (k) ? 2 : 1;
        r.fadeInSamples = 5 + k;
        r.fadeOutSamples = 7 + k;
        r.fadeInShape = (FadeShape) ((k % 5) + 1);
        r.fadeOutShape = (FadeShape) (((k + 2) % 5) + 1);
        r.fadeInAuto = odd (k);
        r.fadeOutAuto = ! odd (k);
        r.gainDb = step (k, -1.0f, -0.1f);
        r.provenance = provenanceFor (k);
        r.customColour = colourFor (k, 7);
        r.label = text ("region");
        r.muted = odd (k);
        r.locked = ! odd (k);
        TakeRef take;
        take.file = juce::File (text ("/audio/take") + ".wav");
        take.sourceOffset = 20 + k;
        take.lengthInSamples = 600 + k;
        take.provenance = provenanceFor (k + 1);
        r.previousTakes.push_back (take);
        t.regions = { r };
    }

    {
        MidiRegion r;
        r.timelineStart = 2000 * k + 3;
        r.lengthInSamples = 4800 + k;
        r.lengthInTicks = 960 + k;
        r.notes.push_back ({ (k % 16) + 1, 30 + k, 50 + k, k, 100 + k });
        r.ccs.push_back ({ (k % 16) + 1, 1 + k, 10 + k, (std::int64_t) k + 3 });
        r.provenance = provenanceFor (k + 2);
        r.customColour = colourFor (k, 11);
        r.label = text ("midi");
        r.muted = ! odd (k);
        r.locked = odd (k);
        r.tempoLock = ! odd (k);
        r.recordedAtBPM = 100.0 + k;
        MidiTakeRef take;
        take.lengthInTicks = 480 + k;
        take.notes.push_back ({ ((k + 1) % 16) + 1, 40 + k, 60 + k, k + 1, 200 + k });
        take.ccs.push_back ({ ((k + 2) % 16) + 1, 2 + k, 20 + k, (std::int64_t) k + 5 });
        take.provenance = provenanceFor (k + 3);
        r.previousTakes.push_back (take);
        t.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (1, r));
    }

    for (int p = 0; p < kNumAutomationParams; ++p)
        t.automationLanes[(size_t) p].publishPoints ({
            { 100 * k + p, 0.01f * (float) k + 0.001f * (float) p, 90.0f + (float) k },
            { 100 * k + p + 50, 0.5f + 0.01f * (float) k, 91.0f + (float) k } });
    t.automationMode = k % 4;
}

// What a save leaves out, filled and read back by hand.
struct Unsaved
{
    bool armed = false;
    float liveFader = 0, livePan = 0;
    std::array<float, ChannelStripParams::kNumAuxSends> liveAux {};
    bool liveMute = false, liveSolo = false;
    bool pingPending = false;
    int pingResult = 0;
    std::array<float, 5> meters {};
    std::array<bool, kNumAutomationParams> passOpen {};
    std::int64_t frozenStart = 0;
    float frozenGain = 0;

    static Unsaved of (const Track& t)
    {
        Unsaved u;
        u.armed = t.recordArmed.load();
        u.liveFader = t.strip.liveFaderDb.load();
        u.livePan = t.strip.livePan.load();
        for (size_t i = 0; i < u.liveAux.size(); ++i) u.liveAux[i] = t.strip.liveAuxSendDb[i].load();
        u.liveMute = t.strip.liveMute.load();
        u.liveSolo = t.strip.liveSolo.load();
        u.pingPending = t.hardwareInsert.pingPending.load();
        u.pingResult = t.hardwareInsert.pingResult.load();
        u.meters = { t.meterGrDb.load(), t.meterInputDb.load(), t.meterInputRDb.load(),
                     t.meterOutLDb.load(), t.meterOutRDb.load() };
        for (size_t p = 0; p < u.passOpen.size(); ++p) u.passOpen[p] = t.automationLanes[p].passOpen.load();
        u.frozenStart = t.frozenRegion.timelineStart;
        u.frozenGain = t.frozenRegion.gainDb;
        return u;
    }

    bool operator== (const Unsaved& o) const
    {
        const auto same = [] (float a, float b) { return ! (a < b) && ! (b < a); };
        bool eq = armed == o.armed && same (liveFader, o.liveFader) && same (livePan, o.livePan)
               && liveMute == o.liveMute && liveSolo == o.liveSolo && pingPending == o.pingPending
               && pingResult == o.pingResult && passOpen == o.passOpen
               && frozenStart == o.frozenStart && same (frozenGain, o.frozenGain);
        for (size_t i = 0; i < liveAux.size(); ++i) eq = eq && same (liveAux[i], o.liveAux[i]);
        for (size_t i = 0; i < meters.size(); ++i) eq = eq && same (meters[i], o.meters[i]);
        return eq;
    }
};

void fillUnsaved (Track& t, int k)
{
    t.recordArmed = odd (k);
    t.strip.liveFaderDb = step (k, -40.0f, 0.3f);
    t.strip.livePan = step (k, 0.9f, -0.05f);
    for (size_t i = 0; i < t.strip.liveAuxSendDb.size(); ++i)
        t.strip.liveAuxSendDb[i] = step (k, -50.0f + (float) i, 0.5f);
    t.strip.liveMute = odd (k);
    t.strip.liveSolo = ! odd (k);
    t.strip.faderTouched = true;
    t.strip.panTouched = true;
    for (auto& touched : t.strip.auxSendTouched) touched = true;
    t.hardwareInsert.pingPending = odd (k);
    t.hardwareInsert.pingResult = 300 + k;
    t.midiActivity = true;
    t.meterGrDb = step (k, -1.0f, -0.1f);
    t.meterInputDb = step (k, -60.0f, 1.0f);
    t.meterInputRDb = step (k, -61.0f, 1.0f);
    t.meterOutLDb = step (k, -62.0f, 1.0f);
    t.meterOutRDb = step (k, -63.0f, 1.0f);
    for (size_t p = 0; p < t.automationLanes.size(); ++p)
        t.automationLanes[p].passOpen = odd (k + (int) p);
}

nlohmann::json saved (const Session& session)
{
    return nlohmann::json::parse (SessionSerializer::serialize (session).toStdString());
}

// Every leaf path of a and b, split into those that agree and those that do
// not. Objects and same-length arrays are walked, so a single missed key is
// named rather than hidden in its parent.
void compareLeaves (const nlohmann::json& a, const nlohmann::json& b, const std::string& path,
                    std::vector<std::string>& same, std::vector<std::string>& different)
{
    static const nlohmann::json absent;
    if (a.is_object() && b.is_object())
    {
        std::set<std::string> keys;
        for (auto it = a.begin(); it != a.end(); ++it) keys.insert (it.key());
        for (auto it = b.begin(); it != b.end(); ++it) keys.insert (it.key());
        for (const auto& key : keys)
        {
            const auto ia = a.find (key);
            const auto ib = b.find (key);
            compareLeaves (ia != a.end() ? *ia : absent, ib != b.end() ? *ib : absent,
                           path + "/" + key, same, different);
        }
        return;
    }
    if (a.is_array() && b.is_array() && a.size() == b.size() && ! a.empty())
    {
        for (size_t i = 0; i < a.size(); ++i)
            compareLeaves (a[i], b[i], path + "[" + std::to_string (i) + "]", same, different);
        return;
    }
    (a == b ? same : different).push_back (path);
}

std::string joined (const std::vector<std::string>& paths)
{
    std::string out;
    for (const auto& p : paths) out += (out.empty() ? "" : ", ") + p;
    return out;
}

// The saved track a move should put at slot `to`: the one from `from`, with a
// followed input made explicit and the LV2 directory it used kept.
nlohmann::json expectedAt (const nlohmann::json& before, int to, int from)
{
    auto expected = before["tracks"][(size_t) from];
    if (from == to) return expected;
    if (expected["input_source"].get<int>() == kInputFollowsTrack)
        expected["input_source"] = from;
    if (! expected.contains ("lv2_state_tag"))
        expected["lv2_state_tag"] = Session::defaultLv2StateTag (from);
    if (expected["lv2_state_tag"] == Session::defaultLv2StateTag (to))
        expected.erase ("lv2_state_tag");
    return expected;
}

void requireMovedAsPlanned (const nlohmann::json& before, const nlohmann::json& after,
                            const TrackMovePlan& plan)
{
    for (int to = 0; to < kN; ++to)
    {
        std::vector<std::string> same, different;
        compareLeaves (expectedAt (before, to, plan.newToOld[(size_t) to]),
                       after["tracks"][(size_t) to], "", same, different);
        INFO ("slot " << to << " holds the wrong " << joined (different));
        CHECK (different.empty());
    }
    auto beforeRest = before, afterRest = after;
    beforeRest.erase ("tracks");
    afterRest.erase ("tracks");
    CHECK (beforeRest == afterRest);
}

// Tests run as parallel processes: a name another process took first comes
// back false from create_directory, and a fresh one is drawn.
std::filesystem::path makeTempDir()
{
    std::random_device rd;
    const auto parent = std::filesystem::temp_directory_path();
    for (;;)
    {
        const auto dir = parent / ("dusk-studio-track-move-" + std::to_string (rd()) + "-"
                                   + std::to_string (rd()));
        if (std::filesystem::create_directory (dir))
            return dir;
    }
}
} // namespace

TEST_CASE ("Session track move: every saved field travels with its track", "[session][track-move]")
{
    auto session = std::make_unique<Session>();
    for (int k = 0; k < kN; ++k)
        fillTrack (session->track (k), k);
    const auto before = saved (*session);

    const auto plan = planBlockMove ({ 17 }, 0);

    // Every slot the move rewrites must start out different on every saved
    // leaf from the track arriving there; a leaf left equal could not show a
    // field the move forgot. A key the save learns later fails here until
    // fillTrack sets it.
    for (int to = plan.lo; to <= plan.hi; ++to)
    {
        std::vector<std::string> same, different;
        compareLeaves (before["tracks"][(size_t) to],
                       before["tracks"][(size_t) plan.newToOld[(size_t) to]], "", same, different);
        same.erase (std::remove (same.begin(), same.end(), "/frozen"), same.end());
        INFO ("slot " << to << " already matches its incoming track on " << joined (same)
              << "; move it in fillTrack");
        REQUIRE (same.empty());
    }

    session->permuteTracks (plan);
    const auto after = saved (*session);
    requireMovedAsPlanned (before, after, plan);

    CHECK (after["tracks"][0]["name"] == "Track-17");
    CHECK (after["tracks"][0]["input_source"] == 17);
    CHECK (after["tracks"][1]["input_source"] == 0);
    CHECK (after["tracks"][2]["input_source"] == 1);
    CHECK (after["tracks"][18]["input_source"] == kInputFollowsTrack);

    SECTION ("a scattered block moved down")
    {
        const auto again = planBlockMove ({ 2, 7, 9 }, 20);
        session->permuteTracks (again);
        requireMovedAsPlanned (after, saved (*session), again);
    }
    SECTION ("the inverse move puts every saved field back but the followed inputs")
    {
        session->permuteTracks (invertTrackMove (plan));
        auto restored = saved (*session);
        for (int k = 0; k <= 17; ++k)
            if (before["tracks"][(size_t) k]["input_source"] == kInputFollowsTrack)
            {
                CHECK (restored["tracks"][(size_t) k]["input_source"] == k);
                restored["tracks"][(size_t) k]["input_source"] = kInputFollowsTrack;
            }
        CHECK (restored == before);
    }
    SECTION ("the inverse move with the followed slots flagged puts everything back")
    {
        TrackSlotMask followed {};
        for (int k = 0; k <= 17; ++k)
            followed[(size_t) k] = before["tracks"][(size_t) k]["input_source"] == kInputFollowsTrack;
        // One pinned input changed after the move keeps the change.
        session->track (1).inputSource = 9;
        session->permuteTracks (invertTrackMove (plan), followed);
        auto restored = saved (*session);
        CHECK (restored["tracks"][0]["input_source"] == 9);
        restored["tracks"][0]["input_source"] = before["tracks"][0]["input_source"];
        CHECK (restored == before);
    }
}

TEST_CASE ("Session track move: a mapping that is not one to one changes nothing", "[session][track-move]")
{
    auto session = std::make_unique<Session>();
    for (int k = 0; k < kN; ++k)
        fillTrack (session->track (k), k);
    const auto before = saved (*session);

    auto repeated = planBlockMove ({ 17 }, 0);
    repeated.newToOld[5] = repeated.newToOld[6];
    CHECK_FALSE (session->permuteTracks (repeated));
    CHECK_FALSE (session->stageTrackMove (repeated).has_value());

    auto outOfRange = planBlockMove ({ 17 }, 0);
    outOfRange.newToOld[0] = kN;
    CHECK_FALSE (session->permuteTracks (outOfRange));
    outOfRange.newToOld[0] = -1;
    CHECK_FALSE (session->permuteTracks (outOfRange));

    CHECK (saved (*session) == before);
    CHECK (session->permuteTracks (identityTrackMove()));
    CHECK (saved (*session) == before);
}

TEST_CASE ("Session track move: a landing takes the atomics as they are then, the rest as staged",
           "[session][track-move]")
{
    auto session = std::make_unique<Session>();
    for (int k = 0; k < kN; ++k)
        fillTrack (session->track (k), k);
    const auto plan = planBlockMove ({ 17 }, 0);

    auto staged = session->stageTrackMove (plan);
    REQUIRE (staged.has_value());
    for (int to = 0; to < kN; ++to)
        CHECK ((staged->into[(size_t) to] != nullptr) == (plan.newToOld[(size_t) to] != to));

    // Between the stage and the landing a control surface moves track 18's
    // fader and track 4's input goes back to following. Names are written on
    // the message thread alone, which is busy moving, so the staged one lands.
    session->track (17).strip.faderDb = -1.5f;
    session->track (3).inputSource = kInputFollowsTrack;
    session->track (17).name = "Renamed";
    session->landTrackMove (*staged);

    CHECK_THAT (session->track (0).strip.faderDb.load(), WithinAbs (-1.5, 1.0e-6));
    CHECK (session->track (4).inputSource.load() == 3);
    CHECK (session->track (0).name == "Track-17");
    CHECK (session->track (1).name == "Track-0");
}

TEST_CASE ("Session track move: what a save leaves out travels too, bar the gesture flags",
           "[session][track-move]")
{
    auto session = std::make_unique<Session>();
    std::array<Unsaved, kN> unsaved;
    for (int k = 0; k < kN; ++k)
    {
        fillTrack (session->track (k), k);
        fillUnsaved (session->track (k), k);
        unsaved[(size_t) k] = Unsaved::of (session->track (k));
    }
    for (int k = 1; k < kN; ++k)
        REQUIRE_FALSE (unsaved[(size_t) k] == unsaved[(size_t) k - 1]);
    REQUIRE_FALSE (unsaved[0] == unsaved[17]);

    const auto plan = planBlockMove ({ 17 }, 0);
    session->permuteTracks (plan);

    for (int to = 0; to < kN; ++to)
    {
        const auto& t = session->track (to);
        const int from = plan.newToOld[(size_t) to];
        INFO ("slot " << to << " from " << from);
        CHECK (Unsaved::of (t) == unsaved[(size_t) from]);
        CHECK (t.frozenRegion.file == juce::File (juce::String ("/freeze/track-") + juce::String (from) + ".wav"));
        const bool moved = from != to;
        CHECK (t.strip.faderTouched.load() == ! moved);
        CHECK (t.strip.panTouched.load() == ! moved);
        for (const auto& touched : t.strip.auxSendTouched)
            CHECK (touched.load() == ! moved);
        CHECK (t.midiActivity.load() == ! moved);
    }
}

TEST_CASE ("Session track move: each moved track publishes each snapshot once, the rest none",
           "[session][track-move]")
{
    auto session = std::make_unique<Session>();
    for (int k = 0; k < kN; ++k)
        fillTrack (session->track (k), k);

    struct Generations
    {
        std::uint64_t midi = 0, routing = 0;
        std::array<std::uint64_t, kNumAutomationParams> lanes {};
    };
    const auto generationsOf = [] (const Track& t)
    {
        Generations g;
        g.midi = t.midiRegions.generation();
        g.routing = t.hardwareInsert.routing.generation();
        for (size_t p = 0; p < g.lanes.size(); ++p) g.lanes[p] = t.automationLanes[p].snapshot.generation();
        return g;
    };

    std::array<Generations, kN> before;
    for (int k = 0; k < kN; ++k) before[(size_t) k] = generationsOf (session->track (k));

    // Two cycles: slots 3..9 rotate, and 12 swaps with 20.
    auto order = identityTrackMove().newToOld;
    for (int to = 3; to <= 9; ++to) order[(size_t) to] = to == 9 ? 3 : to + 1;
    std::swap (order[12], order[20]);
    const auto plan = trackMoveFromNewToOld (order);
    REQUIRE (plan.has_value());
    session->permuteTracks (*plan);

    for (int k = 0; k < kN; ++k)
    {
        const auto now = generationsOf (session->track (k));
        const std::uint64_t publishes = plan->newToOld[(size_t) k] != k ? 1 : 0;
        INFO ("slot " << k);
        CHECK (now.midi == before[(size_t) k].midi + publishes);
        CHECK (now.routing == before[(size_t) k].routing + publishes);
        for (size_t p = 0; p < now.lanes.size(); ++p)
            CHECK (now.lanes[p] == before[(size_t) k].lanes[p] + publishes);
    }

    const auto unchanged = saved (*session);
    session->permuteTracks (identityTrackMove());
    CHECK (saved (*session) == unchanged);
}

TEST_CASE ("Session track move: a moved track keeps its LV2 state directory", "[session][track-move][lv2]")
{
    Session session;
    for (int k = 0; k < kN; ++k)
        REQUIRE (session.lv2StateTagFor (k) == Session::defaultLv2StateTag (k));
    REQUIRE (Session::defaultLv2StateTag (0) == "track01");
    REQUIRE (Session::defaultLv2StateTag (23) == "track24");

    const auto plan = planBlockMove ({ 17 }, 0);
    session.permuteTracks (plan);
    CHECK (session.lv2StateTagFor (0) == "track18");
    for (int k = 1; k <= 17; ++k)
        CHECK (session.lv2StateTagFor (k) == Session::defaultLv2StateTag (k - 1));
    for (int k = 18; k < kN; ++k)
        CHECK (session.track (k).lv2StateTag.empty());

    SECTION ("a second move keeps the directory the track had first")
    {
        // Track 18 goes on to the end; the others close up behind it.
        session.permuteTracks (planBlockMove ({ 0 }, kN));
        CHECK (session.lv2StateTagFor (kN - 1) == "track18");
        for (int k = 0; k <= 16; ++k)
            CHECK (session.track (k).lv2StateTag.empty());
        for (int k = 17; k < kN - 1; ++k)
            CHECK (session.lv2StateTagFor (k) == Session::defaultLv2StateTag (k + 1));
    }
    SECTION ("moving back leaves no tag behind")
    {
        session.permuteTracks (invertTrackMove (plan));
        for (int k = 0; k < kN; ++k)
            CHECK (session.track (k).lv2StateTag.empty());
    }
    SECTION ("the tags survive a save and a load")
    {
        const auto dir = makeTempDir();
        const auto file = dir / "session.json";
        REQUIRE (SessionSerializer::save (session, file));
        Session loaded;
        REQUIRE (SessionSerializer::load (loaded, file));
        for (int k = 0; k < kN; ++k)
        {
            INFO ("slot " << k);
            CHECK (loaded.track (k).lv2StateTag == session.track (k).lv2StateTag);
        }
        std::error_code ignored;
        std::filesystem::remove_all (dir, ignored);
    }
}

TEST_CASE ("Session track move: a session without tags clears the ones the last session kept",
           "[session][track-move][lv2]")
{
    Session session;
    session.permuteTracks (planBlockMove ({ 17 }, 0));
    REQUIRE (session.track (0).lv2StateTag == "track18");

    const auto dir = makeTempDir();
    const auto file = dir / "session.json";
    const auto version = saved (Session {})["version"];
    std::ofstream (file) << nlohmann::json { { "version", version }, { "transport", nlohmann::json::object() } }.dump();

    REQUIRE (SessionSerializer::load (session, file));
    for (int k = 0; k < kN; ++k)
    {
        INFO ("slot " << k);
        CHECK (session.track (k).lv2StateTag.empty());
        CHECK (session.lv2StateTagFor (k) == Session::defaultLv2StateTag (k));
    }

    std::error_code ignored;
    std::filesystem::remove_all (dir, ignored);
}

TEST_CASE ("Session track move: a load drops tags that are malformed or share a directory",
           "[session][track-move][lv2]")
{
    CHECK (Session::isLv2StateTag ("track07"));
    CHECK_FALSE (Session::isLv2StateTag ("track7"));
    CHECK_FALSE (Session::isLv2StateTag ("track007"));
    CHECK_FALSE (Session::isLv2StateTag ("../track07"));
    CHECK_FALSE (Session::isLv2StateTag ("Track07"));
    CHECK_FALSE (Session::isLv2StateTag (""));

    Session source;
    auto root = saved (source);
    auto& tracks = root["tracks"];
    tracks[2]["lv2_state_tag"] = "../../etc";     // malformed
    tracks[3]["lv2_state_tag"] = "track04";       // the slot's own
    tracks[5]["lv2_state_tag"] = "track09";       // twice among kept tags
    tracks[6]["lv2_state_tag"] = "track09";
    tracks[7]["lv2_state_tag"] = "track11";       // slot 10's own directory
    tracks[12]["lv2_state_tag"] = "track14";      // a swapped pair, kept
    tracks[13]["lv2_state_tag"] = "track13";
    tracks[18]["lv2_state_tag"] = "track20";      // falls once 19 falls back
    tracks[19]["lv2_state_tag"] = "track22";      // slot 21's own directory

    const auto dir = makeTempDir();
    const auto file = dir / "session.json";
    std::ofstream (file) << root.dump();

    Session loaded;
    REQUIRE (SessionSerializer::load (loaded, file));
    for (int k = 0; k < kN; ++k)
    {
        INFO ("slot " << k);
        if (k == 12)      CHECK (loaded.track (k).lv2StateTag == "track14");
        else if (k == 13) CHECK (loaded.track (k).lv2StateTag == "track13");
        else              CHECK (loaded.track (k).lv2StateTag.empty());
    }

    std::set<std::string> directories;
    for (int k = 0; k < kN; ++k)
        directories.insert (loaded.lv2StateTagFor (k));
    CHECK (directories.size() == (size_t) kN);

    std::error_code ignored;
    std::filesystem::remove_all (dir, ignored);
}
