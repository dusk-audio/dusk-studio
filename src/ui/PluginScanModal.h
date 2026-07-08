#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <functional>
#include <memory>

namespace duskstudio
{
class PluginManager;

// In-window modal shown while PluginManager::scanInstalledPlugins runs on a
// background thread. A 20 Hz Timer polls scan progress and updates a progress
// bar + "current plugin" label, so a scan-on-startup (or a manual rescan)
// doesn't look like a frozen app. Mirrors BounceDialog's worker-thread +
// Timer pattern.
//
// Lifetime: handed to an EmbeddedModal (the caller keeps the modal alive).
// When the scan finishes, the Timer fires onFinished(addedCount) so the caller
// can close the modal + surface a result. The destructor signals the worker to
// abort mid-file (via the atomic the scanner polls between child-process reads)
// and joins it, so quitting the app mid-scan doesn't hang on a slow plugin.
class PluginScanModal final : public juce::Component,
                              private juce::Timer
{
public:
    PluginScanModal (PluginManager& manager,
                     std::function<void (int addedCount)> onFinished);
    ~PluginScanModal() override;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    struct Worker final : juce::Thread
    {
        explicit Worker (PluginScanModal& o)
            : juce::Thread ("Dusk-PluginScan"), owner (o) {}
        void run() override;
        PluginScanModal& owner;
    };

    PluginManager&                       manager;
    std::function<void (int)>            onFinished;

    std::atomic<bool>  aborting   { false };
    std::atomic<bool>  scanDone   { false };
    std::atomic<float> progress   { 0.0f };
    std::atomic<int>   addedCount { 0 };

    juce::CriticalSection nameLock;
    juce::String          currentName;   // guarded by nameLock

    juce::Label        titleLabel;
    juce::Label        statusLabel;
    // Declared BEFORE progressBar: the ProgressBar ctor binds a double& to
    // this, and members construct in declaration order — so it must be live
    // (initialised) before progressBar's ctor runs.
    double             progressValue { 0.0 };
    juce::ProgressBar  progressBar;
    bool               finishedFired { false };

    // Keep the completion state on screen for a beat so a fast (warm-cache)
    // scan doesn't just flash and vanish — otherwise an enabled scan-on-startup
    // looks like nothing happened.
    bool          completeShown { false };
    std::uint32_t  completeAtMs  { 0 };
    std::uint32_t  startedAtMs   { 0 };
    static constexpr int kMinVisibleMs = 900;

    std::unique_ptr<Worker> worker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginScanModal)
};
} // namespace duskstudio
