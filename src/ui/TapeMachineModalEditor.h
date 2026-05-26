#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>

namespace duskstudio
{
// Hosts a TapeMachine plugin editor inside Dusk Studio's gear modal and trims
// down the parts of its UI that don't belong in the embedded context:
//
//   • Painted "TapeMachine" nameplate + "Vintage Tape Emulation" subtitle
//     are covered by an opaque header overlay; the painted "Dusk Audio"
//     footer is covered by a matching footer overlay. We can't suppress
//     the editor's paint() without modifying donor sources, so we mask.
//   • HQ oversampling combo + label are hidden — global oversampling
//     comes from Audio Settings, so a per-plugin override would just
//     fight the host's setting.
//   • The corner resize handle is hidden — the modal is sized once when
//     it opens; resize gestures don't make sense inside a centered modal.
//   • Preset selector + Save / Del buttons are recentered horizontally
//     in the header (donor pins them flush right; we want them on-axis).
//   • Signal Path + EQ Std selectors are recentered under the 3-column
//     row above (donor places them in the leftmost two of three columns,
//     which looks lopsided once the row only has two items).
//
// Located by traversal — the donor's children don't have stable IDs, so
// we match by combo items / button text / label text. After the donor's
// resized() runs (triggered by our editor->setBounds), we apply the
// overrides on top.
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
        // Reparent the preset cluster onto the wrapper, AFTER the masks, so
        // it ends up on top in z-order. Without this the full-width header
        // mask covers the painted nameplate AND the cluster — the cluster
        // becomes invisible "dead space" in the header. Bounds are set
        // each resized() pass.
        if (presetCombo != nullptr) addAndMakeVisible (presetCombo.getComponent());
        if (saveBtn     != nullptr) addAndMakeVisible (saveBtn.getComponent());
        if (delBtn      != nullptr) addAndMakeVisible (delBtn.getComponent());
    }

    void resized() override
    {
        if (ownedEditor == nullptr) return;

        ownedEditor->setBounds (getLocalBounds());
        applyChildOverrides (*ownedEditor);

        // Donor's own resized() runs DURING ownedEditor->setBounds and
        // sometimes cascades async layout that re-pins the preset
        // cluster to the right edge after applyChildOverrides finishes
        // synchronously. Re-run the recenter on the next message-loop
        // tick so we win the final position. Cheap (one setBounds per
        // cached pointer) and harmless if the donor already left them
        // where we put them.
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
            // Header mask sits over the preset cluster after recentering, so
            // it MUST pass clicks through to the children below it (which
            // are the editor's preset combo + Save / Del buttons re-parented
            // by our overrides). Footer mask doesn't need clicks either way.
            setInterceptsMouseClicks (false, false);
        }
        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff181818));
        }
    };

    // ─── Hides ──────────────────────────────────────────────────────────

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

    // ─── Overrides applied after donor resized() ─────────────────────────

    void applyChildOverrides (juce::Component& editor)
    {
        recenterPresetCluster();
        recenterRowTwoSelectors (editor);
    }

    // Identifies the preset combo + Save / Del buttons by content. Called
    // once at construction; pointers cached for resized() to reposition.
    //
    // Detection is structural, not item-count-based: the previous heuristic
    // ("ComboBox with > 6 items") relied on the donor having finished
    // populating its factory list before our ctor ran. If the donor was
    // still empty / async-loading, the preset combo wasn't identified and
    // recentering silently skipped. Now we find Save/Del by text first,
    // then pick the ComboBox horizontally adjacent to (and left of) Save
    // on the same Y row — works regardless of whether the dropdown has any
    // items in it at the moment.
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

    // Lays out the cached cluster centered horizontally in the wrapper's
    // header band. Run AFTER the donor's resized() (which would otherwise
    // pin them flush right). Reserves Del's slot even when Del is hidden,
    // so toggling Del's visibility doesn't shift Save/preset off-center.
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

    // Donor places the second selector row (Signal Path + EQ Std) in the
    // leftmost two of three columns, leaving the right column empty. Slide
    // the pair right by half a column-width so they sit centered under
    // the row above (Machine / Speed / Tape Type).
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

        // Half-column shift: combo width is roughly colW = rowW/3, so a
        // half-column shift is half the combo's own width.
        const int shiftCombo = signalCombo->getWidth() / 2;
        signalCombo->setBounds (signalCombo->getBounds().translated (shiftCombo, 0));
        eqStdCombo ->setBounds (eqStdCombo ->getBounds().translated (shiftCombo, 0));

        // Their two stacked labels travel with the combos — shift by the
        // same delta the combos moved (combo width / 2), not the label's
        // own width, so labels stay centered over their combos when label
        // and combo widths differ.
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
    // findPresetCluster runs once at construction; the SafePointers below
    // do NOT trigger re-discovery if the donor rebuilds its child tree.
    // We rely on TapeMachine's editor not swapping these controls out at
    // runtime (it doesn't today). The SafePointers exist so a deleted
    // component is observed as nullptr instead of dangling — if the donor
    // ever starts rebuilding, the cluster simply disappears from the
    // header until the modal is reopened, with no crash. Add a
    // ComponentListener (componentChildrenChanged → re-run findPresetCluster
    // + recenterPresetCluster) here if that assumption ever breaks.
    juce::Component::SafePointer<juce::ComboBox>   presetCombo;
    juce::Component::SafePointer<juce::TextButton> saveBtn;
    juce::Component::SafePointer<juce::TextButton> delBtn;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TapeMachineModalEditor)
};
} // namespace duskstudio
