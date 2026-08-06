#include "ConsoleView.h"

#include "BankMapping.h"

#include <algorithm>

namespace duskstudio
{
void ConsoleView::dropAllPluginEditors (NativeEditorTeardown teardown)
{
    for (auto& strip : strips)
        if (strip != nullptr)
            strip->dropPluginEditor (teardown);
}

ConsoleView::ConsoleView (Session& session, AudioEngine& engine) : sessionRef (session)
{
    // Follow the MCU surface's bank (set on the audio thread by the Bank
    // Left/Right buttons). Fires on the message loop, after this ctor.
    startTimerHz (20);

    for (int i = 0; i < Session::kNumTracks; ++i)
    {
        strips[(size_t) i] = std::make_unique<ChannelStripComponent> (
            i, session.track (i), session, engine.getStrip (i).getPluginSlot(), engine);
        addAndMakeVisible (strips[(size_t) i].get());
    }
    for (int i = 0; i < Session::kNumBuses; ++i)
    {
        busStrips[(size_t) i] = std::make_unique<BusComponent> (
            session.bus (i), session, engine, i);
        addAndMakeVisible (busStrips[(size_t) i].get());
    }
    masterStrip = std::make_unique<MasterStripComponent> (
        session.master(),
        session,
        engine);
    addAndMakeVisible (masterStrip.get());

    // ConsoleView owns only the bank-state model + visibility. The BANK A/B
    // controls live in MainComponent (under the stage selector) so the channel
    // strips get the full vertical body for taller faders.
    updateBankVisibility();
}

int ConsoleView::minimumContentWidth()
{
    // Floor: at least ONE channel strip + buses + master + their gaps.
    // Narrower than this and the layout has no useful state to land in;
    // MainWindow's resize limit takes the larger of this and the OS
    // minimum, so the user can't drag the window below it.
    const int gaps = kSectionGap
                   + (Session::kNumBuses - 1) * kStripGap
                   + kSectionGap;
    return /*one channel*/ kMinChannelWidth
         + Session::kNumBuses * kMinBusWidth
         + kMinMasterWidth
         + gaps
         + 12;  // outer padding
}

int ConsoleView::fixedWidthFor16Tracks() const
{
    // Width at which all 16 strips fit at the MINIMUM strip width with
    // buses + master at their MIN widths. Above this we drop banking
    // entirely and show every track at once. Using min (not ref) makes
    // "show all 16" trigger as aggressively as the layout allows
    // without violating the no-shrink-below-kMin rule.
    const int gaps = (Session::kNumTracks - 1) * kStripGap
                   + kSectionGap
                   + (Session::kNumBuses - 1) * kStripGap
                   + kSectionGap;
    return Session::kNumTracks * kMinChannelWidth
         + Session::kNumBuses * kMinBusWidth
         + kMinMasterWidth
         + gaps
         + 12;
}

int ConsoleView::channelsThatFitForWidth (int componentWidth) noexcept
{
    // Available width MINUS the bus + master column (always anchored
    // right) and their inter-strip + section gaps. Divide by the
    // per-channel slot (strip + gap) to get the count.
    //
    // Lower bound is ceil(kNumTracks / kMaxBanks) so the resulting bank
    // count never exceeds kMaxBanks. Without this, a snap-narrow window
    // could produce 6-16 bank buttons (16 tracks / 3-1 per bank),
    // overflowing the bank-row UI. Going wider than the strip's natural
    // min on a very narrow window is the lesser evil.
    constexpr int kMaxBanks = 4;
    constexpr int kMinStride = (Session::kNumTracks + kMaxBanks - 1) / kMaxBanks;
    const int outerPad  = 12;
    const int rightCol  = Session::kNumBuses * kMinBusWidth
                        + (Session::kNumBuses - 1) * kStripGap
                        + kSectionGap     // gap between channels and buses
                        + kSectionGap     // gap between buses and master
                        + kMinMasterWidth;
    const int avail     = std::max (0, componentWidth - outerPad - rightCol);
    // Per-strip slot = strip width + inter-channel gap. We add one gap
    // back so the LAST strip doesn't reserve a trailing gap.
    const int perSlot   = kMinChannelWidth + kStripGap;
    if (perSlot <= 0 || avail <= 0) return kMinStride;
    const int fit = (avail + kStripGap) / perSlot;
    return std::clamp (fit, kMinStride, Session::kNumTracks);
}

int ConsoleView::channelsThatFit() const
{
    return channelsThatFitForWidth (getWidth());
}

int ConsoleView::numBanksForWidth (int componentWidth) noexcept
{
    const int fit = channelsThatFitForWidth (componentWidth);
    if (fit >= Session::kNumTracks) return 1;
    return (Session::kNumTracks + fit - 1) / fit;
}

int ConsoleView::numBanks() const noexcept
{
    return numBanksForWidth (getWidth());
}

int ConsoleView::bankStride() const noexcept
{
    return channelsThatFit();
}

std::pair<int, int> ConsoleView::rangeForBankAtWidth (int bankIndex,
                                                        int componentWidth) noexcept
{
    const int stride = channelsThatFitForWidth (componentWidth);
    const int first  = bankIndex * stride;
    const int last   = std::min (Session::kNumTracks - 1, first + stride - 1);
    return { first + 1, last + 1 };
}

std::pair<int, int> ConsoleView::rangeForBank (int bankIndex) const noexcept
{
    return rangeForBankAtWidth (bankIndex, getWidth());
}

void ConsoleView::setBank (int bankIndex)
{
    const int pages = numBanks();
    bankIndex = std::clamp (bankIndex, 0, std::max (0, pages - 1));
    // Must stay a no-op when the page doesn't move: focusStrip calls this on
    // every strip click and arrow-key move, naming the page already on screen.
    // Moving the surface from there would snap it away under the user with no
    // on-screen feedback, once per click.
    if (bankIndex == currentBank) return;
    currentBank = bankIndex;

    // A page is stride-wide (6..24); the surface and the bank-relative bindings
    // are kBankSize-wide, so the page index is never a valid bank - publish the
    // hardware bank holding the page's first track instead. With every track on
    // screen there is only one page and it carries no bank opinion at all, so
    // the surface keeps wherever its own Bank Left/Right left it.
    if (pages > 1)
    {
        const int hardwareBank = hardwareBankForScreenBank (currentBank, bankStride());
        // Latch before the store: timerCallback mirrors mcu.bank back into the
        // screen page, and the hardware bank rarely lines up with the page.
        // Without the latch our own store reads back as a surface move and
        // drags the view off the page the user just picked.
        lastKnownMcuBank = hardwareBank;
        activeBankFollowsScreen = true;
        sessionRef.activeBank.store (hardwareBank, std::memory_order_relaxed);
        sessionRef.mcu.bank.store (hardwareBank, std::memory_order_release);
    }

    applyBankChange();
}

void ConsoleView::applyBankChange()
{
    updateBankVisibility();
    resized();

    // The MainComponent owns the bank-button row; ask the parent chain
    // to refresh its layout so the toggle states stay in sync.
    if (auto* parent = getParentComponent())
        parent->resized();
}

void ConsoleView::timerCallback()
{
    // The MCU Bank Left/Right buttons write session.mcu.bank on the audio
    // thread. Mirror it into the bank-relative binding base and scroll the
    // screen onto the page holding those tracks. The mcu.bank store is
    // deliberately NOT echoed back: the surface owns its own position, and the
    // page it maps onto maps back to a different hardware bank whenever the
    // stride isn't kBankSize. Binding base follows even when the screen can't
    // page (window wide enough to show all 24 at once).
    const int mcuBank = sessionRef.mcu.bank.load (std::memory_order_acquire);
    if (mcuBank == lastKnownMcuBank) return;
    lastKnownMcuBank = mcuBank;
    activeBankFollowsScreen = false;
    sessionRef.activeBank.store (std::clamp (mcuBank, 0, Session::kNumBanks - 1),
                                 std::memory_order_relaxed);

    const int screenBank = screenBankForHardwareBank (mcuBank, bankStride(), numBanks());
    if (screenBank == currentBank) return;
    currentBank = screenBank;
    applyBankChange();
}

void ConsoleView::updateBankVisibility()
{
    const int stride = bankStride();
    for (int i = 0; i < Session::kNumTracks; ++i)
    {
        const int bank = (stride > 0) ? (i / stride) : 0;
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

    // "Show all 16" trigger: window wide enough to seat every channel
    // strip at kMinChannelWidth alongside the always-anchored bus +
    // master column. Below this we fall back to dynamic banking - bank
    // stride = channelsThatFit().
    showingAllTracks = (area.getWidth() >= fixedWidthFor16Tracks() - 12);

    const int stride = bankStride();
    const int pages  = numBanks();
    // Re-clamp the screen page against the (possibly changed) numBanks so a
    // width change doesn't leave us viewing an empty page past the end.
    const auto remappedBanks = screenBankStateAfterResize (currentBank, pages, stride);
    currentBank = remappedBanks.currentBank;

    // Both the page index and the stride under it are width-derived, so a
    // resize moves which hardware bank the page sits in - re-derive, or
    // bank-relative bindings keep driving whichever strips the OLD width put
    // under the page. When the screen owns the bank, keep the MCU latch, binding
    // base, and surface bank together; with a single page the screen has no bank
    // opinion to impose in the first place.
    if (pages > 1 && activeBankFollowsScreen)
    {
        lastKnownMcuBank = remappedBanks.lastKnownMcuBank;
        sessionRef.activeBank.store (remappedBanks.activeBank, std::memory_order_relaxed);
        sessionRef.mcu.bank.store (remappedBanks.mcuBank, std::memory_order_release);
    }
    updateBankVisibility();

    int visibleChannels;
    if (showingAllTracks)
    {
        visibleChannels = Session::kNumTracks;
    }
    else
    {
        // Sparse last bank gets only the remainder; non-final banks get
        // the full stride.
        const int firstIdx = currentBank * stride;
        visibleChannels = std::min (stride, Session::kNumTracks - firstIdx);
    }

    // Channels stay at *reference* width unless even that won't fit - in which
    // case we scale down, but only as far as the per-strip minimum. We do not
    // scale up: extra horizontal space stays as whitespace on the right.
    //
    // Scale based on the FULL-BANK width (stride * kRefChannelWidth) rather
    // than visibleChannels - so a sparse last bank uses the SAME channel
    // width as a full bank. Without this, bank 2 with only 2 strips would
    // be wider than bank 1 with 14 strips on the same window: bank 2's
    // refTotal would fit and skip the scale-down branch.
    const int widthRefChannels = showingAllTracks ? Session::kNumTracks : stride;
    const int gaps = (widthRefChannels - 1) * kStripGap
                   + kSectionGap
                   + (Session::kNumBuses - 1) * kStripGap
                   + kSectionGap;
    const int availForStrips = std::max (0, area.getWidth() - gaps);
    const int refTotal = widthRefChannels * kRefChannelWidth
                       + Session::kNumBuses * kRefBusWidth
                       + kRefMasterWidth;

    int channelW = kRefChannelWidth;
    int busW     = kRefBusWidth;
    int masterW  = kRefMasterWidth;

    if (availForStrips < refTotal)
    {
        // Window is narrower than the reference - shrink proportionally.
        const float scale = (float) availForStrips / (float) refTotal;
        channelW = std::max (kMinChannelWidth, (int) std::round (kRefChannelWidth * scale));
        busW     = std::max (kMinBusWidth,     (int) std::round (kRefBusWidth     * scale));
        masterW  = std::max (kMinMasterWidth,  (int) std::round (kRefMasterWidth  * scale));

        // Secondary fit-to-budget pass: if the kMin floors pushed total
        // above availForStrips, shrink CHANNELS first (they have the most
        // budget and channel-strip widgets tolerate cramping better than
        // the master's 5-knob comp row). Only spill into bus + master
        // shrinks once channels can't compress any further. Master is
        // protected the longest because its 5 × 40 px comp knob row will
        // clip the rightmost knob the instant the strip goes below 210.
        // Secondary fit pass uses widthRefChannels (the bank-stride
        // budget) - same reason as the primary scale: width stays
        // stable across banks regardless of how many strips are
        // visible RIGHT NOW.
        const auto totalOf = [&]
        {
            return widthRefChannels * channelW
                 + Session::kNumBuses * busW
                 + masterW;
        };
        if (totalOf() > availForStrips)
        {
            int overflow = totalOf() - availForStrips;
            // Step 1: shrink channels (floor 1 px).
            if (overflow > 0 && widthRefChannels > 0)
            {
                const int channelGiveable = std::max (0, channelW - 1);
                // Ceiling division so the per-channel shrink covers the
                // remainder without the extra +1 that overshoots when
                // overflow divides evenly.
                const int eachReducible   = std::min (
                    (overflow + widthRefChannels - 1) / widthRefChannels,
                    channelGiveable);
                channelW -= eachReducible;
                overflow = totalOf() - availForStrips;
            }
            // Step 2: shrink buses if channels couldn't absorb everything.
            if (overflow > 0 && Session::kNumBuses > 0)
            {
                const int busGiveable   = std::max (0, busW - 1);
                const int eachReducible = std::min (
                    (overflow + Session::kNumBuses - 1) / Session::kNumBuses,
                    busGiveable);
                busW -= eachReducible;
                overflow = totalOf() - availForStrips;
            }
            // Step 3: as a last resort, shrink master (the comp row will
            // start clipping past here).
            if (overflow > 0)
                masterW = std::max (1, masterW - overflow);
        }
        // After all three steps, channelW / busW / masterW are clamped
        // to a minimum of 1 via std::max, so totalOf() can still
        // exceed availForStrips in pathologically narrow windows
        // (visibleChannels + Session::kNumBuses + 1 px per strip is
        // the hard floor). That case is treated as a window-sizing
        // bug; minimumContentWidth() returns the threshold below
        // which the OS-enforced resize floor should never let the
        // user drag the window, so we don't add a runtime guard here.
        // If you see this trigger at runtime, raise the resize-limit
        // floor in MainWindow rather than papering over it in layout.
    }

    const int y = area.getY();
    const int h = area.getHeight();

    // Buses + master ANCHORED to the right edge. Channel strips fill
    // from the left up to `visibleChannels`; any leftover horizontal
    // room sits as a flex gap between the channel column and the bus
    // column. Sparse last banks therefore show the strips left-aligned
    // (visible space to the right) rather than stretching widths.
    const int busColW = Session::kNumBuses * busW
                      + (Session::kNumBuses - 1) * kStripGap;
    const int rightColW = busColW + kSectionGap + masterW;
    const int rightColX = area.getRight() - rightColW;

    int x = area.getX();
    for (int i = 0; i < visibleChannels; ++i)
    {
        const int trackIdx = showingAllTracks
                              ? i
                              : (currentBank * stride + i);
        if (trackIdx >= Session::kNumTracks) break;
        strips[(size_t) trackIdx]->setBounds (x, y, channelW, h);
        x += channelW + (i + 1 < visibleChannels ? kStripGap : 0);
    }

    x = rightColX;
    for (int i = 0; i < Session::kNumBuses; ++i)
    {
        busStrips[(size_t) i]->setBounds (x, y, busW, h);
        x += busW + (i + 1 < Session::kNumBuses ? kStripGap : 0);
    }
    x += kSectionGap;
    masterStrip->setBounds (x, y, masterW, h);

    // Auto-engage TIMELINE when the strip's vertical space is too short for
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
    // Bus + master strips collapse their EQ + COMP sections AND shrink
    // their VU meters when the tape TIMELINE consumes vertical room, so
    // the whole console reads as a single compact-state grammar across
    // channels / buses / master.
    for (auto& bus : busStrips)
        if (bus != nullptr)
        {
            bus->setCompactVu  (compact);
            bus->setCompactMode (compact);
        }
    if (masterStrip != nullptr)
    {
        masterStrip->setCompactVu  (compact);
        masterStrip->setCompactMode (compact);
    }
}

void ConsoleView::setStripsMixingMode (bool mixing)
{
    for (auto& strip : strips)
        if (strip != nullptr)
            strip->setMixingMode (mixing);
}

void ConsoleView::setOnStripFocusRequested (std::function<void (int)> cb)
{
    // Route every strip's focus request through focusStrip so a click updates
    // the focus ring the same way an arrow-key move does, then forward to the
    // external callback (which sets the A/S/X target selection).
    stripFocusCb = std::move (cb);
    for (auto& strip : strips)
        if (strip != nullptr)
            strip->onTrackFocusRequested = [this] (int t) { focusStrip (t); };
}

void ConsoleView::focusStrip (int track)
{
    if (track < 0 || track >= Session::kNumTracks) return;
    focusedStrip = track;

    // Bring the focused strip's bank into view when banking is active.
    if (! showingAllTracks)
    {
        const int stride = bankStride();
        if (stride > 0) setBank (track / stride);
    }
    repaint();
    if (stripFocusCb) stripFocusCb (track);
}

void ConsoleView::moveFocus (int delta)
{
    const int start = focusedStrip >= 0 ? focusedStrip : 0;
    focusStrip (std::clamp (start + delta, 0, Session::kNumTracks - 1));
}

void ConsoleView::paintOverChildren (juce::Graphics& g)
{
    if (focusedStrip < 0 || focusedStrip >= Session::kNumTracks) return;
    auto* strip = strips[(size_t) focusedStrip].get();
    if (strip == nullptr || ! strip->isVisible()) return;

    // Gold ring around the focused strip - the keyboard "I am here" signal.
    g.setColour (juce::Colour (0xffd0a050));
    g.drawRoundedRectangle (strip->getBounds().toFloat().reduced (1.0f), 4.0f, 2.0f);
}
} // namespace duskstudio
