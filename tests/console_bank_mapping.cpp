#include <catch2/catch_test_macros.hpp>

#include "ui/BankMapping.h"
#include "ui/ConsoleLayout.h"

using namespace duskstudio;

// Screen pages are bankStride()-wide (channelsThatFit(), clamped to [3, 24]);
// hardware banks are always kBankSize-wide and there are always kNumBanks of
// them. Each case below names the page's first track, which is what decides
// the hardware bank it resolves to.

TEST_CASE ("Bank mapping: screen page resolves to the bank holding its first track",
           "[console][bank]")
{
    SECTION ("stride 6 - four pages fold onto three banks")
    {
        REQUIRE (hardwareBankForScreenBank (0, 6) == 0);   // first track 0
        REQUIRE (hardwareBankForScreenBank (1, 6) == 0);   // first track 6
        REQUIRE (hardwareBankForScreenBank (2, 6) == 1);   // first track 12
        REQUIRE (hardwareBankForScreenBank (3, 6) == 2);   // first track 18
    }

    SECTION ("stride 7 - four pages")
    {
        REQUIRE (hardwareBankForScreenBank (0, 7) == 0);   // first track 0
        REQUIRE (hardwareBankForScreenBank (1, 7) == 0);   // first track 7
        REQUIRE (hardwareBankForScreenBank (2, 7) == 1);   // first track 14
        REQUIRE (hardwareBankForScreenBank (3, 7) == 2);   // first track 21
    }

    SECTION ("stride 8 - the one stride where page and bank are the same index")
    {
        REQUIRE (hardwareBankForScreenBank (0, 8) == 0);
        REQUIRE (hardwareBankForScreenBank (1, 8) == 1);
        REQUIRE (hardwareBankForScreenBank (2, 8) == 2);
    }

    SECTION ("stride 9 - three pages")
    {
        REQUIRE (hardwareBankForScreenBank (0, 9) == 0);   // first track 0
        REQUIRE (hardwareBankForScreenBank (1, 9) == 1);   // first track 9
        REQUIRE (hardwareBankForScreenBank (2, 9) == 2);   // first track 18
    }

    SECTION ("stride 12 - two pages")
    {
        REQUIRE (hardwareBankForScreenBank (0, 12) == 0);  // first track 0
        REQUIRE (hardwareBankForScreenBank (1, 12) == 1);  // first track 12
    }

    SECTION ("stride 16 - two pages, bank 1 never starts one")
    {
        REQUIRE (hardwareBankForScreenBank (0, 16) == 0);  // first track 0
        REQUIRE (hardwareBankForScreenBank (1, 16) == 2);  // first track 16
    }

    SECTION ("stride 24 - no banking, the single page starts at bank 0")
    {
        REQUIRE (hardwareBankForScreenBank (0, SessionLayout::kNumTracks) == 0);
    }
}

