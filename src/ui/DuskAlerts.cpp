#include "DuskAlerts.h"
#include "EmbeddedModal.h"

namespace duskstudio
{
namespace
{
// Each helper owns its own static modal so an alert can stack OVER a
// confirm (e.g. picker's scan-complete inside a destructive-action
// prompt — admittedly rare). Pattern mirrors sharedPickerModal vs
// sharedAlertModal in PluginPickerHelpers.cpp.
EmbeddedModal& alertModal()    { static EmbeddedModal m; return m; }
EmbeddedModal& confirmModal()  { static EmbeddedModal m; return m; }
EmbeddedModal& decisionModal() { static EmbeddedModal m; return m; }
EmbeddedModal& textModal()     { static EmbeddedModal m; return m; }

void styleAction (juce::TextButton& b, bool destructive = false)
{
    b.setColour (juce::TextButton::buttonColourId,
                   destructive ? juce::Colour (0xff7a3030)
                                : juce::Colour (0xff262630));
    b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff5a4880));
    b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd0d0d4));
    b.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
}

void paintTitleAndBody (juce::Graphics& g,
                          juce::Rectangle<int> bounds,
                          const juce::String& title,
                          const juce::String& message,
                          int buttonStripH)
{
    g.fillAll (juce::Colour (0xff1a1a22));
    auto r = bounds.reduced (20);

    g.setColour (juce::Colours::white);
    g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
    g.drawText (title, r.removeFromTop (24),
                 juce::Justification::topLeft, false);
    r.removeFromTop (10);

    g.setColour (juce::Colour (0xffd0d0d4));
    g.setFont (juce::Font (juce::FontOptions (12.5f)));
    auto body = r.withTrimmedBottom (buttonStripH + 8);
    g.drawFittedText (message.isEmpty() ? juce::String ("(no message)") : message,
                        body, juce::Justification::topLeft, 6);
}

constexpr int kButtonStripH = 32;

// -----------------------------------------------------------------------------
// Single-OK alert panel
// -----------------------------------------------------------------------------
class AlertPanel final : public juce::Component
{
public:
    AlertPanel (juce::String title, juce::String message)
        : titleStr (std::move (title)), messageStr (std::move (message))
    {
        setOpaque (true);
        styleAction (okBtn);
        okBtn.onClick = [this] { if (onOK) onOK(); };
        addAndMakeVisible (okBtn);
        setWantsKeyboardFocus (true);
        setSize (460, 220);
    }
    std::function<void()> onOK;

    void paint (juce::Graphics& g) override
    {
        paintTitleAndBody (g, getLocalBounds(), titleStr, messageStr, kButtonStripH);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (20);
        auto row = r.removeFromBottom (kButtonStripH);
        okBtn.setBounds (row.removeFromRight (100));
    }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey || k == juce::KeyPress::returnKey)
        {
            if (onOK) onOK();
            return true;
        }
        return false;
    }

private:
    juce::String titleStr, messageStr;
    juce::TextButton okBtn { "OK" };
};

// -----------------------------------------------------------------------------
// Two-button confirm panel
// -----------------------------------------------------------------------------
class ConfirmPanel final : public juce::Component
{
public:
    ConfirmPanel (juce::String title, juce::String message,
                   juce::String primaryLabel, juce::String secondaryLabel,
                   bool destructive)
        : titleStr (std::move (title)), messageStr (std::move (message))
    {
        setOpaque (true);
        primaryBtn.setButtonText (primaryLabel);
        secondaryBtn.setButtonText (secondaryLabel);
        styleAction (primaryBtn, destructive);
        styleAction (secondaryBtn);
        primaryBtn.onClick   = [this] { if (onPrimary)   onPrimary(); };
        secondaryBtn.onClick = [this] { if (onSecondary) onSecondary(); };
        addAndMakeVisible (primaryBtn);
        addAndMakeVisible (secondaryBtn);
        setWantsKeyboardFocus (true);
        setSize (480, 220);
    }
    std::function<void()> onPrimary;
    std::function<void()> onSecondary;

