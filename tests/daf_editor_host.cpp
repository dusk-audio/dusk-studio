#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ui/imgui/DafEditorHost.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

// The traffic between a built-in unit's own plug-in editor and the unit, driven
// through a stand-in editor so none of it needs a window: values pushed in as they
// change, edits coming back out, the gesture that marks a parameter touched and
// hands focus back, the two-tick close, an editor that fails in its own pump, and
// the first-frame marker a run that never came back leaves behind.

using duskstudio::imgui::DafEditorHost;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr std::uintptr_t kParent = 0x1234;

// A stand-in for the plug-in's editor: records what the host does to it and lets a
// test drive the editor's side of the conversation.
class FakeEditor final : public duskstudio::builtin::DafEditor
{
public:
    explicit FakeEditor (duskstudio::builtin::DafEditorCallbacks cb)
        : callbacks (std::move (cb)) {}

    ~FakeEditor() override { ++destroyed(); }

    bool setOffset (int x, int y) noexcept override
    {
        offsetX = x;
        offsetY = y;
        ++offsetCalls;
        return placeable;
    }

    void setSize (std::uint32_t w, std::uint32_t h) override
    {
        sizeW = w;
        sizeH = h;
    }

    bool idle() noexcept override
    {
        ++idles;
        return alive;
    }

    void parameterChanged (std::uint32_t index, float value) override
    {
        pushed.push_back ({ index, value });
    }

    std::uint32_t width() const noexcept override  { return sizeW; }
    std::uint32_t height() const noexcept override { return sizeH; }
    std::uintptr_t nativeWindow() const noexcept override { return 0x5a5a; }

    // The editor's side: what the plug-in's UI would call.
    void edit (std::uint32_t index, float value) { callbacks.parameterEdited (index, value); }
    void gesture (std::uint32_t index, bool started) { callbacks.gesture (index, started); }

    static int& destroyed() { static int count = 0; return count; }

    struct Push { std::uint32_t index; float value; };

    duskstudio::builtin::DafEditorCallbacks callbacks;
    std::vector<Push> pushed;
    int offsetX = 0, offsetY = 0, offsetCalls = 0, idles = 0;
    std::uint32_t sizeW = 0, sizeH = 0;
    bool placeable = true;
    bool alive = true;
};

// A stand-in unit: four controls, the last one an output the unit writes itself,
// and an integer control it conforms the way a plug-in would.
struct FakeUnit
{
    std::vector<float> values { 0.25f, 1.0f, 0.5f, 0.0f };
    std::vector<int> touched;
    int editorsBuilt = 0;
    bool refuse = false;
    std::string refusal;
    FakeEditor* live = nullptr;

    DafEditorHost::Unit wire()
    {
        DafEditorHost::Unit unit;
        unit.createEditor = [this] (std::uintptr_t, std::uint32_t w, std::uint32_t h, double,
                                    duskstudio::builtin::DafEditorCallbacks callbacks,
                                    std::string& errorOut)
            -> std::unique_ptr<duskstudio::builtin::DafEditor>
        {
            if (refuse)
            {
                errorOut = refusal;
                return nullptr;
            }
            auto editor = std::make_unique<FakeEditor> (std::move (callbacks));
            editor->sizeW = w;
            editor->sizeH = h;
            editor->placeable = placeable;
            live = editor.get();
            ++editorsBuilt;
            return editor;
        };
        unit.paramCount = [this] { return (int) values.size(); };
        unit.paramValue = [this] (int index) { return values[(std::size_t) index]; };
        unit.setParam = [this] (int index, float value)
        {
            // Control 1 is an integer, as a plug-in's mode switch would be.
            values[(std::size_t) index] = index == 1 ? std::round (value) : value;
        };
        unit.noteTouched = [this] (int index) { touched.push_back (index); };
        return unit;
    }

    bool placeable = true;
};

DafEditorHost::Geometry geometryAt (int x, int y, std::uint32_t w, std::uint32_t h,
                                    double scale = 1.0)
{
    return { x, y, w, h, scale };
}

// ctest runs cases as parallel processes, so the name has to be claimed by the
// creation itself rather than merely be unlikely.
std::filesystem::path makeTempDir()
{
    std::random_device entropy;
    for (int attempt = 0; attempt < 64; ++attempt)
    {
        const auto candidate = std::filesystem::temp_directory_path()
                             / ("dusk-daf-editor-host-" + std::to_string (entropy()));
        std::error_code ec;
        if (std::filesystem::create_directory (candidate, ec))
            return candidate;
    }
    FAIL ("could not create a temporary directory");
    return {};
}

