// The host side of CLAP's state and params extensions, and the parameter flush
// that lets a Save capture writes still queued for the audio thread.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/clap/ClapBundle.h"
#include "engine/clap/ClapInstance.h"
#include "engine/clap/NativeClapSlot.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
using Catch::Matchers::WithinAbs;
using duskstudio::clap::ClapBundle;
using duskstudio::clap::ClapInstance;
using duskstudio::clap::NativeClapSlot;
using namespace std::chrono_literals;

constexpr clap_id kAlpha = 40;
constexpr clap_id kBeta  = 41;

// Same layouts as tests/fixtures/param_touch_clap.cpp.
struct HostProbe
{
    std::uint32_t processCalls = 0;
    std::uint32_t flushCalls = 0;
    bool processOnAudioThread = false;
    bool flushOnAudioThread = false;
    bool flushOnMainThread = false;
    bool overlapped = false;
    const void* lastCookie = nullptr;
};

struct HostControl
{
    bool (CLAP_ABI *markDirty) (const clap_plugin_t*);
    bool (CLAP_ABI *requestFlush) (const clap_plugin_t*);
    bool (CLAP_ABI *rescan) (const clap_plugin_t*, std::uint32_t flags);
    bool (CLAP_ABI *clear) (const clap_plugin_t*, clap_id id, std::uint32_t flags);
    bool (CLAP_ABI *editFromGui) (const clap_plugin_t*, clap_id id, double value);
    void (CLAP_ABI *reenter) (const clap_plugin_t*, void (*fn) (void*), void* context);
    void (CLAP_ABI *markDirtyOnDestroy) (const clap_plugin_t*);
    const HostProbe* (CLAP_ABI *probe) (const clap_plugin_t*);
};

const HostControl& hostControl (const clap_plugin_t* plugin)
{
    const auto* control = static_cast<const HostControl*> (
        plugin->get_extension (plugin, "studio.dusk.test.param-touch-host"));
    REQUIRE (control != nullptr);
    return *control;
}

std::array<double, 2> savedValues (const std::vector<std::uint8_t>& blob)
{
    REQUIRE (blob.size() == 20);
    std::array<double, 2> values {};
    std::memcpy (values.data(), blob.data() + 4, sizeof (values));
    return values;
}

struct Fixture
{
    Fixture()
    {
        REQUIRE (bundle.load (DUSKSTUDIO_PARAM_TOUCH_CLAP_FIXTURE_PATH, error));
        REQUIRE (bundle.plugins().size() == 1);
        instance = std::make_unique<ClapInstance>();
        REQUIRE (instance->create (bundle, bundle.plugins().front().id, error));
        REQUIRE (instance->activate (48000.0, 64, error));
        plugin = instance->getPlugin();
        control = &hostControl (plugin);
    }
    const HostProbe& probe() const { return *control->probe (plugin); }
    void process() noexcept
    { instance->processStereo (left.data(), right.data(), left.data(), right.data(), 64); }
    double value (clap_id id)
    {
        double result = -1.0;
        REQUIRE (instance->getParamValue (id, result));
        return result;
    }
    bool callerIsAudioThread()
    {
        const auto* host = instance->getHost().get();
        const auto* check = static_cast<const clap_host_thread_check_t*> (
            host->get_extension (host, CLAP_EXT_THREAD_CHECK));
        REQUIRE (check != nullptr);
        return check->is_audio_thread (host);
    }

    ClapBundle bundle;
    std::unique_ptr<ClapInstance> instance;
    const clap_plugin_t* plugin = nullptr;
    const HostControl* control = nullptr;
    std::string error;
    std::array<float, 64> left {}, right {};
};
} // namespace

TEST_CASE ("CLAP host takes mark_dirty and value rescans as persistent state changes",
           "[clap][host][state]")
{
    Fixture f;
    const auto initial = f.instance->stateRevision();
    REQUIRE (f.control->markDirty (f.plugin));
    const auto dirty = f.instance->stateRevision();
    CHECK (dirty > initial);

    REQUIRE (f.control->rescan (f.plugin, CLAP_PARAM_RESCAN_TEXT | CLAP_PARAM_RESCAN_INFO));
    REQUIRE (f.control->clear (f.plugin, kAlpha, CLAP_PARAM_CLEAR_ALL));
    CHECK (f.instance->stateRevision() == dirty);

    REQUIRE (f.control->rescan (f.plugin, CLAP_PARAM_RESCAN_VALUES));
    const auto rescanned = f.instance->stateRevision();
    CHECK (rescanned > dirty);

    std::vector<std::uint8_t> blob;
    REQUIRE (f.instance->saveState (blob));
    REQUIRE (f.instance->reactivate (96000.0, 32, f.error));
    CHECK (f.instance->stateRevision() == rescanned);

    // The plug-in may still reach its host from inside destroy(); the host is
    // the instance's first member, so that lands on live storage.
    f.control->markDirtyOnDestroy (f.plugin);
    f.instance.reset();
}

