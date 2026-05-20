#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <type_traits>

namespace focal::juce_compat
{
namespace detail
{
// SFINAE detector for the plugdata-team JUCE-wayland fork's FREE
// function juce::addDefaultFormatsToManager(). The fork added it +
// marked the member `= delete`; upstream has neither the free function
// nor a deleted member.
//
// Detecting via the free function (rather than the member) is the only
// path that compiles cleanly against both shapes — a member SFINAE
// would resolve "exists" against the fork's deleted overload and fire
// "use of deleted function" at the call site.
template <typename, typename = void>
struct has_free_addDefaultFormatsToManager : std::false_type {};

template <typename T>
struct has_free_addDefaultFormatsToManager<
    T,
    std::void_t<decltype (juce::addDefaultFormatsToManager (std::declval<T&>()))>>
    : std::true_type {};
} // namespace detail

// One-call shim hiding the AudioPluginFormatManager::addDefaultFormats
// vs the wayland fork's free juce::addDefaultFormatsToManager() split.
// Picks the right API at compile time via if constexpr — no platform
// macro needed at the call site. Works for any combination of host
// JUCE version on any platform.
//
// Templated on FM so if constexpr can ACTUALLY discard the dead
// branch — a non-template function would type-check both arms, hitting
// "use of deleted function" against the fork's = delete member.
template <typename FM = juce::AudioPluginFormatManager>
inline void addDefaultFormats (FM& fm)
{
    if constexpr (detail::has_free_addDefaultFormatsToManager<FM>::value)
        juce::addDefaultFormatsToManager (fm);
    else
        fm.addDefaultFormats();
}
} // namespace focal::juce_compat
