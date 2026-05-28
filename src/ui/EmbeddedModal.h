#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include "DimOverlay.h"

namespace duskstudio
{
// Wraps the "DimOverlay + centred panel" pattern used by piano roll,
// tuner, audio settings, mixdown, bounce, plugin editor.
//
// show() takes ownership, sizes the body to its current getWidth/Height,
// centres it, wires Esc / click-outside dismiss. close() tears down.
// Body can call host.closeModal() via onDismiss if it has its own
// Cancel button.
//
// Paints its own opaque rounded backdrop behind the body so panels
// without solid background don't bleed channel strips through.
class EmbeddedModal final : private juce::KeyListener
{
public:
    EmbeddedModal()  = default;
    ~EmbeddedModal() override { close(); }

    // Modal takes ownership of body; close destructs it.
    //
    // dismissOnClickOutside=false : dim is a no-op (decision modals
    //   need explicit button).
    // dismissOnEscape=false : Esc swallowed (focus-locked decision
    //   modals like save-before-quit).
    // Defaults preserve "Esc + click-outside both dismiss".
    void show (juce::Component& parent,
               std::unique_ptr<juce::Component> body,
               std::function<void()> onDismiss = {},
               bool dismissOnClickOutside = true,
               bool dismissOnEscape = true)
    {
        close();
        host = &parent;
        body_ = std::move (body);
        dim_ = std::make_unique<DimOverlay>();
        dim_->setBounds (parent.getLocalBounds());
        userOnDismiss = std::move (onDismiss);
        escapeDismisses = dismissOnEscape;
        if (dismissOnClickOutside)
            dim_->onClick = [this]
            {
                // Local copy BEFORE invoking — the user's callback may
                // close() this modal, which resets userOnDismiss = {}
                // and destroys the closure (with captures) mid-call.
                // SIGABRT on Linux/XWayland without this.
                if (auto cb = userOnDismiss) cb();
                else                          close();
            };
        parent.addAndMakeVisible (dim_.get());

        const auto bounds = parent.getLocalBounds();
        const int w = juce::jmax (1, body_->getWidth());
        const int h = juce::jmax (1, body_->getHeight());
        const auto bodyBounds = bounds.withSizeKeepingCentre (
            juce::jmin (w, bounds.getWidth()  - 16),
            juce::jmin (h, bounds.getHeight() - 16));

        // Slightly larger than body so rounded corners frame the panel.
        // Added BEFORE body so body paints on top.
        backdrop_ = std::make_unique<Backdrop>();
        backdrop_->setBounds (bodyBounds.expanded (kBackdropMargin));
        parent.addAndMakeVisible (backdrop_.get());

        body_->setBounds (bodyBounds);
        parent.addAndMakeVisible (body_.get());
        // Force topmost — a stage swap that re-adds a fullscreen view
        // (AuxView, MasteringView) after the modal opens can demote it.
        // addAndMakeVisible alone is only topmost-at-add-time.
        dim_     ->toFront (false);
        backdrop_->toFront (false);
        body_    ->toFront (true);
        body_->setWantsKeyboardFocus (true);
        body_->grabKeyboardFocus();
        body_->addKeyListener (this);
    }

    // Body NOT owned — caller keeps alive across show/close cycles.
    // Used for plugin editors: tearing down a plugin's editor window on
    // every close races the WM and on XWayland with GL-heavy GUIs can
    // crash the compositor. Keeping the editor alive and add/removing
    // it as a child is significantly more stable.
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
            // See owning show()'s onClick — local copy survives close().
            if (auto cb = userOnDismiss) cb();
            else                          close();
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
        dim_     ->toFront (false);
        backdrop_->toFront (false);
        body.toFront (true);
        body.setWantsKeyboardFocus (true);
        body.grabKeyboardFocus();
        body.addKeyListener (this);
    }

