#include "DuskFileBrowser.h"
#include "DuskAlerts.h"
#include "EmbeddedModal.h"
#include "../foundation/Fs.h"
#include "../foundation/MessageThread.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace duskstudio::filebrowser
{
namespace
{
EmbeddedModal& sharedFileBrowserModal()
{
    static EmbeddedModal m;
    return m;
}

bool shuttingDown = false;

struct FolderCheckGate
{
    std::mutex mutex;
    std::condition_variable released;
    bool held = false;
};

// Never destroyed: a folder check still running on its own thread at exit would
// otherwise lock a destroyed mutex, which aborts the process on macOS.
FolderCheckGate& folderCheckGate()
{
    static auto* gate = new FolderCheckGate();
    return *gate;
}

// Opens the nearest folder that exists at or above `folder`, the one the browser
// would go on to list.
void openNearestFolder (std::filesystem::path folder)
{
    {
        static constexpr auto kLongestHold = std::chrono::seconds (3);
        auto& gate = folderCheckGate();
        std::unique_lock<std::mutex> lock (gate.mutex);
        gate.released.wait_for (lock, kLongestHold, [&gate] { return ! gate.held; });
    }
    std::error_code error;
    while (! std::filesystem::is_directory (folder, error))
    {
        auto parent = folder.parent_path();
        if (parent == folder) return;
        folder = std::move (parent);
    }
    const std::filesystem::directory_iterator firstEntry (folder, error);
}

class ScanBrowser final : public juce::FileBrowserComponent
{
public:
    ScanBrowser (int browserFlags, const juce::File& initial, const juce::FileFilter* filter)
        : FileBrowserComponent (browserFlags, initial, filter, nullptr)
    {
        for (auto* child : getChildren())
        {
            if (auto* pathBox = dynamic_cast<juce::ComboBox*> (child))
                pathBox->onChange = [this, pathBox, change = pathBox->onChange]
                {
                    // The folder the framework's handler moves to: a place picked
                    // from the list, or the nearest folder at or above the typed path.
                    auto folder = getRoot();
                    const auto typed = pathBox->getText().trim().unquoted();
                    if (typed.isNotEmpty())
                    {
                        juce::StringArray names, paths;
                        getRoots (names, paths);
                        const auto place = paths[pathBox->getSelectedId() - 1];
                        folder = folder.getChildFile (place.isNotEmpty() ? place : typed);
                    }
                    afterOpening (folder, change);
                };
            else if (auto* nameBox = dynamic_cast<juce::TextEditor*> (child))
                nameBox->onReturnKey = [this, nameBox, change = nameBox->onReturnKey]
                {
                    const auto name = nameBox->getText();
                    if (name.containsChar (getRoot().getSeparatorChar()))
                        afterOpening (getRoot().getChildFile (name), change);
                    else
                        change();
                };
        }
        routeGoUp();
    }

    // Private to this browser: the list and any folders it opens scan on it.
    auto& scanThread() { return getDisplayComponent()->directoryContentsList.getTimeSliceThread(); }
    bool scanning() const { return getDisplayComponent()->directoryContentsList.isStillLoading(); }

    // macOS asks the user before an app may list a protected folder such as
    // Desktop, and holds the listing until they answer. The framework lists a
    // new folder on the message thread, so the folder is opened on a thread of
    // its own first and the change runs once macOS has its answer.
    void afterOpening (const juce::File& folder, const std::function<void()>& change)
    {
        pendingUpLevels = 0;
        const int request = ++*latestChange;
        auto path = std::filesystem::u8path (folder.getFullPathName().toStdString());
        try
        {
            std::thread ([path, change, latest = std::weak_ptr<int> (latestChange), request]
            {
                openNearestFolder (path);
                dusk::callAsync ([change, latest, request]
                {
                    if (const auto current = latest.lock(); current != nullptr && *current == request)
                        change();
                });
            }).detach();
        }
        catch (const std::system_error&)
        {
            change();
        }
    }

    void dropFolderChange()
    {
        pendingUpLevels = 0;
        ++*latestChange;
    }

    void fileDoubleClicked (const juce::File& f) override
    {
        if (! f.isDirectory())
        {
            FileBrowserComponent::fileDoubleClicked (f);
            return;
        }
        afterOpening (f, [this, f] { FileBrowserComponent::fileDoubleClicked (f); });
    }

    void lookAndFeelChanged() override
    {
        FileBrowserComponent::lookAndFeelChanged();
        routeGoUp();
    }

private:
    void routeGoUp()
    {
        for (auto* child : getChildren())
            if (auto* up = dynamic_cast<juce::Button*> (child))
                up->onClick = [this]
                {
                    // An Up clicked while the last one still waits on its check
                    // goes on from the folder that one is going to.
                    const int levels = pendingUpLevels + 1;
                    auto target = getRoot();
                    for (int level = 0; level < levels; ++level)
                        target = target.getParentDirectory();
                    afterOpening (target, [this, target]
                    {
                        pendingUpLevels = 0;
                        setRoot (target);
                    });
                    pendingUpLevels = levels;
                };
    }

    int pendingUpLevels = 0;

    std::shared_ptr<int> latestChange = std::make_shared<int> (0);

    // The framework's browser relists its folder whenever the app comes back to
    // the front, and a relist waits for the scan in progress. A scan macOS holds
    // on a privacy prompt would then freeze the window until the prompt is
    // answered, so a folder still being listed is left to finish.
    void timerCallback() override
    {
        const bool active = juce::Process::isForegroundProcess();
        if (active == wasActive) return;
        wasActive = active;
        if (active && ! scanning()) refresh();
    }

    bool wasActive = true;
};

class DuskFileBrowserPanel final : public juce::Component,
                                       private juce::FileBrowserListener
{
public:
    DuskFileBrowserPanel (Options o,
                            std::function<void (juce::File)> onResult,
                            std::function<void (juce::Array<juce::File>)> onMultiResult)
        : opts (std::move (o)),
          resultFn (std::move (onResult)),
          multiResultFn (std::move (onMultiResult))
    {
        const bool multi = (multiResultFn != nullptr);
        setOpaque (true);
        setWantsKeyboardFocus (true);

        // Build the FileBrowserComponent. Flag bitmask follows juce's
        // contract: openMode/saveMode + canSelectFiles or canSelectDirectories.
        int browserFlags = (opts.mode == Mode::Save)
                        ? (int) ScanBrowser::saveMode
                        : (int) ScanBrowser::openMode;
        browserFlags |= opts.selectDirectories
                    ? (int) ScanBrowser::canSelectDirectories
                    : (int) ScanBrowser::canSelectFiles;
        if (multi)
            browserFlags |= (int) ScanBrowser::canSelectMultipleItems;

        // Filter: simple wildcard filter. Empty pattern = any file.
        if (opts.filePatternsAllowed.isNotEmpty())
            filter = std::make_unique<juce::WildcardFileFilter> (
                        opts.filePatternsAllowed, juce::String(),
                        opts.filePatternsAllowed);

        // From $HOME (USERPROFILE on Windows), which a sandboxed run points at
        // its own folder. JUCE asks the OS for the home folder and would start
        // in the real one.
        const juce::File home (dusk::fs::userHomeDir().u8string());
        auto initial = opts.initialFileOrDirectory.getFullPathName().isNotEmpty()
                           ? opts.initialFileOrDirectory
                           : home;
        // The browser takes a start path that isn't there as a file already
        // chosen, and keeps it after the user moves to another folder. For
        // Open, start in the nearest folder that exists instead.
        if (opts.mode == Mode::Open)
        {
            while (! initial.exists() && initial.getParentDirectory() != initial)
                initial = initial.getParentDirectory();
            if (! initial.exists())
                initial = home;
        }
        browser = std::make_unique<ScanBrowser> (browserFlags, initial, filter.get());
        browser->addListener (this);
        addAndMakeVisible (*browser);

        // FileBrowserComponent doesn't expose its filename label; retitle it
        // for save dialogs where "file:" reads wrong next to a name field.
        if (opts.mode == Mode::Save)
            for (auto* child : browser->getChildren())
                if (auto* l = dynamic_cast<juce::Label*> (child))
                    if (l->getText() == TRANS ("file:"))
                        l->setText (TRANS ("Name:"), juce::dontSendNotification);

        titleLabel.setText (opts.title.isNotEmpty() ? opts.title
                                                       : (opts.mode == Mode::Save ? "Save file"
                                                                                   : "Open file"),
                              juce::dontSendNotification);
        setTitle (titleLabel.getText());
        titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
        titleLabel.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
        addAndMakeVisible (titleLabel);

        auto styleBtn = [] (juce::TextButton& b, juce::Colour fill)
        {
            b.setColour (juce::TextButton::buttonColourId,  fill);
            b.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            b.setMouseClickGrabsKeyboardFocus (false);
        };
        styleBtn (okBtn,     juce::Colour (0xff385a78));
        styleBtn (cancelBtn, juce::Colour (0xff262630));
        okBtn.setButtonText (opts.mode == Mode::Save ? "Save" : "Open");
        cancelBtn.setButtonText ("Cancel");
        okBtn.onClick     = [this] { commit(); };
        cancelBtn.onClick = [this] { dismissCancelled(); };
        addAndMakeVisible (okBtn);
        addAndMakeVisible (cancelBtn);

        // New-folder affordance for flows that write somewhere: Save targets
        // and directory pickers. Plain Open stays uncluttered.
        if (opts.mode == Mode::Save || opts.selectDirectories)
        {
            styleBtn (newFolderBtn, juce::Colour (0xff262630));
            newFolderBtn.onClick = [this] { toggleNewFolderRow(); };
            addAndMakeVisible (newFolderBtn);

            newFolderName.setSelectAllWhenFocused (true);
            newFolderName.onReturnKey = [this] { createNewFolder(); };
            newFolderName.onEscapeKey = [this] { toggleNewFolderRow(); };
            newFolderName.onTextChange = [this]
            {
                // Drop the error override so the LookAndFeel default applies
                // again (a copied findColour value would freeze the theme).
                newFolderName.removeColour (juce::TextEditor::outlineColourId);
                newFolderName.repaint();
            };
            addChildComponent (newFolderName);

            styleBtn (createFolderBtn, juce::Colour (0xff385a78));
            createFolderBtn.onClick = [this] { createNewFolder(); };
            addChildComponent (createFolderBtn);
        }

        // Comfortable browse size - clamped to parent by EmbeddedModal.
        setSize (820, 560);
    }

    ~DuskFileBrowserPanel() override
    {
        if (browser == nullptr) return;
        browser->removeListener (this);
        browser->dropFolderChange();
        // A folder scan that macOS holds on a privacy prompt keeps the list locked
        // until the prompt is answered, and destroying the browser waits for that
        // lock with the window frozen half repainted. So a browser still scanning
        // is told to stop and kept off screen, with the filter its scan reads,
        // until its scan thread has finished.
        if (! browser->scanning())
            return;
        removeChildComponent (browser.get());
        auto& scan = browser->scanThread();
        scan.signalThreadShouldExit();
        scan.notify();
        auto& retired = retiredScans();
        retired.push_back (std::make_unique<Retired> (Retired { std::move (filter), std::move (browser) }));
        if (retired.size() == 1 && ! shuttingDown)
            dusk::Timer::callAfterDelay (kRetiredPollMs, sweepRetired);
    }

    static int retiredCount() { return (int) retiredScans().size(); }

    // No timers once the app is going down. A scan that stops in time is
    // destroyed; one the OS still holds is left to the process exit.
    static void letGoOfRetiredScans()
    {
        static constexpr int kShutdownWaitMs = 250;
        for (auto& retired : retiredScans())
            if (! retired->scanningBrowser->scanThread().waitForThreadToExit (kShutdownWaitMs))
                (void) retired.release();
        retiredScans().clear();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1a1a22));
        g.setColour (juce::Colour (0xff3a3a44));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 6.0f, 1.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (14);
        titleLabel.setBounds (area.removeFromTop (24));
        area.removeFromTop (8);

        auto bottom = area.removeFromBottom (36);
        cancelBtn.setBounds (bottom.removeFromRight (100));
        bottom.removeFromRight (8);
        okBtn    .setBounds (bottom.removeFromRight (100));
        if (newFolderBtn.isVisible())
            newFolderBtn.setBounds (bottom.removeFromLeft (110));
        area.removeFromBottom (10);

        if (newFolderName.isVisible())
        {
            auto row = area.removeFromBottom (28);
            createFolderBtn.setBounds (row.removeFromRight (80));
            row.removeFromRight (8);
            newFolderName.setBounds (row);
            area.removeFromBottom (8);
        }

        if (browser != nullptr) browser->setBounds (area);
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey)  { dismissCancelled(); return true; }
        if (k == juce::KeyPress::returnKey)  { commit();           return true; }
        return false;
    }

    bool shownFolderScanning() const { return browser != nullptr && browser->scanning(); }

    std::filesystem::path shownFolder() const
    {
        return browser != nullptr ? std::filesystem::u8path (browser->getRoot().getFullPathName().toStdString())
                                  : std::filesystem::path {};
    }

    void dismissCancelled()
    {
        auto single = resultFn;
        auto multi  = multiResultFn;
        sharedFileBrowserModal().close();
        if (single) single ({});
        if (multi)  multi  ({});
    }

