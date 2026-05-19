#include "ConsoleView.h"

namespace focal
{
void ConsoleView::dropAllPluginEditors()
{
    for (auto& strip : strips)
        if (strip != nullptr)
            strip->dropPluginEditor();
}

ConsoleView::ConsoleView (Session& session, AudioEngine& engine) : sessionRef (session)
{
    for (int i = 0; i < Session::kNumTracks; ++i)
    {
        strips[(size_t) i] = std::make_unique<ChannelStripComponent> (
            i, session.track (i), session, engine.getStrip (i).getPluginSlot(), engine);
        addAndMakeVisible (strips[(size_t) i].get());
    }
    for (int i = 0; i < Session::kNumBuses; ++i)
    {
        busStrips[(size_t) i] = std::make_unique<BusComponent> (
            session.bus (i), session, i);
        addAndMakeVisible (busStrips[(size_t) i].get());
    }
    masterStrip = std::make_unique<MasterStripComponent> (
        session.master(),
        session,
#if FOCAL_HAS_DUSK_DSP
        &engine.getMasterBus().getTapeProcessor());
#else
        nullptr);
#endif
    addAndMakeVisible (masterStrip.get());

    // BANK A/B controls were previously rendered here as a 28-px row at the
    // top of ConsoleView. They moved up to MainComponent (under the stage
    // selector) so the channel strips get the full vertical body for taller
    // faders. ConsoleView now owns only the bank-state model + visibility.
    updateBankVisibility();
}

int ConsoleView::minimumContentWidth()
{
    const int gaps = (Session::kBankSize - 1) * kStripGap
                   + kSectionGap
                   + (Session::kNumBuses - 1) * kStripGap
                   + kSectionGap;
    return Session::kBankSize * kMinChannelWidth
         + Session::kNumBuses * kMinBusWidth
         + kMinMasterWidth
         + gaps
         + 12;  // outer padding
}

int ConsoleView::fixedWidthFor16Tracks() const
{
    // Width required to lay out all 16 channels + 4 buses + master at their
    // *reference* widths - this is the threshold above which we drop the
    // banking and show every track at once.
    const int gaps = (Session::kNumTracks - 1) * kStripGap
                   + kSectionGap
                   + (Session::kNumBuses - 1) * kStripGap
                   + kSectionGap;
    return Session::kNumTracks * kRefChannelWidth
         + Session::kNumBuses * kRefBusWidth
         + kRefMasterWidth
         + gaps
         + 12;  // outer padding match
}

void ConsoleView::setBank (int bankIndex)
{
    bankIndex = juce::jlimit (0, Session::kNumBanks - 1, bankIndex);
    if (bankIndex == currentBank) return;
    currentBank = bankIndex;

    updateBankVisibility();
    resized();

    // The MainComponent owns the BANK A / BANK B buttons; ask the parent
    // chain to refresh its layout so its toggle states stay in sync.
    if (auto* parent = getParentComponent())
        parent->resized();
}

void ConsoleView::updateBankVisibility()
{
    for (int i = 0; i < Session::kNumTracks; ++i)
    {
        const int bank = i / Session::kBankSize;
        strips[(size_t) i]->setVisible (showingAllTracks || bank == currentBank);
    }
}

void ConsoleView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff121214));
}

