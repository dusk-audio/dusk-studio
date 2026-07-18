#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "DimOverlay.h"
#include <functional>
#include <memory>
#include <vector>
#include <algorithm>

namespace duskstudio
{
// Global shortcuts that stay live while a modal / popup holds keyboard
// focus, forwarded to the registered MainComponent. Deliberately limited to
// transport + loop/punch + playhead navigation + window fullscreen: edit /
// clipboard / destructive keys (Delete, split, nudge, undo, save, marker, ...)
// are NOT forwarded because they would act on the arrangement hidden behind
// the modal - e.g. Delete silently removing a region the user can't even see.
// The loop/punch keys ARE forwarded so the engineer can set a loop and audition
// it while a comp / EQ / plugin editor is open (none of L / P / [ / ] has a
// Cmd-binding, so forwarding can't trip a destructive op). A focused child
// that wants one of these keys (TextEditor caret nav) consumes it first, so
// it never reaches the modal's listener and typing is unaffected.
inline bool isModalForwardableShortcut (const juce::KeyPress& k) noexcept
{
    // Bare transport keys only - a Cmd/Ctrl/Alt chord (e.g. Ctrl+R, Cmd+.)
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
        || kc == '[' || kc == ']'                         // set loop in/out at playhead
        || kc == '{' || kc == '}'                         // Shift+bracket (X11 shifted glyph) = punch in/out
        || k == juce::KeyPress::F11Key;
}

// Component-properties tag marking a plugin-editor component (JUCE editor, OOP
// embed, native CLAP/LV2 editor). EmbeddedModal hides tagged components while a
// modal is up - native editor windows otherwise paint above the modal regardless
// of JUCE z-order, burying dialogs. Every editor wrapper must carry this tag.
inline constexpr const char* kPluginEditorTag = "dusk_pluginEditor";

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
class EmbeddedModal final : private juce::KeyListener,
                            private juce::ComponentListener,
                            private juce::MouseListener
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

    // App-wide monotonic modal-lifecycle token, bumped by every show() /
    // showBorrowed() across ALL modal instances. Its sole job is to invalidate
    // a stale deferred focus grab: close() snapshots the token and its async
    // re-grab bails if a newer show() has bumped it since, so the grab can't
    // yank focus off a modal that opened during the async window. WHICH
    // component regains focus is decided by activeModalStack(), not this token
    // (see close()).
    static unsigned long long& modalGeneration()
    {
        static unsigned long long gen = 0;
        return gen;
    }

    // App-wide stack of currently-open modals, in open order (back() = newest).
    // Modals stack (a combo opens over an alert / plugin editor); close() pops
    // itself and hands focus to the newest that remains, falling back to
    // MainComponent only when the stack empties. Every teardown path
    // (close / closeAndDeleteBodyNow / ~EmbeddedModal) removes its entry, so a
    // pointer here is always a live, open modal.
    static std::vector<EmbeddedModal*>& activeModalStack()
    {
        // Heap-allocated and deliberately never freed. The function-local-static
        // shared modals (DuskAlerts / DuskComboBox / ...) run their destructor ->
        // close() -> removeFromModalStack() during static teardown. A plain
        // function-local static vector constructs on the first show() - AFTER
        // those modals - so it would destruct BEFORE them and turn that teardown
        // access into a use-after-free. Leaking it keeps the registry valid for
        // every teardown path.
        static auto* stack = new std::vector<EmbeddedModal*>();
        return *stack;
    }

    EmbeddedModal()  = default;
    ~EmbeddedModal() override { close(); }

