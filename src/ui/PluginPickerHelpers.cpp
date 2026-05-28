#include "PluginPickerHelpers.h"
#include "DuskFileBrowser.h"
#include "EmbeddedModal.h"
#include "PluginPickerPanel.h"
#include "../engine/PluginManager.h"
#include "../engine/PluginSlot.h"
#if DUSKSTUDIO_HAS_MULTISAMPLE
 #include "../engine/multisample/AriaBank.h"
#endif

namespace duskstudio::pluginpicker
{
namespace
{
// One picker modal at a time, app-wide. EmbeddedModal::close() is
// idempotent so re-entering show() across different parents is safe;
// callers don't need to track instance lifetime.
EmbeddedModal& sharedPickerModal()
{
    static EmbeddedModal m;
    return m;
}

// Separate static modal so alerts can stack OVER the picker modal
// without one clobbering the other.
EmbeddedModal& sharedAlertModal()
{
    static EmbeddedModal m;
    return m;
}

// Dedicated chooser modal so the three-button "Hardware / Soundfont /
// Plugin" picker can close itself BEFORE the next step opens its own
// modal (which may be the shared picker modal). Stacking on the same
// static instance would race.
EmbeddedModal& sharedChooserModal()
{
    static EmbeddedModal m;
    return m;
}

// Dusk-styled message panel - title row, multiline message, single OK
// button. Replaces juce::AlertWindow for the picker's scan-complete /
// load-failure feedback so we don't drop another native popup onto the
// XWayland-flaky stack.
class DuskAlertPanel final : public juce::Component
{
public:
    DuskAlertPanel (juce::String title, juce::String message)
        : titleStr (std::move (title)), messageStr (std::move (message))
    {
        setOpaque (true);
        okBtn.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff262630));
        okBtn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff5a4880));
        okBtn.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd0d0d4));
        okBtn.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
        okBtn.onClick = [this] { if (onOK) onOK(); };
        addAndMakeVisible (okBtn);
        setSize (460, 220);
        setWantsKeyboardFocus (true);
    }

    std::function<void()> onOK;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1a1a22));
        auto r = getLocalBounds().reduced (20);

        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
        g.drawText (titleStr, r.removeFromTop (24),
                     juce::Justification::topLeft, false);
        r.removeFromTop (10);

        g.setColour (juce::Colour (0xffd0d0d4));
        g.setFont (juce::Font (juce::FontOptions (12.5f)));
        // Reserve the bottom strip for the OK button; everything above
        // is message body. drawFittedText wraps at the body width.
        auto body = r.withTrimmedBottom (kButtonStripH + 8);
        g.drawFittedText (messageStr.isEmpty() ? juce::String ("Unknown error")
                                                  : messageStr,
                            body, juce::Justification::topLeft, 6);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20);
        auto buttonRow = r.removeFromBottom (kButtonStripH);
        okBtn.setBounds (buttonRow.removeFromRight (100));
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey || k == juce::KeyPress::returnKey)
        {
            if (onOK) onOK();
            return true;
        }
        return false;
    }

private:
    static constexpr int kButtonStripH = 32;
    juce::String titleStr;
    juce::String messageStr;
    juce::TextButton okBtn { "OK" };
};

void showDuskAlert (juce::Component& parent,
                     juce::String title,
                     juce::String message,
                     std::function<void()> onDismiss = {})
{
    auto panel = std::make_unique<DuskAlertPanel> (std::move (title),
                                                     std::move (message));
    // CONTRACT: onDismiss fires exactly once per show. Two paths exist —
    // panel.onOK (button click) and show()'s onDismiss (dim-click / Esc).
    // These are mutually exclusive ONLY because EmbeddedModal::close()
    // does NOT invoke userOnDismiss; it just tears down the dim /
    // backdrop / body. If EmbeddedModal::close() is ever changed to fire
    // userOnDismiss for symmetry, this helper will silently double-fire
    // onDismiss and any downstream chained callback (e.g. picker reopen)
    // will run twice. Audit both lambdas if you touch EmbeddedModal.
    panel->onOK = [onDismiss]() mutable
    {
        sharedAlertModal().close();
        if (onDismiss) onDismiss();
    };
    sharedAlertModal().show (parent, std::move (panel),
                                /*onDismiss*/ [onDismiss]() mutable
                                {
                                    sharedAlertModal().close();
                                    if (onDismiss) onDismiss();
                                });
}

