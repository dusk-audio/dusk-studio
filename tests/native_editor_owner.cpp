#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

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

struct FakeSlotInstance final : public duskstudio::hosting::INativeInstance
{
    bool create (FakeBundle&, const std::string&, std::string&)
    {
        duskstudio::hosting::BusInfo in;
        in.kind = duskstudio::hosting::BusInfo::Kind::Audio;
        in.dir = duskstudio::hosting::BusInfo::Direction::Input;
        in.role = duskstudio::hosting::BusInfo::Role::Main;
        in.channelCount = 2;
        in.active = true;
        auto out = in;
        out.dir = duskstudio::hosting::BusInfo::Direction::Output;
        layout.inputs = { in };
        layout.outputs = { out };
        layout.mainInIndex = 0;
        layout.mainOutIndex = 0;
        return true;
    }
    bool activate (double, int, std::string&) override { active = true; return true; }
    void deactivate() override { active = false; }
    bool reactivate (double, int, std::string& error) override
    {
        active = true;
        error = "state carry failed";
        return false;
    }
    bool isActive() const noexcept override { return active; }
    const duskstudio::hosting::PortLayout& portLayout() const noexcept override { return layout; }
    void processBlock (const duskstudio::hosting::PortBuffers& io) noexcept override
    {
        ++processCalls;
        for (int channel = 0; channel < io.mainOutChannels; ++channel)
            for (int frame = 0; frame < io.numFrames; ++frame)
                io.mainOut[channel][frame] = io.mainIn[channel][frame] * 0.5f;
    }
    bool saveState (std::vector<uint8_t>&) const override { return true; }
    bool loadState (const std::vector<uint8_t>&) override { return true; }
    int getLatencySamples() const noexcept override { return 0; }

