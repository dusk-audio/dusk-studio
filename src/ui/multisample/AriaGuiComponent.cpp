#include "AriaGuiComponent.h"
#include "../DuskComboBox.h"
#include "../../engine/multisample/DuskMultisampleProcessor.h"

namespace duskstudio
{
// Knob LookAndFeel that blits one frame of a vertical filmstrip
// image. Skips JUCE's default rotary draw entirely.
struct AriaGuiComponent::FilmstripKnobLAF : public juce::LookAndFeel_V4
{
    FilmstripKnobLAF(juce::Image strip, int frameCount)
        : strip_(std::move(strip))
        , frames_(juce::jmax(1, frameCount))
    {
        // Detect frame orientation by aspect: vertical strip has
        // height / frames ~= width; horizontal has width / frames ~=
        // height. Default vertical (the ARIA convention).
        if (strip_.isValid())
        {
            const auto sw = strip_.getWidth();
            const auto sh = strip_.getHeight();
            // Heuristic: closer to square per-frame when vertical, so
            // pick vertical when h/frames is closer to w than w/frames
            // is to h.
            const float vErr = std::abs((float) sh / (float) frames_ - (float) sw);
            const float hErr = std::abs((float) sw / (float) frames_ - (float) sh);
            vertical_ = vErr <= hErr;
            if (vertical_)
            {
                frameW_ = sw;
                frameH_ = sh / frames_;
            }
            else
            {
                frameW_ = sw / frames_;
                frameH_ = sh;
            }
        }
    }

    void drawRotarySlider(juce::Graphics& g,
                           int x, int y, int width, int height,
                           float sliderPosProportional,
                           float /*rotaryStartAngle*/,
                           float /*rotaryEndAngle*/,
                           juce::Slider& /*slider*/) override
    {
        if (! strip_.isValid() || frameW_ <= 0 || frameH_ <= 0)
            return;
        const int idx = juce::jlimit(0, frames_ - 1,
                                       (int) std::round(sliderPosProportional * (float) (frames_ - 1)));
        const int sx = vertical_ ? 0 : idx * frameW_;
        const int sy = vertical_ ? idx * frameH_ : 0;

        // Scale the frame into the bounds while preserving aspect.
        const float scale = juce::jmin((float) width  / (float) frameW_,
                                         (float) height / (float) frameH_);
        const int dw = (int) std::round((float) frameW_ * scale);
        const int dh = (int) std::round((float) frameH_ * scale);
        const int dx = x + (width  - dw) / 2;
        const int dy = y + (height - dh) / 2;

        g.drawImage(strip_,
                     dx, dy, dw, dh,
                     sx, sy, frameW_, frameH_);
    }

    juce::Image strip_;
    int  frames_   { 1 };
    int  frameW_   { 0 };
    int  frameH_   { 0 };
    bool vertical_ { true };
};

// Linear fader: paint bg image full-track, draw handle image at the
// slider position. handle is rendered with its native dimensions.
struct AriaGuiComponent::FilmstripFaderLAF : public juce::LookAndFeel_V4
{
    FilmstripFaderLAF(juce::Image bg, juce::Image handle)
        : bg_(std::move(bg)), handle_(std::move(handle)) {}

    void drawLinearSlider(juce::Graphics& g,
                           int x, int y, int width, int height,
                           float sliderPos,
                           float minSliderPos,
                           float maxSliderPos,
                           const juce::Slider::SliderStyle style,
                           juce::Slider& /*slider*/) override
    {
        juce::ignoreUnused(minSliderPos, maxSliderPos);
        if (bg_.isValid())
        {
            // Background sits centred along the long axis.
            const auto bw = bg_.getWidth();
            const auto bh = bg_.getHeight();
            const int bx = x + (width  - bw) / 2;
            const int by = y + (height - bh) / 2;
            g.drawImageAt(bg_, bx, by);
        }
        if (handle_.isValid())
        {
            const auto hw = handle_.getWidth();
            const auto hh = handle_.getHeight();
            int hx = x + (width - hw) / 2;
            int hy = y + (height - hh) / 2;
            if (style == juce::Slider::LinearVertical)
                hy = (int) std::round(sliderPos - (float) hh * 0.5f);
            else
                hx = (int) std::round(sliderPos - (float) hw * 0.5f);
            g.drawImageAt(handle_, hx, hy);
        }
    }

