#pragma once

#include "DuskPanelWindow.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace duskstudio::imgui
{
// One row of the recent-sessions table, already formatted for display.
struct RecentSession
{
    std::string path;
    std::string name;
    std::string sampleRate;
    std::string bitDepth;
    std::string lastModified;
};

// Reads each session directory's modification time and the format of the first audio
// file in it. session.json carries no session-level sample rate, so peeking at a
// header is the only way to fill those columns without a running engine.
std::vector<RecentSession> scanRecentSessions (const std::vector<std::filesystem::path>& paths);

// Every action is a callback; the view holds no session state.
struct StartupCallbacks
{
    std::function<void (const std::string& path)> openRecent;
    std::function<void()> newSession;
    std::function<void()> openFile;
    std::function<void()> skip;
    std::function<void()> quit;
    std::function<void()> openDownloads;
};

// The launch dialog: the wordmark and the Recent / Open / New nav down the left, the
// recent-sessions table in the middle, Quit and Open along the bottom.
class StartupView : public DuskPanelView
{
public:
    // Shows the update banner above the table heading. Called when the async tag
    // check finds a release newer than this build.
    virtual void setUpdateAvailable (const std::string& tagName) = 0;
};

// `brandRgba` is the decoded app icon, width * height * 4 bytes, and must outlive the
// view: the framework side has no image reader, so the host decodes it.
std::unique_ptr<StartupView> makeStartupView (std::vector<RecentSession> recents,
                                              const unsigned char* brandRgba,
                                              int brandWidth, int brandHeight,
                                              StartupCallbacks callbacks);
} // namespace duskstudio::imgui
