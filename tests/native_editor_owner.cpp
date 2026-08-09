#include <catch2/catch_test_macros.hpp>

#include "../src/engine/hosting/NativeInsertSlot.h"
#include "../src/ui/NativeEditorOwner.h"

#include <memory>
#include <vector>

using duskstudio::NativeEditorOwner;

namespace
{
struct FakeInstance { int id = 0; };

// Stands in for a real slot: the editor-side contract is getInstance() plus
// generation().
struct FakeSlot
{
    FakeInstance* getInstance() noexcept { return live; }
    std::uint64_t generation() const noexcept { return gen; }

    FakeInstance* live = nullptr;
    std::uint64_t gen  = 0;
};

struct FakeEditor
{
    explicit FakeEditor (int& counter) : abandonCount (counter) {}

    // Mirrors the real components: abandoning unbinds the slot, so staleness
    // stops reporting from here on and only `abandoned` still marks the editor
    // as reapable.
    void abandonInstance() { ++abandonCount; stale = false; abandoned = true; }
    bool ownerIsStale() const noexcept { return stale; }
    bool wasAbandoned() const noexcept { return abandoned; }

    int& abandonCount;
    bool stale     = false;
    bool abandoned = false;
};

enum class TeardownCall
{
    Quiesce,
    AbandonPlugin,
    AbandonPluginAndContainer,
    Close,
};

struct FakePlatformEditor
{
    void quiesce() { calls.push_back (TeardownCall::Quiesce); }
    void abandonPlugin() { calls.push_back (TeardownCall::AbandonPlugin); }
    void abandonPluginAndContainer()
    { calls.push_back (TeardownCall::AbandonPluginAndContainer); }
    void close() { calls.push_back (TeardownCall::Close); }

    std::vector<TeardownCall> calls;
};

// Minimal traits for a real NativeInsertSlot instantiation - enough to reach
// load() / unload() / leakForShutdown() with no plugin format behind it.
struct FakeBundle
{
    bool load (const std::string&, std::string&) { return true; }
};

struct FakeSlotInstance
{
    bool create (FakeBundle&, const std::string&, std::string&) { return true; }
    bool activate (double, int, std::string&) { active = true; return true; }
    bool isActive() const noexcept { return active; }
    const duskstudio::hosting::PortLayout& portLayout() const noexcept { return layout; }

    duskstudio::hosting::PortLayout layout;
    bool active = false;
};

struct FakeTraits
{
    using Bundle   = FakeBundle;
    using Instance = FakeSlotInstance;
    static constexpr const char* bundleNoun = "bundle";
    static bool pickPlugin (const Bundle&, const std::string&,
                            std::string& idOut, std::string&)
    { idOut = "fake"; return true; }
};
} // namespace

TEST_CASE ("NativeEditorOwner tracks the slot it was stamped against")
{
    FakeInstance inst;
    FakeSlot slot { &inst, 1 };

    NativeEditorOwner owner;
    owner.stamp (slot);
    REQUIRE_FALSE (owner.isStale (slot));

    SECTION ("an unloaded slot is stale")
    {
        slot.live = nullptr;
        slot.gen  = 2;
        REQUIRE (owner.isStale (slot));
    }

    SECTION ("a different instance is stale")
    {
        FakeInstance other;
        slot.live = &other;
        slot.gen  = 2;
        REQUIRE (owner.isStale (slot));
    }

    SECTION ("a reload landing on the same address is stale")
    {
        // The pointer alone cannot tell the successor apart from the instance
        // that was freed from that address - the generation is what catches it.
        slot.gen = 2;
        REQUIRE (owner.isStale (slot));
    }
}

TEST_CASE ("NativeInsertSlot bumps its generation on every identity change")
{
    duskstudio::hosting::NativeInsertSlot<FakeTraits> slot;
    const auto atRest = slot.generation();

    std::string err;
    REQUIRE (slot.load ("fake.bundle", 48000.0, 512, err));
    const auto afterLoad = slot.generation();
    REQUIRE (afterLoad != atRest);

    NativeEditorOwner owner;
    owner.stamp (slot);
    REQUIRE_FALSE (owner.isStale (slot));

    slot.unload();
    REQUIRE (slot.generation() != afterLoad);
    REQUIRE (owner.isStale (slot));

    REQUIRE (slot.load ("fake.bundle", 48000.0, 512, err));
    const auto afterReload = slot.generation();
    owner.stamp (slot);
    slot.leakForShutdown();
    REQUIRE (slot.generation() != afterReload);
    REQUIRE (owner.isStale (slot));
}

TEST_CASE ("AbandonInstance quiesces only while the plugin is live")
{
    FakePlatformEditor editor;

    SECTION ("live owner hides before dropping plugin handles")
    {
        duskstudio::abandonNativeEditorInstance (editor, false);
        const std::vector<TeardownCall> expected {
            TeardownCall::Quiesce,
            TeardownCall::AbandonPlugin,
            TeardownCall::Close,
        };
        REQUIRE (editor.calls == expected);
    }

    SECTION ("stale owner sends no container message")
    {
        duskstudio::abandonNativeEditorInstance (editor, true);
        const std::vector<TeardownCall> expected {
            TeardownCall::AbandonPluginAndContainer,
            TeardownCall::Close,
        };
        REQUIRE (editor.calls == expected);
    }
}

TEST_CASE ("syncNativeEditorOwner drops a stale editor and keeps a live one")
{
    int abandoned = 0;
    auto editor = std::make_unique<FakeEditor> (abandoned);

    int detached = 0;
    auto detach = [&detached] (auto&) { ++detached; };

    duskstudio::syncNativeEditorOwner (editor, detach);
    REQUIRE (editor != nullptr);
    REQUIRE (abandoned == 0);
    REQUIRE (detached == 0);

    editor->stale = true;
    duskstudio::syncNativeEditorOwner (editor, detach);
    REQUIRE (editor == nullptr);
    // The plugin-side handles must be dropped before the component destructs,
    // and the owner's own bookkeeping run while it is still alive.
    REQUIRE (abandoned == 1);
    REQUIRE (detached == 1);

    // Nothing left to reap.
    duskstudio::syncNativeEditorOwner (editor, detach);
    REQUIRE (detached == 1);
}

TEST_CASE ("syncNativeEditorOwner reaps an editor that abandoned itself")
{
    // The guards on the component's own pump / embed paths tear it down first
    // and unbind the slot, so by the time the owner polls there is no staleness
    // left to see - only the abandoned mark. Missing that leaves a dead
    // rectangle on screen until something else disturbs the lane.
    int abandoned = 0;
    auto editor = std::make_unique<FakeEditor> (abandoned);
    editor->abandonInstance();
    REQUIRE_FALSE (editor->ownerIsStale());

    int detached = 0;
    auto detach = [&detached] (auto&) { ++detached; };

    duskstudio::syncNativeEditorOwner (editor, detach);
    REQUIRE (editor == nullptr);
    REQUIRE (detached == 1);
}
