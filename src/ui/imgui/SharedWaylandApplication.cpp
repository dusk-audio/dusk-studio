#include "SharedWaylandApplication.h"

#if DUSKSTUDIO_HAS_SHARED_WAYLAND
#include "PumpPacing.h"

#include <Application.hpp>
#include <WaylandInput.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace duskstudio::imgui
{
struct SharedWaylandApplication::Registration::State
{
    platform::NativeEmbeddedParent parent;
    std::shared_ptr<Callbacks> callbacks;
};

struct SharedWaylandApplication::Impl
{
    explicit Impl (platform::NativeWaylandConnection c)
        : connection (c), app (std::make_shared<DGL::Application> (
              DGL::Application::ExternalWaylandDisplay (c.display, c.seat)))
    {
    }

    ~Impl()
    {
        disconnectInput();
    }

    void connectInput()
    {
        if (app->getWindowSystem() != DGL::Application::kWindowSystemWayland)
            throw std::runtime_error ("the shared Wayland connection is unavailable");
        app->setClassName ("DuskStudio");
        app->setClipboardCallbacks ({ this,
            [] (void* context, std::size_t& size) -> const void* {
                auto& self = *static_cast<Impl*> (context);
                self.clipboard = platform::readWaylandClipboard (self.connection);
                size = self.clipboard.size() + 1;
                return self.clipboard.c_str();
            },
            [] (void* context, const char* mime, const void* data, std::size_t size) {
                if ((std::strcmp (mime, "text/plain") != 0
                     && std::strcmp (mime, "text/plain;charset=utf-8") != 0)
                    || (data == nullptr && size != 0))
                    return false;
                auto& self = *static_cast<Impl*> (context);
                const auto* bytes = static_cast<const char*> (data);
                if (size != 0 && bytes[size - 1] == '\0')
                    --size;
                if (size != 0 && std::memchr (bytes, '\0', size) != nullptr)
                    return false;
                self.clipboard.assign (size == 0 ? "" : bytes, size);
                return platform::writeWaylandClipboard (self.connection, self.clipboard);
            }
        });
        inputRegistration = platform::attachWaylandInput (connection, app->getWaylandInput(), app);
        if (inputRegistration == 0)
            throw std::runtime_error ("the shared Wayland input stream is unavailable");
    }

    void disconnectInput()
    {
        app->setClipboardCallbacks ({});
        platform::detachWaylandInput (connection, std::exchange (inputRegistration, 0));
    }

    using State = Registration::State;
    std::vector<std::shared_ptr<State>> liveClients()
    {
        std::vector<std::shared_ptr<State>> live;
        clients.erase (std::remove_if (clients.begin(), clients.end(), [&] (const auto& weak) {
            auto client = weak.lock();
            if (client == nullptr || client->callbacks == nullptr)
                return true;
            live.push_back (std::move (client));
            return false;
        }), clients.end());
        return live;
    }

    void fail (const char* message)
    {
        failed = true;
        disconnectInput();
        for (const auto& client : liveClients())
        {
            const auto callback = client->callbacks ? client->callbacks->failed : nullptr;
            if (callback)
                try { callback (message); } catch (...) {}
        }
    }

    platform::NativeWaylandConnection connection;
    std::shared_ptr<DGL::Application> app;
    std::vector<std::weak_ptr<State>> clients;
    std::string clipboard;
    std::uint64_t inputRegistration = 0;
    std::chrono::steady_clock::time_point nextPumpAt {};
    bool failed = false, pumping = false;
};

SharedWaylandApplication::Registration::Registration (std::shared_ptr<State> s)
    : state (std::move (s))
{
}

SharedWaylandApplication::Registration::~Registration()
{
    // An active pump can still hold State after its editor has gone.
    state->callbacks.reset();
}

std::weak_ptr<void> SharedWaylandApplication::Registration::lifetime() const noexcept
{
    return state->callbacks;
}

std::shared_ptr<SharedWaylandApplication> SharedWaylandApplication::acquire (
    const platform::NativeEmbeddedParent& parent)
{
    const auto connection = platform::waylandConnectionForParent (parent);
    if (connection.display == nullptr || connection.seat == nullptr)
        return {};
    static std::weak_ptr<SharedWaylandApplication> shared;
    if (auto current = shared.lock())
        if (! current->impl->failed && current->impl->connection == connection)
            return current;
    auto result = std::shared_ptr<SharedWaylandApplication> (new SharedWaylandApplication (connection));
    shared = result;
    return result;
}

SharedWaylandApplication::SharedWaylandApplication (platform::NativeWaylandConnection connection)
    : impl (std::make_unique<Impl> (connection))
{
    impl->connectInput();
}

SharedWaylandApplication::~SharedWaylandApplication() = default;

DGL::Application& SharedWaylandApplication::application() const noexcept
{
    return *impl->app;
}

std::unique_ptr<SharedWaylandApplication::Registration> SharedWaylandApplication::registerClient (
    platform::NativeEmbeddedParent parent, Callbacks callbacks)
{
    auto state = std::make_shared<Registration::State>();
    state->parent = parent;
    state->callbacks = std::make_shared<Callbacks> (std::move (callbacks));
    impl->clients.push_back (state);
    return std::unique_ptr<Registration> (new Registration (std::move (state)));
}

bool SharedWaylandApplication::pump()
{
    const auto keepAlive = shared_from_this();
    if (impl->failed)
        return false;
    const auto started = std::chrono::steady_clock::now();
    if (impl->pumping)
        return false;
    if (started < impl->nextPumpAt)
        return true;
    impl->pumping = true;
    std::vector<std::shared_ptr<Registration::State>> begun;
    std::size_t ended = 0;
    std::string error;
    bool completed = false;
    try
    {
        for (const auto& client : impl->liveClients())
        {
            if (client->callbacks == nullptr)
                continue;
            if (! (platform::waylandConnectionForParent (client->parent) == impl->connection))
            {
                const auto invalid = client->callbacks->invalidParent;
                if (invalid)
                    invalid();
                continue;
            }
            begun.push_back (client);
            const auto begin = client->callbacks->begin;
            if (begin)
                begin();
        }
        impl->app->idle();
        for (; ended < begun.size(); ++ended)
        {
            const auto& client = begun[ended];
            const auto end = client->callbacks ? client->callbacks->end : nullptr;
            if (end)
                end (true);
        }
        completed = true;
    }
    catch (const std::exception& failure)
    {
        error = failure.what();
    }
    catch (...)
    {
        error = "the shared graphics event pump failed";
    }
    if (! completed)
    {
        if (error.empty())
            error = "the shared graphics event pump failed";
        for (; ended < begun.size(); ++ended)
        {
            const auto& client = begun[ended];
            const auto end = client->callbacks ? client->callbacks->end : nullptr;
            if (end)
                try { end (false); } catch (...) {}
        }
        impl->fail (error.c_str());
    }
    impl->nextPumpAt = std::max (started + std::chrono::milliseconds (16),
        nextPumpAfter (started, std::chrono::steady_clock::now(), 16));
    impl->pumping = false;
    return ! impl->failed;
}

bool SharedWaylandApplication::commitParent (const platform::NativeEmbeddedParent& parent)
{
    return ! impl->failed && platform::commitWaylandParent (impl->connection, parent);
}

bool SharedWaylandApplication::activateParent (const platform::NativeEmbeddedParent& parent,
                                               const std::string& token)
{
    return ! impl->failed && platform::waylandConnectionForParent (parent) == impl->connection
        && impl->app->activateWaylandSurface (reinterpret_cast<void*> (parent.handle), token.c_str());
}

void SharedWaylandApplication::forgetSurface (std::uintptr_t surface)
{
    platform::forgetWaylandInputSurface (impl->connection, surface);
}
}
#endif
