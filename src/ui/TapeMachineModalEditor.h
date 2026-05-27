#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>

namespace duskstudio
{
// Hosts a TapeMachine plugin editor in our gear modal + masks/repositions
// the parts that don't belong in the embedded context:
//   - Painted nameplate / subtitle / "Dusk Audio" footer covered by
//     opaque masks (can't suppress donor paint without modding source).
//   - HQ oversampling combo/label hidden — global oversampling comes
//     from Audio Settings; per-plugin would fight the host.
//   - Corner resize handle hidden — modal sized once on open.
//   - Preset combo + Save/Del recentered (donor pins flush right).
//   - Signal Path + EQ Std recentered under the 3-col row above.
//
// Located by traversal (donor children have no stable IDs) — matched
// by combo items / button text / label text. Overrides applied after
// donor's resized() runs.
class TapeMachineModalEditor final : public juce::Component
{
public:
    explicit TapeMachineModalEditor (juce::AudioProcessorEditor* editor)
        : ownedEditor (editor)
    {
        setOpaque (false);
        if (ownedEditor != nullptr)
        {
            setSize (ownedEditor->getWidth(), ownedEditor->getHeight());
            addAndMakeVisible (*ownedEditor);
            hideHqControls (*ownedEditor);
            hideResizeHandle (*ownedEditor);
            findPresetCluster (*ownedEditor);
        }
        addAndMakeVisible (headerMask);
        addAndMakeVisible (footerMask);
        // Reparent AFTER the masks so it ends up topmost in z-order —
        // otherwise the header mask covers the cluster too.
        if (presetCombo != nullptr) addAndMakeVisible (presetCombo.getComponent());
        if (saveBtn     != nullptr) addAndMakeVisible (saveBtn.getComponent());
        if (delBtn      != nullptr) addAndMakeVisible (delBtn.getComponent());
    }

    void resized() override
    {
        if (ownedEditor == nullptr) return;

        ownedEditor->setBounds (getLocalBounds());
        applyChildOverrides (*ownedEditor);

        // Donor's resized cascades async layout that sometimes re-pins
        // the cluster to the right edge after our sync override runs.
        // Re-run on next tick so we win the final position.
        juce::Component::SafePointer<TapeMachineModalEditor> safe (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (auto* s = safe.getComponent())
                s->recenterPresetCluster();
        });