    // Idempotent. For owning show, destructs body; for showBorrowed,
    // just removes from parent.
    //
    // CONTRACT: close() does NOT invoke userOnDismiss. Only Esc and
    // dim-click do. Helpers (DuskAlerts' OK button) rely on this —
    // they wire their action button to close() AND their follow-up,
    // trusting close() won't double-fire. Audit every caller before
    // changing this contract.
    //
    // Safe from inside a body's callback (button onClick): synchronous
    // path detaches body/backdrop/dim from parent so the modal
    // disappears immediately, then defers destruction to the next
    // message-loop tick via callAsync. Without that, body_.reset()
    // would run ~Button -> ~std::function while the button's onClick
    // lambda is still on the stack — observed to corrupt JUCE's
    // message-thread state and crash compositors on next X11 round.
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
            // Hand to a callAsync lambda — destructs on the next tick
            // after the current button-callback stack unwinds. Don't
            // capture `this` — EmbeddedModal itself may be destructed
            // by then (app shutdown). shared_ptr (not unique) so the
            // capture is copyable — std::function needs copyable
            // callables on libc++ (macOS).
            std::shared_ptr<juce::Component> trash (body_.release());
            juce::MessageManager::callAsync (
                [trash]() mutable { (void) trash; });
        }
        borrowedBody_ = nullptr;

        // Defer dim + backdrop the same way. close() can be invoked
        // from inside dim->onClick (DuskComboBox); synchronously
        // destroying the dim while its handler is on the stack is a
        // UAF that SIGABRTs on Linux/XWayland.
        if (backdrop_ != nullptr)
        {
            std::shared_ptr<juce::Component> trash (backdrop_.release());
            juce::MessageManager::callAsync (
                [trash]() mutable { (void) trash; });
        }
        if (dim_ != nullptr)
        {
            std::shared_ptr<juce::Component> trash (dim_.release());
            juce::MessageManager::callAsync (
                [trash]() mutable { (void) trash; });
        }
        // Restore keyboard focus to top-level (MainComponent owns the
        // transport-key handler). Deferred so this runs AFTER the
        // trash lambdas tear the modal down — grabbing focus while a
        // soon-to-be-destroyed child holds it is unsafe. Without this,
        // every popup leaves focus orphaned and spacebar / R silently
        // die until the user clicks the UI.
        if (auto* top = host->getTopLevelComponent())
        {
            juce::Component::SafePointer<juce::Component> safeTop (top);
            juce::MessageManager::callAsync ([safeTop]() mutable
            {
                if (auto* c = safeTop.getComponent())
                    c->grabKeyboardFocus();
            });
        }
        host = nullptr;
        userOnDismiss = {};
    }

    bool isOpen() const noexcept { return body_ != nullptr || borrowedBody_ != nullptr; }

    juce::Component* getBody() const noexcept
    {
        return body_ != nullptr ? body_.get() : borrowedBody_;
    }

    // DuskContextMenu / DuskComboBox call this AFTER show() to anchor
    // to a click/control. Backdrop moves with the body so it doesn't
    // render as a stray blank panel where the body used to be. dim_
    // intentionally stays full-window for click-outside coverage.
    void repositionBody (juce::Point<int> topLeftInParent)
    {
        auto* body = getBody();
        if (body == nullptr) return;
        body->setTopLeftPosition (topLeftInParent);
        if (backdrop_ != nullptr)
            backdrop_->setTopLeftPosition (topLeftInParent.x - kBackdropMargin,
                                              topLeftInParent.y - kBackdropMargin);
    }

private:
    bool keyPressed (const juce::KeyPress& k, juce::Component*) override
    {
        if (k == juce::KeyPress::escapeKey)
        {
            if (! escapeDismisses) return true;  // swallow on focus-locked modals
            // Local copy keeps the closure alive across close().
            if (auto cb = userOnDismiss) cb();
            else                          close();
            return true;
        }
        // Forward global transport hotkeys (spacebar, R) to top-level
        // regardless of borrowed/owned. Modal bodies grab keyboard
        // focus on display, so without this hop the keys would
        // silently die. TextEditor children consume space before this
        // ever fires, so typing isn't affected.
        const bool isTransportKey = k == juce::KeyPress::spaceKey
                                     || k.getKeyCode() == 'R'
                                     || k.getKeyCode() == 'r';
        if (isTransportKey && host != nullptr)
        {
            if (auto* top = host->getTopLevelComponent())
                return top->keyPressed (k);
        }
        return false;
    }

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
    bool escapeDismisses = true;
};
} // namespace duskstudio