TEST_CASE ("Bank mapping: hardware bank resolves to the page showing its first track",
           "[console][bank]")
{
    SECTION ("stride 6, four pages")
    {
        REQUIRE (screenBankForHardwareBank (0, 6, 4) == 0);   // track 0  -> page 0
        REQUIRE (screenBankForHardwareBank (1, 6, 4) == 1);   // track 8  -> page 1
        REQUIRE (screenBankForHardwareBank (2, 6, 4) == 2);   // track 16 -> page 2
    }

    SECTION ("stride 7, four pages")
    {
        REQUIRE (screenBankForHardwareBank (0, 7, 4) == 0);   // track 0  -> page 0
        REQUIRE (screenBankForHardwareBank (1, 7, 4) == 1);   // track 8  -> page 1
        REQUIRE (screenBankForHardwareBank (2, 7, 4) == 2);   // track 16 -> page 2
    }

    SECTION ("stride 8, three pages - identity")
    {
        REQUIRE (screenBankForHardwareBank (0, 8, 3) == 0);
        REQUIRE (screenBankForHardwareBank (1, 8, 3) == 1);
        REQUIRE (screenBankForHardwareBank (2, 8, 3) == 2);
    }

    SECTION ("stride 9, three pages - banks 0 and 1 share page 0")
    {
        REQUIRE (screenBankForHardwareBank (0, 9, 3) == 0);   // track 0  -> page 0
        REQUIRE (screenBankForHardwareBank (1, 9, 3) == 0);   // track 8  -> page 0
        REQUIRE (screenBankForHardwareBank (2, 9, 3) == 1);   // track 16 -> page 1
    }

    SECTION ("stride 12, two pages")
    {
        REQUIRE (screenBankForHardwareBank (0, 12, 2) == 0);  // track 0  -> page 0
        REQUIRE (screenBankForHardwareBank (1, 12, 2) == 0);  // track 8  -> page 0
        REQUIRE (screenBankForHardwareBank (2, 12, 2) == 1);  // track 16 -> page 1
    }

    SECTION ("stride 16, two pages")
    {
        REQUIRE (screenBankForHardwareBank (0, 16, 2) == 0);  // track 0  -> page 0
        REQUIRE (screenBankForHardwareBank (1, 16, 2) == 0);  // track 8  -> page 0
        REQUIRE (screenBankForHardwareBank (2, 16, 2) == 1);  // track 16 -> page 1
    }

    SECTION ("stride 24 - every bank maps to the single page")
    {
        for (int hw = 0; hw < SessionLayout::kNumBanks; ++hw)
            REQUIRE (screenBankForHardwareBank (hw, SessionLayout::kNumTracks, 1) == 0);
    }
}

TEST_CASE ("Bank mapping: indices past the end of a range clamp into it", "[console][bank]")
{
    SECTION ("a page past the last track can't name a bank past the last bank")
    {
        // First track 24 - one past kNumTracks, so the raw quotient is 3.
        REQUIRE (hardwareBankForScreenBank (3, 8) == SessionLayout::kNumBanks - 1);
        REQUIRE (hardwareBankForScreenBank (4, 6) == SessionLayout::kNumBanks - 1);
        // First track 32 - raw quotient 4.
        REQUIRE (hardwareBankForScreenBank (2, 16) == SessionLayout::kNumBanks - 1);
    }

    SECTION ("a bank past the last page clamps to the last page")
    {
        // Track 16 over a 2-page console: raw quotient 2, one past the last page.
        REQUIRE (screenBankForHardwareBank (2, 8, 2) == 1);
        REQUIRE (screenBankForHardwareBank (2, 6, 2) == 1);
    }

    SECTION ("a bank past kNumBanks clamps before it is resolved")
    {
        REQUIRE (screenBankForHardwareBank (SessionLayout::kNumBanks, 8, 3) == 2);
    }

    SECTION ("negative and zero inputs")
    {
        REQUIRE (hardwareBankForScreenBank (-1, 8) == 0);
        REQUIRE (hardwareBankForScreenBank (1, 0) == 0);
        REQUIRE (screenBankForHardwareBank (-1, 8, 3) == 0);
        REQUIRE (screenBankForHardwareBank (1, 0, 4) == 0);
    }
}

TEST_CASE ("Bank mapping: the widest page count the console builds stays in range",
           "[console][bank]")
{
    constexpr int kNarrowestStride = consolelayout::kMinStride;
    constexpr int kMostPages       = consolelayout::kMaxScreenPages;

    for (int page = 0; page < kMostPages; ++page)
    {
        const int hw = hardwareBankForScreenBank (page, kNarrowestStride);
        REQUIRE (hw >= 0);
        REQUIRE (hw < SessionLayout::kNumBanks);
        REQUIRE (hw * SessionLayout::kBankSize + SessionLayout::kBankSize
                 <= SessionLayout::kNumTracks);
    }

    REQUIRE (hardwareBankForScreenBank (kMostPages - 1, kNarrowestStride) == 2);
}

