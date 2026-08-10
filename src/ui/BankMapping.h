#pragma once

#include "../session/SessionLayout.h"

#include <algorithm>

namespace duskstudio
{
// The console pages its 24 strips in width-derived screen pages (stride 3..24),
// while bank-relative MIDI bindings and the MCU surface address a fixed
// kNumBanks x kBankSize hardware bank. The two indices only coincide when the
// stride happens to be kBankSize, so they must be translated, never shared:
// a screen page index stored raw resolves to base tracks past kNumTracks and
// leaves the surface on a bank that does not exist.

inline int hardwareBankForScreenBank (int screenBank, int screenStride) noexcept
{
    if (screenStride <= 0) return 0;
    const int firstTrack = std::max (0, screenBank) * screenStride;
    return std::clamp (firstTrack / SessionLayout::kBankSize,
                       0,
                       SessionLayout::kNumBanks - 1);
}

inline int screenBankForHardwareBank (int hardwareBank,
                                      int screenStride,
                                      int screenBankCount) noexcept
{
    if (screenStride <= 0 || screenBankCount <= 1) return 0;
    const int firstTrack = std::clamp (hardwareBank, 0, SessionLayout::kNumBanks - 1)
                         * SessionLayout::kBankSize;
    return std::clamp (firstTrack / screenStride, 0, screenBankCount - 1);
}

inline int screenBankForTrack (int track, int screenStride, int screenBankCount) noexcept
{
    if (screenStride <= 0 || screenBankCount <= 1) return 0;
    track = std::clamp (track, 0, SessionLayout::kNumTracks - 1);
    return std::clamp (track / screenStride, 0, screenBankCount - 1);
}

inline int screenPageForDigitKey (int keyCode, int screenPageCount) noexcept
{
    if (keyCode < '1' || keyCode > '8') return -1;
    const int page = keyCode - '1';
    return page < screenPageCount ? page : -1;
}

// What the console must publish to the session atoms after a transition.
// hardwareBank is only meaningful when one of the publish flags is set.
struct BankTransition
{
    bool pageMoved            = false;  // the visible page changed; re-lay out
    bool publishActiveBank    = false;  // store session.activeBank
    bool publishSurfaceBank   = false;  // store session.mcu.bank unconditionally
    int  hardwareBank         = 0;
};

// The console's page/bank bookkeeping, kept out of the component so the
// resize / poll / page-press ordering is testable without a message loop.
//
// session.mcu.bank is written on this side by the poll tick or an explicit page
// press - never a resize. A resize only queues the move it wants in
// pendingMcuBank: storing it directly would overwrite, and then latch away, a
// Bank Left/Right press that landed inside the poll window, racing the
// surface's own read-modify-write of the same atom.
struct ConsoleBankState
{
    int  screenBank       = 0;   // page on screen, 0..screenBankCount-1
    int  lastKnownMcuBank = 0;   // last surface bank this side has seen or set
    bool followsScreen    = false;
    int  pendingMcuBank   = -1;  // surface move a resize queued, -1 when none

    // The user picked a screen page (or focus walked onto one).
    BankTransition selectScreenBank (int page, int screenBankCount, int screenStride) noexcept
    {
        page = std::clamp (page, 0, std::max (0, screenBankCount - 1));
        // Must stay a no-op when the page doesn't move: the console names the
        // page already on screen on every strip click and arrow-key move, and
        // moving the surface from there would snap it away under the user with
        // no on-screen feedback, once per click.
        if (page == screenBank) return {};

        screenBank = page;

        BankTransition t;
        t.pageMoved = true;

        // A page is stride-wide (3..24); the surface and the bank-relative
        // bindings are kBankSize-wide, so the page index is never a valid bank
        // - publish the hardware bank holding the page's first track instead.
        // With every track on screen there is only one page and it carries no
        // bank opinion at all, so the surface keeps wherever its own Bank
        // Left/Right left it.
        if (screenBankCount > 1)
        {
            t.hardwareBank = hardwareBankForScreenBank (page, screenStride);
            t.publishActiveBank = true;
            t.publishSurfaceBank = true;
            // Latch before the caller stores: the poll tick mirrors mcu.bank
            // back into the screen page, and the hardware bank rarely lines up
            // with the page. Unlatched, our own store reads back as a surface
            // move and drags the view off the page the user just picked.
            lastKnownMcuBank = t.hardwareBank;
            followsScreen = true;
            pendingMcuBank = -1;
        }
        return t;
    }

