#include "PluginScanModal.h"
#include "../engine/PluginManager.h"

namespace duskstudio
{
PluginScanModal::PluginScanModal (PluginManager& mgr,
                                  std::function<void (int)> onFinishedIn)
    : manager (mgr),
      onFinished (std::move (onFinishedIn)),
      progressBar (progressValue)
{
    setSize (420, 150);

    titleLabel.setText ("Scanning plugins…", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe0e0e0));
    addAndMakeVisible (titleLabel);

    statusLabel.setText ("Starting…", juce::dontSendNotification);
    statusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xffa0a0a0));
    statusLabel.setMinimumHorizontalScale (1.0f);   // ellipsise long paths, don't shrink
    addAndMakeVisible (statusLabel);

    progressBar.setColour (juce::ProgressBar::backgroundColourId, juce::Colour (0xff101012));
    progressBar.setColour (juce::ProgressBar::foregroundColourId, juce::Colour (0xff5fa8ff));
    addAndMakeVisible (progressBar);

    worker = std::make_unique<Worker> (*this);
    worker->startThread();

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
}

void PluginScanModal::timerCallback()
{
    progressValue = (double) progress.load (std::memory_order_relaxed);
    progressBar.repaint();

    juce::String name;
    { const juce::ScopedLock sl (nameLock); name = currentName; }
    if (name.isNotEmpty())
        statusLabel.setText (name, juce::dontSendNotification);

    if (scanDone.load (std::memory_order_acquire) && ! finishedFired)
    {
        finishedFired = true;
        stopTimer();

        const int added = addedCount.load (std::memory_order_relaxed);
        titleLabel.setText ("Plugin scan complete", juce::dontSendNotification);
        statusLabel.setText (juce::String (added) + " new plugin"
                                 + (added == 1 ? "" : "s") + " added.",
                             juce::dontSendNotification);
        progressValue = 1.0;
        progressBar.repaint();

        if (onFinished)
            onFinished (added);   // caller closes the modal; our dtor joins the (done) worker
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