TEST_CASE ("Console layout: default width preserves the channel minimum",
           "[console][layout]")
{
    // A 1440 px MainComponent gives the console the local width minus its
    // 8 px outer inset on both sides.
    constexpr int kDefaultConsoleWidth = 1440 - 16;
    REQUIRE (consolelayout::channelsThatFitForWidth (kDefaultConsoleWidth) == 3);
    REQUIRE (consolelayout::screenPageCountForWidth (kDefaultConsoleWidth) == 8);
}

TEST_CASE ("Console layout: full-floor page width preserves every documented minimum",
           "[console][layout]")
{
    constexpr int width = consolelayout::fullFloorPageContentWidth();
    REQUIRE (consolelayout::channelsThatFitForWidth (width) == consolelayout::kMinStride);
    REQUIRE (width == consolelayout::kMinStride * consolelayout::kMinChannelWidth
                   + (consolelayout::kMinStride - 1) * consolelayout::kStripGap
                   + consolelayout::rightColumnWidth()
                   + consolelayout::kOuterPadding);

    const auto geometry = consolelayout::makeStripGeometry (
        width, consolelayout::kMinStride, consolelayout::kMinStride);
    REQUIRE (geometry.channelWidth >= consolelayout::kMinChannelWidth);
    REQUIRE (geometry.busWidth >= consolelayout::kMinBusWidth);
    REQUIRE (geometry.masterWidth >= consolelayout::kMinMasterWidth);
    REQUIRE (geometry.channelsClearBus());
    REQUIRE (geometry.lastChannelRight + consolelayout::kSectionGap
             == geometry.busColumnLeft);
}

TEST_CASE ("Console layout: forced sub-minimum bounds never overlap Bus 1",
           "[console][layout]")
{
    constexpr int forcedWidth = consolelayout::fullFloorPageContentWidth() - 64;
    const auto geometry = consolelayout::makeStripGeometry (
        forcedWidth, consolelayout::kMinStride, consolelayout::kMinStride);

    REQUIRE (geometry.channelWidth < consolelayout::kMinChannelWidth);
    REQUIRE (geometry.channelsClearBus());
    REQUIRE (geometry.lastChannelRight + geometry.sectionGap
             <= geometry.busColumnLeft);
}

TEST_CASE ("Console layout: eight narrow pages cover every channel exactly",
           "[console][layout]")
{
    constexpr int width = consolelayout::fullFloorPageContentWidth();
    const std::pair<int, int> expected[] = {
        { 1, 3 }, { 4, 6 }, { 7, 9 }, { 10, 12 },
        { 13, 15 }, { 16, 18 }, { 19, 21 }, { 22, 24 }
    };

    REQUIRE (consolelayout::screenPageCountForWidth (width) == 8);
    for (int page = 0; page < 8; ++page)
        REQUIRE (consolelayout::channelRangeForPage (page, width) == expected[page]);
}

TEST_CASE ("Console layout: responsive window floor keeps three strips clear of Bus 1",
           "[console][layout]")
{
    constexpr int mainWidth = consolelayout::responsiveMinimumMainComponentWidth();
    constexpr int consoleWidth = consolelayout::responsiveMinimumContentWidth();
    static_assert (mainWidth == consoleWidth + consolelayout::kMainPadding * 2);
    static_assert (mainWidth == 1162);

    const auto geometry = consolelayout::makeStripGeometry (
        consoleWidth, consolelayout::kMinStride, consolelayout::kMinStride);
    REQUIRE (consolelayout::screenPageCountForWidth (consoleWidth) == 8);
    REQUIRE (geometry.channelsClearBus());
    REQUIRE (geometry.lastChannelRight + geometry.sectionGap
             <= geometry.busColumnLeft);

    const int bandWidth = consolelayout::compactPageButtonBandWidthForMainWidth (mainWidth);
    const auto buttons = consolelayout::fitPageButtons (bandWidth, 8, 70, 6);
    REQUIRE (bandWidth == consolelayout::minimumPageButtonBandWidth());
    REQUIRE (buttons.width == consolelayout::kMinPageButtonWidth);
    REQUIRE (buttons.gap == consolelayout::kMinPageButtonGap);
    REQUIRE (buttons.fits (bandWidth));
}

