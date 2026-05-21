#pragma once

#include <cstddef>
#include <cstdint>

namespace duskstudio::mcu
{
// Mackie Control Universal protocol constants. Numbers sourced from the
// Logic Control / Mackie Control public spec (Mackie LCU Developer Toolkit
// + the de-facto-standard "Logic Control 1.0" PDF). Used by both
// McuReceiver (decode incoming hardware events) and McuController (emit
// host->controller feedback). Keeping the constants in one place lets the
// decoder + emitter share the source of truth and makes byte-stream tests
// readable.

// ── Bank layout ────────────────────────────────────────────────────────
// MCU has 8 physical channel strips. Dusk Studio's 16 tracks split into
// two banks of 8 (matches the existing on-screen BANK A / BANK B
// toggle). kFaderCount fits both the per-bank channel strips (faders
// 0..7) and the dedicated MASTER fader (fader 8 in MCU numbering).
constexpr int kStripsPerBank     = 8;
constexpr int kMasterFaderIndex  = 8;
constexpr int kPitchBendMaxValue = 0x3FFF; // 14-bit position 0..16383

// ── Button notes (note-on with vel >= 0x40 = press, vel 0 = release) ──
// All buttons live on MIDI channel 1 (status byte 0x90). Velocity also
// drives the LED state for emit (0x7F = lit, 0x00 = dark).
namespace btn
{
    // Per-channel (N = 0..7 for strips 0..7)
    constexpr int RecArmBase = 0x00; // 0..7
    constexpr int SoloBase   = 0x08; // 8..15
    constexpr int MuteBase   = 0x10; // 16..23
    constexpr int SelectBase = 0x18; // 24..31
    constexpr int VPotPushBase = 0x20; // 32..39 (push action on the V-pot encoder)

    // Assign mode buttons (lit one-at-a-time)
    constexpr int AssignTrack  = 0x28;
    constexpr int AssignSend   = 0x29;
    constexpr int AssignPan    = 0x2A;
    constexpr int AssignPlugin = 0x2B;
    constexpr int AssignEq     = 0x2C;
    constexpr int AssignInst   = 0x2D;

    // Bank + channel navigation (LEDs reflect last-pressed)
    constexpr int BankLeft    = 0x2E;
    constexpr int BankRight   = 0x2F;
    constexpr int ChannelLeft  = 0x30;
    constexpr int ChannelRight = 0x31;

    // Transport cluster
    constexpr int Rewind      = 0x5B;
    constexpr int FastForward = 0x5C;
    constexpr int Stop        = 0x5D;
    constexpr int Play        = 0x5E;
    constexpr int Record      = 0x5F;
    constexpr int Loop        = 0x56;
    constexpr int Punch       = 0x57; // "Marker" - repurposed for punch toggle

    // Fader touch sense (vel 0x7F = touched, 0 = released)
    constexpr int FaderTouchBase   = 0x68; // 104..111 per-strip
    constexpr int FaderTouchMaster = 0x70; // 112
}

// ── Control change numbers ─────────────────────────────────────────────
namespace cc
{
    // V-pot rotation: data byte is a 7-bit signed delta where bit 6 is
    // direction (0 = right / CW, 1 = left / CCW) and bits 0..5 are
    // magnitude (1..63). Encoder ticks rather than absolute positions.
    constexpr int VPotRotateBase = 0x10; // 16..23 (strips 0..7)

    // V-pot ring LED ring update (emit only): value encodes lit-LED
    // pattern + mode (single / pan-center / fill-from-left / fill-from-
    // center / bar).
    constexpr int VPotRingBase = 0x30; // 48..55

    // Jog wheel rotation (single jog on the right of the control surface).
    constexpr int JogWheel = 0x3C;
}

// ── Sysex (LCD + timecode display) ─────────────────────────────────────
// Mackie sysex prefix: F0 00 00 66 14 ...  (manufacturer 00 00 66 =
// Mackie, device id 14 = MCU). After the prefix, command bytes:
//   0x12 = write to LCD (offset + ASCII bytes)
//   0x10 = update timecode (10 7-bit chars)
//   0x20 = device-info request (host -> controller; we don't emit)
namespace sysex
{
    constexpr std::uint8_t kPrefix[]     = { 0xF0, 0x00, 0x00, 0x66, 0x14 };
    constexpr std::size_t  kPrefixLen    = sizeof (kPrefix) / sizeof (kPrefix[0]);
    constexpr std::uint8_t kEnd          = 0xF7;
    constexpr std::uint8_t kCmdLcd       = 0x12;
    constexpr std::uint8_t kCmdTimecode  = 0x10;

    // LCD is 2 rows x 56 columns = 112 chars total. Row 0 starts at
    // offset 0; row 1 starts at offset 56. Each channel strip owns
    // 7 characters per row (8 strips * 7 = 56).
    constexpr int kLcdCols      = 56;
    constexpr int kLcdRowBytes  = 56;
    constexpr int kLcdCharsPerStrip = 7;
    constexpr int kLcdRow0Addr  = 0;
    constexpr int kLcdRow1Addr  = 56;

    // Timecode display: 10 7-bit ASCII bytes from MSB to LSB.
    constexpr int kTimecodeDigits = 10;
}

// ── Channel pressure (meter feedback emit only) ────────────────────────
// Per-channel signal level: status byte 0xD0, data byte encodes channel
// in the high nibble (0..7) and level 0..14 in the low nibble (15 = clip
// hold).
namespace meter
{
    constexpr std::uint8_t kStatus = 0xD0;
    constexpr int kMaxLevel  = 14;
    constexpr int kClipLevel = 15;
}

// ── V-pot ring modes (emit, CC 0x30+N) ─────────────────────────────────
// High nibble of the CC value selects the visual style. Low nibble (1..11)
// chooses which LED in the ring is highlighted.
namespace vring
{
    constexpr int ModeSingle      = 0 << 4;  // single-dot indicator
    constexpr int ModeBoost       = 1 << 4;  // pan: dot moves from centre
    constexpr int ModeWrap        = 2 << 4;  // fill from left
    constexpr int ModeSpread      = 3 << 4;  // fill from centre, symmetric
    constexpr int DotCenter       = 0x40;    // light up the centre dot
    constexpr int kLedCount       = 11;
}
} // namespace duskstudio::mcu
