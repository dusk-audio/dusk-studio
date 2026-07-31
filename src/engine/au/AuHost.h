#pragma once

#include "../../foundation/TransportPosition.h"

#include <AudioToolbox/AudioToolbox.h>

namespace duskstudio::au
{
// Lock-free host-transport callbacks called synchronously from AudioUnitRender.
// beginBlock publishes a plain snapshot on the same audio thread immediately
// before render, so the callbacks never query engine state or cross threads.
class AuHost
{
public:
    AuHost();

    const HostCallbackInfo& callbacks() const noexcept { return callbackInfo; }
    void setSampleRate (double rate) noexcept { sampleRate = rate; }
    void beginBlock (const dusk::TransportPosition* position, int numFrames) noexcept;

private:
    static OSStatus beatAndTempo (void*, Float64*, Float64*) noexcept;
    static OSStatus musicalTime (void*, UInt32*, Float32*, UInt32*, Float64*) noexcept;
    static OSStatus transportState (void*, Boolean*, Boolean*, Float64*, Boolean*,
                                    Float64*, Float64*) noexcept;
    static OSStatus transportState2 (void*, Boolean*, Boolean*, Boolean*, Float64*,
                                     Boolean*, Float64*, Float64*) noexcept;

    HostCallbackInfo callbackInfo {};
    dusk::TransportPosition current {};
    double sampleRate = 48000.0;
    std::int64_t previousBlockStart = 0;
    int previousBlockFrames = 0;
    bool havePosition = false;
    bool stateChanged = true;
};
} // namespace duskstudio::au