    duskstudio::hosting::PortLayout layout;
    bool active = false;
    int processCalls = 0;
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

TEST_CASE ("NativeInsertSlot resizes its adapter after a partial reactivate failure",
           "[hosting][slot][regression][issue-357]")
{
    duskstudio::hosting::NativeInsertSlot<FakeTraits> slot;
    std::string error;
    REQUIRE (slot.load ("fake.bundle", 48000.0, 8, error));
    REQUIRE_FALSE (slot.reactivate (48000.0, 16, error));
    REQUIRE (slot.getInstance()->isActive());

    std::vector<float> left (16, 1.0f);
    std::vector<float> right (16, -1.0f);
    slot.processStereo (left.data(), right.data(), left.data(), right.data(), 16);
    REQUIRE (slot.getInstance()->processCalls == 1);
    REQUIRE_THAT (left.front(), Catch::Matchers::WithinAbs (0.5, 1.0e-6));
    REQUIRE_THAT (right.front(), Catch::Matchers::WithinAbs (-0.5, 1.0e-6));
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

TEST_CASE ("Native editor peer transitions preserve realised peer identity")
{
    constexpr std::uint32_t peerAId = 41;
    constexpr std::uint32_t peerBId = 42;
    std::uint32_t lastPeerId = 0;

    const auto initial = duskstudio::observeNativeEditorPeer (
        lastPeerId, peerAId, nullptr, false, {});
    REQUIRE (initial.rebuildNativeEditors);
    REQUIRE_FALSE (initial.reopenNativeEditorNow);
    REQUIRE_FALSE (initial.deferNativeEditorReopen);
    REQUIRE (lastPeerId == peerAId);

    const auto transientNull = duskstudio::observeNativeEditorPeer (
        lastPeerId, 0, nullptr, false, {});
    REQUIRE_FALSE (transientNull.rebuildNativeEditors);
    REQUIRE (lastPeerId == peerAId);

    const auto samePeer = duskstudio::observeNativeEditorPeer (
        lastPeerId, peerAId, nullptr, false, {});
    REQUIRE_FALSE (samePeer.rebuildNativeEditors);
    REQUIRE (lastPeerId == peerAId);

    // Distinct unique IDs still detect replacement if the allocator reuses the
    // old ComponentPeer address for the new top-level window.
    const auto replacement = duskstudio::observeNativeEditorPeer (
        lastPeerId, peerBId, nullptr, false, {});
    REQUIRE (replacement.rebuildNativeEditors);
    REQUIRE_FALSE (replacement.reopenNativeEditorNow);
    REQUIRE_FALSE (replacement.deferNativeEditorReopen);
    REQUIRE (lastPeerId == peerBId);
}

TEST_CASE ("Native editor peer transitions reopen only a borrowed peer-bound body")
{
    constexpr std::uint32_t oldPeerId = 41;
    constexpr std::uint32_t newPeerId = 42;
    int nativeEditor = 0;
    int unrelatedEditor = 0;

    SECTION ("a topmost native editor reopens immediately")
    {
        std::uint32_t lastPeerId = oldPeerId;
        const auto transition = duskstudio::observeNativeEditorPeer (
            lastPeerId, newPeerId, &nativeEditor, true, { &nativeEditor });
        REQUIRE (transition.rebuildNativeEditors);
        REQUIRE (transition.reopenNativeEditorNow);
        REQUIRE_FALSE (transition.deferNativeEditorReopen);
    }

    SECTION ("a covered native editor defers reopening")
    {
        std::uint32_t lastPeerId = oldPeerId;
        const auto transition = duskstudio::observeNativeEditorPeer (
            lastPeerId, newPeerId, &nativeEditor, false, { &nativeEditor });
        REQUIRE (transition.rebuildNativeEditors);
        REQUIRE_FALSE (transition.reopenNativeEditorNow);
        REQUIRE (transition.deferNativeEditorReopen);
    }

    SECTION ("a hidden cached editor is rebuilt without reopening")
    {
        std::uint32_t lastPeerId = oldPeerId;
        const auto transition = duskstudio::observeNativeEditorPeer (
            lastPeerId, newPeerId, nullptr, false, { &nativeEditor });
        REQUIRE (transition.rebuildNativeEditors);
        REQUIRE_FALSE (transition.reopenNativeEditorNow);
        REQUIRE_FALSE (transition.deferNativeEditorReopen);
    }

    SECTION ("an unrelated modal body is not reopened")
    {
        std::uint32_t lastPeerId = oldPeerId;
        const auto transition = duskstudio::observeNativeEditorPeer (
            lastPeerId, newPeerId, &unrelatedEditor, false, { &nativeEditor });
        REQUIRE (transition.rebuildNativeEditors);
        REQUIRE_FALSE (transition.reopenNativeEditorNow);
        REQUIRE_FALSE (transition.deferNativeEditorReopen);
    }
}

TEST_CASE ("Only Linux custom standard VST3 editors are peer-bound")
{
    for (const bool isLinux : { false, true })
        for (const bool isStandardVst3 : { false, true })
            for (const bool isCustomEditor : { false, true })
                CHECK (duskstudio::isPeerBoundStandardVst3Editor (
                           isLinux, isStandardVst3, isCustomEditor)
                       == (isLinux && isStandardVst3 && isCustomEditor));
}

TEST_CASE ("Peer-bound editor reborrow preserves disposition and rejects stale callbacks")
{
    constexpr std::uint64_t currentBorrowGeneration = 42;
    for (const bool isPeerBoundEditor : { false, true })
        for (const bool editorIsVisible : { false, true })
            for (const bool editorHostChanged : { false, true })
                for (const bool editorWasTopmost : { false, true })
                    for (const bool editorIsCurrentlyTopmost : { false, true })
                    {
                        const auto disposition = duskstudio::decidePeerBoundEditorReborrow (
                            isPeerBoundEditor, editorIsVisible, editorHostChanged,
                            editorWasTopmost, editorIsCurrentlyTopmost,
                            currentBorrowGeneration, currentBorrowGeneration);
                        const bool shouldReborrow = isPeerBoundEditor
                            && editorIsVisible && editorHostChanged;
                        const bool reopenNow = shouldReborrow && editorWasTopmost
                            && editorIsCurrentlyTopmost;
                        CHECK (disposition.reopenNow == reopenNow);
                        CHECK (disposition.deferReopen
                               == (shouldReborrow && ! reopenNow));
                    }

    const auto stale = duskstudio::decidePeerBoundEditorReborrow (
        true, true, true, true, true,
        currentBorrowGeneration - 1, currentBorrowGeneration);
    CHECK_FALSE (stale.reopenNow);
    CHECK_FALSE (stale.deferReopen);

    const auto newlyCovered = duskstudio::decidePeerBoundEditorReborrow (
        true, true, true, true, false,
        currentBorrowGeneration, currentBorrowGeneration);
    CHECK_FALSE (newlyCovered.reopenNow);
    CHECK (newlyCovered.deferReopen);

    const auto originallyCovered = duskstudio::decidePeerBoundEditorReborrow (
        true, true, true, false, true,
        currentBorrowGeneration, currentBorrowGeneration);
    CHECK_FALSE (originallyCovered.reopenNow);
    CHECK (originallyCovered.deferReopen);
}

TEST_CASE ("Stacked standard and native editors preserve the prior top editor")
{
    int nativeEditor = 0;

    SECTION ("top native editor reopens before covered standard editor")
    {
        const auto standardDisposition = duskstudio::decidePeerBoundEditorReborrow (
            true, true, true, false, false, 42, 42);
        std::uint32_t lastPeerId = 41;
        const auto nativeTransition = duskstudio::observeNativeEditorPeer (
            lastPeerId, 42, &nativeEditor, true, { &nativeEditor });

        REQUIRE (standardDisposition.deferReopen);
        REQUIRE_FALSE (standardDisposition.reopenNow);
        REQUIRE (nativeTransition.reopenNativeEditorNow);
        REQUIRE_FALSE (nativeTransition.deferNativeEditorReopen);
    }

    SECTION ("top standard editor reopens before covered native editor")
    {
        std::uint32_t lastPeerId = 41;
        const auto nativeTransition = duskstudio::observeNativeEditorPeer (
            lastPeerId, 42, &nativeEditor, false, { &nativeEditor });
        const auto standardDisposition = duskstudio::decidePeerBoundEditorReborrow (
            true, true, true, true, true, 42, 42);

        REQUIRE (nativeTransition.deferNativeEditorReopen);
        REQUIRE_FALSE (nativeTransition.reopenNativeEditorNow);
        REQUIRE (standardDisposition.reopenNow);
        REQUIRE_FALSE (standardDisposition.deferReopen);
    }
}