    juce::Image bg_, handle_;
};

// Flat ComboBox LookAndFeel - dark fill, white text, simple chevron.
// Matches the Sforzando aesthetic; default JUCE chrome looks out of
// place over the ARIA background image.
namespace
{
struct FlatAriaComboLAF : public juce::LookAndFeel_V4
{
    void drawComboBox(juce::Graphics& g, int width, int height,
                       bool /*isButtonDown*/,
                       int /*buttonX*/, int /*buttonY*/,
                       int /*buttonW*/, int /*buttonH*/,
                       juce::ComboBox& box) override
    {
        const auto bounds = juce::Rectangle<int>(0, 0, width, height).toFloat();
        g.setColour(juce::Colour(0xcc101014));
        g.fillRoundedRectangle(bounds, 2.0f);
        g.setColour(juce::Colour(0x80ffffff));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 2.0f, 1.0f);

        // Chevron.
        const float cw = 7.0f, ch = 4.0f;
        const float cx = (float) width  - cw - 4.0f;
        const float cy = ((float) height - ch) * 0.5f;
        juce::Path p;
        p.startNewSubPath(cx, cy);
        p.lineTo(cx + cw * 0.5f, cy + ch);
        p.lineTo(cx + cw,        cy);
        g.setColour(box.findColour(juce::ComboBox::arrowColourId)
                       .withAlpha(0.9f));
        g.strokePath(p, juce::PathStrokeType(1.2f));
    }

    juce::Font getComboBoxFont(juce::ComboBox& /*box*/) override
    {
        return juce::Font(juce::FontOptions(11.0f));
    }

    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds(4, 0, box.getWidth() - 16, box.getHeight());
        label.setFont(getComboBoxFont(box));
        label.setColour(juce::Label::textColourId, juce::Colours::white);
    }
};

FlatAriaComboLAF& sharedFlatComboLAF()
{
    static FlatAriaComboLAF laf;
    return laf;
}
}

AriaGuiComponent::AriaGuiComponent(DuskMultisampleProcessor& proc, AriaGuiDoc doc)
    : processor_(proc), doc_(std::move(doc))
{
    setSize(doc_.width, doc_.height);
    buildChildren();
}

AriaGuiComponent::~AriaGuiComponent()
{
    // Detach LAFs from any slider / combobox still around so JUCE
    // doesn't dereference them during child destruction.
    for (auto& c : children_)
    {
        if (auto* s = dynamic_cast<juce::Slider*>(c.get()))
            s->setLookAndFeel(nullptr);
        else if (auto* cb = dynamic_cast<juce::ComboBox*>(c.get()))
            cb->setLookAndFeel(nullptr);
    }
}

juce::Image& AriaGuiComponent::loadImageCached(const juce::String& relPath)
{
    auto it = imageCache_.find(relPath);
    if (it != imageCache_.end())
        return it->second;

    auto file = doc_.resourceDir.getChildFile(relPath);
    auto img  = juce::ImageFileFormat::loadFrom(file);
    auto [ins, _] = imageCache_.emplace(relPath, std::move(img));
    return ins->second;
}

