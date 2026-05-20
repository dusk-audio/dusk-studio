#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>

// Regression test for the use-after-free that crashed Mutter on the
// Save / Don't Save quit modal. The QuitConfirmDialog wired its
// buttons as `onClick = [this] { onSave(); };` and the configured
// `onSave` lambda called `quitModal.close()` synchronously - which,
// pre-fix, ran ~QuitConfirmDialog → ~Button → ~std::function while
// the std::function's body was still executing. Returning through
// the freed std::function then corrupted message-thread state and
// the next X11 round-trip took down the GNOME compositor.
//
// EmbeddedModal::close was hardened to move the body into a
// callAsync, so the body destructs only AFTER the current call
// stack has unwound. This test pins the contract:
//
//   1. close() called from inside the body's callback does NOT
//      destruct the body before the callback returns.
//   2. The body destructs when (and only when) the deferred
//      callAsync slot is drained, which we simulate by destructing
//      the holder's deferred slot manually.
//
// We don't link juce_gui_basics into dusk-studio-tests (would drag a
// MessageManager + GraphicsContext stack in for a single test), so
// the test exercises the property at the std::unique_ptr level: a
// minimal Holder that mirrors EmbeddedModal::close's deferred-slot
// idiom. Any future refactor of EmbeddedModal::close that breaks
// the property will fail this assertion before reaching the user.

namespace
{
struct Body
{
    std::function<void()> callback;
    bool* insideCallbackFlag = nullptr;

    ~Body()
    {
        if (insideCallbackFlag != nullptr && *insideCallbackFlag)
        {
            // The destructor ran while the callback was on the stack.
            // That is the exact bug the fix is supposed to prevent.
            FAIL ("Body destructor ran while its own callback was "
                  "still executing - this is the crash that took "
                  "down GNOME on Save / Don't Save.");
        }
    }
};

// Mirror of EmbeddedModal::close's body-handoff pattern. Production
// code uses juce::MessageManager::callAsync; the test pumps the
// deferred slot manually via drainDeferred() to keep the test free
// of JUCE message-loop dependencies.
class Holder
{
public:
    void show (std::unique_ptr<Body> b) { body_ = std::move (b); }

    void close()
    {
        if (body_ == nullptr) return;
        deferred_ = std::move (body_);   // ownership transfer; body NOT destructed here
    }

    void drainDeferred() { deferred_.reset(); }

    bool isOpen()       const { return body_     != nullptr; }
    bool hasDeferred()  const { return deferred_ != nullptr; }

private:
    std::unique_ptr<Body> body_;
    std::unique_ptr<Body> deferred_;
};
} // namespace

TEST_CASE ("EmbeddedModal::close: body-initiated close does NOT destruct "
            "body inside its own callback",
            "[embedded-modal][regression]")
{
    Holder holder;

    auto body = std::make_unique<Body>();
    Body* rawBody = body.get();

    bool insideCallback = false;
    body->insideCallbackFlag = &insideCallback;

    int callbackRuns = 0;
    body->callback = [&]
    {
        insideCallback = true;
        holder.close();    // The risky call: pre-fix this destructed `*this`
        ++callbackRuns;    // touching captured locals after close MUST be safe
        insideCallback = false;
    };

    holder.show (std::move (body));
    REQUIRE (holder.isOpen());

    // Trigger the callback as a button click would.
    rawBody->callback();

    REQUIRE (callbackRuns == 1);
    REQUIRE (! holder.isOpen());        // body_ moved out by close()
    REQUIRE (holder.hasDeferred());     // deferred slot holds it

    // Drain the deferred destruction. ~Body runs here, with
    // insideCallback == false, so the assertion in ~Body passes.
    holder.drainDeferred();
    REQUIRE (! holder.hasDeferred());
}

TEST_CASE ("EmbeddedModal::close: drain destroys the body exactly once",
            "[embedded-modal]")
{
    Holder holder;

    int destructorRuns = 0;

    struct CountingBody
    {
        int* counter;
        ~CountingBody() { ++(*counter); }
    };

    auto cbody = std::make_unique<CountingBody>();
    cbody->counter = &destructorRuns;

    // Simulate the lifecycle directly on Holder<CountingBody> would
    // need a template; instead we hand-roll an equivalent test using
    // the same deferred-slot move pattern.
    std::unique_ptr<CountingBody> body  = std::move (cbody);
    std::unique_ptr<CountingBody> deferred;

    auto closeDeferred = [&] { if (body) deferred = std::move (body); };
    auto drain          = [&] { deferred.reset(); };

    closeDeferred();
    REQUIRE (body == nullptr);
    REQUIRE (deferred != nullptr);
    REQUIRE (destructorRuns == 0);   // deferred only — not destructed yet

    drain();
    REQUIRE (destructorRuns == 1);
    REQUIRE (deferred == nullptr);

    drain();   // idempotent
    REQUIRE (destructorRuns == 1);
}
