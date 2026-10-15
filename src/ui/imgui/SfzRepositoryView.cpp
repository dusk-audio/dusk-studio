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

std::string packKey (const sfz::InstalledPack& pack)
{
    return pack.provider.sourceId + "/" + pack.provider.packId + "/" + pack.provider.releaseId + "/" + pack.archiveSha256;
}

std::string summaryKey (const sfz::ProviderSummary& summary)
{
    return summary.sourceId + "/" + summary.packId;
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
          openSource (std::move (source)), openLocalFolders (std::move (local)), sources (sfz::providerSources()) {}

    ImVec2 preferredSize() const override { return { 780.0f, 580.0f }; }
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
        ctx.dl->AddRectFilled (origin, { origin.x + size.x, origin.y + size.y }, ctx.theme->panelFill);
        ImGui::SetCursorScreenPos (origin);
        if (ImGui::BeginChild ("##sfz-repository-body", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
        {
            ImGui::TextUnformatted ("SFZ instrument library");
            const float footerHeight = ctx.s (112.0f);
            const float panelHeight = std::max (ctx.s (140.0f), ImGui::GetContentRegionAvail().y - footerHeight - ctx.s (38.0f));
            if (ImGui::BeginTabBar ("##sfz-library-tabs"))
            {
                const auto installedFlag = forceInstalled ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                const bool installed = ImGui::BeginTabItem ("Installed", nullptr, installedFlag);
                remember ("installed-tab");
                if (installed)
                {
                    forceInstalled = false;
                    drawInstalled (ctx, snapshot, panelHeight, deferred);
                    ImGui::EndTabItem();
                }
                const bool online = ImGui::BeginTabItem ("Online");
                remember ("online-tab");
                if (online)
                {
                    drawOnline (ctx, snapshot, panelHeight, deferred);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::Separator();
            drawStatus (ctx, snapshot);
        }
        ImGui::EndChild();
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

    bool button (const char* name, const char* label, bool enabled = true, ImVec2 size = {})
    {
        ImGui::BeginDisabled (! enabled || frameMutation);
        const bool clicked = ImGui::Button (label, size);
        remember (name);
        ImGui::EndDisabled();
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

    bool row (const std::string& control, const std::string& title, const std::string& subtitle,
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
        list->AddText ({ first.x + 4, first.y + 3 }, colour, title.c_str());
        const float line = ImGui::GetTextLineHeight();
        const ImVec2 subtitleAt { first.x + 4, first.y + line + 6 };
        list->PushClipRect (subtitleAt, { last.x, subtitleAt.y + line }, true);
        list->AddText (subtitleAt, ImGui::GetColorU32 (ImGuiCol_TextDisabled), subtitle.c_str());
        list->PopClipRect();
        if (! hint.empty()) list->AddText ({ first.x + 4, first.y + 2 * line + 9 },
                                          ImGui::GetColorU32 (ImGuiCol_TextDisabled), hint.c_str());
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

    void drawOnline (dw::Context& ctx, const sfz::LibrarySnapshot& snapshot, float height,
                     std::function<void()>& deferred)
    {
        const bool available = ! snapshot.busy && snapshot.state != LibraryState::Paused;
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::BeginDisabled (! available);
        ImGui::SetNextItemWidth (ctx.s (190.0f));
        ComboModel sourceModel;
        for (const auto& source : sources) sourceModel.add (source.name);
        sourceModel.finish (sourceIndex);
        const auto top = ImGui::GetCursorScreenPos();
        if (formCombo (ctx, "##sfz-source", top, { top.x + ctx.s (190.0f), top.y + ctx.s (28.0f) }, sourceModel, available))
        {
            sourceIndex = sourceModel.selected;
            results.clear(); nextPage = 0; currentPage = 0; clearSelection();
        }
        controls["source"] = { top, { top.x + ctx.s (190.0f), top.y + ctx.s (28.0f) } };
        ImGui::SameLine();
        ImGui::SetNextItemWidth (std::max (ctx.s (80.0f), width - ctx.s (286.0f)));
        if (ImGui::InputTextWithHint ("##sfz-query", "Search instruments", query.data(), query.size()))
        { results.clear(); nextPage = 0; currentPage = 0; clearSelection(); }
        remember ("query");
        ImGui::SameLine();
        if (button ("browse", currentPage == 0 ? "Browse" : "Refresh", available)) beginBrowse (1, false);
        if (button ("cached", "Browse cached", available)) beginBrowse (1, true);
        ImGui::SameLine();
        if (button ("next-page", "Next page", available && nextPage != 0)) beginBrowse (nextPage, pageCached);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (currentPage != 0) ImGui::Text ("Page %u%s", currentPage, pageCached ? "  (Cached)" : "");
        const float bodyHeight = std::max (ctx.s (80.0f), height - ctx.s (72.0f));
        const float listWidth = std::min (ctx.s (280.0f), width * 0.42f);
        if (ImGui::BeginChild ("##sfz-online-list", { listWidth, bodyHeight }, ImGuiChildFlags_None))
        {
            if (results.empty()) ImGui::TextWrapped ("Select a repository and Browse, or Browse cached for saved results. Downloads begin only when you choose Download.");
            for (std::size_t i = 0; i < results.size(); ++i)
            {
                const auto& summary = results[i];
                const auto hint = summary.licenseHint.empty() || summary.licenseHint == "NOASSERTION"
                    ? "License check on selection" : summary.licenseHint;
                if (row ("online-item-" + std::to_string (i), summary.name,
                    summary.author, selectedSummary && summaryKey (*selectedSummary) == summaryKey (summary),
                    available, ctx.s (68.0f), hint))
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
        if (ImGui::BeginChild ("##sfz-online-detail", { 0, bodyHeight }, ImGuiChildFlags_None))
        {
            if (selectedSummary)
            {
                ImGui::TextWrapped ("%s", selectedSummary->name.c_str());
                ImGui::TextWrapped ("%s", selectedSummary->author.c_str());
                ImGui::TextWrapped ("%s", selectedSummary->description.c_str());
                sourceButton (selectedSummary->sourceUrl, deferred);
            }
            if (resolved)
            {
                ImGui::Separator();
                if (resolutionCached) ImGui::TextUnformatted ("Cached version and license details");
                ImGui::TextWrapped ("License: %s", resolved->license.id.c_str());
                ImGui::TextWrapped ("%s", resolved->license.constraints.c_str());
                licenseButton (*resolved, deferred);
                bool installed = false;
                for (const auto& pack : snapshot.installed)
                    installed = installed || (resolved->versionPinned && pack.provider.sourceId == resolved->sourceId
                        && pack.provider.packId == resolved->packId && pack.provider.releaseId == resolved->releaseId);
                if (button ("download", installed ? "Installed" : "Download", available && ! installed))
                    start (Intent::Install, [&] { return service.install (*resolved); });
                if (installed) ImGui::TextWrapped ("Choose this version in Installed to load it.");
                drawPackDetails (*resolved);
                licenseText (*resolved);
            }
            else if (selectedSummary) ImGui::TextWrapped ("A Creative Commons license and a downloadable version must be checked before Download becomes available.");
        }
        ImGui::EndChild();
    }

    void beginBrowse (unsigned page, bool cached)
    {
        if (sourceIndex < 0 || sourceIndex >= static_cast<int> (sources.size())) return;
        if (start (Intent::Browse, [&] { return service.browse (sources[static_cast<std::size_t> (sourceIndex)].id, page, query.data(), cached); }))
        {
            results.clear(); clearSelection(); nextPage = 0;
            currentPage = page; pageCached = false;
        }
    }

    void drawPackDetails (const sfz::ProviderPack& pack)
    {
        ImGui::TextWrapped ("%s", sizeLabel (pack.advertisedBytes).c_str());
        if (pack.sourceId == "sfzinstruments") ImGui::TextWrapped ("Git commit: %s", pack.releaseId.c_str());
        else ImGui::TextWrapped ("Version: %s", pack.releaseId.c_str());
        ImGui::TextWrapped ("Downloads use HTTPS.");
        ImGui::TextWrapped ("%s", pack.expectedSha256.empty()
            ? "The repository provides no archive checksum. Downloaded bytes are hashed for the local library receipt."
            : "The download is checked against the repository's SHA-256 checksum.");
    }

    void drawInstalled (dw::Context& ctx, const sfz::LibrarySnapshot& snapshot, float height,
                        std::function<void()>& deferred)
    {
        const bool available = ! snapshot.busy && snapshot.state != LibraryState::Paused;
        if (button ("refresh-installed", "Refresh library", available)) start (Intent::Refresh, [&] { return service.requestInstalled(); });
        if (openLocalFolders)
        {
            ImGui::SameLine();
            if (button ("local-folders", "Local SFZ folders", available)) deferred = openLocalFolders;
        }
        const float width = ImGui::GetContentRegionAvail().x;
        const float listWidth = std::min (ctx.s (280.0f), width * 0.42f);
        const float bodyHeight = std::max (ctx.s (80.0f), height - ctx.s (34.0f));
        const sfz::InstalledPack* selected = nullptr;
        for (const auto& pack : snapshot.installed) if (packKey (pack) == selectedInstalled) selected = &pack;
        if (ImGui::BeginChild ("##sfz-installed-list", { listWidth, bodyHeight }, ImGuiChildFlags_None))
        {
            if (snapshot.installed.empty()) ImGui::TextWrapped ("No repository packs are installed. Open Online to browse Creative Commons SFZ instruments, or use Local SFZ folders.");
            for (std::size_t i = 0; i < snapshot.installed.size(); ++i)
            {
                const auto& pack = snapshot.installed[i];
                if (row ("installed-item-" + std::to_string (i), pack.provider.displayName,
                    pack.provider.license.id + " | " + pack.provider.releaseId.substr (0, 12),
                    packKey (pack) == selectedInstalled, available, ctx.s (50.0f)))
                { selectedInstalled = packKey (pack); selectedInstrument.clear(); selected = &pack; }
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild ("##sfz-installed-detail", { 0, bodyHeight }, ImGuiChildFlags_None))
        {
            if (selected)
            {
                ImGui::TextWrapped ("%s", selected->provider.displayName.c_str());
                ImGui::TextWrapped ("%s", selected->provider.author.c_str());
                ImGui::TextWrapped ("License: %s", selected->provider.license.id.c_str());
                ImGui::TextWrapped ("%s", selected->provider.license.constraints.c_str());
                ImGui::Separator();
                ImGui::TextUnformatted ("Choose an SFZ file");
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
                if (button ("load", "Load", available && instrument && static_cast<bool> (onLoad)))
                {
                    const auto callback = onLoad;
                    const auto pack = *selected;
                    const auto entry = *instrument;
                    deferred = [callback, pack, entry] { callback (pack, entry); };
                }
                ImGui::SameLine();
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
                sourceButton (selected->provider.sourceUrl, deferred);
                licenseButton (selected->provider, deferred);
                drawPackDetails (selected->provider);
                ImGui::TextWrapped ("Local archive SHA-256: %s", selected->archiveSha256.c_str());
                licenseText (selected->provider);
            }
            else ImGui::TextWrapped ("Select an installed version to choose its SFZ file.");
        }
        ImGui::EndChild();
    }

    void drawStatus (dw::Context& ctx, const sfz::LibrarySnapshot& snapshot)
    {
        if (ImGui::BeginChild ("##sfz-library-status", { 0, ctx.s (106.0f) }, ImGuiChildFlags_None))
        {
            ImGui::TextUnformatted (stateLabel (snapshot.state, snapshot.busy));
            if (snapshot.state == LibraryState::Downloading || snapshot.state == LibraryState::Extracting)
            {
                const float fraction = snapshot.progress.totalBytes == 0 ? 0.0f
                    : static_cast<float> (std::min (1.0, static_cast<double> (snapshot.progress.completedBytes) / static_cast<double> (snapshot.progress.totalBytes)));
                const auto label = (snapshot.progress.completedBytes == 0 ? std::string ("0 bytes") : sizeLabel (snapshot.progress.completedBytes))
                    + " / " + sizeLabel (snapshot.progress.totalBytes);
                ImGui::ProgressBar (fraction, { -1, ctx.s (18.0f) }, label.c_str());
            }
            if (snapshot.state == LibraryState::Paused)
            {
                ImGui::TextWrapped ("Paused while recording or rendering. Stop recording or the render, then select Resume.");
                if (button ("resume", "Resume", ! snapshot.busy))
                {
                    const auto before = service.snapshot();
                    if (service.resume()) { requestedGeneration = before.generation + 1; frameMutation = true; notice.clear(); }
                    else notice = "Recording or rendering is still active. Stop it before resuming library work.";
                }
                ImGui::SameLine();
            }
            if (snapshot.busy || snapshot.state == LibraryState::Paused)
                if (button ("cancel", "Cancel")) { service.cancel(); frameMutation = true; }
            if (! snapshot.error.empty()) ImGui::TextWrapped ("%s", snapshot.error.c_str());
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
    int sourceIndex = 0;
    unsigned currentPage = 0, nextPage = 0;
    bool pageCached = false, resolutionCached = false, forceInstalled = false;
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