private:
    // FileBrowserListener - double-click on a file triggers Open. Single
    // selection writes the path into the active filename for Save mode.
    void selectionChanged() override {}
    void fileClicked (const juce::File&, const juce::MouseEvent&) override {}
    void fileDoubleClicked (const juce::File& f) override
    {
        if (f.isDirectory()) return;   // browser navigates into it
        chosen = f;
        commit();
    }
    void browserRootChanged (const juce::File&) override {}

    void commit()
    {
        if (browser == nullptr) { dismissCancelled(); return; }

        if (multiResultFn)
        {
            juce::Array<juce::File> files;
            for (int i = 0; i < browser->getNumSelectedFiles(); ++i)
            {
                const auto f = browser->getSelectedFile (i);
                if (accepts (f)) files.add (f);
            }
            if (files.isEmpty()) { dismissCancelled(); return; }
            auto cb = multiResultFn;
            sharedFileBrowserModal().close();
            if (cb) cb (files);
            return;
        }

        const auto file = chosen.exists() ? chosen : browser->getSelectedFile (0);
        if (! accepts (file))
        {
            // The browser reports its own folder for an empty name box, so
            // this also covers Save with nothing typed.
            if (opts.mode == Mode::Save && file.isDirectory())
            {
                browser->afterOpening (file, [shown = browser.get(), file]
                {
                    shown->setRoot (file);
                    shown->setFileName ({});
                });
            }
            else
            {
                dismissCancelled();
            }
            return;
        }

        auto cb = resultFn;
        sharedFileBrowserModal().close();
        if (cb) cb (file);
    }

    bool accepts (const juce::File& f) const
    {
        return isAcceptableChoice (f.getFullPathName().toStdString(), opts.mode,
                                   opts.selectDirectories, opts.saveAnswerIsFolder);
    }

    void toggleNewFolderRow()
    {
        const bool show = ! newFolderName.isVisible();
        newFolderName.setVisible (show);
        createFolderBtn.setVisible (show);
        resized();
        if (show)
        {
            newFolderName.setText ("New folder", juce::dontSendNotification);
            newFolderName.grabKeyboardFocus();
        }
    }

    void createNewFolder()
    {
        if (browser == nullptr) return;
        const auto name = juce::File::createLegalFileName (newFolderName.getText().trim());
        if (name.isEmpty())
        {
            // Same feedback as a failed create - a silent return reads as a
            // dead Create button when the name was all whitespace.
            newFolderName.setColour (juce::TextEditor::outlineColourId,
                                      juce::Colour (0xffcc4444));
            newFolderName.repaint();
            return;
        }

        const auto dir = browser->getRoot().getChildFile (name);
        if (! dir.isDirectory() && ! dir.createDirectory())
        {
            newFolderName.setColour (juce::TextEditor::outlineColourId,
                                      juce::Colour (0xffcc4444));
            newFolderName.repaint();
            return;
        }
        toggleNewFolderRow();
        browser->dropFolderChange();
        browser->setRoot (dir);
    }

    Options opts;
    std::function<void (juce::File)> resultFn;
    std::function<void (juce::Array<juce::File>)> multiResultFn;
    std::unique_ptr<juce::WildcardFileFilter> filter;
    std::unique_ptr<ScanBrowser> browser;
    juce::Label titleLabel;
    juce::TextButton okBtn, cancelBtn;
    juce::TextButton newFolderBtn { "New folder..." }, createFolderBtn { "Create" };
    juce::TextEditor newFolderName;
    juce::File chosen;

    // Members are destroyed in reverse, so the browser goes before the filter it reads.
    struct Retired
    {
        decltype (filter)  scanFilter;
        decltype (browser) scanningBrowser;
    };

    static constexpr int kRetiredPollMs = 250;

    // Never freed: a browser still held here at exit is one whose scan could
    // not be stopped, and destroying it would wait on that scan.
    static std::vector<std::unique_ptr<Retired>>& retiredScans()
    {
        static auto* scans = new std::vector<std::unique_ptr<Retired>>();
        return *scans;
    }

    static void sweepRetired()
    {
        auto& retired = retiredScans();
        retired.erase (std::remove_if (retired.begin(), retired.end(),
                                       [] (const auto& r) { return ! r->scanningBrowser->scanThread().isThreadRunning(); }),
                       retired.end());
        if (! retired.empty() && ! shuttingDown)
            dusk::Timer::callAfterDelay (kRetiredPollMs, sweepRetired);
    }
};
} // namespace

