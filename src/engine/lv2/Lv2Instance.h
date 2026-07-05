#pragma once

#include "../hosting/INativeInstance.h"

#include <juce_core/juce_core.h>
#include <memory>
#include <string>
#include <vector>

namespace duskstudio::lv2
{
class Lv2Bundle;

// One LV2 plugin instance driven through the host-agnostic INativeInstance, so a
// plugin slot hosts CLAP, VST3 and LV2 alike. Mirrors ClapInstance: create + init
// from a bundle + URI, activate at a fixed sample rate / block, process audio.
// lilv/lv2 stay behind a pImpl so this header pulls in no LV2 headers.
//
// LV2 fixes the sample rate at instantiate (unlike CLAP's activate), so reactivate
// re-instantiates the plugin, carrying the full state across (control ports + the
// plugin's state:interface blob via saveState/loadState — lilv state serialized
// as Turtle). The suil editor attaches through the opaque accessors below.
class Lv2Instance : public hosting::INativeInstance
{
public:
    // One plugin parameter: an input control port, or a patch:writable float
    // property (JUCE-built LV2s expose ONLY the latter). Values are in the
    // parameter's own units (min..max), like CLAP. `id` is opaque — port index
    // for control ports, a marked property token for patch properties; round-trip
    // it through get/setParamValue, don't interpret it.
    struct ParamInfo
    {
        uint32_t    id = 0;
        std::string name;
        float       minValue = 0.0f, maxValue = 1.0f, defaultValue = 0.0f;
        bool        stepped = false;           // toggled / integer / enumeration
        bool        isPatchProperty = false;   // written as a patch:Set atom, not a port
    };

    Lv2Instance();
    ~Lv2Instance() override;
    Lv2Instance (const Lv2Instance&)            = delete;
    Lv2Instance& operator= (const Lv2Instance&) = delete;

    // Resolve `uri` in `bundle`, classify its ports, and build the PortLayout. The
    // bundle owns the LilvWorld backing the plugin, so it MUST outlive this instance.
    // False (+ errorOut) on failure.
    bool create (const Lv2Bundle& bundle, const std::string& uri, std::string& errorOut);

    const hosting::PortLayout& portLayout() const noexcept override;

    bool activate (double sampleRate, int maxBlockFrames, std::string& errorOut) override;
    void deactivate() override;
    bool reactivate (double sampleRate, int maxBlockFrames, std::string& errorOut) override;
    bool isActive() const noexcept override;

    void processBlock (const hosting::PortBuffers& io) noexcept override;

    bool saveState (std::vector<uint8_t>& out) const override;
    bool loadState (const std::vector<uint8_t>& in) override;

    // Session-scoped directory for FILE-BACKED plugin state (a sampler's
    // loaded bank, a convolution IR). Empty = blob-only save (control ports
    // + in-memory state:interface — the pre-file-state behaviour). Set by
    // the engine before saveState/loadState. saveState rotates
    // <dir>/prev <- <dir>/cur and snapshots referenced files into cur/;
    // loadState resolves the blob's abstract paths against cur/.
    void setStateDirectory (const juce::File& dir);

    int getLatencySamples() const noexcept override;

    // ── Editor support (message thread; opaque so this header stays lilv-free) ──
    // The LilvPlugin*, the live LilvInstance* (null when inactive), and this
    // instance's URID map/unmap LV2_Features — the UI must share the plugin's
    // URID space, so the editor forwards these instead of building its own.
    void*       lilvWorld()        const noexcept;
    const void* lilvPlugin()       const noexcept;
    void*       lilvInstance()     const noexcept;
    void*       uridMapFeature()   const noexcept;
    void*       uridUnmapFeature() const noexcept;

    // UI → plugin control-port write (ui:floatProtocol). Staged into a lock-free
    // ring; the audio thread applies it at the top of its next processBlock so
    // nothing writes portValues while run() reads it.
    void setControlPortValue (uint32_t portIndex, float value) noexcept;
    // Same, from the plugin's OWN editor — also stamps the MIDI Learn
    // last-touched tracker (host-initiated writes must not self-stamp).
    void setControlPortValueFromUi (uint32_t portIndex, float value) noexcept;
    // Port index for the suil port-index-by-symbol callback; -1 when unknown.
    int portIndexForSymbol (const char* symbol) const noexcept;

    // Message thread (engine drain timer): fold the plugin's outgoing
    // patch:Set / patch:Put responses into the read-back shadow, so
    // plugin-side property changes (a preset loaded in its own UI) don't
    // leave getParamValue stale.
    void drainPatchFeedback();

    // ui:eventTransfer writes from the plugin's own editor (message thread):
    // forwarded onto the control atom input port so the DSP hears them without
    // relying on the instance-access shortcut, and patch:Set events stamp the
    // MIDI Learn last-touched tracker + the patch read-back shadow.
    uint32_t uiEventTransferUrid() const noexcept;
    void forwardUiAtomEvent (const void* atomData, uint32_t sizeBytes) noexcept;

    // Parameters (message thread). Enumerated once at create().
    int              paramCount() const noexcept;
    const ParamInfo* paramInfo (int index) const noexcept;
    // Patch properties read back the last host/UI-written value (the plugin's
    // own patch:Put responses aren't parsed yet).
    bool getParamValue (uint32_t paramId, double& out) const;
    // Clamps to the parameter's range; control ports stage through
    // setControlPortValue, patch properties as a patch:Set atom on the control
    // atom port (applied at the top of the next process block).
    void setParamValue (uint32_t paramId, double value) noexcept;
    // Index (into paramInfo order) of the port the user last moved in the
    // plugin's own UI; -1 when nothing has been touched.
    int lastTouchedParamIndex() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio::lv2
