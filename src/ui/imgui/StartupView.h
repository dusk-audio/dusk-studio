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

// What the user picked. Every one of these dismisses the dialog, and the host runs
// the choice only once the panel is down: a file chooser opened while the framework
// child is still mapped would land behind it.
enum class StartupAction
{
    none,
    openRecent,
    newSession,
    openFile,
    skip,
    quit
};

// The launch dialog: the wordmark and the Recent / Open / New nav down the left, the
// recent-sessions table in the middle, Quit and Open along the bottom.
class StartupView : public DuskPanelView
{
public:
    // Shows the update banner above the table heading. Called when the async tag
    // check finds a release newer than this build.
    virtual void setUpdateAvailable (const std::string& tagName) = 0;

    // Read from the dismissed callback, while the view is still alive.
    virtual StartupAction chosenAction() const = 0;
    virtual const std::string& chosenPath() const = 0;
};

// `brandRgba` is the decoded app icon, width * height * 4 bytes, and must outlive the
// view: the framework side has no image reader, so the host decodes it.
// `openDownloads` is the one action that does not dismiss: the update banner hands
// the viewer to a browser and leaves the dialog where it was.
std::unique_ptr<StartupView> makeStartupView (std::vector<RecentSession> recents,
                                              const unsigned char* brandRgba,
                                              int brandWidth, int brandHeight,
                                              std::function<void()> openDownloads);
} // namespace duskstudio::imgui
