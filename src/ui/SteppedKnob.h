#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>
#include <cmath>

namespace duskstudio
{
// Turns a juce::Slider into a stepped selector with hard detents over a small
// set of discrete values — used for the SSL-style bus/master comp controls
// (ratio 2:1/4:1/10:1, attack 0.1..30 ms, release 0.1..1.2 s + Auto) whose
// donor params are AudioParameterChoice, so a continuous knob both lies about
// the value and lands between the real steps.
//
// The owning session atom holds the actual VALUE (e.g. 4.0 for 4:1); the knob
// holds the integer INDEX (so detents snap). On change, store(value) is called
// with the picked step's value. `current` chooses the initial index by nearest
// value. Labels are shown in the readout; textValueSuffix is irrelevant once a
// textFromValueFunction is set. Call AFTER the usual style helper so colours /
// textbox styling stay, then this overrides range + value + callbacks.
inline void configureSteppedKnob (juce::Slider& s,
                                  std::vector<double> values,
                                  juce::StringArray labels,
                                  double current,
                                  std::function<void (double)> store)
{
    jassert (! values.empty() && (int) values.size() == labels.size());
    // Release-safe guard: the jassert above is compiled out in release, so
    // bail rather than index an empty vector / mismatched labels / call a
    // null store if a caller ever violates the contract. Leaves the slider
    // in its prior state (no detents) instead of crashing.
    if (values.empty() || (int) values.size() != labels.size() || ! store)
        return;

    int initial = 0;
    double best = 1.0e30;
    for (size_t i = 0; i < values.size(); ++i)
    {
        const double d = std::abs (values[i] - current);
        if (d < best) { best = d; initial = (int) i; }
    }

    s.setRange (0.0, (double) ((int) values.size() - 1), 1.0);   // integer detents
    s.setSkewFactor (1.0);   // clear any skew a prior style helper set for the old range
    s.setValue ((double) initial, juce::dontSendNotification);
    s.setDoubleClickReturnValue (false, 0.0);

    s.textFromValueFunction = [labels] (double v)
    {
        const int i = juce::jlimit (0, labels.size() - 1, (int) std::lround (v));
        return labels[i];
    };
    s.valueFromTextFunction = [labels, sp = &s] (const juce::String& t)
    {
        const int i = labels.indexOf (t.trim());
        // Unknown / typo'd text: keep the current value instead of snapping to
        // index 0, which would silently overwrite the parameter.
        return i < 0 ? sp->getValue() : (double) i;
    };
    s.onValueChange = [sp = &s, values, store]
    {
        const int i = juce::jlimit (0, (int) values.size() - 1,
                                    (int) std::lround (sp->getValue()));
        store (values[(size_t) i]);
    };
    // Normalise the atom to the chosen step so a legacy off-step value (e.g. a
    // pre-stepped 200 ms release, an exact 100/300 tie that the nearest-index
    // picks differently than the engine's threshold mapping) doesn't leave the
    // knob, the atom, and the choice-index conversion disagreeing until the
    // first knob move.
    store (values[(size_t) initial]);
    s.updateText();
}

// Shared step tables (donor Choice orders). AUTO release uses a -1 sentinel.
namespace sslsteps
{
    inline const std::vector<double>& ratioValues()   { static const std::vector<double> v { 2.0, 4.0, 10.0 }; return v; }
    inline const juce::StringArray&   ratioLabels()   { static const juce::StringArray a { "2:1", "4:1", "10:1" }; return a; }

    inline const std::vector<double>& attackValues()  { static const std::vector<double> v { 0.1, 0.3, 1.0, 3.0, 10.0, 30.0 }; return v; }
    inline const juce::StringArray&   attackLabels()  { static const juce::StringArray a { "0.1 ms", "0.3 ms", "1 ms", "3 ms", "10 ms", "30 ms" }; return a; }

    // -1 = Auto (engine reads compReleaseAuto, ignores compReleaseMs).
    inline const std::vector<double>& releaseValues() { static const std::vector<double> v { 100.0, 300.0, 600.0, 1200.0, -1.0 }; return v; }
    inline const juce::StringArray&   releaseLabels() { static const juce::StringArray a { "0.1 s", "0.3 s", "0.6 s", "1.2 s", "AUTO" }; return a; }
}
} // namespace duskstudio