void AriaGuiComponent::buildChildren()
{
    for (const auto& w : doc_.widgets)
    {
        switch (w.kind)
        {
            case AriaWidgetKind::StaticImage:
            {
                struct ImgComp : public juce::Component
                {
                    juce::Image img;
                    void paint(juce::Graphics& g) override
                    {
                        if (img.isValid())
                            g.drawImage(img, getLocalBounds().toFloat());
                    }
                };
                auto c = std::make_unique<ImgComp>();
                c->img = loadImageCached(w.image);
                c->setBounds(w.bounds);
                c->setInterceptsMouseClicks(false, false);
                addAndMakeVisible(*c);
                // StaticImage is the background - keep it at the back.
                c->toBack();
                children_.push_back(std::move(c));
                break;
            }
            case AriaWidgetKind::StaticText:
            {
                auto lbl = std::make_unique<juce::Label>(juce::String{}, w.text);
                lbl->setBounds(w.bounds);
                lbl->setColour(juce::Label::textColourId, w.textColour);
                lbl->setJustificationType(juce::Justification::centred);
                lbl->setInterceptsMouseClicks(false, false);
                // ARIA labels are typically small; pick a sane default
                // matching the bounds height.
                const int fontH = juce::jmax(10, w.bounds.getHeight() - 4);
                lbl->setFont(juce::Font(juce::FontOptions((float) fontH)));
                addAndMakeVisible(*lbl);
                children_.push_back(std::move(lbl));
                break;
            }
            case AriaWidgetKind::Slider:
            {
                auto s = std::make_unique<juce::Slider>();
                s->setRange(0.0, 1.0, 0.0);
                s->setValue(0.5, juce::dontSendNotification);
                s->setSliderStyle(w.orient == AriaOrientation::Vertical
                                    ? juce::Slider::LinearVertical
                                    : juce::Slider::LinearHorizontal);
                s->setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
                s->setWantsKeyboardFocus(false);
                if (w.imageBg.isNotEmpty() || w.imageHandle.isNotEmpty())
                {
                    // Filmstrip replaces the default look entirely - blank
                    // the stock track/thumb colours so nothing bleeds through.
                    s->setColour(juce::Slider::backgroundColourId, juce::Colours::transparentBlack);
                    s->setColour(juce::Slider::trackColourId,      juce::Colours::transparentBlack);
                    const auto key = w.imageBg + "|" + w.imageHandle;
                    auto it = faderLAFs_.find(key);
                    if (it == faderLAFs_.end())
                        it = faderLAFs_.emplace(key, std::make_unique<FilmstripFaderLAF>(
                                                       loadImageCached(w.imageBg),
                                                       loadImageCached(w.imageHandle))).first;
                    s->setLookAndFeel(it->second.get());
                }
                // else: keep JUCE's default linear look (auto-skin path).
                bindControl(*s, w.paramCC);
                s->setBounds(w.bounds);
                addAndMakeVisible(*s);
                children_.push_back(std::move(s));
                break;
            }
            case AriaWidgetKind::Knob:
            {
                auto s = std::make_unique<juce::Slider>();
                s->setRange(0.0, 1.0, 0.0);
                s->setValue(0.5, juce::dontSendNotification);
                s->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
                s->setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
                s->setWantsKeyboardFocus(false);
                if (w.image.isNotEmpty())
                {
                    // Filmstrip replaces the default rotary - blank the
                    // stock fill/outline/thumb so they don't bleed through.
                    s->setColour(juce::Slider::rotarySliderFillColourId,    juce::Colours::transparentBlack);
                    s->setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colours::transparentBlack);
                    s->setColour(juce::Slider::thumbColourId,      juce::Colours::transparentBlack);
                    auto& img = loadImageCached(w.image);
                    auto  key = w.image + "@" + juce::String(w.frames);
                    auto  it  = knobLAFs_.find(key);
                    if (it == knobLAFs_.end())
                        it = knobLAFs_.emplace(key, std::make_unique<FilmstripKnobLAF>(
                                                       img, w.frames)).first;
                    s->setLookAndFeel(it->second.get());
                }
                // else: keep JUCE's default rotary (auto-skin path).
                bindControl(*s, w.paramCC);
                // Knobs in ARIA usually omit w/h; derive from filmstrip
                // frame size when the XML didn't supply them. Detect
                // horizontal vs vertical strips the same way the
                // FilmstripKnobLAF does so the bounds match the asset.
                juce::Rectangle<int> b = w.bounds;
                if ((b.getWidth() == 0 || b.getHeight() == 0) && w.image.isNotEmpty())
                {
                    auto&     img    = loadImageCached(w.image);
                    const int iw     = img.getWidth();
                    const int ih     = img.getHeight();
                    const int frames = juce::jmax(1, w.frames);
                    int fw, fh;
                    if (frames > 1 && iw > ih)   // horizontal strip
                    {
                        fw = iw / frames;
                        fh = ih;
                    }
                    else                          // vertical strip (ARIA default)
                    {
                        fw = iw;
                        fh = ih / frames;
                    }
                    b.setSize(juce::jmax(20, fw), juce::jmax(20, fh));
                }
                // Malformed widget (no size + no filmstrip): give it a
                // sane default so it's still grabbable.
                if (b.getWidth() == 0 || b.getHeight() == 0)
                    b.setSize(juce::jmax(20, b.getWidth()), juce::jmax(20, b.getHeight()));
                s->setBounds(b);
                addAndMakeVisible(*s);
                children_.push_back(std::move(s));
                break;
            }
            case AriaWidgetKind::CommandButton:
            {
                auto& img = loadImageCached(w.image);
                auto b = std::make_unique<juce::ImageButton>();
                b->setImages(false, true, true,
                              img, 1.0f, {},
                              img, 0.85f, {},
                              img, 1.0f, juce::Colours::white.withAlpha(0.2f));
                b->setBounds(w.bounds);
                if (w.command == "launch_url")
                {
                    const auto url = w.data0;
                    b->onClick = [url] { juce::URL(url).launchInDefaultBrowser(); };
                }
                addAndMakeVisible(*b);
                children_.push_back(std::move(b));
                break;
            }
            case AriaWidgetKind::OptionMenu:
            {
                auto cb = std::make_unique<DuskComboBox>();
                for (size_t i = 0; i < w.options.size(); ++i)
                    cb->addItem(w.options[i].text, (int) i + 1);
                // Initial selection: the option whose normalized value is
                // closest to the CC's current value (restored / default),
                // else the first item.
                {
                    int sel = 1;
                    const float cur = processor_.getHDCC(w.paramCC);
                    if (cur >= 0.0f && ! w.options.empty())
                    {
                        float best = 1.0e9f;
                        for (size_t i = 0; i < w.options.size(); ++i)
                        {
                            const float d = std::abs(w.options[i].value - cur);
                            if (d < best) { best = d; sel = (int) i + 1; }
                        }
                    }
                    if (! w.options.empty())
                        cb->setSelectedId(sel, juce::dontSendNotification);
                }
                {
                    auto* raw = cb.get();
                    const int cc = w.paramCC;
                    const auto opts = w.options;
                    raw->onChange = [this, raw, cc, opts]
                    {
                        const int idx = raw->getSelectedId() - 1;
                        if (cc >= 0 && idx >= 0 && idx < (int) opts.size())
                            processor_.setHDCC(cc, opts[(size_t) idx].value);
                    };
                }
                cb->setLookAndFeel(&sharedFlatComboLAF());
                cb->setColour(juce::ComboBox::backgroundColourId, juce::Colours::transparentBlack);
                cb->setColour(juce::ComboBox::outlineColourId,    juce::Colours::transparentBlack);
                cb->setColour(juce::ComboBox::textColourId,       juce::Colours::white);
                cb->setColour(juce::ComboBox::arrowColourId,      juce::Colours::white);
                cb->setBounds(w.bounds);
                addAndMakeVisible(*cb);
                children_.push_back(std::move(cb));
                break;
            }
            case AriaWidgetKind::Unknown:
            default:
            {
                // Placeholder: a thin outline + the tag name centred.
                // Lets a developer eyeball which ARIA tags this MVP
                // doesn't render yet.
                struct PlaceholderComp : public juce::Component
                {
                    juce::String tag;
                    void paint(juce::Graphics& g) override
                    {
                        g.setColour(juce::Colours::orange.withAlpha(0.4f));
                        g.drawRect(getLocalBounds(), 1);
                        g.setColour(juce::Colours::orange);
                        g.setFont(juce::Font(juce::FontOptions(10.0f)));
                        g.drawText(tag, getLocalBounds(),
                                    juce::Justification::centred, true);
                    }
                };
                auto c = std::make_unique<PlaceholderComp>();
                c->tag = w.tagName;
                c->setBounds(w.bounds);
                addAndMakeVisible(*c);
                children_.push_back(std::move(c));
                break;
            }
        }
    }
}

void AriaGuiComponent::bindControl(juce::Slider& s, int cc)
{
    s.setRange(0.0, 1.0, 0.0);
    // Initial position: the CC's current value (restored / default) or
    // the centre when the UI hasn't set it yet.
    const float cur = processor_.getHDCC(cc);
    s.setValue(cur >= 0.0f ? (double) cur : 0.5, juce::dontSendNotification);
    if (cc >= 0)
    {
        auto* raw = &s;
        s.onValueChange = [this, raw, cc] { processor_.setHDCC(cc, (float) raw->getValue()); };
    }
}

void AriaGuiComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1a1a20));
}

void AriaGuiComponent::resized()
{
    // Widgets carry absolute coordinates from the XML; nothing to do.
    // (If we add a scale transform later, apply it here.)
}
} // namespace duskstudio
