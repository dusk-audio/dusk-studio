// A VST3 effect with a separate processor and edit controller. The processor
// learns a host parameter change only through process(), so a host that sets
// a value and reads the component state before the next block saves the old
// value. Both halves write their own view of Gain into their state chunks.
//
// Hold blocks the controller's setParamNormalized - the host's message thread -
// until the file named by DUSKSTUDIO_SPLIT_STATE_HOLD_FILE exists, so a test can
// order that thread's queued work against a sandbox child's state capture.
//
// A block whose first input sample is exactly kEditCue makes the processor move
// its own Gain and report it as an output parameter change: a change the
// plug-in made, with no host parameter write behind it.
//
// Two reports that are not state changes: every block writes the input peak to
// a read-only Meter, and every activation re-announces the latency through
// restartComponent, as plug-ins that recompute it in setActive do.

#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

namespace Steinberg::Vst
{
namespace
{
constexpr ParamID kGain = 200;
constexpr ParamID kHold = 201;
constexpr ParamID kMeter = 202;
constexpr const char* kRestartLatency = "RestartLatency";
constexpr float kEditCue = 0.75f;
constexpr float kEditedGain = 0.5f;

const FUID kProcessorUid (0x2B0C61D4, 0x7E3A4F55, 0x9C1D2E07, 0x51A6B3F8);
const FUID kControllerUid (0x6E4F1A92, 0x0D8B4C33, 0xB27E5A10, 0xC39F8D64);

bool writeGain (IBStream* stream, float gain)
{
    if (stream == nullptr) return false;
    std::uint32_t bits = 0;
    static_assert (sizeof (bits) == sizeof (gain));
    std::memcpy (&bits, &gain, sizeof (gain));
    std::array<uint8, 4> bytes {};
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<uint8> (bits >> (index * 8));
    int32 count = 0;
    return stream->write (bytes.data(), static_cast<int32> (bytes.size()), &count) == kResultOk
        && count == static_cast<int32> (bytes.size());
}

bool readGain (IBStream* stream, float& gain)
{
    if (stream == nullptr) return false;
    std::array<uint8, 4> bytes {};
    int32 count = 0;
    if (stream->read (bytes.data(), static_cast<int32> (bytes.size()), &count) != kResultOk
        || count != static_cast<int32> (bytes.size()))
        return false;
    std::uint32_t bits = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bits |= static_cast<std::uint32_t> (bytes[index]) << (index * 8);
    float value = 0.0f;
    std::memcpy (&value, &bits, sizeof (value));
    if (! std::isfinite (value) || value < 0.0f || value > 1.0f) return false;
    gain = value;
    return true;
}

void holdUntilReleased()
{
    const char* path = std::getenv ("DUSKSTUDIO_SPLIT_STATE_HOLD_FILE");
    if (path == nullptr || *path == '\0') return;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (20);
    std::error_code error;
    while (! std::filesystem::exists (std::filesystem::u8path (path), error)
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
}

class SplitStateProcessor final : public AudioEffect
{
public:
    SplitStateProcessor() { setControllerClass (kControllerUid); }

    static FUnknown* createInstance (void*)
    {
        return static_cast<IAudioProcessor*> (new SplitStateProcessor);
    }

    tresult PLUGIN_API initialize (FUnknown* context) override
    {
        const auto result = AudioEffect::initialize (context);
        if (result != kResultOk) return result;
        addAudioInput (STR16 ("Input"), SpeakerArr::kStereo);
        addAudioOutput (STR16 ("Output"), SpeakerArr::kStereo);
        return kResultOk;
    }

    tresult PLUGIN_API setActive (TBool state) override
    {
        const auto result = AudioEffect::setActive (state);
        if (result == kResultOk && state) sendMessageID (kRestartLatency);
        return result;
    }

    tresult PLUGIN_API process (ProcessData& data) override
    {
        if (auto* changes = data.inputParameterChanges)
        {
            for (int32 index = 0; index < changes->getParameterCount(); ++index)
            {
                auto* queue = changes->getParameterData (index);
                if (queue == nullptr || queue->getParameterId() != kGain || queue->getPointCount() <= 0)
                    continue;
                int32 offset = 0;
                ParamValue value = 0.0;
                if (queue->getPoint (queue->getPointCount() - 1, offset, value) == kResultOk)
                    gain.store (static_cast<float> (value), std::memory_order_relaxed);
            }
        }

        if (data.numSamples <= 0 || data.numInputs < 1 || data.numOutputs < 1
            || data.symbolicSampleSize != kSample32)
            return kResultOk;
        const auto& input = data.inputs[0];
        auto& output = data.outputs[0];
        const auto channels = std::min (input.numChannels, output.numChannels);
        if (channels < 1 || input.channelBuffers32 == nullptr || output.channelBuffers32 == nullptr)
            return kResultOk;

        if (input.channelBuffers32[0] != nullptr && input.channelBuffers32[0][0] == kEditCue
            && data.outputParameterChanges != nullptr)
        {
            gain.store (kEditedGain, std::memory_order_relaxed);
            int32 queueIndex = 0;
            if (auto* queue = data.outputParameterChanges->addParameterData (kGain, queueIndex))
            {
                int32 pointIndex = 0;
                queue->addPoint (0, kEditedGain, pointIndex);
            }
        }

        const float scale = gain.load (std::memory_order_relaxed);
        float peak = 0.0f;
        for (int32 channel = 0; channel < channels; ++channel)
        {
            const auto* in = input.channelBuffers32[channel];
            auto* out = output.channelBuffers32[channel];
            if (in == nullptr || out == nullptr) continue;
            for (int32 frame = 0; frame < data.numSamples; ++frame)
            {
                peak = std::max (peak, std::abs (in[frame]));
                out[frame] = in[frame] * scale;
            }
        }
        if (data.outputParameterChanges != nullptr)
        {
            int32 queueIndex = 0;
            if (auto* queue = data.outputParameterChanges->addParameterData (kMeter, queueIndex))
            {
                int32 pointIndex = 0;
                queue->addPoint (0, std::min (peak, 1.0f), pointIndex);
            }
        }
        return kResultOk;
    }

    tresult PLUGIN_API setState (IBStream* state) override
    {
        float value = 0.0f;
        if (! readGain (state, value)) return kResultFalse;
        gain.store (value, std::memory_order_relaxed);
        return kResultOk;
    }

    tresult PLUGIN_API getState (IBStream* state) override
    {
        return writeGain (state, gain.load (std::memory_order_relaxed)) ? kResultOk : kResultFalse;
    }

private:
    std::atomic<float> gain { 1.0f };
};

class SplitStateController final : public EditController
{
public:
    static FUnknown* createInstance (void*)
    {
        return static_cast<IEditController*> (new SplitStateController);
    }

    tresult PLUGIN_API initialize (FUnknown* context) override
    {
        const auto result = EditController::initialize (context);
        if (result != kResultOk) return result;
        parameters.addParameter (STR16 ("Gain"), nullptr, 0, 1.0, ParameterInfo::kCanAutomate, kGain);
        parameters.addParameter (STR16 ("Hold"), nullptr, 1, 0.0, ParameterInfo::kNoFlags, kHold);
        parameters.addParameter (STR16 ("Meter"), nullptr, 0, 0.0, ParameterInfo::kIsReadOnly, kMeter);
        return kResultOk;
    }

    tresult PLUGIN_API notify (IMessage* message) override
    {
        if (message == nullptr || std::strcmp (message->getMessageID(), kRestartLatency) != 0)
            return EditController::notify (message);
        if (componentHandler) componentHandler->restartComponent (kLatencyChanged);
        return kResultOk;
    }

    tresult PLUGIN_API setParamNormalized (ParamID id, ParamValue value) override
    {
        if (id == kHold && value >= 0.5) holdUntilReleased();
        return EditController::setParamNormalized (id, value);
    }

    tresult PLUGIN_API setComponentState (IBStream* state) override
    {
        float value = 0.0f;
        if (! readGain (state, value)) return kResultFalse;
        return EditController::setParamNormalized (kGain, value);
    }

    tresult PLUGIN_API setState (IBStream* state) override
    {
        float value = 0.0f;
        if (! readGain (state, value)) return kResultFalse;
        return EditController::setParamNormalized (kGain, value);
    }

    tresult PLUGIN_API getState (IBStream* state) override
    {
        return writeGain (state, static_cast<float> (getParamNormalized (kGain))) ? kResultOk
                                                                                    : kResultFalse;
    }
};
}
}

BEGIN_FACTORY_DEF ("Dusk Audio", "https://dusk.audio", "support@dusk.audio")
    DEF_CLASS2 (INLINE_UID_FROM_FUID (Steinberg::Vst::kProcessorUid),
                PClassInfo::kManyInstances, kVstAudioEffectClass,
                "Dusk Split State Fixture", Steinberg::Vst::kDistributable, "Fx", "1.0.0",
                kVstVersionString, Steinberg::Vst::SplitStateProcessor::createInstance)
    DEF_CLASS2 (INLINE_UID_FROM_FUID (Steinberg::Vst::kControllerUid),
                PClassInfo::kManyInstances, kVstComponentControllerClass,
                "Dusk Split State Fixture Controller", 0, "", "1.0.0",
                kVstVersionString, Steinberg::Vst::SplitStateController::createInstance)
END_FACTORY
