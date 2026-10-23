#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../session/Session.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace duskstudio::scenario
{
namespace
{
using Target = MidiBindingTarget;
enum class Mapping { Linear, Log, Send, TrackHpf, BusHpf, Button, Noop };

struct Control
{
    std::function<double()> read;
    std::function<void (double)> write;
    double expected = 0.0;
};

struct Operation
{
    MidiBinding binding;
    std::string destination;
    Mapping mapping = Mapping::Linear;
    double low = 0.0, high = 1.0;
    std::string section;
    int bank = 0;
    std::string modeControl;
    int mode = 0;
};

struct Matrix
{
    std::map<std::string, Control> controls;
    std::vector<Operation> operations;
    std::set<Target> visited;
    std::size_t operation = 0;
    int position = 0;
    int messages = 0;

    template <typename T>
    void add (ScenarioContext& ctx, const std::string& name, std::atomic<T>& atom)
    {
        ctx.keep (atom);
        controls.emplace (name, Control {
            [&atom] { return static_cast<double> (atom.load()); },
            [&atom] (double value) { atom.store (static_cast<T> (value)); },
            static_cast<double> (atom.load()) });
    }

    void set (const std::string& name, double value)
    {
        auto& control = controls.at (name);
        control.write (value);
        control.expected = value;
    }

    bool matches (const std::string& name) const
    {
        const auto& control = controls.at (name);
        const double actual = control.read();
        return std::isfinite (actual)
            && std::abs (actual - control.expected) <= 1.0e-6 * std::max (1.0, std::abs (control.expected));
    }

    bool check (ScenarioContext& ctx) const
    {
        for (const auto& entry : controls)
            if (! ctx.expect (matches (entry.first), "message " + std::to_string (messages) + ": "
                               + entry.first + " expected " + std::to_string (entry.second.expected)
                               + ", observed " + std::to_string (entry.second.read()))) return false;
        const auto any = [this] (int count, const char* prefix, const char* suffix)
        {
            for (int index = 0; index < count; ++index)
                if (controls.at (std::string (prefix) + std::to_string (index) + suffix).expected > 0.5) return true;
            return false;
        };
        auto& session = ctx.session();
        return ctx.expect (session.anyTrackSoloed() == any (24, "t", ".solo"), "track solo aggregate disagrees")
            && ctx.expect (session.anyTrackArmed() == any (24, "t", ".arm"), "track arm counter disagrees")
            && ctx.expect (session.anyBusSoloed() == any (4, "b", ".solo"), "bus solo counter disagrees");
    }
};

void publish (Session& session, const MidiBinding& binding)
{
    session.midiBindings.publish (std::make_unique<std::vector<MidiBinding>> (std::vector<MidiBinding> { binding }));
}

void send (ScenarioContext& ctx, Matrix& state, int value)
{
    const std::uint8_t bytes[] { 0xB0, 23, static_cast<std::uint8_t> (value) };
    dusk::MidiBuffer events;
    events.addEvent (bytes, 3, 0);
    ctx.pumpWithMidi (0, std::move (events));
    ++state.messages;
}

bool release (ScenarioContext& ctx, Matrix& state)
{
    ctx.session().midiBindings.publish (std::make_unique<std::vector<MidiBinding>>());
    send (ctx, state, 0);
    return state.check (ctx);
}

void append (Matrix& state, Target target, int index, const std::string& destination,
             Mapping mapping, double low, double high, const std::string& section = {},
             int bank = 0, int parameter = -1, const std::string& modeControl = {}, int mode = 0)
{
    MidiBinding binding;
    binding.channel = 1;
    binding.dataNumber = 23;
    binding.target = target;
    binding.targetIndex = index;
    binding.paramIndex = parameter;
    state.operations.push_back ({ binding, destination, mapping, low, high, section, bank, modeControl, mode });
}

void buildOperations (Matrix& state)
{
    for (const bool banked : { false, true })
        for (int track = 0; track < 24; ++track)
        {
            const std::string p = "t" + std::to_string (track);
            const int index = banked ? track % 8 : track;
            const int bank = banked ? track / 8 : 0;
            const auto target = [banked] (Target absolute, Target relative) { return banked ? relative : absolute; };
            const auto add = [&] (Target absolute, Target relative, const char* suffix, Mapping mapping,
                                  double low, double high, const std::string& section = {})
            { append (state, target (absolute, relative), index, p + suffix, mapping, low, high, section, bank); };
            add (Target::TrackFader, Target::TrackFaderBank, ".fader", Mapping::Linear, -90, 12);
            add (Target::TrackPan, Target::TrackPanBank, ".pan", Mapping::Linear, -1, 1);
            add (Target::TrackMute, Target::TrackMuteBank, ".mute", Mapping::Button, 0, 1);
            add (Target::TrackSolo, Target::TrackSoloBank, ".solo", Mapping::Button, 0, 1);
            add (Target::TrackArm, Target::TrackArmBank, ".arm", Mapping::Button, 0, 1);
            add (Target::TrackHpfFreq, Target::TrackHpfFreqBank, ".hpfFreq", Mapping::TrackHpf, 20, 300, p + ".eq");
            for (int sub = 0; sub < 4; ++sub)
            {
                const auto s = std::to_string (sub);
                append (state, target (Target::TrackAuxSend, Target::TrackAuxSendBank), index * 4 + sub,
                        p + ".send" + s, Mapping::Send, -60, 6, {}, bank);
                append (state, target (Target::TrackEqGain, Target::TrackEqGainBank), index * 4 + sub,
                        p + ".gain" + s, Mapping::Linear, -15, 15, p + ".eq", bank);
                static constexpr double lows[] { 30, 200, 600, 1500 };
                static constexpr double highs[] { 450, 2500, 7000, 16000 };
                append (state, target (Target::TrackEqFreq, Target::TrackEqFreqBank), index * 4 + sub,
                        p + ".freq" + s, Mapping::Log, lows[sub], highs[sub], p + ".eq", bank);
                append (state, target (Target::TrackEqQ, Target::TrackEqQBank), index * 4 + sub,
                        sub == 1 || sub == 2 ? p + ".q" + s : std::string(),
                        sub == 1 || sub == 2 ? Mapping::Linear : Mapping::Noop, 0.4, 4, p + ".eq", bank);
            }
            for (int mode = 0; mode < 3; ++mode)
            {
                const std::string s = std::to_string (mode);
                static constexpr double thresholdLow[] { 100, -60, -38 };
                static constexpr double thresholdHigh[] { 0, 0, 12 };
                append (state, target (Target::TrackCompThresh, Target::TrackCompThreshBank), index,
                        p + ".threshold" + s, Mapping::Linear, thresholdLow[mode], thresholdHigh[mode],
                        p + ".comp", bank, -1, p + ".compMode", mode);
                append (state, target (Target::TrackCompMakeup, Target::TrackCompMakeupBank), index,
                        p + ".makeup" + s, Mapping::Linear, mode == 0 ? 0 : -20, mode == 0 ? 100 : 20,
                        p + ".comp", bank, -1, p + ".compMode", mode);
            }
            for (const int parameter : { 0, 2 })
                append (state, target (Target::TrackPluginParam, Target::TrackPluginParamBank), index,
                        p + ".plugin" + std::to_string (parameter), Mapping::Linear,
                        parameter == 0 ? -60 : 0, parameter == 0 ? 24 : 200, {}, bank, parameter);
        }
    for (int track = 0; track < 24; ++track)
    {
        const std::string p = "t" + std::to_string (track);
        append (state, Target::TrackEqEnabled, track, p + ".eq", Mapping::Button, 0, 1);
        append (state, Target::TrackCompEnabled, track, p + ".comp", Mapping::Button, 0, 1);
        append (state, Target::TrackInsertBypass, track, p + ".hardware", Mapping::Button, 0, 1);
        for (int sub = 0; sub < 4; ++sub)
            append (state, Target::TrackAuxSendPrePost, track * 4 + sub,
                    p + ".pre" + std::to_string (sub), Mapping::Button, 0, 1);
    }
    for (int bus = 0; bus < 4; ++bus)
    {
        const std::string p = "b" + std::to_string (bus);
        append (state, Target::BusFader, bus, p + ".fader", Mapping::Linear, -90, 12);
        append (state, Target::BusPan, bus, p + ".pan", Mapping::Linear, -1, 1);
        append (state, Target::BusMute, bus, p + ".mute", Mapping::Button, 0, 1);
        append (state, Target::BusSolo, bus, p + ".solo", Mapping::Button, 0, 1);
        append (state, Target::BusHpfFreq, bus, p + ".hpfFreq", Mapping::BusHpf, 20, 3000, p + ".eq");
        for (int band = 0; band < 3; ++band)
            append (state, Target::BusEqGain, bus * 3 + band, p + ".gain" + std::to_string (band),
                    Mapping::Linear, -9, 9, p + ".eq");
        const std::string a = "a" + std::to_string (bus);
        append (state, Target::AuxLaneFader, bus, a + ".fader", Mapping::Linear, -90, 12);
        append (state, Target::AuxLaneMute, bus, a + ".mute", Mapping::Button, 0, 1);
        for (const int parameter : { 0, 2 })
            append (state, Target::AuxPluginParam, bus, a + ".plugin" + std::to_string (parameter),
                    Mapping::Linear, parameter == 0 ? -60 : 0, parameter == 0 ? 24 : 200, {}, 0, parameter);
    }
    append (state, Target::MasterFader, 0, "m.fader", Mapping::Linear, -90, 12);
    append (state, Target::MasterEqLfBoost, 0, "m.lf", Mapping::Linear, 0, 10);
    append (state, Target::MasterEqHfBoost, 0, "m.hf", Mapping::Linear, 0, 10);
    append (state, Target::MasterCompThresh, 0, "m.threshold", Mapping::Linear, -38, 12);
    append (state, Target::MasterCompMakeup, 0, "m.makeup", Mapping::Linear, -20, 20);
    append (state, Target::MasterCompRatio, 0, "m.ratio", Mapping::Linear, 1, 20);
}

void next (ScenarioContext& ctx, const std::shared_ptr<Matrix>& state)
{
    while (state->operation < state->operations.size())
    {
        const auto& op = state->operations[state->operation];
        const bool filter = op.mapping == Mapping::TrackHpf || op.mapping == Mapping::BusHpf;
        if (state->position == 0)
        {
            if (! release (ctx, *state)) { ctx.complete (ctx.verdict()); return; }
            state->set ("bank", op.bank);
            if (! op.modeControl.empty()) state->set (op.modeControl, op.mode);
            if (! op.section.empty()) state->set (op.section, 0);
            if (op.mapping == Mapping::Button) state->set (op.destination, 0);
            ctx.session().recomputeRtCounters();
            publish (ctx.session(), op.binding);
            state->visited.insert (op.binding.target);
        }
        if (filter && state->position == 3) state->set (op.section, 0);
        const int value = filter && state->position == 3 ? 0 : op.mapping == Mapping::Button
            ? (state->position == 1 ? 0 : 127)
            : (state->position == 0 ? 127 : state->position == 1 ? 0 : 37);
        const double fraction = static_cast<double> (value) / 127;
        if (op.mapping != Mapping::Noop)
        {
            double expected = op.low + (op.high - op.low) * fraction;
            if (op.mapping == Mapping::Log || op.mapping == Mapping::TrackHpf || op.mapping == Mapping::BusHpf)
                expected = op.low * std::pow (op.high / op.low, fraction);
            if (op.mapping == Mapping::Send && value == 0) expected = -100;
            if (op.mapping == Mapping::Button) expected = state->position < 2 ? 1 : 0;
            state->controls.at (op.destination).expected = expected;
            if (! op.section.empty())
            {
                if (! filter || value != 0) state->controls.at (op.section).expected = 1;
                if (filter)
                {
                    const auto prefix = op.destination.substr (0, op.destination.find ('.'));
                    state->controls.at (prefix + ".hpf").expected = value != 0 ? 1 : 0;
                }
            }
        }
        const double before = op.binding.paramIndex >= 0 ? state->controls.at (op.destination).read() : 0;
        send (ctx, *state, value);
        if (op.binding.paramIndex >= 0)
        {
            const double after = state->controls.at (op.destination).read();
            if (! ctx.expect (std::isfinite (after) && std::abs (after - before) <= 1.0e-6,
                              "plugin binding changed " + op.destination + " before the message-thread drain"))
            { ctx.complete (ctx.verdict()); return; }
            ctx.waitUntil ([state, name = op.destination] { return state->matches (name); }, 3000,
                           [&ctx, state]
                           {
                               if (! state->check (ctx)) { ctx.complete (ctx.verdict()); return; }
                               if (++state->position == 3) { state->position = 0; ++state->operation; }
                               next (ctx, state);
                           }, "the engine timer did not drain a plugin binding to " + op.destination);
            return;
        }
        if (! state->check (ctx)) { ctx.complete (ctx.verdict()); return; }
        if (++state->position == (filter ? 4 : 3)) { state->position = 0; ++state->operation; }
    }
    ctx.expect (state->visited.size() == 49, "the matrix did not exercise all 49 target kinds");
    ctx.expect (state->messages == 6708, "the matrix did not execute all endpoint, interior and button messages");
    ctx.note (std::to_string (state->messages) + " MIDI messages checked against "
              + std::to_string (state->controls.size()) + " controls after every dispatch");
    std::printf ("MIDI binding matrix: %d messages, %zu controls, %zu target kinds\n",
                 state->messages, state->controls.size(), state->visited.size());
    ctx.complete (ctx.verdict());
}

std::optional<ScenarioResult> runMatrix (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto state = std::make_shared<Matrix>();
    ctx.cleanup ([&session] { session.recomputeRtCounters(); });
    const auto bindings = session.midiBindings.current();
    ctx.cleanup ([&session, bindings]
    { session.midiBindings.publish (std::make_unique<std::vector<MidiBinding>> (bindings)); });
    const bool takeover = engine.getMidiSoftTakeover();
    const bool hardware = engine.setHardwareMidiInputsEnabledForScenario (false);
    engine.setMidiSoftTakeover (false);
    ctx.cleanup ([&engine, takeover, hardware]
    { engine.setMidiSoftTakeover (takeover); engine.setHardwareMidiInputsEnabledForScenario (hardware); });
    ctx.keep (session.midiLearnPending);
    ctx.keep (session.midiLearnCapture);
    ctx.keep (session.deviceCaptureChannels);
    session.midiLearnPending.store (-1);
    session.midiLearnCapture.store (0);
    session.deviceCaptureChannels.store (16);
    state->add (ctx, "bank", session.activeBank);
    state->add (ctx, "transport", session.pendingTransportAction);
    state->set ("transport", static_cast<int> (PendingTransportAction::None));
    for (int track = 0; track < 24; ++track)
    {
        auto& t = session.track (track);
        auto& s = t.strip;
        const std::string p = "t" + std::to_string (track);
        std::array<std::uint64_t, ChannelStripParams::kNumEqFreqs> dials {};
        for (std::size_t i = 0; i < dials.size(); ++i) dials[i] = s.eqFreqDial[i].raw();
        ctx.cleanup ([&s, dials]
        { for (std::size_t i = 0; i < dials.size(); ++i) s.eqFreqDial[i].setRaw (dials[i]); });
        const auto add = [&] (const char* name, auto& atom) { state->add (ctx, p + name, atom); };
        add (".fader", s.faderDb); add (".pan", s.pan); add (".mute", s.mute); add (".solo", s.solo);
        add (".arm", t.recordArmed); add (".hardware", t.hardwareInsert.enabled);
        add (".insertBypassed", s.insertBypassed); add (".phase", s.phaseInvert); add (".group", s.faderGroupId);
        add (".eq", s.eqEnabled); add (".hpf", s.hpfEnabled); add (".hpfFreq", s.hpfFreq);
        add (".lpf", s.lpfEnabled); add (".lpfFreq", s.lpfFreq); add (".black", s.eqBlackMode);
        add (".gain0", s.lfGainDb); add (".gain1", s.lmGainDb); add (".gain2", s.hmGainDb); add (".gain3", s.hfGainDb);
        add (".freq0", s.lfFreq); add (".freq1", s.lmFreq); add (".freq2", s.hmFreq); add (".freq3", s.hfFreq);
        add (".q1", s.lmQ); add (".q2", s.hmQ);
        add (".comp", s.compEnabled); add (".compMode", s.compMode);
        add (".threshold0", s.compOptoPeakRed); add (".threshold1", s.compFetThresholdDb); add (".threshold2", s.compVcaThreshDb);
        add (".makeup0", s.compOptoGain); add (".makeup1", s.compFetOutput); add (".makeup2", s.compVcaOutput);
        add (".modePicked", s.compModePicked); add (".optoLimit", s.compOptoLimit); add (".fetInput", s.compFetInput);
        add (".fetRatio", s.compFetRatio); add (".vcaRatio", s.compVcaRatio);
        add (".fetAttack", s.compFetAttack); add (".fetRelease", s.compFetRelease);
        add (".vcaAttack", s.compVcaAttack); add (".vcaRelease", s.compVcaRelease);
        for (int sub = 0; sub < 4; ++sub)
        {
            state->add (ctx, p + ".send" + std::to_string (sub), s.auxSendDb[static_cast<std::size_t> (sub)]);
            state->add (ctx, p + ".pre" + std::to_string (sub), s.auxSendPreFader[static_cast<std::size_t> (sub)]);
        }
        state->set (p + ".solo", 0); state->set (p + ".arm", 0); state->set (p + ".group", 0);
        ctx.keep (t.mode); ctx.keep (t.inputSource); ctx.keep (t.frozen);
        t.mode.store (static_cast<int> (Track::Mode::Mono)); t.inputSource.store (0); t.frozen.store (false);
    }
    for (int bus = 0; bus < 4; ++bus)
    {
        auto& s = session.bus (bus).strip;
        const std::string p = "b" + std::to_string (bus);
        const auto add = [&] (const char* name, auto& atom) { state->add (ctx, p + name, atom); };
        add (".fader", s.faderDb); add (".pan", s.pan); add (".mute", s.mute); add (".solo", s.solo);
        add (".eq", s.eqEnabled); add (".hpf", s.hpfEnabled); add (".hpfFreq", s.hpfFreq);
        add (".gain0", s.eqLfGainDb); add (".gain1", s.eqMidGainDb); add (".gain2", s.eqHfGainDb);
        add (".comp", s.compEnabled); add (".threshold", s.compThreshDb); add (".makeup", s.compMakeupDb);
        state->set (p + ".solo", 0);
        state->add (ctx, "a" + std::to_string (bus) + ".fader", session.auxLane (bus).params.returnLevelDb);
        state->add (ctx, "a" + std::to_string (bus) + ".mute", session.auxLane (bus).params.mute);
    }
    auto& m = session.master();
    state->add (ctx, "m.fader", m.faderDb); state->add (ctx, "m.eq", m.eqEnabled);
    state->add (ctx, "m.lf", m.eqLfBoost); state->add (ctx, "m.hf", m.eqHfBoost);
    state->add (ctx, "m.comp", m.compEnabled); state->add (ctx, "m.threshold", m.compThreshDb);
    state->add (ctx, "m.makeup", m.compMakeupDb); state->add (ctx, "m.ratio", m.compRatio);
    state->set ("m.eq", 0); state->set ("m.comp", 0);
    for (int index = 0; index < 28; ++index)
    {
        auto& strip = engine.getChannelStrip (std::min (index, 23));
        auto& lane = engine.getAuxLaneStrip (index < 24 ? 0 : index - 24);
        if (index < 24) ctx.keep (strip.insertMode);
        auto& slot = index < 24 ? strip.getBuiltinSlot() : lane.getBuiltinSlot (0);
        const bool bypassed = slot.isBypassed();
        ctx.cleanup ([&strip, &lane, &slot, index, bypassed]
        { if (index < 24) strip.unloadBuiltin(); else lane.unloadBuiltin (0); slot.setBypassed (bypassed); });
        std::string error;
        const bool loaded = index < 24 ? strip.loadBuiltin ("dusk.builtin.utility", error)
                                      : lane.loadBuiltin (0, "dusk.builtin.utility", error);
        if (! ctx.expect (loaded && slot.paramCount() == 4, "the matrix Utility did not load: " + error))
            return ctx.verdict();
        const auto* gain = slot.paramInfo (0);
        const auto* width = slot.paramInfo (2);
        if (! ctx.expect (gain != nullptr && width != nullptr && gain->id != nullptr && width->id != nullptr
                              && std::string (gain->id) == "gain_db" && std::string (width->id) == "width",
                          "the Utility parameter order changed")) return ctx.verdict();
        slot.setBypassed (false);
        const auto prefix = (index < 24 ? "t" + std::to_string (index) : "a" + std::to_string (index - 24));
        state->controls.emplace (prefix + ".unitBypass", Control {
            [&slot] { return slot.isBypassed() ? 1.0 : 0.0; },
            [&slot] (double value) { slot.setBypassed (value > 0.5); }, 0 });
        auto& legacy = index < 24 ? strip.getPluginSlot() : lane.getPluginSlot (0);
        const bool legacyBypassed = legacy.isBypassed();
        ctx.cleanup ([&legacy, legacyBypassed] { legacy.setBypassed (legacyBypassed); });
        legacy.setBypassed (false);
        state->controls.emplace (prefix + ".legacyBypass", Control {
            [&legacy] { return legacy.isBypassed() ? 1.0 : 0.0; },
            [&legacy] (double value) { legacy.setBypassed (value > 0.5); }, 0 });
        for (int parameter = 0; parameter < 4; ++parameter)
            state->controls.emplace (prefix + ".plugin" + std::to_string (parameter), Control {
                [&slot, parameter] { return static_cast<double> (slot.getParamValue (parameter)); },
                [&slot, parameter] (double value) { slot.setParamValue (parameter, static_cast<float> (value)); },
                slot.getParamValue (parameter) });
    }
    session.recomputeRtCounters();
    const std::array<Target, 4> targets { Target::TransportPlay, Target::TransportStop, Target::TransportRecord, Target::TransportToggle };
    const std::array<PendingTransportAction, 4> actions { PendingTransportAction::Play, PendingTransportAction::Stop,
                                                       PendingTransportAction::Record, PendingTransportAction::Toggle };
    for (std::size_t index = 0; index < targets.size(); ++index)
    {
        if (! release (ctx, *state)) return ctx.verdict();
        MidiBinding binding; binding.channel = 1; binding.dataNumber = 23; binding.target = targets[index];
        publish (session, binding); state->controls.at ("transport").expected = static_cast<int> (actions[index]);
        send (ctx, *state, 127); state->visited.insert (targets[index]);
        if (! state->check (ctx)) return ctx.verdict();
        state->set ("transport", static_cast<int> (PendingTransportAction::None));
    }
    buildOperations (*state);
    next (ctx, state);
    return std::nullopt;
}

const ScenarioRegistrar registrar { Scenario {
    "midi.every_target_reaches_only_its_control", { "midi", "session" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runMatrix (ctx); }, 120000
} };
}
}
