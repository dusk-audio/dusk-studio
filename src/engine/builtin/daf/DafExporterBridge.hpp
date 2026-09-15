#pragma once

// Compiled only into a DAF unit's own static library, against that plug-in's
// DafPluginInfo.h and inside the DAF namespace the library defines.
#if ! defined (DAF_PLUGIN_TARGET_STATIC) || ! defined (DAF_NAMESPACE)
 #error "a DAF unit bridge needs its library's static target and DAF namespace"
#endif

#include "../DafPlugin.h"

#include "src/DafPluginInternal.hpp"

#if DUSKSTUDIO_DAF_UNIT_EDITOR
 #include "src/DafUIInternal.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <exception>
#include <memory>
#include <utility>
#include <vector>

START_NAMESPACE_DAF

#if DUSKSTUDIO_DAF_UNIT_EDITOR
// The plug-in's own editor over DAF's UIExporter, which owns its window and its
// event loop. The host places it, drives that loop and feeds it parameter values.
class ExporterEditor final : public duskstudio::builtin::DafEditor
{
public:
    ExporterEditor (duskstudio::builtin::DafEditorCallbacks hostCallbacks,
                    const uintptr_t nativeParent, const double sampleRate, void* const dsp,
                    const uint32_t width, const uint32_t height, const double scaleFactor)
        : callbacks (std::move (hostCallbacks)),
          ui (this, nativeParent, sampleRate, editParamCallback, setParamCallback,
              nullptr, nullptr, setSizeCallback, nullptr, nullptr, dsp, scaleFactor,
              DGL_NAMESPACE::Application::kTypeAuto, 0, 0xffffffff, "dusk-studio-daf-unit")
    {
        ui.setWindowSizeFromHost (width, height);
    }

    ~ExporterEditor() override { ui.quit(); }

    bool setOffset (const int x, const int y) noexcept override
    {
        return ui.setWindowEmbeddedOffset (x, y);
    }

    void setSize (const uint32_t width, const uint32_t height) override
    {
        ui.setWindowSizeFromHost (width, height);
    }

    // A graphics driver that fails inside the editor's own event loop throws out
    // of it. The host closes the editor instead of losing the application.
    bool idle() noexcept override
    {
        try
        {
            return ui.plugin_idle();
        }
        catch (...)
        {
            return false;
        }
    }

    void parameterChanged (const uint32_t index, const float value) override
    {
        ui.parameterChanged (index, value);
    }

    uint32_t width() const noexcept override  { return ui.getWidth(); }
    uint32_t height() const noexcept override { return ui.getHeight(); }

private:
    static void editParamCallback (void* const ptr, const uint32_t index, const bool started)
    {
        auto& self = *static_cast<ExporterEditor*> (ptr);
        if (self.callbacks.gesture)
            self.callbacks.gesture (index, started);
    }

    static void setParamCallback (void* const ptr, const uint32_t index, const float value)
    {
        auto& self = *static_cast<ExporterEditor*> (ptr);
        if (self.callbacks.parameterEdited)
            self.callbacks.parameterEdited (index, value);
    }

    static void setSizeCallback (void* const ptr, const uint width, const uint height)
    {
        auto& self = *static_cast<ExporterEditor*> (ptr);
        if (self.callbacks.sizeRequested)
            self.callbacks.sizeRequested (width, height);
    }

    duskstudio::builtin::DafEditorCallbacks callbacks;
    UIExporter ui;
};
#endif

// DafPlugin over DAF's PluginExporter, the object every DAF format wrapper
// drives. The static target builds the plug-in with no format wrapper, so this
// class is its wrapper.
class ExporterBridge final : public duskstudio::builtin::DafPlugin
{
public:
    ExporterBridge()
        : exporter (stageConstruction(), nullptr, nullptr, nullptr)
    {
        const uint32_t count = exporter.getParameterCount();
        descs.resize (count);
        for (uint32_t i = 0; i < count; ++i)
        {
            auto& d = descs[i];
            const ParameterRanges& ranges = exporter.getParameterRanges (i);
            const uint32_t hints = exporter.getParameterHints (i);
            d.symbol       = exporter.getParameterSymbol (i).buffer();
            d.name         = exporter.getParameterName (i).buffer();
            d.unit         = exporter.getParameterUnit (i).buffer();
            d.minValue     = ranges.min;
            d.maxValue     = ranges.max;
            d.defaultValue = ranges.def;
            d.isOutput     = (hints & kParameterIsOutput) != 0;
            d.isHidden     = (hints & kParameterIsHidden) != 0;
            d.isBoolean    = (hints & kParameterIsBoolean) != 0;
            d.isInteger    = (hints & kParameterIsInteger) != 0;

            const ParameterEnumerationValues& e = exporter.getParameterEnumValues (i);
            if (e.restrictedMode && e.values != nullptr)
            {
                for (uint8_t v = 0; v < e.count; ++v)
                {
                    d.enumValues.push_back (e.values[v].value);
                    d.enumLabels.push_back (e.values[v].label.buffer());
                }
            }
        }
    }

    ~ExporterBridge() override { exporter.deactivateIfNeeded(); }

    const std::vector<duskstudio::builtin::DafParamDesc>& params() const noexcept override
    {
        return descs;
    }

    int numInputs() const noexcept override  { return DAF_PLUGIN_NUM_INPUTS; }
    int numOutputs() const noexcept override { return DAF_PLUGIN_NUM_OUTPUTS; }