// Fallback for callsites that have no parent component handy. Keeps the
// flow alive (better than swallowing the error silently) but the
// long-term direction is to plumb a parent everywhere.
void showLoadFailureAlertFallback (const juce::String& message)
{
    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::WarningIcon)
            .withTitle ("Plugin load failed")
            .withMessage (message.isEmpty() ? "Unknown error" : message)
            .withButton ("OK"),
        nullptr);
}
} // namespace

void runScanModal (PluginManager& manager, juce::Component* parent,
                    std::function<void()> onAlertDismiss)
{
    // scanInstalledPlugins is synchronous and blocks the message thread,
    // so a real progress dialog can't repaint while it runs. The previous
    // JUCE AlertWindow "scanning..." pop was cosmetic - it would freeze
    // and then disappear. Skip the progress UX entirely; show only the
    // completion alert as a Dusk in-window modal.
    const int added = manager.scanInstalledPlugins();

    auto message = juce::String::formatted (
        "Added %d plugin%s to the picker. (Total known: %d)",
        added, added == 1 ? "" : "s",
        manager.getKnownPluginList().getNumTypes());

    if (parent != nullptr)
    {
        showDuskAlert (*parent, "Plugin scan complete", std::move (message),
                          std::move (onAlertDismiss));
    }
    else
    {
        // Fallback for callers without a parent component handy. JUCE
        // AlertWindow remains a native popup; eventually every callsite
        // should plumb a parent and this branch can be removed. Pass the
        // dismiss callback through ModalCallbackFunction so it fires when
        // the user actually closes the alert, not immediately on
        // schedule.
        juce::AlertWindow::showAsync (
            juce::MessageBoxOptions()
                .withIconType (juce::MessageBoxIconType::InfoIcon)
                .withTitle ("Plugin scan complete")
                .withMessage (message)
                .withButton ("OK"),
            juce::ModalCallbackFunction::create (
                [cb = std::move (onAlertDismiss)] (int /*result*/) mutable
                {
                    if (cb) cb();
                }));
    }
}

namespace
{
// Returns true when the plugin currently loaded into `slot` matches the
// expected kind. Empty slot returns false (caller decides whether to
// treat that as success or failure). Message-thread only - calls into
// the plugin via fillInPluginDescription.
bool loadedKindMatches (PluginSlot& slot, PluginKind kind)
{
    if (! slot.isLoaded()) return false;
    const bool isInstrument = slot.isLoadedPluginInstrument();
    return (kind == PluginKind::Instruments) ? isInstrument : ! isInstrument;
}

void rejectMismatchedKind (PluginSlot& slot, PluginKind kind)
{
    slot.unload();
    const juce::String wanted = (kind == PluginKind::Instruments)
                                  ? "instrument" : "effect";
    const juce::String got    = (kind == PluginKind::Instruments)
                                  ? "effect" : "instrument";
    const auto msg = "This slot expects an " + wanted + " plugin but the chosen file is an "
                   + got + ". The slot was left empty. Use a MIDI track for instrument "
                     "plugins and an audio track for effect plugins.";
    if (auto* tlw = juce::TopLevelWindow::getActiveTopLevelWindow())
        if (auto* dw = dynamic_cast<juce::DocumentWindow*> (tlw))
            if (auto* content = dw->getContentComponent())
            {
                showDuskAlert (*content, "Plugin kind mismatch", msg);
                return;
            }
    // Fallback path — no top-level component (shouldn't happen in
    // normal use). Keep native alert so we don't swallow the warning.
    juce::AlertWindow::showMessageBoxAsync (
        juce::AlertWindow::WarningIcon, "Plugin kind mismatch", msg);
}
} // namespace

