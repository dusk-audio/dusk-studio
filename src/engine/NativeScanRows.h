#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#if DUSKSTUDIO_HAS_NATIVE_CLAP
 #include "clap/ClapBundle.h"
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
 #include "vst3/Vst3Bundle.h"
#endif
#include "hosting/NativePluginId.h"

namespace duskstudio::nativescan
{
// One bundle -> picker rows, shared verbatim by the sandboxed scan child
// (dusk-studio-plugin-host --scan-native) and the parent's in-process fallback,
// so both sides emit identical descriptions. Loading a bundle executes its code
// (dlopen + factory read) - that is exactly what the sandbox exists for; call
// these in-process only as the fallback when the child can't spawn.

#if DUSKSTUDIO_HAS_NATIVE_CLAP
inline void appendClapRows (const juce::File& bundle,
                            juce::Array<juce::PluginDescription>& into)
{
    clap::ClapBundle b;
    std::string err;
    if (! b.load (bundle.getFullPathName().toStdString(), err))
        return;
    for (const auto& d : b.plugins())
    {
        juce::PluginDescription desc;
        desc.name             = juce::String (juce::CharPointer_UTF8 (d.name.c_str()));
        desc.manufacturerName = juce::String (juce::CharPointer_UTF8 (d.vendor.c_str()));
        desc.version          = juce::String (juce::CharPointer_UTF8 (d.version.c_str()));
        desc.pluginFormatName = "CLAP";
        desc.fileOrIdentifier = hosting::joinNativeIdentifier (
            bundle.getFullPathName(), juce::String (juce::CharPointer_UTF8 (d.id.c_str())));
        desc.isInstrument     = d.isInstrument();
        into.add (desc);
    }
}
#endif

#if DUSKSTUDIO_HAS_NATIVE_VST3
inline void appendVst3Rows (const juce::File& bundle,
                            juce::Array<juce::PluginDescription>& into)
{
    vst3::Vst3Bundle b;
    std::string err;
    if (! b.load (bundle.getFullPathName().toStdString(), err))
        return;
    for (const auto& d : b.plugins())
    {
        juce::PluginDescription desc;
        desc.name             = juce::String (juce::CharPointer_UTF8 (d.name.c_str()));
        desc.manufacturerName = juce::String (juce::CharPointer_UTF8 (d.vendor.c_str()));
        desc.version          = juce::String (juce::CharPointer_UTF8 (d.version.c_str()));
        desc.pluginFormatName = "VST3-Native";
        desc.fileOrIdentifier = hosting::joinNativeIdentifier (
            bundle.getFullPathName(), juce::String (juce::CharPointer_UTF8 (d.id.c_str())));
        desc.isInstrument     = d.isInstrument;
        into.add (desc);
    }
}
#endif
} // namespace duskstudio::nativescan
