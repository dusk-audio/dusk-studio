#pragma once

#include "DafEditorHost.h"
#include "../../session/CompParams.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace duskstudio
{
class MultiCompStage;
}

namespace duskstudio::imgui
{
// The session values behind one compressor, which its editor reads and writes.
struct CompEditorTarget
{
    CompParams* comp = nullptr;
    std::atomic<bool>* engaged = nullptr;
    // A channel strip's mode and its picked flag; null on a bus and the master.
    std::atomic<int>* stripMode = nullptr;
    std::atomic<bool>* modePicked = nullptr;
    // Set where the face engages the compressor from one control only (the bus
    // and master threshold handle). Unset, every control but the mode and the
    // bypass engages it, except a switch turned off, as on the channel strip.
    std::optional<CompParamId> engagedOnlyBy;
    // False where BYPASS is the only control that switches the compressor
    // (the mastering multiband, whose header carries its engage switch).
    bool editsEngage = true;
};

// What the editor shows for one of the plug-in's parameter indices.
float compEditorValue (const MultiCompStage& stage, const CompEditorTarget& target, int index) noexcept;

// An edit the editor made, stored where the strip face, MIDI and a control
// surface read it, with the engage and mode-picked side effects the faces have.
void compEditorEdit (const MultiCompStage& stage, const CompEditorTarget& target,
                     int index, float value) noexcept;

// One compressor's Multi-Comp 2 editor: the plug-in's own face, hosted by a
// DafEditorHost over a native parent and driving the session through the
// stage's mapping. The editor's meters read the instance it was built from, so
// it is built from the stage processing the compressor's audio, and rebuilt in
// place, the popup kept up, when another stage takes over (a channel strip
// changing width). The engine owns every stage for the life of the app.
class CompEditorHost final
{
public:
    CompEditorHost (std::function<MultiCompStage&()> runningStage, CompEditorTarget target);
    ~CompEditorHost();

    // closed runs once the editor has gone, whether asked to or forced by a
    // graphics failure or a rebuild that could not open.
    void setCallbacks (DafEditorHost::Callbacks callbacks);

    // The design size of the editor the running stage builds.
    std::uint32_t designWidth() const;
    std::uint32_t designHeight() const;

    bool open (std::uintptr_t nativeParent, DafEditorHost::Geometry geometry);
    const std::string& lastOpenFailure() const noexcept;
    void close();
    void shutdown();
    bool isOpen() const noexcept;
    bool hasRenderedFrame() const noexcept;
    std::uintptr_t nativeWindow() const noexcept;

    // The stage the open editor draws; null while none is open.
    const MultiCompStage* shownStage() const noexcept;

    // By the plug-in's parameter symbol: work one control through the
    // callbacks the editor was handed, and read what the editor shows.
    bool editForScenario (const std::string& symbol, float value);
    std::optional<float> shownValueForScenario (const std::string& symbol) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio::imgui