void openFileChooser (PluginSlot& slot,
                       std::unique_ptr<juce::FileChooser>& chooserOwner,
                       std::function<void()> onChange,
                       juce::Component::SafePointer<juce::Component> parentForLifetime,
                       PluginKind expectedKind)
{
    // VST3 plugins are bundles (directories), so the chooser allows both
    // files and directories. canSelectDirectories lets the user pick the
    // .vst3 bundle root. Default location is platform-specific - macOS
    // installs to ~/Library/Audio/Plug-Ins/VST3; Linux uses ~/.vst3.
#if defined(__APPLE__)
    const auto defaultDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                .getChildFile ("Library/Audio/Plug-Ins/VST3");
#else
    const auto defaultDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                .getChildFile (".vst3");
#endif
    auto* host = parentForLifetime.getComponent();
    if (host == nullptr) return;

    // DuskFileBrowser owns its own lifetime (in-window EmbeddedModal).
    // The legacy chooserOwner unique_ptr is unused on this path; left
    // in the signature for source-compat with callers that still hold
    // it (removed in a follow-up pass).
    juce::ignoreUnused (chooserOwner);

    filebrowser::open (*host, {
        /*title*/                  "Select a plugin",
        /*initialFileOrDirectory*/ defaultDir,
        /*filePatternsAllowed*/    "*.vst3;*.component;*.so;*.lv2",
        /*mode*/                   filebrowser::Mode::Open,
        /*warnAboutOverwriting*/   false,
        /*selectDirectories*/      true,
    },
    [&slot, onChange = std::move (onChange),
     parentForLifetime, expectedKind] (juce::File file)
    {
        if (parentForLifetime.getComponent() == nullptr) return;
        if (file == juce::File()) return;

        juce::String error;
        const bool ok = slot.loadFromFile (file, error);
        if (! ok)
        {
            if (auto* tl = parentForLifetime.getComponent() != nullptr
                              ? parentForLifetime.getComponent()->getTopLevelComponent()
                              : nullptr)
                showDuskAlert (*tl, "Plugin load failed", error);
            else
                showLoadFailureAlertFallback (error);
            return;
        }
        if (! loadedKindMatches (slot, expectedKind))
        {
            rejectMismatchedKind (slot, expectedKind);
            if (onChange) onChange();
            return;
        }
        if (onChange) onChange();
    });
}

#if DUSKSTUDIO_HAS_MULTISAMPLE
// Soundfont (SFZ) chooser. Routes through DuskFileBrowser (in-window
// EmbeddedModal panel) so it shares the same chrome as the plugin
// browse / session save / bounce dialogs. No standalone window, no
// XWayland / Mutter positioning workarounds.
static void openSoundfontFileChooser (PluginSlot& slot,
                                       std::unique_ptr<juce::FileChooser>& chooserOwner,
                                       std::function<void()> onChange,
                                       juce::Component::SafePointer<juce::Component> parentForLifetime)
{
    juce::ignoreUnused (chooserOwner);
    auto* host = parentForLifetime.getComponent();
    if (host == nullptr) return;
    const auto defaultDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    filebrowser::open (*host, {
        /*title*/                  "Load soundfont (.sfz / .sf2 / .bank.xml)",
        /*initialFileOrDirectory*/ defaultDir,
        /*filePatternsAllowed*/    "*.sfz;*.sf2;*.bank.xml",
        /*mode*/                   filebrowser::Mode::Open,
        /*warnAboutOverwriting*/   false,
        /*selectDirectories*/      false,
    },
    [&slot, onChange = std::move (onChange),
     parentForLifetime] (juce::File file)
    {
        if (parentForLifetime.getComponent() == nullptr) return;
        if (file == juce::File()) return;

        // ARIA bank manifest - resolve to the first program's .sfz so
        // PluginSlot::loadFromFile (which routes through
        // DuskMultisamplePluginFormat and only accepts .sfz / .sf2) has
        // something it can consume. Program switcher comes later.
        juce::File fileToLoad = file;
        if (file.getFileName().toLowerCase().endsWith (".bank.xml"))
        {
            auto bankOpt = AriaBank::tryLoadFromSfz (file);
            if (! bankOpt.has_value() || bankOpt->programs.empty())
            {
                if (auto* tl = parentForLifetime.getComponent() != nullptr
                                  ? parentForLifetime.getComponent()->getTopLevelComponent()
                                  : nullptr)
                    showDuskAlert (*tl, "Bank load failed",
                                    "Bank manifest had no programs.");
                else
                    showLoadFailureAlertFallback ("Bank load failed: "
                                                   "bank manifest had no programs.");
                return;
            }
            fileToLoad = bankOpt->programs.front().sfzFile;
        }

        juce::String error;
        const bool ok = slot.loadFromFile (fileToLoad, error);
        if (! ok)
        {
            if (auto* tl = parentForLifetime.getComponent() != nullptr
                              ? parentForLifetime.getComponent()->getTopLevelComponent()
                              : nullptr)
                showDuskAlert (*tl, "Plugin load failed", error);
            else
                showLoadFailureAlertFallback (error);
            return;
        }
        if (onChange) onChange();
    });
}
#endif