struct Rig
{
    explicit Rig (std::filesystem::path marker = {})
        : host ("daf-editor-test", "Tape Echo 2", std::move (marker), /*pumpIntervalMs*/ 0)
    {
        host.setUnit (unit.wire());

        DafEditorHost::Callbacks callbacks;
        callbacks.closed = [this] { ++closes; };
        callbacks.gestureEnded = [this] { ++focusHandbacks; };
        callbacks.geometry = [this] { return wanted; };
        host.setCallbacks (std::move (callbacks));
    }

    FakeUnit unit;
    DafEditorHost host;
    DafEditorHost::Geometry wanted = geometryAt (10, 20, 900, 340);
    int closes = 0;
    int focusHandbacks = 0;
};
} // namespace

TEST_CASE ("a plug-in editor host pushes every value in when it opens", "[builtin][daf][editor]")
{
    Rig rig;
    REQUIRE (rig.host.open (kParent, rig.wanted));
    REQUIRE (rig.host.isOpen());
    REQUIRE (rig.unit.live->offsetCalls == 1);
    REQUIRE (rig.unit.live->offsetX == 10);

    rig.host.tick();
    REQUIRE (rig.unit.live->idles == 1);
    REQUIRE (rig.unit.live->pushed.size() == rig.unit.values.size());
    for (std::size_t i = 0; i < rig.unit.values.size(); ++i)
        REQUIRE_THAT (rig.unit.live->pushed[i].value,
                      WithinAbs (rig.unit.values[i], 1e-9));

    // Only what changed afterwards, meters included: the unit writes control 3.
    rig.unit.live->pushed.clear();
    rig.host.tick();
    REQUIRE (rig.unit.live->pushed.empty());

    rig.unit.values[3] = 0.8f;
    rig.host.tick();
    REQUIRE (rig.unit.live->pushed.size() == 1);
    REQUIRE (rig.unit.live->pushed[0].index == 3u);
    REQUIRE_THAT (rig.unit.live->pushed[0].value, WithinAbs (0.8, 1e-6));
}

TEST_CASE ("an edit in a plug-in editor reaches the unit and does not come back",
           "[builtin][daf][editor]")
{
    Rig rig;
    REQUIRE (rig.host.open (kParent, rig.wanted));
    rig.host.tick();
    rig.unit.live->pushed.clear();

    rig.unit.live->edit (0, 0.75f);
    REQUIRE_THAT (rig.unit.values[0], WithinAbs (0.75, 1e-9));

    // The control the user is working is not written back into.
    rig.host.tick();
    REQUIRE (rig.unit.live->pushed.empty());

    // A control the unit conforms is pushed back, because what it holds is not
    // what the editor sent.
    rig.unit.live->edit (1, 2.4f);
    REQUIRE_THAT (rig.unit.values[1], WithinAbs (2.0, 1e-9));
    rig.host.tick();
    REQUIRE (rig.unit.live->pushed.empty());
}

TEST_CASE ("a gesture marks the parameter touched and hands focus back at its end",
           "[builtin][daf][editor]")
{
    Rig rig;
    REQUIRE (rig.host.open (kParent, rig.wanted));

    rig.unit.live->gesture (2, true);
    REQUIRE (rig.unit.touched == std::vector<int> { 2 });
    REQUIRE (rig.focusHandbacks == 0);

    rig.unit.live->gesture (2, false);
    REQUIRE (rig.focusHandbacks == 1);
    REQUIRE (rig.unit.touched == std::vector<int> { 2 });
}

TEST_CASE ("a plug-in editor host follows the geometry it is given", "[builtin][daf][editor]")
{
    Rig rig;
    REQUIRE (rig.host.open (kParent, rig.wanted));
    rig.host.tick();

    rig.wanted = geometryAt (40, 50, 600, 226);
    rig.host.tick();
    REQUIRE (rig.unit.live->sizeW == 600u);
    REQUIRE (rig.unit.live->offsetX == 40);
    REQUIRE (rig.unit.editorsBuilt == 1);

    // A display-scale change cannot be applied to a built window, so the editor
    // is rebuilt for it.
    rig.wanted = geometryAt (40, 50, 600, 226, 2.0);
    rig.host.tick();
    REQUIRE (rig.unit.editorsBuilt == 2);
    REQUIRE (rig.host.isOpen());
}

