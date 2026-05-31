#include <catch2/catch_test_macros.hpp>

#include "engine/ipc/PluginScanProtocol.h"

using namespace duskstudio::scanproto;

namespace
{
juce::PluginDescription makeDesc (const juce::String& name,
                                  const juce::String& format,
                                  juce::uint32 uid,
                                  bool isInstrument)
{
    juce::PluginDescription d;
    d.name             = name;
    d.descriptiveName  = name;
    d.pluginFormatName = format;
    d.manufacturerName = "Acme";
    d.fileOrIdentifier = "/some/path/" + name + ".vst3";
    d.uniqueId         = (int) uid;
    d.isInstrument     = isInstrument;
    d.numInputChannels  = isInstrument ? 0 : 2;
    d.numOutputChannels = 2;
    return d;
}
} // namespace

TEST_CASE ("scan protocol round-trips plugin descriptions", "[scanproto]")
{
    juce::OwnedArray<juce::PluginDescription> in;
    in.add (new juce::PluginDescription (makeDesc ("Reverb",  "VST3", 0xabc123, false)));
    in.add (new juce::PluginDescription (makeDesc ("Synth X", "VST3", 0x00ff01, true)));

    const juce::String stdoutBytes = makePayload (in);

    const juce::String payload = extractPayload (stdoutBytes);
    REQUIRE (payload.isNotEmpty());

    juce::OwnedArray<juce::PluginDescription> out;
    parsePayload (payload, out);

    REQUIRE (out.size() == 2);
    CHECK (out[0]->name             == "Reverb");
    CHECK (out[0]->pluginFormatName == "VST3");
    CHECK (out[0]->uniqueId         == (int) 0xabc123);
    CHECK (out[0]->isInstrument     == false);
    CHECK (out[1]->name             == "Synth X");   // name with a space survives
    CHECK (out[1]->isInstrument     == true);
    CHECK (out[1]->uniqueId         == (int) 0x00ff01);
}

TEST_CASE ("scan protocol: clean empty result is not a crash", "[scanproto]")
{
    // A search-path file that is not actually a plugin: the child finds zero
    // descriptions but exits cleanly. The payload must still be extractable
    // (so the parent does NOT blacklist it) and parse to zero descriptions.
    juce::OwnedArray<juce::PluginDescription> none;
    const juce::String payload = extractPayload (makePayload (none));
    REQUIRE (payload.isNotEmpty());

    juce::OwnedArray<juce::PluginDescription> out;
    parsePayload (payload, out);
    CHECK (out.isEmpty());
}

TEST_CASE ("scan protocol: missing/truncated payload reads as failure", "[scanproto]")
{
    SECTION ("no sentinels at all (child crashed before printing)")
    {
        CHECK (extractPayload ("").isEmpty());
        CHECK (extractPayload ("Segmentation fault\n").isEmpty());
    }
    SECTION ("begin sentinel but no end (crashed mid-print)")
    {
        const juce::String truncated =
            juce::String (kPayloadBegin) + "\n<PLUGINS><PLUGIN name=\"x\"";
        CHECK (extractPayload (truncated).isEmpty());
    }
    SECTION ("end sentinel but no begin")
    {
        CHECK (extractPayload (juce::String ("noise ") + kPayloadEnd).isEmpty());
    }
}

TEST_CASE ("scan protocol: plugin stdout chatter before the payload is skipped", "[scanproto]")
{
    juce::OwnedArray<juce::PluginDescription> in;
    in.add (new juce::PluginDescription (makeDesc ("Delay", "VST3", 0x42, false)));

    // Plugins commonly spew banner / licence lines to stdout during init,
    // which run before the payload is printed. extractPayload must skip them.
    const juce::String noisy = "lib v1.2 loaded\n[plugin] hello world\n"
                             + makePayload (in);

    juce::OwnedArray<juce::PluginDescription> out;
    parsePayload (extractPayload (noisy), out);
    REQUIRE (out.size() == 1);
    CHECK (out[0]->name == "Delay");
}

TEST_CASE ("scan protocol: malformed payload XML parses to nothing", "[scanproto]")
{
    juce::OwnedArray<juce::PluginDescription> out;
    parsePayload ("<PLUGINS><PLUGIN garbled", out);   // not well-formed
    CHECK (out.isEmpty());
}
