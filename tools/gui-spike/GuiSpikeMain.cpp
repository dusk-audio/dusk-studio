// Non-shipping gate spike for the GUI tower (issue #301). It puts one Dusk Studio channel
// strip on a native application window driven by the Dusk Audio Framework's DGL/pugl stack
// and Dear ImGui, with no JUCE anywhere in the target, and reports what a 60 Hz frame of it
// costs. It is a measuring instrument, not a step toward the shipping UI.

#include "ChannelStripView.h"
#include "StripModel.h"
#include "StripTheme.h"

#include <Application.hpp>
#include <OpenGL.hpp>
#include <DearImGui.hpp>
#include <DearImGui/imgui_internal.h>
#ifndef DGL_NO_SHARED_RESOURCES
# include "src/Resources.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#if defined (__unix__)
# include <sys/resource.h>
#endif

namespace
{

struct Options
{
    double seconds = 0.0;      // 0 runs until the window is closed
    int    strips = 1;
    int    reopen = 0;         // window close/reopen cycles before measuring
    double scaleOverride = 0.0;
    int    width = 0;
    int    height = 0;
    bool   resizeSweep = false;
    bool   quiet = false;
    std::string capturePath;
    int    captureFrame = 60;
    bool   selftest = false;
    int    demo = 0;           // 1 opens the modal, 2 opens the insert menu
};

double cpuSeconds()
{
#if defined (__unix__)
    rusage ru {};
    getrusage (RUSAGE_SELF, &ru);
    return static_cast<double> (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec)
         + 1.0e-6 * static_cast<double> (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
#else
    return 0.0;
#endif
}

double percentile (std::vector<double>& v, double p)
{
    if (v.empty())
        return 0.0;
    std::sort (v.begin(), v.end());
    const auto i = std::min (v.size() - 1,
                             static_cast<std::size_t> (p * static_cast<double> (v.size() - 1)));
    return v[i];
}

const char* glStr (unsigned int name)
{
    const auto* const s = reinterpret_cast<const char*> (glGetString (name));
    return s != nullptr ? s : "(null)";
}

} // namespace

class SpikeWindow final : public DGL::ImGuiStandaloneWindow
{
public:
    SpikeWindow (DGL::Application& app, const Options& opts)
        : DGL::ImGuiStandaloneWindow (app, 13.0f), application (app), options (opts)
    {
        setTitle ("Dusk Studio GUI gate spike");
        setResizable (true);

        const float scale = static_cast<float> (options.scaleOverride > 0.0
                                                ? options.scaleOverride : getScaleFactor());
        const int w = options.width > 0 ? options.width
                    : static_cast<int> ((duskspike::layout::kStripWidth + 24.0f)
                                        * static_cast<float> (options.strips) * scale);
        const int h = options.height > 0 ? options.height
                    : static_cast<int> ((duskspike::layout::kMinStripHeight + 20.0f) * scale);
        setSize (static_cast<uint> (w), static_cast<uint> (h));

        strips.resize (static_cast<std::size_t> (std::max (1, options.strips)));
        views.resize (strips.size());
        for (std::size_t i = 0; i < strips.size(); ++i)
        {
            strips[i].reset (new duskspike::StripParams());
            char name[32];
            std::snprintf (name, sizeof name, "TRK %02d", static_cast<int> (i) + 1);
            strips[i]->name = name;
            sources.emplace_back (new duskspike::StubMeterSource (*strips[i]));
            sources.back()->start();
        }

        buildFonts (scale);
    }

    ~SpikeWindow() override
    {
        for (auto& s : sources)
            s->stop();
    }

    void beginMeasuring() { measuring = true; frameDeltas.clear(); }

    const std::vector<double>& deltas() const noexcept { return frameDeltas; }
    int framesDrawn() const noexcept { return frames; }
    const std::string& rendererName() const noexcept { return renderer; }
    int lastVertices() const noexcept { return vertices; }

