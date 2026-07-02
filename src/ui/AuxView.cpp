#include "AuxView.h"
#include "AuxLaneComponent.h"
#include "../engine/AudioEngine.h"

namespace duskstudio
{
namespace
{
void styleSelectorButton (juce::TextButton& b, juce::Colour onColour)
{
    b.setClickingTogglesState (true);
    b.setRadioGroupId (2001);
    b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    b.setColour (juce::TextButton::buttonOnColourId, onColour);
    b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff909094));
    b.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
}

// In-window rename panel — replaces juce::AlertWindow so the rename UI
// matches the project's other modals (DimOverlay backdrop, rounded
// panel, Esc/click-outside dismiss). Owned and shown via EmbeddedModal.
class RenameAuxPanel final : public juce::Component
{
public:
    RenameAuxPanel (int laneIndex,
                    const juce::String& currentName,
                    int maxChars,
                    std::function<void(const juce::String&)> onCommitIn,
                    std::function<void()> onCancelIn)
        : onCommit (std::move (onCommitIn)),
          onCancel (std::move (onCancelIn))
    {
        titleLabel.setText ("Rename AUX " + juce::String (laneIndex + 1),
                              juce::dontSendNotification);
        titleLabel.setJustificationType (juce::Justification::centred);
        titleLabel.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
        titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible (titleLabel);

        promptLabel.setText ("Enter a name for this AUX lane (max "
                                + juce::String (maxChars) + " characters):",
                               juce::dontSendNotification);
        promptLabel.setJustificationType (juce::Justification::centred);
        promptLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
        promptLabel.setColour (juce::Label::textColourId, juce::Colour (0xffc0c0c8));
        addAndMakeVisible (promptLabel);

        editor.setText (currentName, juce::dontSendNotification);
        editor.setJustification (juce::Justification::centredLeft);
        editor.setFont (juce::Font (juce::FontOptions (14.0f)));
        editor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff181820));
        editor.setColour (juce::TextEditor::outlineColourId,    juce::Colour (0xff3a3a42));
        editor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colour (0xff6a6a78));
        editor.setColour (juce::TextEditor::textColourId,       juce::Colours::white);
        editor.setInputRestrictions (maxChars);    // hard char cap at typing time
        editor.setSelectAllWhenFocused (true);
        editor.onReturnKey = [this] { commit(); };
        editor.onEscapeKey = [this] { if (onCancel) onCancel(); };
        addAndMakeVisible (editor);

        okButton    .setButtonText ("OK");
        cancelButton.setButtonText ("Cancel");
        for (auto* b : { &okButton, &cancelButton })
        {
            b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a2a32));
            b->setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            b->setColour (juce::TextButton::textColourOnId,  juce::Colours::white);
            addAndMakeVisible (*b);
        }
        okButton    .onClick = [this] { commit(); };
        cancelButton.onClick = [this] { if (onCancel) onCancel(); };

        setSize (380, 220);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (24, 20);
        titleLabel .setBounds (area.removeFromTop (28));
        area.removeFromTop (12);
        promptLabel.setBounds (area.removeFromTop (36));
        area.removeFromTop (8);
        editor     .setBounds (area.removeFromTop (28));
        area.removeFromTop (20);

        auto buttons = area.removeFromTop (32);
        const int buttonW = 96;
        const int gap = 16;
        const int total = buttonW * 2 + gap;
        auto row = buttons.withSizeKeepingCentre (total, buttons.getHeight());
        okButton    .setBounds (row.removeFromLeft (buttonW));
        row.removeFromLeft (gap);
        cancelButton.setBounds (row.removeFromLeft (buttonW));
    }

    void visibilityChanged() override
    {
        if (isShowing()) editor.grabKeyboardFocus();
    }

private:
    void commit()
    {
        if (! onCommit) return;
        onCommit (editor.getText());
    }

    juce::Label titleLabel, promptLabel;
    juce::TextEditor editor;
    juce::TextButton okButton, cancelButton;
    std::function<void(const juce::String&)> onCommit;
    std::function<void()> onCancel;
};
} // namespace

AuxView::AuxView (Session& session, AudioEngine& engine)
{
    sessionPtr = &session;
    for (int i = 0; i < Session::kNumAuxLanes; ++i)
    {
        // Selector button. Use full-saturation accent (matches stage-tab
        // saturation; the earlier 0.7f multiplier made these read as
        // washed-out next to the stage tabs they sit underneath).
        auto& btn = selectorButtons[(size_t) i];
        btn.setButtonText (session.auxLane (i).name);
        styleSelectorButton (btn, session.auxLane (i).colour);
        if (i == 0)
            btn.setConnectedEdges (juce::Button::ConnectedOnRight);
        else if (i == Session::kNumAuxLanes - 1)
            btn.setConnectedEdges (juce::Button::ConnectedOnLeft);
        else
            btn.setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
        btn.onClick = [this, i] { setActiveLane (i); };
        btn.setTooltip ("Single-click to select this AUX lane. Double-click to rename it.");
        // Route the button's mouse events through this AuxView so we
        // can catch double-clicks for inline rename. Single click still
        // hits onClick on the button itself.
        btn.addMouseListener (this, false);
        addAndMakeVisible (btn);

        // Lane content. All four are constructed up front so plugin
        // editors and timer callbacks survive across selector switches;
        // only the active one is visible.
        lanes[(size_t) i] = std::make_unique<AuxLaneComponent> (
            session.auxLane (i), engine.getAuxLaneStrip (i), i, engine);
        addChildComponent (lanes[(size_t) i].get());
    }

    setActiveLane (0);
    // Poll the session lane names so a rename from anywhere (channel
    // strip aux label, aux-lane header, compact-mode popup) refreshes
    // this tab row within ~70 ms.
    startTimerHz (15);
}