void openPickerMenu (PluginSlot& slot,
                      juce::Component& target,
                      std::unique_ptr<juce::FileChooser>& chooserOwner,
                      std::function<void()> onChange,
                      PluginKind kind,
                      juce::Point<int> /*screenPosition*/,
                      std::function<void()> onPickHardwareInsert,
                      bool suppressSecondaryButtons)
{
    auto& manager = slot.getManagerForUi();

    auto descriptions = (kind == PluginKind::Instruments)
                          ? manager.getInstrumentDescriptions()
                          : manager.getEffectDescriptions();

    auto* parent = target.getTopLevelComponent();
    if (parent == nullptr) parent = &target;

    juce::Component::SafePointer<juce::Component> safeTarget (&target);
    juce::Component::SafePointer<juce::Component> safeParent  (parent);
    auto* slotPtr = &slot;
    auto* chooserOwnerPtr = &chooserOwner;

    // Shared closure helpers - all callbacks close the modal first so
    // the picker disappears immediately, then run the action.
    auto closeModal = []
    {
        sharedPickerModal().close();
    };

    PluginPickerPanel::Callbacks cb;

    cb.onCancel = closeModal;

    cb.onScan = [closeModal, slotPtr, safeTarget, safeParent, chooserOwnerPtr,
                  onChange, kind, onPickHardwareInsert]() mutable
    {
        closeModal();

        // Capture for the post-alert reopen. Run scan, then schedule
        // picker reopen ONLY after the user dismisses the completion
        // alert — otherwise the picker stacks back over the alert (alert
        // was added before the picker, so JUCE z-order puts the picker
        // on top), making the result message invisible.
        auto reopenPicker = [slotPtr, safeTarget, chooserOwnerPtr,
                              onChange, kind, onPickHardwareInsert]() mutable
        {
            if (auto* t = safeTarget.getComponent())
                openPickerMenu (*slotPtr, *t, *chooserOwnerPtr,
                                  std::move (onChange), kind, { -1, -1 },
                                  std::move (onPickHardwareInsert));
        };
        runScanModal (slotPtr->getManagerForUi(), safeParent.getComponent(),
                       std::move (reopenPicker));
    };

    cb.onBrowseFile = [closeModal, slotPtr, safeTarget, chooserOwnerPtr,
                        onChange, kind]() mutable
    {
        closeModal();
        if (safeTarget.getComponent() == nullptr) return;
        openFileChooser (*slotPtr, *chooserOwnerPtr,
                           std::move (onChange), safeTarget, kind);
    };

    // Hardware-insert + soundfont bottom-row buttons are SUPPRESSED when
    // this picker was reached via the two-step InsertChooser (those
    // options are already on the chooser's top-level buttons). Other
    // entry points — Replace plugin..., Scan rescan-reopen — keep them
    // for ergonomic in-place switching.
    if (onPickHardwareInsert && ! suppressSecondaryButtons)
    {
        cb.onHardwareInsert = [closeModal, hw = onPickHardwareInsert]() mutable
        {
            closeModal();
            if (hw) hw();
        };
    }

   #if DUSKSTUDIO_HAS_MULTISAMPLE
    if (kind == PluginKind::Instruments && ! suppressSecondaryButtons)
    {
        cb.onLoadSoundfont = [closeModal, slotPtr, safeTarget, chooserOwnerPtr,
                                onChange]() mutable
        {
            closeModal();
            if (safeTarget.getComponent() == nullptr) return;
            openSoundfontFileChooser (*slotPtr, *chooserOwnerPtr,
                                         std::move (onChange), safeTarget);
        };
    }
   #endif

    cb.onPickPlugin = [closeModal, slotPtr, safeTarget, safeParent, onChange, kind]
                        (const juce::PluginDescription& desc) mutable
    {
        closeModal();
        if (safeTarget.getComponent() == nullptr) return;

        // Defence-in-depth: getInstrument/EffectDescriptions already
        // filtered by kind, but a stale KnownPluginList entry could slip
        // through (plugin reclassified by vendor since last scan).
        const bool descMatches = (kind == PluginKind::Instruments)
                                   ? desc.isInstrument
                                   : ! desc.isInstrument;
        if (! descMatches)
        {
            rejectMismatchedKind (*slotPtr, kind);
            if (onChange) onChange();
            return;
        }

        juce::String error;
        if (! slotPtr->loadFromDescription (desc, error))
        {
            if (auto* p = safeParent.getComponent())
                showDuskAlert (*p, "Plugin load failed", error);
            else
                showLoadFailureAlertFallback (error);
            return;
        }
        // Post-load sanity check: rare for the loaded plugin's
        // self-reported flag to differ from its scanned description,
        // but it has been seen (plugin reports differently when hosted
        // vs scanned). Reject + warn in that case too.
        if (! loadedKindMatches (*slotPtr, kind))
        {
            rejectMismatchedKind (*slotPtr, kind);
            if (onChange) onChange();
            return;
        }
        if (onChange) onChange();
    };

    auto panel = std::make_unique<PluginPickerPanel> (
        std::move (descriptions),
        kind == PluginKind::Instruments ? PluginPickerPanel::Kind::Instruments
                                          : PluginPickerPanel::Kind::Effects,
        std::move (cb));

    sharedPickerModal().show (*parent, std::move (panel),
                                /*onDismiss*/ [] { sharedPickerModal().close(); });
}

