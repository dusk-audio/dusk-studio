#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace duskstudio::juce_compat
{
// One-call shim hiding the AudioPluginFormatManager::addDefaultFormats
// vs the wayland fork's free juce::addDefaultFormatsToManager() split.
// DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS is set by CMake at configure time
// (CMakeLists.txt scans the JUCE source for the free function symbol)
// so this picks the right API for any combination of platform + JUCE
// version. No __linux__ branch at the call site.
//
// Why CMake-time detection over SFINAE: the plugdata-team fork marks
// the member `= delete`, which passes SFINAE then fails at the call
// site. The upstream JUCE lacks the free function entirely, so
// `decltype(juce::addDefaultFormatsToManager(...))` fails lookup at
// template definition time rather than substitution — also unusable
// for SFINAE. CMake-time text scan is the only clean cross-shape
// detection.
inline void addDefaultFormats (juce::AudioPluginFormatManager& fm)
{
   #if DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS
    juce::addDefaultFormatsToManager (fm);
   #else
    fm.addDefaultFormats();
   #endif
}
} // namespace duskstudio::juce_compat
