#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../../engine/sfz/SfzLibrary.h"

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace duskstudio
{
// Offline instrument library. Lists the .sfz and .sf2 already on this machine
// under the configured roots, so loading an instrument does not require knowing
// where it lives. Shown through EmbeddedModal like the plugin picker, and
// deliberately separate from "Open local file", which still walks the
// filesystem for a path the library does not cover.
//
// Nothing here reaches the network. The scan starts only when the panel opens
// or the user asks for it, never at startup and never during a session load,
// and it runs on a worker this panel owns and joins.
class SfzLibraryPanel final : public juce::Component,
                              private juce::Timer
{
public:
    struct Callbacks
    {
        // Fires with the chosen instrument. The panel does not load anything
        // itself; the editor owns that path and its async plumbing.
        std::function<void (const juce::File&)> onPick;
        std::function<void()> onBrowseFile;
        std::function<void()> onCancel;
    };

    // `roots` overrides the configured library roots. Empty, the normal case,
    // scans the per-OS defaults plus the user's own. The capture harness passes
    // a fixture directory so the figure does not depend on what this machine
    // happens to have installed.
    explicit SfzLibraryPanel (Callbacks cb,
                              std::vector<std::filesystem::path> roots = {});
    ~SfzLibraryPanel() override;

    // Moves a finished scan into the list and returns true when it did. The
    // timer calls this; the capture harness calls it directly, because it
    // sleeps between frames rather than pumping the message loop.
    bool applyFinishedScan();

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    class ListBody;

    void timerCallback() override;
    void updateStatus();
    void startScan();
    void stopScan() noexcept;
    void addRootFolder();

    Callbacks callbacks;
    std::vector<std::filesystem::path> rootsOverride;

    juce::Label      titleLabel  { {}, "Instrument library" };
    juce::Label      statusLabel { {}, "" };
    juce::TextEditor filterEditor;
    juce::TextButton rescanBtn   { "Rescan" };
    juce::TextButton addRootBtn  { "Add folder..." };
    juce::TextButton browseBtn   { "Open local file..." };
    juce::TextButton closeBtn    { "Close" };

    std::unique_ptr<ListBody> listBody;

    // Owned worker. The flag is raised and the thread joined in stopScan, which
    // both the destructor and a fresh scan go through, so a scan can never
    // outlive the panel or overlap another.
    std::thread            scanThread;
    std::atomic<bool>      cancelScan { false };
    std::atomic<bool>      scanDone   { false };
    std::atomic<int>       rootsDone  { 0 };
    std::atomic<int>       rootsTotal { 0 };
    std::shared_ptr<sfz::ScanResult> pendingResult;

    static constexpr int kPad     = 10;
    static constexpr int kTitleH  = 28;
    static constexpr int kFilterH = 28;
    static constexpr int kStatusH = 20;
    static constexpr int kActionH = 32;
};

namespace sfzlibrary
{
// Opens the library over `host`'s top-level through the shared modal stack.
// `onPick` receives the chosen instrument and the modal closes first, so the
// caller's load runs against a panel that is already gone.
void show (juce::Component& host,
           std::function<void (const juce::File&)> onPick,
           std::function<void()> onBrowseFile);
} // namespace sfzlibrary
} // namespace duskstudio