TEST_CASE ("CLAP host stops handing back cookies once the plug-in rescans everything",
           "[clap][host][params]")
{
    Fixture f;
    f.instance->setParamValue (kAlpha, 0.25);
    f.process();
    CHECK (f.probe().lastCookie != nullptr);

    // Queued before the rescan, delivered after it: its cookie is void too.
    f.instance->setParamValue (kBeta, 0.75);
    f.instance->deactivate();
    REQUIRE (f.control->rescan (f.plugin, CLAP_PARAM_RESCAN_ALL));
    REQUIRE (f.instance->activate (48000.0, 64, f.error));
    f.process();
    CHECK (f.probe().lastCookie == nullptr);
    CHECK_THAT (f.value (kBeta), WithinAbs (0.75, 1e-12));

    f.instance->setParamValue (kAlpha, 0.5);
    f.process();
    CHECK (f.probe().lastCookie == nullptr);
    CHECK_THAT (f.value (kAlpha), WithinAbs (0.5, 1e-12));
}

TEST_CASE ("ClapInstance flushes queued host writes outside process() as the plug-in's audio thread",
           "[clap][instance][params]")
{
    Fixture f;
    f.instance->setParamValue (kAlpha, 0.25);
    const auto queued = f.instance->stateRevision();
    REQUIRE (f.instance->flushParamsOutsideProcess());
    CHECK (f.probe().flushCalls == 1);
    CHECK (f.probe().processCalls == 0);
    CHECK (f.probe().flushOnAudioThread);
    CHECK (f.probe().flushOnMainThread);
    CHECK_FALSE (f.callerIsAudioThread());
    CHECK_THAT (f.value (kAlpha), WithinAbs (0.25, 1e-12));
    CHECK (f.instance->stateRevision() > queued);

    REQUIRE (f.instance->flushParamsOutsideProcess());
    CHECK (f.probe().flushCalls == 1);

    SECTION ("an inactive plug-in takes the flush on the main thread")
    {
        f.instance->deactivate();
        f.instance->setParamValue (kBeta, 0.5);
        REQUIRE (f.instance->flushParamsOutsideProcess());
        CHECK (f.probe().flushCalls == 2);
        CHECK_FALSE (f.probe().flushOnAudioThread);
        CHECK (f.probe().flushOnMainThread);
        CHECK_THAT (f.value (kBeta), WithinAbs (0.5, 1e-12));
    }

    SECTION ("a flush and a block never run inside one another")
    {
        struct Nested
        {
            ClapInstance* instance = nullptr;
            bool flushed = true;
            std::array<float, 64> left {}, right {};
        } nested;
        nested.instance = f.instance.get();

        f.instance->setParamValue (kAlpha, 0.75);
        f.control->reenter (f.plugin, [] (void* context)
        {
            auto& n = *static_cast<Nested*> (context);
            n.flushed = n.instance->flushParamsOutsideProcess();
        }, &nested);
        f.process();
        CHECK_FALSE (nested.flushed);
        CHECK (f.probe().flushCalls == 1);
        CHECK (f.probe().processCalls == 1);
        CHECK_THAT (f.value (kAlpha), WithinAbs (0.75, 1e-12));

        f.instance->setParamValue (kBeta, 0.125);
        f.control->reenter (f.plugin, [] (void* context)
        {
            auto& n = *static_cast<Nested*> (context);
            n.left.fill (1.0f);
            n.right.fill (1.0f);
            n.instance->processStereo (n.left.data(), n.right.data(),
                                       n.left.data(), n.right.data(), 64);
        }, &nested);
        REQUIRE (f.instance->flushParamsOutsideProcess());
        CHECK (f.probe().flushCalls == 2);
        CHECK (f.probe().processCalls == 1);
        CHECK_THAT (nested.left[0], WithinAbs (0.0, 1e-9));
        CHECK_THAT (nested.right[63], WithinAbs (0.0, 1e-9));
        CHECK_THAT (f.value (kBeta), WithinAbs (0.125, 1e-12));
    }

    CHECK_FALSE (f.probe().overlapped);
}