    void paint (juce::Graphics& g) override
    {
        paintTitleAndBody (g, getLocalBounds(), titleStr, messageStr, kButtonStripH);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (20);
        auto row = r.removeFromBottom (kButtonStripH);
        primaryBtn.setBounds (row.removeFromRight (110));
        row.removeFromRight (8);
        secondaryBtn.setBounds (row.removeFromRight (100));
    }
    bool keyPressed (const juce::KeyPress& k) override
    {
        // Enter triggers primary, Esc triggers secondary — only when
        // dismissOnEscape was true at show() time (the modal swallows
        // Esc otherwise). Mirrors native dialog conventions.
        if (k == juce::KeyPress::returnKey)
        {
            if (onPrimary) onPrimary();
            return true;
        }
        return false;
    }

private:
    juce::String titleStr, messageStr;
    juce::TextButton primaryBtn   { "OK" };
    juce::TextButton secondaryBtn { "Cancel" };
};

// -----------------------------------------------------------------------------
// N-button decision panel
// -----------------------------------------------------------------------------
class DecisionPanel final : public juce::Component
{
public:
    DecisionPanel (juce::String title, juce::String message,
                    std::vector<DuskDecisionAction> actionsIn)
        : titleStr (std::move (title)),
          messageStr (std::move (message)),
          actions (std::move (actionsIn))
    {
        setOpaque (true);
        for (auto& act : actions)
        {
            auto btn = std::make_unique<juce::TextButton> (act.label);
            styleAction (*btn, act.destructive);
            auto cb = act.onClick;
            btn->onClick = [cb] { if (cb) cb(); };
            addAndMakeVisible (*btn);
            buttons.push_back (std::move (btn));
        }
        setWantsKeyboardFocus (true);
        const int w = juce::jmax (480, 60 + 120 * (int) actions.size());
        setSize (w, 220);
    }

    void paint (juce::Graphics& g) override
    {
        paintTitleAndBody (g, getLocalBounds(), titleStr, messageStr, kButtonStripH);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (20);
        auto row = r.removeFromBottom (kButtonStripH);
        // Right-to-left: last action in vector lands at far right.
        for (int i = (int) buttons.size() - 1; i >= 0; --i)
        {
            buttons[(size_t) i]->setBounds (row.removeFromRight (110));
            if (i > 0) row.removeFromRight (8);
        }
    }
    // No keyPressed override — decision modals do nothing on Enter / Esc
    // (Esc swallowed by EmbeddedModal when dismissOnEscape=false). Forces
    // an explicit button click.

private:
    juce::String titleStr, messageStr;
    std::vector<DuskDecisionAction> actions;
    std::vector<std::unique_ptr<juce::TextButton>> buttons;
};

// -----------------------------------------------------------------------------
// Text-input panel
// -----------------------------------------------------------------------------
class TextInputPanel final : public juce::Component
{
public:
    TextInputPanel (juce::String title, juce::String prompt, juce::String initial)
        : titleStr (std::move (title)), promptStr (std::move (prompt))
    {
        setOpaque (true);
        editor.setText (initial, juce::dontSendNotification);
        editor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff14141a));
        editor.setColour (juce::TextEditor::textColourId,        juce::Colour (0xffe0e0e4));
        editor.setColour (juce::TextEditor::outlineColourId,     juce::Colour (0xff34343c));
        editor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colour (0xff6a5aa0));
        editor.setSelectAllWhenFocused (true);
        editor.onReturnKey = [this] { if (onAccept) onAccept (editor.getText().trim()); };
        addAndMakeVisible (editor);

        styleAction (okBtn);
        styleAction (cancelBtn);
        okBtn.onClick = [this] { if (onAccept) onAccept (editor.getText().trim()); };
        cancelBtn.onClick = [this] { if (onCancel) onCancel(); };
        addAndMakeVisible (okBtn);
        addAndMakeVisible (cancelBtn);

        setWantsKeyboardFocus (true);
        setSize (460, 220);
    }
    std::function<void (const juce::String&)> onAccept;
    std::function<void()>                      onCancel;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1a1a22));
        auto r = getLocalBounds().reduced (20);

        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
        g.drawText (titleStr, r.removeFromTop (24),
                     juce::Justification::topLeft, false);
        r.removeFromTop (10);

        g.setColour (juce::Colour (0xffd0d0d4));
        g.setFont (juce::Font (juce::FontOptions (12.5f)));
        g.drawText (promptStr, r.removeFromTop (18),
                     juce::Justification::topLeft, false);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (20);
        r.removeFromTop (24 + 10 + 18 + 8);
        editor.setBounds (r.removeFromTop (28));
        auto row = r.removeFromBottom (kButtonStripH);
        okBtn.setBounds (row.removeFromRight (100));
        row.removeFromRight (8);
        cancelBtn.setBounds (row.removeFromRight (100));
    }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey)
        {
            if (onCancel) onCancel();
            return true;
        }
        return false;
    }
    void visibilityChanged() override
    {
        if (isVisible()) editor.grabKeyboardFocus();
    }

