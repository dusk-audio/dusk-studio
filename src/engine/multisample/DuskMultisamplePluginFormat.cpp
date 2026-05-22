#include "DuskMultisamplePluginFormat.h"
#include "DuskMultisampleProcessor.h"

namespace duskstudio
{
bool DuskMultisamplePluginFormat::isSoundfontExtension (const juce::String& path) noexcept
{
    const auto ext = juce::File (path).getFileExtension().toLowerCase();
    // .sf2 listed here so Phase 2 picks up SF2 import for free once
    // Sf2ToSfz lands; for Phase 1, createPluginInstance returns an
    // error for .sf2 (the editor's tooltip surfaces it as "in 1.0").
    return ext == ".sfz" || ext == ".sf2";
}

bool DuskMultisamplePluginFormat::fileMightContainThisPluginType (const juce::String& path)
{
    return isSoundfontExtension (path);
}

bool DuskMultisamplePluginFormat::doesPluginStillExist (const juce::PluginDescription& desc)
{
    if (desc.fileOrIdentifier.isEmpty()) return true;   // unbound desc
    return juce::File (desc.fileOrIdentifier).existsAsFile();
}

juce::String DuskMultisamplePluginFormat::getNameOfPluginFromIdentifier (const juce::String& path)
{
    const auto file = juce::File (path);
    if (file.existsAsFile())
        return file.getFileNameWithoutExtension();
    // Empty / unresolved identifier - the picker uses this for the
    // "blank" Multisample entry the user selects to open a file dialog.
    return "Dusk Multisample";
}

void DuskMultisamplePluginFormat::findAllTypesForFile (
    juce::OwnedArray<juce::PluginDescription>& results,
    const juce::String& path)
{
    if (! isSoundfontExtension (path)) return;
    auto desc = std::make_unique<juce::PluginDescription>();
    desc->name              = juce::File (path).getFileNameWithoutExtension();
    desc->descriptiveName   = "Dusk Multisample: " + desc->name;
    desc->pluginFormatName  = getName();
    desc->category          = "Instrument";
    desc->manufacturerName  = "Dusk Audio";
    desc->version           = "0.9.0";
    desc->fileOrIdentifier  = path;
    desc->isInstrument      = true;
    desc->numInputChannels  = 0;
    desc->numOutputChannels = 2;
    results.add (desc.release());
}

void DuskMultisamplePluginFormat::createPluginInstance (
    const juce::PluginDescription& desc,
    double initialSampleRate,
    int initialBufferSize,
    PluginCreationCallback callback)
{
    // .sf2 placeholder: createPluginInstance fails with a clear
    // message until Phase 2's Sf2ToSfz converter lands. The picker
    // still shows .sf2 files (because fileMightContainThisPluginType
    // claims them) but selecting one surfaces the "Coming in 1.0"
    // error so the user knows it's not unsupported, just deferred.
    const auto ext = juce::File (desc.fileOrIdentifier).getFileExtension().toLowerCase();
    if (ext == ".sf2")
    {
        callback (nullptr,
                  "SF2 import lands in Dusk Studio 1.0. Beta supports "
                  "SFZ files; use one of the many free SFZ packs in "
                  "the meantime.");
        return;
    }

    juce::ignoreUnused (initialSampleRate, initialBufferSize);
    // PluginSlot::loadFromFile calls prepareToPlay on the returned
    // instance with the host's live SR + block size, so we don't
    // call it here - the format's initialSampleRate / initialBufferSize
    // args are a hint, not a contract, and double-calling would mean
    // sfizz_set_sample_rate fires twice on every load.
    auto inst = std::make_unique<DuskMultisampleProcessor>();

    if (desc.fileOrIdentifier.isNotEmpty())
    {
        juce::String err;
        if (! inst->loadSfzFile (juce::File (desc.fileOrIdentifier), err))
        {
            callback (nullptr, err);
            return;
        }
    }
    callback (std::move (inst), {});
}
} // namespace duskstudio