AuxView::~AuxView()
{
    // Stop the polling timer BEFORE the derived members (sessionPtr,
    // selectorButtons, lanes) tear down. juce::Timer's own dtor would
    // call stopTimer too, but only after the AuxView destructor body
    // returns — by then the timerCallback has nothing alive to touch.
    stopTimer();
}

void AuxView::timerCallback()
{
    if (sessionPtr == nullptr) return;
    for (int i = 0; i < Session::kNumAuxLanes; ++i)
    {
        const auto& laneName = sessionPtr->auxLane (i).name;
        if (selectorButtons[(size_t) i].getButtonText() != laneName)
            selectorButtons[(size_t) i].setButtonText (laneName);
    }
}

void AuxView::mouseDoubleClick (const juce::MouseEvent& e)
{
    for (int i = 0; i < Session::kNumAuxLanes; ++i)
    {
        if (e.eventComponent == &selectorButtons[(size_t) i])
        {
            promptRenameLane (i);
            return;
        }
    }
}

void AuxView::promptRenameLane (int index)
{
    if (sessionPtr == nullptr) return;
    if (index < 0 || index >= Session::kNumAuxLanes) return;

    // static: referenced inside the rename lambda below. A function-local
    // automatic constexpr would trip MSVC's C3493 (implicit-capture error)
    // there; static storage duration sidesteps capture on every compiler.
    static constexpr int kAuxNameMaxChars = 12;
    auto* host = getTopLevelComponent();
    if (host == nullptr) host = this;

    juce::Component::SafePointer<AuxView> safeSelf (this);
    auto panel = std::make_unique<RenameAuxPanel> (
        index,
        sessionPtr->auxLane (index).name,
        kAuxNameMaxChars,
        [safeSelf, index] (const juce::String& raw)
        {
            if (safeSelf == nullptr || safeSelf->sessionPtr == nullptr) return;
            auto txt = raw.trim();
            if (! txt.isEmpty())
            {
                if (txt.length() > kAuxNameMaxChars)
                    txt = txt.substring (0, kAuxNameMaxChars);
                safeSelf->sessionPtr->auxLane (index).name = txt;
                safeSelf->selectorButtons[(size_t) index].setButtonText (txt);
            }
            safeSelf->renameModal.close();
        },
        [safeSelf]
        {
            if (safeSelf == nullptr) return;
            safeSelf->renameModal.close();
        });

    renameModal.show (*host, std::move (panel));
}

void AuxView::setActiveLane (int index)
{
    index = juce::jlimit (0, Session::kNumAuxLanes - 1, index);
    activeLaneIndex = index;

    for (int i = 0; i < Session::kNumAuxLanes; ++i)
    {
        selectorButtons[(size_t) i].setToggleState (i == index, juce::dontSendNotification);
        if (lanes[(size_t) i] != nullptr)
            lanes[(size_t) i]->setVisible (i == index);
    }
    resized();
}

void AuxView::dropAllClapEditors()
{
    for (auto& lane : lanes)
        if (lane != nullptr)
            lane->dropAllClapEditors();
}

void AuxView::visibilityChanged()
{
    // Stage switches flip THIS component's visible flag; the lanes' own flags
    // don't change, so their visibilityChanged never fires and a lane whose
    // editors were torn down while hidden (the first parent-attach peer sweep
    // after a session restore) would come up empty. Forward the change.
    for (auto& lane : lanes)
        if (lane != nullptr)
            lane->refreshEditorsForShowState();
}

void AuxView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff121214));
}

void AuxView::resized()
{
    auto area = getLocalBounds().reduced (4);

    // Selector row across the top - same height as the main stage buttons
    // for visual consistency.
    constexpr int kSelectorH = 28;
    auto row = area.removeFromTop (kSelectorH);
    const int btnW = juce::jmax (1, row.getWidth() / Session::kNumAuxLanes);
    for (int i = 0; i < Session::kNumAuxLanes; ++i)
    {
        // Last button absorbs any rounding remainder so the row exactly
        // fills the available width.
        const int w = (i == Session::kNumAuxLanes - 1) ? row.getWidth() : btnW;
        selectorButtons[(size_t) i].setBounds (row.removeFromLeft (w));
    }
    area.removeFromTop (6);

    // Active lane fills the rest.
    if (activeLaneIndex >= 0 && activeLaneIndex < Session::kNumAuxLanes
        && lanes[(size_t) activeLaneIndex] != nullptr)
    {
        lanes[(size_t) activeLaneIndex]->setBounds (area);
    }
}
} // namespace duskstudio
