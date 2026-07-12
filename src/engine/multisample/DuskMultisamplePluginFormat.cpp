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
    // Empty / unresolved identifier - rarely hit now that the picker's
    // "Load Soundfont..." entry goes straight through loadFromFile;
    // this label only surfaces if a saved session ever round-trips an
    // unresolved soundfont description.
    return "Soundfont";
}

void DuskMultisamplePluginFormat::findAllTypesForFile (
    juce::OwnedArray<juce::PluginDescription>& results,
    const juce::String& path)
{
    if (! isSoundfontExtension (path)) return;
    auto desc = std::make_unique<juce::PluginDescription>();
    desc->name              = juce::File (path).getFileNameWithoutExtension();
    desc->descriptiveName   = "Soundfont: " + desc->name;
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
    juce::ignoreUnused (initialSampleRate, initialBufferSize);
    // PluginSlot::loadFromFile calls prepareToPlay on the returned
    // instance with the host's live SR + block size, so we don't
    // call it here - the format's initialSampleRate / initialBufferSize
    // args are a hint, not a contract, and double-calling would mean
    // the backend's set_sample_rate fires twice on every load.
    auto inst = std::make_unique<DuskMultisampleProcessor>();

    if (desc.fileOrIdentifier.isNotEmpty())
    {
        const auto file = juce::File (desc.fileOrIdentifier);
        const auto ext = file.getFileExtension().toLowerCase();
        juce::String err;
        const bool ok = (ext == ".sf2")
                          ? inst->loadSf2File (file, err)
                          : inst->loadSfzFile (file, err);
        if (! ok)
        {
            callback (nullptr, err);
            return;
        }
    }
    callback (std::move (inst), {});
}
} // namespace duskstudio
