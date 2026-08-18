#include "ConsoleView.h"

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

int ConsoleView::allTracksContentWidth() const
{
    // Width at which all 24 strips fit at the MINIMUM strip width with
    // buses + master at their MIN widths. Above this we drop banking
    // entirely and show every track at once. Using min (not ref) makes
    // "show all" trigger as aggressively as the layout allows
    // without violating the no-shrink-below-kMin rule.
    return consolelayout::allTracksContentWidth();
}

int ConsoleView::channelsThatFitForWidth (int componentWidth) noexcept
{
    // Available width MINUS the bus + master column (always anchored
    // right) and their inter-strip + section gaps. Divide by the
    // per-channel slot (strip + gap) to get the count.
    //
    return consolelayout::channelsThatFitForWidth (componentWidth);
}

int ConsoleView::channelsThatFit() const
{
    return channelsThatFitForWidth (getWidth());
}

int ConsoleView::numBanksForWidth (int componentWidth) noexcept
{
    return consolelayout::screenPageCountForWidth (componentWidth);
}

int ConsoleView::numBanks() const noexcept
{
    return numBanksForWidth (getWidth());
}

void ConsoleView::synchroniseBankStateForWidth (int componentWidth) noexcept
{
    bankState.resize (numBanksForWidth (componentWidth),
                      channelsThatFitForWidth (componentWidth));
}

int ConsoleView::bankStride() const noexcept
{
    return channelsThatFit();
}

std::pair<int, int> ConsoleView::rangeForBankAtWidth (int bankIndex,
                                                        int componentWidth) noexcept
{
    return consolelayout::channelRangeForPage (bankIndex, componentWidth);
}

std::pair<int, int> ConsoleView::rangeForBank (int bankIndex) const noexcept
{
    return rangeForBankAtWidth (bankIndex, getWidth());
}

void ConsoleView::setBank (int bankIndex)
{
    applyBankTransition (bankState.selectScreenBank (bankIndex, numBanks(), bankStride()));
}

void ConsoleView::applyBankTransition (const BankTransition& transition)
{
    if (transition.publishActiveBank)
        sessionRef.activeBank.store (transition.hardwareBank, std::memory_order_relaxed);
    if (transition.publishSurfaceBank)
        sessionRef.mcu.bank.store (transition.hardwareBank, std::memory_order_release);
    if (transition.pageMoved)
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
    // thread. This tick is the only place that reads it, and - together with an
    // explicit page press - the only place that writes it, so a surface press
    // always gets seen before any screen-derived move goes out. A press landing
    // between the load and the exchange wins it: the exchange fails, the
    // surface keeps its position, and the next tick follows it.
    const int mcuBank = sessionRef.mcu.bank.load (std::memory_order_acquire);
    applyBankTransition (bankState.pollTick (
        mcuBank, numBanks(), bankStride(),
        [this] (int expected, int desired)
        {
            return sessionRef.mcu.bank.compare_exchange_strong (expected, desired,
                                                                std::memory_order_release,
                                                                std::memory_order_relaxed);
        }));
}

void ConsoleView::updateBankVisibility()
{
    const int stride = bankStride();
    for (int i = 0; i < Session::kNumTracks; ++i)
    {
        const int bank = (stride > 0) ? (i / stride) : 0;
        strips[(size_t) i]->setVisible (showingAllTracks || bank == bankState.screenBank);
    }
}

void ConsoleView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff121214));
}

void ConsoleView::resized()
{
    auto area = getLocalBounds().reduced (6);

    // "Show all" trigger: window wide enough to seat every channel
    // strip at kMinChannelWidth alongside the always-anchored bus +
    // master column. Below this we fall back to dynamic banking - bank
    // stride = channelsThatFit().
    showingAllTracks = (getWidth() >= allTracksContentWidth());

    const auto channelFit = consolelayout::channelFitForWidth (getWidth());
    const int stride = channelFit.channels;
    // Re-derives the page against the new width and queues any surface move it
    // implies for the poll tick. Publishes nothing itself - a drag-resize
    // storing mcu.bank here would swallow a Bank Left/Right press.
    synchroniseBankStateForWidth (getWidth());
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
        const int firstIdx = bankState.screenBank * stride;
        visibleChannels = std::min (stride, Session::kNumTracks - firstIdx);
    }

    // Channels stay at *reference* width unless even that won't fit. The shared
    // geometry then compacts them as needed, including below the documented
    // per-strip floor at legal responsive widths. We do not scale up: extra
    // horizontal space stays as whitespace on the right.
    //
    // Scale based on the FULL-BANK width (stride * kRefChannelWidth) rather
    // than visibleChannels - so a sparse last bank uses the SAME channel
    // width as a full bank. Without this, bank 2 with only 2 strips would
    // be wider than bank 1 with 14 strips on the same window: bank 2's
    // refTotal would fit and skip the scale-down branch.
    const int widthRefChannels = showingAllTracks ? Session::kNumTracks : stride;
    const auto geometry = consolelayout::makeStripGeometry (
        getWidth(), widthRefChannels, visibleChannels, channelFit.density);
    const int channelW = geometry.channelWidth;
    const int busW     = geometry.busWidth;
    const int masterW  = geometry.masterWidth;

    const int y = area.getY();
    const int h = area.getHeight();

    for (auto& strip : strips)
        if (strip != nullptr)
            strip->setHorizontalDensity (channelFit.density);

    // Buses + master ANCHORED to the right edge. Channel strips fill
    // from the left up to `visibleChannels`; any leftover horizontal
    // room sits as a flex gap between the channel column and the bus
    // column. Sparse last banks therefore show the strips left-aligned
    // (visible space to the right) rather than stretching widths.
    int x = geometry.contentLeft;
    for (int i = 0; i < visibleChannels; ++i)
    {
        const int trackIdx = showingAllTracks
                              ? i
                              : (bankState.screenBank * stride + i);
        if (trackIdx >= Session::kNumTracks) break;
        strips[(size_t) trackIdx]->setBounds (x, y, channelW, h);
        x += channelW + (i + 1 < visibleChannels ? geometry.stripGap : 0);
    }

    x = geometry.busColumnLeft;
    for (int i = 0; i < Session::kNumBuses; ++i)
    {
        busStrips[(size_t) i]->setBounds (x, y, busW, h);
        x += busW + (i + 1 < Session::kNumBuses ? geometry.stripGap : 0);
    }
    x += geometry.sectionGap;
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
        if (stride > 0) setBank (screenBankForTrack (track, stride, numBanks()));
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
