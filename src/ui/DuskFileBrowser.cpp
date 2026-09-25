#include "DuskFileBrowser.h"
#include "DuskAlerts.h"
#include "EmbeddedModal.h"

namespace duskstudio::filebrowser
{
namespace
{
EmbeddedModal& sharedFileBrowserModal()
{
    static EmbeddedModal m;
    return m;
}

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
                        ? (int) juce::FileBrowserComponent::saveMode
                        : (int) juce::FileBrowserComponent::openMode;
        browserFlags |= opts.selectDirectories
                    ? (int) juce::FileBrowserComponent::canSelectDirectories
                    : (int) juce::FileBrowserComponent::canSelectFiles;
        if (opts.mode == Mode::Save && opts.warnAboutOverwriting)
            browserFlags |= (int) juce::FileBrowserComponent::warnAboutOverwriting;
        if (multi)
            browserFlags |= (int) juce::FileBrowserComponent::canSelectMultipleItems;

        // Filter: simple wildcard filter. Empty pattern = any file.
        if (opts.filePatternsAllowed.isNotEmpty())
            filter = std::make_unique<juce::WildcardFileFilter> (
                        opts.filePatternsAllowed, juce::String(),
                        opts.filePatternsAllowed);

        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
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
        browser = std::make_unique<juce::FileBrowserComponent> (
            browserFlags, initial, filter.get(), /*previewComp*/ nullptr);
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
        if (browser != nullptr) browser->removeListener (this);
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

    void dismissCancelled()
    {
        auto single = resultFn;
        auto multi  = multiResultFn;
        sharedFileBrowserModal().close();
        if (single) single (juce::File());
        if (multi)  multi  (juce::Array<juce::File>());
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
                browser->setRoot (file);
                browser->setFileName ({});
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
        browser->setRoot (dir);
    }

    Options opts;
    std::function<void (juce::File)> resultFn;
    std::function<void (juce::Array<juce::File>)> multiResultFn;
    std::unique_ptr<juce::WildcardFileFilter> filter;
    std::unique_ptr<juce::FileBrowserComponent> browser;
    juce::Label titleLabel;
    juce::TextButton okBtn, cancelBtn;
    juce::TextButton newFolderBtn { "New folder..." }, createFolderBtn { "Create" };
    juce::TextEditor newFolderName;
    juce::File chosen;
};
} // namespace

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
