#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include "DimOverlay.h"

namespace focal
{
// Small helper that wraps the recurring "DimOverlay + centred panel"
// pattern used by the piano roll, tuner, and (post-conversion) audio
// settings / mixdown / bounce / channel plugin editor.
//
// One ModalHost member on the host component. show() takes ownership of
// the panel body, displays the dim overlay + body sized to the host, and
// wires Esc / click-outside dismiss. close() tears it all down. The panel
// body can call host.closeModal() through an onDismiss callback when it
// has its own Cancel/Close button.
//
// The body is sized to its current `getWidth()` / `getHeight()` and
// centred. Pre-sizing on the body before calling show() is the caller's
// job - matches how juce::DialogWindow already worked.
//
// EmbeddedModal paints its own opaque rounded-panel backdrop behind the
// body so panels that don't fill their own background (e.g. raw juce::
// Component subclasses without paint() / setOpaque) still render solid
// instead of letting the channel strips bleed through.
class EmbeddedModal final : private juce::KeyListener
{
public:
    EmbeddedModal()  = default;
    ~EmbeddedModal() override { close(); }

    // Show `body` centred over `parent`. The modal takes ownership of
    // `body`; closing the modal destructs it.
    //
    // Click on the dim or Esc keypress fires onDismiss (which usually
    // just calls close()). Caller may chain teardown logic onto
    // onDismiss for things like clearing session state when the modal
    // closes.
    void show (juce::Component& parent,
               std::unique_ptr<juce::Component> body,
               std::function<void()> onDismiss = {},
               bool dismissOnClickOutside = true)
    {
        close();
        host = &parent;
        body_ = std::move (body);
        dim_ = std::make_unique<DimOverlay>();
        dim_->setBounds (parent.getLocalBounds());
        userOnDismiss = std::move (onDismiss);
        if (dismissOnClickOutside)
            dim_->onClick = [this]
            {
                if (userOnDismiss) userOnDismiss();
                else close();
            };
        // else: leave dim_->onClick null - click on the dim is a no-op,
        // forcing the user to explicit-action the body (Cancel / Import).
        parent.addAndMakeVisible (dim_.get());

        const auto bounds = parent.getLocalBounds();
        const int w = juce::jmax (1, body_->getWidth());
        const int h = juce::jmax (1, body_->getHeight());
        const auto bodyBounds = bounds.withSizeKeepingCentre (
            juce::jmin (w, bounds.getWidth()  - 16),
            juce::jmin (h, bounds.getHeight() - 16));

        // Backdrop sized slightly larger than the body so its rounded
        // corners frame the panel. Added BEFORE the body so the body
        // paints on top of it.
        backdrop_ = std::make_unique<Backdrop>();
        backdrop_->setBounds (bodyBounds.expanded (kBackdropMargin));
        parent.addAndMakeVisible (backdrop_.get());

        body_->setBounds (bodyBounds);
        parent.addAndMakeVisible (body_.get());
        body_->setWantsKeyboardFocus (true);
        body_->grabKeyboardFocus();
        body_->addKeyListener (this);
    }

    // Show a body the modal does NOT own. The caller keeps the body
    // alive across show / close cycles. Used for plugin editors -
    // tearing down a plugin's editor window on every close races the
    // host WM and (on XWayland with OpenGL-heavy plugin GUIs) can crash
    // the compositor. Keeping the editor alive and only adding /
    // removing it as a child is significantly more stable.
    //
    // Same Esc / click-outside semantics as the owning show().
    void showBorrowed (juce::Component& parent,
                       juce::Component& body,
                       std::function<void()> onDismiss = {})
    {
        close();
        host = &parent;
        borrowedBody_ = &body;
        dim_ = std::make_unique<DimOverlay>();
        dim_->setBounds (parent.getLocalBounds());
        userOnDismiss = std::move (onDismiss);
        dim_->onClick = [this]
        {
            if (userOnDismiss) userOnDismiss();
            else close();
        };
        parent.addAndMakeVisible (dim_.get());

        const auto bounds = parent.getLocalBounds();
        const int w = juce::jmax (1, body.getWidth());
        const int h = juce::jmax (1, body.getHeight());
        const auto bodyBounds = bounds.withSizeKeepingCentre (
            juce::jmin (w, bounds.getWidth()  - 16),
            juce::jmin (h, bounds.getHeight() - 16));

        backdrop_ = std::make_unique<Backdrop>();
        backdrop_->setBounds (bodyBounds.expanded (kBackdropMargin));
        parent.addAndMakeVisible (backdrop_.get());

        body.setBounds (bodyBounds);
        parent.addAndMakeVisible (&body);
        body.setWantsKeyboardFocus (true);
        body.grabKeyboardFocus();
        body.addKeyListener (this);
    }

