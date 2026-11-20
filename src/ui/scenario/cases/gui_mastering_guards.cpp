#include "../GuiScenarioSupport.h"
#include "../../LargestUniformRect.h"
#include "../../../foundation/Json.h"
#include "../../../session/SessionSerializer.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

// The MASTERING page's modules with nothing clicked: every setting each one has in
// the session has a control on the page, and no module's panel is mostly empty.
// The multiband compressor's controls are held by gui.mastering_comp_panel.
namespace duskstudio::scenario::gui
{
namespace
{
constexpr const char* kNoReadBack = "reads the editor window back through the X server";
// The most of a module's panel one flat rectangle of a single colour may cover.
constexpr double kFlatCeiling = 0.40;

struct Module
{
    int index;
    const char* panel;
    const char* title;
    // The session keys under "mastering" that belong to this module.
    const char* prefix;
    // Session keys the page deliberately has no control for.
    std::vector<std::string> uncontrolled;
    // Controls the page has that hold no session value.
    std::vector<std::string> viewOnly;
};

const std::vector<Module>& modules()
{
    static const std::vector<Module> all {
        { 0, "mastering-eq", "MASTERING EQ", "eq_",
          // The shelves keep a fixed slope; the rest are the Tube-EQ settings older
          // sessions carry, which nothing plays any more.
          { "eq_band_0_q", "eq_band_4_q", "eq_lf_boost", "eq_hf_boost", "eq_hf_atten", "eq_tube_drive",
            "eq_output_gain_db" },
          { "fft" } },
        { 2, "mastering-limiter", "LIMITER", "limiter_", {}, {} },
        { 3, "mastering-phase", "PHASE", "phase_", {}, { "phase_display", "phase_reset", "phase_mono" } },
    };
    return all;
}

std::string joined (const std::vector<std::string>& names)
{
    std::string text;
    for (const auto& name : names)
        text += (text.empty() ? "" : ", ") + name;
    return text;
}

std::string sizeText (GuiHost& host)
{
    const auto now = host.mainWindowSize();
    return now.size() >= 2 ? std::to_string (now[0]) + " x " + std::to_string (now[1]) : std::string ("unknown size");
}

std::optional<ScenarioResult> requireMasteringPanels (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native UI");
   #else
    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped() || engine.getMasteringPlayer().isPlaying() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and mastering player and no modal");
    if (! host.canEmbedPluginEditors())
        return ScenarioResult::skip ("requires a window the native mastering panels can embed into");
    return std::nullopt;
   #endif
}

struct Layout
{
    int width, height;
    std::string name;
    bool sideBySide = false;
};

// A window a pixel wider than the narrowest that shows every module side by side,
// as short as the window goes: every panel at its minimum width.
Layout narrowestSideBySide (GuiHost& host, const std::vector<int>& size)
{
    return { host.masteringSideBySideWidth() + 1, size[3], "the narrowest window with every module side by side", true };
}

// Wide enough for every module side by side, then the smallest window, where one
// module shows at a time.
std::vector<Layout> layouts (GuiHost& host, const std::vector<int>& size, bool all)
{
    std::vector<Layout> list { { 1424, 1120, "a 1424 x 1120 window" }, narrowestSideBySide (host, size) };
    if (all)
    {
        list.push_back ({ std::max (1440, size[0]), std::max (900, size[1]), "the window the case found, widened" });
        list.push_back ({ 1920, 1080, "a 1920 x 1080 window" });
        list.push_back ({ 1366, 768, "a 1366 x 768 window" });
        list.push_back ({ std::max (size[2], 1040), std::max (size[3], 900), "a compact window" });
    }
    list.push_back ({ size[2], size[3], "the smallest window" });
    return list;
}

// For each layout and each module: size the window, pick the module where the page
// shows one at a time, wait for its panel to draw twice, then check it.
void pushModuleSteps (GuiHost& host, ScenarioContext& ctx, std::vector<Step>& steps, const std::vector<Layout>& list,
                      const std::vector<int>& indices,
                      std::function<void (const Layout&, int module, const std::string& panel)> check)
{
    auto frames = std::make_shared<std::uint64_t> (0);
    for (const auto& layout : list)
        for (std::size_t m = 0; m < indices.size(); ++m)
        {
            const int module = indices[m];
            const std::string panel = module == 0 ? "mastering-eq" : module == 1 ? "mastering-comp"
                                    : module == 2 ? "mastering-limiter" : "mastering-phase";
            const bool resize = m == 0;
            steps.push_back ({ 0, [&host, &ctx, frames, layout, module, panel, resize]
            {
                if (resize)
                    ctx.expect (host.resizeMainWindow (layout.width, layout.height),
                                "could not size the window for " + layout.name);
                host.restoreMasteringModule (module);
                *frames = host.panelFrames (panel).value_or (0);
            } });
            steps.push_back ({ 400, [&host, &ctx, layout, module, panel, check]
            {
                if (layout.sideBySide)
                    ctx.expect (! host.masteringModulePickerShown(), "in " + layout.name + " (" + sizeText (host)
                                                                         + ") the page shows one module at a time");
                check (layout, module, panel);
            }, [&host, frames, panel]
            {
                const auto drawn = host.panelFrames (panel);
                if (! drawn) return false;
                // A layout switch that put up a new window starts its count again.
                if (*drawn < *frames) *frames = 0;
                return *drawn >= *frames + 2;
            }, "the " + panel + " panel did not draw in " + layout.name });
        }
}

// On the MASTERING page with nothing clicked, every setting the session holds for
// the EQ, the limiter and Phase has a control the module's panel drew whole, inside
// it and clear of the others, as do the panel's own switches: at each window size,
// and in the smallest window with each module picked in turn. A setting added to the
// session without a control on the page fails here.
std::optional<ScenarioResult> runMasteringModuleControls (GuiHost& host, ScenarioContext& ctx)
{
    if (auto skip = requireMasteringPanels (host, ctx)) return skip;
    const auto size = host.mainWindowSize();
    if (size.size() < 4) return ScenarioResult::fail ("the window size and its limits are unavailable");

    const auto saved = ctx.tempDir() / "keys" / "session.json";
    std::filesystem::create_directories (saved.parent_path());
    if (! SessionSerializer::save (ctx.session(), saved)) return ScenarioResult::fail ("could not save the session");
    const auto root = dusk::json::Json::parse (fileBytes (saved), nullptr, false);
    if (root.is_discarded() || ! dusk::json::has (root, "mastering") || ! root["mastering"].is_object())
        return ScenarioResult::fail ("the saved session has no mastering settings");
    std::vector<std::string> sessionKeys;
    for (auto it = root["mastering"].begin(); it != root["mastering"].end(); ++it)
        sessionKeys.push_back (it.key());

    auto expected = std::make_shared<std::vector<std::vector<std::string>>>();
    for (const auto& module : modules())
    {
        std::vector<std::string> names;
        const std::set<std::string> skipped (module.uncontrolled.begin(), module.uncontrolled.end());
        for (const auto& key : sessionKeys)
            if (key.rfind (module.prefix, 0) == 0 && skipped.count (key) == 0)
                names.push_back (key);
        if (names.empty())
            return ScenarioResult::fail (std::string ("the session holds no ") + module.prefix + " settings");
        for (const auto& key : module.uncontrolled)
            if (std::find (sessionKeys.begin(), sessionKeys.end(), key) == sessionKeys.end())
                return ScenarioResult::fail ("the session no longer holds " + key + "; take it off the list");
        names.insert (names.end(), module.viewOnly.begin(), module.viewOnly.end());
        expected->push_back (names);
    }

    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Mastering);
    if (host.masteringPanelsOpen() < 0) return ScenarioResult::skip ("this build has no native mastering panels");
    const int picked = host.masteringModuleIndex();
    ctx.cleanup ([&host, size, picked]
    {
        drainModals (host);
        host.restoreMasteringModule (picked);
        host.resizeMainWindow (size[0], size[1]);
    });

