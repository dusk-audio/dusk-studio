#include "AriaGui.h"

#include <algorithm>

namespace duskstudio
{
namespace
{
juce::Rectangle<int> readBounds(const juce::XmlElement& el)
{
    return { el.getIntAttribute("x"),
             el.getIntAttribute("y"),
             el.getIntAttribute("w"),
             el.getIntAttribute("h") };
}

juce::Colour parseAriaColour(const juce::String& s, juce::Colour fallback)
{
    // ARIA writes "#RRGGBBAA" (alpha LAST, e.g. #000000FF = opaque
    // black, #C6A24EFF = opaque gold). juce::Colour::fromString wants
    // "AARRGGBB", so move the trailing alpha pair to the front.
    if (s.startsWithChar('#') && s.length() == 9)
        return juce::Colour::fromString(s.substring(7) + s.substring(1, 7));
    // Bare 8-hex with no '#': assume already AARRGGBB.
    if (s.length() == 8)
        return juce::Colour::fromString(s);
    return fallback;
}

AriaWidget makeStaticImage(const juce::XmlElement& el)
{
    AriaWidget w;
    w.kind        = AriaWidgetKind::StaticImage;
    w.tagName     = el.getTagName();
    w.bounds      = readBounds(el);
    w.image       = el.getStringAttribute("image");
    w.transparent = el.getIntAttribute("transparent", 0) != 0;
    return w;
}

AriaWidget makeStaticText(const juce::XmlElement& el)
{
    AriaWidget w;
    w.kind        = AriaWidgetKind::StaticText;
    w.tagName     = el.getTagName();
    w.bounds      = readBounds(el);
    w.text        = el.getStringAttribute("text");
    w.textColour  = parseAriaColour(el.getStringAttribute("color_text"),
                                     juce::Colours::white);
    w.transparent = el.getIntAttribute("transparent", 0) != 0;
    return w;
}

AriaWidget makeSlider(const juce::XmlElement& el)
{
    AriaWidget w;
    w.kind        = AriaWidgetKind::Slider;
    w.tagName     = el.getTagName();
    w.bounds      = readBounds(el);
    w.paramCC     = el.getIntAttribute("param", -1);
    w.imageHandle = el.getStringAttribute("image_handle");
    w.imageBg     = el.getStringAttribute("image_bg");
    const auto o  = el.getStringAttribute("orientation").toLowerCase();
    w.orient      = (o == "horizontal") ? AriaOrientation::Horizontal
                                         : AriaOrientation::Vertical;
    return w;
}

AriaWidget makeKnob(const juce::XmlElement& el)
{
    AriaWidget w;
    w.kind     = AriaWidgetKind::Knob;
    w.tagName  = el.getTagName();
    // Knobs in ARIA XML usually omit w/h - bounds derive from the
    // filmstrip frame size at render time. Honour explicit w/h when
    // present.
    w.bounds   = readBounds(el);
    w.paramCC  = el.getIntAttribute("param", -1);
    w.image    = el.getStringAttribute("image");
    w.frames   = std::max(1, el.getIntAttribute("frames", 1));
    return w;
}

AriaWidget makeCommandButton(const juce::XmlElement& el)
{
    AriaWidget w;
    w.kind     = AriaWidgetKind::CommandButton;
    w.tagName  = el.getTagName();
    w.bounds   = readBounds(el);
    w.image    = el.getStringAttribute("image");
    w.command  = el.getStringAttribute("command");
    w.data0    = el.getStringAttribute("data0");
    return w;
}

AriaWidget makeOptionMenu(const juce::XmlElement& el)
{
    AriaWidget w;
    w.kind     = AriaWidgetKind::OptionMenu;
    w.tagName  = el.getTagName();
    w.bounds   = readBounds(el);
    w.paramCC  = el.getIntAttribute("param", -1);

    for (auto* child : el.getChildIterator())
    {
        if (! child->hasTagName("OptionItem"))
            continue;
        AriaOption opt;
        // ARIA OptionItem uses `name=` for display label (Sforzando spec).
        // Some libraries also write `text=` - accept either.
        opt.text  = child->getStringAttribute("name");
        if (opt.text.isEmpty())
            opt.text = child->getStringAttribute("text");
        // value is a normalized float 0..1 in Sforzando-style menus.
        opt.value = (float) child->getDoubleAttribute("value", 0.0);
        w.options.push_back(std::move(opt));
    }
    return w;
}

AriaWidget makeUnknown(const juce::XmlElement& el)
{
    AriaWidget w;
    w.kind    = AriaWidgetKind::Unknown;
    w.tagName = el.getTagName();
    w.bounds  = readBounds(el);
    return w;
}

AriaWidget parseWidget(const juce::XmlElement& el)
{
    const auto& tag = el.getTagName();
    if (tag == "StaticImage")    return makeStaticImage   (el);
    if (tag == "StaticText")     return makeStaticText    (el);
    if (tag == "Slider")         return makeSlider        (el);
    if (tag == "Knob")           return makeKnob          (el);
    if (tag == "CommandButton")  return makeCommandButton (el);
    if (tag == "OptionMenu")     return makeOptionMenu    (el);
    return makeUnknown(el);
}
}

std::optional<AriaGuiDoc> AriaGuiDoc::parse(const juce::File& xmlFile)
{
    if (! xmlFile.existsAsFile())
        return std::nullopt;

    auto xml = juce::parseXML(xmlFile);
    if (xml == nullptr)
        return std::nullopt;

    // Root must be <GUI w=".." h="..">.
    if (! xml->hasTagName("GUI"))
        return std::nullopt;

    AriaGuiDoc doc;
    doc.sourceFile  = xmlFile;
    doc.resourceDir = xmlFile.getParentDirectory();
    doc.width       = xml->getIntAttribute("w");
    doc.height      = xml->getIntAttribute("h");
    if (doc.width <= 0 || doc.height <= 0)
        return std::nullopt;

    doc.widgets.reserve((size_t) xml->getNumChildElements());
    for (auto* child : xml->getChildIterator())
        doc.widgets.push_back(parseWidget(*child));

    return doc;
}

AriaGuiDoc AriaGuiDoc::buildAutoSkin(const juce::File& bgImage,
                                      const std::vector<std::pair<int, juce::String>>& ccLabels)
{
    AriaGuiDoc doc;
    doc.resourceDir = bgImage.getParentDirectory();
    doc.sourceFile  = bgImage;

    int imgW = 0, imgH = 0;
    if (bgImage.existsAsFile())
    {
        auto img = juce::ImageFileFormat::loadFrom(bgImage);
        if (img.isValid()) { imgW = img.getWidth(); imgH = img.getHeight(); }
    }

    // Knob grid geometry. Each cell: caption (16) above a 48px knob.
    constexpr int kCellW = 76, kKnobSz = 48, kCapH = 16, kCellH = kKnobSz + kCapH + 8;
    const int gridW   = std::max(imgW, 480);
    const int perRow  = std::max(1, gridW / kCellW);
    const int rows    = (int) ((ccLabels.size() + (size_t) perRow - 1) / (size_t) perRow);
    const int gridH   = rows * kCellH;

    doc.width  = std::max(gridW, imgW > 0 ? imgW : gridW);
    doc.height = (imgH > 0 ? imgH : 0) + gridH + 8;

    // Background image (if any) spanning the top.
    if (imgW > 0 && imgH > 0)
    {
        AriaWidget bg;
        bg.kind   = AriaWidgetKind::StaticImage;
        bg.image  = bgImage.getFileName();
        bg.bounds = { 0, 0, imgW, imgH };
        doc.widgets.push_back(std::move(bg));
    }

    const int gridTop = (imgH > 0 ? imgH : 0) + 4;
    for (size_t i = 0; i < ccLabels.size(); ++i)
    {
        const int col = (int) i % perRow;
        const int row = (int) i / perRow;
        const int cx  = col * kCellW + (kCellW - kKnobSz) / 2;
        const int cy  = gridTop + row * kCellH;

        AriaWidget cap;
        cap.kind       = AriaWidgetKind::StaticText;
        cap.text       = ccLabels[i].second;
        cap.textColour = juce::Colour(0xffd0d0d4);
        cap.bounds     = { col * kCellW, cy, kCellW, kCapH };
        doc.widgets.push_back(std::move(cap));

        AriaWidget knob;
        knob.kind    = AriaWidgetKind::Knob;
        knob.paramCC = ccLabels[i].first;
        knob.frames  = 1;                      // no filmstrip -> default rotary
        knob.bounds  = { cx, cy + kCapH, kKnobSz, kKnobSz };
        doc.widgets.push_back(std::move(knob));
    }

    return doc;
}
} // namespace duskstudio
