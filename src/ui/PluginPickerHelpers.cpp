#include "PluginPickerHelpers.h"
#include "../engine/PluginManager.h"
#include "../engine/PluginSlot.h"

namespace duskstudio::pluginpicker
{
namespace
{
constexpr int kIdScan            = 9001;
constexpr int kIdBrowseFile      = 9002;
constexpr int kIdHardwareInsert  = 9003;

void showLoadFailureAlert (const juce::String& message)
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

void runScanModal (PluginManager& manager)
{
    // PluginDirectoryScanner is internally synchronous when allowAsync=false;
    // we surface a tiny banner so the user knows we're working. A polished
    // UX would lift this onto a thread with a real progress bar - good
    // follow-up once the picker is in active use across surfaces.
    auto* dialog = new juce::AlertWindow ("Scanning plugins",
                                            "Looking through VST3 / LV2 install "
                                            "locations... this can take a few "
                                            "seconds the first time.",
                                            juce::MessageBoxIconType::NoIcon);
    dialog->setUsingNativeTitleBar (true);
    dialog->enterModalState (false /*not blocking*/);

    const int added = manager.scanInstalledPlugins();
    delete dialog;

    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::InfoIcon)
            .withTitle ("Plugin scan complete")
            .withMessage (juce::String::formatted (
                "Added %d plugin%s to the picker. (Total known: %d)",
                added, added == 1 ? "" : "s",
                manager.getKnownPluginList().getNumTypes()))
            .withButton ("OK"),
        nullptr);
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
            if (! ok) { showLoadFailureAlert (error); return; }
            if (! loadedKindMatches (slot, expectedKind))
            {
                rejectMismatchedKind (slot, expectedKind);
                if (onChange) onChange();
                return;
            }
            if (onChange) onChange();
        });
}

