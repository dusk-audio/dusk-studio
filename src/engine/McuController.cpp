#include "McuController.h"
#include "McuProtocol.h"
#include "Transport.h"
#include "../session/Session.h"

namespace duskstudio
{
McuController::McuController (Session& sessionRef) noexcept
    : session (sessionRef)
{
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
