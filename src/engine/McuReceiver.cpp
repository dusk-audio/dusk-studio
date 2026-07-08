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
    const int mode = session.mcu.assignMode.load (std::memory_order_relaxed);

    // EQ + COMP: encoder bank acts on the SELECTED channel, not the
    // banked strip. Push action resets the encoder's parameter to a
    // sane default. Encoder indices match the assign-mode table.
    if (mode == 5 || mode == 6)
    {
        const int selT = session.mcu.selectedChannel.load (std::memory_order_relaxed);
        if (selT < 0 || selT >= Session::kNumTracks) return;
        auto& strip = session.track (selT).strip;
        if (mode == 5)
        {
            // EQ encoders 1=HPF, 2=LF gain, 3=LF freq, 4=LM gain,
            // 5=LM freq, 6=HM gain, 7=HF gain, 8=HF freq. Push -> 0
            // gain / centre freq. Encoder index = stripIndex (0..7).
            switch (stripIndex)
            {
                case 0: strip.hpfFreq.store (20.0f, std::memory_order_relaxed); break;
                case 1: strip.lfGainDb.store (0.0f, std::memory_order_relaxed); break;
                case 2: strip.lfFreq.store (100.0f, std::memory_order_relaxed); break;
                case 3: strip.lmGainDb.store (0.0f, std::memory_order_relaxed); break;
                case 4: strip.lmFreq.store (600.0f, std::memory_order_relaxed); break;
                case 5: strip.hmGainDb.store (0.0f, std::memory_order_relaxed); break;
                case 6: strip.hfGainDb.store (0.0f, std::memory_order_relaxed); break;
                case 7: strip.hfFreq.store (4000.0f, std::memory_order_relaxed); break;
                default: break;
            }
        }
        else
        {
            // COMP encoders 1=thresh, 2=ratio, 3=attack, 4=release,
            // 5=makeup. 6/7/8 unused. Push -> sensible defaults.
            switch (stripIndex)
            {
                case 0: strip.compThresholdDb.store (0.0f, std::memory_order_relaxed); break;
                case 1: strip.compVcaRatio   .store (4.0f, std::memory_order_relaxed); break;
                case 2: strip.compVcaAttack  .store (1.0f, std::memory_order_relaxed); break;
                case 3: strip.compVcaRelease .store (100.0f, std::memory_order_relaxed); break;
                case 4: strip.compMakeupDb   .store (0.0f, std::memory_order_relaxed); break;
                default: break;
            }
        }
        return;
    }

    const int bank = session.mcu.bank.load (std::memory_order_relaxed);
    const int trackIdx = bank * mcu::kStripsPerBank + stripIndex;
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return;
    auto& strip = session.track (trackIdx).strip;
    if (mode == 0)
        strip.pan.store (0.0f, std::memory_order_relaxed);
    else if (mode >= 1 && mode <= 4)
        strip.auxSendDb[(size_t) (mode - 1)].store (
            ChannelStripParams::kAuxSendOffDb, std::memory_order_relaxed);
}