    std::vector<int> indices;
    for (const auto& module : modules())
        indices.push_back (module.index);
    auto steps = std::make_shared<std::vector<Step>>();
    pushModuleSteps (host, ctx, *steps, layouts (host, size, true), indices,
                     [&host, &ctx, expected] (const Layout& layout, int index, const std::string& panel)
    {
        const bool compact = host.masteringModulePickerShown();
        for (std::size_t m = 0; m < modules().size(); ++m)
        {
            const auto& module = modules()[m];
            if (module.index != index) continue;
            std::vector<std::string> hidden;
            for (const auto& name : (*expected)[m])
                if (! host.masteringControl (module.panel, name)) hidden.push_back (name);
            ctx.note (std::string (module.title) + " in " + layout.name + " (" + sizeText (host)
                      + (compact ? ", compact" : "") + "): " + std::to_string ((*expected)[m].size()) + " controls");
            ctx.expect (hidden.empty(), "in " + layout.name + (compact ? " with " + panel + " picked" : "")
                                            + ", " + module.title + " does not show " + joined (hidden) + " whole");
        }
    });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar masteringModuleControls { Scenario {
    "gui.mastering_module_controls", { "gui", "mastering" }, Needs::Engine | Needs::Gui,
    {}, {}, 90000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringModuleControls (host, ctx); }
} };

struct Pixels
{
    int width = 0, height = 0;
    std::vector<unsigned char> rgb;
};

std::optional<Pixels> readPpm (const std::filesystem::path& ppm)
{
    std::ifstream in (ppm, std::ios::binary);
    std::string magic;
    Pixels pixels;
    int maxValue = 0;
    in >> magic >> pixels.width >> pixels.height >> maxValue;
    if (! in || magic != "P6" || pixels.width <= 0 || pixels.height <= 0 || maxValue != 255) return std::nullopt;
    in.get();
    pixels.rgb.resize ((std::size_t) pixels.width * (std::size_t) pixels.height * 3u);
    if (! in.read (reinterpret_cast<char*> (pixels.rgb.data()), (std::streamsize) pixels.rgb.size()))
        return std::nullopt;
    return pixels;
}

bool panelReadable (GuiHost& host)
{
   #if defined (__linux__)
    return ! host.nativeWaylandWindow();
   #else
    (void) host;
    return false;
   #endif
}

// Each of the four module panels read back as the display shows it, in a 1424 x
// 1120 window and in the smallest one: no flat rectangle of one colour covers more
// than kFlatCeiling of a panel. A module whose controls went missing, or sit behind
// a click, leaves most of its panel bare and fails here.
std::optional<ScenarioResult> runMasteringPanelsFilled (GuiHost& host, ScenarioContext& ctx)
{
    if (auto skip = requireMasteringPanels (host, ctx)) return skip;
    if (! panelReadable (host)) return ScenarioResult::skip (kNoReadBack);
    const auto size = host.mainWindowSize();
    if (size.size() < 4) return ScenarioResult::fail ("the window size and its limits are unavailable");
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Mastering);
    if (host.masteringPanelsOpen() < 0) return ScenarioResult::skip ("this build has no native mastering panels");
    const int picked = host.masteringModuleIndex();
    ctx.cleanup ([&host, size, picked]
    {
        drainModals (host);
        host.restoreMasteringModule (picked);
        host.resizeMainWindow (size[0], size[1]);
    });

