#include "StartupView.h"
#include "DuskTheme.h"
#include "../../engine/audiofile/FileReader.h"
#include "../../foundation/Fs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <system_error>
#include <utility>

namespace duskstudio::imgui
{
namespace
{
namespace dw = DuskWidgets;

// StartupDialog::resized()'s geometry, in design pixels.
constexpr float kPanelW = 720.0f;
constexpr float kPanelH = 460.0f;
constexpr float kSidebarW = 100.0f;
constexpr float kFooterH = 52.0f;
constexpr float kBrandIconH = 80.0f;
constexpr float kWordmarkH = 14.0f;
constexpr float kTabH = 36.0f;
constexpr float kHeadingH = 22.0f;
constexpr float kBannerH = 24.0f;
constexpr float kRowH = 24.0f;
constexpr float kHeaderRowH = 22.0f;
constexpr float kFooterButtonW = 90.0f;

constexpr unsigned int kBg = 0x202024ff;
constexpr unsigned int kSidebarBg = 0x181820ff;
constexpr unsigned int kBorder = 0x32323aff;
constexpr unsigned int kSelection = 0x305a82ff;
constexpr unsigned int kTextHi = 0xe8e8e8ff;
constexpr unsigned int kTextMid = 0xb0b0b8ff;
constexpr unsigned int kTextLo = 0x707078ff;
constexpr unsigned int kAccent = 0x80b0ffff;
constexpr unsigned int kAmber = 0xe0a050ff;

// The blink draws the eye, then the badge settles on steady amber rather than
// strobing for as long as the dialog is open.
constexpr int kBlinkFrames = 30;
constexpr int kMaxBlinks = 10;

ImU32 rgba (unsigned int hex)
{
    return IM_COL32 ((hex >> 24) & 0xff, (hex >> 16) & 0xff, (hex >> 8) & 0xff, hex & 0xff);
}

struct Column
{
    const char* title;
    float width;   // design pixels; the name column takes what is left
};

const Column kColumns[] = {
    { "Session Name", 0.0f },
    { "Sample Rate", 100.0f },
    { "File Resolution", 120.0f },
    { "Last Modified", 140.0f }
};

std::string formatSampleRate (double sampleRate)
{
    if (sampleRate <= 0.0)
        return {};
    // Whole kHz when round (48000 reads "48 kHz"), one decimal otherwise.
    const double khz = sampleRate / 1000.0;
    char buffer[24];
    if (std::abs (khz - std::round (khz)) < 0.05)
        std::snprintf (buffer, sizeof buffer, "%d kHz", static_cast<int> (std::round (khz)));
    else
        std::snprintf (buffer, sizeof buffer, "%.1f kHz", khz);
    return buffer;
}

std::string formatBitDepth (int bits, bool isFloat)
{
    if (bits <= 0)
        return {};
    return std::to_string (bits) + "-bit" + (isFloat ? " float" : "");
}

std::string formatLastModified (const std::filesystem::path& path)
{
    std::error_code error;
    const auto written = std::filesystem::last_write_time (path, error);
    if (error)
        return {};

    // No portable file_clock to system_clock conversion before C++20, so the epoch
    // difference is measured rather than assumed.
    const auto now = std::filesystem::file_time_type::clock::now();
    const auto age = std::chrono::duration_cast<std::chrono::seconds> (now - written);
    const auto stamp = std::chrono::system_clock::to_time_t (
        std::chrono::system_clock::now() - age);

    std::tm local {};
   #if defined (_WIN32)
    localtime_s (&local, &stamp);
   #else
    localtime_r (&stamp, &local);
   #endif
    char buffer[32] {};
    std::strftime (buffer, sizeof buffer, "%Y-%m-%d %H:%M", &local);
    return buffer;
}

void inferAudioFormat (const std::filesystem::path& sessionDir, std::string& sampleRate,
                       std::string& bitDepth)
{
    std::error_code error;
    const auto audioDir = sessionDir / "audio";
    if (! std::filesystem::is_directory (audioDir, error))
        return;

    std::filesystem::path candidate;
    auto iterator = std::filesystem::directory_iterator (audioDir, error);
    const auto scanSucceeded = dusk::fs::walkDirectoryEntries (
        std::move (iterator), error, [&] (const std::filesystem::directory_entry& entry)
    {
        std::error_code inspectionError;
        if (! entry.is_regular_file (inspectionError) || inspectionError)
            return true;
        auto extension = entry.path().extension().string();
        std::transform (extension.begin(), extension.end(), extension.begin(),
                        [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
        if (extension == ".wav")
        {
            candidate = entry.path();
            return false;
        }
        if (candidate.empty()
            && (extension == ".flac" || extension == ".aiff" || extension == ".aif"))
            candidate = entry.path();
        return true;
    });
    if (! scanSucceeded || candidate.empty())
        return;

    const auto reader = dusk::audio::FileReader::open (candidate);
    if (reader == nullptr)
        return;
    const auto& info = reader->info();
    sampleRate = formatSampleRate (info.sampleRate);
    bitDepth = formatBitDepth (info.bitsPerSample, info.isFloat);
}

class StartupViewImpl final : public StartupView
{
public:
    StartupViewImpl (std::vector<RecentSession> recentSessions, const unsigned char* brandRgba,
                     int brandW, int brandH, std::function<void()> downloads)
        : recents (std::move (recentSessions)), brandPixels (brandRgba),
          brandWidth (brandW), brandHeight (brandH), openDownloads (std::move (downloads))
    {
        // The newest session is selected on open so Enter or Open works without an
        // extra click.
        selectedRow = recents.empty() ? -1 : 0;
    }

    ImVec2 preferredSize() const override { return ImVec2 (kPanelW, kPanelH); }

    void setUpdateAvailable (const std::string& tagName) override
    {
        updateBanner = "Update available: " + tagName + "  -  click to download";
        blinkFrames = 0;
        blinks = 0;
    }

    void reserveAtlasImages (ImFontAtlas& atlas) override
    {
        brand.reserve (atlas, brandPixels, brandWidth, brandHeight);
    }

    void rasteriseAtlasImages (ImFontAtlas& atlas) override
    {
        brand.rasterise (atlas);
    }

    // Escape skips the dialog rather than only closing it: the bootstrap default
    // session is what the DAW opens with, which is what skipping means.
    bool escapeDismisses() const override { return false; }

    // Reported once. The choice itself has to survive for the host to read it from
    // the dismissed callback, and the teardown it starts takes a couple of event-pump
    // ticks - long enough to re-report and run the action twice.
    bool takeDismissRequest() override
    {
        if (action == StartupAction::none || reported)
            return false;
        reported = true;
        return true;
    }

    StartupAction chosenAction() const override { return action; }
    const std::string& chosenPath() const override { return path; }

    void draw (dw::Context& ctx, ImVec2 origin, ImVec2 size) override
    {
        const float scale = ctx.scale;
        const ImVec2 br (origin.x + size.x, origin.y + size.y);
        ctx.dl->AddRectFilled (origin, br, rgba (kBg));

        const float sidebarRight = origin.x + scale * kSidebarW;
        ctx.dl->AddRectFilled (origin, ImVec2 (sidebarRight, br.y), rgba (kSidebarBg));
        ctx.dl->AddLine (ImVec2 (sidebarRight - scale, origin.y),
                         ImVec2 (sidebarRight - scale, br.y), rgba (kBorder), scale);

        const float footerTop = br.y - scale * kFooterH;
        ctx.dl->AddLine (ImVec2 (sidebarRight, footerTop), ImVec2 (br.x, footerTop),
                         rgba (kBorder), scale);
        ctx.dl->AddRect (origin, br, rgba (kBorder), 0.0f, 0, scale);

        drawSidebar (ctx, origin, sidebarRight);
        drawFooter (ctx, footerTop, br);
        drawMain (ctx, ImVec2 (sidebarRight, origin.y), ImVec2 (br.x, footerTop));

        handleKeys (ctx);
    }

private:
    void drawSidebar (dw::Context& ctx, ImVec2 origin, float right)
    {
        const float scale = ctx.scale;
        const float left = origin.x + scale * 8.0f;
        const float width = (right - scale * 8.0f) - left;
        float y = origin.y + scale * 12.0f;

        brand.draw (*ctx.dl, ImVec2 (left, y), ImVec2 (left + width, y + scale * kBrandIconH));
        y += scale * (kBrandIconH + 4.0f);

        dw::text (ctx, ctx.fonts->pill, scale * 10.0f, ImVec2 (left, y), width,
                  rgba (kTextMid), "DUSK STUDIO");
        y += scale * (kWordmarkH + 16.0f);

        // RECENT is the tab this dialog is showing; the other two are the actions
        // they name, and both dismiss once the host takes over.
        drawTab (ctx, "##tab-recent", ImVec2 (left, y), width, "RECENT", true);
        y += scale * kTabH;
        if (drawTab (ctx, "##tab-open", ImVec2 (left, y), width, "OPEN", false))
            action = StartupAction::openFile;
        y += scale * kTabH;
        if (drawTab (ctx, "##tab-new", ImVec2 (left, y), width, "NEW", false))
            action = StartupAction::newSession;
    }

    bool drawTab (dw::Context& ctx, const char* id, ImVec2 at, float width,
                  const char* label, bool active)
    {
        dw::ButtonStyle style;
        style.offFill = active ? rgba (0x282830ff) : rgba (kSidebarBg);
        style.onFill = rgba (0x282830ff);
        style.offText = active ? rgba (kAccent) : rgba (kTextMid);
        style.onText = rgba (kAccent);
        style.fontSize = 11.0f * ctx.scale;
        style.rounding = 0.0f;
        return dw::textButton (ctx, id, at, ImVec2 (at.x + width, at.y + ctx.scale * kTabH),
                               label, active, style).clicked;
    }

    void drawFooter (dw::Context& ctx, float top, ImVec2 br)
    {
        const float scale = ctx.scale;
        const float buttonTop = top + scale * 10.0f;
        const float buttonBottom = br.y - scale * 10.0f;
        float right = br.x - scale * 16.0f;

        dw::ButtonStyle open;
        open.offFill = rgba (0x305a82ff);
        open.onFill = rgba (0x305a82ff);
        open.offText = rgba (kTextHi);
        open.onText = rgba (kTextHi);
        open.fontSize = 12.0f * scale;

        const bool canOpen = selectedRow >= 0 && selectedRow < static_cast<int> (recents.size());
        if (! canOpen)
        {
            open.offFill = rgba (0x28303aff);
            open.offText = rgba (kTextLo);
        }
        if (dw::textButton (ctx, "##open",
                            ImVec2 (right - scale * kFooterButtonW, buttonTop),
                            ImVec2 (right, buttonBottom), "Open", false, open).clicked
            && canOpen)
            openSelected();

        right -= scale * (kFooterButtonW + 8.0f);

        dw::ButtonStyle quit;
        quit.offFill = rgba (0x2a2a30ff);
        quit.onFill = rgba (0x2a2a30ff);
        quit.offText = rgba (kTextHi);
        quit.onText = rgba (kTextHi);
        quit.fontSize = 12.0f * scale;
        if (dw::textButton (ctx, "##quit",
                            ImVec2 (right - scale * kFooterButtonW, buttonTop),
                            ImVec2 (right, buttonBottom), "Quit", false, quit).clicked)
            action = StartupAction::quit;
    }

    void drawMain (dw::Context& ctx, ImVec2 tl, ImVec2 br)
    {
        const float scale = ctx.scale;
        const float left = tl.x + scale * 16.0f;
        const float right = br.x - scale * 16.0f;
        float y = tl.y + scale * 12.0f;

        if (! updateBanner.empty())
        {
            drawUpdateBanner (ctx, ImVec2 (left, y), right - left);
            y += scale * (kBannerH + 6.0f);
        }

        dw::text (ctx, ctx.fonts->title, scale * 16.0f, ImVec2 (left, y), right - left,
                  rgba (kTextHi), "Recent Sessions", dw::Align::left);
        y += scale * (kHeadingH + 8.0f);

        if (recents.empty())
        {
            dw::text (ctx, ctx.fonts->band, scale * 13.0f,
                      ImVec2 (left, (y + br.y) * 0.5f), right - left, rgba (kTextLo),
                      "No recent sessions yet.");
            return;
        }

        drawTable (ctx, ImVec2 (left, y), ImVec2 (right, br.y - scale * 12.0f));
    }

    void drawUpdateBanner (dw::Context& ctx, ImVec2 at, float width)
    {
        const float scale = ctx.scale;
        const ImVec2 br (at.x + width, at.y + scale * kBannerH);

        if (blinks < kMaxBlinks && ++blinkFrames >= kBlinkFrames)
        {
            blinkFrames = 0;
            ++blinks;
            flashOn = ! flashOn;
        }
        const bool lit = blinks >= kMaxBlinks || flashOn;

        if (dw::hitArea (ctx, "##update-banner", at, br)
            && ImGui::IsMouseClicked (ImGuiMouseButton_Left) && openDownloads)
            openDownloads();

        dw::text (ctx, ctx.fonts->band, scale * 12.0f,
                  ImVec2 (at.x, at.y + scale * 5.0f), width,
                  lit ? rgba (kAmber) : rgba (kTextLo), updateBanner.c_str(),
                  dw::Align::left);
    }

    void drawTable (dw::Context& ctx, ImVec2 tl, ImVec2 br)
    {
        const float scale = ctx.scale;
        auto& dl = *ctx.dl;

        float fixed = 0.0f;
        for (const auto& column : kColumns)
            fixed += column.width * scale;
        const float nameW = std::max (scale * 80.0f, (br.x - tl.x) - fixed);

        const float headerBottom = tl.y + scale * kHeaderRowH;
        dl.AddRectFilled (tl, ImVec2 (br.x, headerBottom), rgba (kSidebarBg));
        float x = tl.x;
        for (const auto& column : kColumns)
        {
            const float w = column.width > 0.0f ? column.width * scale : nameW;
            dw::text (ctx, ctx.fonts->band, scale * 12.0f,
                      ImVec2 (x + scale * 8.0f, tl.y + scale * 4.0f), w - scale * 16.0f,
                      rgba (kTextMid), column.title, dw::Align::left);
            x += w;
            if (x < br.x)
                dl.AddLine (ImVec2 (x, tl.y), ImVec2 (x, headerBottom), rgba (kBorder), scale);
        }
        dl.AddRect (tl, br, rgba (kBorder), 0.0f, 0, scale);

        const float rowsTop = tl.y + scale * kHeaderRowH;
        const float rowH = scale * kRowH;
        visibleRows = std::max (0, static_cast<int> ((br.y - rowsTop) / rowH));

        // The list scrolls under the pointer rather than growing a scrollbar: at
        // twenty visible rows a wheel is the only gesture that matters here.
        if (dw::hitArea (ctx, "##recent-rows", ImVec2 (tl.x, rowsTop), br))
        {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel < 0.0f || wheel > 0.0f)
                scrollRow = std::clamp (scrollRow - static_cast<int> (wheel), 0,
                                        std::max (0, static_cast<int> (recents.size())
                                                         - visibleRows));
        }

        dl.PushClipRect (ImVec2 (tl.x + scale, rowsTop), ImVec2 (br.x - scale, br.y), true);
        for (int i = 0; i < visibleRows; ++i)
        {
            const int index = scrollRow + i;
            if (index >= static_cast<int> (recents.size()))
                break;

            const auto& row = recents[static_cast<std::size_t> (index)];
            const ImVec2 rowTl (tl.x + scale, rowsTop + rowH * static_cast<float> (i));
            const ImVec2 rowBr (br.x - scale, rowTl.y + rowH);
            const bool selected = index == selectedRow;
            dl.AddRectFilled (rowTl, rowBr, selected ? rgba (kSelection) : rgba (kBg));

            char id[32];
            std::snprintf (id, sizeof id, "##row%d", index);
            if (dw::hitArea (ctx, id, rowTl, rowBr))
            {
                if (ImGui::IsMouseClicked (ImGuiMouseButton_Left))
                    selectedRow = index;
                if (ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
                {
                    selectedRow = index;
                    openSelected();
                }
            }

            const char* const cells[] = { row.name.c_str(), row.sampleRate.c_str(),
                                          row.bitDepth.c_str(), row.lastModified.c_str() };
            float cellX = rowTl.x;
            for (std::size_t c = 0; c < 4; ++c)
            {
                const float w = kColumns[c].width > 0.0f ? kColumns[c].width * scale : nameW;
                const char* const text = cells[c][0] != 0 ? cells[c] : "\xe2\x80\x94";
                dw::text (ctx, ctx.fonts->band, scale * 13.0f,
                          ImVec2 (cellX + scale * 8.0f, rowTl.y + scale * 5.0f),
                          w - scale * 16.0f, selected ? rgba (kTextHi) : rgba (kTextMid),
                          text, dw::Align::left);
                cellX += w;
            }
        }
        dl.PopClipRect();
    }

    void handleKeys (const dw::Context& ctx)
    {
        if (! dw::shortcutsAvailable (ctx))
            return;

        if (ImGui::IsKeyPressed (ImGuiKey_Escape, false))
        {
            action = StartupAction::skip;
            return;
        }
        if (ImGui::IsKeyPressed (ImGuiKey_Enter, false)
            || ImGui::IsKeyPressed (ImGuiKey_KeypadEnter, false))
        {
            openSelected();
            return;
        }
        if (recents.empty())
            return;
        const int last = static_cast<int> (recents.size()) - 1;
        if (ImGui::IsKeyPressed (ImGuiKey_DownArrow, true))
            selectedRow = std::clamp (selectedRow + 1, 0, last);
        else if (ImGui::IsKeyPressed (ImGuiKey_UpArrow, true))
            selectedRow = std::clamp (selectedRow - 1, 0, last);
        scrollToSelection();
    }

    // The arrows and the wheel share one offset, so a keyboard move has to pull the
    // window onto the row it selected - otherwise the selection walks off screen and
    // Enter opens a session the user cannot see.
    void scrollToSelection()
    {
        if (visibleRows < 1 || selectedRow < 0)
            return;
        scrollRow = std::clamp (scrollRow, std::max (0, selectedRow - visibleRows + 1),
                                selectedRow);
        scrollRow = std::clamp (scrollRow, 0,
                                std::max (0, static_cast<int> (recents.size()) - visibleRows));
    }

    void openSelected()
    {
        if (selectedRow < 0 || selectedRow >= static_cast<int> (recents.size()))
            return;
        path = recents[static_cast<std::size_t> (selectedRow)].path;
        action = StartupAction::openRecent;
    }

    std::vector<RecentSession> recents;
    const unsigned char* brandPixels = nullptr;
    int brandWidth = 0;
    int brandHeight = 0;
    std::function<void()> openDownloads;
    AtlasImage brand;
    std::string updateBanner;
    std::string path;
    StartupAction action = StartupAction::none;
    bool reported = false;
    int selectedRow = -1;
    int scrollRow = 0;
    int visibleRows = 0;
    int blinkFrames = 0;
    int blinks = kMaxBlinks;
    bool flashOn = true;
};
} // namespace

std::vector<RecentSession> scanRecentSessions (const std::vector<std::filesystem::path>& paths)
{
    std::vector<RecentSession> rows;
    rows.reserve (paths.size());
    for (const auto& path : paths)
    {
        RecentSession row;
        // u8string, not string: the recents file stores UTF-8 and RecentSessions reads
        // it back through u8path, but native_string() on Windows is the ANSI code page.
        // The row is drawn as UTF-8 and handed back as a UTF-8 file path, so anything
        // outside ASCII would come back mangled and fail to load.
        row.path = path.u8string();
        row.name = path.filename().u8string();
        row.lastModified = formatLastModified (path);
        inferAudioFormat (path, row.sampleRate, row.bitDepth);
        rows.push_back (std::move (row));
    }
    return rows;
}

std::unique_ptr<StartupView> makeStartupView (std::vector<RecentSession> recents,
                                              const unsigned char* brandRgba,
                                              int brandWidth, int brandHeight,
                                              std::function<void()> openDownloads)
{
    return std::unique_ptr<StartupView> (
        new StartupViewImpl (std::move (recents), brandRgba, brandWidth, brandHeight,
                             std::move (openDownloads)));
}
} // namespace duskstudio::imgui
