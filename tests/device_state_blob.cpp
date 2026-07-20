#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/device/DeviceStateBlob.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <string>

// DeviceStateBlob reads/writes the per-machine audio-device state. The version-1
// JSON form is Dusk's own; the legacy <DEVICESETUP> form is what JUCE wrote before
// the de-JUCE migration. These tests pin two things: the JSON form round-trips,
// and the bespoke legacy reader is bit-identical to the real juce::XmlDocument +
// juce::BigInteger path it replaces (A/B - JUCE is linkable in the test binary).

using duskstudio::device::ChannelSet;
using duskstudio::device::DeviceSetup;
using duskstudio::device::DeviceStateBlob;

namespace
{
std::uint64_t bigToRaw (const juce::BigInteger& b)
{
    std::uint64_t v = 0;
    for (int i = 0; i < 64; ++i)
        if (b[i]) v |= (std::uint64_t) 1 << i;
    return v;
}

// Build the legacy blob string exactly as juce::AudioDeviceManager::createStateXml
// does (attribute set + XmlElement::toString prolog). A null chans pointer means
// the attribute is absent (the useDefault*Channels case).
std::string makeLegacyXml (const std::string& type,
                           const std::string& outName, const std::string& inName,
                           double rate, int buf,
                           const std::string* inChansBase2,
                           const std::string* outChansBase2)
{
    juce::XmlElement xml ("DEVICESETUP");
    xml.setAttribute ("deviceType", juce::String (type));
    xml.setAttribute ("audioOutputDeviceName", juce::String (outName));
    xml.setAttribute ("audioInputDeviceName", juce::String (inName));
    xml.setAttribute ("audioDeviceRate", rate);
    xml.setAttribute ("audioDeviceBufferSize", buf);
    if (inChansBase2  != nullptr) xml.setAttribute ("audioDeviceInChans",  juce::String (*inChansBase2));
    if (outChansBase2 != nullptr) xml.setAttribute ("audioDeviceOutChans", juce::String (*outChansBase2));
    return xml.toString().toStdString();
}

// The reference decode: the JUCE parser + BigInteger, mirroring
// juce::AudioDeviceManager::initialiseFromXML line for line.
struct Reference
{
    std::string deviceType;
    DeviceSetup setup;
};

Reference juceReference (const std::string& blob)
{
    Reference r;
    const auto xml = juce::parseXML (juce::String (blob));
    REQUIRE (xml != nullptr);

    r.deviceType             = xml->getStringAttribute ("deviceType").toStdString();
    r.setup.outputDeviceName = xml->getStringAttribute ("audioOutputDeviceName").toStdString();
    r.setup.inputDeviceName  = xml->getStringAttribute ("audioInputDeviceName").toStdString();
    r.setup.sampleRate       = xml->getDoubleAttribute ("audioDeviceRate", 0.0);
    r.setup.bufferSize       = xml->getIntAttribute ("audioDeviceBufferSize", 0);

    juce::BigInteger inB, outB;
    inB .parseString (xml->getStringAttribute ("audioDeviceInChans",  "11"), 2);
    outB.parseString (xml->getStringAttribute ("audioDeviceOutChans", "11"), 2);
    r.setup.inputChannels  = ChannelSet::fromRaw (bigToRaw (inB));
    r.setup.outputChannels = ChannelSet::fromRaw (bigToRaw (outB));

    r.setup.useDefaultInputChannels  = ! xml->hasAttribute ("audioDeviceInChans");
    r.setup.useDefaultOutputChannels = ! xml->hasAttribute ("audioDeviceOutChans");
    return r;
}

void requireSameSetup (const DeviceStateBlob& got, const Reference& ref)
{
    REQUIRE (got.deviceType == ref.deviceType);
    REQUIRE (got.setup.outputDeviceName == ref.setup.outputDeviceName);
    REQUIRE (got.setup.inputDeviceName  == ref.setup.inputDeviceName);
    REQUIRE_THAT (got.setup.sampleRate, Catch::Matchers::WithinAbs (ref.setup.sampleRate, 1e-9));
    REQUIRE (got.setup.bufferSize == ref.setup.bufferSize);
    REQUIRE (got.setup.useDefaultInputChannels  == ref.setup.useDefaultInputChannels);
    REQUIRE (got.setup.useDefaultOutputChannels == ref.setup.useDefaultOutputChannels);
    REQUIRE (got.setup.inputChannels  == ref.setup.inputChannels);
    REQUIRE (got.setup.outputChannels == ref.setup.outputChannels);
}

ChannelSet chans (std::initializer_list<int> idx)
{
    ChannelSet cs;
    for (int i : idx) cs.setBit (i);
    return cs;
}
} // namespace