    auto steps = std::make_shared<std::vector<Step>>();
    auto shot = std::make_shared<int> (0);
    pushModuleSteps (host, ctx, *steps, layouts (host, size, false), { 0, 1, 2, 3 },
                     [&host, &ctx, shot] (const Layout& layout, int, const std::string& panel)
    {
        const auto ppm = ctx.tempDir() / ("panel-" + std::to_string ((*shot)++) + ".ppm");
        if (! ctx.expect (host.capturePanel (panel, ppm), "could not read the " + panel + " panel back in "
                                                              + layout.name))
            return;
        const auto image = readPpm (ppm);
        if (! ctx.expect (image.has_value(), "the " + panel + " capture in " + layout.name + " does not read"))
            return;
        const auto flat = pixels::largestUniformRect (image->rgb.data(), image->width, image->height);
        const double share = (double) flat.area() / ((double) image->width * (double) image->height);
        char text[160];
        std::snprintf (text, sizeof text, "%s in %s (%s, %d x %d): largest flat rectangle %d x %d at %d,%d, %.3f",
                       panel.c_str(), layout.name.c_str(), sizeText (host).c_str(), image->width, image->height,
                       flat.width, flat.height, flat.x, flat.y, share);
        ctx.note (text);
        ctx.expect (share <= kFlatCeiling, std::string ("the ") + panel + " panel is mostly empty in " + layout.name
                                               + ": " + text);
        std::error_code ignored;
        std::filesystem::remove (ppm, ignored);
    });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar masteringPanelsFilled { Scenario {
    "gui.mastering_panels_filled", { "gui", "mastering" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringPanelsFilled (host, ctx); }
} };

const char* panelTag (int module)
{
    static constexpr const char* tags[] = { "mastering-eq", "mastering-comp", "mastering-limiter", "mastering-phase" };
    return tags[std::clamp (module, 0, 3)];
}

// A dropdown on the MASTERING page: one a module's panel holds (its list opens
// inside that panel), or one of the page's own pickers (its list is a modal of
// the window and may lie over the panels).
struct Dropdown
{
    std::string name;
    // The panel that holds it, or empty for a page picker.
    std::string owner;
    std::string control;
    std::function<bool (GuiHost&)> openPage;
    std::function<int (GuiHost&, ScenarioContext&)> choice;
    int choices;
};

std::vector<Dropdown> dropdowns()
{
    return {
        { "the multiband PRESET picker", "mastering-comp", "comp_preset", {},
          [] (GuiHost& host, ScenarioContext&) { return host.masteringCompPreset(); }, 1 + 9 },
        { "the limiter Mode picker", "mastering-limiter", "limiter_mode", {},
          [] (GuiHost&, ScenarioContext& ctx) { return ctx.session().mastering().limiterMode.load(); }, 3 },
        { "the PHASE display picker", "mastering-phase", "phase_display", {},
          [] (GuiHost& host, ScenarioContext&)
          {
              return host.masteringControl ("mastering-phase", "phase_display:0") ? 0
                   : host.masteringControl ("mastering-phase", "phase_display:1") ? 1 : -1;
          }, 2 },
        { "the loudness Target picker", "", "", [] (GuiHost& host) { return host.clickMasteringTarget(); },
          [] (GuiHost&, ScenarioContext& ctx) { return ctx.session().mastering().targetPresetIndex.load(); }, 4 },
        { "the compact module picker", "", "", [] (GuiHost& host) { return host.clickMasteringModulePicker(); },
          [] (GuiHost& host, ScenarioContext&) { return host.masteringModuleIndex(); }, 4 },
    };
}

bool intersects (const std::vector<int>& a, const std::vector<int>& b)
{
    return a.size() >= 4 && b.size() >= 4 && a[0] < b[0] + b[2] && b[0] < a[0] + a[2]
        && a[1] < b[1] + b[3] && b[1] < a[1] + a[3];
}

// What an open page list takes over: its body and the backdrop framing it
// (EmbeddedModal's kBackdropMargin), which hides a module it overlaps too.
std::vector<int> listCover (const std::vector<int>& list)
{
    static constexpr int kFrame = 6;
    if (list.size() < 4) return {};
    return { list[0] - kFrame, list[1] - kFrame, list[2] + 2 * kFrame, list[3] + 2 * kFrame };
}

std::vector<std::string> shownPanels (GuiHost& host)
{
    std::vector<std::string> shown;
    for (int m = 0; m < 4; ++m)
        if (host.panelFrames (panelTag (m))) shown.push_back (panelTag (m));
    return shown;
}

struct DropdownRun
{
    std::vector<std::string> shown;
    int before = -1;
    int wanted = -1;
};

// While the dropdown's list is open: a native panel paints above any list, so a
// module a page list overlaps at all must be hidden, and every module it does not
// overlap, or that a panel's own list sits in, must still be drawn.
void expectModulesBesideList (GuiHost& host, ScenarioContext& ctx, const Dropdown& dropdown,
                              const std::vector<std::string>& shown, const std::string& where)
{
    const bool page = dropdown.owner.empty();
    const auto cover = page ? listCover (host.modalLayout()) : std::vector<int> {};
    for (const auto& panel : shown)
    {
        const auto area = host.masteringPanelArea (panel);
        const bool up = host.panelFrames (panel).has_value();
        if (page && intersects (cover, area))
        {
            char text[200];
            std::snprintf (text, sizeof text, "%s in %s lies over %s (%d x %d), which is %s", dropdown.name.c_str(),
                           where.c_str(), panel.c_str(), area[2], area[3], up ? "still drawn above it" : "hidden");
            ctx.note (text);
            ctx.expect (! up, std::string ("the open list is buried: ") + text);
            continue;
        }
        ctx.expect (up, panel + " was hidden while " + dropdown.name + " was open in " + where
                            + ", though its list does not lie over it");
    }
}

// Opens the dropdown as the pointer does, checks the page while its list is open,
// picks the next entry from the keyboard and checks the page once it has closed.
void pushDropdownSteps (GuiHost& host, ScenarioContext& ctx, std::vector<Step>& steps, const Dropdown& dropdown,
                        const std::string& where, std::shared_ptr<int> shot)
{
    auto run = std::make_shared<DropdownRun>();
    const bool page = dropdown.owner.empty();
    const auto open = [&host, dropdown, page]
    {
        return page ? ! host.modalStackEmpty() && host.modalHasKeyboardFocus()
                    : host.masteringPanelCapturesKeyboard (dropdown.owner);
    };
    steps.push_back ({ 100, [&host, &ctx, dropdown, run, where, page]
    {
        run->shown = shownPanels (host);
        run->before = dropdown.choice (host, ctx);
        run->wanted = run->before < dropdown.choices - 1 ? run->before + 1 : run->before - 1;
        if (page)
            ctx.expect (dropdown.openPage (host), dropdown.name + " is not on screen in " + where);
        else
            ctx.expect (host.masteringPanelPointer (dropdown.owner, dropdown.control, 0.5f, true),
                        dropdown.name + " is not on screen in " + where);
    }, [&host] { return host.masteringPanelsOpen() > 0; }, "no mastering panel is up in " + where });
    if (! page)
        steps.push_back ({ 60, [&host, &ctx, dropdown, where]
        {
            if (! host.masteringControl (dropdown.owner, dropdown.control))
                ctx.expect (false, dropdown.owner + " went off the screen when " + dropdown.name + " was pressed in "
                                       + where);
            else
                ctx.expect (host.masteringPanelPointer (dropdown.owner, dropdown.control, 0.5f, false),
                            dropdown.name + " lost the pointer in " + where);
        } });
    // A panel the page hides goes down on the next tick, so the check waits a
    // few of them once the list is up.
    steps.push_back ({ 300, [&host, &ctx, dropdown, run, where, page, shot]
    {
        expectModulesBesideList (host, ctx, dropdown, run->shown, where);
        if (! page && panelReadable (host))
        {
            const auto ppm = ctx.tempDir() / ("dropdown-" + std::to_string ((*shot)++) + ".ppm");
            if (ctx.expect (host.capturePanel (dropdown.owner, ppm), dropdown.owner + " could not be read back with "
                                                                     + dropdown.name + " open in " + where))
            {
                const auto image = readPpm (ppm);
                if (ctx.expect (image.has_value(), "the " + dropdown.owner + " capture does not read"))
                {
                    const double share = pixels::largestUniformShare (image->rgb.data(), image->width, image->height);
                    char text[160];
                    std::snprintf (text, sizeof text, "%s with %s open in %s: largest flat share %.3f",
                                   dropdown.owner.c_str(), dropdown.name.c_str(), where.c_str(), share);
                    ctx.note (text);
                    ctx.expect (share <= kFlatCeiling, std::string ("the panel holding the open list is mostly empty: ")
                                                           + text);
                }
                std::error_code ignored;
                std::filesystem::remove (ppm, ignored);
            }
        }
        // A panel's own list is picked with the pointer; the page's lists from the
        // keyboard, from the top.
        if (page)
        {
            ctx.expect (host.pressPeerKey ("home", 0), dropdown.name + " took no Home");
            for (int i = 0; i < run->wanted; ++i)
                ctx.expect (host.pressPeerKey ("cursor down", 0), dropdown.name + " took no Down");
            ctx.expect (host.pressPeerKey ("return", '\r'), dropdown.name + " took no Return");
        }
        else
            ctx.expect (host.masteringPanelPointer (dropdown.owner, "list:" + std::to_string (run->wanted), 0.5f, true),
                        dropdown.name + " shows no entry " + std::to_string (run->wanted) + " in " + where);
    }, open, dropdown.name + " did not open its list in " + where });
    if (! page)
        steps.push_back ({ 60, [&host, &ctx, dropdown, run, where]
        {
            ctx.expect (host.masteringPanelPointer (dropdown.owner, "list:" + std::to_string (run->wanted), 0.5f, false),
                        dropdown.name + " lost entry " + std::to_string (run->wanted) + " in " + where);
        } });
    steps.push_back ({ 300, [&host, &ctx, dropdown, run, where]
    {
        ctx.expect (dropdown.choice (host, ctx) == run->wanted,
                    dropdown.name + " in " + where + " shows choice " + std::to_string (dropdown.choice (host, ctx))
                        + ", not the " + std::to_string (run->wanted) + " picked");
    }, [&host, &ctx, dropdown, run, open, page]
    {
        if (open() || (page && ! host.modalStackEmpty()) || dropdown.choice (host, ctx) != run->wanted) return false;
        const auto now = shownPanels (host);
        return ! now.empty() && (dropdown.name != "the compact module picker" ? now == run->shown : now.size() == 1);
    }, dropdown.name + " did not close with every panel drawn again in " + where });
}

// A page picker opened, then pressed again where the first press was. A list that
// opens over its picker puts the current choice there, so that press picks it again:
// the list closes and the choice holds. A list that opens clear of its picker is
// closed with Escape.
void pushSecondPressSteps (GuiHost& host, ScenarioContext& ctx, std::vector<Step>& steps, const Dropdown& dropdown,
                           bool target, const std::string& where)
{
    auto run = std::make_shared<DropdownRun>();
    steps.push_back ({ 100, [&host, &ctx, dropdown, run, where]
    {
        run->shown = shownPanels (host);
        run->before = dropdown.choice (host, ctx);
        ctx.expect (dropdown.openPage (host), dropdown.name + " is not on screen in " + where);
    }, [&host] { return host.modalStackEmpty() && host.masteringPanelsOpen() > 0; },
       "the mastering panels did not settle before " + dropdown.name + " opened in " + where });
    steps.push_back ({ 300, [&host, &ctx, dropdown, run, target, where]
    {
        expectModulesBesideList (host, ctx, dropdown, run->shown, where);
        const auto list = host.modalLayout();
        const auto at = host.masteringPickerPoint (target);
        const bool over = list.size() >= 4 && at.size() >= 2 && at[0] >= list[0] && at[0] < list[0] + list[2]
                       && at[1] >= list[1] && at[1] < list[1] + list[3];
        ctx.note (dropdown.name + " showing choice " + std::to_string (run->before) + " in " + where
                  + (over ? " opened over its picker" : " opened clear of its picker"));
        if (over)
            ctx.expect (dropdown.openPage (host), dropdown.name + " could not be pressed again in " + where);
        else
            ctx.expect (host.pressPeerKey ("escape"), dropdown.name + " took no Escape in " + where);
    }, [&host] { return ! host.modalStackEmpty() && host.modalHasKeyboardFocus(); },
       dropdown.name + " did not open its list in " + where });
    steps.push_back ({ 300, [&host, &ctx, dropdown, run, where]
    {
        ctx.expect (dropdown.choice (host, ctx) == run->before,
                    dropdown.name + " pressed again in " + where + " changed choice " + std::to_string (run->before)
                        + " to " + std::to_string (dropdown.choice (host, ctx)));
    }, [&host, run] { return host.modalStackEmpty() && shownPanels (host) == run->shown; },
       dropdown.name + " pressed again did not close with every panel drawn again in " + where });
}

// Every dropdown on the MASTERING page, opened through the pointer: while its list
// is up the module that holds it stays drawn and full, no module a page list lies
// over is left drawn above it, none it does not lie over is hidden, and once an
// entry is picked it has taken and every panel is drawn again. The loudness Target
// list, opened with each of its choices showing, and the compact picker's, pressed
// again where they were opened, close and keep their choice. In a 1424 x 1120 window, and in the smallest window, where one
// module shows at a time, with the multiband, the limiter and Phase each picked.
// Panels are read back only where the display server allows it.
std::optional<ScenarioResult> runMasteringDropdownsKeepModules (GuiHost& host, ScenarioContext& ctx)
{
    if (auto skip = requireMasteringPanels (host, ctx)) return skip;
    const auto size = host.mainWindowSize();
    if (size.size() < 4) return ScenarioResult::fail ("the window size and its limits are unavailable");
    auto& mastering = ctx.session().mastering();
    for (std::size_t i = 0; i < kCompParamCount; ++i)
        ctx.keep (mastering.comp.atom ((CompParamId) i));
    ctx.keep (mastering.limiterMode);
    ctx.keep (mastering.targetPresetIndex);
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Mastering);
    if (host.masteringPanelsOpen() < 0) return ScenarioResult::skip ("this build has no native mastering panels");
    const int picked = host.masteringModuleIndex();
    const int target = mastering.targetPresetIndex.load();
    const int preset = host.masteringCompPreset();
    ctx.cleanup ([&host, size, picked, target, preset]
    {
        drainModals (host);
        host.restoreMasteringCompPreset (preset);
        host.restoreMasteringTarget (target);
        host.restoreMasteringModule (picked);
        host.resizeMainWindow (size[0], size[1]);
    });