    // The window changed width, so both the page count and the stride are new.
    // Deliberately publishes nothing.
    void resize (int screenBankCount, int screenStride) noexcept
    {
        // A page index only means anything against the stride that produced it,
        // so a width too narrow to hold the index destroys the record of what
        // the user picked. What survives is the hardware bank the console
        // published for them, which makes the surface authoritative again -
        // clamping the index instead would drag the surface to whatever the
        // truncated index resolves to, purely because the window changed size.
        if (followsScreen
            && screenBank != std::clamp (screenBank, 0, std::max (0, screenBankCount - 1)))
            followsScreen = false;

        if (! followsScreen)
        {
            // The surface's position is width-independent, so the page moves
            // back onto the tracks it drives. Clamping alone strands the two
            // apart: a widen-then-narrow leaves the page at 0 with mcu.bank
            // untouched, and the poll tick has no change to notice.
            screenBank = screenBankForHardwareBank (lastKnownMcuBank, screenStride, screenBankCount);
            pendingMcuBank = -1;
            return;
        }

        // The page still names a real position and only the stride under it
        // moved, which moves which hardware bank the page sits in. Recomputed
        // every time, so a resize that lands back on the current bank also
        // retires a request an earlier one queued.
        const int derived = (screenBankCount > 1)
                          ? hardwareBankForScreenBank (screenBank, screenStride)
                          : lastKnownMcuBank;
        pendingMcuBank = (derived != lastKnownMcuBank) ? derived : -1;
    }

    // A poll tick carrying the value just read from session.mcu.bank.
    //
    // moveSurfaceBank(expected, desired) must move the surface atom from
    // expected to desired atomically and report whether it won. A press can
    // land between that read and this write - the audio thread's own
    // read-modify-write is not atomic against ours either - and the surface's
    // position outranks a move a resize queued, so losing the exchange drops
    // the queued move and leaves the latch behind for the next tick to follow.
    // Only the queued move is conditional: a page press stores unconditionally,
    // because the user is watching for the surface to follow their click.
    template <typename MoveSurfaceBank>
    BankTransition pollTick (int mcuBank, int screenBankCount, int screenStride,
                             MoveSurfaceBank&& moveSurfaceBank)
    {
        BankTransition t;

        if (mcuBank != lastKnownMcuBank)
        {
            // The surface moved itself, which outranks anything a resize
            // queued. Its bank is echoed into the binding base but never back
            // into mcu.bank: the surface owns its own position, and the page it
            // maps onto maps back to a different hardware bank whenever the
            // stride isn't kBankSize.
            pendingMcuBank = -1;
            lastKnownMcuBank = mcuBank;
            followsScreen = false;

            t.publishActiveBank = true;
            t.hardwareBank = std::clamp (mcuBank, 0, SessionLayout::kNumBanks - 1);

            const int page = screenBankForHardwareBank (mcuBank, screenStride, screenBankCount);
            if (page != screenBank)
            {
                screenBank = page;
                t.pageMoved = true;
            }
            return t;
        }

        if (pendingMcuBank < 0) return t;

        const int queued = pendingMcuBank;
        // Stay queued when the exchange loses: the surface moved between the
        // read and the compare, and if it moves back to the latched bank before
        // the next tick nothing else would ever repair the page.
        if (! moveSurfaceBank (lastKnownMcuBank, queued))
            return t;

        pendingMcuBank = -1;

        t.hardwareBank = queued;
        t.publishActiveBank = true;
        lastKnownMcuBank = queued;
        followsScreen = true;
        return t;
    }
};
} // namespace duskstudio
