#include "McuController.h"
#include "McuProtocol.h"
#include "McuFaderTaper.h"
#include "Transport.h"
#include "../session/Session.h"
#include "../foundation/Text.h"

#include <algorithm>
#include <string>

namespace duskstudio
{
namespace
{
// 7-char-per-strip LCD formatter helpers. The MCU's character ROM is
// ASCII-compatible (printable 0x20..0x7E); non-ASCII bytes glitch the
// display, so any non-printable input is replaced with a space.
constexpr int kLcdCharsPerStrip = mcu::sysex::kLcdCharsPerStrip;   // 7

void writeStripField (std::array<char, mcu::sysex::kLcdRowBytes>& row,
                      int strip, const std::string& text)
{
    const int base = strip * kLcdCharsPerStrip;
    if (base + kLcdCharsPerStrip > (int) row.size()) return;
    // Truncate or right-pad to exactly kLcdCharsPerStrip. Skip non-
    // ASCII runs (utf-8 multi-byte glyphs would otherwise emit two
    // garbage characters before the rest shifts).
    int written = 0;
    for (const char* p = text.c_str(); *p && written < kLcdCharsPerStrip; ++p)
    {
        const unsigned char c = (unsigned char) *p;
        if (c >= 0x7F)
            continue;   // drop UTF-8 bytes without spending LCD columns on them
        row[(size_t) (base + written++)] = (c >= 0x20) ? (char) c : ' ';
    }
    while (written < kLcdCharsPerStrip)
        row[(size_t) (base + written++)] = ' ';
}

void emitLcdRow (juce::MidiBuffer& buf,
                 int rowAddr,
                 const std::array<char, mcu::sysex::kLcdRowBytes>& row)
{
    // Mackie LCD sysex: F0 00 00 66 14 12 <offset> <chars...> F7
    // Build the byte sequence into a small stack buffer (5 prefix + 1
    // cmd + 1 offset + 56 chars + 1 end = 64 bytes max) and feed it
    // to MidiMessage::createSysExMessage which copies internally.
    std::array<std::uint8_t, 64> bytes;
    size_t n = 0;
    for (size_t i = 0; i < mcu::sysex::kPrefixLen; ++i)
        bytes[n++] = mcu::sysex::kPrefix[i];
    bytes[n++] = mcu::sysex::kCmdLcd;
    bytes[n++] = (std::uint8_t) (rowAddr & 0x7F);
    for (size_t i = 0; i < row.size(); ++i)
        bytes[n++] = (std::uint8_t) row[i];
    bytes[n++] = mcu::sysex::kEnd;
    // createSysExMessage expects the body WITHOUT F0/F7; we feed the
    // body slice and let it wrap. Saves one allocation vs constructing
    // a MidiMessage from raw bytes including the framing.
    buf.addEvent (juce::MidiMessage (bytes.data(), (int) n), 0);
}

void emitTimecode (juce::MidiBuffer& buf,
                   const std::array<char, mcu::sysex::kTimecodeDigits>& digits)
{
    // F0 00 00 66 14 10 <10 digits> F7
    std::array<std::uint8_t, 32> bytes;
    size_t n = 0;
    for (size_t i = 0; i < mcu::sysex::kPrefixLen; ++i)
        bytes[n++] = mcu::sysex::kPrefix[i];
    bytes[n++] = mcu::sysex::kCmdTimecode;
    for (size_t i = 0; i < digits.size(); ++i)
        bytes[n++] = (std::uint8_t) digits[i];
    bytes[n++] = mcu::sysex::kEnd;
    buf.addEvent (juce::MidiMessage (bytes.data(), (int) n), 0);
}

std::string formatFaderDbForLcd (float db)
{
    if (db <= ChannelStripParams::kFaderInfThreshDb) return "  -inf";
    // 1 decimal, max width 6 chars including sign. " +3.0 " etc.
    if (db >= 0.0f) return "+" + dusk::text::format ("%.1f", db);
    return dusk::text::format ("%.1f", db);
}

std::string formatPanForLcd (float pan)
{
    if (std::abs (pan) < 0.01f) return "  C   ";
    const int magnitude = (int) std::round (std::abs (pan) * 100.0f);
    return (pan < 0.0f ? std::string ("L") : std::string ("R"))
         + std::to_string (magnitude);
}
} // namespace

McuController::McuController (Session& sessionRef) noexcept
    : session (sessionRef)
{
    lastLcdRow0.fill (' ');
    lastLcdRow1.fill (' ');
    lastTimecode.fill (' ');
    startTimerHz (kRefreshHz);
}

McuController::~McuController()
{
    stopTimer();
}

juce::MidiBuffer McuController::buildEmitBuffer (bool forceAll)
{
    juce::MidiBuffer buf;
    const int bank = session.mcu.bank.load (std::memory_order_acquire);
    const int assign = session.mcu.assignMode.load (std::memory_order_acquire);
    const int selected = session.mcu.selectedChannel.load (std::memory_order_acquire);

    // Per-strip fader / button feedback for the 8 banked tracks
    for (int strip = 0; strip < kStripsPerBank; ++strip)
    {
        const int t = bank * kStripsPerBank + strip;
        if (t < 0 || t >= Session::kNumTracks) continue;
        const auto& trk = session.track (t);

        // Fader. liveFaderDb (post-automation) so motors track Read mode.
        const float liveDb = trk.strip.liveFaderDb.load (std::memory_order_relaxed);
        const int pb14 = mcu::faderDbToPitchBend14 (liveDb);
        if (forceAll || pb14 != lastFader[(size_t) strip])
        {
            // MidiMessage::pitchWheel uses 1-based channels.
            buf.addEvent (juce::MidiMessage::pitchWheel (strip + 1, pb14), 0);
            lastFader[(size_t) strip] = pb14;
        }

        const bool muteOn = trk.strip.mute.load (std::memory_order_relaxed);
        if (forceAll || muteOn != lastMute[(size_t) strip])
        {
            buf.addEvent (juce::MidiMessage::noteOn (1,
                mcu::btn::MuteBase + strip,
                (std::uint8_t) (muteOn ? 0x7F : 0x00)), 0);
            lastMute[(size_t) strip] = muteOn;
        }

        const bool soloOn = trk.strip.solo.load (std::memory_order_relaxed);
        if (forceAll || soloOn != lastSolo[(size_t) strip])
        {
            buf.addEvent (juce::MidiMessage::noteOn (1,
                mcu::btn::SoloBase + strip,
                (std::uint8_t) (soloOn ? 0x7F : 0x00)), 0);
            lastSolo[(size_t) strip] = soloOn;
        }

        const bool armOn = trk.recordArmed.load (std::memory_order_relaxed);
        if (forceAll || armOn != lastArm[(size_t) strip])
        {
            buf.addEvent (juce::MidiMessage::noteOn (1,
                mcu::btn::RecArmBase + strip,
                (std::uint8_t) (armOn ? 0x7F : 0x00)), 0);
            lastArm[(size_t) strip] = armOn;
        }
    }

    // Master fader
    const float masterDb = session.master().liveFaderDb.load (std::memory_order_relaxed);
    const int masterPb = mcu::faderDbToPitchBend14 (masterDb);
    if (forceAll || masterPb != lastMasterFader)
    {
        buf.addEvent (juce::MidiMessage::pitchWheel (mcu::kMasterFaderIndex + 1, masterPb), 0);
        lastMasterFader = masterPb;
    }

    // Bank arrow LEDs
    if (forceAll || bank != lastBank)
    {
        const bool leftAvailable  = bank > 0;
        const bool rightAvailable = bank < Session::kNumBanks - 1;
        buf.addEvent (juce::MidiMessage::noteOn (1, mcu::btn::BankLeft,
            (std::uint8_t) (leftAvailable ? 0x7F : 0x00)), 0);
        buf.addEvent (juce::MidiMessage::noteOn (1, mcu::btn::BankRight,
            (std::uint8_t) (rightAvailable ? 0x7F : 0x00)), 0);
        lastBank = bank;
    }

    // Assign-mode LEDs (exactly one lit)
    if (forceAll || assign != lastAssignMode)
    {
        // Map session.mcu.assignMode -> the lit button:
        //   0 = PAN       -> AssignPan
        //   1..4 = SEND   -> AssignSend (the cycle within SEND isn't
        //                    individually represented on a 6-button
        //                    bank; the SEND LED lights and the LCD
        //                    later shows which send)
        //   5 = EQ        -> AssignEq
        //   6 = COMP      -> AssignTrack (we re-used TRACK for COMP)
        const int lit = (assign == 0)                  ? mcu::btn::AssignPan
                      : (assign >= 1 && assign <= 4)   ? mcu::btn::AssignSend
                      : (assign == 5)                   ? mcu::btn::AssignEq
                                                         : mcu::btn::AssignTrack;
        for (int n : { mcu::btn::AssignTrack, mcu::btn::AssignSend,
                        mcu::btn::AssignPan,   mcu::btn::AssignPlugin,
                        mcu::btn::AssignEq,    mcu::btn::AssignInst })
        {
            buf.addEvent (juce::MidiMessage::noteOn (1, n,
                (std::uint8_t) (n == lit ? 0x7F : 0x00)), 0);
        }
        lastAssignMode = assign;
    }

    // Selected-channel LED (when the selection falls in the bank)
    if (forceAll || selected != lastSelectedChannel || bank != lastBank)
    {
        // Clear all 8 SELECT LEDs first, then light the active one if
        // it's currently visible. forceAll case + bank-change case
        // both need the wipe; the simple-selection-change case could
        // optimise but the gain is 7 notes / second worst case which
        // isn't worth the branching.
        for (int strip = 0; strip < kStripsPerBank; ++strip)
        {
            const int absTrack = bank * kStripsPerBank + strip;
            const bool lit = (absTrack == selected);
            buf.addEvent (juce::MidiMessage::noteOn (1,
                mcu::btn::SelectBase + strip,
                (std::uint8_t) (lit ? 0x7F : 0x00)), 0);
        }
        lastSelectedChannel = selected;
    }

    // Transport LEDs (and loop)
    // Provider is wired by AudioEngine in its ctor; tests can leave it
    // unset and the controller skips this branch (the transport LEDs
    // simply stay at their last cached value).
    if (transportProvider)
    {
        if (auto* transport = transportProvider())
        {
            const int state = (int) transport->getState();
            if (forceAll || state != lastTransportState)
            {
                const bool playing   = (state == (int) Transport::State::Playing);
                const bool stopped   = (state == (int) Transport::State::Stopped);
                const bool recording = (state == (int) Transport::State::Recording);
                buf.addEvent (juce::MidiMessage::noteOn (1, mcu::btn::Play,
                    (std::uint8_t) ((playing || recording) ? 0x7F : 0x00)), 0);
                buf.addEvent (juce::MidiMessage::noteOn (1, mcu::btn::Stop,
                    (std::uint8_t) (stopped ? 0x7F : 0x00)), 0);
                buf.addEvent (juce::MidiMessage::noteOn (1, mcu::btn::Record,
                    (std::uint8_t) (recording ? 0x7F : 0x00)), 0);
                lastTransportState = state;
            }
            const bool loopOn = transport->isLoopEnabled();
            if (forceAll || loopOn != lastLoopOn)
            {
                buf.addEvent (juce::MidiMessage::noteOn (1, mcu::btn::Loop,
                    (std::uint8_t) (loopOn ? 0x7F : 0x00)), 0);
                lastLoopOn = loopOn;
            }
        }
    }
    else if (forceAll)
    {
        // No transport provider but resync requested: emit a sensible
        // default (Stop lit, Play / Record / Loop dark). Matches the
        // controller's idle state on first connect before AudioEngine
        // hooks the provider up.
        buf.addEvent (juce::MidiMessage::noteOn (1, mcu::btn::Stop,   (std::uint8_t) 0x7F), 0);
        buf.addEvent (juce::MidiMessage::noteOn (1, mcu::btn::Play,   (std::uint8_t) 0x00), 0);
        buf.addEvent (juce::MidiMessage::noteOn (1, mcu::btn::Record, (std::uint8_t) 0x00), 0);
        buf.addEvent (juce::MidiMessage::noteOn (1, mcu::btn::Loop,   (std::uint8_t) 0x00), 0);
    }

    // LCD row 0: track names (7 chars per strip)
    // LCD row 1: value readouts depending on assign mode
    std::array<char, mcu::sysex::kLcdRowBytes> row0 {};
    std::array<char, mcu::sysex::kLcdRowBytes> row1 {};
    row0.fill (' ');
    row1.fill (' ');
    for (int strip = 0; strip < kStripsPerBank; ++strip)
    {
        const int t = bank * kStripsPerBank + strip;
        if (t < 0 || t >= Session::kNumTracks) continue;
        const auto& trk = session.track (t);

        // Row 0: track name. Empty / default-numeric -> "TRK N".
        const auto rawName = trk.name.trim();
        const std::string displayName = rawName.isNotEmpty()
                                            ? rawName.toStdString()
                                            : "TRK " + std::to_string (t + 1);
        writeStripField (row0, strip, displayName);

        // Row 1: value depends on assign mode.
        std::string value;
        switch (assign)
        {
            case 0:   value = formatPanForLcd (trk.strip.pan.load (std::memory_order_relaxed)); break;
            case 1: case 2: case 3: case 4:
                value = formatFaderDbForLcd (
                            trk.strip.auxSendDb[(size_t) (assign - 1)]
                                .load (std::memory_order_relaxed));
                break;
            case 5:   value = "EQ";   break;
            case 6:   value = "COMP"; break;
            default:  value = formatFaderDbForLcd (
                            trk.strip.liveFaderDb.load (std::memory_order_relaxed));
                break;
        }
        writeStripField (row1, strip, value);
    }

    // Per-channel meters (channel pressure 0xD0)
    // Toggle every tick so we emit at ~15 Hz - half of the 30 Hz UI
    // cadence. MCU meters fall back from peak under their own
    // ballistics; over-driving them buries the bus in low-value
    // updates. Skip the toggle on forceAll so a fresh resync gets
    // initial meter values immediately.
    emitMeterOnThisTick = ! emitMeterOnThisTick;
    if (forceAll || emitMeterOnThisTick)
    {
        for (int strip = 0; strip < kStripsPerBank; ++strip)
        {
            const int t = bank * kStripsPerBank + strip;
            if (t < 0 || t >= Session::kNumTracks) continue;
            const auto& trk = session.track (t);
            const float dbL = trk.meterInputDb .load (std::memory_order_relaxed);
            const float dbR = trk.meterInputRDb.load (std::memory_order_relaxed);
            const float peakDb = std::max (dbL, dbR);
            // Map -60..0 dB to 0..14, clip > 0 dB to 15.
            int level;
            if (peakDb >= 0.0f) level = mcu::meter::kClipLevel;
            else if (peakDb <= -60.0f) level = 0;
            else level = (int) std::round ((peakDb + 60.0f) * mcu::meter::kMaxLevel / 60.0f);
            if (forceAll || level != lastMeter[(size_t) strip])
            {
                // 0xD0 status (channel pressure, ch 1): hi-nibble of
                // data byte = strip 0..7, lo-nibble = level 0..15.
                const std::uint8_t dataByte = (std::uint8_t) ((strip << 4) | (level & 0x0F));
                buf.addEvent (juce::MidiMessage (mcu::meter::kStatus, dataByte), 0);
                lastMeter[(size_t) strip] = level;
            }
        }
    }

    // V-pot LED rings (CC 0x30+N)
    // Value encodes mode (high nibble) + LED position 1..11 (low
    // nibble). In PAN mode use ModeBoost (single dot moves from
    // centre); in SEND modes use ModeWrap (fill from left); in EQ /
    // COMP modes light the centre dot for the selected channel's
    // strip (visual cue that the encoders are now editing the
    // selected channel, not the banked strip). Bit 6 turns on the
    // centre-dot LED regardless of mode - we use it for the C
    // (centre) indicator in pan.
    for (int strip = 0; strip < kStripsPerBank; ++strip)
    {
        const int t = bank * kStripsPerBank + strip;
        if (t < 0 || t >= Session::kNumTracks) continue;
        const auto& trk = session.track (t);
        int ringValue = 0;
        switch (assign)
        {
            case 0:
            {
                // PAN: 0 = centre, -1..+1 maps to LED 1..11. Use
                // ModeBoost with centre-dot lit when pan == 0.
                const float pan = trk.strip.pan.load (std::memory_order_relaxed);
                const int led = juce::jlimit (1, mcu::vring::kLedCount,
                    1 + (int) std::round ((pan + 1.0f) * 0.5f * (mcu::vring::kLedCount - 1)));
                ringValue = mcu::vring::ModeBoost | led
                              | (std::abs (pan) < 0.01f ? mcu::vring::DotCenter : 0);
                break;
            }
            case 1: case 2: case 3: case 4:
            {
                // SEND: -inf..+6 dB. ModeWrap (fill from left).
                const float db = trk.strip.auxSendDb[(size_t) (assign - 1)]
                                    .load (std::memory_order_relaxed);
                const float norm = (db <= ChannelStripParams::kFaderInfThreshDb)
                                      ? 0.0f
                                      : juce::jlimit (0.0f, 1.0f,
                                          (db - ChannelStripParams::kAuxSendMinDb)
                                          / (ChannelStripParams::kAuxSendMaxDb
                                             - ChannelStripParams::kAuxSendMinDb));
                const int led = juce::jlimit (1, mcu::vring::kLedCount,
                    (int) std::round (norm * mcu::vring::kLedCount));
                ringValue = mcu::vring::ModeWrap | led;
                break;
            }
            case 5: case 6:
                // EQ / COMP modes: the encoders edit the selected
                // channel, not the banked strip. Show a single dot in
                // the middle so the user knows the ring isn't tracking
                // strip state. The selected channel's V-pot lights
                // its centre dot brighter (via DotCenter).
                ringValue = mcu::vring::ModeSingle | 6
                              | ((bank * kStripsPerBank + strip) == selected
                                    ? mcu::vring::DotCenter : 0);
                break;
            default:
                ringValue = 0;
                break;
        }
        if (forceAll || ringValue != lastVpotRing[(size_t) strip])
        {
            buf.addEvent (juce::MidiMessage::controllerEvent (1,
                mcu::cc::VPotRingBase + strip,
                (std::uint8_t) (ringValue & 0x7F)), 0);
            lastVpotRing[(size_t) strip] = ringValue;
        }
    }

    if (forceAll || row0 != lastLcdRow0)
    {
        emitLcdRow (buf, mcu::sysex::kLcdRow0Addr, row0);
        lastLcdRow0 = row0;
    }
    if (forceAll || row1 != lastLcdRow1)
    {
        emitLcdRow (buf, mcu::sysex::kLcdRow1Addr, row1);
        lastLcdRow1 = row1;
    }

    // Timecode display
    // Format BBT or SMPTE depending on session.timeDisplayMode. 10
    // chars total (MCU's number panel adds the dots itself, so we
    // emit pure digits with no separators).
    std::array<char, mcu::sysex::kTimecodeDigits> tc {};
    tc.fill (' ');
    std::int64_t playhead = 0;
    if (transportProvider)
        if (auto* transport = transportProvider())
            playhead = transport->getPlayhead();
    const float bpm   = session.tempoBpm.load (std::memory_order_relaxed);
    const int bpb     = std::max (1, session.beatsPerBar.load (std::memory_order_relaxed));
    double sr = sampleRateProvider ? sampleRateProvider() : 0.0;
    if (sr <= 0.0) sr = 48000.0;
    const double samplesPerBeat = (bpm > 0.0f)
                                    ? sr * 60.0 / (double) bpm : sr;
    const double samplesPerBar  = samplesPerBeat * (double) bpb;
    const int bar  = (int) ((double) playhead / std::max (1.0, samplesPerBar));
    const int beat = (int) (((double) playhead - bar * samplesPerBar) / std::max (1.0, samplesPerBeat));
    const int subdiv = (int) (((double) playhead
                                - bar * samplesPerBar
                                - beat * samplesPerBeat)
                                * 4.0
                                / std::max (1.0, samplesPerBeat));
    auto pad = [] (int v, int width)
    {
        return dusk::text::paddedLeft (std::to_string (std::max (0, v) + 1), '0', width);
    };
    const auto tcStr = dusk::text::substring (
                           pad (bar, 3) + pad (beat, 2) + pad (subdiv, 2) + "  0",
                           0, mcu::sysex::kTimecodeDigits);
    for (int i = 0; i < mcu::sysex::kTimecodeDigits
                    && i < (int) tcStr.length(); ++i)
        tc[(size_t) i] = (char) tcStr[i];

    const bool transportRolling = lastTransportState
                                   == (int) Transport::State::Playing
                               || lastTransportState
                                   == (int) Transport::State::Recording;
    ++ticksSinceTimecode;
    const bool timecodeDue = transportRolling
                                ? true                                   // every tick when rolling
                                : (ticksSinceTimecode >= kRefreshHz);   // ~1 Hz when idle
    if (forceAll || (timecodeDue && tc != lastTimecode))
    {
        emitTimecode (buf, tc);
        lastTimecode = tc;
        ticksSinceTimecode = 0;
    }

    return buf;
}

void McuController::timerCallback()
{
    // No sink wired yet (early ctor / unit test) -> skip the emit
    // entirely. Output-index gate keeps the controller silent until
    // the user has actually picked a device.
    if (! sink) return;
    if (session.mcu.resolvedOutputIdx.load (std::memory_order_acquire) < 0)
        return;

    const bool force = resyncRequested.exchange (false, std::memory_order_acq_rel)
                    || transportEdgeRequested.exchange (false, std::memory_order_acq_rel);
    auto buf = buildEmitBuffer (force);
    if (buf.isEmpty()) return;
    sink (buf);
}

juce::MidiBuffer McuController::buildBufferForTest()
{
    // Test entry: build the buffer but don't touch the engine.
    // Always full-resync semantics so a test's first call gets the
    // initial state in one shot.
    return buildEmitBuffer (/*forceAll*/ true);
}
} // namespace duskstudio
