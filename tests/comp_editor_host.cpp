#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/MultiCompStage.h"
#include "engine/builtin/DafPlugin.h"
#include "session/CompParams.h"
#include "ui/imgui/CompEditorHost.h"

#include <atomic>
#include <string>

// What a compressor's Multi-Comp 2 editor reads and writes: the session values
// through the stage's mapping, with the engage switch and the strip's picked mode
// moved the way the strip, bus and master faces move them.

using Catch::Matchers::WithinAbs;
using duskstudio::CompParamId;
using duskstudio::CompParams;
using duskstudio::CompStage;
using duskstudio::MultiCompStage;
using duskstudio::imgui::CompEditorTarget;
using duskstudio::imgui::compEditorEdit;
using duskstudio::imgui::compEditorValue;

namespace
{
int indexOf (MultiCompStage& stage, const std::string& symbol)
{
    const auto& descs = stage.plugin().params();
    for (std::size_t i = 0; i < descs.size(); ++i)
        if (descs[i].symbol == symbol)
            return (int) i;
    return -1;
}

struct StripSession
{
    CompParams comp;
    std::atomic<bool> engaged { false };
    std::atomic<int> mode { 0 };
    std::atomic<bool> picked { false };

    CompEditorTarget target()
    {
        CompEditorTarget t;
        t.comp = &comp;
        t.engaged = &engaged;
        t.stripMode = &mode;
        t.modePicked = &picked;
        return t;
    }
};
} // namespace

TEST_CASE ("comp editor: a strip control edit lands in the session and engages the compressor")
{
    MultiCompStage stage (CompStage::Strip);
    StripSession s;
    const int input = indexOf (stage, "fet_input");
    REQUIRE (input >= 0);

    compEditorEdit (stage, s.target(), input, 12.0f);
    CHECK_THAT (s.comp.get (CompParamId::FetInput), WithinAbs (12.0f, 1.0e-6f));
    CHECK (s.engaged.load());
    CHECK_THAT (compEditorValue (stage, s.target(), input), WithinAbs (12.0f, 1.0e-6f));
    CHECK_FALSE (s.picked.load());
}

TEST_CASE ("comp editor: a strip switch turned off leaves the compressor bypassed")
{
    MultiCompStage stage (CompStage::Strip);
    StripSession s;
    const int limit = indexOf (stage, "opto_limit");
    REQUIRE (limit >= 0);

    compEditorEdit (stage, s.target(), limit, 0.0f);
    CHECK_FALSE (s.engaged.load());
    compEditorEdit (stage, s.target(), limit, 1.0f);
    CHECK (s.comp.get (CompParamId::OptoLimit) > 0.5f);
    CHECK (s.engaged.load());
}

TEST_CASE ("comp editor: a mode tab sets the strip's mode and marks it picked without engaging")
{
    MultiCompStage stage (CompStage::Strip);
    StripSession s;
    const int mode = indexOf (stage, "mode");
    REQUIRE (mode >= 0);

    compEditorEdit (stage, s.target(), mode, 2.0f);
    CHECK (s.mode.load() == 2);
    CHECK (s.picked.load());
    CHECK_FALSE (s.engaged.load());
    CHECK_THAT (compEditorValue (stage, s.target(), mode), WithinAbs (2.0f, 0.0f));

    SECTION ("a face mode change shows in the editor")
    {
        s.mode.store (1);
        CHECK_THAT (compEditorValue (stage, s.target(), mode), WithinAbs (1.0f, 0.0f));
    }
}

TEST_CASE ("comp editor: BYPASS is the engage switch inverted, both ways")
{
    MultiCompStage stage (CompStage::Strip);
    StripSession s;
    const int bypass = indexOf (stage, "daf_bypass");
    REQUIRE (bypass >= 0);

    CHECK_THAT (compEditorValue (stage, s.target(), bypass), WithinAbs (1.0f, 0.0f));
    compEditorEdit (stage, s.target(), bypass, 0.0f);
    CHECK (s.engaged.load());
    compEditorEdit (stage, s.target(), bypass, 1.0f);
    CHECK_FALSE (s.engaged.load());
    s.engaged.store (true);
    CHECK_THAT (compEditorValue (stage, s.target(), bypass), WithinAbs (0.0f, 0.0f));
}

TEST_CASE ("comp editor: on a bus only the threshold engages the compressor")
{
    MultiCompStage stage (CompStage::Bus);
    CompParams comp;
    std::atomic<bool> engaged { false };
    CompEditorTarget target;
    target.comp = &comp;
    target.engaged = &engaged;
    target.engagedOnlyBy = CompParamId::BusThreshold;

    const int ratio = indexOf (stage, "bus_ratio");
    const int threshold = indexOf (stage, "bus_threshold");
    const int mode = indexOf (stage, "mode");
    REQUIRE (ratio >= 0);
    REQUIRE (threshold >= 0);
    REQUIRE (mode >= 0);

    compEditorEdit (stage, target, ratio, 2.0f);
    CHECK_THAT (comp.get (CompParamId::BusRatio), WithinAbs (2.0f, 0.0f));
    CHECK_FALSE (engaged.load());

    compEditorEdit (stage, target, mode, 0.0f);
    CHECK_FALSE (engaged.load());

    compEditorEdit (stage, target, threshold, -12.0f);
    CHECK_THAT (comp.get (CompParamId::BusThreshold), WithinAbs (-12.0f, 1.0e-6f));
    CHECK (engaged.load());
}

// The stage an editor draws keeps the session's values in its plug-in only
// while the editor is up; one that never opened leaves no such duty behind.
TEST_CASE ("comp editor: an editor that could not open leaves its compressor's plug-in alone")
{
    MultiCompStage stage (CompStage::Bus);
    CompParams comp;
    std::atomic<bool> engaged { false };
    CompEditorTarget target;
    target.comp = &comp;
    target.engaged = &engaged;
    {
        duskstudio::imgui::CompEditorHost host ([&stage] () -> MultiCompStage& { return stage; }, target);
        CHECK_FALSE (host.open (0, { 0, 0, 640, 480, 1.0 }));
        CHECK (host.shownStage() == nullptr);
        CHECK_FALSE (stage.editorOpen());
    }
    CHECK_FALSE (stage.editorOpen());
}
