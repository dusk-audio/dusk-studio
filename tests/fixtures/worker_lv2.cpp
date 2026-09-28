#include <lv2/core/lv2.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

// LV2 Worker fixture. The output is the input times a gain only the worker
// produces: run() asks for it whenever the Target port changes, work() computes
// Target / 100, work_response() stages it and end_run() applies it, so no audio
// comes through until the whole round trip, end_run included, has happened.
//
// State stores the Target work() last committed, the way a DAF plug-in only
// takes a state change from its UI in work(): a request the host drops never
// reaches the state. restore() asks the worker again and applies a reply that
// arrives before restore() returns straight away - which a host only achieves by
// running the work synchronously - so the first block after a restore already
// carries the restored gain.
//
// work() aborts if it runs on a freed instance or twice at once for one
// instance, turning a host lifetime bug into a crash. The Delay port makes
// work() sleep for that many milliseconds, so a test can unload mid-request.
namespace
{
constexpr const char* kWorkerUri      = "urn:duskstudio:test:worker";
constexpr const char* kUnsupportedUri = "urn:duskstudio:test:worker-unsupported";
constexpr const char* kHardRtUri      = "urn:duskstudio:test:worker-hard-rt";
constexpr float       kDefaultTarget  = 50.0f;
constexpr const char* kTargetKey      = "urn:duskstudio:test:worker#target";

enum Port : uint32_t { InL, InR, OutL, OutR, TargetPort, DelayPort };

struct Request { float target; float delayMs; };
struct Reply   { float gain; };

std::mutex liveLock;
std::set<const void*> live;

bool isLive (const void* instance)
{
    const std::lock_guard<std::mutex> lock (liveLock);
    return live.count (instance) != 0;
}

struct Instance
{
    LV2_URID_Map* map = nullptr;
    const LV2_Worker_Schedule* schedule = nullptr;
    LV2_URID targetKey = 0, atomFloat = 0;
    const float* in[2] {};
    float* out[2] {};
    const float* target = nullptr;
    const float* delay = nullptr;

