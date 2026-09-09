#include "SfzLibraryPanel.h"

#include "../AppConfig.h"
#include "../EmbeddedModal.h"
#include "../DuskFileBrowser.h"
#include "../../foundation/MessageThread.h"

#include <algorithm>

namespace duskstudio
{
namespace
{
std::vector<std::filesystem::path> configuredRoots()
{
    auto roots = sfz::defaultLibraryRoots();
    for (const auto& extra : appconfig::getSfzLibraryRoots())
        roots.emplace_back (extra);
    return roots;
}
} // namespace

// Same self-drawn list as the plugin picker, for the same reason: a Viewport
// inside a nested EmbeddedModal sized its content holder wrongly and left rows
// invisible until the first resize.
class SfzLibraryPanel::ListBody final : public juce::Component
{
public:
    explicit ListBody (std::function<void (const juce::File&)> picker)
        : onPick (std::move (picker))
    {
    }

    void setResult (sfz::ScanResult result)
    {
        scan = std::move (result);
        applyFilter (currentFilter);
    }

    void applyFilter (const juce::String& needle)
    {
        currentFilter = needle;
        rows.clear();

        // Unusable roots head the list rather than being dropped. An empty
        // library and a mistyped root look identical otherwise.
        for (const auto& problem : scan.problems)
            rows.push_back ({ Row::Kind::Problem,
                              juce::String (problem.root.u8string())
                                  + "  -  " + juce::String (problem.reason),
                              {} });

        for (const auto* entry : sfz::filterEntries (scan.entries,
                                                     needle.toStdString()))
        {
            juce::String label (entry->displayName);
            label += "   (" + juce::String (std::string (sfz::formatName (entry->format))) + ")";
            if (! entry->folder.empty())
                label += "   " + juce::String (entry->folder);
            rows.push_back ({ Row::Kind::Instrument, label,
                              juce::File (juce::String (entry->path.u8string())) });
        }

        clampScroll();
        repaint();
    }

    int instrumentCount() const noexcept
    {
        return (int) std::count_if (rows.begin(), rows.end(),
                                    [] (const Row& r)
                                    { return r.kind == Row::Kind::Instrument; });
    }

    int getContentHeight() const noexcept { return 6 + (int) rows.size() * kRowH + 6; }

    void resized() override { clampScroll(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff141418));

        const int w = getWidth();
        const int viewH = getHeight();
        const auto mouse = getMouseXYRelative();
        const bool mouseInside = getLocalBounds().contains (mouse);

        int y = 6 - scrollOffset;
        for (const auto& row : rows)
        {
            if (y + kRowH > 0 && y < viewH)
            {
                juce::Rectangle<int> bounds (0, y, w - kScrollbarW, kRowH);
                if (row.kind == Row::Kind::Problem)
                {
                    g.setColour (juce::Colour (0xff3a2020));
                    g.fillRect (bounds.reduced (4, 2));
                    g.setColour (juce::Colour (0xffd08080));
                }
                else
                {
                    if (mouseInside && bounds.contains (mouse))
                    {
                        g.setColour (juce::Colour (0xff2a2a36));
                        g.fillRect (bounds);
                    }
                    g.setColour (juce::Colour (0xffdddde0));
                }
                g.setFont (juce::Font (juce::FontOptions (12.5f)));
                g.drawText (row.text, bounds.reduced (16, 0),
                            juce::Justification::centredLeft, false);
            }
            y += kRowH;
        }

        if (rows.empty())
        {
            g.setColour (juce::Colour (0xff70707a));
            g.setFont (juce::Font (juce::FontOptions (12.0f)));
            g.drawText (emptyMessage, getLocalBounds().reduced (12),
                        juce::Justification::centred, false);
        }

        drawScrollbar (g);
    }

    void setEmptyMessage (juce::String text) { emptyMessage = std::move (text); repaint(); }