// ── InsertChooserPanel — three big buttons in a stack ────────────────────

namespace
{
class InsertChooserPanel final : public juce::Component
{
public:
    InsertChooserPanel (bool showHardware, bool showSoundfont,
                         std::function<void()> onHw,
                         std::function<void()> onSf,
                         std::function<void()> onPlugin,
                         std::function<void()> onCancel)
        : onHwFn (std::move (onHw)),
          onSfFn (std::move (onSf)),
          onPluginFn (std::move (onPlugin)),
          onCancelFn (std::move (onCancel)),
          hwVisible (showHardware),
          sfVisible (showSoundfont)
    {
        setOpaque (true);

        auto style = [] (juce::TextButton& b, juce::Colour fill)
        {
            b.setColour (juce::TextButton::buttonColourId,   fill);
            b.setColour (juce::TextButton::buttonOnColourId, fill.brighter (0.2f));
            b.setColour (juce::TextButton::textColourOffId,  juce::Colours::white);
            b.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
            b.setMouseClickGrabsKeyboardFocus (false);
        };

        titleLabel.setText ("Add insert", juce::dontSendNotification);
        titleLabel.setJustificationType (juce::Justification::centredLeft);
        titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
        titleLabel.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
        addAndMakeVisible (titleLabel);

        promptLabel.setText ("Pick what kind of insert to load:", juce::dontSendNotification);
        promptLabel.setJustificationType (juce::Justification::centredLeft);
        promptLabel.setColour (juce::Label::textColourId, juce::Colour (0xffc0c0c8));
        promptLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
        addAndMakeVisible (promptLabel);

        if (hwVisible)
        {
            hwBtn.setButtonText ("Hardware Insert");
            style (hwBtn, juce::Colour (0xff3a5878));
            hwBtn.onClick = [this] { if (onHwFn) onHwFn(); };
            addAndMakeVisible (hwBtn);
        }
        if (sfVisible)
        {
            sfBtn.setButtonText ("Soundfont (.sfz / .sf2 / .bank.xml)");
            style (sfBtn, juce::Colour (0xff5a4a78));
            sfBtn.onClick = [this] { if (onSfFn) onSfFn(); };
            addAndMakeVisible (sfBtn);
        }
        pluginBtn.setButtonText ("Plugin (VST3 / LV2)");
        style (pluginBtn, juce::Colour (0xff385a38));
        pluginBtn.onClick = [this] { if (onPluginFn) onPluginFn(); };
        addAndMakeVisible (pluginBtn);

        cancelBtn.setButtonText ("Cancel");
        cancelBtn.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff262630));
        cancelBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffd0d0d8));
        cancelBtn.onClick = [this] { if (onCancelFn) onCancelFn(); };
        addAndMakeVisible (cancelBtn);

        const int visibleBigButtons = 1 + (hwVisible ? 1 : 0) + (sfVisible ? 1 : 0);
        constexpr int kBigBtnH = 44;
        constexpr int kGap     = 8;
        const int total = 16 + 22 + 6 + 18 + 12               // title + prompt + padding
                        + visibleBigButtons * kBigBtnH
                        + (visibleBigButtons - 1) * kGap
                        + 14 + 32 + 16;                        // gap + cancel + bottom padding
        setSize (380, total);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1a1a22));
        g.setColour (juce::Colour (0xff353540));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 6.0f, 1.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16);
        titleLabel.setBounds (area.removeFromTop (22));
        area.removeFromTop (6);
        promptLabel.setBounds (area.removeFromTop (18));
        area.removeFromTop (12);

        constexpr int kBigBtnH = 44;
        constexpr int kGap     = 8;
        if (hwVisible)
        {
            hwBtn.setBounds (area.removeFromTop (kBigBtnH));
            area.removeFromTop (kGap);
        }
        if (sfVisible)
        {
            sfBtn.setBounds (area.removeFromTop (kBigBtnH));
            area.removeFromTop (kGap);
        }
        pluginBtn.setBounds (area.removeFromTop (kBigBtnH));
        area.removeFromTop (14);

        // Cancel pinned to the bottom, slightly narrower so it reads as
        // secondary action vs the three big choice buttons above.
        auto cancelRow = area.removeFromTop (32);
        const int cw = juce::jmin (120, cancelRow.getWidth());
        cancelBtn.setBounds (cancelRow.withSizeKeepingCentre (cw, 32));
    }