TEST_CASE ("a plug-in editor closes over two ticks and reopens", "[builtin][daf][editor]")
{
    Rig rig;
    const int before = FakeEditor::destroyed();
    REQUIRE (rig.host.open (kParent, rig.wanted));
    rig.host.tick();

    rig.host.close();
    REQUIRE (rig.host.isOpen());
    rig.host.tick();
    REQUIRE (FakeEditor::destroyed() == before + 1);
    REQUIRE (rig.closes == 0);

    rig.host.tick();
    REQUIRE (rig.closes == 1);
    REQUIRE_FALSE (rig.host.isOpen());

    REQUIRE (rig.host.open (kParent, rig.wanted));
    rig.host.tick();
    REQUIRE (rig.unit.editorsBuilt == 2);
    REQUIRE (rig.unit.live->idles == 1);
}

TEST_CASE ("a plug-in editor host names its editor's window only while it is open",
           "[builtin][daf][editor]")
{
    Rig rig;
    REQUIRE (rig.host.nativeWindow() == 0u);

    REQUIRE (rig.host.open (kParent, rig.wanted));
    rig.host.tick();
    REQUIRE (rig.host.nativeWindow() == 0x5a5au);

    // A window on its way down is not one to read.
    rig.host.close();
    REQUIRE (rig.host.nativeWindow() == 0u);
    rig.host.tick();
    rig.host.tick();
    REQUIRE_FALSE (rig.host.isOpen());
    REQUIRE (rig.host.nativeWindow() == 0u);
}

TEST_CASE ("an editor that fails inside its own pump takes itself down",
           "[builtin][daf][editor]")
{
    Rig rig;
    REQUIRE (rig.host.open (kParent, rig.wanted));
    rig.host.tick();

    rig.unit.live->alive = false;
    rig.host.tick();
    REQUIRE (rig.closes == 1);
    REQUIRE_FALSE (rig.host.isOpen());

    // Nothing is pumped afterwards, and a later tick is harmless.
    rig.host.tick();
    REQUIRE (rig.closes == 1);
}

TEST_CASE ("a plug-in editor host refuses what it cannot open", "[builtin][daf][editor]")
{
    SECTION ("a parent that is not ready")
    {
        Rig rig;
        REQUIRE_FALSE (rig.host.open (0, rig.wanted));
        REQUIRE_FALSE (rig.host.isOpen());
    }

    SECTION ("a plug-in that cannot build one")
    {
        Rig rig;
        rig.unit.refuse = true;
        rig.unit.refusal = "cannot embed (no context).";
        REQUIRE_FALSE (rig.host.open (kParent, rig.wanted));
        REQUIRE (rig.host.lastOpenFailure().find ("cannot embed (no context).")
                 != std::string::npos);
    }

    SECTION ("a display backend that cannot place an embedded child")
    {
        Rig rig;
        rig.unit.placeable = false;
        const int before = FakeEditor::destroyed();
        REQUIRE_FALSE (rig.host.open (kParent, rig.wanted));
        REQUIRE (FakeEditor::destroyed() == before + 1);
        REQUIRE (rig.host.lastOpenFailure().find ("display backend") != std::string::npos);
        REQUIRE_FALSE (rig.host.isOpen());
    }
}

TEST_CASE ("a first-frame marker refuses the editor until it is deleted",
           "[builtin][daf][editor]")
{
    const auto dir = makeTempDir();
    const auto marker = dir / "daf-editor-first-frame";
    {
        std::ofstream out (marker);
        out << "this display\n";
    }

    {
        Rig rig (marker);
        REQUIRE_FALSE (rig.host.open (kParent, rig.wanted));
        REQUIRE (rig.host.lastOpenFailure().find ("first frame") != std::string::npos);
        REQUIRE (rig.unit.editorsBuilt == 0);
    }

    std::error_code ec;
    std::filesystem::remove (marker, ec);

    {
        Rig rig (marker);
        REQUIRE (rig.host.open (kParent, rig.wanted));
        // Armed while the first frame is in flight, and gone once one completes.
        REQUIRE (std::filesystem::exists (marker));
        rig.host.tick();
        REQUIRE_FALSE (std::filesystem::exists (marker));
    }

    std::filesystem::remove_all (dir, ec);
}