private:
    juce::String titleStr, promptStr;
    juce::TextEditor editor;
    juce::TextButton okBtn     { "OK" };
    juce::TextButton cancelBtn { "Cancel" };
};
} // namespace

// -----------------------------------------------------------------------------
void showDuskAlert (juce::Component& parent,
                     juce::String title,
                     juce::String message,
                     std::function<void()> onDismiss)
{
    auto panel = std::make_unique<AlertPanel> (std::move (title), std::move (message));
    panel->onOK = [onDismiss]() mutable
    {
        alertModal().close();
        if (onDismiss) onDismiss();
    };
    alertModal().show (parent, std::move (panel),
                          /*onDismiss*/ [onDismiss]() mutable
                          {
                              alertModal().close();
                              if (onDismiss) onDismiss();
                          });
}

void showDuskConfirm (juce::Component& parent,
                       juce::String title,
                       juce::String message,
                       juce::String primaryLabel,
                       std::function<void()> onPrimary,
                       juce::String secondaryLabel,
                       std::function<void()> onSecondary,
                       bool destructive)
{
    auto panel = std::make_unique<ConfirmPanel> (
        std::move (title), std::move (message),
        std::move (primaryLabel), std::move (secondaryLabel), destructive);
    panel->onPrimary = [onPrimary]() mutable
    {
        confirmModal().close();
        if (onPrimary) onPrimary();
    };
    panel->onSecondary = [onSecondary]() mutable
    {
        confirmModal().close();
        if (onSecondary) onSecondary();
    };
    // Destructive prompts lock click-outside + Esc so accidental
    // dismissal can't fire either action. Non-destructive prompts treat
    // click-outside / Esc as Cancel (secondary).
    auto dismissCb = [onSecondary]() mutable
    {
        confirmModal().close();
        if (onSecondary) onSecondary();
    };
    confirmModal().show (parent, std::move (panel), dismissCb,
                            /*dismissOnClickOutside*/ ! destructive,
                            /*dismissOnEscape*/        ! destructive);
}

void showDuskDecision (juce::Component& parent,
                        juce::String title,
                        juce::String message,
                        std::vector<DuskDecisionAction> actions)
{
    // Wrap each action's onClick so the modal closes first, then the
    // callback fires. Without the wrap, callbacks that themselves open
    // another modal would stack instead of replace.
    for (auto& act : actions)
    {
        auto orig = std::move (act.onClick);
        act.onClick = [orig]() mutable
        {
            decisionModal().close();
            if (orig) orig();
        };
    }
    auto panel = std::make_unique<DecisionPanel> (
        std::move (title), std::move (message), std::move (actions));
    decisionModal().show (parent, std::move (panel), /*onDismiss*/ {},
                             /*dismissOnClickOutside*/ false,
                             /*dismissOnEscape*/        false);
}

void showDuskTextInput (juce::Component& parent,
                         juce::String title,
                         juce::String prompt,
                         juce::String initial,
                         std::function<void (const juce::String&)> onAccept)
{
    auto panel = std::make_unique<TextInputPanel> (
        std::move (title), std::move (prompt), std::move (initial));
    panel->onAccept = [onAccept] (const juce::String& s) mutable
    {
        textModal().close();
        if (onAccept) onAccept (s);
    };
    panel->onCancel = []
    {
        textModal().close();
    };
    textModal().show (parent, std::move (panel),
                         /*onDismiss*/ [] { textModal().close(); });
}
} // namespace duskstudio
