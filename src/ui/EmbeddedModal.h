#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include "DimOverlay.h"

namespace duskstudio
{
// Global shortcuts that stay live while a modal / popup holds keyboard
// focus, forwarded to the registered MainComponent. Deliberately limited to
// transport + loop/punch + playhead navigation + window fullscreen: edit /
// clipboard / destructive keys (Delete, split, nudge, undo, save, marker, ...)
// are NOT forwarded because they would act on the arrangement hidden behind
// the modal — e.g. Delete silently removing a region the user can't even see.
// The loop/punch keys ARE forwarded so the engineer can set a loop and audition
// it while a comp / EQ / plugin editor is open (none of L / P / [ / ] has a
// Cmd-binding, so forwarding can't trip a destructive op). A focused child
// that wants one of these keys (TextEditor caret nav) consumes it first, so
// it never reaches the modal's listener and typing is unaffected.
inline bool isModalForwardableShortcut (const juce::KeyPress& k) noexcept
{
    // Bare transport keys only — a Cmd/Ctrl/Alt chord (e.g. Ctrl+R, Cmd+.)
    // must NOT be mistaken for the unmodified transport key, otherwise a
    // host shortcut would silently trigger record / loop / etc. The full
    // KeyPress comparisons below (spaceKey/homeKey/F11Key) already reject
    // modifiers; this guards the character-code checks the same way.
    const auto mods = k.getModifiers();
    if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown())
        return false;
    const int kc = k.getKeyCode();
    return k == juce::KeyPress::spaceKey                  // play / stop
        || kc == 'R' || kc == 'r'                         // record
        || k == juce::KeyPress::homeKey                   // playhead -> 0
        || k.getTextCharacter() == '.'                    // stop + rewind
        || kc == 'L' || kc == 'l'                         // loop on/off
        || kc == 'P' || kc == 'p'                         // punch on/off
        || kc == '[' || kc == ']'                         // set loop/punch in/out at playhead
        || k == juce::KeyPress::F11Key;
}

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
    // Singleton focus-restore target. MainComponent registers itself here
    // in its ctor; close() grabs focus on it after teardown. Bypasses
    // JUCE's default-focus-traverser on DocumentWindow (which on the
    // hybrid X11/Wayland setup doesn't reliably land focus on the
    // content component, so spacebar / R die after every popup pick).
    static juce::Component::SafePointer<juce::Component>& focusRestoreTarget()
    {
        static juce::Component::SafePointer<juce::Component> target;
        return target;
    }

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
               bool dismissOnEscape = true,
               float dimAlpha = 0.55f)   // processing editors pass kEditorDimAlpha
    {
        close();
        host = &parent;
        body_ = std::move (body);
        dim_ = std::make_unique<DimOverlay> (dimAlpha);
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

        hidePluginEditorsUnder (parent);
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
        // showBorrowed is plugin-editors-only — use the lighter editor dim so
        // the strip meters behind stay readable while auditioning.
        dim_ = std::make_unique<DimOverlay> (kEditorDimAlpha);
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

        hidePluginEditorsUnder (parent);
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

        // Restore any plugin editors we hid in show() before tearing
        // down the modal body. Done first so their setVisible(true)
        // doesn't fight with the message-loop teardown path below.
        restoreHiddenPluginEditors();

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
        // Restore keyboard focus. MainComponent registered itself with
        // focusRestoreTarget() in its ctor; we grab focus on it both
        // synchronously (so the very next key event already lands
        // there) AND deferred (so any focus stolen by JUCE's own modal
        // teardown / focus-traverser logic gets overridden once the
        // trash lambdas have run and the modal's children are gone).
        //
        // Without this, every popup pick leaves focus orphaned on the
        // DocumentWindow itself — JUCE's default focus traverser does
        // NOT reliably drill into the content component on this hybrid
        // X11/Wayland setup, so spacebar / R silently die until the
        // user clicks the canvas.
        if (auto* mc = focusRestoreTarget().getComponent())
            mc->grabKeyboardFocus();

        auto safeTarget = focusRestoreTarget();
        juce::MessageManager::callAsync ([safeTarget]() mutable
        {
            if (auto* c = safeTarget.getComponent())
                c->grabKeyboardFocus();
        });
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
        // Forward global transport / navigation shortcuts (Space, R, Home,
        // '.', F11) to MainComponent (registered as the focus-restore target
        // on app startup). The modal body has keyboard focus, so without this
        // hop the keys die at the body. Forwarding to MainComponent::keyPressed
        // directly reaches the handler — forwarding to the top-level
        // DocumentWindow does NOT, because DocumentWindow's own keyPressed only
        // handles its title-bar shortcuts. Edit/destructive keys are excluded
        // (see isModalForwardableShortcut). TextEditor children consume their
        // keys before this fires, so typing isn't affected.
        if (isModalForwardableShortcut (k))
        {
            if (auto* mc = focusRestoreTarget().getComponent())
                return mc->keyPressed (k);
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

    // Components hidden by hidePluginEditorsUnder() in show(); restored
    // by restoreHiddenPluginEditors() in close(). Plugin editors (in
    // particular OOP / XEmbed / GL-rendering hosts) can paint above
    // JUCE's modal in the native window's z-order regardless of
    // toFront() — toggling their visibility forces them out of view
    // for the modal's lifetime.
    juce::Array<juce::Component::SafePointer<juce::Component>> hiddenForModal_;

    void hidePluginEditorsUnder (juce::Component& root)
    {
        restoreHiddenPluginEditors();   // defensive: idempotent show
        walkHidePluginEditors (root);
    }

    void walkHidePluginEditors (juce::Component& c)
    {
        for (auto* child : c.getChildren())
        {
            if (child == nullptr) continue;
            // Skip the modal's own backdrop / body / dim so we don't
            // hide the modal itself.
            if (child == dim_.get() || child == backdrop_.get()
                || child == body_.get() || child == borrowedBody_)
                continue;
            const bool isPluginEditor =
                (bool) child->getProperties()
                            .getWithDefault ("dusk_pluginEditor", false);
            if (isPluginEditor)
            {
                // Per-editor hide token (dusk_modalHideCount). Each stacked
                // modal that covers the editor holds one; the editor stays
                // hidden until the LAST of them restores. A plain
                // setVisible(true) on close would re-show an editor still
                // covered by another modal when modals close out of order.
                //
                // Only manage editors that were on-screen when the first
                // covering modal opened (count == 0 && visible) or that an
                // outer modal is already managing (count > 0). A genuinely
                // app-hidden editor (count 0, invisible) is left alone so we
                // never wrongly re-show it.
                const int count = (int) child->getProperties()
                                            .getWithDefault ("dusk_modalHideCount", 0);
                if (count > 0 || child->isVisible())
                {
                    child->getProperties().set ("dusk_modalHideCount", count + 1);
                    if (count == 0)
                        child->setVisible (false);
                    hiddenForModal_.add (juce::Component::SafePointer<juce::Component> (child));
                }
                // Tagged editor: its whole subtree hides with it, so don't
                // recurse - a nested tagged editor would collect a redundant
                // hide token and unbalance the reference count.
                continue;
            }
            walkHidePluginEditors (*child);
        }
    }

    void restoreHiddenPluginEditors()
    {
        for (auto& safe : hiddenForModal_)
        {
            if (auto* c = safe.getComponent())
            {
                const int count = (int) c->getProperties()
                                          .getWithDefault ("dusk_modalHideCount", 0);
                if (count <= 1)
                {
                    c->getProperties().remove ("dusk_modalHideCount");
                    c->setVisible (true);   // last covering modal gone → editor returns
                }
                else
                {
                    c->getProperties().set ("dusk_modalHideCount", count - 1);
                }
            }
        }
        hiddenForModal_.clearQuick();
    }
};

// Global KeyListener that forwards transport / navigation hotkeys (Space, R,
// Home, '.', F11 — see isModalForwardableShortcut) to the registered
// MainComponent regardless of where focus currently sits.
// Attach to popup top-levels that bypass EmbeddedModal (juce::CallOutBox,
// e.g., the COMP / EQ / AUX-send compact-mode editors) so the user can
// play / stop / record / return-to-zero while those modals are open.
// EmbeddedModal-based popups already forward in their own keyPressed;
// CallOutBox doesn't.
class TransportKeyForwarder final : public juce::KeyListener
{
public:
    static TransportKeyForwarder& instance()
    {
        static TransportKeyForwarder t;
        return t;
    }

    bool keyPressed (const juce::KeyPress& k, juce::Component*) override
    {
        if (! isModalForwardableShortcut (k)) return false;
        if (auto* mc = EmbeddedModal::focusRestoreTarget().getComponent())
            return mc->keyPressed (k);
        return false;
    }
};

// Convenience — installs the singleton transport-key forwarder on c.
// No removal step needed: the singleton outlives every component, and
// JUCE auto-clears the listener list when c is destructed.
inline void attachTransportKeyForwarder (juce::Component& c)
{
    c.addKeyListener (&TransportKeyForwarder::instance());
}
} // namespace duskstudio