void openPickerMenu (PluginSlot& slot,
                      juce::Component& target,
                      std::unique_ptr<juce::FileChooser>& chooserOwner,
                      std::function<void()> onChange,
                      PluginKind kind,
                      juce::Point<int> screenPosition,
                      std::function<void()> onPickHardwareInsert)
{
    auto& manager = slot.getManagerForUi();
    auto& known   = manager.getKnownPluginList();

    // Build the filtered list. We can't use KnownPluginList::addToMenu
    // because it builds menu IDs off the full list with no filter hook;
    // we sort and group by manufacturer manually instead. IDs 1..N map
    // to indices in `sorted` (captured by the lambda); 9000+ are reserved
    // for our own action items.
    auto descriptions = (kind == PluginKind::Instruments)
                          ? manager.getInstrumentDescriptions()
                          : manager.getEffectDescriptions();
    std::sort (descriptions.begin(), descriptions.end(),
        [] (const juce::PluginDescription& a, const juce::PluginDescription& b)
        {
            if (a.manufacturerName != b.manufacturerName)
                return a.manufacturerName.compareIgnoreCase (b.manufacturerName) < 0;
            return a.name.compareIgnoreCase (b.name) < 0;
        });

    juce::PopupMenu menu;

    // External Hardware Insert lives at the very top of the menu (above
    // even the "no plugins scanned" banner) so the user can reach it
    // regardless of plugin state. Only shown when the caller wired up
    // a handler - aux + channel slots both do, mastering stages don't.
    if (onPickHardwareInsert)
    {
        menu.addItem (kIdHardwareInsert, "External Hardware Insert...");
        menu.addSeparator();
    }

    if (known.getNumTypes() == 0)
    {
        menu.addSectionHeader ("No plugins scanned yet");
    }
    else if (descriptions.isEmpty())
    {
        menu.addSectionHeader (kind == PluginKind::Instruments
                                  ? "No instruments scanned yet"
                                  : "No effects scanned yet");
    }
    else
    {
        juce::String currentManufacturer;
        juce::PopupMenu submenu;
        for (int i = 0; i < descriptions.size(); ++i)
        {
            const auto& d = descriptions.getReference (i);
            if (d.manufacturerName != currentManufacturer)
            {
                if (currentManufacturer.isNotEmpty())
                    menu.addSubMenu (currentManufacturer, submenu);
                submenu = juce::PopupMenu();
                currentManufacturer = d.manufacturerName;
            }
            // Append format in parens so the user can distinguish
            // VST3 vs LV2 vs AU when the same plugin exists in multiple
            // formats (common for cross-platform vendors). LV2 plugins
            // without a native UI now fall back to a generic parameter
            // editor at open time, so no per-format suffix is needed.
            juce::String label = d.name;
            if (d.pluginFormatName.isNotEmpty())
                label += "  (" + d.pluginFormatName + ")";
            submenu.addItem (i + 1, label);
        }
        if (currentManufacturer.isNotEmpty())
            menu.addSubMenu (currentManufacturer, submenu);
    }
    menu.addSeparator();
    menu.addItem (kIdScan,       "Scan plugins (VST3 / LV2)...");
    menu.addItem (kIdBrowseFile, "Browse for file...");

    juce::Component::SafePointer<juce::Component> safeTarget (&target);
    auto* slotPtr = &slot;
    auto* chooserOwnerPtr = &chooserOwner;
    auto onChangeCopy = onChange;

    // Capture descriptions by shared_ptr so the result lambda can resolve
    // an ID back to a description without copying the whole array per
    // showMenuAsync.
    auto sharedDescriptions = std::make_shared<juce::Array<juce::PluginDescription>> (std::move (descriptions));

    // Anchor on click position when supplied (large click-target buttons
    // would otherwise drop the menu at their top-left), otherwise on the
    // component's bounds.
    auto options = juce::PopupMenu::Options();
    if (screenPosition.x >= 0 && screenPosition.y >= 0)
        options = options.withTargetScreenArea (
            juce::Rectangle<int> (screenPosition.x, screenPosition.y, 1, 1));
    else
        options = options.withTargetComponent (&target);

    auto onHardwareCopy = onPickHardwareInsert;

    menu.showMenuAsync (options,
        [slotPtr, safeTarget, chooserOwnerPtr, onChangeCopy = std::move (onChangeCopy),
         onHardwareCopy = std::move (onHardwareCopy),
         kind, screenPosition, sharedDescriptions] (int result) mutable
        {
            if (safeTarget.getComponent() == nullptr) return;  // host UI gone
            if (result == 0) return;  // cancelled
            if (result == kIdHardwareInsert)
            {
                if (onHardwareCopy) onHardwareCopy();
                return;
            }
            if (result == kIdScan)
            {
                runScanModal (slotPtr->getManagerForUi());
                // Reopen the picker so the user immediately sees the newly
                // scanned plugins without a second click. Preserve the
                // original anchor so the reopened menu lands in the same
                // visual spot.
                openPickerMenu (*slotPtr, *safeTarget.getComponent(),
                                  *chooserOwnerPtr, std::move (onChangeCopy),
                                  kind, screenPosition,
                                  std::move (onHardwareCopy));
                return;
            }
            if (result == kIdBrowseFile)
            {
                openFileChooser (*slotPtr, *chooserOwnerPtr,
                                  std::move (onChangeCopy), safeTarget, kind);
                return;
            }

            // 1..N → index into the filtered, sorted descriptions array.
            const int idx = result - 1;
            if (idx < 0 || idx >= sharedDescriptions->size()) return;
            const auto& desc = sharedDescriptions->getReference (idx);

            // Defence-in-depth: getInstrument/EffectDescriptions already
            // filtered by kind, but a stale KnownPluginList entry (e.g.
            // scanned before the plugin was reclassified by the vendor)
            // could slip through. Verify the description matches the
            // expected kind before loading.
            const bool descIsInstrument = desc.isInstrument;
            const bool descMatches = (kind == PluginKind::Instruments)
                                       ? descIsInstrument
                                       : ! descIsInstrument;
            if (! descMatches)
            {
                rejectMismatchedKind (*slotPtr, kind);
                if (onChangeCopy) onChangeCopy();
                return;
            }

            juce::String error;
            if (! slotPtr->loadFromDescription (desc, error))
            {
                showLoadFailureAlert (error);
                return;
            }
            // Post-load sanity check: rare for the loaded plugin's
            // self-reported flag to differ from its scanned description,
            // but it has been seen (plugin reports differently when
            // hosted vs scanned). Reject + warn in that case too.
            if (! loadedKindMatches (*slotPtr, kind))
            {
                rejectMismatchedKind (*slotPtr, kind);
                if (onChangeCopy) onChangeCopy();
                return;
            }
            if (onChangeCopy) onChangeCopy();
        });
}
} // namespace duskstudio::pluginpicker
