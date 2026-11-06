#pragma once

#if DUSKSTUDIO_HAS_SHARED_WAYLAND

#include "../NativeEmbeddedParent.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace DGL { class Application; struct WaylandInput; }

namespace duskstudio::platform
{
struct NativeWaylandConnection
{
    void* host = nullptr;
    void* display = nullptr;
    void* seat = nullptr;

    bool operator== (const NativeWaylandConnection& other) const noexcept
    {
        return host == other.host && display == other.display && seat == other.seat;
    }
};

NativeWaylandConnection waylandConnectionForParent (const NativeEmbeddedParent&);
std::uint64_t attachWaylandInput (const NativeWaylandConnection&, const DGL::WaylandInput&,
                                  std::shared_ptr<void> lifetime);
void detachWaylandInput (const NativeWaylandConnection&, std::uint64_t registration);
void forgetWaylandInputSurface (const NativeWaylandConnection&, std::uintptr_t surface);
bool commitWaylandParent (const NativeWaylandConnection&, const NativeEmbeddedParent&);
std::string readWaylandClipboard (const NativeWaylandConnection&);
bool writeWaylandClipboard (const NativeWaylandConnection&, const std::string&);
}

namespace duskstudio::imgui
{
// All operations run on the display owner's thread. Editors must release their
// windows before releasing this application, and before shell display shutdown.
class SharedWaylandApplication final : public std::enable_shared_from_this<SharedWaylandApplication>
{
public:
    struct Callbacks
    {
        std::function<void()> begin;
        std::function<void(bool)> end;
        std::function<void()> invalidParent;
        std::function<void(const char*)> failed;
    };

    class Registration final
    {
    public:
        ~Registration();
        std::weak_ptr<void> lifetime() const noexcept;
        Registration (const Registration&) = delete;
        Registration& operator= (const Registration&) = delete;

    private:
        friend class SharedWaylandApplication;
        struct State;
        explicit Registration (std::shared_ptr<State>);
        std::shared_ptr<State> state;
    };

    static std::shared_ptr<SharedWaylandApplication> acquire (const platform::NativeEmbeddedParent&);
    ~SharedWaylandApplication();
    DGL::Application& application() const noexcept;
    std::unique_ptr<Registration> registerClient (platform::NativeEmbeddedParent, Callbacks);
    bool pump();
    bool commitParent (const platform::NativeEmbeddedParent&);
    bool activateParent (const platform::NativeEmbeddedParent&, const std::string& token);
    void forgetSurface (std::uintptr_t);

private:
    struct Impl;
    explicit SharedWaylandApplication (platform::NativeWaylandConnection);
    std::unique_ptr<Impl> impl;
};
}
#endif