    // Modal takes ownership of body; close destructs it.
    //
    // dismissOnClickOutside=false : dim is a no-op (decision modals
    //   need explicit button).
    // dismissOnEscape=false : Esc swallowed (focus-locked decision
    //   modals like save-before-quit).
    // hidePluginEditors=false : keep tagged plugin editors visible. This is
    //   intended for lightweight controls (such as combo popups) opened from
    //   inside an editor; hiding the editor would also hide the control that
    //   owns the popup and leave only its empty backdrop visible.
    // useOverlay=false : don't place a full-parent JUCE component over the
    //   host. Some embedded/native editor surfaces render black whenever any
    //   sibling covers them, even when that sibling paints fully transparent.
    //   Outside clicks are captured with a global mouse listener instead.
    // forwardShortcuts=false : do NOT forward transport hotkeys (Space, R, L,
    //   P, brackets) to MainComponent while this modal is up. Required for a
    //   body that captures typing itself (e.g. the combo popup's type-to-filter);
    //   otherwise a typed letter fires the app shortcut behind the modal.
    // onDismissOutside : invoked instead of onDismiss on a click-outside
    //   dismissal (falls back to onDismiss when empty). Lets a caller close
    //   WITHOUT restoring focus so a control clicked outside keeps it (see
    //   close(bool)); Esc keeps using onDismiss.
    // Defaults preserve "Esc + click-outside both dismiss".
    void show (juce::Component& parent,
               std::unique_ptr<juce::Component> body,
               std::function<void()> onDismiss = {},
               bool dismissOnClickOutside = true,
               bool dismissOnEscape = true,
               float dimAlpha = 0.55f,   // processing editors pass kEditorDimAlpha
               bool hidePluginEditors = true,
               bool useOverlay = true,
               bool forwardShortcuts = true,
               std::function<void()> onDismissOutside = {})
    {
        close();
        ++modalGeneration();
        host = &parent;
        body_ = std::move (body);
        userOnDismiss = std::move (onDismiss);
        userOnDismissOutside = std::move (onDismissOutside);
        escapeDismisses = dismissOnEscape;
        forwardShortcuts_ = forwardShortcuts;
        if (useOverlay)
        {
            dim_ = std::make_unique<DimOverlay> (dimAlpha);
            dim_->setBounds (parent.getLocalBounds());
            if (dismissOnClickOutside)
                dim_->onClick = [this]
                {
                    // An overlay-less popup (DuskComboBox) may be stacked above
                    // this modal; its global listener dismisses it from the same
                    // click. Only the topmost modal may act on a dim click, or
                    // one click tears down both layers.
                    if (activeModalStack().empty() || activeModalStack().back() != this)
                        return;
                    // Local copy BEFORE invoking - the user's callback may
                    // close() this modal, which resets the dismiss callbacks
                    // and destroys the closure (with captures) mid-call.
                    // SIGABRT on Linux/XWayland without this. Prefer the
                    // outside-click callback so a clicked control keeps focus.
                    auto cb = userOnDismissOutside ? userOnDismissOutside : userOnDismiss;
                    if (cb) cb();
                    else    close();
                };
            parent.addAndMakeVisible (dim_.get());
        }
        else if (dismissOnClickOutside)
        {
            juce::Desktop::getInstance().addGlobalMouseListener (this);
            listeningForOutsideClicks = true;
        }

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
        // Force topmost - a stage swap that re-adds a fullscreen view
        // (AuxView, MasteringView) after the modal opens can demote it.
        // addAndMakeVisible alone is only topmost-at-add-time.
        if (dim_ != nullptr)
            dim_->toFront (false);
        backdrop_->toFront (false);
        body_    ->toFront (true);
        body_->setWantsKeyboardFocus (true);
        body_->grabKeyboardFocus();
        body_->addKeyListener (this);

        if (hidePluginEditors)
            hidePluginEditorsUnder (parent);

        activeModalStack().push_back (this);
    }

