#include "PluginPickerHelpers.h"
#include "EmbeddedModal.h"
#include "PluginPickerPanel.h"
#include "../engine/PluginManager.h"
#include "../engine/PluginSlot.h"

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
    juce::AlertWindow::showMessageBoxAsync (
        juce::AlertWindow::WarningIcon,
        "Plugin kind mismatch",
        "This slot expects an " + wanted + " plugin but the chosen file is an "
        + got + ". The slot was left empty. Use a MIDI track for instrument "
        "plugins and an audio track for effect plugins.");
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
    chooserOwner = std::make_unique<juce::FileChooser> (
        "Select a plugin",
        defaultDir,
        "*.vst3;*.component;*.so;*.lv2");

    auto* chooserPtr = chooserOwner.get();
    chooserPtr->launchAsync (
        juce::FileBrowserComponent::openMode |
        juce::FileBrowserComponent::canSelectFiles |
        juce::FileBrowserComponent::canSelectDirectories,
        [&slot, &chooserOwner, onChange = std::move (onChange),
         parentForLifetime, expectedKind] (const juce::FileChooser& chooser)
        {
            // If the owning UI component has been destroyed (user
            // switched stages / quit) while the OS dialog was open,
            // the chooserOwner unique_ptr it lives on is also gone.
            // Bail without touching it. JUCE FileChooser self-cleans
            // after this callback returns, so the chooser itself is
            // fine - we just must not deref the now-dead unique_ptr.
            if (parentForLifetime.getComponent() == nullptr) return;
            const auto file = chooser.getResult();

            if (file == juce::File())
            {
                // User cancelled - drop the chooser and bail. (Reset comes
                // last because the JUCE FileChooser keeps `chooser` alive
                // while this lambda runs, but the unique_ptr destruction
                // schedules its own teardown after we return.)
                chooserOwner.reset();
                return;
            }

            juce::String error;
            const bool ok = slot.loadFromFile (file, error);
            chooserOwner.reset();
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
// Soundfont (SFZ) chooser. Mirrors openFileChooser's lifetime model
// but filters for .sfz files and loads through the built-in
// DuskMultisamplePluginFormat — feels native, not like a plugin.
static void openSoundfontFileChooser (PluginSlot& slot,
                                       std::unique_ptr<juce::FileChooser>& chooserOwner,
                                       std::function<void()> onChange,
                                       juce::Component::SafePointer<juce::Component> parentForLifetime)
{
    const auto defaultDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    // useOSNativeDialogBox = false (JUCE's own FileBrowserComponent
    // top-level dialog) without a parentComponent. Native GTK dialog
    // loses stacking against the DAW window on Wayland; non-native
    // + parent crashed Mutter (JUCE-wayland fork's modal-child
    // path); standalone top-level non-native works — same shape as
    // PluginEditorWindow which we already handle on the fork.
    chooserOwner = std::make_unique<juce::FileChooser> (
        "Load soundfont (.sfz)",
        defaultDir,
        "*.sfz;*.sf2",
        /*useOSNativeDialogBox*/ false);

    chooserOwner->launchAsync (
        juce::FileBrowserComponent::openMode |
        juce::FileBrowserComponent::canSelectFiles,
        [&slot, &chooserOwner, onChange = std::move (onChange),
         parentForLifetime] (const juce::FileChooser& chooser)
        {
            if (parentForLifetime.getComponent() == nullptr) return;
            const auto file = chooser.getResult();
            if (file == juce::File())
            {
                chooserOwner.reset();
                return;
            }
            juce::String error;
            const bool ok = slot.loadFromFile (file, error);
            chooserOwner.reset();
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
                      std::function<void()> onPickHardwareInsert)
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

    if (onPickHardwareInsert)
    {
        cb.onHardwareInsert = [closeModal, hw = onPickHardwareInsert]() mutable
        {
            closeModal();
            if (hw) hw();
        };
    }

   #if DUSKSTUDIO_HAS_MULTISAMPLE
    if (kind == PluginKind::Instruments)
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
} // namespace duskstudio::pluginpicker