TEST_CASE ("Console layout: 1366 display startup stays screen-fit and usable",
           "[console][layout]")
{
    constexpr int displayWidth = 1366;
    constexpr int mainTargetWidth = displayWidth - 24;
    constexpr int consoleTargetWidth = mainTargetWidth - consolelayout::kMainPadding * 2;
    static_assert (mainTargetWidth == 1342);
    static_assert (consoleTargetWidth == 1326);

    REQUIRE (consolelayout::responsiveMinimumMainComponentWidth() <= mainTargetWidth);
    REQUIRE (consolelayout::screenPageCountForWidth (consoleTargetWidth) == 8);

    const auto geometry = consolelayout::makeStripGeometry (
        consoleTargetWidth, consolelayout::kMinStride, consolelayout::kMinStride);
    REQUIRE (geometry.channelsClearBus());
    REQUIRE (geometry.lastChannelRight + geometry.sectionGap
             <= geometry.busColumnLeft);

    const int bandWidth = consolelayout::compactPageButtonBandWidthForMainWidth (mainTargetWidth);
    const auto buttons = consolelayout::fitPageButtons (bandWidth, 8, 70, 6);
    REQUIRE (bandWidth == 450);
    REQUIRE (buttons.width >= consolelayout::kMinPageButtonWidth);
    REQUIRE (buttons.fits (bandWidth));
}

TEST_CASE ("Bank mapping: every three-strip page resolves to its exact hardware bank",
           "[console][bank]")
{
    constexpr int expected[] = { 0, 0, 0, 1, 1, 1, 2, 2 };
    for (int page = 0; page < 8; ++page)
        REQUIRE (hardwareBankForScreenBank (page, 3) == expected[page]);
}

TEST_CASE ("Bank mapping: focused tracks cross three-strip page boundaries",
           "[console][bank]")
{
    REQUIRE (screenBankForTrack (0, 3, 8) == 0);
    REQUIRE (screenBankForTrack (2, 3, 8) == 0);
    REQUIRE (screenBankForTrack (3, 3, 8) == 1);
    REQUIRE (screenBankForTrack (20, 3, 8) == 6);
    REQUIRE (screenBankForTrack (21, 3, 8) == 7);
    REQUIRE (screenBankForTrack (23, 3, 8) == 7);
}

TEST_CASE ("Bank mapping: plain digits select exactly pages one through eight",
           "[console][bank]")
{
    for (int digit = '1'; digit <= '8'; ++digit)
        REQUIRE (screenPageForDigitKey (digit, 8) == digit - '1');

    REQUIRE (screenPageForDigitKey ('0', 8) == -1);
    REQUIRE (screenPageForDigitKey ('9', 8) == -1);
    REQUIRE (screenPageForDigitKey ('8', 7) == -1);
}

// The remaining cases drive ConsoleBankState the way the console does: a page
// press or poll tick hands back a BankTransition the caller stores into the
// session atoms, while a resize stores nothing. Console holds the two atoms so
// a sequence can assert what actually reached the surface.
namespace
{
struct Console
{
    ConsoleBankState state;
    int activeBankAtom = 0;
    int mcuBankAtom    = 0;   // also written by the surface, on the audio thread
    int pages          = 3;
    int stride         = SessionLayout::kBankSize;
    // Runs a press between the tick's read of mcuBankAtom and its exchange,
    // which is the window the exchange exists to close.
    bool pressDuringExchange = false;

    void apply (const BankTransition& t)
    {
        if (t.publishActiveBank)  activeBankAtom = t.hardwareBank;
        if (t.publishSurfaceBank) mcuBankAtom    = t.hardwareBank;
        if (t.pageMoved)          state.resize (pages, stride);   // console relayout
    }

    void pressPage (int page)     { apply (state.selectScreenBank (page, pages, stride)); }
    void resizeTo (int p, int s)  { pages = p; stride = s; state.resize (pages, stride); }

