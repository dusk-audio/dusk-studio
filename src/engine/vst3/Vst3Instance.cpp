#include "Vst3Instance.h"
#include "Vst3Bundle.h"
#include "Vst3HostContext.h"

#include <public.sdk/source/vst/hosting/connectionproxy.h>
#include <public.sdk/source/vst/hosting/eventlist.h>
#include <public.sdk/source/vst/hosting/module.h>
#include <public.sdk/source/vst/hosting/parameterchanges.h>
#include <public.sdk/source/vst/hosting/processdata.h>
#include <public.sdk/source/vst/utility/stringconvert.h>
#include <public.sdk/source/common/memorystream.h>

#include "../hosting/SpscRing.h"
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/vstspeaker.h>

#include <algorithm>
#include <cstring>

namespace duskstudio::vst3
{
using namespace Steinberg;

// Impl is the host-context Callbacks owner: editor param edits (performEdit) land
// here and join host-initiated setParamValue in one UI->RT ring; resize requests
// forward to whatever Vst3Editor is attached.
struct Vst3Instance::Impl : public Vst3HostContext::Callbacks
{
    Vst3HostContext host;

    IPtr<Vst::IComponent>      component;
    IPtr<Vst::IAudioProcessor> processor;
    IPtr<Vst::IEditController> controller;
    bool controllerIsComponent = false;   // single-component plugin

    // Keeps component<->controller messaging alive (presets, custom UIs).
    IPtr<Vst::ConnectionProxy> componentCP, controllerCP;

    hosting::PortLayout layout;
    int  mainInBus = -1, mainOutBus = -1, sidechainBus = -1;
    bool hasEventIn = false;

    bool   created = false, active = false, processing = false;
    double sampleRate = 0.0;
    int    maxFrames = 0;
    int    latencySamples = 0;

    // Audio-thread process plumbing, sized in activate(). HostProcessData owns
    // the AudioBusBuffers arrays; per block we only re-point channel buffers.
    Vst::HostProcessData     processData;
    Vst::ProcessContext      processContext {};
    Vst::ParameterChanges    inParams, outParams;
    Vst::EventList           inEvents, outEvents;
    std::vector<float>       silence;                 // shared silent input scratch
    std::vector<float>       sink;                    // shared output sink scratch
    std::vector<float*>      scratchPtrs;             // per-channel pointer scratch

    // Parameters: snapshot at create(); UI->RT ring drained into inParams each block.
    std::vector<ParamInfo> params;
    struct ParamChange { uint32_t id; double value; };
    hosting::SpscRing<ParamChange, 512> paramRing;

    std::function<bool (int, int)> resizeViewFn;

    // Last param id the plugin's editor touched (performEdit), for MIDI Learn.
    std::atomic<int64_t> lastTouchedParamId { -1 };

    // kLatencyChanged arrived; the engine's drain timer consumes it and cycles
    // the instance (VST3 latency only re-reads across a setActive cycle).
    std::atomic<bool> latencyChanged { false };
    // kParamTitlesChanged arrived; the drain timer re-snapshots on the message
    // thread (misbehaving plugins fire restartComponent off-thread, and the
    // snapshot allocates).
    std::atomic<bool> paramInfoChanged { false };
    std::atomic<bool> ccMapChanged     { false };

    // MIDI CC -> parameter bridge (VST3 has no CC events: IMidiMapping assigns a
    // parameter per (channel, controller) and the host feeds value changes).
    // Cached on the message thread - getMidiControllerAssignment is a controller
    // call - and read lock-free by the audio thread per incoming CC.
    static constexpr uint32_t kNoCcParam = 0xFFFFFFFFu;
    std::array<std::atomic<uint32_t>, 16 * Vst::kCountCtrlNumber> ccParamId {};

    void rebuildMidiCcMap()
    {
        FUnknownPtr<Vst::IMidiMapping> mapping (controller);
        for (int16 ch = 0; ch < 16; ++ch)
            for (int16 ctrl = 0; ctrl < Vst::kCountCtrlNumber; ++ctrl)
            {
                Vst::ParamID id = 0;
                const bool mapped = mapping
                    && mapping->getMidiControllerAssignment (0, ch,
                           (Vst::CtrlNumber) ctrl, id) == kResultTrue;
                ccParamId[(size_t) (ch * Vst::kCountCtrlNumber + ctrl)]
                    .store (mapped ? (uint32_t) id : kNoCcParam,
                            std::memory_order_relaxed);
            }
    }