TEST_CASE ("DeviceStateBlob JSON round-trips explicit channels", "[audio][device]")
{
    DeviceStateBlob in;
    in.deviceType             = "PipeWire";
    in.setup.outputDeviceName = "UMC1820";
    in.setup.inputDeviceName  = "UMC1820";
    in.setup.sampleRate       = 48000.0;
    in.setup.bufferSize       = 256;
    in.setup.useDefaultOutputChannels = false;
    in.setup.useDefaultInputChannels  = false;
    in.setup.outputChannels = chans ({ 0, 1 });
    in.setup.inputChannels  = chans ({ 2, 3 });

    const auto blob = in.toJson();
    const auto out = DeviceStateBlob::parse (blob);
    REQUIRE (out.has_value());

    REQUIRE (out->deviceType == in.deviceType);
    REQUIRE (out->setup.outputDeviceName == in.setup.outputDeviceName);
    REQUIRE (out->setup.inputDeviceName  == in.setup.inputDeviceName);
    REQUIRE_THAT (out->setup.sampleRate, Catch::Matchers::WithinAbs (in.setup.sampleRate, 1e-9));
    REQUIRE (out->setup.bufferSize == in.setup.bufferSize);
    REQUIRE_FALSE (out->setup.useDefaultOutputChannels);
    REQUIRE_FALSE (out->setup.useDefaultInputChannels);
    REQUIRE (out->setup.outputChannels == in.setup.outputChannels);
    REQUIRE (out->setup.inputChannels  == in.setup.inputChannels);
}

TEST_CASE ("DeviceStateBlob JSON omits channel keys when useDefault", "[audio][device]")
{
    DeviceStateBlob in;
    in.deviceType             = "PipeWire";
    in.setup.outputDeviceName = "Built-in Audio";
    in.setup.sampleRate       = 44100.0;
    in.setup.bufferSize       = 512;
    // useDefault*Channels stay at their true default.

    const auto blob = in.toJson();
    REQUIRE (blob.find ("outputChans") == std::string::npos);
    REQUIRE (blob.find ("inputChans")  == std::string::npos);

    const auto out = DeviceStateBlob::parse (blob);
    REQUIRE (out.has_value());
    REQUIRE (out->setup.useDefaultOutputChannels);
    REQUIRE (out->setup.useDefaultInputChannels);
    // Absent key => cleared mask.
    REQUIRE (out->setup.outputChannels.isZero());
    REQUIRE (out->setup.inputChannels.isZero());
}

TEST_CASE ("DeviceStateBlob legacy XML A/B: full attribute set", "[audio][device]")
{
    const std::string in  = "1111";   // channels 0-3
    const std::string out = "11";     // channels 0-1
    const auto blob = makeLegacyXml ("ALSA", "UMC1820", "UMC1820", 48000.0, 256, &in, &out);

    const auto got = DeviceStateBlob::parse (blob);
    REQUIRE (got.has_value());
    requireSameSetup (*got, juceReference (blob));
}

TEST_CASE ("DeviceStateBlob legacy XML A/B: missing chans attrs => useDefault", "[audio][device]")
{
    const auto blob = makeLegacyXml ("PipeWire", "Scarlett", "Scarlett", 44100.0, 128, nullptr, nullptr);

    const auto got = DeviceStateBlob::parse (blob);
    REQUIRE (got.has_value());
    REQUIRE (got->setup.useDefaultInputChannels);
    REQUIRE (got->setup.useDefaultOutputChannels);
    requireSameSetup (*got, juceReference (blob));
}

