#include <catch2/catch_test_macros.hpp>

#include "TestTempDirectory.h"
#include "engine/lv2/Lv2Editor.h"
#include "engine/lv2/NativeLv2Slot.h"
#include "fixtures/Lv2UiLifetimeProbe.h"

#include <lv2/core/lv2.h>
#include <X11/Xlib.h>
#include <dlfcn.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace
{
struct FixtureModule
{
    explicit FixtureModule (const std::filesystem::path& bundle)
        : value (::dlopen ((bundle / "file_state_fixture.so").c_str(), RTLD_NOW)) {}

    ~FixtureModule() { if (value != nullptr) ::dlclose (value); }

    void* value = nullptr;
};

struct ParentWindow
{
    ParentWindow()
    {
        display = XOpenDisplay (nullptr);
        if (display == nullptr) return;
        window = XCreateSimpleWindow (display, DefaultRootWindow (display),
                                      0, 0, 320, 160, 0, 0, 0);
        XMapWindow (display, window);
        XSync (display, False);
    }

    ~ParentWindow()
    {
        if (display == nullptr) return;
        if (window != 0) XDestroyWindow (display, window);
        XCloseDisplay (display);
    }

    Display* display = nullptr;
    Window window = 0;
};
}

TEST_CASE ("LV2 UI cleanup precedes DSP teardown", "[lv2][editor][lifetime]")
{
    using namespace duskstudio;
    const std::filesystem::path bundle = DUSKSTUDIO_FILE_STATE_LV2_FIXTURE_PATH;
    FixtureModule module (bundle);
    REQUIRE (module.value != nullptr);
    const auto descriptor = reinterpret_cast<const LV2_Descriptor* (*) (std::uint32_t)> (
        ::dlsym (module.value, "lv2_descriptor"));
    REQUIRE (descriptor != nullptr);
    const auto* probe = static_cast<const test::Lv2UiLifetimeProbe*> (
        descriptor (0)->extension_data (test::kLv2UiLifetimeProbeUri));
    REQUIRE (probe != nullptr);
    const auto cleanupsBefore = probe->cleanupCount();
    const auto staleBefore = probe->staleCleanupCount();

    ParentWindow parent;
    REQUIRE (parent.window != 0);
    test::TempDirectory temp ("dusk-lv2-editor-lifetime-");
    auto slot = std::make_unique<lv2::NativeLv2Slot>();
    std::string error;
    REQUIRE (slot->load (bundle, 48000.0, 32, error, "urn:duskstudio:test:file-state"));
    slot->setStateDirectory (temp.path() / "state");

    lv2::Lv2Editor first;
    lv2::Lv2Editor second;
    const auto embed = [&] (lv2::Lv2Editor& editor)
    {
        REQUIRE (editor.open (*slot->getInstance(), error));
        REQUIRE (editor.embed (static_cast<std::uintptr_t> (parent.window),
                               0, 0, 160, 80, error));
        REQUIRE (editor.isEmbedded());
    };
    embed (first);
    embed (second);

    SECTION ("reactivation")
    {
        const auto generation = slot->generation();
        REQUIRE (slot->reactivate (44100.0, 64, error));
        REQUIRE (slot->generation() == generation);
    }
    SECTION ("failed reactivation")
    {
        REQUIRE_FALSE (slot->reactivate (0.0, 64, error));
        REQUIRE_FALSE (slot->hasActiveInstance());
    }
    SECTION ("deactivation") { slot->getInstance()->deactivate(); }
    SECTION ("unload") { slot->unload(); }
    SECTION ("slot destruction") { slot.reset(); }

    CHECK_FALSE (first.isOpen());
    CHECK_FALSE (second.isOpen());
    first.close();
    second.close();
    CHECK (probe->cleanupCount() == cleanupsBefore + 2);
    CHECK (probe->staleCleanupCount() == staleBefore);

    if (slot != nullptr && slot->isLoaded())
    {
        if (! slot->hasActiveInstance())
            REQUIRE (slot->reactivate (48000.0, 32, error));
        embed (first);
        first.pump();
        first.close();
        CHECK (probe->cleanupCount() == cleanupsBefore + 3);
        CHECK (probe->staleCleanupCount() == staleBefore);
    }
}
