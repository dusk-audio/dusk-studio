#include <catch2/catch_test_macros.hpp>

#include "engine/midi/JuceMidiBackend.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <new>

using duskstudio::midi::makeJuceMidiInputBackend;

// The headless scenario suite holds two engines at once: its own, and one a
// scenario builds after it and destroys first. At shutdown the first engine's
// MIDI input has to stop on its own device manager. Reaching the second one's
// freed manager instead crashes whenever the heap has handed that block on.
TEST_CASE ("JUCE MIDI input keeps its own device manager when a later one dies", "[midi][devices][regression][issue-831]")
{
    juce::AudioDeviceManager first;
    auto firstInput = makeJuceMidiInputBackend (&first);
    firstInput->start();

    alignas (juce::AudioDeviceManager) unsigned char storage[sizeof (juce::AudioDeviceManager)];
    auto* second = new (storage) juce::AudioDeviceManager();
    {
        auto secondInput = makeJuceMidiInputBackend (second);
        secondInput->start();
    }
    second->~AudioDeviceManager();

    // What a reused heap block can hold: every count positive, every pointer
    // wild. An input that reached the dead manager would fault on every run.
    static constexpr unsigned char kReusedByte = 0x5a;
    std::memset (storage, kReusedByte, sizeof (storage));

    firstInput->stop();
    firstInput->start();
    firstInput.reset();

    REQUIRE (std::count (std::begin (storage), std::end (storage), kReusedByte)
             == (std::ptrdiff_t) sizeof (storage));
}