    void mouseMove (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

    void mouseDown (const juce::MouseEvent& ev) override
    {
        const int index = rowForY (ev.y);
        if (index < 0 || index >= (int) rows.size()) return;
        const auto& row = rows[(size_t) index];
        if (row.kind != Row::Kind::Instrument) return;
        if (onPick) onPick (row.file);
    }

    void mouseWheelMove (const juce::MouseEvent&,
                         const juce::MouseWheelDetails& wheel) override
    {
        scrollOffset += (int) (-wheel.deltaY * 80.0f);
        clampScroll();
        repaint();
    }

private:
    struct Row
    {
        enum class Kind { Instrument, Problem };
        Kind kind;
        juce::String text;
        juce::File file;
    };

    static constexpr int kRowH       = 22;
    static constexpr int kScrollbarW = 8;

    int rowForY (int y) const noexcept
    {
        const int index = (y - 6 + scrollOffset) / kRowH;
        return (y - 6 + scrollOffset) < 0 ? -1 : index;
    }

    void clampScroll() noexcept
    {
        scrollOffset = std::clamp (scrollOffset, 0,
                                   std::max (0, getContentHeight() - getHeight()));
    }

    void drawScrollbar (juce::Graphics& g)
    {
        const int contentH = getContentHeight();
        const int viewH = getHeight();
        if (contentH <= viewH) return;

        const int thumbH = std::max (20, (int) (viewH * ((float) viewH / (float) contentH)));
        const int maxOff = contentH - viewH;
        const float prog = maxOff > 0 ? (float) scrollOffset / (float) maxOff : 0.0f;
        const int x = getWidth() - kScrollbarW;

        g.setColour (juce::Colour (0xff202028));
        g.fillRect (x, 0, kScrollbarW, viewH);
        g.setColour (juce::Colour (0xff5a5a68));
        g.fillRect (x + 1, (int) ((viewH - thumbH) * prog), kScrollbarW - 2, thumbH);
    }

    std::function<void (const juce::File&)> onPick;
    sfz::ScanResult scan;
    juce::String currentFilter;
    juce::String emptyMessage { "Scanning..." };
    std::vector<Row> rows;
    int scrollOffset = 0;
};

SfzLibraryPanel::SfzLibraryPanel (Callbacks cb,
                                  std::vector<std::filesystem::path> roots)
    : callbacks (std::move (cb)), rootsOverride (std::move (roots))
{
    setWantsKeyboardFocus (true);

    titleLabel.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe0e0e4));
    addAndMakeVisible (titleLabel);

    statusLabel.setFont (juce::Font (juce::FontOptions (11.5f)));
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9090a0));
    addAndMakeVisible (statusLabel);

    filterEditor.setTextToShowWhenEmpty ("Filter by name or folder",
                                         juce::Colour (0xff70707a));
    filterEditor.onTextChange = [this]
    {
        if (listBody != nullptr) listBody->applyFilter (filterEditor.getText());
        timerCallback();
    };
    addAndMakeVisible (filterEditor);

    listBody = std::make_unique<ListBody> ([this] (const juce::File& file)
    {
        if (callbacks.onPick) callbacks.onPick (file);
    });
    addAndMakeVisible (*listBody);

    rescanBtn.onClick  = [this] { startScan(); };
    addRootBtn.onClick = [this] { addRootFolder(); };
    browseBtn.onClick  = [this] { if (callbacks.onBrowseFile) callbacks.onBrowseFile(); };
    closeBtn.onClick   = [this] { if (callbacks.onCancel) callbacks.onCancel(); };
    for (auto* b : { &rescanBtn, &addRootBtn, &browseBtn, &closeBtn })
        addAndMakeVisible (*b);

    startScan();
    startTimerHz (10);
}

SfzLibraryPanel::~SfzLibraryPanel()
{
    stopTimer();
    stopScan();
}

void SfzLibraryPanel::stopScan() noexcept
{
    cancelScan.store (true, std::memory_order_release);
    if (scanThread.joinable()) scanThread.join();
    cancelScan.store (false, std::memory_order_release);
}

void SfzLibraryPanel::startScan()
{
    stopScan();

    auto roots = rootsOverride.empty() ? configuredRoots() : rootsOverride;
    rootsDone.store (0, std::memory_order_relaxed);
    rootsTotal.store ((int) roots.size(), std::memory_order_relaxed);
    scanDone.store (false, std::memory_order_release);
    pendingResult = std::make_shared<sfz::ScanResult>();
    if (listBody != nullptr) listBody->setEmptyMessage ("Scanning...");

    auto result = pendingResult;
    scanThread = std::thread ([this, roots = std::move (roots), result]
    {
        *result = sfz::scanLibraryRoots (roots, &cancelScan,
                                         [this] (std::size_t done, std::size_t)
                                         {
                                             rootsDone.store ((int) done,
                                                              std::memory_order_relaxed);
                                         });
        scanDone.store (true, std::memory_order_release);
    });
}

