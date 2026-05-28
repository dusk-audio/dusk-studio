#pragma once

#include <cstddef>
#include <cstdint>

namespace duskstudio::mcu
{
// Mackie Control Universal protocol constants. Sources: Mackie LCU
// Developer Toolkit + "Logic Control 1.0" PDF. Shared by McuReceiver
// (decode) and McuController (emit).

// 8 physical strips per bank. Master fader is index 8 in MCU numbering.
constexpr int kStripsPerBank     = 8;
constexpr int kMasterFaderIndex  = 8;
constexpr int kPitchBendMaxValue = 0x3FFF;

// All buttons on MIDI channel 1 (0x90). Velocity 0x7F = lit / pressed,
// 0x00 = dark / released.
namespace btn
{
    // Per-channel (N = 0..7)
    constexpr int RecArmBase = 0x00; // 0..7
    constexpr int SoloBase   = 0x08; // 8..15
    constexpr int MuteBase   = 0x10; // 16..23
    constexpr int SelectBase = 0x18; // 24..31
    constexpr int VPotPushBase = 0x20; // 32..39

    // Assign modes (lit one-at-a-time)
    constexpr int AssignTrack  = 0x28;
    constexpr int AssignSend   = 0x29;
    constexpr int AssignPan    = 0x2A;
    constexpr int AssignPlugin = 0x2B;
    constexpr int AssignEq     = 0x2C;
    constexpr int AssignInst   = 0x2D;

    constexpr int BankLeft    = 0x2E;
    constexpr int BankRight   = 0x2F;
    constexpr int ChannelLeft  = 0x30;
    constexpr int ChannelRight = 0x31;

    constexpr int Rewind      = 0x5B;
    constexpr int FastForward = 0x5C;
    constexpr int Stop        = 0x5D;
    constexpr int Play        = 0x5E;
    constexpr int Record      = 0x5F;
    constexpr int Loop        = 0x56;
    constexpr int Punch       = 0x57;  // "Marker" repurposed for punch toggle

    constexpr int FaderTouchBase   = 0x68; // 104..111 per-strip
    constexpr int FaderTouchMaster = 0x70; // 112
}

namespace cc
{
    // V-pot rotate: 7-bit signed delta. Bit 6 = direction (0=CW, 1=CCW),
    // bits 0..5 = magnitude 1..63. Encoder ticks, not absolute.
    constexpr int VPotRotateBase = 0x10; // 16..23 (strips 0..7)

    // Emit only: encodes lit-LED pattern + mode (single / pan / fill /
    // bar). See `vring` below.
    constexpr int VPotRingBase = 0x30; // 48..55

    constexpr int JogWheel = 0x3C;
}

// Mackie sysex prefix: F0 00 00 66 14 ...
// 0x12 = LCD (offset + ASCII)
// 0x10 = timecode (10 7-bit chars)
// 0x20 = device-info request (host -> controller; we don't emit)
namespace sysex
{
    constexpr std::uint8_t kPrefix[]     = { 0xF0, 0x00, 0x00, 0x66, 0x14 };
    constexpr std::size_t  kPrefixLen    = sizeof (kPrefix) / sizeof (kPrefix[0]);
    constexpr std::uint8_t kEnd          = 0xF7;
    constexpr std::uint8_t kCmdLcd       = 0x12;
    constexpr std::uint8_t kCmdTimecode  = 0x10;

    // 2 rows × 56 cols. Row 0 at offset 0, row 1 at offset 56.
    // Each strip owns 7 chars per row (8 × 7 = 56).
    constexpr int kLcdCols      = 56;
    constexpr int kLcdRowBytes  = 56;
    constexpr int kLcdCharsPerStrip = 7;
    constexpr int kLcdRow0Addr  = 0;
    constexpr int kLcdRow1Addr  = 56;

    constexpr int kTimecodeDigits = 10;
}

// Channel pressure (0xD0). Data byte: channel in high nibble (0..7),
// level 0..14 in low nibble (15 = clip hold).
namespace meter
{
    constexpr std::uint8_t kStatus = 0xD0;
    constexpr int kMaxLevel  = 14;
    constexpr int kClipLevel = 15;
}

// High nibble of CC value = visual style. Low nibble (1..11) = which
// LED in the ring is highlighted.
namespace vring
{
    constexpr int ModeSingle      = 0 << 4;  // single dot
    constexpr int ModeBoost       = 1 << 4;  // pan: dot from centre
    constexpr int ModeWrap        = 2 << 4;  // fill from left
    constexpr int ModeSpread      = 3 << 4;  // fill from centre, symmetric
    constexpr int DotCenter       = 0x40;
    constexpr int kLedCount       = 11;
}
} // namespace duskstudio::mcu
