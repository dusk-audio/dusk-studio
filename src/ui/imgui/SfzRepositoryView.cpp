#include "SfzRepositoryView.h"
#include "DuskTheme.h"
#include "PanelControls.h"
#include "../../foundation/MessageThread.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <utility>

namespace duskstudio::imgui
{
namespace
{
namespace dw = DuskWidgets;
using sfz::LibraryState;
constexpr const char* kPauseMessage = "Paused while recording or rendering. Stop recording or the render, then select Resume.";

std::string packKey (const sfz::InstalledPack& pack)
{
    return pack.provider.sourceId + "/" + pack.provider.packId + "/" + pack.provider.releaseId + "/" + pack.archiveSha256;
}

std::string summaryKey (const sfz::ProviderSummary& summary)
{
    return summary.sourceId + "/" + summary.packId;
}

std::string displayTitle (std::string name, const std::string& source)
{
    if (source != "sfzinstruments") return name;
    std::string title;
    bool word = true;
    char previous = 0;
    for (const char c : name)
    {
        if (c == '_' || c == '.' || c == '-' || c == ' ')
        {
            if (! title.empty() && title.back() != ' ') title += ' ';
            word = true;
        }
        else
        {
            if (c >= 'A' && c <= 'Z' && previous >= 'a' && previous <= 'z' && ! word) title += ' ';
            title += word && c >= 'a' && c <= 'z' ? static_cast<char> (c - 'a' + 'A') : c;
            word = false;
        }
        previous = c;
    }
    while (! title.empty() && title.back() == ' ') title.pop_back();
    return title.empty() ? name : title;
}

std::string sizeLabel (std::uint64_t bytes)
{
    if (bytes == 0) return "Size unknown";
    char text[48] {};
    if (bytes >= 1024ULL * 1024 * 1024)
        std::snprintf (text, sizeof text, "%.2f GiB", static_cast<double> (bytes) / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024 * 1024)
        std::snprintf (text, sizeof text, "%.1f MiB", static_cast<double> (bytes) / (1024.0 * 1024));
    else std::snprintf (text, sizeof text, "%llu bytes", static_cast<unsigned long long> (bytes));
    return text;
}

const char* stateLabel (LibraryState state, bool busy)
{
    switch (state)
    {
        case LibraryState::Idle:
        case LibraryState::Complete: return busy ? "Updating library..." : "Ready";
        case LibraryState::Browsing: return "Browsing repository...";
        case LibraryState::Resolving: return "Checking version and license...";
        case LibraryState::Downloading: return "Downloading instrument pack...";
        case LibraryState::Extracting: return "Installing instrument pack...";
        case LibraryState::Paused: return "Library work paused";
        case LibraryState::Failed: return "Library operation could not finish";
        case LibraryState::Cancelled: return "Library operation cancelled";
        default: return busy ? "Updating library..." : "Ready";
    }
}

class SfzRepositoryView final : public DuskPanelView
{
public:
    SfzRepositoryView (sfz::LibraryService& s,
        std::function<void (const sfz::InstalledPack&, const sfz::ProviderInstrument&)> load,
        std::function<std::vector<sfz::ManagedSfzLocator>()> used,
        std::function<void (std::string)> source, std::function<void()> local)
        : service (s), onLoad (std::move (load)), inUse (std::move (used)),
          openSource (std::move (source)), openLocalFolders (std::move (local)), sources (sfz::providerSources())
    {
        sources.insert (sources.begin(), { sfz::kAllRepositoriesSourceId, "All repositories", {} });
    }

    ImVec2 preferredSize() const override { return { 1100.0f, 760.0f }; }
    bool capturesKeyboard() const override { return keyboardActive; }
    bool escapeDismisses() const override { return ! keyboardWasActive; }
    bool controlPointForScenario (const std::string& name, ImVec2& point, float position) const override
    {
        const auto found = controls.find (name);
        if (found == controls.end()) return false;
        point = { found->second.first.x + std::clamp (position, 0.0f, 1.0f)
                    * (found->second.second.x - found->second.first.x),
                  (found->second.first.y + found->second.second.y) * 0.5f };
        return true;
    }

