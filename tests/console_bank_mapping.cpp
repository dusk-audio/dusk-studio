#include <catch2/catch_test_macros.hpp>

#include "ui/BankMapping.h"

using namespace duskstudio;

// Screen pages are bankStride()-wide (channelsThatFit(), clamped to [6, 24]);
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
    // channelsThatFitForWidth floors the stride at ceil(kNumTracks / 4), so the
    // console can build a 4th page. Stored raw, that page index names a bank
    // that does not exist: past kNumBanks for the surface, and past kNumTracks
    // once the audio thread multiplies it by kBankSize.
    constexpr int kNarrowestStride = 6;
    constexpr int kMostPages       = 4;

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

TEST_CASE ("Bank mapping: a resize republishes the clamped page hardware bank",
           "[console][bank]")
{
    // Page 3 at stride 6 begins at track 18 (hardware bank 2). Widening to
    // stride 12 leaves only two pages, so the screen clamps to page 1 and all
    // screen-owned hardware-bank state must follow its first track (track 12).
    REQUIRE (hardwareBankForScreenBank (3, 6) == 2);
    const auto state = screenBankStateAfterResize (3, 2, 12);

    REQUIRE (state.currentBank == 1);
    REQUIRE (state.lastKnownMcuBank == 1);
    REQUIRE (state.activeBank == 1);
    REQUIRE (state.mcuBank == 1);
}
