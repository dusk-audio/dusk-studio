#pragma once

#include "CompStageAlign.h"
#include "../session/CompParams.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace duskstudio
{
namespace builtin { class DafPlugin; }

// One compressor of the mixer: Multi-Comp 2 run in process as one of Dusk's own
// DAF plug-ins, in the mode its kind plays. The session's CompParams hold the
// plug-in's parameters and the audio thread pushes whatever changed into it at
// the top of each block, so the session stays the one owner of every value an
// editor shows.
//
// prepare allocates and must run with the audio thread fenced; pushParameters,
// process, reset and gainReductionDb are audio-thread and lock-free.
class MultiCompStage final : public CompStageProcessor
{
public:
    explicit MultiCompStage (CompStage kind);
    ~MultiCompStage() override;

    // channels is the layout the plug-in runs: 1 (mono) or 2 (stereo). The
    // plug-in keeps its own Oversampling, 2x, whatever the engine's factor:
    // its reference parity was established there. Latency is resolved here, so
    // latencySamples() is correct once this returns.
    void prepare (double sampleRate, int blockSize, int channels);

    // The delay the stage adds while it runs, in every mode.
    int latencySamples() const noexcept;

    // The mode is stripMode clamped to 0..2 on a Strip and the kind's own on
    // the others; the plug-in's bypass is !enabled.
    void pushParameters (const CompParams& comp, bool enabled, int stripMode) noexcept;

    // In place. R is null on a mono layout and required on a stereo one.
    void process (float* L, float* R, int numSamples) noexcept override;

    // Back to the rest the plug-in's activation leaves, every delay, filter
    // and envelope included, keeping its parameters; its next block runs as
    // the first one after prepare. Does nothing when it has not run since.
    void reset() noexcept override;

    // The plug-in's GR output, in dB at or below 0.
    float gainReductionDb() const noexcept;

    // One Multiband band's GR output (band 0..kCompBandCount - 1), in dB at or
    // below 0; 0 on the other kinds and for a band out of range. The plug-in
    // publishes it through an atomic, so any thread may read it. This is the
    // last value it published: a stage paused without a reset keeps it, and a
    // reset, such as the one CompStageAlign makes once a fade out ends, clears
    // it to 0.
    float bandGainReductionDb (int band) const noexcept;

    // The plug-in, for building its editor. Message thread.
    builtin::DafPlugin& plugin() noexcept;

    // What Multi-Comp 2 prints for a value, in the host domain, of one of the
    // kind's parameters: the plug-in's own text where it has some, otherwise
    // its enumeration label, otherwise the value with the unit it declares.
    // Empty for a parameter the kind does not hold. Message thread only, so
    // the audio thread never formats text; the formatter is a pure function of
    // the value, which is why the stage's playing instance can answer.
    std::string valueText (CompParamId id, float value) const;

    // A stepped parameter's steps as valueText prints them, in order, between
    // separators; its two ends for a continuous one. Message thread only.
    std::string stepsText (CompParamId id, const char* separator) const;

    // Text typed for one of the kind's parameters back to a host value: as the
    // plug-in reads it where it reads text of its own, otherwise a label or a
    // number with or without the unit. False, and value untouched, when the
    // text does not read. Message thread only.
    bool valueFromText (CompParamId id, const std::string& text, float& value) const;

    CompStage kind() const noexcept;

    // The first symbol among the kind's parameters, mode, bypass and GR (and
    // the Multiband's band GRs) that the plug-in lacks, or null when it has
    // them all.
    const char* unresolvedSymbol() const noexcept;

    // The session value behind each of the plug-in's parameter indices, which
    // is what its editor reads and writes. The bypass parameter is the engage
    // switch inverted. The mode parameter is the strip's compMode on a Strip
    // (a write outside 0..2 is ignored) and the kind's fixed mode elsewhere,
    // which writes leave alone. An output, and a parameter outside the kind's
    // list, reads the plug-in and ignores writes. Message thread.
    float sessionValue (const CompParams& comp, bool engaged, int stripMode,
                        int index) const noexcept;
    void  setSessionValue (CompParams& comp, std::atomic<bool>& engaged,
                           std::atomic<int>* stripMode, int index, float value) const noexcept;
    bool  isEngageParam (int index) const noexcept;
    bool  isModeParam (int index) const noexcept;

    // The session value a plug-in index stands for when it is one of the
    // kind's parameters; empty for the mode, the bypass, an output and an
    // unbound parameter.
    std::optional<CompParamId> sessionParam (int index) const noexcept;

    // Multi-Comp 2's editor reads some values back from the plug-in rather
    // than from what its host hands it: all of them when it opens, the three
    // crossovers every frame. While one is open the owner pushes the session's
    // values on every block it processes, so a stage that is out keeps them
    // current without running. That changes nothing it plays: a plug-in that
    // is out sits at rest, is not reset again before it next runs, and stores
    // a pushed value until then. setEditorOpen counts opens and closes;
    // message thread.
    void setEditorOpen (bool open) noexcept;
    bool editorOpen() const noexcept { return editorsOpen.load (std::memory_order_relaxed) > 0; }

    // Message thread, as an editor is built: the session's values straight
    // into the plug-in, which a strip or bus that is silent may not have run a
    // block to push for a long while, and the plug-in's BYPASS set to show
    // `engaged`, which a push never does. The plug-in's parameters are atomics
    // on both sides, so a push the audio thread makes meanwhile races nothing;
    // the stage's record of what it pushed is the audio thread's alone, so the
    // seed leaves it and has the next push send everything again. The plug-in
    // gets its own bypass back from the audio thread before it next runs or
    // is reset, and at the next push while it runs, so what it plays is
    // unchanged.
    void seedEditor (const CompParams& comp, bool engaged, int stripMode) noexcept;

    // What the editor reads back from the plug-in for one of its indices.
    float editorReading (int index) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::atomic<int> editorsOpen { 0 };
    std::atomic<std::uint32_t> editorSeeds { 0 };
};
} // namespace duskstudio