    void draw (dw::Context& ctx, ImVec2 origin, ImVec2 size) override
    {
        keyboardWasActive = keyboardActive;
        frameMutation = false;
        controls.clear();
        const auto snapshot = service.snapshot();
        synchronize (snapshot);
        std::function<void()> deferred;
        const ScopedFormStyle style (ctx);
        ImGui::PushFont (ctx.fonts->textEntry != nullptr ? ctx.fonts->textEntry : ImGui::GetFont());
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, { ctx.s (18), ctx.s (16) });
        ImGui::PushStyleVar (ImGuiStyleVar_FramePadding, { ctx.s (12), ctx.s (11) });
        ImGui::PushStyleVar (ImGuiStyleVar_ItemSpacing, { ctx.s (12), ctx.s (12) });
        ImGui::PushStyleVar (ImGuiStyleVar_ChildRounding, ctx.s (6));
        ImGui::PushStyleVar (ImGuiStyleVar_FrameRounding, ctx.s (5));
        ImGui::PushStyleColor (ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4 (ctx.theme->panelBorder));
        ctx.dl->AddRectFilled (origin, { origin.x + size.x, origin.y + size.y }, ctx.theme->panelFill);
        ImGui::SetCursorScreenPos (origin);
        if (ImGui::BeginChild ("##sfz-repository-body", size, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground))
        {
            heading (ctx, "SFZ instrument library", 24);
            muted ("Creative Commons instruments from community repositories.");
            if (forceInstalled) { onlineTab = false; forceInstalled = false; }
            const float tabWidth = std::min (ctx.s (158), (ImGui::GetContentRegionAvail().x - ctx.s (12)) * 0.5f);
            if (button ("installed-tab", "Installed", true, { tabWidth, ctx.s (38) }, ! onlineTab)) onlineTab = false;
            ImGui::SameLine();
            if (button ("online-tab", "Online", true, { tabWidth, ctx.s (38) }, onlineTab)) onlineTab = true;
            ImGui::Separator();
            const auto extraHeight = [] (const std::string& text)
            {
                return text.empty() ? 0.0f : ImGui::CalcTextSize (text.c_str(), nullptr, false,
                    ImGui::GetContentRegionAvail().x).y + ImGui::GetStyle().ItemSpacing.y;
            };
            float footerHeight = snapshot.busy || snapshot.state == LibraryState::Paused
                ? ImGui::GetFrameHeight() : ImGui::GetTextLineHeight();
            if (snapshot.state == LibraryState::Downloading || snapshot.state == LibraryState::Extracting)
                footerHeight += ctx.s (22) + ImGui::GetStyle().ItemSpacing.y;
            if (snapshot.state == LibraryState::Paused) footerHeight += extraHeight (kPauseMessage);
            else if (snapshot.state != LibraryState::Cancelled) footerHeight += extraHeight (snapshot.error);
            footerHeight += extraHeight (notice) + ctx.s (4);
            footerHeight = std::clamp (footerHeight, ctx.s (32),
                std::max (ctx.s (32), ImGui::GetContentRegionAvail().y * 0.4f));
            const float panelHeight = std::max (ctx.s (40), ImGui::GetContentRegionAvail().y - footerHeight
                - 2 * ImGui::GetStyle().ItemSpacing.y);
            if (onlineTab) drawOnline (ctx, snapshot, panelHeight, deferred);
            else drawInstalled (ctx, snapshot, panelHeight, deferred);
            ImGui::Separator();
            drawStatus (ctx, snapshot, footerHeight);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar (5);
        ImGui::PopFont();
        keyboardActive = ImGui::IsAnyItemActive()
            || ImGui::IsPopupOpen (nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        if (deferred) dusk::callAsync (std::move (deferred));
    }

private:
    enum class Intent { None, Browse, Resolve, Install, Refresh, Remove };

    void remember (const std::string& name)
    {
        if (ImGui::IsItemVisible()) controls[name] = { ImGui::GetItemRectMin(), ImGui::GetItemRectMax() };
    }

    void heading (const dw::Context& ctx, const std::string& text, float fontSize = 20)
    {
        auto* font = ctx.fonts->textEntry != nullptr ? ctx.fonts->textEntry : ImGui::GetFont();
        const float width = ImGui::GetContentRegionAvail().x;
        const auto extent = font->CalcTextSizeA (ctx.s (fontSize), width, width, text.c_str());
        ImGui::GetWindowDrawList()->AddText (font, ctx.s (fontSize), ImGui::GetCursorScreenPos(),
                                           ctx.theme->textBright, text.c_str(), nullptr, width);
        ImGui::Dummy ({ width, extent.y });
    }

    void muted (const std::string& text)
    {
        ImGui::PushStyleColor (ImGuiCol_Text, ImGui::GetStyleColorVec4 (ImGuiCol_TextDisabled));
        ImGui::TextWrapped ("%s", text.c_str());
        ImGui::PopStyleColor();
    }

    std::string sourceName (const std::string& id) const
    {
        for (const auto& source : sources) if (source.id == id) return source.name;
        return id;
    }

    bool button (const char* name, const char* label, bool enabled = true, ImVec2 size = {}, bool primary = false)
    {
        if (primary)
        {
            const auto accent = consolePalette().focusRing;
            ImGui::PushStyleColor (ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4 (accent));
            ImGui::PushStyleColor (ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4 (dw::brighter (accent, 0.15f)));
            ImGui::PushStyleColor (ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4 (dw::darker (accent, 0.1f)));
            ImGui::PushStyleColor (ImGuiCol_Text, ImVec4 (0.07f, 0.07f, 0.08f, 1));
        }
        ImGui::PushID (name);
        ImGui::BeginDisabled (! enabled || frameMutation);
        const bool clicked = ImGui::Button (label, size);
        remember (name);
        ImGui::EndDisabled();
        ImGui::PopID();
        if (primary) ImGui::PopStyleColor (4);
        return clicked;
    }

    template <typename Action>
    bool start (Intent intent, Action&& action)
    {
        const auto before = service.snapshot();
        if (! action()) { notice = "Library work is busy or paused. Resume or cancel it before starting another operation."; return false; }
        requestedGeneration = before.generation + 1;
        pendingIntent = intent;
        frameMutation = true;
        notice.clear();
        return true;
    }

    void clearSelection()
    {
        selectedSummary.reset();
        resolved.reset();
        resolvingSelection.clear();
        resolutionCached = false;
    }

    void synchronize (const sfz::LibrarySnapshot& snapshot)
    {
        if (pendingIntent == Intent::None) return;
        if (snapshot.generation > requestedGeneration)
        {
            if (pendingIntent == Intent::Resolve) resolved.reset();
            pendingIntent = Intent::None;
            return;
        }
        if (snapshot.generation != requestedGeneration || snapshot.busy) return;
        if (snapshot.state == LibraryState::Paused) return;
        if (snapshot.state == LibraryState::Complete)
        {
            if (pendingIntent == Intent::Browse)
            {
                results = snapshot.page.items;
                nextPage = snapshot.page.nextPage;
                pageCached = snapshot.cachedResponse;
                currentPage = requestedPage;
                activeSource = requestedSource;
                activeQuery = requestedQuery;
                browseWarning = snapshot.error;
                clearSelection();
            }
            else if (pendingIntent == Intent::Resolve && selectedSummary && snapshot.pack
                && resolvingSelection == summaryKey (*selectedSummary)
                && snapshot.pack->sourceId == selectedSummary->sourceId && snapshot.pack->packId == selectedSummary->packId)
            {
                resolved = snapshot.pack;
                resolutionCached = snapshot.cachedResponse;
            }
            else if (pendingIntent == Intent::Install)
            {
                forceInstalled = true;
                for (const auto& installed : snapshot.installed)
                    if (resolved && installed.provider.sourceId == resolved->sourceId
                        && installed.provider.packId == resolved->packId && installed.provider.releaseId == resolved->releaseId)
                        selectedInstalled = packKey (installed);
                selectedInstrument.clear();
                notice = "Pack installed. Choose an SFZ file in Installed to load it.";
            }
        }
        pendingIntent = Intent::None;
    }

    bool row (const dw::Context& ctx, const std::string& control, const std::string& title, const std::string& subtitle,
              bool selected, bool enabled, float height, const std::string& hint = {})
    {
        ImGui::PushID (control.c_str());
        ImGui::BeginDisabled (! enabled || frameMutation);
        const bool clicked = ImGui::Selectable ("##row", selected, ImGuiSelectableFlags_None, { 0, height });
        remember (control);
        const auto first = ImGui::GetItemRectMin(), last = ImGui::GetItemRectMax();
        auto* list = ImGui::GetWindowDrawList();
        list->PushClipRect (first, last, true);
        const auto colour = ImGui::GetColorU32 (enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        list->AddText ({ first.x + ctx.s (10), first.y + ctx.s (9) }, colour, title.c_str());
        const float line = ImGui::GetTextLineHeight();
        const ImVec2 subtitleAt { first.x + ctx.s (10), first.y + line + ctx.s (16) };
        list->PushClipRect (subtitleAt, { last.x, subtitleAt.y + line }, true);
        list->AddText (subtitleAt, ImGui::GetColorU32 (ImGuiCol_TextDisabled), subtitle.c_str());
        list->PopClipRect();
        if (! hint.empty())
        {
            const ImVec2 at { first.x + ctx.s (10), first.y + 2 * line + ctx.s (24) };
            const float badgeWidth = std::min (ImGui::CalcTextSize (hint.c_str()).x + ctx.s (12), last.x - at.x - ctx.s (8));
            list->AddRectFilled (at, { at.x + badgeWidth, at.y + line + ctx.s (8) }, IM_COL32 (42, 48, 52, 255), ctx.s (4));
            list->PushClipRect (at, { at.x + badgeWidth, at.y + line + ctx.s (8) }, true);
            list->AddText ({ at.x + ctx.s (6), at.y + ctx.s (4) }, ImGui::GetColorU32 (ImGuiCol_Text), hint.c_str());
            list->PopClipRect();
        }
        list->PopClipRect();
        ImGui::EndDisabled();
        ImGui::PopID();
        return clicked;
    }

    void sourceButton (const std::string& url, std::function<void()>& deferred)
    {
        if (button ("source-link", "Open source website", ! url.empty() && static_cast<bool> (openSource)))
        {
            const auto callback = openSource;
            deferred = [callback, url] { callback (url); };
        }
    }

    void licenseButton (const sfz::ProviderPack& pack, std::function<void()>& deferred)
    {
        if (button ("license-link", "View license source", static_cast<bool> (openSource)))
        {
            const auto callback = openSource;
            const auto url = pack.license.sourceUrl;
            deferred = [callback, url] { callback (url); };
        }
    }

    void licenseText (const sfz::ProviderPack& pack)
    {
        if (ImGui::CollapsingHeader (pack.license.path.empty() ? "License declaration" : "License text"))
            ImGui::TextWrapped ("%s", pack.license.text.c_str());
    }

    std::string popularity (const sfz::ProviderPresentation& presentation, bool compact = false) const
    {
        if (presentation.stars)
            return compact ? std::to_string (*presentation.stars) + " stars"
                           : "GitHub stars: " + std::to_string (*presentation.stars);
        if (presentation.favorites)
            return compact ? std::to_string (*presentation.favorites) + " favorites"
                           : "Musical Artifacts favorites: " + std::to_string (*presentation.favorites);
        return {};
    }

    void demoLinks (dw::Context& ctx, const sfz::ProviderPresentation& presentation,
                    std::function<void()>& deferred, bool besideDownload)
    {
        if (presentation.demos.empty())
        {
            muted ("No source demo available.");
            return;
        }
        if (besideDownload && ImGui::GetContentRegionAvail().x >= ctx.s (340)) ImGui::SameLine();
        const auto demoButton = [&] (std::size_t index, const char* label)
        {
            const auto& demo = presentation.demos[index];
            const auto id = "demo-" + std::to_string (index);
            if (button (id.c_str(), label, static_cast<bool> (openSource), { 0, ctx.s (40) }))
            {
                const auto callback = openSource;
                const auto url = demo.url;
                deferred = [callback, url] { callback (url); };
            }
            formTooltip ((demo.label + "\n" + demo.url).c_str());
        };
        demoButton (0, "Listen to demo");
        muted ("Opens in your browser without downloading the instrument pack.");
        if (presentation.demos.size() > 1 && ImGui::CollapsingHeader ("More demos"))
            for (std::size_t i = 1; i < presentation.demos.size(); ++i)
                demoButton (i, presentation.demos[i].label.c_str());
    }

    void drawOnline (dw::Context& ctx, const sfz::LibrarySnapshot& snapshot, float height,
                     std::function<void()>& deferred)
    {
        const bool available = ! snapshot.busy && snapshot.state != LibraryState::Paused;
        const float top = ImGui::GetCursorScreenPos().y;
        const float width = ImGui::GetContentRegionAvail().x;
        const bool wide = width >= ctx.s (850);
        const float searchWidth = ctx.s (106);
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        ImGui::BeginDisabled (! available);
        ImGui::SetNextItemWidth (wide ? ctx.s (250) : width);
        const char* sourceLabel = sourceIndex >= 0 && sourceIndex < static_cast<int> (sources.size())
            ? sources[static_cast<std::size_t> (sourceIndex)].name.c_str() : "Choose a repository";
        const auto sourceAt = ImGui::GetCursorScreenPos();
        const float sourceWidth = wide ? ctx.s (250) : width;
        controls["source"] = { sourceAt, { sourceAt.x + sourceWidth, sourceAt.y + ImGui::GetFrameHeight() } };
        if (ImGui::BeginCombo ("##sfz-source", sourceLabel))
        {
            for (std::size_t i = 0; i < sources.size(); ++i)
                if (ImGui::Selectable (sources[i].name.c_str(), sourceIndex == static_cast<int> (i))
                    && sourceIndex != static_cast<int> (i))
                {
                    sourceIndex = static_cast<int> (i);
                    results.clear(); nextPage = 0; currentPage = 0; clearSelection();
                    activeSource.clear(); activeQuery.clear(); browseWarning.clear();
                }
            ImGui::EndCombo();
        }
        if (wide) ImGui::SameLine();
        ImGui::SetNextItemWidth (std::max (ctx.s (40), width - searchWidth - spacing
            - (wide ? ctx.s (250) + spacing : 0)));
        const bool submit = ImGui::InputTextWithHint ("##sfz-query", "Search instruments", query.data(), query.size(),
                                                     ImGuiInputTextFlags_EnterReturnsTrue);
        remember ("query");
        ImGui::SameLine();
        const bool search = button ("browse", "Search", available, { searchWidth, 0 }, true);
        if (available && (search || submit)) beginBrowse (1, false);
        ImGui::EndDisabled();
        ImGui::PushStyleVar (ImGuiStyleVar_FramePadding, { ctx.s (10), ctx.s (6) });
        if (button ("cached", "Browse cached", available)) beginBrowse (1, true);
        ImGui::SameLine();
        if (button ("previous-page", "Previous", available && currentPage > 1)) beginBrowse (currentPage - 1, pageCached, true);
        ImGui::SameLine();
        if (button ("next-page", "Next", available && nextPage != 0)) beginBrowse (nextPage, pageCached, true);
        ImGui::PopStyleVar();
        if (width >= ctx.s (620)) ImGui::SameLine();
        if (currentPage == 0) muted ("No search yet");
        else ImGui::Text ("%zu results  |  Page %u%s", results.size(), currentPage, pageCached ? "  |  Cached" : "");
        if (! activeQuery.empty()) muted ("Results for \"" + activeQuery + "\"");
        if (! browseWarning.empty()) muted (browseWarning);
        const float bodyHeight = std::max (ctx.s (40), height - (ImGui::GetCursorScreenPos().y - top));
        const float listWidth = std::max (ctx.s (100), (width - spacing) * 0.32f);
        if (ImGui::BeginChild ("##sfz-online-list", { listWidth, bodyHeight }, ImGuiChildFlags_Borders))
        {
            if (results.empty())
            {
                heading (ctx, currentPage == 0 ? "Explore instruments" : "No instruments found", 18);
                muted (currentPage == 0 ? "Search all repositories, or choose one to narrow the results. Browse cached opens saved results while offline."
                                       : "Try a different search or repository.");
            }
            for (std::size_t i = 0; i < results.size(); ++i)
            {
                const auto& summary = results[i];
                auto hint = summary.licenseHint.empty() || summary.licenseHint == "NOASSERTION"
                    ? "Check license on selection" : "Listed: " + summary.licenseHint;
                const auto count = popularity (summary.presentation, true);
                if (! count.empty()) hint += " | " + count;
                const auto identifier = summary.locator.empty() ? summary.packId : summary.locator;
                if (row (ctx, "online-item-" + std::to_string (i), displayTitle (summary.name, summary.sourceId),
                    sourceName (summary.sourceId) + " | " + identifier,
                    selectedSummary && summaryKey (*selectedSummary) == summaryKey (summary),
                    available, ctx.s (96), hint))
                {
                    clearSelection();
                    selectedSummary = summary;
                    resolvingSelection = summaryKey (summary);
                    start (Intent::Resolve, [&] { return service.resolve (summary, pageCached); });
                }
                const auto tooltip = summary.author + "\n" + hint + "\n" + summary.sourceUrl + "\n" + summary.description;
                formTooltip (tooltip.c_str());
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild ("##sfz-online-detail", { 0, bodyHeight }, ImGuiChildFlags_Borders))
        {
            if (selectedSummary)
            {
                heading (ctx, displayTitle (selectedSummary->name, selectedSummary->sourceId));
                muted (sourceName (selectedSummary->sourceId));
                if (! selectedSummary->author.empty() && selectedSummary->author != selectedSummary->sourceId
                    && selectedSummary->author != sourceName (selectedSummary->sourceId)) muted (selectedSummary->author);
                const auto count = popularity (resolved ? resolved->presentation : selectedSummary->presentation);
                if (! count.empty())
                {
                    ImGui::TextUnformatted (count.c_str());
                    formTooltip ("Stars and favorites indicate popularity, not a sound-quality rating.");
                }
            }
            if (resolved)
            {
                ImGui::TextColored (ImGui::ColorConvertU32ToFloat4 (consolePalette().eqAccent),
                                    "Verified license: %s", resolved->license.id.c_str());
                muted (sizeLabel (resolved->advertisedBytes) + (resolutionCached ? "  |  Cached details" : ""));
                bool installed = false;
                for (const auto& pack : snapshot.installed)
                    installed = installed || (resolved->versionPinned && pack.provider.sourceId == resolved->sourceId
                        && pack.provider.packId == resolved->packId && pack.provider.releaseId == resolved->releaseId);
                if (button ("download", installed ? "Installed" : "Download", available && ! installed, { ctx.s (154), ctx.s (40) }, true))
                    start (Intent::Install, [&] { return service.install (*resolved); });
                demoLinks (ctx, resolved->presentation, deferred, true);
                if (installed) ImGui::TextWrapped ("Choose this version in Installed to load it.");
                ImGui::TextWrapped ("%s", resolved->license.constraints.c_str());
                ImGui::Separator();
                sourceButton (resolved->sourceUrl, deferred);
                licenseButton (*resolved, deferred);
                if (selectedSummary && ! selectedSummary->description.empty())
                    ImGui::TextWrapped ("%s", selectedSummary->description.c_str());
                if (ImGui::CollapsingHeader ("Version and download details")) drawPackDetails (*resolved);
                licenseText (*resolved);
            }
            else if (selectedSummary)
            {
                button ("download", "Download", false, { ctx.s (154), ctx.s (40) }, true);
                if (! selectedSummary->presentation.demos.empty()) demoLinks (ctx, selectedSummary->presentation, deferred, true);
                muted (pendingIntent == Intent::Resolve ? "Checking this version's Creative Commons license..."
                    : "A Creative Commons license and a downloadable version must be checked before Download is available.");
                sourceButton (selectedSummary->sourceUrl, deferred);
                if (! selectedSummary->description.empty()) ImGui::TextWrapped ("%s", selectedSummary->description.c_str());
            }
            else
            {
                heading (ctx, "Choose an instrument");
                muted ("Select a search result to see its source, license and download. Packs are added to your library only when you choose Download.");
            }
        }
        ImGui::EndChild();
    }

    void beginBrowse (unsigned page, bool cached, bool existingSearch = false)
    {
        if (sourceIndex < 0 || sourceIndex >= static_cast<int> (sources.size())) return;
        const auto source = existingSearch ? activeSource : sources[static_cast<std::size_t> (sourceIndex)].id;
        const std::string search = existingSearch ? activeQuery : query.data();
        if (start (Intent::Browse, [&] { return service.browse (source, page, search, cached); }))
        {
            requestedSource = source;
            requestedQuery = search;
            requestedPage = page;
        }
    }

    void drawPackDetails (const sfz::ProviderPack& pack)
    {
        ImGui::TextWrapped ("%s", sizeLabel (pack.advertisedBytes).c_str());
        if (pack.sourceId == "sfzinstruments") ImGui::TextWrapped ("Git commit: %s", pack.releaseId.c_str());
        else ImGui::TextWrapped ("Version: %s", pack.releaseId.c_str());
        ImGui::TextWrapped ("Source: %s", pack.sourceUrl.c_str());
        ImGui::TextWrapped ("Downloads use HTTPS.");
        ImGui::TextWrapped ("%s", pack.expectedSha256.empty()
            ? "The repository provides no archive checksum. Downloaded bytes are hashed for the local library receipt."
            : "The download is checked against the repository's SHA-256 checksum.");
    }

    void drawInstalled (dw::Context& ctx, const sfz::LibrarySnapshot& snapshot, float height,
                        std::function<void()>& deferred)
    {
        const bool available = ! snapshot.busy && snapshot.state != LibraryState::Paused;
        const float top = ImGui::GetCursorScreenPos().y;
        const float width = ImGui::GetContentRegionAvail().x;
        if (button ("refresh-installed", "Refresh library", available)) start (Intent::Refresh, [&] { return service.requestInstalled(); });
        if (openLocalFolders)
        {
            if (width > ctx.s (400)) ImGui::SameLine();
            if (button ("local-folders", "Local SFZ folders", available)) deferred = openLocalFolders;
        }
        if (width > ctx.s (680)) ImGui::SameLine();
        muted (std::to_string (snapshot.installed.size()) + " installed packs");
        const float listWidth = std::max (ctx.s (100), (width - ImGui::GetStyle().ItemSpacing.x) * 0.32f);
        const float bodyHeight = std::max (ctx.s (40), height - (ImGui::GetCursorScreenPos().y - top));
        const sfz::InstalledPack* selected = nullptr;
        for (const auto& pack : snapshot.installed) if (packKey (pack) == selectedInstalled) selected = &pack;
        if (ImGui::BeginChild ("##sfz-installed-list", { listWidth, bodyHeight }, ImGuiChildFlags_Borders))
        {
            if (snapshot.installed.empty())
            {
                heading (ctx, "Your instrument library", 18);
                muted ("Downloaded packs appear here. Open Online to find Creative Commons instruments, or use Local SFZ folders.");
            }
            for (std::size_t i = 0; i < snapshot.installed.size(); ++i)
            {
                const auto& pack = snapshot.installed[i];
                std::string version = "Version " + pack.provider.releaseId.substr (0, 10);
                const auto sameVersion = std::count_if (snapshot.installed.begin(), snapshot.installed.end(), [&pack] (const auto& other)
                {
                    return other.provider.sourceId == pack.provider.sourceId && other.provider.packId == pack.provider.packId
                        && other.provider.releaseId == pack.provider.releaseId;
                });
                if (! pack.provider.versionPinned || sameVersion > 1) version += " / " + pack.archiveSha256.substr (0, 6);
                version += " | " + sourceName (pack.provider.sourceId);
                if (row (ctx, "installed-item-" + std::to_string (i), displayTitle (pack.provider.displayName, pack.provider.sourceId),
                    version, packKey (pack) == selectedInstalled, available,
                    ctx.s (96), pack.provider.license.id + " | Installed"))
                { selectedInstalled = packKey (pack); selectedInstrument.clear(); selected = &pack; }
                const auto tooltip = pack.provider.releaseId + "\n" + pack.archiveSha256 + "\n" + pack.provider.sourceUrl;
                formTooltip (tooltip.c_str());
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild ("##sfz-installed-detail", { 0, bodyHeight }, ImGuiChildFlags_Borders))
        {
            if (selected)
            {
                heading (ctx, displayTitle (selected->provider.displayName, selected->provider.sourceId));
                muted (sourceName (selected->provider.sourceId));
                ImGui::TextColored (ImGui::ColorConvertU32ToFloat4 (consolePalette().eqAccent),
                                    "Verified license: %s", selected->provider.license.id.c_str());
                ImGui::TextUnformatted ("Instrument file");
                const sfz::ProviderInstrument* instrument = nullptr;
                for (const auto& candidate : selected->instruments) if (candidate.id == selectedInstrument) instrument = &candidate;
                ImGui::BeginDisabled (! available);
                ImGui::SetNextItemWidth (-1);
                const auto comboAt = ImGui::GetCursorScreenPos();
                const float comboWidth = ImGui::GetContentRegionAvail().x;
                const ImVec2 comboEnd { comboAt.x + comboWidth, comboAt.y + ImGui::GetFrameHeight() };
                if (ImGui::IsRectVisible (comboAt, comboEnd)) controls["instrument"] = { comboAt, comboEnd };
                if (ImGui::BeginCombo ("##sfz-instrument", instrument ? instrument->relativePath.c_str() : "Select an SFZ file"))
                {
                    for (std::size_t i = 0; i < selected->instruments.size(); ++i)
                    {
                        const auto& candidate = selected->instruments[i];
                        ImGui::PushID (static_cast<int> (i));
                        if (ImGui::Selectable (candidate.relativePath.c_str(), candidate.id == selectedInstrument))
                        { selectedInstrument = candidate.id; instrument = &candidate; }
                        remember ("instrument-choice-" + std::to_string (i));
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();
                if (button ("load", "Load", available && instrument && static_cast<bool> (onLoad), { ctx.s (140), ctx.s (40) }, true))
                {
                    const auto callback = onLoad;
                    const auto pack = *selected;
                    const auto entry = *instrument;
                    deferred = [callback, pack, entry] { callback (pack, entry); };
                }
                if (ImGui::GetContentRegionAvail().x >= ctx.s (280)) ImGui::SameLine();
                const auto used = inUse ? inUse() : std::vector<sfz::ManagedSfzLocator>{};
                const bool referenced = std::any_of (used.begin(), used.end(), [&] (const auto& locator)
                {
                    return locator.sourceId == selected->provider.sourceId && locator.packId == selected->provider.packId
                        && locator.releaseId == selected->provider.releaseId && locator.archiveSha256 == selected->archiveSha256;
                });
                if (button ("remove", "Remove", available && ! referenced && static_cast<bool> (inUse)))
                    start (Intent::Remove, [&] { return service.remove (*selected, used); });
                if (referenced) ImGui::TextWrapped ("This version is used by the current session. Unload its instruments before removing it.");
                if (! inUse) ImGui::TextWrapped ("Session use cannot be checked, so Remove is unavailable.");
                if (instrument && instrument->candidateOnly)
                    ImGui::TextWrapped ("This is a discovered SFZ file. Choose the instrument's main file; include fragments may also appear in the list.");
                ImGui::Separator();
                if (! selected->provider.author.empty() && selected->provider.author != selected->provider.sourceId
                    && selected->provider.author != sourceName (selected->provider.sourceId)) muted (selected->provider.author);
                ImGui::TextWrapped ("%s", selected->provider.license.constraints.c_str());
                sourceButton (selected->provider.sourceUrl, deferred);
                licenseButton (selected->provider, deferred);
                if (ImGui::CollapsingHeader ("Version and download details"))
                {
                    drawPackDetails (selected->provider);
                    ImGui::TextWrapped ("Local archive SHA-256: %s", selected->archiveSha256.c_str());
                }
                licenseText (selected->provider);
            }
            else
            {
                heading (ctx, "Ready to play");
                muted ("Choose an installed pack, select its instrument file, then Load it onto this track.");
            }
        }
        ImGui::EndChild();
    }

    void drawStatus (dw::Context& ctx, const sfz::LibrarySnapshot& snapshot, float height)
    {
        if (ImGui::BeginChild ("##sfz-library-status", { 0, height }, ImGuiChildFlags_None))
        {
            ImGui::TextUnformatted (stateLabel (snapshot.state, snapshot.busy));
            if (snapshot.state == LibraryState::Paused)
            {
                ImGui::SameLine();
                if (button ("resume", "Resume", ! snapshot.busy, {}, true))
                {
                    const auto before = service.snapshot();
                    if (service.resume()) { requestedGeneration = before.generation + 1; frameMutation = true; notice.clear(); }
                    else notice = "Recording or rendering is still active. Stop it before resuming library work.";
                }
            }
            if (snapshot.busy || snapshot.state == LibraryState::Paused)
            {
                ImGui::SameLine();
                if (button ("cancel", "Cancel")) { service.cancel(); frameMutation = true; }
            }
            if (snapshot.state == LibraryState::Downloading || snapshot.state == LibraryState::Extracting)
            {
                const float fraction = snapshot.progress.totalBytes == 0 ? 0.0f
                    : static_cast<float> (std::min (1.0, static_cast<double> (snapshot.progress.completedBytes) / static_cast<double> (snapshot.progress.totalBytes)));
                const auto label = (snapshot.progress.completedBytes == 0 ? std::string ("0 bytes") : sizeLabel (snapshot.progress.completedBytes))
                    + " / " + sizeLabel (snapshot.progress.totalBytes);
                ImGui::ProgressBar (fraction, { -1, ctx.s (22) }, label.c_str());
            }
            if (snapshot.state == LibraryState::Paused)
            {
                ImGui::TextWrapped ("%s", kPauseMessage);
            }
            if (! snapshot.error.empty() && snapshot.state != LibraryState::Paused
                && snapshot.state != LibraryState::Cancelled && snapshot.state != LibraryState::Complete)
                ImGui::TextWrapped ("%s", snapshot.error.c_str());
            if (! notice.empty()) ImGui::TextWrapped ("%s", notice.c_str());
        }
        ImGui::EndChild();
    }

    sfz::LibraryService& service;
    std::function<void (const sfz::InstalledPack&, const sfz::ProviderInstrument&)> onLoad;
    std::function<std::vector<sfz::ManagedSfzLocator>()> inUse;
    std::function<void (std::string)> openSource;
    std::function<void()> openLocalFolders;
    std::vector<sfz::ProviderSource> sources;
    std::vector<sfz::ProviderSummary> results;
    std::optional<sfz::ProviderSummary> selectedSummary;
    std::optional<sfz::ProviderPack> resolved;
    std::array<char, 257> query {};
    std::string selectedInstalled, selectedInstrument, resolvingSelection, notice;
    std::string activeSource, activeQuery, requestedSource, requestedQuery, browseWarning;
    int sourceIndex = 0;
    unsigned currentPage = 0, nextPage = 0, requestedPage = 0;
    bool pageCached = false, resolutionCached = false, forceInstalled = false;
    bool onlineTab = false;
    bool keyboardActive = false, keyboardWasActive = false, frameMutation = false;
    std::uint64_t requestedGeneration = 0;
    Intent pendingIntent = Intent::None;
    std::map<std::string, std::pair<ImVec2, ImVec2>> controls;
};
} // namespace

std::unique_ptr<DuskPanelView> makeSfzRepositoryView (
    sfz::LibraryService& service,
    std::function<void (const sfz::InstalledPack&, const sfz::ProviderInstrument&)> onLoad,
    std::function<std::vector<sfz::ManagedSfzLocator>()> inUse,
    std::function<void (std::string)> openSource,
    std::function<void()> openLocalFolders)
{
    return std::make_unique<SfzRepositoryView> (service, std::move (onLoad), std::move (inUse),
                                               std::move (openSource), std::move (openLocalFolders));
}
} // namespace duskstudio::imgui