    // What a file dialog opened through xdg-desktop-portal would be parented to.
    std::string portalHandle() const { return Window::getPortalParentHandle(); }

protected:
    // pugl has no screenshot hook, so the spike takes its own: the base onDisplay has
    // finished submitting the frame and the buffer has not been swapped yet, which is the
    // only point where a readback is the frame the compositor is about to show.
    void onDisplay() override
    {
        // The widget kit releases the scoped graphics context at the end of its constructor,
        // so the first frame is the earliest point where GL can be asked what it is.
        if (renderer.empty())
            reportGraphics();

        DGL::ImGuiStandaloneWindow::onDisplay();

        if (options.capturePath.empty() || frames != options.captureFrame)
            return;

        const int w = static_cast<int> (getWidth());
        const int h = static_cast<int> (getHeight());
        std::vector<unsigned char> pixels (static_cast<std::size_t> (w) * h * 3);

        glFinish();
        glPixelStorei (GL_PACK_ALIGNMENT, 1);
        glReadPixels (0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

        if (FILE* const f = std::fopen (options.capturePath.c_str(), "wb"))
        {
            std::fprintf (f, "P6\n%d %d\n255\n", w, h);
            for (int row = h - 1; row >= 0; --row)
                std::fwrite (pixels.data() + static_cast<std::size_t> (row) * w * 3, 1,
                             static_cast<std::size_t> (w) * 3, f);
            std::fclose (f);
            std::printf ("[spike] captured frame %d to %s (%dx%d)\n",
                         frames, options.capturePath.c_str(), w, h);
        }
    }

    // Counted so a run can say whether the compositor delivered any input at all, rather
    // than leaving "input works" to be inferred from the code compiling.
    bool onMouse (const DGL::Widget::MouseEvent& e) override
    { ++pointerEvents; return DGL::ImGuiStandaloneWindow::onMouse (e); }

    bool onMotion (const DGL::Widget::MotionEvent& e) override
    { ++pointerEvents; return DGL::ImGuiStandaloneWindow::onMotion (e); }

    bool onScroll (const DGL::Widget::ScrollEvent& e) override
    { ++pointerEvents; return DGL::ImGuiStandaloneWindow::onScroll (e); }

    bool onKeyboard (const DGL::Widget::KeyboardEvent& e) override
    { ++keyEvents; return DGL::ImGuiStandaloneWindow::onKeyboard (e); }

    bool onCharacterInput (const DGL::Widget::CharacterInputEvent& e) override
    { ++textEvents; return DGL::ImGuiStandaloneWindow::onCharacterInput (e); }

    void onFocusChanged (const DGL::Widget::FocusEvent& e) override
    {
        if (e.focus) ++focusInEvents; else ++focusOutEvents;
        DGL::ImGuiStandaloneWindow::onFocusChanged (e);
    }

    void onImGuiDisplay() override
    {
        if (options.selftest)
            driveSelftest();

        const auto now = std::chrono::steady_clock::now();
        if (frames > 0 && measuring)
            frameDeltas.push_back (std::chrono::duration<double, std::milli> (now - lastFrame).count());
        lastFrame = now;
        ++frames;

        if (options.seconds > 0.0)
        {
            if (frames == 1)
                startedAt = now;
            else if (std::chrono::duration<double> (now - startedAt).count() >= options.seconds)
                application.quit();
        }

        const float scale = static_cast<float> (options.scaleOverride > 0.0
                                                ? options.scaleOverride : getScaleFactor());
        const float w = static_cast<float> (getWidth());
        const float h = static_cast<float> (getHeight());

        if (options.resizeSweep && (frames % 30) == 0 && frames < 600)
        {
            const uint sweep = static_cast<uint> (600 + (frames / 30) % 8 * 40);
            setSize (sweep, static_cast<uint> (h));
        }

        ImGui::SetNextWindowPos (ImVec2 (0.0f, 0.0f));
        ImGui::SetNextWindowSize (ImVec2 (w, h));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (0.0f, 0.0f));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor (ImGuiCol_WindowBg,
                               ImGui::ColorConvertU32ToFloat4 (IM_COL32 (0x12, 0x12, 0x14, 255)));
        ImGui::Begin ("##console", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                      | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
                      | ImGuiWindowFlags_NoBringToFrontOnFocus);

        auto* const dl = ImGui::GetWindowDrawList();

        const float stripW = duskspike::layout::kStripWidth * scale;
        const float gap = 4.0f * scale;
        const float stripH = std::max (duskspike::layout::kMinStripHeight * scale, h - 12.0f * scale);
        float x = 6.0f * scale;

        duskspike::StripFrameResult firstResult;
        for (std::size_t i = 0; i < views.size(); ++i)
        {
            const auto r = views[i].draw (*dl, ImVec2 (x, 6.0f * scale), stripW, stripH,
                                          scale, *strips[i], fonts);
            if (i == 0)
                firstResult = r;
            x += stripW + gap;
        }

        vertices = dl->VtxBuffer.Size;

        // Hide the pointer for the length of a knob drag, the idiom every DAW knob uses.
        bool dragging = false;
        for (const auto& v : views)
            dragging = dragging || v.isDragging();

        if (dragging != pointerHidden)
        {
            pointerHidden = dragging;
            // Both bases carry a setCursor, so the window's has to be named.
            cursorCalls += Window::setCursor (dragging ? DGL::kMouseCursorNone
                                                       : DGL::kMouseCursorArrow) ? 1 : 0;
        }

        // Keyboard: the shell owns the shortcuts a view would not, so a key press proves it
        // reaches the application and not only the focused text field.
        // Not gated on io.WantCaptureKeyboard: the widgets library forces keyboard nav on,
        // which pins that flag true, so it says nothing about whether the application may
        // take the key.
        if (! views[0].isEditingName() && ! ImGui::IsAnyItemActive())
        {
            auto& p = *strips[0];
            if (ImGui::IsKeyPressed (ImGuiKey_M, false))
                p.mute.store (! p.mute.load(), std::memory_order_relaxed);
            if (ImGui::IsKeyPressed (ImGuiKey_S, false))
                p.solo.store (! p.solo.load(), std::memory_order_relaxed);
            if (ImGui::IsKeyPressed (ImGuiKey_UpArrow, true))
                p.faderDb.store (std::min (6.0f, p.faderDb.load() + 0.5f), std::memory_order_relaxed);
            if (ImGui::IsKeyPressed (ImGuiKey_DownArrow, true))
                p.faderDb.store (std::max (-90.0f, p.faderDb.load() - 0.5f), std::memory_order_relaxed);
            if (ImGui::IsKeyPressed (ImGuiKey_Escape, false) && options.seconds <= 0.0)
                application.quit();
        }

        // A popup id is derived from the id stack of the window that is current when it is
        // opened, so both halves have to happen inside this window: the selftest runs before
        // the console is begun and can only ask for a popup, never open one itself.
        if (firstResult.openInsertMenu || std::exchange (requestInsertMenu, false)
            || (options.demo == 2 && frames == 30))
            ImGui::OpenPopup ("##insertMenu");

        insertMenuUp = ImGui::BeginPopup ("##insertMenu");
        if (insertMenuUp)
        {
            ImGui::TextDisabled ("Insert slot");
            ImGui::Separator();
            const char* items[] = { "Baroness", "Langevin", "Tape Machine", "Clear slot" };
            for (const auto* item : items)
                if (ImGui::MenuItem (item))
                    menuChoice = item;
            ImGui::EndPopup();
        }

        if (firstResult.openIoModal || std::exchange (requestIoModal, false)
            || (options.demo == 1 && frames == 30))
            ImGui::OpenPopup ("Channel input");
        ioModalUp = drawIoModal (w, h, scale);

        drawHud (w, scale);

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar (2);
    }

public:
    int pointerEvents = 0, keyEvents = 0, textEvents = 0;
    int focusInEvents = 0, focusOutEvents = 0, cursorCalls = 0;
    std::vector<std::string> selftestLog;

private:
    // A headless compositor has no seat, so nothing arrives through the Wayland input path.
    // This injects at the ImGui event queue instead: it proves the widget layer reacts, and
    // deliberately proves nothing about pugl's seat handling, which is bench work.
    void driveSelftest()
    {
        auto& io = ImGui::GetIO();
        auto& p = *strips[0];
        const float scale = static_cast<float> (options.scaleOverride > 0.0
                                                ? options.scaleOverride : getScaleFactor());
        auto note = [this] (const char* what, bool ok)
        { selftestLog.emplace_back (std::string (ok ? "pass  " : "FAIL  ") + what); };

        // HF gain knob, centre of the first EQ band row's GAIN column.
        const ImVec2 hfGain (6.0f * scale + (4.0f + 3.0f + 28.0f + (182.0f - 34.0f) / 6.0f) * scale,
                             6.0f * scale + (4.0f + 6.0f + 20.0f + 2.0f + 18.0f + 3.0f + 18.0f
                                             + 18.0f + 2.0f + 16.0f + 10.0f + 40.0f + 5.0f
                                             + 10.0f + 13.0f) * scale);

        switch (selftestStep++)
        {
            case 10: io.AddMousePosEvent (hfGain.x, hfGain.y); break;
            case 12: selftestBefore = p.hfGainDb.load(); io.AddMouseButtonEvent (0, true); break;
            case 14: io.AddMousePosEvent (hfGain.x, hfGain.y - 40.0f * scale); break;
            case 16: io.AddMouseButtonEvent (0, false); break;
            case 18: note ("knob drag changes value", p.hfGainDb.load() > selftestBefore + 0.5f); break;

            case 22: io.AddMouseWheelEvent (0.0f, 3.0f); break;
            case 24: note ("knob wheel changes value", p.hfGainDb.load() != selftestBefore); break;

            case 28: io.AddMousePosEvent (hfGain.x, hfGain.y); break;
            case 30: io.AddMouseButtonEvent (0, true); break;
            case 31: io.AddMouseButtonEvent (0, false); break;
            case 32: io.AddMouseButtonEvent (0, true); break;
            case 33: io.AddMouseButtonEvent (0, false); break;
            case 36: note ("knob double-click resets", std::fabs (p.hfGainDb.load()) < 0.01f); break;

            case 40: io.AddKeyEvent (ImGuiKey_M, true); break;
            case 41: io.AddKeyEvent (ImGuiKey_M, false); break;
            case 44: note ("shell keyboard shortcut", p.mute.load()); break;

            case 48: views[0].setEditingName (true); break;
            case 52: io.AddInputCharacter ('Z'); break;
            case 56: io.AddKeyEvent (ImGuiKey_Enter, true); break;
            case 57: io.AddKeyEvent (ImGuiKey_Enter, false); break;
            case 62: note ("text entry commits", ! views[0].isEditingName()
                                                 && p.name.find ('Z') != std::string::npos); break;

            case 64: io.AddMousePosEvent (2.0f, 2.0f); io.AddMouseButtonEvent (0, false); break;
            case 66: requestIoModal = true; break;
            case 70: note ("modal is open", ioModalUp); break;
            // What has to hold for an embedded modal: the strip underneath stops taking
            // the pointer while it is up.
            case 72: io.AddMousePosEvent (hfGain.x, hfGain.y); break;
            case 74: selftestBefore = p.hfGainDb.load(); io.AddMouseButtonEvent (0, true); break;
            case 76: io.AddMousePosEvent (hfGain.x, hfGain.y - 40.0f * scale); break;
            case 78: io.AddMouseButtonEvent (0, false); break;
            case 80: note ("modal blocks the strip underneath",
                           std::fabs (p.hfGainDb.load() - selftestBefore) < 0.01f); break;

            case 84: requestInsertMenu = true; break;
            case 88: note ("context menu is open", insertMenuUp); break;

            // A key held while the window loses the focus must not stay down. The compositor
            // this runs under has no seat, so the focus change is delivered through the
            // framework's own callback rather than through a real one.
            case 90: io.AddKeyEvent (ImGuiKey_M, true); break;
            case 92: keyWasHeld = ImGui::IsKeyDown (ImGuiKey_M);
                     onFocusChanged (DGL::Widget::FocusEvent()); break;
            case 94: note ("focus loss clears a held key",
                           keyWasHeld && ! ImGui::IsKeyDown (ImGuiKey_M)); break;
            case 96: application.quit(); break;
            default: break;
        }
    }