private:
    juce::Label titleLabel, promptLabel;
    juce::TextButton hwBtn, sfBtn, pluginBtn, cancelBtn;
    std::function<void()> onHwFn, onSfFn, onPluginFn, onCancelFn;
    bool hwVisible, sfVisible;
};
} // namespace

void openInsertChooser (PluginSlot& slot,
                         juce::Component& target,
                         std::unique_ptr<juce::FileChooser>& chooserOwner,
                         std::function<void()> onChange,
                         PluginKind kind,
                         std::function<void()> onPickHardwareInsert)
{
    auto* parent = target.getTopLevelComponent();
    if (parent == nullptr) parent = &target;

    juce::Component::SafePointer<juce::Component> safeTarget (&target);
    auto* slotPtr = &slot;
    auto* chooserOwnerPtr = &chooserOwner;

    auto closeChooser = [] { sharedChooserModal().close(); };

    // HW insert only makes sense on audio (effects) slots — a MIDI
    // instrument slot can't route audio out to a physical pair.
    // Soundfont is shown ALWAYS when the multisample backend is built
    // in: it loads via the same path as an instrument plugin, and the
    // engine routes MIDI-driven content even on an audio track (the
    // user picks the SFZ for a reason — flagging it as "wrong slot"
    // here hides a valid choice).
    const bool showHw = (kind == PluginKind::Effects)
                        && static_cast<bool> (onPickHardwareInsert);
   #if DUSKSTUDIO_HAS_MULTISAMPLE
    const bool showSf = true;
   #else
    const bool showSf = false;
   #endif

    auto onHw = [closeChooser, hw = onPickHardwareInsert]() mutable
    {
        closeChooser();
        if (hw) hw();
    };

    auto onSf = [closeChooser, slotPtr, safeTarget, chooserOwnerPtr,
                  onChange]() mutable
    {
        closeChooser();
        if (safeTarget.getComponent() == nullptr) return;
       #if DUSKSTUDIO_HAS_MULTISAMPLE
        openSoundfontFileChooser (*slotPtr, *chooserOwnerPtr,
                                     std::move (onChange), safeTarget);
       #else
        juce::ignoreUnused (slotPtr, chooserOwnerPtr, onChange);
       #endif
    };

    auto onPlugin = [closeChooser, slotPtr, safeTarget, chooserOwnerPtr,
                       onChange, kind]() mutable
    {
        closeChooser();
        if (auto* t = safeTarget.getComponent())
            openPickerMenu (*slotPtr, *t, *chooserOwnerPtr,
                              std::move (onChange), kind, { -1, -1 },
                              /*onPickHardwareInsert*/ {},
                              /*suppressSecondaryButtons*/ true);
    };

    auto onCancel = closeChooser;

    auto panel = std::make_unique<InsertChooserPanel> (
        showHw, showSf, std::move (onHw), std::move (onSf),
        std::move (onPlugin), std::move (onCancel));

    sharedChooserModal().show (*parent, std::move (panel),
                                /*onDismiss*/ [] { sharedChooserModal().close(); });
}
} // namespace duskstudio::pluginpicker