    // Body NOT owned - caller keeps alive across show/close cycles.
    // Used for plugin editors: tearing down a plugin's editor window on
    // every close races the WM and on XWayland with GL-heavy GUIs can
    // crash the compositor. Keeping the editor alive and add/removing
    // it as a child is significantly more stable.
    void showBorrowed (juce::Component& parent,
                       juce::Component& body,
                       std::function<void()> onDismiss = {})
    {
        close();
        ++modalGeneration();
        host = &parent;
        borrowedBody_ = &body;
        // Borrowed modals are plugin editors: always forward transport shortcuts
        // so the engineer can play / loop / audition while the editor is open.
        // Reset explicitly - a prior show(..., forwardShortcuts=false) on this
        // instance leaves it disabled (close() doesn't reset it).
        forwardShortcuts_ = true;
        // showBorrowed is plugin-editors-only - use the lighter editor dim so
        // the strip meters behind stay readable while auditioning.
        dim_ = std::make_unique<DimOverlay> (kEditorDimAlpha);
        dim_->setBounds (parent.getLocalBounds());
        userOnDismiss = std::move (onDismiss);
        dim_->onClick = [this]
        {
            // See owning show()'s onClick - topmost-only guard + local copy
            // survives close().
            if (activeModalStack().empty() || activeModalStack().back() != this)
                return;
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
        // Plugin editors announce their real size only once the native
        // window embeds (after this show) and can rescale live from
        // their own UI - track it, or the body grows down-right from
        // the stale centre and the backdrop stays at the old size.
        body.addComponentListener (this);
        // Track the host too: resizing the main window with an editor
        // open otherwise leaves the modal at the old centre, clipped at
        // the new edges. Borrowed-only - owned bodies include anchored
        // popups (DuskContextMenu / DuskComboBox) that must NOT recentre.
        parent.addComponentListener (this);

        hidePluginEditorsUnder (parent);

        activeModalStack().push_back (this);
    }

    // Idempotent. For owning show, destructs body; for showBorrowed,
    // just removes from parent.
    //
    // CONTRACT: close() does NOT invoke userOnDismiss. Only Esc and
    // dim-click do. Helpers (DuskAlerts' OK button) rely on this -
    // they wire their action button to close() AND their follow-up,
    // trusting close() won't double-fire. Audit every caller before
    // changing this contract.
    //
    // restoreFocus=false : skip the immediate MainComponent focus grab. For a
    // click-outside dismissal the clicked control has already taken focus;
    // grabbing it back to MainComponent would steal it (see show()'s
    // onDismissOutside). A deferred grab still fires if nothing took focus.
    // Esc / pick / button closes keep the default.
    //
    // Safe from inside a body's callback (button onClick): synchronous
    // path detaches body/backdrop/dim from parent so the modal
    // disappears immediately, then defers destruction to the next
    // message-loop tick via callAsync. Without that, body_.reset()
    // would run ~Button -> ~std::function while the button's onClick
    // lambda is still on the stack - observed to corrupt JUCE's
    // message-thread state and crash compositors on next X11 round.
    void close (bool restoreFocus = true)
    {
        if (host == nullptr) return;

        removeFromModalStack();
        stopListeningForOutsideClicks();

        // Restore any plugin editors we hid in show() before tearing
        // down the modal body. Done first so their setVisible(true)
        // doesn't fight with the message-loop teardown path below.
        restoreHiddenPluginEditors();

        host->removeComponentListener (this);
        if (body_         != nullptr) host->removeChildComponent (body_.get());
        if (borrowedBody_ != nullptr)
        {
            borrowedBody_->removeKeyListener (this);
            borrowedBody_->removeComponentListener (this);
            host->removeChildComponent (borrowedBody_);
        }
        if (backdrop_ != nullptr) host->removeChildComponent (backdrop_.get());
        if (dim_      != nullptr) host->removeChildComponent (dim_.get());

        if (body_ != nullptr)
        {
            // Hand to a callAsync lambda - destructs on the next tick
            // after the current button-callback stack unwinds. Don't
            // capture `this` - EmbeddedModal itself may be destructed
            // by then (app shutdown). shared_ptr (not unique) so the
            // capture is copyable - std::function needs copyable
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
        // Restore keyboard focus to whatever should own it now this modal is
        // gone. With modals stacked (a combo opened over an alert / plugin
        // editor), focus must return to the newest modal STILL open, not all
        // the way back to MainComponent - the latter would yank focus off a
        // modal the user can still see. Fall back to MainComponent (registered
        // in its ctor via focusRestoreTarget()) only when the stack empties.
        //
        // Grab both synchronously (so the very next key event already lands
        // there) AND deferred (JUCE's own modal-teardown / focus-traverser can
        // steal focus once the trash lambdas run and this modal's children are
        // gone; the default traverser does NOT reliably drill into the content
        // component on this hybrid X11/Wayland setup, so spacebar / R silently
        // die without the re-grab). The epoch snapshot invalidates the deferred
        // grab if a show()/showBorrowed() during the async window opened a newer
        // modal that now owns focus - don't steal it back.
        {
            juce::Component::SafePointer<juce::Component> target;
            const auto& stack = activeModalStack();
            if (! stack.empty())
                target = stack.back()->getBody();
            if (target == nullptr)
                target = focusRestoreTarget();

            if (restoreFocus)
                if (auto* c = target.getComponent())
                    c->grabKeyboardFocus();

            // restoreFocus=false still needs the deferred pass: the dismissing
            // click may have landed on a component that takes no keyboard focus
            // (setMouseClickGrabsKeyboardFocus(false)), leaving focus orphaned
            // once the body is destroyed. Grab only when nothing else took it.
            const auto genAtClose = modalGeneration();
            juce::MessageManager::callAsync ([target, genAtClose, restoreFocus]() mutable
            {
                if (genAtClose != modalGeneration()) return;
                if (! restoreFocus
                    && juce::Component::getCurrentlyFocusedComponent() != nullptr)
                    return;   // the clicked control owns focus - keep it there
                if (auto* c = target.getComponent())
                    c->grabKeyboardFocus();
            });
        }
        host = nullptr;
        userOnDismiss = {};
        userOnDismissOutside = {};
    }

    // Shutdown-only teardown. close() defers body destruction to the
    // next message-loop tick; on the quit path the dispatch loop has
    // already exited, so the deferred lambda runs after AudioEngine is
    // destroyed and a body whose destructor talks to the engine
    // (AudioSettingsPanel, PluginScanModal, BounceDialog) frees memory
    // it then dereferences. This variant destroys the body in place.
    // Only call when no body callback is on the stack - i.e. from
    // ~MainComponent / beginSafeShutdown.
    void closeAndDeleteBodyNow()
    {
        removeFromModalStack();
        stopListeningForOutsideClicks();
        restoreHiddenPluginEditors();

        if (host != nullptr)
        {
            host->removeComponentListener (this);
            if (body_         != nullptr) host->removeChildComponent (body_.get());
            if (borrowedBody_ != nullptr)
            {
                borrowedBody_->removeKeyListener (this);
                borrowedBody_->removeComponentListener (this);
                host->removeChildComponent (borrowedBody_);
            }
            if (backdrop_ != nullptr) host->removeChildComponent (backdrop_.get());
            if (dim_      != nullptr) host->removeChildComponent (dim_.get());
        }

        body_.reset();
        backdrop_.reset();
        dim_.reset();
        borrowedBody_ = nullptr;
        host = nullptr;
        userOnDismiss = {};
        userOnDismissOutside = {};
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

    // Re-centre the body (and its backdrop) on the host after the caller has
    // resized it while the modal is open. show() centres once at open time; a
    // body that grows/shrinks in place (e.g. the I/O popup gaining MIDI rows)
    // must call this or it stays anchored at its old top-left, off-centre.
    void recenterBody()
    {
        auto* body = getBody();
        if (body == nullptr || host == nullptr) return;
        const auto bounds = host->getLocalBounds();
        const auto bodyBounds = bounds.withSizeKeepingCentre (
            juce::jmin (juce::jmax (1, body->getWidth()),  bounds.getWidth()  - 16),
            juce::jmin (juce::jmax (1, body->getHeight()), bounds.getHeight() - 16));
        body->setTopLeftPosition (bodyBounds.getTopLeft());
        // Re-fit the backdrop to the body's REAL bounds (not the clamped
        // rect) - a body that outgrew its open-time size otherwise keeps
        // the old, smaller frame behind it.
        if (backdrop_ != nullptr)
            backdrop_->setBounds (body->getBounds().expanded (kBackdropMargin));
    }

private:
    void removeFromModalStack()
    {
        auto& stack = activeModalStack();
        stack.erase (std::remove (stack.begin(), stack.end(), this), stack.end());
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! listeningForOutsideClicks) return;

        // Only the topmost open modal may act on an outside click - with a
        // modal stacked above this one, that layer owns the click, and
        // dismissing from underneath would tear down both layers at once.
        if (activeModalStack().empty() || activeModalStack().back() != this)
            return;

        // Test against the backdrop frame, not just the body - the visible
        // popup includes the kBackdropMargin ring, and a click on it must not
        // count as "outside".
        juce::Component* hit = backdrop_ != nullptr ? backdrop_.get() : getBody();
        if (hit != nullptr && hit->getScreenBounds().contains (e.getScreenPosition()))
            return;

        // Keep the callback alive across close(), which clears the dismiss
        // callbacks. Prefer the outside-click callback so the clicked control
        // keeps focus.
        auto cb = userOnDismissOutside ? userOnDismissOutside : userOnDismiss;
        if (cb) cb();
        else    close();
    }

    void stopListeningForOutsideClicks()
    {
        if (! listeningForOutsideClicks) return;
        juce::Desktop::getInstance().removeGlobalMouseListener (this);
        listeningForOutsideClicks = false;
    }

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
        // directly reaches the handler - forwarding to the top-level
        // DocumentWindow does NOT, because DocumentWindow's own keyPressed only
        // handles its title-bar shortcuts. Edit/destructive keys are excluded
        // (see isModalForwardableShortcut). TextEditor children consume their
        // keys before this fires, so typing isn't affected.
        if (forwardShortcuts_ && isModalForwardableShortcut (k))
        {
            if (auto* mc = focusRestoreTarget().getComponent())
                return mc->keyPressed (k);
        }
        return false;
    }

    // Borrowed plugin-editor bodies resize themselves when the native
    // window embeds or the plugin rescales its UI; the host resizes when
    // the user resizes the main window with the editor open. Re-centre
    // and re-fit the backdrop either way; moves are ignored (recenterBody
    // itself moves the body, so reacting to them would recurse).
    void componentMovedOrResized (juce::Component& c, bool, bool wasResized) override
    {
        if (! wasResized || borrowedBody_ == nullptr) return;
        if (&c == borrowedBody_ || &c == host.getComponent())
        {
            recenterBody();
            if (dim_ != nullptr && host != nullptr)
                dim_->setBounds (host->getLocalBounds());
        }
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

    // SafePointer, not raw: the function-local-static shared modals
    // (DuskAlerts, DuskContextMenu, DuskComboBox, file browser, plugin
    // picker) outlive MainComponent. If one is still open at quit, its
    // static destructor runs close() after the host was destroyed - a
    // raw pointer would dereference freed memory in removeChildComponent.
    juce::Component::SafePointer<juce::Component> host;
    std::unique_ptr<DimOverlay> dim_;
    std::unique_ptr<Backdrop> backdrop_;
    std::unique_ptr<juce::Component> body_;
    juce::Component* borrowedBody_ = nullptr;
    std::function<void()> userOnDismiss;
    std::function<void()> userOnDismissOutside;
    bool escapeDismisses = true;
    bool forwardShortcuts_ = true;
    bool listeningForOutsideClicks = false;

    // Components hidden by hidePluginEditorsUnder() in show(); restored
    // by restoreHiddenPluginEditors() in close(). Plugin editors (in
    // particular OOP / XEmbed / GL-rendering hosts) can paint above
    // JUCE's modal in the native window's z-order regardless of
    // toFront() - toggling their visibility forces them out of view
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
                            .getWithDefault (kPluginEditorTag, false);
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
                    c->setVisible (true);   // last covering modal gone -> editor returns
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
// Home, '.', F11 - see isModalForwardableShortcut) to the registered
// MainComponent regardless of where focus currently sits.
// Attach to modal surfaces that bypass EmbeddedModal (e.g. the TapeMachine
// gear modal's raw DimOverlay host) so the user can play / stop / record /
// return-to-zero while those modals are open. EmbeddedModal-based popups
// already forward in their own keyPressed.
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

// Convenience - installs the singleton transport-key forwarder on c.
// No removal step needed: the singleton outlives every component, and
// JUCE auto-clears the listener list when c is destructed.
inline void attachTransportKeyForwarder (juce::Component& c)
{
    c.addKeyListener (&TransportKeyForwarder::instance());
}
} // namespace duskstudio