bool SfzLibraryPanel::applyFinishedScan()
{
    if (! scanDone.load (std::memory_order_acquire) || pendingResult == nullptr)
        return false;
    {
        auto result = std::move (*pendingResult);
        pendingResult.reset();
        if (scanThread.joinable()) scanThread.join();

        const bool foundAnything = ! result.entries.empty();
        if (listBody != nullptr)
        {
            listBody->setEmptyMessage (foundAnything
                                         ? "No instrument matches the filter."
                                         : "No .sfz or .sf2 found. Add a folder to look in.");
            listBody->setResult (std::move (result));
            listBody->applyFilter (filterEditor.getText());
        }
    }
    updateStatus();
    return true;
}

void SfzLibraryPanel::timerCallback()
{
    (void) applyFinishedScan();
    updateStatus();
}

void SfzLibraryPanel::updateStatus()
{
    juce::String status;
    if (pendingResult != nullptr)
        status = "Scanning " + juce::String (rootsDone.load (std::memory_order_relaxed))
                   + " / " + juce::String (rootsTotal.load (std::memory_order_relaxed))
                   + " folders";
    else if (listBody != nullptr)
        status = juce::String (listBody->instrumentCount()) + " instruments";

    if (status != statusLabel.getText())
        statusLabel.setText (status, juce::dontSendNotification);
}

void SfzLibraryPanel::addRootFolder()
{
    juce::Component::SafePointer<SfzLibraryPanel> safe (this);
    filebrowser::open (*this, {
        /*title*/                  "Add a folder to the instrument library",
        /*initialFileOrDirectory*/ juce::File(),
        /*filePatternsAllowed*/    {},
        /*mode*/                   filebrowser::Mode::Open,
        /*warnAboutOverwriting*/   false,
        /*selectDirectories*/      true,
    },
    [safe] (juce::File folder)
    {
        auto* self = safe.getComponent();
        if (self == nullptr || ! folder.isDirectory()) return;

        auto roots = appconfig::getSfzLibraryRoots();
        const auto added = folder.getFullPathName().toStdString();
        if (std::find (roots.begin(), roots.end(), added) == roots.end())
        {
            roots.push_back (added);
            appconfig::setSfzLibraryRoots (roots);
        }
        self->startScan();
    });
}

void SfzLibraryPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff181820));
    g.setColour (juce::Colour (0xff2a2a2e));
    g.drawRect (getLocalBounds(), 1);
}

void SfzLibraryPanel::resized()
{
    auto r = getLocalBounds().reduced (kPad);
    titleLabel.setBounds (r.removeFromTop (kTitleH));
    statusLabel.setBounds (r.removeFromTop (kStatusH));
    filterEditor.setBounds (r.removeFromTop (kFilterH).reduced (0, 2));
    r.removeFromTop (6);

    auto actions = r.removeFromBottom (kActionH);
    r.removeFromBottom (6);
    const int btnW = std::max (80, actions.getWidth() / 4 - 6);
    rescanBtn  .setBounds (actions.removeFromLeft (btnW).reduced (2));
    addRootBtn .setBounds (actions.removeFromLeft (btnW).reduced (2));
    closeBtn   .setBounds (actions.removeFromRight (btnW).reduced (2));
    browseBtn  .setBounds (actions.removeFromRight (btnW + 30).reduced (2));

    if (listBody != nullptr) listBody->setBounds (r);
}

bool SfzLibraryPanel::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        if (callbacks.onCancel) callbacks.onCancel();
        return true;
    }
    return false;
}

namespace sfzlibrary
{
namespace
{
// Its own modal, not the picker's: the library can be opened from an editor
// that is itself inside a modal, and sharing one instance would close the host.
EmbeddedModal& sharedLibraryModal()
{
    static EmbeddedModal m;
    return m;
}
} // namespace

void show (juce::Component& host,
           std::function<void (const juce::File&)> onPick,
           std::function<void()> onBrowseFile)
{
    auto* parent = host.getTopLevelComponent();
    if (parent == nullptr) return;

    SfzLibraryPanel::Callbacks cb;
    cb.onCancel = [] { sharedLibraryModal().close(); };
    cb.onPick = [onPick = std::move (onPick)] (const juce::File& file)
    {
        sharedLibraryModal().close();
        if (onPick) onPick (file);
    };
    cb.onBrowseFile = [onBrowseFile = std::move (onBrowseFile)]
    {
        sharedLibraryModal().close();
        if (onBrowseFile) onBrowseFile();
    };

    auto panel = std::make_unique<SfzLibraryPanel> (std::move (cb));
    panel->setSize (620, 460);
    sharedLibraryModal().show (*parent, std::move (panel),
                               /*onDismiss*/ [] { sharedLibraryModal().close(); });
}
} // namespace sfzlibrary
} // namespace duskstudio