    // Tear-down. Idempotent. Safe to call when nothing is open.
    // For owning show(), destructs the body. For showBorrowed(), just
    // removes the body from the parent without destructing it.
    //
    // Safe to call from inside a callback owned by the body (e.g. a
    // button's onClick). The synchronous path detaches the body /
    // backdrop / dim from the parent (so the user sees the modal
    // disappear immediately), then defers ~Body to the next message-
    // loop tick via callAsync. Without that defer, body_.reset() would
    // run ~Button → ~std::function while the button's onClick lambda is
    // still on the stack — a use-after-free that has been observed to
    // corrupt JUCE's message-thread state and trigger compositor
    // crashes on the next X11 round-trip.
    void close()
    {
        if (host == nullptr) return;
        if (body_         != nullptr) host->removeChildComponent (body_.get());
        if (borrowedBody_ != nullptr)
        {
            borrowedBody_->removeKeyListener (this);
            host->removeChildComponent (borrowedBody_);
        }
        if (backdrop_ != nullptr) host->removeChildComponent (backdrop_.get());
        if (dim_      != nullptr) host->removeChildComponent (dim_.get());

        if (body_ != nullptr)
        {
            // Hand ownership to a callAsync lambda - the body destructs
            // when that lambda is invoked (and then destroyed) on the
            // next message-loop tick, AFTER the current button-callback
            // stack has fully unwound. We don't capture `this` because
            // the EmbeddedModal itself may have been destructed by the
            // time the message-loop processes the call (e.g. on app
            // shutdown). Self-contained ownership = safe across every
            // teardown order.
            //
            // shared_ptr (not unique_ptr) so the capturing lambda is
            // copyable - std::function requires copy-constructible
            // callables on libc++ (macOS).
            std::shared_ptr<juce::Component> trash (body_.release());
            juce::MessageManager::callAsync (
                [trash]() mutable { (void) trash; });
        }
        borrowedBody_ = nullptr;
        backdrop_.reset();
        dim_.reset();
        host = nullptr;
        userOnDismiss = {};
    }

    bool isOpen() const noexcept { return body_ != nullptr || borrowedBody_ != nullptr; }

    // Body access for typed callers - useful when a host needs to push
    // updates into the panel (e.g. polling a meter). Returns nullptr
    // when nothing is open.
    juce::Component* getBody() const noexcept
    {
        return body_ != nullptr ? body_.get() : borrowedBody_;
    }

private:
    bool keyPressed (const juce::KeyPress& k, juce::Component*) override
    {
        if (k == juce::KeyPress::escapeKey)
        {
            if (userOnDismiss) userOnDismiss();
            else close();
            return true;
        }
        return false;
    }

    // Opaque rounded panel painted behind the body. Sized via
    // bodyBounds.expanded(kBackdropMargin) so its corners frame the body.
    class Backdrop final : public juce::Component
    {
    public:
        Backdrop() { setOpaque (false); setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics& g) override
        {
            const auto r = getLocalBounds().toFloat();
            g.setColour (juce::Colour (0xff141418).withAlpha (0.55f));
            g.fillRoundedRectangle (r.translated (0.0f, 4.0f), 8.0f);
            g.setColour (juce::Colour (0xff202024));
            g.fillRoundedRectangle (r, 8.0f);
            g.setColour (juce::Colour (0xff3a3a42));
            g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);
        }
    };

    static constexpr int kBackdropMargin = 6;

    juce::Component* host = nullptr;
    std::unique_ptr<DimOverlay> dim_;
    std::unique_ptr<Backdrop> backdrop_;
    std::unique_ptr<juce::Component> body_;
    juce::Component* borrowedBody_ = nullptr;
    std::function<void()> userOnDismiss;
};
} // namespace focal
