// The Linux IRunLoop is the piece VST3 plugin UIs (and some controllers) hang
// their event handling on — prove it actually DISPATCHES: a readable pipe fd
// reaches onFDIsSet, timers fire on cadence, and unregistration sticks. Pure
// host machinery, no plugin required.

#include <catch2/catch_test_macros.hpp>

#include "engine/vst3/Vst3HostContext.h"

#include <pluginterfaces/gui/iplugview.h>

#include <unistd.h>

namespace
{
using namespace Steinberg;

struct FdHandler final : Linux::IEventHandler
{
    tresult PLUGIN_API queryInterface (const TUID, void** obj) override { *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override  { return ++refs; }
    uint32 PLUGIN_API release() override { return --refs; }
    void PLUGIN_API onFDIsSet (Linux::FileDescriptor fd) override { ++fires; lastFd = fd; }

    uint32 refs = 1;
    int fires = 0;
    Linux::FileDescriptor lastFd = -1;
};

struct TimerHandler final : Linux::ITimerHandler
{
    tresult PLUGIN_API queryInterface (const TUID, void** obj) override { *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override  { return ++refs; }
    uint32 PLUGIN_API release() override { return --refs; }
    void PLUGIN_API onTimer() override { ++fires; }

    uint32 refs = 1;
    int fires = 0;
};
} // namespace

TEST_CASE ("Vst3HostContext run loop dispatches fds and timers", "[vst3][hostcontext]")
{
    duskstudio::vst3::Vst3HostContext ctx;
    auto* runLoop = static_cast<Linux::IRunLoop*> (ctx.runLoop());
    REQUIRE (runLoop != nullptr);

    SECTION ("pipe fd reaches onFDIsSet only when readable")
    {
        int fds[2] {};
        REQUIRE (::pipe (fds) == 0);

        FdHandler handler;
        REQUIRE (runLoop->registerEventHandler (&handler, fds[0]) == kResultOk);
        REQUIRE (ctx.numEventHandlers() == 1);

        ctx.pump (16.0);
        REQUIRE (handler.fires == 0);   // nothing written yet

        const char byte = 1;
        REQUIRE (::write (fds[1], &byte, 1) == 1);
        ctx.pump (16.0);
        REQUIRE (handler.fires == 1);
        REQUIRE (handler.lastFd == fds[0]);

        char sink = 0;
        REQUIRE (::read (fds[0], &sink, 1) == 1);   // drain
        ctx.pump (16.0);
        REQUIRE (handler.fires == 1);   // quiet again

        REQUIRE (runLoop->unregisterEventHandler (&handler) == kResultOk);
        REQUIRE (ctx.numEventHandlers() == 0);
        REQUIRE (::write (fds[1], &byte, 1) == 1);
        ctx.pump (16.0);
        REQUIRE (handler.fires == 1);   // unregistered — no dispatch

        ::close (fds[0]);
        ::close (fds[1]);
    }

    SECTION ("timer fires on cadence, not per pump")
    {
        TimerHandler timer;
        REQUIRE (runLoop->registerTimer (&timer, 50) == kResultOk);
        REQUIRE (ctx.numTimerHandlers() == 1);

        for (int i = 0; i < 4; ++i) ctx.pump (10.0);   // 40 ms — below interval
        REQUIRE (timer.fires == 0);
        ctx.pump (10.0);                                // 50 ms — fires
        REQUIRE (timer.fires == 1);
        for (int i = 0; i < 5; ++i) ctx.pump (10.0);   // next 50 ms — fires again
        REQUIRE (timer.fires == 2);

        REQUIRE (runLoop->unregisterTimer (&timer) == kResultOk);
        REQUIRE (ctx.numTimerHandlers() == 0);
        for (int i = 0; i < 10; ++i) ctx.pump (10.0);
        REQUIRE (timer.fires == 2);
    }

    SECTION ("bogus registrations are rejected")
    {
        REQUIRE (runLoop->registerEventHandler (nullptr, 0) == kInvalidArgument);
        REQUIRE (runLoop->registerTimer (nullptr, 50) == kInvalidArgument);
        FdHandler handler;
        REQUIRE (runLoop->unregisterEventHandler (&handler) == kResultFalse);
    }
}
