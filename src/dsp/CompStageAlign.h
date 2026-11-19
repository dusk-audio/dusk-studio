#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace duskstudio
{
// The compressor a member runs: Multi-Comp 2 behind MultiCompStage. Its output
// is its input delayed by the stage's latency, and reset() puts it back to the
// rest a fresh prepare leaves. Both are audio-thread calls.
class CompStageProcessor
{
public:
    virtual ~CompStageProcessor() = default;
    // In place; R is null on a mono layout.
    virtual void process (float* L, float* R, int numSamples) noexcept = 0;
    virtual void reset() noexcept = 0;
};

// One block of a mixer stage's compressor latency (the strips', the buses' or
// the master's), as every member of the stage plays it. The stage's clock
// writes it before any member runs the block; members only read it, so each of
// them delays and fades in the same samples.
struct CompStageBlock
{
    // The plug-in's latency. 0: the stage holds none and every member passes
    // its input through untouched.
    int latency = 0;
    // Members store their input in their delay this block.
    bool write = false;
    // The weight of the delayed path is alignAt / rampSamples at the block's
    // start and moves by alignDir / rampSamples a sample, held to 0..1.
    int rampSamples = 1;
    int alignAt = 0;
    int alignDir = 0;
    // One more every block the stage runs, so a member that skipped one knows.
    // A block left as it is plays as a fixed stage.
    std::uint32_t serial = 0;
    // Changes when the stage snaps to its target with no fade.
    std::uint32_t snap = 0;
    // Changes when the mixer stopped running for a while (a suspension, the
    // mastering stage): what any member held, its compressor included, is
    // dropped.
    std::uint32_t cut = 0;

    bool settledOn() const noexcept { return alignDir == 0 && alignAt == rampSamples; }
};

// A stage's latency state. It turns on in the block its first compressor is
// engaged and off only once none is engaged and the transport is stopped, so
// switching a compressor in and out while playing never moves the stage's
// timing. Off -> on stores the latency's worth of input before the delayed
// path fades in; on -> off fades back, and a transport that rolls again before
// that fade ends puts the stage straight back on. The engine owns one per
// stage and runs it on the audio thread at the top of each block.
class CompStageClock
{
public:
    // Message thread, audio fenced. Leaves the stage off; snap() then sets it.
    void prepare (double sampleRate, int latency) noexcept;
    // To on (when engaged and the stage has a latency) or off, with no fade.
    void snap (bool engaged) noexcept;
    void advance (bool anyEngaged, bool transportStopped, int numSamples) noexcept;
    // For blocks the mixer did not run: members drop what they held.
    void markGap() noexcept { ++cuts; current.cut = cuts; }

    const CompStageBlock& block() const noexcept { return current; }
    // The latency the stage holds once its fades settle. Audio thread.
    int latencySamples() const noexcept { return heading() ? latency : 0; }
    // The same, for any thread.
    int reportedLatencySamples() const noexcept { return reported.load (std::memory_order_relaxed); }

private:
    enum class State { Off, Filling, Rising, On, Falling };

    bool heading() const noexcept
    {
        return state == State::Filling || state == State::Rising || state == State::On;
    }
    void emit() noexcept;

    State state = State::Off;
    int latency = 0;
    int ramp = 1;
    int pos = 0;
    int filled = 0;
    bool wantOn = false;
    std::uint32_t serial = 0;
    std::uint32_t snaps = 0;
    std::uint32_t cuts = 0;
    CompStageBlock current;
    std::atomic<int> reported { 0 };
};

// One member of a stage: a strip, bus or master compressor, or a path that
// bypasses the buses. While the stage's latency is on, the member carries it
// whether its compressor is engaged (the plug-in's own delay) or not (a plain
// delay of its input), and crossfades between the two, time-aligned, when the
// compressor is switched. Nothing runs while the stage is off.
//
// prepare allocates; process and discard are audio-thread and lock-free.
class CompStageAlign
{
public:
    // maxLatency bounds the stage latency the member can carry.
    void prepare (int blockSize, int maxLatency);

    // In place on L (and R; null on a mono block). `compressor` is what runs
    // when `engaged`, null for a member that has none; a different one than
    // last block (the strip's width flipped) restarts from rest.
    void process (float* L, float* R, int numSamples, const CompStageBlock& stage,
                  bool engaged, CompStageProcessor* compressor) noexcept;

    // The member's output was cut (gated, muted or bailed): the audio it holds
    // must not play when it next runs.
    void discard() noexcept;

    // The compressor runs now and may be heard; its meter means something.
    bool compressorRunning() const noexcept { return running != nullptr; }
    // How long its output can still sound after its input fell silent.
    int tailSamples() const noexcept;

private:
    void stop() noexcept;
    void clearRing() noexcept;
    void push (const float* L, const float* R, int n) noexcept;
    void pushAndRead (const float* L, const float* R, float* outL, float* outR, int n, int latency) noexcept;
    void runChunk (float* L, float* R, int n, int offset, const CompStageBlock& stage) noexcept;

    std::vector<float> ringL, ringR;
    int mask = 0;
    int writeAt = 0;
    int maxRingLatency = 0;
    bool ringLive = false;

    std::vector<float> dryL, dryR, wetL, wetR;
    int maxBlock = 1;

    CompStageProcessor* running = nullptr;
    int runSamples = 0;
    int mixAt = 0;
    int mixDir = 0;

    int heldLatency = 0;
    std::uint32_t lastSerial = 0;
    std::uint32_t lastSnap = 0;
    std::uint32_t lastCut = 0;
    bool resync = true;
    bool dropHeld = false;
};
} // namespace duskstudio
