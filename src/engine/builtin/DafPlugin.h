#pragma once

#include "../../foundation/TransportPosition.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace duskstudio::builtin
{
// One parameter of a DAF plug-in, as the plug-in declared it.
struct DafParamDesc
{
    std::string symbol;
    std::string name;
    std::string unit;
    float minValue     = 0.0f;
    float maxValue     = 1.0f;
    float defaultValue = 0.0f;
    bool isOutput  = false;
    bool isHidden  = false;
    bool isBoolean = false;
    bool isInteger = false;
    // A restricted enumeration: the only values the parameter takes, ascending.
    std::vector<float>       enumValues;
    std::vector<std::string> enumLabels;
};

// What a plug-in's editor tells the host as the user works it. Message thread.
struct DafEditorCallbacks
{
    // A control's gesture opening and closing, which is how the host learns which
    // parameter was touched last and when to hand keyboard focus back.
    std::function<void (std::uint32_t index, bool started)> gesture;
    std::function<void (std::uint32_t index, float value)> parameterEdited;
    std::function<void (const std::string& key, const std::string& value)> stateEdited;
    // The editor asking for a size of its own.
    std::function<void (std::uint32_t width, std::uint32_t height)> sizeRequested;
};

// A plug-in's own editor, embedded in a native parent the host owns. Every call is
// message-thread, and the editor is destroyed by releasing it.
class DafEditor
{
public:
    virtual ~DafEditor() = default;

    // Place the editor inside its parent. False when the display backend cannot
    // carry an embedded child at all, which is how native Wayland is refused.
    virtual bool setOffset (int x, int y) noexcept = 0;
    virtual void setSize (std::uint32_t width, std::uint32_t height) = 0;

    // One turn of the editor's own event loop. False once it has quit or its
    // graphics stack failed, and the host closes it.
    virtual bool idle() noexcept = 0;

    // Tell the editor a parameter's value, including the plug-in's output meters.
    virtual void parameterChanged (std::uint32_t index, float value) = 0;

    virtual std::uint32_t width() const noexcept = 0;
    virtual std::uint32_t height() const noexcept = 0;

    // The editor's own native window, for reading its pixels back; zero when it
    // has none.
    virtual std::uintptr_t nativeWindow() const noexcept { return 0; }
};

// One of Dusk's own DAF plug-ins compiled into the app, reached through a plain
// interface so that no DAF header reaches past the plug-in's bridge. Each bridge
// is compiled against its plug-in's DafPluginInfo.h inside a DAF namespace of its
// own, which is what lets several plug-ins share one program.
//
// Threading follows DAF's PluginExporter: activate / deactivate and host state
// save/load are message-thread with audio fenced; setParameterValue,
// setTimePosition and run belong to the audio thread. A stateful unit editor may
// call setState concurrently with run, so every stateful plug-in compiled through
// this bridge must publish editor state to its DSP without blocking the reader.
class DafPlugin
{
public:
    virtual ~DafPlugin() = default;

    virtual const std::vector<DafParamDesc>& params() const noexcept = 0;
    virtual int numInputs() const noexcept = 0;
    virtual int numOutputs() const noexcept = 0;

    virtual void activate (double sampleRate, int maxBlockFrames) = 0;
    virtual void deactivate() = 0;

    virtual float getParameterValue (std::uint32_t index) const noexcept = 0;
    virtual void  setParameterValue (std::uint32_t index, float value) noexcept = 0;

    // Full-state access. A plug-in without DAF full state reports a count of zero.
    virtual std::uint32_t getStateCount() const noexcept = 0;
    virtual const std::string& getStateKey (std::uint32_t index) const noexcept = 0;
    virtual const std::string& getStateDefaultValue (std::uint32_t index) const noexcept = 0;
    virtual std::string getStateValue (const std::string& key) const = 0;
    virtual void setState (const std::string& key, const std::string& value) = 0;
    virtual void  setTimePosition (const dusk::TransportPosition& position) noexcept = 0;
    virtual void  run (const float* const* inputs, float* const* outputs,
                       std::uint32_t frames) noexcept = 0;

    virtual int latencySamples() const noexcept = 0;

    // False for a plug-in with no editor, and for a build that left the editor
    // half out. The size is the editor's own, in its design pixels.
    virtual bool hasEditor() const noexcept = 0;
    virtual std::uint32_t editorWidth() const noexcept = 0;
    virtual std::uint32_t editorHeight() const noexcept = 0;

    // Null with errorOut set when there is no editor to build or the display
    // cannot carry one.
    virtual std::unique_ptr<DafEditor> createEditor (std::uintptr_t nativeParent,
                                                     std::uint32_t width,
                                                     std::uint32_t height,
                                                     double scaleFactor,
                                                     DafEditorCallbacks callbacks,
                                                     std::string& errorOut) = 0;
};

// Defined by each plug-in's bridge, in the builds that compile it.
std::unique_ptr<DafPlugin> createTapeEcho2();
std::unique_ptr<DafPlugin> createDuskVerb2();
} // namespace duskstudio::builtin