TEST_CASE ("DeviceStateBlob legacy XML A/B: entity-laden and UTF-8 device names", "[audio][device]")
{
    SECTION ("the five standard XML entities")
    {
        const std::string name = "M&M <Audio> \"Pro\" 'X'";
        const auto blob = makeLegacyXml ("ALSA", name, name, 48000.0, 256, nullptr, nullptr);

        const auto ref = juceReference (blob);
        REQUIRE (ref.setup.outputDeviceName == name);   // JUCE escaped + decoded losslessly

        const auto got = DeviceStateBlob::parse (blob);
        REQUIRE (got.has_value());
        requireSameSetup (*got, ref);
    }

    SECTION ("UTF-8 device name")
    {
        const std::string name = "R\xC3\xB8""de NT\xE2\x84\xA2 \xE2\x99\xAA";   // Røde NT™ ♪
        const auto blob = makeLegacyXml ("PipeWire", name, "", 48000.0, 256, nullptr, nullptr);

        const auto got = DeviceStateBlob::parse (blob);
        REQUIRE (got.has_value());
        requireSameSetup (*got, juceReference (blob));
    }
}

TEST_CASE ("DeviceStateBlob legacy XML A/B: MSB-first mask strings", "[audio][device]")
{
    // "110" and "011" are different numbers, so different channel sets. This is
    // the property that a byte-reversed reader would silently break.
    const std::string a = "110";   // value 6 => channels 1,2
    const std::string b = "011";   // value 3 => channels 0,1

    const auto blobA = makeLegacyXml ("ALSA", "dev", "dev", 48000.0, 256, &a, &a);
    const auto blobB = makeLegacyXml ("ALSA", "dev", "dev", 48000.0, 256, &b, &b);

    const auto gotA = DeviceStateBlob::parse (blobA);
    const auto gotB = DeviceStateBlob::parse (blobB);
    REQUIRE (gotA.has_value());
    REQUIRE (gotB.has_value());

    requireSameSetup (*gotA, juceReference (blobA));
    requireSameSetup (*gotB, juceReference (blobB));

    REQUIRE (gotA->setup.inputChannels != gotB->setup.inputChannels);
    REQUIRE (gotA->setup.inputChannels == chans ({ 1, 2 }));
    REQUIRE (gotB->setup.inputChannels == chans ({ 0, 1 }));
}

TEST_CASE ("DeviceStateBlob malformed input => empty-equivalent", "[audio][device]")
{
    REQUIRE_FALSE (DeviceStateBlob::parse ("").has_value());
    REQUIRE_FALSE (DeviceStateBlob::parse ("    \n\t ").has_value());
    REQUIRE_FALSE (DeviceStateBlob::parse ("not markup at all").has_value());
    REQUIRE_FALSE (DeviceStateBlob::parse ("{ broken json ").has_value());
    REQUIRE_FALSE (DeviceStateBlob::parse ("<OTHERROOT deviceType=\"x\"/>").has_value());
    REQUIRE_FALSE (DeviceStateBlob::parse ("<DEVICESETUP deviceType=\"x").has_value());   // unterminated value
    // A non-object JSON value is not a valid blob.
    REQUIRE_FALSE (DeviceStateBlob::parse ("[1,2,3]").has_value());
}

TEST_CASE ("DeviceStateBlob outputDeviceName on both formats", "[audio][device]")
{
    SECTION ("JSON: output name wins")
    {
        DeviceStateBlob in;
        in.setup.outputDeviceName = "MainOut";
        in.setup.inputDeviceName  = "MainIn";
        REQUIRE (DeviceStateBlob::outputDeviceName (in.toJson()) == "MainOut");
    }

    SECTION ("JSON: empty output falls back to input")
    {
        DeviceStateBlob in;
        in.setup.inputDeviceName = "OnlyIn";
        REQUIRE (DeviceStateBlob::outputDeviceName (in.toJson()) == "OnlyIn");
    }

    SECTION ("legacy XML: output name wins")
    {
        const auto blob = makeLegacyXml ("ALSA", "MainOut", "MainIn", 48000.0, 256, nullptr, nullptr);
        REQUIRE (DeviceStateBlob::outputDeviceName (blob) == "MainOut");
    }

    SECTION ("legacy XML: empty output falls back to input")
    {
        const auto blob = makeLegacyXml ("ALSA", "", "OnlyIn", 48000.0, 256, nullptr, nullptr);
        REQUIRE (DeviceStateBlob::outputDeviceName (blob) == "OnlyIn");
    }

    SECTION ("unparseable => empty")
    {
        REQUIRE (DeviceStateBlob::outputDeviceName ("garbage").empty());
    }
}
