#include "StartupDialog.h"

namespace duskstudio
{
namespace
{
void styleHeaderButton (juce::TextButton& b)
{
    b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a2a30));
    b.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffe0e0e0));
}

void styleRecentButton (juce::TextButton& b)
{
    b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff181820));
    b.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffd0d0d0));
}
} // namespace

StartupDialog::StartupDialog (juce::Array<juce::File> r)
    : recents (std::move (r))
{
    titleLabel.setText ("Focal", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe8e8e8));
    addAndMakeVisible (titleLabel);

    recentsHeading.setText ("Recent sessions", juce::dontSendNotification);
    recentsHeading.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    recentsHeading.setColour (juce::Label::textColourId, juce::Colour (0xff909098));
    addAndMakeVisible (recentsHeading);

    emptyRecentsLabel.setText ("No recent sessions yet.", juce::dontSendNotification);
    emptyRecentsLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    emptyRecentsLabel.setColour (juce::Label::textColourId, juce::Colour (0xff707078));
    emptyRecentsLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (emptyRecentsLabel);
    emptyRecentsLabel.setVisible (recents.isEmpty());

    rebuildRecentButtons();

    styleHeaderButton (newSessionButton);
    styleHeaderButton (openFileButton);
    styleHeaderButton (skipButton);

    newSessionButton.onClick = [this]
    {
        if (onNewSession) onNewSession();
        closeDialog (1);
    };
    openFileButton.onClick = [this]
    {
        if (onOpenFile) onOpenFile();
        closeDialog (1);
    };
    skipButton.onClick = [this]
    {
        if (onSkip) onSkip();
        closeDialog (0);
    };

    addAndMakeVisible (newSessionButton);
    addAndMakeVisible (openFileButton);
    addAndMakeVisible (skipButton);
}

void StartupDialog::rebuildRecentButtons()
{
    recentButtons.clear();
    for (auto& dir : recents)
    {
        auto* b = recentButtons.add (new juce::TextButton);
        styleRecentButton (*b);
        // Folder name first (most identifying), then the parent path so two
        // sessions called "Mix" in different parents are distinguishable.
        const auto display = dir.getFileName()
                              + "  -  "
                              + dir.getParentDirectory().getFullPathName();
        b->setButtonText (display);
        b->setTooltip (dir.getFullPathName());
        b->setConnectedEdges (juce::Button::ConnectedOnTop | juce::Button::ConnectedOnBottom);
        b->onClick = [this, dir]
        {
            if (onOpenRecent) onOpenRecent (dir);
            closeDialog (1);
        };
        addAndMakeVisible (b);
    }
}

void StartupDialog::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff202024));
}

void StartupDialog::resized()
{
    auto area = getLocalBounds().reduced (16);

    titleLabel.setBounds (area.removeFromTop (28));
    area.removeFromTop (8);
    recentsHeading.setBounds (area.removeFromTop (18));
    area.removeFromTop (4);

    auto bottomBar = area.removeFromBottom (32);
    skipButton.setBounds       (bottomBar.removeFromRight (130));
    bottomBar.removeFromRight (8);
    openFileButton.setBounds   (bottomBar.removeFromRight (90));
    bottomBar.removeFromRight (8);
    newSessionButton.setBounds (bottomBar.removeFromRight (130));

    area.removeFromBottom (12);

    if (recentButtons.isEmpty())
    {
        emptyRecentsLabel.setBounds (area.removeFromTop (22));
    }
    else
    {
        constexpr int rowH = 28;
        for (auto* b : recentButtons)
        {
            b->setBounds (area.removeFromTop (rowH));
            area.removeFromTop (2);
        }
    }
}

void StartupDialog::closeDialog (int returnCode)
{
    // Embedded-modal hosts wire onDismiss to delete the dialog + its dim
    // overlay. juce::DialogWindow hosts (legacy path) reach the dialog via
    // exitModalState; kept here so the dialog still works either way.
    juce::ignoreUnused (returnCode);
    // Move the callback into a local before invoking — host's onDismiss
    // typically deletes `this`, after which touching any member (including
    // returning to a member function frame that re-reads `this`) is UB.
    if (auto cb = std::move (onDismiss))
    {
        cb();
        return;
    }
    if (auto* parent = findParentComponentOfClass<juce::DialogWindow>())
        parent->exitModalState (returnCode);
}
} // namespace duskstudio
