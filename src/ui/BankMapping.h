#pragma once

#include "../session/Session.h"

#include <algorithm>

namespace duskstudio
{
// The console pages its 24 strips in width-derived screen pages (stride 6..24),
// while bank-relative MIDI bindings and the MCU surface address a fixed
// kNumBanks x kBankSize hardware bank. The two indices only coincide when the
// stride happens to be kBankSize, so they must be translated, never shared:
// a screen page index stored raw resolves to base tracks past kNumTracks and
// leaves the surface on a bank that does not exist.

inline int hardwareBankForScreenBank (int screenBank, int screenStride) noexcept
{
    if (screenStride <= 0) return 0;
    const int firstTrack = std::max (0, screenBank) * screenStride;
    return std::clamp (firstTrack / Session::kBankSize, 0, Session::kNumBanks - 1);
}

inline int screenBankForHardwareBank (int hardwareBank,
                                      int screenStride,
                                      int screenBankCount) noexcept
{
    if (screenStride <= 0 || screenBankCount <= 1) return 0;
    const int firstTrack = std::clamp (hardwareBank, 0, Session::kNumBanks - 1)
                         * Session::kBankSize;
    return std::clamp (firstTrack / screenStride, 0, screenBankCount - 1);
}
} // namespace duskstudio