    void activate (double sampleRate, int maxBlockFrames) override
    {
        exporter.deactivateIfNeeded();
        exporter.setSampleRate (sampleRate, true);
        exporter.setBufferSize ((uint32_t) maxBlockFrames, true);
        exporter.activate();
    }

    void deactivate() override { exporter.deactivateIfNeeded(); }

    float getParameterValue (uint32_t index) const noexcept override
    {
        return exporter.getParameterValue (index);
    }

    void setParameterValue (uint32_t index, float value) noexcept override
    {
        exporter.setParameterValue (index, value);
    }

    void setTimePosition (const dusk::TransportPosition& position) noexcept override
    {
       #if DAF_PLUGIN_WANT_TIMEPOS
        exporter.setTimePosition (toTimePosition (position));
       #else
        (void) position;
       #endif
    }

    void run (const float* const* inputs, float* const* outputs, uint32_t frames) noexcept override
    {
       #if DAF_PLUGIN_WANT_MIDI_INPUT
        exporter.run (const_cast<const float**> (inputs), const_cast<float**> (outputs), frames,
                      nullptr, 0);
       #else
        exporter.run (const_cast<const float**> (inputs), const_cast<float**> (outputs), frames);
       #endif
    }

    int latencySamples() const noexcept override
    {
       #if DAF_PLUGIN_WANT_LATENCY
        return (int) exporter.getLatency();
       #else
        return 0;
       #endif
    }

    bool hasEditor() const noexcept override
    {
       #if DUSKSTUDIO_DAF_UNIT_EDITOR
        return DAF_PLUGIN_HAS_UI;
       #else
        return false;
       #endif
    }

    uint32_t editorWidth() const noexcept override  { return kEditorWidth; }
    uint32_t editorHeight() const noexcept override { return kEditorHeight; }

    std::unique_ptr<duskstudio::builtin::DafEditor> createEditor (
        const uintptr_t nativeParent, const uint32_t width, const uint32_t height,
        const double scaleFactor, duskstudio::builtin::DafEditorCallbacks callbacks,
        std::string& errorOut) override
    {
       #if DUSKSTUDIO_DAF_UNIT_EDITOR
        try
        {
            return std::unique_ptr<duskstudio::builtin::DafEditor> (
                new ExporterEditor (std::move (callbacks), nativeParent,
                                    exporter.getSampleRate(), exporter.getInstancePointer(),
                                    width, height, scaleFactor));
        }
        catch (const std::exception& error)
        {
            errorOut = std::string ("cannot embed (") + error.what() + ").";
        }
        catch (...)
        {
            errorOut = "cannot embed.";
        }
       #else
        (void) nativeParent; (void) width; (void) height; (void) scaleFactor;
        (void) callbacks;
        errorOut = "this build was made without the built-in unit editors.";
       #endif
        return nullptr;
    }

private:
   #if defined (DAF_UI_DEFAULT_WIDTH) && defined (DAF_UI_DEFAULT_HEIGHT)
    static constexpr uint32_t kEditorWidth  = DAF_UI_DEFAULT_WIDTH;
    static constexpr uint32_t kEditorHeight = DAF_UI_DEFAULT_HEIGHT;
   #else
    static constexpr uint32_t kEditorWidth  = 0;
    static constexpr uint32_t kEditorHeight = 0;
   #endif

    // PluginExporter's constructor creates the plug-in, which reads the rate and
    // block size DAF stages in these globals. activate() replaces both.
    static void* stageConstruction() noexcept
    {
        d_nextBufferSize = 512;
        d_nextSampleRate = 48000.0;
        return nullptr;
    }

   #if DAF_PLUGIN_WANT_TIMEPOS
    static constexpr double kTicksPerBeat = 1920.0;

    static TimePosition toTimePosition (const dusk::TransportPosition& position) noexcept
    {
        TimePosition tp;
        tp.playing = position.isPlaying || position.isRecording;
        tp.frame   = position.timeInSamples > 0 ? (uint64_t) position.timeInSamples : 0;

        const double beatsPerBar = (double) position.timeSignatureNumerator;
        const double beatType    = (double) position.timeSignatureDenominator;
        if (position.bpm > 0.0 && beatsPerBar > 0.0 && beatType > 0.0)
        {
            // ppq counts quarter notes, and a DAF beat is one 1/beatType note.
            const double beats = std::max (0.0, position.ppqPosition) * beatType / 4.0;
            const double bar   = std::floor (beats / beatsPerBar);
            const double inBar = beats - bar * beatsPerBar;
            const double beat  = std::floor (inBar);

            tp.bbt.valid          = true;
            tp.bbt.bar            = (int32_t) bar + 1;
            tp.bbt.beat           = (int32_t) beat + 1;
            tp.bbt.tick           = (inBar - beat) * kTicksPerBeat;
            tp.bbt.barStartTick   = bar * beatsPerBar * kTicksPerBeat;
            tp.bbt.beatsPerBar    = (float) beatsPerBar;
            tp.bbt.beatType       = (float) beatType;
            tp.bbt.ticksPerBeat   = kTicksPerBeat;
            tp.bbt.beatsPerMinute = position.bpm;
        }
        return tp;
    }
   #endif

    PluginExporter exporter;
    std::vector<duskstudio::builtin::DafParamDesc> descs;
};

END_NAMESPACE_DAF