    void snapshotParams()
    {
        params.clear();
        if (! controller) return;
        const int32 numParams = controller->getParameterCount();
        params.reserve ((size_t) std::max<int32> (0, numParams));
        for (int32 i = 0; i < numParams; ++i)
        {
            Vst::ParameterInfo pi {};
            if (controller->getParameterInfo (i, pi) != kResultOk)
                continue;
            ParamInfo p;
            p.id           = (uint32_t) pi.id;
            p.name         = VST3::StringConvert::convert (pi.title);
            p.defaultValue = pi.defaultNormalizedValue;
            p.stepCount    = (int) pi.stepCount;
            p.canAutomate  = (pi.flags & Vst::ParameterInfo::kCanAutomate) != 0;
            p.isReadOnly   = (pi.flags & Vst::ParameterInfo::kIsReadOnly)  != 0;
            params.push_back (std::move (p));
        }
    }

    // Vst3HostContext::Callbacks (message thread)
    void onPerformEdit (uint32_t paramId, double normalised) override
    {
        // The controller already holds the new value (the edit came from its own
        // editor); the processor hears it through the ring -> IParameterChanges.
        paramRing.push ({ paramId, normalised });
        lastTouchedParamId.store ((int64_t) paramId, std::memory_order_relaxed);
    }
    bool onResizeView (int w, int h) override
    {
        return resizeViewFn != nullptr && resizeViewFn (w, h);
    }
    void onRestartComponent (int32_t flags) override
    {
        // Flag-only: values need no action (nothing caches them), param-info and
        // latency changes are consumed by the engine's message-thread drain
        // timer (the snapshot allocates and the latency pickup needs a fenced
        // setActive cycle - and misbehaving plugins call this off-thread).
        // kIoChanged (a live bus re-layout) stays unhandled: the PortLayout is
        // fixed at create() and no hosted effect exercises it yet.
        if ((flags & Vst::RestartFlags::kParamTitlesChanged) != 0)
            paramInfoChanged.store (true, std::memory_order_relaxed);
        if ((flags & Vst::RestartFlags::kMidiCCAssignmentChanged) != 0)
            ccMapChanged.store (true, std::memory_order_relaxed);
        if ((flags & Vst::RestartFlags::kLatencyChanged) != 0)
            latencyChanged.store (true, std::memory_order_relaxed);
    }