    void poll()
    {
        apply (state.pollTick (mcuBankAtom, pages, stride,
                               [this] (int expected, int desired)
                               {
                                   if (pressDuringExchange) surfaceBankRight();
                                   if (mcuBankAtom != expected) return false;
                                   mcuBankAtom = desired;
                                   return true;
                               }));
    }

    // Bank Right on the surface, handled on the audio thread.
    void surfaceBankRight()
    {
        if (mcuBankAtom < SessionLayout::kNumBanks - 1) ++mcuBankAtom;
    }
};
} // namespace

TEST_CASE ("Console bank state: a resize that only relocates the page queues the move",
           "[console][bank]")
{
    // The page index still names a real position, so the screen keeps the bank
    // and the surface follows the tracks that moved under the page: page 2
    // begins at track 16 at stride 8, and at track 12 once the stride is 6.
    Console c;
    c.pages = 3;
    c.stride = SessionLayout::kBankSize;
    c.pressPage (2);
    REQUIRE (c.mcuBankAtom == 2);
    REQUIRE (c.state.followsScreen);

    c.resizeTo (4, 6);
    REQUIRE (c.state.screenBank == 2);
    // Queued, not stored: the poll tick owns the write.
    REQUIRE (c.state.pendingMcuBank == 1);
    REQUIRE (c.mcuBankAtom == 2);

    c.poll();
    REQUIRE (c.mcuBankAtom == 1);
    REQUIRE (c.activeBankAtom == 1);
    REQUIRE (c.state.lastKnownMcuBank == 1);
    REQUIRE (c.state.pendingMcuBank == -1);
}

TEST_CASE ("Console bank state: page eight normalises before a five-strip layout",
           "[console][bank]")
{
    Console c;
    c.pages = 8;
    c.stride = 3;
    c.pressPage (7);
    REQUIRE (c.state.screenBank == 7);
    REQUIRE (c.mcuBankAtom == 2);

    // The selected page no longer exists. The fixed hardware bank survives,
    // and its first track is immediately mapped into the new page geometry.
    c.resizeTo (5, 5);
    REQUIRE (c.state.screenBank == 3);
    REQUIRE_FALSE (c.state.followsScreen);
    REQUIRE (c.state.pendingMcuBank == -1);
}

TEST_CASE ("Console bank state: surface Bank Left and Right map at stride three",
           "[console][bank]")
{
    Console c;
    c.pages = 8;
    c.stride = 3;

    c.surfaceBankRight();
    c.poll();
    REQUIRE (c.activeBankAtom == 1);
    REQUIRE (c.state.screenBank == 2);

    c.surfaceBankRight();
    c.poll();
    REQUIRE (c.activeBankAtom == 2);
    REQUIRE (c.state.screenBank == 5);

    --c.mcuBankAtom;
    c.poll();
    REQUIRE (c.activeBankAtom == 1);
    REQUIRE (c.state.screenBank == 2);
}

TEST_CASE ("Console bank state: a widen that destroys the page hands the bank to the surface",
           "[console][bank]")
{
    // Page 3 at stride 6 begins at track 18 (hardware bank 2). One page cannot
    // hold that index, so the record of what the user picked is gone and the
    // published hardware bank is what survives. Clamping the index instead
    // would drag the surface off bank 2 on the way back down.
    REQUIRE (hardwareBankForScreenBank (3, 6) == 2);

    Console c;
    c.pages = 4;
    c.stride = 6;
    c.pressPage (3);
    REQUIRE (c.mcuBankAtom == 2);
    REQUIRE (c.state.followsScreen);

    c.resizeTo (1, SessionLayout::kNumTracks);
    REQUIRE_FALSE (c.state.followsScreen);
    REQUIRE (c.state.screenBank == 0);
    REQUIRE (c.state.pendingMcuBank == -1);
    REQUIRE (c.mcuBankAtom == 2);

    c.resizeTo (3, SessionLayout::kBankSize);
    REQUIRE (c.state.screenBank == 2);
    REQUIRE (c.state.pendingMcuBank == -1);
    REQUIRE (c.mcuBankAtom == 2);

    c.poll();
    REQUIRE (c.state.screenBank == 2);
    REQUIRE (c.mcuBankAtom == 2);
}

