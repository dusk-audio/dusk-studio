#include "PluginScanModal.h"
#include "../engine/PluginManager.h"
#include <cstdio>

namespace duskstudio
{
PluginScanModal::PluginScanModal (PluginManager& mgr,
                                  std::function<void (int)> onFinishedIn)
    : manager (mgr),
      onFinished (std::move (onFinishedIn)),
      progressBar (progressValue)
{
    setSize (420, 150);

    titleLabel.setText ("Scanning plugins...", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe0e0e0));
    addAndMakeVisible (titleLabel);

    statusLabel.setText ("Starting...", juce::dontSendNotification);
    statusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xffa0a0a0));
    statusLabel.setMinimumHorizontalScale (1.0f);   // ellipsise long paths, don't shrink
    addAndMakeVisible (statusLabel);

    progressBar.setColour (juce::ProgressBar::backgroundColourId, juce::Colour (0xff101012));
    progressBar.setColour (juce::ProgressBar::foregroundColourId, juce::Colour (0xff5fa8ff));
    addAndMakeVisible (progressBar);

    startedAtMs = juce::Time::getMillisecondCounter();
    std::fprintf (stderr, "[Dusk Studio] PluginScanModal: created, starting worker thread\n");
    std::fflush (stderr);

    worker = std::make_unique<Worker> (*this);
    if (! worker->startThread())
    {
        // Thread launch failed — mark the scan done so the timer's completion
        // path fires and the modal closes instead of hanging on "Scanning...".
        std::fprintf (stderr, "[Dusk Studio] PluginScanModal: worker thread failed to start\n");
        std::fflush (stderr);
        scanDone.store (true, std::memory_order_release);
    }

    startTimerHz (20);
}

PluginScanModal::~PluginScanModal()
{
    stopTimer();
    // Tell the scanner to abort the in-flight child (so we don't wait out a
    // slow plugin's full timeout), then join.
    aborting.store (true, std::memory_order_relaxed);
    if (worker != nullptr)
    {
        worker->signalThreadShouldExit();
        worker->stopThread (5000);
        worker.reset();
    }
}

void PluginScanModal::Worker::run()
{
    std::fprintf (stderr, "[Dusk Studio] plugin-scan worker: started\n");
    std::fflush (stderr);

    const int added = owner.manager.scanInstalledPlugins (
        [this] (float frac, const juce::String& name) -> bool
        {
            owner.progress.store (frac, std::memory_order_relaxed);
            {
                const juce::ScopedLock sl (owner.nameLock);
                owner.currentName = name;
            }
            return ! threadShouldExit();
        },
        &owner.aborting);

    owner.addedCount.store (added, std::memory_order_relaxed);
    owner.scanDone.store (true, std::memory_order_release);

    std::fprintf (stderr, "[Dusk Studio] plugin-scan worker: done, %d added\n", added);
    std::fflush (stderr);
}

void PluginScanModal::timerCallback()
{
    progressValue = (double) progress.load (std::memory_order_relaxed);
    progressBar.repaint();

    // While still scanning, show the plugin currently being probed.
    if (! completeShown)
    {
        juce::String name;
        { const juce::ScopedLock sl (nameLock); name = currentName; }
        if (name.isNotEmpty())
            statusLabel.setText (name, juce::dontSendNotification);
    }

    // First tick that observes completion: switch to the "complete" state and
    // start the minimum-visible timer (do NOT close yet).
    if (scanDone.load (std::memory_order_acquire) && ! completeShown)
    {
        completeShown = true;
        completeAtMs  = juce::Time::getMillisecondCounter();

        const int added = addedCount.load (std::memory_order_relaxed);
        titleLabel.setText ("Plugin scan complete", juce::dontSendNotification);
        statusLabel.setText (juce::String (added) + " new plugin"
                                 + (added == 1 ? "" : "s") + " added.",
                             juce::dontSendNotification);
        progressValue = 1.0;
        progressBar.repaint();
    }

    // Hold the completion state briefly so a warm-cache scan doesn't just
    // flash, then close.
    if (completeShown && ! finishedFired
        && juce::Time::getMillisecondCounter() - completeAtMs >= (juce::uint32) kMinVisibleMs)
    {
        finishedFired = true;
        stopTimer();
        // Defer onFinished off the timer stack: the caller's callback closes
        // (destroys) this modal, and invoking it synchronously here would
        // unwind back into a freed object. Copy the callback + count so the
        // lambda never touches `this`.
        if (onFinished)
            juce::MessageManager::callAsync (
                [cb = onFinished, count = addedCount.load (std::memory_order_relaxed)]
                { cb (count); });
    }
}

void PluginScanModal::resized()
{
    auto area = getLocalBounds().reduced (20);
    titleLabel.setBounds (area.removeFromTop (26));
    area.removeFromTop (10);
    progressBar.setBounds (area.removeFromTop (22));
    area.removeFromTop (12);
    statusLabel.setBounds (area.removeFromTop (20));
}

void PluginScanModal::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff202024));
}
} // namespace duskstudio