        // Donor uses a scale relative to baseWidth=800 for pixel sizes.
        // Mirror the same scale here so masks line up at non-1.0 sizes.
        constexpr float kBaseWidth  = 800.0f;
        constexpr int   kHeaderBase = 50;
        constexpr int   kFooterBase = 16;
        const float scale = ownedEditor->getWidth() > 0
                                ? (float) ownedEditor->getWidth() / kBaseWidth
                                : 1.0f;
        const int kHeaderH = (int) (kHeaderBase * scale);
        const int kFooterH = (int) (kFooterBase * scale);
        headerMask.setBounds (0, 0, getWidth(), kHeaderH);
        footerMask.setBounds (0, getHeight() - kFooterH, getWidth(), kFooterH);
    }

private:
    struct Mask : juce::Component
    {
        Mask()
        {
            setOpaque (true);
            // MUST pass clicks through — header mask sits over the
            // recentered preset cluster.
            setInterceptsMouseClicks (false, false);
        }
        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff181818));
        }
    };

    static void hideHqControls (juce::Component& root)
    {
        for (auto* child : root.getChildren())
        {
            if (auto* cb = dynamic_cast<juce::ComboBox*> (child))
            {
                if (cb->getNumItems() == 3
                    && cb->getItemText (0) == "1x"
                    && cb->getItemText (1) == "2x"
                    && cb->getItemText (2) == "4x")
                {
                    cb->setVisible (false);
                    hideHqLabel (root, *cb);
                }
            }
            else
            {
                hideHqControls (*child);
            }
        }
    }

    static void hideHqLabel (juce::Component& root, const juce::Component& target)
    {
        const auto cbBounds = target.getBounds();
        for (auto* child : root.getChildren())
        {
            auto* lbl = dynamic_cast<juce::Label*> (child);
            if (lbl == nullptr || lbl->getText() != "HQ")
                continue;
            const auto lb = lbl->getBounds();
            const bool stacked = lb.getY() < cbBounds.getY()
                              && lb.getCentreX() > cbBounds.getX()
                              && lb.getCentreX() < cbBounds.getRight();
            if (stacked)
                lbl->setVisible (false);
        }
    }

    static void hideResizeHandle (juce::Component& root)
    {
        for (auto* child : root.getChildren())
            if (dynamic_cast<juce::ResizableCornerComponent*> (child) != nullptr)
                child->setVisible (false);
    }

    void applyChildOverrides (juce::Component& editor)
    {
        recenterPresetCluster();
        recenterRowTwoSelectors (editor);
    }

    // Structural detection — find Save/Del by text, then pick the
    // ComboBox horizontally adjacent and left of Save on the same row.
    // Item-count heuristic ("ComboBox with > 6 items") was unreliable —
    // depended on the donor finishing its async factory-list populate
    // before our ctor ran.
    void findPresetCluster (juce::Component& editor)
    {
        for (auto* child : editor.getChildren())
        {
            if (auto* btn = dynamic_cast<juce::TextButton*> (child))
            {
                if (btn->getButtonText() == "Save")     saveBtn = btn;
                else if (btn->getButtonText() == "Del") delBtn  = btn;
            }
        }
        if (auto* save = saveBtn.getComponent())
        {
            const int saveY = save->getY();
            const int saveX = save->getX();
            juce::ComboBox* best = nullptr;
            int bestRight = -1;
            for (auto* child : editor.getChildren())
            {
                auto* cb = dynamic_cast<juce::ComboBox*> (child);
                if (cb == nullptr) continue;
                if (std::abs (cb->getY() - saveY) > 12) continue;
                if (cb->getRight() > saveX + 6)         continue;
                if (cb->getRight() > bestRight)
                {
                    bestRight = cb->getRight();
                    best      = cb;
                }
            }
            if (best != nullptr) presetCombo = best;
        }
    }

    // Reserves Del's slot even when hidden so toggling Del's visibility
    // doesn't shift Save/preset off-center.
    void recenterPresetCluster()
    {
        if (presetCombo.getComponent() == nullptr || ownedEditor == nullptr) return;
        constexpr float kBaseWidth = 800.0f;
        const float scale = ownedEditor->getWidth() > 0
                                ? (float) ownedEditor->getWidth() / kBaseWidth
                                : 1.0f;
        const int presetW = (int) (190 * scale);
        const int saveW   = (int) (45  * scale);
        const int delW    = (int) (35  * scale);
        const int gap     = (int) (4   * scale);
        const int rowH    = (int) (26  * scale);
        const int headerH = (int) (50  * scale);

        const int totalW = presetW + gap + saveW + gap + delW;
        int x = (getWidth() - totalW) / 2;
        const int y = (headerH - rowH) / 2;

        presetCombo->setBounds (x, y, presetW, rowH); x += presetW + gap;
        if (saveBtn.getComponent() != nullptr) saveBtn->setBounds (x, y, saveW, rowH);
        x += saveW + gap;
        if (delBtn.getComponent()  != nullptr) delBtn ->setBounds (x, y, delW,  rowH);
    }

    // Donor puts row 2 (Signal Path + EQ Std) in cols 0-1 of 3, leaving
    // col 2 empty. Slide by half a column-width to centre under row 1.
    static void recenterRowTwoSelectors (juce::Component& editor)
    {
        juce::ComboBox* signalCombo = nullptr;
        juce::ComboBox* eqStdCombo  = nullptr;

        for (auto* child : editor.getChildren())
        {
            auto* cb = dynamic_cast<juce::ComboBox*> (child);
            if (cb == nullptr) continue;
            if (cb->getNumItems() == 4
                && cb->getItemText (0) == "Repro"
                && cb->getItemText (1) == "Sync"
                && cb->getItemText (2) == "Input"
                && cb->getItemText (3) == "Thru")
                signalCombo = cb;
            else if (cb->getNumItems() == 3
                && cb->getItemText (0) == "NAB"
                && cb->getItemText (1) == "CCIR"
                && cb->getItemText (2) == "AES")
                eqStdCombo = cb;
        }
        if (signalCombo == nullptr || eqStdCombo == nullptr) return;

        // colW ≈ rowW/3 so half-column = half the combo's width.
        const int shiftCombo = signalCombo->getWidth() / 2;
        signalCombo->setBounds (signalCombo->getBounds().translated (shiftCombo, 0));
        eqStdCombo ->setBounds (eqStdCombo ->getBounds().translated (shiftCombo, 0));

        // Labels shift by the same combo delta (not label width) so
        // they stay centred over their combos when widths differ.
        for (auto* child : editor.getChildren())
        {
            auto* lbl = dynamic_cast<juce::Label*> (child);
            if (lbl == nullptr) continue;
            const auto t = lbl->getText();
            if (t == "SIGNAL PATH" || t == "EQ STD")
                lbl->setBounds (lbl->getBounds().translated (shiftCombo, 0));
        }
    }

    std::unique_ptr<juce::AudioProcessorEditor> ownedEditor;
    Mask headerMask;
    Mask footerMask;
    // findPresetCluster runs once. SafePointers don't re-discover if
    // the donor rebuilds its child tree (it doesn't today). A deleted
    // component reads as nullptr — cluster vanishes from the header
    // until reopened, no crash. Add a ComponentListener
    // (componentChildrenChanged -> re-run discovery) if this breaks.
    juce::Component::SafePointer<juce::ComboBox>   presetCombo;
    juce::Component::SafePointer<juce::TextButton> saveBtn;
    juce::Component::SafePointer<juce::TextButton> delBtn;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TapeMachineModalEditor)
};
} // namespace duskstudio
