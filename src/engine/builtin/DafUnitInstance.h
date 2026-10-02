#pragma once

#include "BuiltinUnit.h"
#include "DafPlugin.h"
#include "../../foundation/MidiBuffer.h"
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
    // legacyParams, when given, restores a blob the knob unit this plug-in
    // replaced wrote under the same id.
    DafUnitInstance (std::string stateId, std::unique_ptr<DafPlugin> plugin,
                     const std::vector<LegacyParam>* legacyParams = nullptr);
    ~DafUnitInstance() override;

    const hosting::PortLayout& portLayout() const noexcept override { return layout; }
    bool activate (double sampleRate, int maxBlockFrames, std::string& errorOut) override;
    void deactivate() override;
    bool reactivate (double sampleRate, int maxBlockFrames, std::string& errorOut) override;
    bool isActive() const noexcept override { return active.load (std::memory_order_acquire); }
    void processBlock (const hosting::PortBuffers& io) noexcept override;
    // Audio thread, in place of processBlock for a block the strip does not run
    // the plug-in (a frozen track): the notes its editor's keyboard queued meanwhile
    // are dropped, not held to play in a burst once it runs again.
    void skipBlock() noexcept;
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
    static constexpr std::uint32_t kNoteRingSize = 256;

private:
    struct ParamWrite
    {
        std::uint32_t index;
        float value;
        std::uint32_t sequence;
    };

    struct EditorNote
    {
        std::uint8_t status;
        std::uint8_t note;
        std::uint8_t velocity;
    };

    // Message thread: a note from the editor's keyboard, played at the top of the
    // next block.
    void playEditorNote (std::uint8_t channel, std::uint8_t note, std::uint8_t velocity) noexcept;
    // Audio thread: the editor's notes, then the block's own MIDI, into events.
    std::uint32_t gatherEvents (const dusk::MidiBuffer* midi, int numFrames) noexcept;

    // The audio thread, or the message thread while the audio thread is fenced.
    void pushAllParams() noexcept;
    void refreshParamMirrors() noexcept;
    void applyEditorState (const std::string& key, const std::string& value);
    const LegacyParam* legacyParam (const std::string& symbol) const noexcept;

    std::string id;
    std::unique_ptr<DafPlugin> plugin;
    const std::vector<LegacyParam>* legacy = nullptr;
    std::vector<ParamInfo> infos;
    std::vector<std::vector<std::string>> choiceText;
    std::vector<std::vector<const char*>> choicePointers;
    // The mirror. Each entry packs the value with the sequence number of the
    // message thread's last write to it, which only that thread advances, so the
    // audio thread can tell a write the plug-in has not been handed yet from one
    // it has, even when the two carry the same value.
    std::vector<std::atomic<std::uint64_t>> values;
    std::vector<std::uint32_t> outputIndices;
    hosting::SpscRing<ParamWrite, kWriteRingSize> writes;
    hosting::SpscRing<EditorNote, kNoteRingSize> editorNotes;
    // Sized once for an instrument and left empty for an effect, which plays no MIDI.
    std::vector<DafMidiEvent> events;
    // The sequence number of the last write to each parameter the plug-in has been
    // handed. The audio thread's, or the message thread's while that is fenced.
    std::vector<std::uint32_t> appliedWrites;
    std::atomic<bool> resyncAll { false };
    hosting::PortLayout layout;
    std::atomic<bool> active { false };
    int preparedFrames = 0;
};
} // namespace duskstudio::builtin