TEST_CASE ("Console bank state: a press landing inside the exchange keeps the surface",
           "[console][bank]")
{
    // The queued move reads mcu.bank on the tick and writes it a few
    // instructions later. A press in between must win the atom, and must not be
    // latched away - the latch is what the next tick compares against.
    Console c;
    c.pages = 3;
    c.stride = SessionLayout::kBankSize;
    c.pressPage (1);                 // track 8 -> bank 1
    REQUIRE (c.mcuBankAtom == 1);

    c.resizeTo (4, 6);               // page 1 -> track 6 -> bank 0
    REQUIRE (c.state.pendingMcuBank == 0);

    c.pressDuringExchange = true;
    c.poll();
    REQUIRE (c.mcuBankAtom == 2);
    REQUIRE (c.state.lastKnownMcuBank == 1);
    // The move stays queued: were the surface to come back to bank 1 before the
    // next tick, dropping it here would strand the page with nothing to repair it.
    REQUIRE (c.state.pendingMcuBank == 0);
    REQUIRE (c.state.followsScreen);

    c.pressDuringExchange = false;
    c.poll();
    REQUIRE_FALSE (c.state.followsScreen);
    REQUIRE (c.state.lastKnownMcuBank == 2);
    REQUIRE (c.activeBankAtom == 2);
    REQUIRE (c.mcuBankAtom == 2);
    REQUIRE (c.state.screenBank == screenBankForHardwareBank (2, 6, 4));
}

TEST_CASE ("Console bank state: a widen-then-narrow re-derives the page from the surface bank",
           "[console][bank]")
{
    // The surface owns the bank here, so its position is width-independent and
    // the screen page is what has to move. Clamping alone leaves the screen on
    // tracks 1-8 while the surface drives 17-24, and the poll tick cannot
    // repair it: mcu.bank never changed, so there is nothing for it to notice.
    Console c;
    c.pages = 3;
    c.stride = SessionLayout::kBankSize;
    c.mcuBankAtom = 2;
    c.poll();

    REQUIRE_FALSE (c.state.followsScreen);
    REQUIRE (c.state.screenBank == 2);
    REQUIRE (c.activeBankAtom == 2);

    c.resizeTo (1, SessionLayout::kNumTracks);   // widen: every track on one page
    REQUIRE (c.state.screenBank == 0);

    c.resizeTo (3, SessionLayout::kBankSize);    // narrow back
    REQUIRE (c.state.screenBank == 2);
    REQUIRE (c.state.pendingMcuBank == -1);
    REQUIRE (c.mcuBankAtom == 2);

    // A poll tick after the resize must stay a no-op - nothing moved.
    c.poll();
    REQUIRE (c.state.screenBank == 2);
    REQUIRE (c.mcuBankAtom == 2);
}

TEST_CASE ("Console bank state: a narrower stride re-derives the page for the same surface bank",
           "[console][bank]")
{
    Console c;
    c.pages = 3;
    c.stride = SessionLayout::kBankSize;
    c.mcuBankAtom = 1;
    c.poll();
    REQUIRE (c.state.screenBank == 1);

    // Bank 1 starts at track 8, which at stride 6 sits on page 1 (tracks 6-11).
    c.resizeTo (4, 6);
    REQUIRE (c.state.screenBank == screenBankForHardwareBank (1, 6, 4));
    REQUIRE (c.mcuBankAtom == 1);
}