void closeForShutdown()
{
    shuttingDown = true;
    sharedFileBrowserModal().closeAndDeleteBodyNow();
    DuskFileBrowserPanel::letGoOfRetiredScans();
}

bool shownFolderScanningForScenario()
{
    const auto& stack = EmbeddedModal::activeModalStack();
    const auto* panel = stack.empty() ? nullptr
                                      : dynamic_cast<const DuskFileBrowserPanel*> (stack.back()->getBody());
    return panel != nullptr && panel->shownFolderScanning();
}

int retiredScansForScenario()
{
    return DuskFileBrowserPanel::retiredCount();
}

void holdFolderChecksForScenario (bool held)
{
    auto& gate = folderCheckGate();
    {
        const std::lock_guard<std::mutex> lock (gate.mutex);
        gate.held = held;
    }
    gate.released.notify_all();
}

std::filesystem::path shownFolderForScenario()
{
    const auto& stack = EmbeddedModal::activeModalStack();
    const auto* panel = stack.empty() ? nullptr
                                      : dynamic_cast<const DuskFileBrowserPanel*> (stack.back()->getBody());
    return panel != nullptr ? panel->shownFolder() : std::filesystem::path {};
}

void open (juce::Component& host, Options opts,
            std::function<void (juce::File)> onResult)
{
    auto* parent = host.getTopLevelComponent();
    if (parent == nullptr) parent = &host;

    auto panel = std::make_unique<DuskFileBrowserPanel> (
        std::move (opts), std::move (onResult), nullptr);

    auto* const shown = panel.get();
    sharedFileBrowserModal().show (*parent, std::move (panel),
        [shown] { shown->dismissCancelled(); });
}

void openMulti (juce::Component& host, Options opts,
                  std::function<void (juce::Array<juce::File>)> onResult)
{
    auto* parent = host.getTopLevelComponent();
    if (parent == nullptr) parent = &host;

    auto panel = std::make_unique<DuskFileBrowserPanel> (
        std::move (opts), nullptr, std::move (onResult));

    auto* const shown = panel.get();
    sharedFileBrowserModal().show (*parent, std::move (panel),
        [shown] { shown->dismissCancelled(); });
}
} // namespace duskstudio::filebrowser
