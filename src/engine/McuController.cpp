#include "McuController.h"
#include "McuProtocol.h"
#include "Transport.h"
#include "../session/Session.h"

namespace duskstudio
{
namespace
{
// 7-char-per-strip LCD formatter helpers. The MCU's character ROM is
// ASCII-compatible (printable 0x20..0x7E); non-ASCII bytes glitch the
// display, so any non-printable input is replaced with a space.
constexpr int kLcdCharsPerStrip = mcu::sysex::kLcdCharsPerStrip;   // 7

void writeStripField (std::array<char, mcu::sysex::kLcdRowBytes>& row,
                      int strip, const juce::String& text)
{
    const int base = strip * kLcdCharsPerStrip;
    if (base + kLcdCharsPerStrip > (int) row.size()) return;
    // Truncate or right-pad to exactly kLcdCharsPerStrip. Skip non-
    // ASCII runs (utf-8 multi-byte glyphs would otherwise emit two
    // garbage characters before the rest shifts).
    const auto utf = text.toRawUTF8();
    int written = 0;
    for (const char* p = utf; *p && written < kLcdCharsPerStrip; ++p)
    {
        const unsigned char c = (unsigned char) *p;
        row[(size_t) (base + written++)] = (c >= 0x20 && c < 0x7F)
                                             ? (char) c : ' ';
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
    std::array<juce::uint8, 64> bytes;
    size_t n = 0;
    for (size_t i = 0; i < mcu::sysex::kPrefixLen; ++i)
        bytes[n++] = mcu::sysex::kPrefix[i];
    bytes[n++] = mcu::sysex::kCmdLcd;
    bytes[n++] = (juce::uint8) (rowAddr & 0x7F);
    for (size_t i = 0; i < row.size(); ++i)
        bytes[n++] = (juce::uint8) row[i];
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
    std::array<juce::uint8, 32> bytes;
    size_t n = 0;
    for (size_t i = 0; i < mcu::sysex::kPrefixLen; ++i)
        bytes[n++] = mcu::sysex::kPrefix[i];
    bytes[n++] = mcu::sysex::kCmdTimecode;
    for (size_t i = 0; i < digits.size(); ++i)
        bytes[n++] = (juce::uint8) digits[i];
    bytes[n++] = mcu::sysex::kEnd;
    buf.addEvent (juce::MidiMessage (bytes.data(), (int) n), 0);
}

juce::String formatFaderDbForLcd (float db)
{
    if (db <= ChannelStripParams::kFaderInfThreshDb) return juce::String ("  -inf");
    // 1 decimal, max width 6 chars including sign. " +3.0 " etc.
    if (db >= 0.0f) return "+" + juce::String (db, 1);
    return juce::String (db, 1);
}

juce::String formatPanForLcd (float pan)
{
    if (std::abs (pan) < 0.01f) return juce::String ("  C   ");
    const int magnitude = (int) std::round (std::abs (pan) * 100.0f);
    return (pan < 0.0f ? juce::String ("L") : juce::String ("R"))
         + juce::String (magnitude);
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

int McuController::faderDbToPitchBend (float db) const noexcept
{
    constexpr float kMin = ChannelStripParams::kFaderMinDb;
    constexpr float kMax = ChannelStripParams::kFaderMaxDb;
    if (db < kMin) db = kMin;
    if (db > kMax) db = kMax;
    const float norm = (db - kMin) / (kMax - kMin);
    return (int) (norm * (float) mcu::kPitchBendMaxValue);
}

juce::MidiBuffer McuController::buildEmitBuffer (bool forceAll)
{
    juce::MidiBuffer buf;
    const int bank = session.mcu.bank.load (std::memory_order_acquire);
    const int assign = session.mcu.assignMode.load (std::memory_order_acquire);
    const int selected = session.mcu.selectedChannel.load (std::memory_order_acquire);

    // ── Per-strip fader / button feedback for the 8 banked tracks ──
    for (int strip = 0; strip < kStripsPerBank; ++strip)
    {
        const int t = bank * kStripsPerBank + strip;
        if (t < 0 || t >= Session::kNumTracks) continue;
        const auto& trk = session.track (t);

        // Fader. liveFaderDb (post-automation) so motors track Read mode.
        const float liveDb = trk.strip.liveFaderDb.load (std::memory_order_relaxed);
        const int pb14 = faderDbToPitchBend (liveDb);
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
                (juce::uint8) (muteOn ? 0x7F : 0x00)), 0);
            lastMute[(size_t) strip] = muteOn;
        }

        const bool soloOn = trk.strip.solo.load (std::memory_order_relaxed);
        if (forceAll || soloOn != lastSolo[(size_t) strip])
        {
            buf.addEvent (juce::MidiMessage::noteOn (1,
                mcu::btn::SoloBase + strip,
                (juce::uint8) (soloOn ? 0x7F : 0x00)), 0);
            lastSolo[(size_t) strip] = soloOn;
        }

        const bool armOn = trk.recordArmed.load (std::memory_order_relaxed);
        if (forceAll || armOn != lastArm[(size_t) strip])
        {
            buf.addEvent (juce::MidiMessage::noteOn (1,
                mcu::btn::RecArmBase + strip,
                (juce::uint8) (armOn ? 0x7F : 0x00)), 0);
            lastArm[(size_t) strip] = armOn;
        }
    }