    bool drawIoModal (float w, float h, float scale)
    {
        ImGui::SetNextWindowPos (ImVec2 (w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2 (0.5f, 0.5f));
        ImGui::SetNextWindowSize (ImVec2 (290.0f * scale, 0.0f));

        if (! ImGui::BeginPopupModal ("Channel input", nullptr,
                                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
            return false;

        static const char* modes[] = { "Mono", "Stereo", "MIDI" };
        static const char* ins[]   = { "In 1", "In 2", "In 3", "In 4" };
        ImGui::Combo ("Mode", &modeIndex, modes, IM_ARRAYSIZE (modes));
        ImGui::Combo ("Input L", &inputIndex, ins, IM_ARRAYSIZE (ins));
        ImGui::Combo ("Input R", &inputRIndex, ins, IM_ARRAYSIZE (ins));
        ImGui::Separator();
        if (ImGui::Button ("Close", ImVec2 (120.0f * scale, 0.0f))
            || ImGui::IsKeyPressed (ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
        return true;
    }

    void drawHud (float w, float scale)
    {
        if (options.quiet || w < 420.0f * scale)
            return;

        char line[256];
        std::snprintf (line, sizeof line, "%.1f fps   %d verts   %d widgets   scale %.2f   %s",
                       static_cast<double> (ImGui::GetIO().Framerate),
                       vertices, views[0].lastWidgetCount, static_cast<double> (scale),
                       menuChoice.empty() ? "" : menuChoice.c_str());

        auto* const dl = ImGui::GetWindowDrawList();
        const ImVec2 at (w - 380.0f * scale, 6.0f * scale);
        dl->AddRectFilled (at, ImVec2 (w - 6.0f * scale, at.y + 18.0f * scale),
                           IM_COL32 (0, 0, 0, 160), 3.0f);
        dl->AddText (fonts.mono, 11.0f * scale, ImVec2 (at.x + 6.0f * scale, at.y + 3.0f * scale),
                     IM_COL32 (0xc0, 0xc0, 0xc8, 255), line);
    }

    void buildFonts (float scale)
    {
        auto& io = ImGui::GetIO();
        io.Fonts->Clear();

        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        cfg.PixelSnapH = false;

        // ImGui bakes a fixed glyph set. The default range is Latin only, which silently
        // drops the fader's infinity mark - a glyph the JUCE strip gets for free from the
        // system font. Every non-Latin mark the UI uses has to be declared here.
        static const ImWchar ranges[] = {
            0x0020, 0x024f,   // Latin and its supplements
            0x2000, 0x22ff,   // punctuation, arrows, maths (infinity lives here)
            0x2500, 0x25ff,   // box drawing and geometric shapes
            0
        };

        auto add = [&] (float size)
        {
            return io.Fonts->AddFontFromMemoryTTF (
                const_cast<void*> (static_cast<const void*> (daf_resources::dejavusans_ttf)),
                daf_resources::dejavusans_ttf_size, size * scale, &cfg, ranges);
        };

        // One face per design size rather than one scaled face: the strip's 8 pt column
        // headers are unreadable if they are a scaled 13 pt atlas entry.
        fonts.small   = add (8.0f);
        fonts.label   = add (9.0f);
        fonts.pill    = add (10.5f);
        fonts.band    = add (12.0f);
        fonts.name    = add (13.0f);
        fonts.mono    = add (11.0f);
        fonts.monoBig = add (14.0f);
        io.FontDefault = fonts.band;
        io.Fonts->Build();
    }

    void reportGraphics()
    {
        renderer = glStr (GL_RENDERER);
        if (! options.quiet)
            std::printf ("[spike] GL vendor=%s renderer=%s version=%s\n",
                         glStr (GL_VENDOR), renderer.c_str(), glStr (GL_VERSION));
    }

    DGL::Application& application;
    Options options;

    std::vector<std::unique_ptr<duskspike::StripParams>> strips;
    std::vector<std::unique_ptr<duskspike::StubMeterSource>> sources;
    std::vector<duskspike::ChannelStripView> views;
    duskspike::StripFonts fonts;

    std::string renderer;
    std::string menuChoice;
    int modeIndex = 0, inputIndex = 0, inputRIndex = 1;
    bool requestIoModal = false, requestInsertMenu = false;
    bool ioModalUp = false, insertMenuUp = false;
    bool pointerHidden = false;

    bool measuring = true;
    int selftestStep = 0;
    float selftestBefore = 0.0f;
    bool keyWasHeld = false;
    int frames = 0;
    int vertices = 0;
    std::chrono::steady_clock::time_point lastFrame {};
    std::chrono::steady_clock::time_point startedAt {};
    std::vector<double> frameDeltas;
};

int main (int argc, char* argv[])
{
    Options opts;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&] () -> const char* { return i + 1 < argc ? argv[++i] : "0"; };

        if      (a == "--seconds") opts.seconds = std::atof (next());
        else if (a == "--strips")  opts.strips = std::atoi (next());
        else if (a == "--reopen")  opts.reopen = std::atoi (next());
        else if (a == "--scale")   opts.scaleOverride = std::atof (next());
        else if (a == "--width")   opts.width = std::atoi (next());
        else if (a == "--height")  opts.height = std::atoi (next());
        else if (a == "--resize-sweep") opts.resizeSweep = true;
        else if (a == "--quiet")   opts.quiet = true;
        else if (a == "--capture") opts.capturePath = next();
        else if (a == "--capture-frame") opts.captureFrame = std::atoi (next());
        else if (a == "--selftest") opts.selftest = true;
        else if (a == "--demo")    opts.demo = std::atoi (next());
        else if (a == "--help")
        {
            std::printf ("usage: dusk-gui-spike [--seconds N] [--strips N] [--reopen N]\n"
                         "                      [--scale F] [--width N] [--height N]\n"
                         "                      [--resize-sweep] [--quiet]\n"
                         "                      [--capture FILE.ppm] [--capture-frame N]\n"
                         "                      [--selftest] [--demo 1|2]\n");
            return 0;
        }
    }

    DGL::Application app (true);
    app.setClassName ("dusk-studio-gui-spike");

    // Renderer and context lifetime: build and tear the whole window down a few times
    // before the measured run, so a leaked EGL display or a stale surface shows up as a
    // failure here rather than in the shipping app months later.
    for (int i = 0; i < opts.reopen; ++i)
    {
        Options warm = opts;
        warm.seconds = 0.35;
        warm.quiet = true;
        SpikeWindow w (app, warm);
        w.show();
        app.exec (4);
        std::printf ("[spike] reopen cycle %d: %d frames, renderer=%s\n",
                     i + 1, w.framesDrawn(), w.rendererName().c_str());
    }

    SpikeWindow window (app, opts);
    window.show();

    const double cpuBefore = cpuSeconds();
    const auto wallBefore = std::chrono::steady_clock::now();

    window.beginMeasuring();
    app.exec (4);

    const double wall = std::chrono::duration<double> (
        std::chrono::steady_clock::now() - wallBefore).count();
    const double cpu = cpuSeconds() - cpuBefore;

    auto d = window.deltas();
    if (d.empty())
    {
        std::printf ("[spike] no frames drawn\n");
        return 1;
    }

    const double p50 = percentile (d, 0.50);
    const double p95 = percentile (d, 0.95);
    const double worst = d.back();
    double sum = 0.0;
    for (const double v : d) sum += v;

    std::printf ("[spike] frames=%zu wall=%.2fs mean=%.3fms p50=%.3fms p95=%.3fms worst=%.3fms\n",
                 d.size(), wall, sum / static_cast<double> (d.size()), p50, p95, worst);
    std::printf ("[spike] effective fps=%.1f  cpu=%.2fs (%.1f%% of one core)  verts/frame=%d  strips=%d\n",
                 static_cast<double> (d.size()) / std::max (1.0e-6, wall),
                 cpu, 100.0 * cpu / std::max (1.0e-6, wall),
                 window.lastVertices(), opts.strips);
    std::printf ("[spike] renderer=%s\n", window.rendererName().c_str());
    std::printf ("[spike] input events: pointer=%d key=%d text=%d focusIn=%d focusOut=%d\n",
                 window.pointerEvents, window.keyEvents, window.textEvents,
                 window.focusInEvents, window.focusOutEvents);
    std::printf ("[spike] cursor changes accepted by the backend: %d\n", window.cursorCalls);
    std::printf ("[spike] portal parent handle: \"%s\"\n", window.portalHandle().c_str());

    int failures = 0;
    for (const auto& line : window.selftestLog)
    {
        std::printf ("[spike] selftest %s\n", line.c_str());
        if (line.rfind ("FAIL", 0) == 0)
            ++failures;
    }

    return failures == 0 ? 0 : 2;
}