void McuReceiver::applyVpotDelta (int stripIndex, int delta) noexcept
{
    if (delta == 0) return;
    const int mode = session.mcu.assignMode.load (std::memory_order_relaxed);

    // EQ / COMP: encoders act on the SELECTED channel. The 8 V-pots
    // map to fixed parameter slots (see resetVpotTarget for the same
    // index->param table). Step size tuned per param so a full
    // encoder sweep (~64 ticks) covers the typical adjust range.
    if (mode == 5 || mode == 6)
    {
        const int selT = session.mcu.selectedChannel.load (std::memory_order_relaxed);
        if (selT < 0 || selT >= Session::kNumTracks) return;
        auto& strip = session.track (selT).strip;
        const float d = (float) delta;
        if (mode == 5)
        {
            switch (stripIndex)
            {
                case 0: strip.hpfFreq.store (juce::jlimit (ChannelStripParams::kHpfMinHz,
                                                            ChannelStripParams::kHpfMaxHz,
                                                            strip.hpfFreq.load() + d * 4.0f),
                                              std::memory_order_relaxed); break;
                case 1: strip.lfGainDb.store (juce::jlimit (ChannelStripParams::kBandGainMin,
                                                             ChannelStripParams::kBandGainMax,
                                                             strip.lfGainDb.load() + d * 0.3f),
                                               std::memory_order_relaxed); break;
                case 2: strip.lfFreq.store (juce::jlimit (ChannelStripParams::kLfFreqMin,
                                                           ChannelStripParams::kLfFreqMax,
                                                           strip.lfFreq.load() + d * 5.0f),
                                             std::memory_order_relaxed); break;
                case 3: strip.lmGainDb.store (juce::jlimit (ChannelStripParams::kBandGainMin,
                                                             ChannelStripParams::kBandGainMax,
                                                             strip.lmGainDb.load() + d * 0.3f),
                                               std::memory_order_relaxed); break;
                case 4: strip.lmFreq.store (juce::jlimit (ChannelStripParams::kLmFreqMin,
                                                           ChannelStripParams::kLmFreqMax,
                                                           strip.lmFreq.load() + d * 20.0f),
                                             std::memory_order_relaxed); break;
                case 5: strip.hmGainDb.store (juce::jlimit (ChannelStripParams::kBandGainMin,
                                                             ChannelStripParams::kBandGainMax,
                                                             strip.hmGainDb.load() + d * 0.3f),
                                               std::memory_order_relaxed); break;
                case 6: strip.hfGainDb.store (juce::jlimit (ChannelStripParams::kBandGainMin,
                                                             ChannelStripParams::kBandGainMax,
                                                             strip.hfGainDb.load() + d * 0.3f),
                                               std::memory_order_relaxed); break;
                case 7: strip.hfFreq.store (juce::jlimit (ChannelStripParams::kHfFreqMin,
                                                           ChannelStripParams::kHfFreqMax,
                                                           strip.hfFreq.load() + d * 100.0f),
                                             std::memory_order_relaxed); break;
                default: break;
            }
        }
        else
        {
            switch (stripIndex)
            {
                case 0: strip.compThresholdDb.store (juce::jlimit (ChannelStripParams::kCompThreshMin,
                                                                    ChannelStripParams::kCompThreshMax,
                                                                    strip.compThresholdDb.load() + d * 0.5f),
                                                       std::memory_order_relaxed); break;
                case 1: strip.compVcaRatio.store (juce::jlimit (ChannelStripParams::kCompRatioMin,
                                                                  ChannelStripParams::kCompRatioMax,
                                                                  strip.compVcaRatio.load() + d * 0.2f),
                                                    std::memory_order_relaxed); break;
                case 2: strip.compVcaAttack.store (juce::jlimit (ChannelStripParams::kCompAttackMin,
                                                                   ChannelStripParams::kCompAttackMax,
                                                                   strip.compVcaAttack.load() + d * 0.5f),
                                                     std::memory_order_relaxed); break;
                case 3: strip.compVcaRelease.store (juce::jlimit (ChannelStripParams::kCompReleaseMin,
                                                                    ChannelStripParams::kCompReleaseMax,
                                                                    strip.compVcaRelease.load() + d * 10.0f),
                                                      std::memory_order_relaxed); break;
                case 4: strip.compMakeupDb.store (juce::jlimit (ChannelStripParams::kCompMakeupMin,
                                                                  ChannelStripParams::kCompMakeupMax,
                                                                  strip.compMakeupDb.load() + d * 0.3f),
                                                    std::memory_order_relaxed); break;
                default: break;
            }
        }
        return;
    }

    const int bank = session.mcu.bank.load (std::memory_order_relaxed);
    const int trackIdx = bank * mcu::kStripsPerBank + stripIndex;
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return;
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

    // REW / FFWD publish held-state on both edges. TransportBar's timer
    // turns a short press into a marker jump and a hold into a 10x playhead
    // scrub, mirroring the on-screen Rewind / Forward buttons.
    if (noteNumber == mcu::btn::Rewind)
    {
        if (pressed) session.mcu.rewPressCount.fetch_add (1, std::memory_order_relaxed);
        session.mcu.rewHeld.store (pressed, std::memory_order_relaxed);
        return;
    }
    if (noteNumber == mcu::btn::FastForward)
    {
        if (pressed) session.mcu.ffwdPressCount.fetch_add (1, std::memory_order_relaxed);
        session.mcu.ffwdHeld.store (pressed, std::memory_order_relaxed);
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
            case mcu::btn::Loop:
                // Flipping savedLoopEnabled here wouldn't take effect (the
                // audio thread reads loop state from the Transport directly).
                // Defer the toggle to the message-thread drain, which owns the
                // Transport API.
                session.pendingTransportAction.store (
                    (int) PendingTransportAction::LoopToggle,
                    std::memory_order_relaxed);
                return;
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
                            std::int64_t blockStartSample) noexcept
{
    juce::ignoreUnused (blockStartSample);
    for (const auto meta : events)
    {
        const auto& m = meta.getMessage();
        const auto* raw = m.getRawData();
        const int sz = m.getRawDataSize();
        if (raw == nullptr || sz < 1) continue;
        const auto status = (std::uint8_t) raw[0];
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
                    session.setTrackFaderGrouped (t, db);
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
                // Jog wheel: same sign encoding as V-pot. Magnitude is
                // detent count; nudge the playhead by a fixed sample
                // count per detent (~50 ms at 48 kHz; close enough at
                // common sample rates — jog feel is approximate).
                // setPlayhead isn't RT-safe so queue via the existing
                // pendingTransportPlayhead atom drained on the message
                // thread (same channel Rewind / FFwd / MTC-chase use).
                //
                // Base the next target on any already-pending value
                // first so multiple jog ticks arriving in the same
                // audio block accumulate (otherwise rapid spins lose
                // all but the last detent). Only fall back to the
                // block-start playhead when no value is queued (-1
                // sentinel).
                const int magnitude = val & 0x3F;
                const int delta     = (val & 0x40) ? -magnitude : +magnitude;
                if (delta == 0) continue;
                constexpr std::int64_t kSamplesPerDetent = 2400;
                const auto stepSamples = (std::int64_t) delta * kSamplesPerDetent;
                const auto pending = session.pendingTransportPlayhead.load (
                    std::memory_order_relaxed);
                const std::int64_t base = (pending >= 0) ? pending : blockStartSample;
                const std::int64_t target = juce::jmax ((std::int64_t) 0,
                                                         base + stepSamples);
                session.pendingTransportPlayhead.store (target,
                                                          std::memory_order_relaxed);
                continue;
            }
        }
    }
}
} // namespace duskstudio
