#pragma once

#include "AuBundle.h"
#include "AuHost.h"
#include "../hosting/INativeInstance.h"

#include <AudioToolbox/AudioUnitUtilities.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace duskstudio::au
{
class AuInstance final : public hosting::INativeInstance
{
public:
    struct ParamInfo
    {
        std::uint32_t id = 0;
        std::string name;
        std::string label;
        double minValue = 0.0;
        double maxValue = 1.0;
        double defaultValue = 0.0;
        bool writable = true;
    };

    AuInstance() = default;
    ~AuInstance() override;
    AuInstance (const AuInstance&) = delete;
    AuInstance& operator= (const AuInstance&) = delete;

    bool create (AuBundle& bundle, const std::string& identifier, std::string& errorOut);

    const hosting::PortLayout& portLayout() const noexcept override { return layout; }
    bool activate (double sampleRate, int maxBlockFrames, std::string& errorOut) override;
    void deactivate() override;
    bool reactivate (double sampleRate, int maxBlockFrames, std::string& errorOut) override;
    bool isActive() const noexcept override { return active.load (std::memory_order_acquire); }
    void processBlock (const hosting::PortBuffers& io) noexcept override;
    bool saveState (std::vector<std::uint8_t>& out) const override;
    bool loadState (const std::vector<std::uint8_t>& in) override;
    int getLatencySamples() const noexcept override
        { return latencySamples.load (std::memory_order_relaxed); }

    int paramCount() const noexcept { return static_cast<int> (parameters.size()); }
    const ParamInfo* paramInfo (int index) const noexcept;
    bool getParamValue (std::uint32_t id, double& out) const;
    void setParamValue (std::uint32_t id, double value);
    int lastTouchedParamIndex() const noexcept
        { return lastTouched.load (std::memory_order_relaxed); }

    // Message thread: property callbacks only set a flag, so the actual latency
    // property query stays out of AudioUnitRender even if a plugin signals there.
    bool refreshLatencyIfChanged() noexcept;

    void* nativeAudioUnit() const noexcept { return audioUnit; }

private:
    bool configureBuses (double sampleRate, std::string& errorOut);
    bool configureStream (AudioUnitScope scope, UInt32 element, UInt32 channels,
                          double sampleRate, std::string& errorOut);
    void enumerateParameters();
    void installListeners();
    void removeListeners() noexcept;
    void refreshLatency() noexcept;
    void dispose() noexcept;
    AudioBufferList* outputBufferList() noexcept;

    static OSStatus inputRenderCallback (void*, AudioUnitRenderActionFlags*,
                                         const AudioTimeStamp*, UInt32, UInt32,
                                         AudioBufferList*) noexcept;
    OSStatus provideInput (UInt32 bus, UInt32 frames, AudioBufferList*) noexcept;
    static void parameterChanged (void*, void*, const AudioUnitParameter*,
                                  AudioUnitParameterValue) noexcept;
    static void propertyChanged (void*, AudioUnit, AudioUnitPropertyID,
                                 AudioUnitScope, AudioUnitElement) noexcept;

    AudioUnit audioUnit = nullptr;
    ComponentId componentId;
    hosting::PortLayout layout;
    AuHost host;
    std::vector<ParamInfo> parameters;
    std::vector<AudioUnitParameter> observedParameters;
    AUParameterListenerRef parameterListener = nullptr;
    std::atomic<int> lastTouched { -1 };
    std::atomic<bool> latencyDirty { false };
    std::atomic<int> latencySamples { 0 };
    std::atomic<bool> active { false };

    const hosting::PortBuffers* currentIo = nullptr;
    UInt32 mainInputElement = 0;
    UInt32 sidechainInputElement = std::numeric_limits<UInt32>::max();
    UInt32 mainOutputElement = 0;
    UInt32 outputChannels = 0;
    UInt32 silenceChannels = 0;
    int maximumFrames = 0;
    double currentSampleRate = 0.0;
    double sampleTime = 0.0;
    std::vector<float> silence;
    std::unique_ptr<std::byte[]> outputListStorage;
};
} // namespace duskstudio::au