TEST_CASE ("Console bank state: a resize inside the poll window cannot swallow a bank press",
           "[console][bank]")
{
    // The surface writes mcu.bank on the audio thread and the console only
    // notices on its 50 ms tick. A drag-resize firing in that window must not
    // store over the press, nor latch it away by advancing lastKnownMcuBank.
    Console c;
    c.pages = 2;
    c.stride = 12;
    c.pressPage (1);                 // page 1 at stride 12 -> track 12 -> bank 1
    REQUIRE (c.mcuBankAtom == 1);
    REQUIRE (c.state.lastKnownMcuBank == 1);

    c.surfaceBankRight();            // audio thread: bank 1 -> 2
    c.resizeTo (4, 6);               // page 1 at stride 6 -> track 6 -> bank 0

    REQUIRE (c.mcuBankAtom == 2);              // the press survives the resize
    REQUIRE (c.state.lastKnownMcuBank == 1);   // still unseen, so the tick will see it
    REQUIRE (c.state.pendingMcuBank == 0);     // the resize only queued its move

    c.poll();
    REQUIRE (c.mcuBankAtom == 2);              // surface outranks the queued move
    REQUIRE (c.activeBankAtom == 2);
    REQUIRE (c.state.lastKnownMcuBank == 2);
    REQUIRE (c.state.pendingMcuBank == -1);
    REQUIRE_FALSE (c.state.followsScreen);
    REQUIRE (c.state.screenBank == screenBankForHardwareBank (2, 6, 4));
}

TEST_CASE ("Console bank state: a page press latches its own store", "[console][bank]")
{
    // Page 1 at stride 12 resolves to hardware bank 1, which maps back to page
    // 0. Unlatched, the next tick would read our own store as a surface move
    // and drag the view off the page the user just picked.
    Console c;
    c.pages = 2;
    c.stride = 12;
    c.pressPage (1);

    REQUIRE (c.state.screenBank == 1);
    REQUIRE (c.state.lastKnownMcuBank == 1);
    REQUIRE (screenBankForHardwareBank (1, 12, 2) == 0);

    c.poll();
    REQUIRE (c.state.screenBank == 1);
}

TEST_CASE ("Console bank state: a page press retires a move an earlier resize queued",
           "[console][bank]")
{
    Console c;
    c.pages = 3;
    c.stride = SessionLayout::kBankSize;
    c.pressPage (2);                 // track 16 -> bank 2
    c.resizeTo (4, 6);               // page 2 -> track 12 -> bank 1
    REQUIRE (c.state.pendingMcuBank == 1);

    c.pressPage (0);                 // user picks page 0 before the tick
    REQUIRE (c.mcuBankAtom == 0);
    REQUIRE (c.state.pendingMcuBank == -1);

    c.poll();
    REQUIRE (c.mcuBankAtom == 0);
}

TEST_CASE ("Console bank state: a resize back onto the current bank queues nothing",
           "[console][bank]")
{
    Console c;
    c.pages = 3;
    c.stride = SessionLayout::kBankSize;
    c.pressPage (1);                 // track 8 -> bank 1
    REQUIRE (c.mcuBankAtom == 1);

    c.resizeTo (2, 12);              // page 1 -> track 12 -> still bank 1
    REQUIRE (c.state.pendingMcuBank == -1);

    c.poll();
    REQUIRE (c.mcuBankAtom == 1);
    REQUIRE (c.activeBankAtom == 1);
}

TEST_CASE ("Console bank state: one page carries no bank opinion", "[console][bank]")
{
    // Every track on screen: the screen has no page to impose, so the surface
    // keeps wherever its own Bank Left/Right left it.
    Console c;
    c.pages = 1;
    c.stride = SessionLayout::kNumTracks;
    c.mcuBankAtom = 2;
    c.poll();
    REQUIRE (c.activeBankAtom == 2);
    REQUIRE (c.state.screenBank == 0);

    c.state.screenBank = 1;          // stale page left by a narrower width
    c.pressPage (0);
    REQUIRE (c.state.screenBank == 0);
    REQUIRE (c.mcuBankAtom == 2);
    REQUIRE_FALSE (c.state.followsScreen);

    c.resizeTo (1, SessionLayout::kNumTracks);
    REQUIRE (c.state.pendingMcuBank == -1);

    c.poll();
    REQUIRE (c.mcuBankAtom == 2);
}