    float gain = 0.0f;
    float staged = 0.0f;
    bool hasStaged = false;
    float requested = NAN;   // the Target last handed to the worker
    bool restoring = false;
    std::atomic<int> working { 0 };
    std::atomic<float> committed { kDefaultTarget };   // the Target work() last took
};

template <typename T>
T* feature (const LV2_Feature* const* features, const char* uri)
{
    if (features == nullptr) return nullptr;
    for (size_t i = 0; features[i] != nullptr; ++i)
        if (std::strcmp (features[i]->URI, uri) == 0)
            return static_cast<T*> (features[i]->data);
    return nullptr;
}

LV2_Handle instantiate (const LV2_Descriptor*, double, const char*,
                        const LV2_Feature* const* features)
{
    auto* map = feature<LV2_URID_Map> (features, LV2_URID__map);
    const auto* schedule = feature<const LV2_Worker_Schedule> (features, LV2_WORKER__schedule);
    if (map == nullptr || schedule == nullptr) return nullptr;

    auto* self = new Instance;
    self->map = map;
    self->schedule = schedule;
    self->targetKey = map->map (map->handle, kTargetKey);
    self->atomFloat = map->map (map->handle, "http://lv2plug.in/ns/ext/atom#Float");
    const std::lock_guard<std::mutex> lock (liveLock);
    live.insert (self);
    return self;
}

void connectPort (LV2_Handle instance, uint32_t port, void* data)
{
    auto& self = *static_cast<Instance*> (instance);
    switch (port)
    {
        case InL:        self.in[0]  = static_cast<const float*> (data); break;
        case InR:        self.in[1]  = static_cast<const float*> (data); break;
        case OutL:       self.out[0] = static_cast<float*> (data); break;
        case OutR:       self.out[1] = static_cast<float*> (data); break;
        case TargetPort: self.target = static_cast<const float*> (data); break;
        case DelayPort:  self.delay  = static_cast<const float*> (data); break;
        default: break;
    }
}

void run (LV2_Handle instance, uint32_t frames)
{
    auto& self = *static_cast<Instance*> (instance);
    for (int c = 0; c < 2; ++c)
        if (self.out[c] != nullptr)
            for (uint32_t i = 0; i < frames; ++i)
                self.out[c][i] = (self.in[c] != nullptr ? self.in[c][i] : 0.0f) * self.gain;

    if (self.target == nullptr || *self.target == self.requested) return;
    const Request request { *self.target, self.delay != nullptr ? *self.delay : 0.0f };
    if (self.schedule->schedule_work (self.schedule->handle, sizeof (request), &request)
        == LV2_WORKER_SUCCESS)
        self.requested = request.target;
}

void cleanup (LV2_Handle instance)
{
    {
        const std::lock_guard<std::mutex> lock (liveLock);
        live.erase (instance);
    }
    delete static_cast<Instance*> (instance);
}

LV2_Worker_Status work (LV2_Handle instance, LV2_Worker_Respond_Function respond,
                        LV2_Worker_Respond_Handle handle, uint32_t size, const void* data)
{
    auto* self = static_cast<Instance*> (instance);
    if (! isLive (self) || self->working.fetch_add (1) != 0)
        std::abort();
    if (size != sizeof (Request))
    {
        self->working.fetch_sub (1);
        return LV2_WORKER_ERR_UNKNOWN;
    }
    Request request {};
    std::memcpy (&request, data, sizeof (request));
    if (request.delayMs > 0.0f)
        std::this_thread::sleep_for (std::chrono::milliseconds ((int) request.delayMs));
    const Reply reply { std::round (request.target) / 100.0f };
    self->committed.store (request.target);
    self->working.fetch_sub (1);
    if (! isLive (self))
        std::abort();
    return respond (handle, sizeof (reply), &reply);
}

LV2_Worker_Status workResponse (LV2_Handle instance, uint32_t size, const void* body)
{
    auto& self = *static_cast<Instance*> (instance);
    if (size != sizeof (Reply)) return LV2_WORKER_ERR_UNKNOWN;
    Reply reply {};
    std::memcpy (&reply, body, sizeof (reply));
    if (self.restoring)
    {
        self.gain = reply.gain;
    }
    else
    {
        self.staged = reply.gain;
        self.hasStaged = true;
    }
    return LV2_WORKER_SUCCESS;
}

LV2_Worker_Status endRun (LV2_Handle instance)
{
    auto& self = *static_cast<Instance*> (instance);
    if (self.hasStaged)
    {
        self.gain = self.staged;
        self.hasStaged = false;
    }
    return LV2_WORKER_SUCCESS;
}

LV2_State_Status save (LV2_Handle instance, LV2_State_Store_Function store,
                       LV2_State_Handle handle, uint32_t, const LV2_Feature* const*)
{
    auto& self = *static_cast<Instance*> (instance);
    const float value = self.committed.load();
    return store (handle, self.targetKey, &value, sizeof (value), self.atomFloat,
                  LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}

LV2_State_Status restore (LV2_Handle instance, LV2_State_Retrieve_Function retrieve,
                          LV2_State_Handle handle, uint32_t,
                          const LV2_Feature* const* features)
{
    auto& self = *static_cast<Instance*> (instance);
    size_t size = 0;
    uint32_t type = 0, flags = 0;
    const void* value = retrieve (handle, self.targetKey, &size, &type, &flags);
    if (value == nullptr || size != sizeof (float) || type != self.atomFloat)
        return LV2_STATE_ERR_NO_PROPERTY;

    const auto* schedule = feature<const LV2_Worker_Schedule> (features, LV2_WORKER__schedule);
    if (schedule == nullptr) schedule = self.schedule;
    Request request { *static_cast<const float*> (value), 0.0f };
    self.restoring = true;
    const auto status = schedule->schedule_work (schedule->handle, sizeof (request), &request);
    self.restoring = false;
    if (status == LV2_WORKER_SUCCESS)
        self.requested = request.target;
    return LV2_STATE_SUCCESS;
}

const void* extensionData (const char* uri)
{
    static const LV2_Worker_Interface worker { &work, &workResponse, &endRun };
    static const LV2_State_Interface state { &save, &restore };
    if (std::strcmp (uri, LV2_WORKER__interface) == 0) return &worker;
    if (std::strcmp (uri, LV2_STATE__interface) == 0) return &state;
    return nullptr;
}

const LV2_Descriptor workerDescriptor {
    kWorkerUri, &instantiate, &connectPort, nullptr, &run, nullptr, &cleanup, &extensionData
};
const LV2_Descriptor unsupportedDescriptor {
    kUnsupportedUri, &instantiate, &connectPort, nullptr, &run, nullptr, &cleanup, &extensionData
};
const LV2_Descriptor hardRtDescriptor {
    kHardRtUri, &instantiate, &connectPort, nullptr, &run, nullptr, &cleanup, &extensionData
};
} // namespace

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor (uint32_t index)
{
    if (index == 0) return &workerDescriptor;
    if (index == 1) return &unsupportedDescriptor;
    if (index == 2) return &hardRtDescriptor;
    return nullptr;
}