    void destroy()
    {
        if (processor && processing) processor->setProcessing (false);
        if (component && active)     component->setActive (false);
        processing = false;
        active = false;

        if (componentCP && controller)  componentCP->disconnect (nullptr);
        if (controllerCP && component)  controllerCP->disconnect (nullptr);
        componentCP  = nullptr;
        controllerCP = nullptr;

        host.setCallbacks (nullptr);
        params.clear();
        resizeViewFn = nullptr;
        if (controller && ! controllerIsComponent)
            controller->terminate();
        controller = nullptr;
        processor = nullptr;
        if (component)
            component->terminate();
        component = nullptr;
        created = false;
        processData.unprepare();
    }
};

Vst3Instance::Vst3Instance() : impl (std::make_unique<Impl>()) {}
Vst3Instance::~Vst3Instance() { impl->destroy(); }

const hosting::PortLayout& Vst3Instance::portLayout() const noexcept { return impl->layout; }
bool Vst3Instance::isActive() const noexcept { return impl->active; }
int  Vst3Instance::getLatencySamples() const noexcept { return impl->latencySamples; }
Vst3HostContext&       Vst3Instance::getHost()       noexcept { return impl->host; }
const Vst3HostContext& Vst3Instance::getHost() const noexcept { return impl->host; }
void* Vst3Instance::editController() const noexcept { return impl->controller.get(); }

int Vst3Instance::paramCount() const noexcept { return (int) impl->params.size(); }

const Vst3Instance::ParamInfo* Vst3Instance::paramInfo (int index) const noexcept
{
    return (index >= 0 && index < (int) impl->params.size())
             ? &impl->params[(size_t) index] : nullptr;
}

bool Vst3Instance::getParamValue (uint32_t id, double& out) const
{
    if (! impl->controller) return false;
    for (const auto& p : impl->params)
        if (p.id == id)
        {
            out = impl->controller->getParamNormalized ((Vst::ParamID) id);
            return true;
        }
    return false;
}

bool Vst3Instance::paramValueToText (uint32_t id, double value, std::string& out) const
{
    if (! impl->controller) return false;
    Vst::String128 text {};
    if (impl->controller->getParamStringByValue ((Vst::ParamID) id, value, text) != kResultOk)
        return false;
    out = VST3::StringConvert::convert (text);
    return true;
}

void Vst3Instance::setParamValue (uint32_t id, double value) noexcept
{
    // Controller first so an open editor tracks the move; processor hears it
    // through the ring -> IParameterChanges on the next block.
    if (impl->controller)
        impl->controller->setParamNormalized ((Vst::ParamID) id, value);
    impl->paramRing.push ({ id, value });   // full => drop (pathological flood only)
}

void Vst3Instance::setResizeViewHandler (std::function<bool (int, int)> fn)
{
    impl->resizeViewFn = std::move (fn);
}

bool Vst3Instance::consumeLatencyChanged() noexcept
{
    return impl->latencyChanged.exchange (false, std::memory_order_relaxed);
}

void Vst3Instance::refreshParamInfoIfChanged()
{
    if (impl->paramInfoChanged.exchange (false, std::memory_order_relaxed))
        impl->snapshotParams();
    if (impl->ccMapChanged.exchange (false, std::memory_order_relaxed))
        impl->rebuildMidiCcMap();
}

int Vst3Instance::midiCcMappingCount() const noexcept
{
    int n = 0;
    for (const auto& id : impl->ccParamId)
        if (id.load (std::memory_order_relaxed) != Impl::kNoCcParam)
            ++n;
    return n;
}

int Vst3Instance::lastTouchedParamIndex() const noexcept
{
    const auto id = impl->lastTouchedParamId.load (std::memory_order_relaxed);
    if (id < 0) return -1;
    for (size_t i = 0; i < impl->params.size(); ++i)
        if ((int64_t) impl->params[i].id == id) return (int) i;
    return -1;
}

bool Vst3Instance::create (const Vst3Bundle& bundle, const std::string& classId, std::string& errorOut)
{
    impl->destroy();

    auto* module = static_cast<VST3::Hosting::Module*> (bundle.module());
    if (module == nullptr) { errorOut = "bundle not loaded"; return false; }

    const auto uid = VST3::UID::fromString (classId);
    if (! uid) { errorOut = "malformed class id: " + classId; return false; }

    auto* hostApp = static_cast<Vst::IHostApplication*> (impl->host.hostApplication());

    const auto factory = module->getFactory();
    impl->component = factory.createInstance<Vst::IComponent> (*uid);
    if (! impl->component) { errorOut = "createInstance(IComponent) failed"; return false; }
    if (impl->component->initialize (hostApp) != kResultOk)
    { errorOut = "component initialize() failed"; impl->component = nullptr; return false; }
    impl->created = true;   // terminate() is owed from here on

    impl->processor = FUnknownPtr<Vst::IAudioProcessor> (impl->component);
    if (! impl->processor)
    { errorOut = "plugin has no IAudioProcessor"; impl->destroy(); return false; }
    if (impl->processor->canProcessSampleSize (Vst::kSample32) != kResultTrue)
    { errorOut = "plugin cannot process 32-bit float"; impl->destroy(); return false; }

    // Controller: separate class (created + connected) or the component itself.
    TUID controllerCid {};
    if (impl->component->getControllerClassId (controllerCid) == kResultTrue)
    {
        impl->controller = factory.createInstance<Vst::IEditController> (VST3::UID (controllerCid));
        if (impl->controller && impl->controller->initialize (hostApp) != kResultOk)
            impl->controller = nullptr;
    }
    if (! impl->controller)
    {
        impl->controller = FUnknownPtr<Vst::IEditController> (impl->component);
        impl->controllerIsComponent = impl->controller != nullptr;
    }
    if (impl->controller)
    {
        impl->controller->setComponentHandler (
            static_cast<Vst::IComponentHandler*> (impl->host.componentHandler()));

        // Wire the private component<->controller message path, then hand the
        // controller the component's initial state so both sides agree.
        FUnknownPtr<Vst::IConnectionPoint> compICP (impl->component);
        FUnknownPtr<Vst::IConnectionPoint> ctrlICP (impl->controller);
        if (compICP && ctrlICP && ! impl->controllerIsComponent)
        {
            impl->componentCP  = owned (new Vst::ConnectionProxy (compICP));
            impl->controllerCP = owned (new Vst::ConnectionProxy (ctrlICP));
            impl->componentCP->connect (ctrlICP);
            impl->controllerCP->connect (compICP);
        }
        MemoryStream stream;
        if (impl->component->getState (&stream) == kResultOk)
        {
            stream.seek (0, IBStream::kIBSeekSet, nullptr);
            impl->controller->setComponentState (&stream);
        }

        impl->snapshotParams();
        impl->rebuildMidiCcMap();
    }
    impl->host.setCallbacks (impl.get());

    // Bus discovery -> PortLayout. The InsertAdapter folds whatever the plugin
    // really has onto the stereo insert, so we take the default arrangements
    // as-is instead of forcing stereo.
    impl->layout = {};
    impl->mainInBus = impl->mainOutBus = impl->sidechainBus = -1;

    const auto busChannels = [this] (Vst::BusDirection dir, int32 index) -> int
    {
        Vst::SpeakerArrangement arr = 0;
        if (impl->processor->getBusArrangement (dir, index, arr) == kResultOk)
            return (int) Vst::SpeakerArr::getChannelCount (arr);
        return 0;
    };

    const int32 numIn = impl->component->getBusCount (Vst::kAudio, Vst::kInput);
    for (int32 i = 0; i < numIn; ++i)
    {
        Vst::BusInfo info {};
        if (impl->component->getBusInfo (Vst::kAudio, Vst::kInput, i, info) != kResultOk)
            continue;
        hosting::BusInfo b;
        b.kind = hosting::BusInfo::Kind::Audio;
        b.dir  = hosting::BusInfo::Direction::Input;
        b.channelCount = busChannels (Vst::kInput, i);
        b.active = false;
        if (info.busType == Vst::kMain && impl->mainInBus < 0)
        {
            b.role = hosting::BusInfo::Role::Main;
            b.active = true;
            impl->mainInBus = (int) i;
            impl->layout.mainInIndex = (int) impl->layout.inputs.size();
        }
        else if (info.busType == Vst::kAux && impl->sidechainBus < 0)
        {
            b.role = hosting::BusInfo::Role::Sidechain;
            b.active = true;
            impl->sidechainBus = (int) i;
            impl->layout.sidechainInIndex = (int) impl->layout.inputs.size();
        }
        impl->layout.inputs.push_back (b);
    }

    const int32 numOut = impl->component->getBusCount (Vst::kAudio, Vst::kOutput);
    for (int32 i = 0; i < numOut; ++i)
    {
        Vst::BusInfo info {};
        if (impl->component->getBusInfo (Vst::kAudio, Vst::kOutput, i, info) != kResultOk)
            continue;
        hosting::BusInfo b;
        b.kind = hosting::BusInfo::Kind::Audio;
        b.dir  = hosting::BusInfo::Direction::Output;
        b.channelCount = busChannels (Vst::kOutput, i);
        if (info.busType == Vst::kMain && impl->mainOutBus < 0)
        {
            b.role = hosting::BusInfo::Role::Main;
            b.active = true;
            impl->mainOutBus = (int) i;
            impl->layout.mainOutIndex = (int) impl->layout.outputs.size();
        }
        impl->layout.outputs.push_back (b);
    }

    impl->hasEventIn = impl->component->getBusCount (Vst::kEvent, Vst::kInput) > 0;
    if (impl->hasEventIn)
    {
        hosting::BusInfo ev;
        ev.kind = hosting::BusInfo::Kind::Event;
        ev.dir  = hosting::BusInfo::Direction::Input;
        ev.carriesMidi = true;
        ev.active = true;
        ev.name = "Events";
        impl->layout.eventInIndex = (int) impl->layout.inputs.size();
        impl->layout.inputs.push_back (ev);
    }
    impl->layout.isInstrument = impl->mainInBus < 0 && impl->hasEventIn && impl->mainOutBus >= 0;

    if (impl->mainOutBus < 0)
    { errorOut = "plugin has no main audio output bus"; impl->destroy(); return false; }

    // Activate exactly the buses we drive; leave the rest off so the plugin
    // doesn't expect data on them.
    for (int32 i = 0; i < numIn; ++i)
        impl->component->activateBus (Vst::kAudio, Vst::kInput, i,
                                      i == impl->mainInBus || i == impl->sidechainBus);
    for (int32 i = 0; i < numOut; ++i)
        impl->component->activateBus (Vst::kAudio, Vst::kOutput, i, i == impl->mainOutBus);
    if (impl->hasEventIn)
        impl->component->activateBus (Vst::kEvent, Vst::kInput, 0, true);

    return true;
}

bool Vst3Instance::activate (double sampleRate, int maxBlockFrames, std::string& errorOut)
{
    if (! impl->created) { errorOut = "not created"; return false; }
    if (impl->active) return true;

    impl->sampleRate = sampleRate;
    impl->maxFrames  = std::max (1, maxBlockFrames);

    Vst::ProcessSetup setup {};
    setup.processMode        = Vst::kRealtime;
    setup.symbolicSampleSize = Vst::kSample32;
    setup.maxSamplesPerBlock = impl->maxFrames;
    setup.sampleRate         = sampleRate;
    if (impl->processor->setupProcessing (setup) != kResultOk)
    { errorOut = "setupProcessing() failed"; return false; }

    // HostProcessData builds the AudioBusBuffers arrays from the component's bus
    // config. bufferSamples MUST be 0: a non-zero count makes it allocate and OWN
    // the channel buffers, and setChannelBuffers then silently refuses to point
    // at ours - the plugin would process the SDK's private (silent) scratch.
    if (! impl->processData.prepare (*impl->component, 0, Vst::kSample32))
    { errorOut = "process-data prepare failed"; return false; }
    impl->processData.inputParameterChanges  = &impl->inParams;
    impl->processData.outputParameterChanges = &impl->outParams;
    // Pre-warm the queue pool; per-queue point storage keeps its capacity across
    // clearQueue, so the drain below is allocation-free at steady state.
    impl->inParams.setMaxParameters ((int32) impl->params.size());
    impl->processData.inputEvents  = &impl->inEvents;
    impl->processData.outputEvents = &impl->outEvents;
    impl->processData.processContext = &impl->processContext;
    impl->processContext = {};
    impl->processContext.sampleRate = sampleRate;

    // Shared scratch: silent feed for input channels the caller doesn't supply,
    // sink for output channels beyond the fold. Pointer scratch sized to the
    // widest bus so per-block assembly never allocates.
    impl->silence.assign ((size_t) impl->maxFrames, 0.0f);
    impl->sink.assign ((size_t) impl->maxFrames, 0.0f);
    int widest = 2;
    for (const auto& b : impl->layout.inputs)  widest = std::max (widest, b.channelCount);
    for (const auto& b : impl->layout.outputs) widest = std::max (widest, b.channelCount);
    impl->scratchPtrs.assign ((size_t) widest, nullptr);

    if (impl->component->setActive (true) != kResultOk)
    { errorOut = "setActive(true) failed"; return false; }

    impl->latencySamples = (int) impl->processor->getLatencySamples();

    impl->processor->setProcessing (true);
    impl->processing = true;
    impl->active = true;
    return true;
}

void Vst3Instance::deactivate()
{
    if (! impl->created) return;
    if (impl->processing) { impl->processor->setProcessing (false); impl->processing = false; }
    if (impl->active)     { impl->component->setActive (false);     impl->active = false; }
}

bool Vst3Instance::reactivate (double sampleRate, int maxBlockFrames, std::string& errorOut)
{
    // VST3 renegotiates rate/block through setupProcessing on the SAME instance,
    // so state and any open editor survive without a re-instantiate.
    deactivate();
    return activate (sampleRate, maxBlockFrames, errorOut);
}

void Vst3Instance::processBlock (const hosting::PortBuffers& io) noexcept
{
    const int numFrames = io.numFrames;

    auto clearOutputs = [&]
    {
        if (io.mainOut == nullptr || numFrames <= 0) return;
        for (int c = 0; c < io.mainOutChannels; ++c)
            if (io.mainOut[c] != nullptr)
                std::memset (io.mainOut[c], 0, sizeof (float) * (size_t) numFrames);
    };

    if (! impl->active || ! impl->processing
        || numFrames <= 0 || numFrames > impl->maxFrames
        || io.mainOut == nullptr || io.mainOutChannels <= 0)
    {
        clearOutputs();
        return;
    }

    // Main input bus: caller channels, silence for any the caller lacks.
    if (impl->mainInBus >= 0)
    {
        const int busCh = impl->layout.inputs[(size_t) impl->layout.mainInIndex].channelCount;
        for (int c = 0; c < busCh; ++c)
            impl->scratchPtrs[(size_t) c] =
                (io.mainIn != nullptr && c < io.mainInChannels && io.mainIn[c] != nullptr)
                    ? io.mainIn[c] : impl->silence.data();
        impl->processData.setChannelBuffers (Vst::kInput, impl->mainInBus,
                                             impl->scratchPtrs.data(), busCh);
    }
    if (impl->sidechainBus >= 0)
    {
        const int busCh = impl->layout.inputs[(size_t) impl->layout.sidechainInIndex].channelCount;
        for (int c = 0; c < busCh; ++c)
            impl->scratchPtrs[(size_t) c] =
                (io.sidechainIn != nullptr && c < io.sidechainInChannels
                 && io.sidechainIn[c] != nullptr)
                    ? io.sidechainIn[c] : impl->silence.data();
        impl->processData.setChannelBuffers (Vst::kInput, impl->sidechainBus,
                                             impl->scratchPtrs.data(), busCh);
    }
    if (impl->mainOutBus >= 0)
    {
        const int busCh = impl->layout.outputs[(size_t) impl->layout.mainOutIndex].channelCount;
        for (int c = 0; c < busCh; ++c)
            impl->scratchPtrs[(size_t) c] =
                (c < io.mainOutChannels && io.mainOut[c] != nullptr)
                    ? io.mainOut[c] : impl->sink.data();
        impl->processData.setChannelBuffers (Vst::kOutput, impl->mainOutBus,
                                             impl->scratchPtrs.data(), busCh);
    }

    impl->processData.numSamples = numFrames;
    impl->inEvents.clear();
    impl->outEvents.clear();
    impl->inParams.clearQueue();
    impl->outParams.clearQueue();

    // Queued UI/editor parameter changes -> this block's IParameterChanges.
    // All at offset 0 - sample-accurate automation is a later step.
    impl->paramRing.drain ([&] (const Impl::ParamChange& pc)
    {
        int32 queueIndex = 0;
        if (auto* queue = impl->inParams.addParameterData ((Vst::ParamID) pc.id, queueIndex))
        {
            int32 pointIndex = 0;
            queue->addPoint (0, pc.value, pointIndex);
        }
    });

    // Notes -> the event bus; CCs / pitch bend / channel pressure -> the
    // IMidiMapping-assigned parameters (VST3 has no CC events by design).
    if (io.midiIn != nullptr)
    {
        auto queueCcParam = [&] (int16 channel, int16 ctrl, double normalized, int32 offset)
        {
            const uint32_t id = impl->ccParamId[(size_t) (channel * Vst::kCountCtrlNumber
                                                          + ctrl)]
                                    .load (std::memory_order_relaxed);
            if (id == Impl::kNoCcParam) return;
            int32 queueIndex = 0;
            if (auto* queue = impl->inParams.addParameterData ((Vst::ParamID) id, queueIndex))
            {
                int32 pointIndex = 0;
                queue->addPoint (offset, normalized, pointIndex);
            }
        };

        for (const auto meta : *io.midiIn)
        {
            const auto* d = meta.data;
            if (meta.numBytes < 2) continue;
            const auto status  = (uint8_t) (d[0] & 0xF0u);
            const auto channel = (int16_t) (d[0] & 0x0Fu);

            if (impl->hasEventIn && meta.numBytes >= 3
                && (status == 0x90 || status == 0x80))
            {
                Vst::Event ev {};
                ev.busIndex     = 0;
                ev.sampleOffset = meta.samplePosition;
                ev.flags        = Vst::Event::kIsLive;
                if (status == 0x90 && d[2] > 0)
                {
                    ev.type   = Vst::Event::kNoteOnEvent;
                    ev.noteOn = { channel, (int16_t) d[1], 0.0f,
                                  (float) d[2] / 127.0f, 0, -1 };
                }
                else
                {
                    ev.type    = Vst::Event::kNoteOffEvent;
                    ev.noteOff = { channel, (int16_t) d[1],
                                   (float) d[2] / 127.0f, -1, 0.0f };
                }
                impl->inEvents.addEvent (ev);
            }
            else if (status == 0xB0 && meta.numBytes >= 3 && d[1] < 128)
            {
                queueCcParam (channel, (int16_t) d[1],
                              (double) d[2] / 127.0, meta.samplePosition);
            }
            else if (status == 0xE0 && meta.numBytes >= 3)
            {
                const int bend14 = (int) d[1] | ((int) d[2] << 7);
                queueCcParam (channel, Vst::kPitchBend,
                              (double) bend14 / 16383.0, meta.samplePosition);
            }
            else if (status == 0xD0)
            {
                queueCcParam (channel, Vst::kAfterTouch,
                              (double) d[1] / 127.0, meta.samplePosition);
            }
        }
    }

    if (impl->processor->process (impl->processData) != kResultOk)
        clearOutputs();
}

// State blob: "DV31" magic, then u32-LE-length-prefixed component and
// controller streams. VST3 state is dual-stream - persisting only the
// component stream loses controller-private data (UI prefs, custom curves).
namespace
{
constexpr uint8_t kStateMagic[4] = { 'D', 'V', '3', '1' };

void appendU32 (std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back ((uint8_t) (x & 0xffu));
    v.push_back ((uint8_t) ((x >> 8) & 0xffu));
    v.push_back ((uint8_t) ((x >> 16) & 0xffu));
    v.push_back ((uint8_t) ((x >> 24) & 0xffu));
}

bool readU32 (const std::vector<uint8_t>& v, size_t& pos, uint32_t& out)
{
    if (v.size() - pos < 4) return false;
    out = (uint32_t) v[pos]
        | ((uint32_t) v[pos + 1] << 8)
        | ((uint32_t) v[pos + 2] << 16)
        | ((uint32_t) v[pos + 3] << 24);
    pos += 4;
    return true;
}

void appendStream (std::vector<uint8_t>& v, MemoryStream& stream)
{
    const auto size = (uint32_t) std::max<Steinberg::TSize> (0, stream.getSize());
    appendU32 (v, size);
    if (size > 0)
    {
        const auto* data = reinterpret_cast<const uint8_t*> (stream.getData());
        v.insert (v.end(), data, data + size);
    }
}
} // namespace

bool Vst3Instance::saveState (std::vector<uint8_t>& out) const
{
    if (! impl->created) return false;

    MemoryStream compStream;
    if (impl->component->getState (&compStream) != kResultOk)
        return false;

    MemoryStream ctrlStream;
    if (! (impl->controller && impl->controller->getState (&ctrlStream) == kResultOk))
        ctrlStream.setSize (0);

    out.clear();
    out.insert (out.end(), std::begin (kStateMagic), std::end (kStateMagic));
    appendStream (out, compStream);
    appendStream (out, ctrlStream);
    return true;
}

bool Vst3Instance::loadState (const std::vector<uint8_t>& in)
{
    if (! impl->created) return false;
    if (in.size() < 12 || std::memcmp (in.data(), kStateMagic, sizeof (kStateMagic)) != 0)
        return false;

    size_t pos = sizeof (kStateMagic);
    uint32_t compSize = 0, ctrlSize = 0;
    if (! readU32 (in, pos, compSize) || in.size() - pos < compSize) return false;
    const size_t compPos = pos;
    pos += compSize;
    if (! readU32 (in, pos, ctrlSize) || in.size() - pos < ctrlSize) return false;
    const size_t ctrlPos = pos;

    MemoryStream compStream (const_cast<uint8_t*> (in.data() + compPos), (TSize) compSize);
    const bool componentOk = impl->component->setState (&compStream) == kResultOk;

    if (impl->controller)
    {
        // Same order a preset load uses: mirror the component state into the
        // controller first, then apply the controller's own stream on top.
        compStream.seek (0, IBStream::kIBSeekSet, nullptr);
        impl->controller->setComponentState (&compStream);
        if (ctrlSize > 0)
        {
            MemoryStream ctrlStream (const_cast<uint8_t*> (in.data() + ctrlPos), (TSize) ctrlSize);
            impl->controller->setState (&ctrlStream);
        }
    }
    return componentOk;
}
} // namespace duskstudio::vst3
