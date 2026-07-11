#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>

// Wire protocol for out-of-process plugin scanning. The parent
// (PluginManager's OutOfProcessPluginScanner) spawns dusk-studio-plugin-host
// with `--scan <format> <file>`; the child instantiates the plugin just far
// enough to read its PluginDescription(s) and prints them between sentinel
// markers on stdout. A plugin that crashes / hangs during the scan dies in
// the child, leaving no payload — the parent treats that as a failed scan and
// blacklists the file.
//
// Both sides go through this one header so the sentinels + serialisation can
// never drift out of sync, and so the parse path is unit-testable without
// spawning a subprocess.
namespace duskstudio::scanproto
{
inline constexpr const char* kPayloadBegin = "==DUSK_SCAN_BEGIN==";
inline constexpr const char* kPayloadEnd   = "==DUSK_SCAN_END==";

// Scan-sandbox policy: which formats load third-party binary code and therefore
// MUST be probed out-of-process — an unauthorized or crashy plugin scanned
// in-process takes down the whole app. Our own in-house formats (e.g.
// DuskMultisample) are trusted and scan in-process. Centralised here so the
// scanner's routing and its unit test read from one list.
inline bool formatRequiresSandbox (const juce::String& formatName)
{
    return formatName == "VST3"
        || formatName == "LV2"
        || formatName == "AudioUnit"
        || formatName == "VST";
}

// Child side: serialise the discovered descriptions into the stdout payload.
// The plugin's own stdout chatter (if any) is emitted by its init code, which
// runs BEFORE this is printed, so it lands ahead of kPayloadBegin and the
// parent's extract step skips it.
inline juce::String makePayload (const juce::OwnedArray<juce::PluginDescription>& found)
{
    juce::XmlElement root ("PLUGINS");
    for (auto* d : found)
        if (d != nullptr)
            root.addChildElement (d->createXml().release());

    juce::String s;
    s << kPayloadBegin << "\n"
      << root.toString (juce::XmlElement::TextFormat().singleLine()) << "\n"
      << kPayloadEnd << "\n";
    return s;
}

// Parent side: pull the XML bytes between the sentinels out of the child's
// captured stdout. Returns empty when either sentinel is missing (child
// crashed / hung before completing the print) — the caller treats empty as a
// failed scan.
inline juce::String extractPayload (const juce::String& childStdout)
{
    const int b = childStdout.indexOf (kPayloadBegin);
    if (b < 0) return {};
    const int from = b + juce::String (kPayloadBegin).length();
    const int e = childStdout.indexOf (from, kPayloadEnd);
    if (e < 0) return {};
    return childStdout.substring (from, e).trim();
}

// Parent side: parse an extracted payload into descriptions. No-op (leaves
// `out` untouched) on malformed XML.
inline void parsePayload (const juce::String& payload,
                          juce::OwnedArray<juce::PluginDescription>& out)
{
    if (auto xml = juce::parseXML (payload))
        for (auto* e : xml->getChildIterator())
        {
            auto desc = std::make_unique<juce::PluginDescription>();
            if (desc->loadFromXml (*e))
                out.add (desc.release());
        }
}
} // namespace duskstudio::scanproto
