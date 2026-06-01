#include "StartupDialog.h"

#include <juce_audio_formats/juce_audio_formats.h>

#if __has_include("BinaryData.h")
 #include "BinaryData.h"
 #define DUSKSTUDIO_HAS_BINARY_ICON 1
#else
 #define DUSKSTUDIO_HAS_BINARY_ICON 0
#endif

namespace duskstudio
{
namespace
{
const juce::Colour kBg        { 0xff202024 };
const juce::Colour kSidebarBg { 0xff181820 };
const juce::Colour kBorder    { 0xff32323a };
const juce::Colour kSelection { 0xff305a82 };
const juce::Colour kTextHi    { 0xffe8e8e8 };
const juce::Colour kTextMid   { 0xffb0b0b8 };
const juce::Colour kTextLo    { 0xff707078 };
const juce::Colour kAccent    { 0xff80b0ff };

void styleSidebarTab (juce::TextButton& b, bool active)
{
    b.setColour (juce::TextButton::buttonColourId,
                  active ? juce::Colour (0xff282830) : kSidebarBg);
    b.setColour (juce::TextButton::textColourOffId,
                  active ? kAccent : kTextMid);
    b.setColour (juce::TextButton::textColourOnId, kAccent);
}

void styleFooterButton (juce::TextButton& b, bool primary)
{
    b.setColour (juce::TextButton::buttonColourId,
                  primary ? juce::Colour (0xff305a82) : juce::Colour (0xff2a2a30));
    b.setColour (juce::TextButton::textColourOffId, kTextHi);
}

juce::String formatSampleRate (double sr)
{
    if (sr <= 0.0) return {};
    // Whole-kHz when round (48000 -> "48 kHz"); else one decimal
    // (44100 -> "44.1 kHz").
    const double khz = sr / 1000.0;
    if (std::abs (khz - std::round (khz)) < 0.05)
        return juce::String ((int) std::round (khz)) + " kHz";
    return juce::String (khz, 1) + " kHz";
}

juce::String formatBitDepth (int bits, bool isFloat)
{
    if (bits <= 0) return {};
    return juce::String (bits) + "-bit" + (isFloat ? " float" : juce::String());
}

juce::String formatLastModified (juce::Time t)
{
    if (t.toMilliseconds() == 0) return {};
    // Match Ardour: "YYYY-MM-DD HH:MM".
    return t.formatted ("%Y-%m-%d %H:%M");
}

// Pull sample rate + bit depth from the first audio file in <dir>/audio/.
// Cheap header read, no full file scan. Empty strings if we can't tell —
// session.json doesn't store sample-rate at the session level so this is
// the best we can do without an in-progress engine.
void inferAudioFormat (const juce::File& sessionDir,
                        juce::String& sampleRateOut,
                        juce::String& bitDepthOut)
{
    sampleRateOut = {};
    bitDepthOut   = {};
    const auto audioDir = sessionDir.getChildFile ("audio");
    if (! audioDir.isDirectory()) return;

    juce::Array<juce::File> files;
    audioDir.findChildFiles (files, juce::File::findFiles, false, "*.wav");
    if (files.isEmpty())
        audioDir.findChildFiles (files, juce::File::findFiles, false, "*.flac;*.aiff;*.aif");
    if (files.isEmpty()) return;

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader
        (fm.createReaderFor (files.getReference (0)));
    if (reader == nullptr) return;

    sampleRateOut = formatSampleRate (reader->sampleRate);
    bitDepthOut   = formatBitDepth ((int) reader->bitsPerSample,
                                       reader->usesFloatingPointData);
}
} // namespace

StartupDialog::StartupDialog (juce::Array<juce::File> r)
{
    rows.reserve ((size_t) r.size());
    for (auto& dir : r)
    {
        RecentRow row;
        row.dir          = dir;
        row.name         = dir.getFileName();
        row.lastModified = formatLastModified (dir.getLastModificationTime());
        inferAudioFormat (dir, row.sampleRate, row.bitDepth);
        rows.push_back (std::move (row));
    }

   #if DUSKSTUDIO_HAS_BINARY_ICON
    const auto img = juce::ImageCache::getFromMemory (
        BinaryData::dsicon_png, BinaryData::dsicon_pngSize);
    if (img.isValid())
    {
        // centred + fillDestination — source PNG is square so aspect
        // ratio is preserved; without fillDestination the icon was
        // shrinking to leave large empty margins inside its slot.
        brandIcon.setImage (img, juce::RectanglePlacement::centred);
    }
   #endif
    addAndMakeVisible (brandIcon);

    brandWordmark.setText ("DUSK STUDIO", juce::dontSendNotification);
    brandWordmark.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
    brandWordmark.setColour (juce::Label::textColourId, kTextMid);
    brandWordmark.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (brandWordmark);

    tableHeading.setText ("Recent Sessions", juce::dontSendNotification);
    tableHeading.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
    tableHeading.setColour (juce::Label::textColourId, kTextHi);
    tableHeading.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (tableHeading);

    emptyLabel.setText ("No recent sessions yet.", juce::dontSendNotification);
    emptyLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
    emptyLabel.setColour (juce::Label::textColourId, kTextLo);
    emptyLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (emptyLabel);
    emptyLabel.setVisible (rows.empty());

    styleSidebarTab (recentTab, true);
    styleSidebarTab (openTab,   false);
    styleSidebarTab (newTab,    false);

    recentTab.setConnectedEdges (juce::Button::ConnectedOnBottom);
    openTab.setConnectedEdges   (juce::Button::ConnectedOnTop | juce::Button::ConnectedOnBottom);
    newTab.setConnectedEdges    (juce::Button::ConnectedOnTop);

    recentTab.onClick = [this]
    {
        // RECENT just focuses the table — no-op if it's already visible.
        table.grabKeyboardFocus();
    };
    // The host callbacks (onOpenFile / onNewSession / onOpenRecent / onQuit)
    // can synchronously dismiss + delete this dialog via the onDismiss path, so
    // guard `this` with a SafePointer and only closeDialog if we're still alive.
    openTab.onClick = [this]
    {
        juce::Component::SafePointer<StartupDialog> safe (this);
        if (onOpenFile) onOpenFile();
        if (safe != nullptr) safe->closeDialog (1);
    };
    newTab.onClick = [this]
    {
        juce::Component::SafePointer<StartupDialog> safe (this);
        if (onNewSession) onNewSession();
        if (safe != nullptr) safe->closeDialog (1);
    };
    addAndMakeVisible (recentTab);
    addAndMakeVisible (openTab);
    addAndMakeVisible (newTab);

    table.setModel (this);
    table.setColour (juce::ListBox::backgroundColourId, kBg);
    table.setRowHeight (24);
    table.setHeader (std::make_unique<juce::TableHeaderComponent>());

    auto& header = table.getHeader();
    using Flags = juce::TableHeaderComponent;
    header.addColumn ("Session Name",   kColName,       240, 120, -1,
                       Flags::defaultFlags & ~Flags::sortable);
    header.addColumn ("Sample Rate",    kColSampleRate, 100, 80,  -1,
                       Flags::defaultFlags & ~Flags::sortable);
    header.addColumn ("File Resolution", kColBitDepth,  120, 80,  -1,
                       Flags::defaultFlags & ~Flags::sortable);
    header.addColumn ("Last Modified",  kColModified,   140, 100, -1,
                       Flags::defaultFlags & ~Flags::sortable);
    header.setStretchToFitActive (true);
    table.setColour (juce::TableHeaderComponent::backgroundColourId, kSidebarBg);
    table.setColour (juce::TableHeaderComponent::textColourId,       kTextMid);
    table.setColour (juce::TableHeaderComponent::outlineColourId,    kBorder);
    addAndMakeVisible (table);

    // Auto-select the newest session so Enter / "Open" works without an
    // extra click — matches Ardour's behaviour.
    if (! rows.empty())
        table.selectRow (0);

    styleFooterButton (quitButton, false);
    styleFooterButton (openButton, true);
    openButton.setEnabled (! rows.empty());

    quitButton.onClick = [this]
    {
        juce::Component::SafePointer<StartupDialog> safe (this);
        if (onQuit) onQuit();
        if (safe != nullptr) safe->closeDialog (0);
    };
    openButton.onClick = [this] { openSelectedRow(); };
    addAndMakeVisible (quitButton);
    addAndMakeVisible (openButton);

    setWantsKeyboardFocus (true);
}

bool StartupDialog::keyPressed (const juce::KeyPress& key)
{
    if (key.isKeyCode (juce::KeyPress::escapeKey))
    {
        if (onSkip) onSkip();
        closeDialog (0);
        return true;
    }
    if (key.isKeyCode (juce::KeyPress::returnKey))
    {
        openSelectedRow();
        return true;
    }
    return false;
}

void StartupDialog::openSelectedRow()
{
    const int row = table.getSelectedRow();
    if (row < 0 || row >= (int) rows.size()) return;
    const auto dir = rows[(size_t) row].dir;
    juce::Component::SafePointer<StartupDialog> safe (this);
    if (onOpenRecent) onOpenRecent (dir);
    if (safe != nullptr) safe->closeDialog (1);
}

int StartupDialog::getNumRows()
{
    return (int) rows.size();
}

void StartupDialog::paintRowBackground (juce::Graphics& g, int /*rowNumber*/,
                                          int width, int height, bool rowIsSelected)
{
    if (rowIsSelected)
        g.fillAll (kSelection);
    else
        g.fillAll (kBg);
    juce::ignoreUnused (width, height);
}

void StartupDialog::paintCell (juce::Graphics& g, int rowNumber, int columnId,
                                  int width, int height, bool rowIsSelected)
{
    if (rowNumber < 0 || rowNumber >= (int) rows.size()) return;
    const auto& row = rows[(size_t) rowNumber];

    juce::String text;
    switch (columnId)
    {
        case kColName:       text = row.name;         break;
        case kColSampleRate: text = row.sampleRate;   break;
        case kColBitDepth:   text = row.bitDepth;     break;
        case kColModified:   text = row.lastModified; break;
        default: break;
    }
    if (text.isEmpty())
        text = juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94"));  // em dash

    g.setColour (rowIsSelected ? kTextHi : kTextMid);
    g.setFont (juce::Font (juce::FontOptions (13.0f)));
    g.drawText (text, 8, 0, width - 16, height,
                  juce::Justification::centredLeft, true);
}

void StartupDialog::cellDoubleClicked (int /*rowNumber*/, int /*columnId*/,
                                          const juce::MouseEvent&)
{
    openSelectedRow();
}

void StartupDialog::selectedRowsChanged (int /*lastRowSelected*/)
{
    openButton.setEnabled (table.getSelectedRow() >= 0);
}

void StartupDialog::paint (juce::Graphics& g)
{
    g.fillAll (kBg);

    // Sidebar background — separate column from the table body.
    const int sidebarW = 100;
    auto sidebar = getLocalBounds().removeFromLeft (sidebarW);
    g.setColour (kSidebarBg);
    g.fillRect (sidebar);
    g.setColour (kBorder);
    g.drawVerticalLine (sidebarW - 1, 0.0f, (float) getHeight());

    // Footer divider.
    constexpr int footerH = 52;
    g.setColour (kBorder);
    g.drawHorizontalLine (getHeight() - footerH,
                             (float) sidebarW, (float) getWidth());

    // Window frame.
    g.setColour (kBorder);
    g.drawRect (getLocalBounds(), 1);
}

void StartupDialog::resized()
{
    auto bounds = getLocalBounds();

    // Sidebar.
    const int sidebarW = 100;
    auto sidebar = bounds.removeFromLeft (sidebarW).reduced (8, 12);
    brandIcon.setBounds     (sidebar.removeFromTop (80));
    sidebar.removeFromTop (4);
    brandWordmark.setBounds (sidebar.removeFromTop (14));
    sidebar.removeFromTop (16);
    recentTab.setBounds (sidebar.removeFromTop (36));
    openTab  .setBounds (sidebar.removeFromTop (36));
    newTab   .setBounds (sidebar.removeFromTop (36));

    // Footer.
    constexpr int footerH = 52;
    auto footer = bounds.removeFromBottom (footerH).reduced (16, 10);
    openButton.setBounds (footer.removeFromRight (90));
    footer.removeFromRight (8);
    quitButton.setBounds (footer.removeFromRight (90));

    // Main panel: heading + table.
    auto main = bounds.reduced (16, 12);
    tableHeading.setBounds (main.removeFromTop (22));
    main.removeFromTop (8);

    if (rows.empty())
    {
        emptyLabel.setBounds (main);
        table.setVisible (false);
    }
    else
    {
        table.setVisible (true);
        table.setBounds (main);
    }
}

void StartupDialog::closeDialog (int returnCode)
{
    // Embedded-modal hosts wire onDismiss to delete the dialog + its dim
    // overlay. juce::DialogWindow hosts (legacy path) reach the dialog via
    // exitModalState; kept here so the dialog still works either way.
    juce::ignoreUnused (returnCode);
    if (auto cb = std::move (onDismiss))
    {
        cb();
        return;
    }
    if (auto* parent = findParentComponentOfClass<juce::DialogWindow>())
        parent->exitModalState (returnCode);
}
} // namespace duskstudio