    const auto all = dropdowns();
    auto shot = std::make_shared<int> (0);
    auto steps = std::make_shared<std::vector<Step>>();
    const auto settle = [&host, &ctx, steps] (int width, int height, int module, const std::string& where)
    {
        steps->push_back ({ 0, [&host, &ctx, width, height, module, where]
        {
            ctx.expect (host.resizeMainWindow (width, height), "could not size the window for " + where);
            host.restoreMasteringModule (module);
        } });
        steps->push_back ({ 500, [] {}, [&host, module]
        {
            return host.panelFrames (panelTag (module)).value_or (0) > 1 && host.modalStackEmpty();
        }, "the mastering panels did not settle in " + where });
    };

    const auto pressTwice = [&host, &ctx, steps, all] (int choice, const std::string& where)
    {
        steps->push_back ({ 0, [&host, choice] { host.restoreMasteringTarget (choice); } });
        steps->push_back ({ 0, [] {}, [&ctx, choice]
        { return ctx.session().mastering().targetPresetIndex.load() == choice; },
           "the loudness Target did not take choice " + std::to_string (choice) });
        pushSecondPressSteps (host, ctx, *steps, all[3], true, where);
    };

    const std::string wide = "a 1424 x 1120 window";
    settle (1424, 1120, 1, wide);
    for (int d = 0; d < 4; ++d)
        pushDropdownSteps (host, ctx, *steps, all[(std::size_t) d], wide, shot);
    for (int choice = 0; choice < all[3].choices; ++choice)
        pressTwice (choice, wide);
    for (const int module : { 1, 2, 3 })
    {
        const std::string where = "the smallest window with " + std::string (panelTag (module)) + " picked";
        settle (size[2], size[3], module, where);
        pushDropdownSteps (host, ctx, *steps, all[(std::size_t) module - 1], where, shot);
        if (module == 1)
        {
            pushDropdownSteps (host, ctx, *steps, all[3], where, shot);
            for (int choice = 0; choice < all[3].choices; ++choice)
                pressTwice (choice, where);
        }
        settle (size[2], size[3], module, where);
        pushDropdownSteps (host, ctx, *steps, all[4], where, shot);
        settle (size[2], size[3], module, where);
        pushSecondPressSteps (host, ctx, *steps, all[4], false, where);
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar masteringDropdownsKeepModules { Scenario {
    "gui.mastering_dropdowns_keep_modules", { "gui", "mastering" }, Needs::Engine | Needs::Gui,
    {}, {}, 120000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringDropdownsKeepModules (host, ctx); }
} };

// A list left open in a module's panel closes when the user turns to the rest of the
// page, whether the press there reaches the panel as a lost keyboard focus or not at
// all (Windows), or lands in another module's panel, and the keys go back to the
// shell: Down and Return pick nothing, and Space plays the mix. For the multiband
// presets, the limiter's mode and the Phase display, in a 1424 x 1120 window.
std::optional<ScenarioResult> runMasteringListsCloseOutside (GuiHost& host, ScenarioContext& ctx)
{
    if (auto skip = requireMasteringPanels (host, ctx)) return skip;
    const auto size = host.mainWindowSize();
    if (size.size() < 2) return ScenarioResult::fail ("the window size is unavailable");
    auto& mastering = ctx.session().mastering();
    auto& player = ctx.engine().getMasteringPlayer();
    for (std::size_t i = 0; i < kCompParamCount; ++i)
        ctx.keep (mastering.comp.atom ((CompParamId) i));
    ctx.keep (mastering.limiterMode);
    ctx.keep (mastering.targetPresetIndex);
    ctx.keep (mastering.eqBandGainDb[2]);
    keepStage (host, ctx);
    const auto originalFile = player.getLoadedFile();
    const auto originalSource = mastering.sourceFile;
    const auto originalPosition = player.getPlayhead();
    ctx.cleanup ([&host, &player, &mastering, originalFile, originalSource, originalPosition, size]
    {
        player.stop();
        drainModals (host);
        if (originalFile.existsAsFile()) player.loadFile (originalFile);
        else player.unloadFile();
        player.setPlayhead (originalPosition);
        mastering.sourceFile = originalSource;
        host.refreshMasteringSource();
        host.resizeMainWindow (size[0], size[1]);
    });
    if (! host.resizeMainWindow (1424, 1120)) return ScenarioResult::fail ("could not size the window");
    host.switchToStage (GuiHost::Stage::Mastering);
    if (host.masteringPanelsOpen() < 0) return ScenarioResult::skip ("this build has no native mastering panels");
    const auto mix = ctx.tempDir() / "mix.wav";
    if (! writeRampFixture (mix, 48000.0, 480000)) return ScenarioResult::fail ("could not write the mix");
    if (! host.loadMasteringFile (mix)) return ScenarioResult::fail ("could not load the mix");

    const auto all = dropdowns();
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [] {}, [&host]
    { return host.masteringPanelsOpen() == host.masteringPanelsExpected(); }, "the mastering panels never opened" });
    // A press on an EQ dial that does not move changes nothing there.
    static constexpr const char* kOtherPanel = "mastering-eq";
    static constexpr const char* kOtherControl = "eq_band_2_gain_db";
    enum class Away { waveform, focus, otherPanel };
    for (std::size_t d = 0; d < 3; ++d)
        for (const auto away : { Away::waveform, Away::focus, Away::otherPanel })
        {
            const auto& dropdown = all[d];
            const std::string how = away == Away::focus ? "its panel lost the keyboard focus"
                                  : away == Away::waveform ? "the waveform was pressed"
                                                           : "the EQ panel was pressed";
            auto before = std::make_shared<int> (-1);
            steps->push_back ({ 100, [&host, &ctx, dropdown, before]
            {
                *before = dropdown.choice (host, ctx);
                ctx.expect (host.masteringPanelPointer (dropdown.owner, dropdown.control, 0.5f, true),
                            dropdown.name + " is not on screen");
            }, [&host, &player] { return ! player.isPlaying() && host.modalStackEmpty(); },
               "the mix did not stop" });
            steps->push_back ({ 60, [&host, &ctx, dropdown]
            {
                ctx.expect (host.masteringPanelPointer (dropdown.owner, dropdown.control, 0.5f, false),
                            dropdown.name + " lost the pointer");
            } });
            steps->push_back ({ 100, [&host, &ctx, away, dropdown, how]
            {
                const bool done = away == Away::focus      ? host.panelInput (dropdown.owner, "focus-out")
                                : away == Away::waveform   ? host.clickMasteringWaveform (0.5f)
                                                           : host.masteringPanelPointer (kOtherPanel, kOtherControl, 0.5f, true);
                ctx.expect (done, "could not act as when " + how);
            }, [&host, dropdown]
            {
                return host.masteringPanelCapturesKeyboard (dropdown.owner)
                    && host.masteringControl (dropdown.owner, "list:0").has_value();
            }, dropdown.name + " did not open its list" });
            if (away == Away::otherPanel)
                steps->push_back ({ 60, [&host, &ctx]
                {
                    ctx.expect (host.masteringPanelPointer (kOtherPanel, kOtherControl, 0.5f, false),
                                "the EQ panel lost its dial");
                } });
            // The shell has nothing on Down or Return, so neither reports being taken.
            steps->push_back ({ 100, [&host]
            {
                host.pressPeerKey ("cursor down", 0);
                host.pressPeerKey ("return", '\r');
            }, [&host, dropdown]
            {
                return ! host.masteringPanelCapturesKeyboard (dropdown.owner)
                    && ! host.masteringControl (dropdown.owner, "list:0").has_value();
            }, dropdown.name + " stayed open, holding the keyboard, after " + how });
            steps->push_back ({ 300, [&host, &ctx, dropdown, before, how]
            {
                ctx.expect (dropdown.choice (host, ctx) == *before,
                            "Down and Return after " + how + " moved " + dropdown.name + " from "
                                + std::to_string (*before) + " to " + std::to_string (dropdown.choice (host, ctx)));
                ctx.expect (host.pressPeerKey ("spacebar", ' '), "the window took no Space after " + how);
            } });
            steps->push_back ({ 0, [&player] { player.stop(); }, [&player] { return player.isPlaying(); },
                               "Space did not play the mix after " + dropdown.name + " closed when " + how });
        }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar masteringListsCloseOutside { Scenario {
    "gui.mastering_lists_close_outside", { "gui", "mastering" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringListsCloseOutside (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
