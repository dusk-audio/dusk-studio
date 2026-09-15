#pragma once

#include "BuiltinUnit.h"
#include "DafPlugin.h"
#include "../hosting/INativeInstance.h"
#include "../hosting/SpscRing.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
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
// State save/load is message-thread and must be called while the engine's process
// gate fences the audio callback, as NativeInsertSlot requires for state access.
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

    // The plug-in's own editor. The host drives it; the unit only builds it and
    // says how big the plug-in wants it.
    bool hasEditor() const noexcept { return plugin->hasEditor(); }
    std::uint32_t editorWidth() const noexcept { return plugin->editorWidth(); }
    std::uint32_t editorHeight() const noexcept { return plugin->editorHeight(); }
    std::unique_ptr<DafEditor> createEditor (std::uintptr_t nativeParent,
                                             std::uint32_t width, std::uint32_t height,
                                             double scaleFactor,
                                             DafEditorCallbacks callbacks,
                                             std::string& errorOut);

    static constexpr int kStateVersion = 3;
    static constexpr std::uint32_t kWriteRingSize = 1024;

private:
    struct ParamWrite
    {
        std::uint32_t index;
        float value;
    };

    // The audio thread, or the message thread while the audio thread is fenced.
    void pushAllParams() noexcept;
    void refreshParamMirrors() noexcept;
    void applyEditorState (const std::string& key, const std::string& value);

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