void ConsoleView::resized()
{
    auto area = getLocalBounds().reduced (6);

    // Decide whether to show all 16 tracks at reference width (windows that
    // are wide enough - typically fullscreen on a 1080p+ display) or fall
    // back to the banked-8 layout that scales to fit narrower windows.
    showingAllTracks = (area.getWidth() >= fixedWidthFor16Tracks() - 12);
    updateBankVisibility();

    const int visibleChannels = showingAllTracks ? Session::kNumTracks : Session::kBankSize;

    // Channels stay at *reference* width unless even that won't fit - in which
    // case we scale down, but only as far as the per-strip minimum. We do not
    // scale up: extra horizontal space stays as whitespace on the right.
    const int gaps = (visibleChannels - 1) * kStripGap
                   + kSectionGap
                   + (Session::kNumBuses - 1) * kStripGap
                   + kSectionGap;
    const int availForStrips = juce::jmax (0, area.getWidth() - gaps);
    const int refTotal = visibleChannels * kRefChannelWidth
                       + Session::kNumBuses * kRefBusWidth
                       + kRefMasterWidth;

    int channelW = kRefChannelWidth;
    int busW     = kRefBusWidth;
    int masterW  = kRefMasterWidth;

    if (availForStrips < refTotal)
    {
        // Window is narrower than the reference - shrink proportionally.
        const float scale = (float) availForStrips / (float) refTotal;
        channelW = juce::jmax (kMinChannelWidth, (int) std::round (kRefChannelWidth * scale));
        busW     = juce::jmax (kMinBusWidth,     (int) std::round (kRefBusWidth     * scale));
        masterW  = juce::jmax (kMinMasterWidth,  (int) std::round (kRefMasterWidth  * scale));

        // Secondary fit-to-budget pass: if the kMin floors pushed total
        // above availForStrips, shrink CHANNELS first (they have the most
        // budget and channel-strip widgets tolerate cramping better than
        // the master's 5-knob comp row). Only spill into bus + master
        // shrinks once channels can't compress any further. Master is
        // protected the longest because its 5 × 40 px comp knob row will
        // clip the rightmost knob the instant the strip goes below 210.
        const auto totalOf = [&]
        {
            return visibleChannels * channelW
                 + Session::kNumBuses * busW
                 + masterW;
        };
        if (totalOf() > availForStrips)
        {
            int overflow = totalOf() - availForStrips;
            // Step 1: shrink channels (floor 1 px).
            if (overflow > 0 && visibleChannels > 0)
            {
                const int channelGiveable = juce::jmax (0, channelW - 1);
                // Ceiling division so the per-channel shrink covers the
                // remainder without the extra +1 that overshoots when
                // overflow divides evenly.
                const int eachReducible   = juce::jmin (
                    (overflow + visibleChannels - 1) / visibleChannels,
                    channelGiveable);
                channelW -= eachReducible;
                overflow = totalOf() - availForStrips;
            }
            // Step 2: shrink buses if channels couldn't absorb everything.
            if (overflow > 0 && Session::kNumBuses > 0)
            {
                const int busGiveable   = juce::jmax (0, busW - 1);
                const int eachReducible = juce::jmin (
                    (overflow + Session::kNumBuses - 1) / Session::kNumBuses,
                    busGiveable);
                busW -= eachReducible;
                overflow = totalOf() - availForStrips;
            }
            // Step 3: as a last resort, shrink master (the comp row will
            // start clipping past here).
            if (overflow > 0)
                masterW = juce::jmax (1, masterW - overflow);
        }
        // After all three steps, channelW / busW / masterW are clamped
        // to a minimum of 1 via juce::jmax, so totalOf() can still
        // exceed availForStrips in pathologically narrow windows
        // (visibleChannels + Session::kNumBuses + 1 px per strip is
        // the hard floor). That case is treated as a window-sizing
        // bug; minimumContentWidth() returns the threshold below
        // which the OS-enforced resize floor should never let the
        // user drag the window, so we don't add a runtime guard here.
        // If you see this trigger at runtime, raise the resize-limit
        // floor in MainWindow rather than papering over it in layout.
    }

    int x = area.getX();
    const int y = area.getY();
    const int h = area.getHeight();

    for (int i = 0; i < visibleChannels; ++i)
    {
        const int trackIdx = showingAllTracks
                              ? i
                              : (currentBank * Session::kBankSize + i);
        if (trackIdx >= Session::kNumTracks) break;
        strips[(size_t) trackIdx]->setBounds (x, y, channelW, h);
        x += channelW + (i + 1 < visibleChannels ? kStripGap : 0);
    }

    x += kSectionGap;
    for (int i = 0; i < Session::kNumBuses; ++i)
    {
        busStrips[(size_t) i]->setBounds (x, y, busW, h);
        x += busW + (i + 1 < Session::kNumBuses ? kStripGap : 0);
    }

    x += kSectionGap;
    masterStrip->setBounds (x, y, masterW, h);

    // Auto-engage SUMMARY when the strip's vertical space is too short for
    // the fader (EQ + COMP eat fixed pixels), or when the strip width has
    // been pushed well below the kMin floor by the secondary scaling pass.
    // Recomputed every layout pass; fires only when the auto flag actually
    // changes so we don't thrash applyCompactState() (which calls
    // setCompactMode on every strip) each resize. Visibility of EQ/COMP
    // children is an internal detail of ChannelStripComponent.
    const bool wantAutoCompact = (h < kAutoCompactStripHeight)
                              || (channelW < kAutoCompactChannelWidth);
    if (autoCompact != wantAutoCompact)
    {
        autoCompact = wantAutoCompact;
        applyCompactState();
    }
}

void ConsoleView::setStripsCompactMode (bool compact)
{
    userWantsCompact = compact;
    applyCompactState();
}

void ConsoleView::applyCompactState()
{
    const bool compact = userWantsCompact || autoCompact;
    for (auto& strip : strips)
        if (strip != nullptr)
            strip->setCompactMode (compact);
    // Bus + master strips don't collapse their EQ/COMP sections, but
    // they shrink their VU meters in compact mode so the fader keeps a
    // workable height when the tape SUMMARY is consuming vertical room.
    for (auto& bus : busStrips)
        if (bus != nullptr)
            bus->setCompactVu (compact);
    if (masterStrip != nullptr)
        masterStrip->setCompactVu (compact);
}

void ConsoleView::setStripsMixingMode (bool mixing)
{
    for (auto& strip : strips)
        if (strip != nullptr)
            strip->setMixingMode (mixing);
}

void ConsoleView::setOnStripFocusRequested (std::function<void (int)> cb)
{
    for (auto& strip : strips)
        if (strip != nullptr)
            strip->onTrackFocusRequested = cb;
}
} // namespace focal
