#pragma once

#include "BuiltinUnit.h"
#include "DafPlugin.h"
#include "../hosting/INativeInstance.h"
#include "../hosting/SpscRing.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace duskstudio::builtin
{
// A DAF plug-in run in process as a native instance: its audio path, the
// transport it syncs to, its latency, its session state, and a parameter surface
// indexed exactly as the plug-in indexes its parameters, so an index stays valid
// the way a DAF parameter id does. Output and hidden parameters keep their
// indices and are marked hidden.
//
// Parameter writes come from the message thread (the session, an editor, a MIDI
// binding) and reach the plug-in only on the audio thread, through a
// single-producer ring drained at the top of each block. The message thread reads
// back what it wrote from a mirror, into which the audio thread copies the output
// parameters after each block.
//
// Threading otherwise matches INativeInstance.
class DafUnitInstance final : public hosting::INativeInstance
{
public:
    // stateId names the blob saveState writes and the only one loadState accepts.
    DafUnitInstance (std::string stateId, std::unique_ptr<DafPlugin> plugin);
    ~DafUnitInstance() override;

    const hosting::PortLayout& portLayout() const noexcept override { return layout; }
    bool activate (double sampleRate, int maxBlockFrames, std::string& errorOut) override;
    void deactivate() override;
    bool reactivate (double sampleRate, int maxBlockFrames, std::string& errorOut) override;
    bool isActive() const noexcept override { return active.load (std::memory_order_acquire); }
    void processBlock (const hosting::PortBuffers& io) noexcept override;
    bool saveState (std::vector<std::uint8_t>& out) const override;
    bool loadState (const std::vector<std::uint8_t>& in) override;
    int  getLatencySamples() const noexcept override;

    // Message thread.
    int paramCount() const noexcept { return (int) infos.size(); }
    const ParamInfo* paramInfo (int index) const noexcept;
    float getParamValue (int index) const noexcept;
    void  setParamValue (int index, float value) noexcept;

    static constexpr int kStateVersion = 2;
    static constexpr std::uint32_t kWriteRingSize = 1024;

private:
    struct ParamWrite
    {
        std::uint32_t index;
        float value;
    };

    // The audio thread, or the message thread while the audio thread is fenced.
    void pushAllParams() noexcept;

    std::string id;
    std::unique_ptr<DafPlugin> plugin;
    std::vector<ParamInfo> infos;
    std::vector<std::vector<std::string>> choiceText;
    std::vector<std::vector<const char*>> choicePointers;
    std::vector<std::atomic<float>> values;
    std::vector<std::uint32_t> outputIndices;
    hosting::SpscRing<ParamWrite, kWriteRingSize> writes;
    std::atomic<bool> resyncAll { false };
    hosting::PortLayout layout;
    std::atomic<bool> active { false };
    int preparedFrames = 0;
};
} // namespace duskstudio::builtin
