#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

#include <optional>
#include <utility>
#include <vector>

namespace duskstudio
{
// Plogue ARIA GUI XML widget model. Loosely follows the subset Karoryfer
// libraries (Swirly Drums) ship: StaticImage, StaticText, Slider, Knob,
// CommandButton, OptionMenu/OptionItem. Unknown tags survive parse so
// Phase 6 can stub them as labelled placeholders.
enum class AriaWidgetKind
{
    Unknown,
    StaticImage,
    StaticText,
    Slider,
    Knob,
    CommandButton,
    OptionMenu,
};

enum class AriaOrientation
{
    Vertical,
    Horizontal,
};

struct AriaOption
{
    juce::String text;
    float        value { 0.0f };   // normalized 0..1 CC value sent when picked
};

struct AriaWidget
{
    AriaWidgetKind          kind { AriaWidgetKind::Unknown };
    juce::String            tagName;     // verbatim XML tag (for Unknown reporting)
    juce::Rectangle<int>    bounds;      // x/y/w/h - for Knob, w/h may be 0 (filmstrip frame derives it)

    // Common-ish attributes. Not every field applies to every kind;
    // unused fields stay at default.
    juce::String            image;
    juce::String            imageBg;
    juce::String            imageHandle;
    juce::String            text;
    juce::Colour            textColour { juce::Colours::white };
    bool                    transparent { false };

    AriaOrientation         orient { AriaOrientation::Vertical };

    int                     paramCC { -1 };   // 0..511; -1 = unbound
    int                     frames  { 1 };    // filmstrip frame count for Knob

    juce::String            command;          // CommandButton.command
    juce::String            data0;            // CommandButton.data0 (e.g. URL)

    std::vector<AriaOption> options;          // OptionMenu items
};

struct AriaGuiDoc
{
    int                       width  { 0 };
    int                       height { 0 };
    std::vector<AriaWidget>   widgets;
    juce::File                resourceDir;    // directory the XML lives in; image paths resolve here
    juce::File                sourceFile;     // the XML file itself (for diagnostics)

    static std::optional<AriaGuiDoc> parse(const juce::File& xmlFile);

    // Synthesize a skin for a stock (non-ARIA) SFZ that declares a
    // <control> image= plus label_cc& entries: the image becomes the
    // background and each labelled CC gets an auto-placed knob + caption
    // laid out in a grid below it. Returns a doc with width/height sized
    // to fit. ccLabels = (cc number, caption) pairs.
    static AriaGuiDoc buildAutoSkin(const juce::File& bgImage,
                                     const std::vector<std::pair<int, juce::String>>& ccLabels);
};
} // namespace duskstudio
