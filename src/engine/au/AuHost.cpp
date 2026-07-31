#include "AuHost.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace duskstudio::au
{
namespace
{
template <typename T, typename U>
void setIfPresent (T* target, U value) noexcept
{
    if (target != nullptr) *target = static_cast<T> (value);
}
} // namespace

AuHost::AuHost()
{
    callbackInfo.hostUserData = this;
    callbackInfo.beatAndTempoProc = &AuHost::beatAndTempo;
    callbackInfo.musicalTimeLocationProc = &AuHost::musicalTime;
    callbackInfo.transportStateProc = &AuHost::transportState;
    callbackInfo.transportStateProc2 = &AuHost::transportState2;
}

void AuHost::beginBlock (const dusk::TransportPosition* position, int numFrames) noexcept
{
    if (position == nullptr)
    {
        current = {};
        stateChanged = havePosition;
        havePosition = false;
        previousBlockFrames = numFrames;
        return;
    }

    const auto expectedStart = previousBlockStart + previousBlockFrames;
    stateChanged = ! havePosition || current.isPlaying != position->isPlaying
        || current.isRecording != position->isRecording
        || current.isLooping != position->isLooping
        || position->timeInSamples != expectedStart;
    current = *position;
    previousBlockStart = position->timeInSamples;
    previousBlockFrames = numFrames;
    havePosition = true;
}

OSStatus AuHost::beatAndTempo (void* user, Float64* beat, Float64* tempo) noexcept
{
    auto& self = *static_cast<AuHost*> (user);
    setIfPresent (beat, self.current.ppqPosition);
    setIfPresent (tempo, self.current.bpm);
    return noErr;
}

OSStatus AuHost::musicalTime (void* user, UInt32* deltaToBeat, Float32* numerator,
                              UInt32* denominator, Float64* downBeat) noexcept
{
    auto& self = *static_cast<AuHost*> (user);
    const double bpm = std::max (1.0, self.current.bpm);
    const double fraction = self.current.ppqPosition - std::floor (self.current.ppqPosition);
    const double samples = (fraction <= 1.0e-12 ? 0.0 : 1.0 - fraction)
        * (60.0 / bpm) * self.sampleRate;
    setIfPresent (deltaToBeat, static_cast<UInt32> (std::clamp (
        samples, 0.0, static_cast<double> (std::numeric_limits<UInt32>::max()))));
    setIfPresent (numerator, std::max (1, self.current.timeSignatureNumerator));
    setIfPresent (denominator, std::max (1, self.current.timeSignatureDenominator));
    // ppqPosition is expressed in quarter-note beats. A measure therefore
    // spans numerator * 4 / denominator of those beats (for example, 6/8 is
    // three quarter-note beats, not six).
    const auto beatsPerBar = static_cast<double> (
        std::max (1, self.current.timeSignatureNumerator)) * 4.0
        / static_cast<double> (std::max (1, self.current.timeSignatureDenominator));
    setIfPresent (downBeat, std::floor (self.current.ppqPosition / beatsPerBar) * beatsPerBar);
    return noErr;
}

OSStatus AuHost::transportState (void* user, Boolean* playing, Boolean* changed,
                                 Float64* sample, Boolean* cycling,
                                 Float64* cycleStart, Float64* cycleEnd) noexcept
{
    auto& self = *static_cast<AuHost*> (user);
    setIfPresent (playing, self.current.isPlaying);
    setIfPresent (changed, self.stateChanged);
    setIfPresent (sample, self.current.timeInSamples);
    // TransportPosition carries no loop bounds, and a cycling flag with a
    // zero-length range reads as a degenerate cycle - report not-cycling until
    // real start/end beats exist to hand over.
    setIfPresent (cycling, false);
    setIfPresent (cycleStart, 0.0);
    setIfPresent (cycleEnd, 0.0);
    self.stateChanged = false;
    return noErr;
}

OSStatus AuHost::transportState2 (void* user, Boolean* playing, Boolean* recording,
                                  Boolean* changed, Float64* sample, Boolean* cycling,
                                  Float64* cycleStart, Float64* cycleEnd) noexcept
{
    auto& self = *static_cast<AuHost*> (user);
    setIfPresent (recording, self.current.isRecording);
    return transportState (user, playing, changed, sample, cycling, cycleStart, cycleEnd);
}
} // namespace duskstudio::au