TEST_CASE ("ClapInstance answers request_flush with process() when it runs and flush() when it idles",
           "[clap][instance][params]")
{
    Fixture f;
    const auto start = std::chrono::steady_clock::time_point {} + 1h;

    REQUIRE (f.control->editFromGui (f.plugin, kAlpha, 0.6));
    const auto beforeGuiEdit = f.instance->stateRevision();
    f.instance->serviceIdleParams (start);
    f.instance->serviceIdleParams (start + 100ms);
    CHECK (f.probe().flushCalls == 0);
    f.instance->serviceIdleParams (start + 600ms);
    CHECK (f.probe().flushCalls == 1);
    CHECK (f.probe().flushOnAudioThread);
    CHECK_THAT (f.value (kAlpha), WithinAbs (0.6, 1e-12));
    CHECK (f.instance->stateRevision() > beforeGuiEdit);
    CHECK (f.instance->lastTouchedParamIndex() == 0);

    const auto running = start + 2s;
    f.process();
    REQUIRE (f.control->editFromGui (f.plugin, kBeta, 0.3));
    f.instance->serviceIdleParams (running);
    f.process();
    f.instance->serviceIdleParams (running + 600ms);
    CHECK (f.probe().flushCalls == 1);
    CHECK (f.probe().processCalls == 2);
    CHECK_THAT (f.value (kBeta), WithinAbs (0.3, 1e-12));

    f.instance->setParamValue (kAlpha, 0.9);
    f.instance->serviceIdleParams (running + 700ms);
    CHECK (f.probe().flushCalls == 1);
    f.instance->serviceIdleParams (running + 1200ms);
    CHECK (f.probe().flushCalls == 2);
    CHECK_THAT (f.value (kAlpha), WithinAbs (0.9, 1e-12));

    f.instance->serviceIdleParams (running + 5s);
    CHECK (f.probe().flushCalls == 2);
    CHECK_FALSE (f.probe().overlapped);
}

TEST_CASE ("ClapInstance answers is_audio_thread on whichever thread runs the block",
           "[clap][instance][thread-check]")
{
    Fixture f;
    f.process();
    CHECK (f.probe().processOnAudioThread);

    // A worker-lane change moves a strip to another real-time thread.
    std::thread lane ([&f] { f.process(); });
    lane.join();
    CHECK (f.probe().processCalls == 2);
    CHECK (f.probe().processOnAudioThread);
    CHECK_FALSE (f.callerIsAudioThread());

    f.process();
    CHECK (f.probe().processOnAudioThread);
}

TEST_CASE ("NativeClapSlot hands Save the writes still queued for the audio thread",
           "[clap][slot][state]")
{
    NativeClapSlot slot;
    std::string error;
    REQUIRE (slot.load (std::filesystem::u8path (DUSKSTUDIO_PARAM_TOUCH_CLAP_FIXTURE_PATH),
                        48000.0, 64, error));
    std::array<float, 64> left {}, right {};
    const auto block = [&] { slot.processStereo (left.data(), right.data(), left.data(), right.data(), 64); };
    block();
    const auto generation = slot.generation();
    const auto* plugin = slot.getInstance()->getPlugin();
    const auto& control = hostControl (plugin);

    // The engine's order: flush, then the revision, then the state.
    const auto capture = [&] (std::uint64_t& token)
    {
        REQUIRE (slot.flushPendingWrites());
        token = slot.stateRevision();
        std::vector<std::uint8_t> blob;
        REQUIRE (slot.saveState (blob));
        CHECK (slot.stateRevision() == token);
        return savedValues (blob);
    };

    SECTION ("a MIDI binding and a host write")
    {
        slot.queueParamBinding (0, 0.75f);
        slot.setParamValue (kBeta, 0.5);
        const auto before = slot.stateRevision();
        std::uint64_t token = 0;
        const auto saved = capture (token);
        CHECK_THAT (saved[0], WithinAbs (0.75, 1e-6));
        CHECK_THAT (saved[1], WithinAbs (0.5, 1e-12));
        CHECK (token > before);
    }

    SECTION ("a bypassed insert the audio thread passes over")
    {
        slot.setBypassed (true);
        slot.setParamValue (kAlpha, 0.25);
        block();
        std::uint64_t token = 0;
        CHECK_THAT (capture (token)[0], WithinAbs (0.25, 1e-12));
    }

    SECTION ("an edit made in the plug-in's own GUI")
    {
        REQUIRE (control.editFromGui (plugin, kBeta, 0.4));
        const auto before = slot.stateRevision();
        std::uint64_t token = 0;
        CHECK_THAT (capture (token)[1], WithinAbs (0.4, 1e-12));
        CHECK (token > before);
    }

    CHECK (slot.generation() == generation);
    const auto settled = slot.stateRevision();
    REQUIRE (slot.flushPendingWrites());
    CHECK (slot.stateRevision() == settled);
}
