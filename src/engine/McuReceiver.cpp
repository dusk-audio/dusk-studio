#include "McuReceiver.h"
#include "McuProtocol.h"
#include "../session/Session.h"

namespace duskstudio
{
namespace mcu_local
{
constexpr float kFaderMinDb = ChannelStripParams::kFaderMinDb;   // -100
constexpr float kFaderMaxDb = ChannelStripParams::kFaderMaxDb;   //  +12

// MCU V-pot encoder ticks divided by this many to convert one tick into
// a "musical" delta. Logic uses ~64 ticks per full sweep; we want a knob
// turn (10..20 ticks for a deliberate move) to be a noticeable change
// without overshoot. Per-target overrides could refine this later.
constexpr float kVpotPanStep      = 0.02f;     // 100 ticks -> full sweep
constexpr float kVpotSendStepDb   = 0.5f;      // per tick
} // namespace mcu_local

void McuReceiver::reset() noexcept
{
    // No accumulator state today - V-pot deltas are stateless per
    // event. If a future change adds a multi-byte sysex decode
    // (e.g. MCU device-info handshake) this is where to clear it.
}

float McuReceiver::pitchBendToFaderDb (int pb14) const noexcept
{
    if (pb14 < 0) pb14 = 0;
    if (pb14 > mcu::kPitchBendMaxValue) pb14 = mcu::kPitchBendMaxValue;
    const float norm = (float) pb14 / (float) mcu::kPitchBendMaxValue;
    return mcu_local::kFaderMinDb
         + norm * (mcu_local::kFaderMaxDb - mcu_local::kFaderMinDb);
}

int McuReceiver::faderDbToPitchBend (float db) const noexcept
{
    if (db < mcu_local::kFaderMinDb) db = mcu_local::kFaderMinDb;
    if (db > mcu_local::kFaderMaxDb) db = mcu_local::kFaderMaxDb;
    const float norm = (db - mcu_local::kFaderMinDb)
                     / (mcu_local::kFaderMaxDb - mcu_local::kFaderMinDb);
    return (int) (norm * (float) mcu::kPitchBendMaxValue);
}

void McuReceiver::resetVpotTarget (int stripIndex) noexcept
{
    const int bank = session.mcu.bank.load (std::memory_order_relaxed);
    const int trackIdx = bank * mcu::kStripsPerBank + stripIndex;
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return;
    const int mode = session.mcu.assignMode.load (std::memory_order_relaxed);
    auto& strip = session.track (trackIdx).strip;
    if (mode == 0)
        strip.pan.store (0.0f, std::memory_order_relaxed);
    else if (mode >= 1 && mode <= 4)
        strip.auxSendDb[(size_t) (mode - 1)].store (
            ChannelStripParams::kAuxSendOffDb, std::memory_order_relaxed);
    // EQ + COMP V-pot push resets the currently-edited param to its
    // mid value. Step 5 will fill in the per-encoder mapping; for now
    // PAN-reset only is enough to validate the path.
}

void McuReceiver::applyVpotDelta (int stripIndex, int delta) noexcept
{
    if (delta == 0) return;
    const int bank = session.mcu.bank.load (std::memory_order_relaxed);
    const int trackIdx = bank * mcu::kStripsPerBank + stripIndex;
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return;
    const int mode = session.mcu.assignMode.load (std::memory_order_relaxed);
    auto& strip = session.track (trackIdx).strip;

    if (mode == 0)
    {
        // PAN: ±1 per tick * kVpotPanStep, clamp to [-1, +1].
        const float cur = strip.pan.load (std::memory_order_relaxed);
        const float nxt = juce::jlimit (-1.0f, 1.0f,
            cur + (float) delta * mcu_local::kVpotPanStep);
        strip.pan.store (nxt, std::memory_order_relaxed);
    }
    else if (mode >= 1 && mode <= 4)
    {
        // SEND 1..4: ±0.5 dB per tick. Clamp to send-range; below the
        // floor we snap to the kAuxSendOffDb sentinel so silent sends
        // short-circuit in the audio path.
        auto& atom = strip.auxSendDb[(size_t) (mode - 1)];
        const float cur = atom.load (std::memory_order_relaxed);
        const float curOrFloor = (cur == ChannelStripParams::kAuxSendOffDb)
                                    ? ChannelStripParams::kAuxSendMinDb
                                    : cur;
        const float nxt = curOrFloor + (float) delta * mcu_local::kVpotSendStepDb;
        if (nxt <= ChannelStripParams::kAuxSendMinDb)
            atom.store (ChannelStripParams::kAuxSendOffDb,
                         std::memory_order_relaxed);
        else
            atom.store (juce::jlimit (ChannelStripParams::kAuxSendMinDb,
                                        ChannelStripParams::kAuxSendMaxDb,
                                        nxt),
                         std::memory_order_relaxed);
    }
    // EQ + COMP encoder modes land in step 5.
}

void McuReceiver::handleNotePress (int noteNumber, bool pressed) noexcept
{
    // Per-strip buttons live in contiguous note ranges. The selected
    // strip is `noteNumber - base` (0..7); banking maps that to an
    // absolute track index.
    const int bank = session.mcu.bank.load (std::memory_order_relaxed);

    auto stripToTrack = [bank] (int strip) noexcept
    {
        return bank * mcu::kStripsPerBank + strip;
    };

    // Rec arm 0..7
    if (noteNumber >= mcu::btn::RecArmBase
        && noteNumber <  mcu::btn::RecArmBase + mcu::kStripsPerBank)
    {
        if (! pressed) return;       // only respond to press, not release
        const int t = stripToTrack (noteNumber - mcu::btn::RecArmBase);
        if (t < 0 || t >= Session::kNumTracks) return;
        const bool on = session.track (t).recordArmed.load (std::memory_order_relaxed);
        session.setTrackArmed (t, ! on);
        return;
    }

    // Solo 8..15
    if (noteNumber >= mcu::btn::SoloBase
        && noteNumber <  mcu::btn::SoloBase + mcu::kStripsPerBank)
    {
        if (! pressed) return;
        const int t = stripToTrack (noteNumber - mcu::btn::SoloBase);
        if (t < 0 || t >= Session::kNumTracks) return;
        const bool on = session.track (t).strip.solo.load (std::memory_order_relaxed);
        session.setTrackSoloed (t, ! on);
        return;
    }

    // Mute 16..23
    if (noteNumber >= mcu::btn::MuteBase
        && noteNumber <  mcu::btn::MuteBase + mcu::kStripsPerBank)
    {
        if (! pressed) return;
        const int t = stripToTrack (noteNumber - mcu::btn::MuteBase);
        if (t < 0 || t >= Session::kNumTracks) return;
        auto& m = session.track (t).strip.mute;
        const bool on = m.load (std::memory_order_relaxed);
        m.store (! on, std::memory_order_release);
        return;
    }

    // Select 24..31
    if (noteNumber >= mcu::btn::SelectBase
        && noteNumber <  mcu::btn::SelectBase + mcu::kStripsPerBank)
    {
        if (! pressed) return;
        const int t = stripToTrack (noteNumber - mcu::btn::SelectBase);
        if (t < 0 || t >= Session::kNumTracks) return;
        session.mcu.selectedChannel.store (t, std::memory_order_release);
        return;
    }

    // V-pot push 32..39 (encoder click)
    if (noteNumber >= mcu::btn::VPotPushBase
        && noteNumber <  mcu::btn::VPotPushBase + mcu::kStripsPerBank)
    {
        if (! pressed) return;
        resetVpotTarget (noteNumber - mcu::btn::VPotPushBase);
        return;
    }

    // Assign mode buttons. The protocol layout exposes 6 modes;
    // McuSessionState only needs PAN / SEND* / EQ / COMP. The MCU
    // sends one note per button (TRACK / SEND / PAN / PLUGIN / EQ /
    // INSTRUMENT). Map them sensibly: PAN=0, SEND=1 (cycles SEND1..4
    // on repeated press), EQ=5, COMP=6 (re-use the "TRACK" button as
    // COMP since Dusk Studio has no track-attribute mode).
    if (pressed)
    {
        switch (noteNumber)
        {
            case mcu::btn::AssignPan:    session.mcu.assignMode.store (0, std::memory_order_release); return;
            case mcu::btn::AssignSend:
            {
                // Cycle SEND1 -> SEND2 -> SEND3 -> SEND4 -> SEND1.
                int m = session.mcu.assignMode.load (std::memory_order_relaxed);
                m = (m >= 1 && m <= 4) ? (m % 4 + 1) : 1;
                session.mcu.assignMode.store (m, std::memory_order_release);
                return;
            }
            case mcu::btn::AssignEq:     session.mcu.assignMode.store (5, std::memory_order_release); return;
            case mcu::btn::AssignTrack:  session.mcu.assignMode.store (6, std::memory_order_release); return;
            case mcu::btn::AssignPlugin: return;   // unused in Dusk Studio for now
            case mcu::btn::AssignInst:   return;   // unused
            default: break;
        }
    }

    // Bank navigation
    if (noteNumber == mcu::btn::BankLeft && pressed)
    {
        const int b = session.mcu.bank.load (std::memory_order_relaxed);
        if (b > 0) session.mcu.bank.store (b - 1, std::memory_order_release);
        return;
    }
    if (noteNumber == mcu::btn::BankRight && pressed)
    {
        const int b = session.mcu.bank.load (std::memory_order_relaxed);
        const int maxBank = Session::kNumBanks - 1;
        if (b < maxBank) session.mcu.bank.store (b + 1, std::memory_order_release);
        return;
    }
    if (noteNumber == mcu::btn::ChannelLeft && pressed)
    {
        const int c = session.mcu.selectedChannel.load (std::memory_order_relaxed);
        if (c > 0) session.mcu.selectedChannel.store (c - 1, std::memory_order_release);
        return;
    }
    if (noteNumber == mcu::btn::ChannelRight && pressed)
    {
        const int c = session.mcu.selectedChannel.load (std::memory_order_relaxed);
        if (c < Session::kNumTracks - 1)
            session.mcu.selectedChannel.store (c + 1, std::memory_order_release);
        return;
    }

    // Transport cluster - dispatch through pendingTransportAction so
    // engine.play() / stop() / record() run on the message thread.
    if (pressed)
    {
        switch (noteNumber)
        {
            case mcu::btn::Play:
                session.pendingTransportAction.store (
                    (int) PendingTransportAction::Play,
                    std::memory_order_relaxed);
                return;
            case mcu::btn::Stop:
                session.pendingTransportAction.store (
                    (int) PendingTransportAction::Stop,
                    std::memory_order_relaxed);
                return;
            case mcu::btn::Record:
                session.pendingTransportAction.store (
                    (int) PendingTransportAction::Record,
                    std::memory_order_relaxed);
                return;
            case mcu::btn::Rewind:
                session.pendingTransportPlayhead.store (
                    (juce::int64) 0, std::memory_order_relaxed);
                return;
            case mcu::btn::FastForward:
                // No "end of timeline" concept in Dusk Studio; let
                // FastForward jump to the last record point via the
                // engine's existing helper. Audio thread can't call
                // it directly; defer to TransportBar's binding logic
                // by setting a sentinel that the message-thread timer
                // catches. For step 2, leave the FFWD button as a
                // no-op (will be wired in step 3 with the rest of
                // the transport feedback).
                return;
            case mcu::btn::Loop:
            {
                auto& t = session.savedLoopEnabled;
                t = ! t;
                // The audio thread normally reads loop state from the
                // Transport directly; flipping a Session::saved* field
                // wouldn't take effect. TransportBar's binding path
                // does this through a pending-action sentinel too;
                // for step 2, the wiring is deliberately simple - the
                // emit step in 3/4 will pair with the on-screen
                // toggle's existing Transport API.
                return;
            }
            default: break;
        }
    }

    // Fader touch sense (per strip + master). Drives the strip's
    // faderTouched latch so Write / Touch automation behaves on
    // hardware moves the same way it does on on-screen drags.
    if (noteNumber >= mcu::btn::FaderTouchBase
        && noteNumber <  mcu::btn::FaderTouchBase + mcu::kStripsPerBank)
    {
        const int t = stripToTrack (noteNumber - mcu::btn::FaderTouchBase);
        if (t < 0 || t >= Session::kNumTracks) return;
        session.track (t).strip.faderTouched.store (pressed, std::memory_order_release);
        return;
    }
    if (noteNumber == mcu::btn::FaderTouchMaster)
    {
        session.master().faderTouched.store (pressed, std::memory_order_release);
        return;
    }
}

void McuReceiver::process (const juce::MidiBuffer& events,
                            juce::int64 blockStartSample) noexcept
{
    juce::ignoreUnused (blockStartSample);
    for (const auto meta : events)
    {
        const auto& m = meta.getMessage();
        const auto* raw = m.getRawData();
        const int sz = m.getRawDataSize();
        if (raw == nullptr || sz < 1) continue;
        const auto status = (juce::uint8) raw[0];
        const int statusType = status & 0xF0;
        const int channel    = status & 0x0F;

        // Pitch-bend = fader. Channel 0..7 = banked strip, 8 = master.
        if (statusType == 0xE0 && sz >= 3)
        {
            const int lsb = raw[1] & 0x7F;
            const int msb = raw[2] & 0x7F;
            const int pb14 = lsb | (msb << 7);
            const float db = pitchBendToFaderDb (pb14);
            if (channel == mcu::kMasterFaderIndex)
            {
                session.master().faderDb.store (db, std::memory_order_relaxed);
            }
            else if (channel >= 0 && channel < mcu::kStripsPerBank)
            {
                const int bank = session.mcu.bank.load (std::memory_order_relaxed);
                const int t = bank * mcu::kStripsPerBank + channel;
                if (t >= 0 && t < Session::kNumTracks)
                    session.track (t).strip.faderDb.store (db, std::memory_order_relaxed);
            }
            continue;
        }

        // Note On = button press (vel > 0) or release (vel 0).
        if (statusType == 0x90 && sz >= 3)
        {
            const int noteNumber = raw[1] & 0x7F;
            const int velocity   = raw[2] & 0x7F;
            handleNotePress (noteNumber, velocity >= 0x40);
            continue;
        }

        // Control change. CC 0x10..0x17 = V-pot rotation; CC 0x3C = jog.
        if (statusType == 0xB0 && sz >= 3)
        {
            const int cc = raw[1] & 0x7F;
            const int val = raw[2] & 0x7F;
            if (cc >= mcu::cc::VPotRotateBase
                && cc <  mcu::cc::VPotRotateBase + mcu::kStripsPerBank)
            {
                // Bits 0..5 = magnitude, bit 6 = sign (1 = negative).
                const int magnitude = val & 0x3F;
                const int delta = (val & 0x40) ? -magnitude : +magnitude;
                applyVpotDelta (cc - mcu::cc::VPotRotateBase, delta);
                continue;
            }
            if (cc == mcu::cc::JogWheel)
            {
                // Jog wheel: same sign encoding as V-pot. Step 2 only
                // decodes - actual playhead nudge is wired in step 3
                // alongside the rest of the transport feedback.
                juce::ignoreUnused (val);
                continue;
            }
        }
    }
}
} // namespace duskstudio