    // ── Master fader ──
    const float masterDb = session.master().liveFaderDb.load (std::memory_order_relaxed);
    const int masterPb = faderDbToPitchBend (masterDb);
    if (forceAll || masterPb != lastMasterFader)
    {
        buf.addEvent (juce::MidiMessage::pitchWheel (mcu::kMasterFaderIndex + 1, masterPb), 0);
        lastMasterFader = masterPb;
    }

    // ── Bank arrow LEDs ──
    if (forceAll || bank != lastBank)
    {
        const bool leftAvailable  = bank > 0;
        const bool rightAvailable = bank < Session::kNumBanks - 1;
        buf.addEvent (juce::MidiMessage::noteOn (1, mcu::btn::BankLeft,
            (juce::uint8) (leftAvailable ? 0x7F : 0x00)), 0);
        buf.addEvent (juce::MidiMessage::noteOn (1, mcu::btn::BankRight,
            (juce::uint8) (rightAvailable ? 0x7F : 0x00)), 0);
        lastBank = bank;
    }

    // ── Assign-mode LEDs (exactly one lit) ──
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
                (juce::uint8) (n == lit ? 0x7F : 0x00)), 0);
        }
        lastAssignMode = assign;
    }

    // ── Selected-channel LED (when the selection falls in the bank) ──
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
                (juce::uint8) (lit ? 0x7F : 0x00)), 0);
        }
        lastSelectedChannel = selected;
    }

    // ── Transport LEDs (and loop) ──
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
                    (juce::uint8) ((playing || recording) ? 0x7F : 0x00)), 0);
                buf.addEvent (juce::MidiMessage::noteOn (1, mcu::btn::Stop,
                    (juce::uint8) (stopped ? 0x7F : 0x00)), 0);
                buf.addEvent (juce::MidiMessage::noteOn (1, mcu::btn::Record,
                    (juce::uint8) (recording ? 0x7F : 0x00)), 0);
                lastTransportState = state;
            }
            const bool loopOn = transport->isLoopEnabled();
            if (forceAll || loopOn != lastLoopOn)
            {
                buf.addEvent (juce::MidiMessage::noteOn (1, mcu::btn::Loop,
                    (juce::uint8) (loopOn ? 0x7F : 0x00)), 0);
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
        buf.addEvent (juce::MidiMessage::noteOn (1, mcu::btn::Stop,   (juce::uint8) 0x7F), 0);
        buf.addEvent (juce::MidiMessage::noteOn (1, mcu::btn::Play,   (juce::uint8) 0x00), 0);
        buf.addEvent (juce::MidiMessage::noteOn (1, mcu::btn::Record, (juce::uint8) 0x00), 0);
        buf.addEvent (juce::MidiMessage::noteOn (1, mcu::btn::Loop,   (juce::uint8) 0x00), 0);
    }

    // ── LCD row 0: track names (7 chars per strip) ──
    // ── LCD row 1: value readouts depending on assign mode ──
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
        const juce::String displayName = rawName.isNotEmpty()
                                            ? rawName
                                            : "TRK " + juce::String (t + 1);
        writeStripField (row0, strip, displayName);

        // Row 1: value depends on assign mode.
        juce::String value;
        switch (assign)
        {
            case 0:   value = formatPanForLcd (trk.strip.pan.load (std::memory_order_relaxed)); break;
            case 1: case 2: case 3: case 4:
                value = formatFaderDbForLcd (
                            trk.strip.auxSendDb[(size_t) (assign - 1)]
                                .load (std::memory_order_relaxed));
                break;
            case 5:   value = juce::String ("EQ");   break;
            case 6:   value = juce::String ("COMP"); break;
            default:  value = formatFaderDbForLcd (
                            trk.strip.liveFaderDb.load (std::memory_order_relaxed));
                break;
        }
        writeStripField (row1, strip, value);
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

    // ── Timecode display ──
    // Format BBT or SMPTE depending on session.timeDisplayMode. 10
    // chars total (MCU's number panel adds the dots itself, so we
    // emit pure digits with no separators).
    std::array<char, mcu::sysex::kTimecodeDigits> tc {};
    tc.fill (' ');
    juce::int64 playhead = 0;
    if (transportProvider)
        if (auto* transport = transportProvider())
            playhead = transport->getPlayhead();
    const float bpm   = session.tempoBpm.load (std::memory_order_relaxed);
    const int bpb     = juce::jmax (1, session.beatsPerBar.load (std::memory_order_relaxed));
    const double srGuess = 48000.0;   // placeholder; engine will wire a real SR later
    const double samplesPerBeat = (bpm > 0.0f)
                                    ? srGuess * 60.0 / (double) bpm : srGuess;
    const double samplesPerBar  = samplesPerBeat * (double) bpb;
    const int bar  = (int) ((double) playhead / juce::jmax (1.0, samplesPerBar));
    const int beat = (int) (((double) playhead - bar * samplesPerBar) / juce::jmax (1.0, samplesPerBeat));
    const int subdiv = (int) (((double) playhead
                                - bar * samplesPerBar
                                - beat * samplesPerBeat)
                                * 4.0
                                / juce::jmax (1.0, samplesPerBeat));
    auto pad = [] (int v, int width)
    {
        return juce::String (juce::jmax (0, v) + 1).paddedLeft ('0', width);
    };
    const auto tcStr = (pad (bar, 3) + pad (beat, 2) + pad (subdiv, 2) + "  0")
                          .substring (0, mcu::sysex::kTimecodeDigits);
    for (int i = 0; i < mcu::sysex::kTimecodeDigits
                    && i < tcStr.length(); ++i)
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
