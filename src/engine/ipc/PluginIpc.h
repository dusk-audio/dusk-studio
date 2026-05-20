#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

// IPC primitives shared between Focal (the parent / host process) and
// dusk-studio-plugin-host (the child process running a JUCE AudioPluginInstance
// out-of-process). Keep this header dependency-free - no JUCE includes,
// no STL containers - so both binaries can include it without dragging
// in their respective build flags.
//
// Threading model: the parent's audio thread and the child's worker thread
// communicate exclusively through the shared-memory layout below + Linux
// futex syscalls. No mutexes, no condition variables, no allocations on
// the hot path.

namespace duskstudio::ipc
{

constexpr std::uint32_t kMagic     = 0x46434C30;  // 'FCL0'
constexpr std::uint32_t kVersion   = 1;
constexpr int           kMaxBlock  = 1024;        // upper bound on numSamples per block
constexpr int           kMaxChans  = 2;           // stereo plenty for v1
constexpr std::size_t   kMidiBytes = 16 * 1024;   // serialised juce::MidiBuffer cap
constexpr std::size_t   kStateBytes = 4 * 1024 * 1024; // up to 4 MB plugin state blob

// State values for `BlockHeader::state`.
constexpr std::uint32_t kStateReady    = 0;
constexpr std::uint32_t kStateCrashed  = 1;
constexpr std::uint32_t kStateTeardown = 2;

// --- Control-plane OpCodes ------------------------------------------------
// Sent over the parent/child Unix-domain socket as length-prefixed binary
// records. Each record on the wire is:
//
//     [uint32 totalLen]   little-endian, includes the rest of the record
//     [uint32 op]         OpCode value
//     [uint32 status]     reply only - 0 = ok, non-zero = error code
//     [uint32 payloadLen] length of the variable-size payload that follows
//     [bytes  payload]    op-specific
//
// totalLen = 12 + payloadLen. Both sides drain one record at a time;
// nothing is interleaved with the audio hot path (which lives entirely
// in the SHM region). Anything > a few KB (state blobs) goes through the
// SHM staging area at kStateOffset to avoid pressure on the socket
// buffer; the payload then carries the byte count and the staging region
// holds the bytes.
enum class OpCode : std::uint32_t
{
    Ping             = 1,   // payload: empty.                 reply: empty.
    LoadPlugin       = 2,   // payload: PluginDescription XML + sr (double) + bs (int).
                            //          reply: numIn / numOut / latency.
    PrepareToPlay    = 3,   // payload: sr (double) + bs (int).
    Release          = 4,   // payload: empty.
    GetState         = 5,   // payload: empty. reply: state size; bytes are at kStateOffset.
    SetState         = 6,   // payload: state size; bytes at kStateOffset.
    SetParam         = 7,   // payload: paramIndex (uint32) + value (float).
    ShowEditor       = 8,   // Phase 3.
    HideEditor       = 9,   // Phase 3.
    ResizeEditor     = 10,  // Phase 3.
};

// Wire-record header. Followed on the wire by `payloadLen` bytes.
struct ControlMsgHeader
{
    std::uint32_t totalLen;     // 12 + payloadLen
    std::uint32_t op;           // cast from OpCode
    std::uint32_t status;       // 0 = ok on replies; ignored on requests
    std::uint32_t payloadLen;
};

// Fixed-size payloads for the simple ops. Variable ones (LoadPlugin XML)
// are packed by hand at the call site - no need for a struct.
struct LoadPluginReply
{
    std::int32_t  numInChans;
    std::int32_t  numOutChans;
    std::int32_t  latencySamples;
    std::uint32_t reserved;
};

struct PrepareToPlayPayload
{
    double sampleRate;
    std::int32_t blockSize;
    std::uint32_t reserved;
};

// ShowEditor reply. windowId is a host-OS native window handle (X11 Window
// on Linux, packed into 64 bits so the wire format doesn't depend on the
// X11 unsigned-long width). Width/height are the editor's preferred size
// reported via getWidth()/getHeight() after createEditorIfNeeded.
struct ShowEditorReply
{
    std::uint64_t windowId;
    std::int32_t  width;
    std::int32_t  height;
    std::uint32_t reserved;
};

// ResizeEditor request payload. Sent parent→child when the user resizes
// the embedding window; the child resizes its native editor wrapper to
// match so the plugin's GUI gets a JUCE resized() callback.
struct ResizeEditorPayload
{
    std::int32_t  width;
    std::int32_t  height;
    std::uint64_t reserved;
};

// Cache-line aligned so the parent's `cmdSeq` write doesn't false-share
// with the child's `replySeq` write. 64 bytes covers all live fields with
// room to grow without breaking the layout.
struct alignas (64) BlockHeader
{
    // Bumped by the parent immediately before signalling the child.
    // Read by the child to detect "new block to process".
    std::atomic<std::uint32_t> cmdSeq;

    // Bumped by the child immediately before signalling the parent.
    // Read by the parent (post-spin / post-futex-wait) to confirm the
    // round-trip completed.
    std::atomic<std::uint32_t> replySeq;

    // Set by the child when the JUCE plugin throws or otherwise marks
    // itself dead. Sticky - the parent observes this and switches to
    // bypass without waiting on the futex.
    std::atomic<std::uint32_t> state;

    // Per-block parameters set by the parent before signalling.
    std::uint32_t numSamples;
    std::uint32_t numInChans;
    std::uint32_t numOutChans;
    std::uint32_t midiInBytes;

    // Set by the child as the reply.
    std::uint32_t midiOutBytes;

    // Transport / play-head info for plugins that need it.
    double  bpm;
    std::int64_t timeInSamples;
    std::uint64_t hostFrameCounter;
    std::uint32_t flags;            // bit 0 = isPlaying, bit 1 = isLooping

    // Sanity check. Set once at SHM init by the parent, read by the
    // child to refuse mismatched layouts.
    std::uint64_t magic;
    std::uint32_t version;
    std::uint32_t reservedTail;
};

// Layout offsets within the shared mmap region. Computed at compile time
// so both processes see identical addresses.
constexpr std::size_t kHeaderOffset   = 0;
constexpr std::size_t kHeaderSize     = 256;
constexpr std::size_t kAudioInOffset  = kHeaderSize;
constexpr std::size_t kAudioInSize    = kMaxChans * kMaxBlock * sizeof (float);
constexpr std::size_t kAudioOutOffset = kAudioInOffset + kAudioInSize;
constexpr std::size_t kAudioOutSize   = kMaxChans * kMaxBlock * sizeof (float);
constexpr std::size_t kMidiInOffset   = kAudioOutOffset + kAudioOutSize;
constexpr std::size_t kMidiOutOffset  = kMidiInOffset   + kMidiBytes;
constexpr std::size_t kStateOffset    = kMidiOutOffset  + kMidiBytes;
constexpr std::size_t kTotalSize      = kStateOffset    + kStateBytes;

// Static checks - any change to BlockHeader fields needs a layout
// revisit before kVersion bumps.
static_assert (sizeof (BlockHeader) <= kHeaderSize,
               "BlockHeader must fit in kHeaderSize");
static_assert (alignof (BlockHeader) <= kHeaderSize,
               "BlockHeader alignment exceeds reserved header");

inline BlockHeader* headerOf (void* shm) noexcept
{
    return static_cast<BlockHeader*> (shm);
}

inline float* audioInChannel (void* shm, int chan) noexcept
{
    auto* base = static_cast<char*> (shm) + kAudioInOffset;
    return reinterpret_cast<float*> (base + chan * kMaxBlock * sizeof (float));
}

inline float* audioOutChannel (void* shm, int chan) noexcept
{
    auto* base = static_cast<char*> (shm) + kAudioOutOffset;
    return reinterpret_cast<float*> (base + chan * kMaxBlock * sizeof (float));
}

inline std::uint8_t* midiIn (void* shm) noexcept
{
    return reinterpret_cast<std::uint8_t*> (static_cast<char*> (shm) + kMidiInOffset);
}

inline std::uint8_t* midiOut (void* shm) noexcept
{
    return reinterpret_cast<std::uint8_t*> (static_cast<char*> (shm) + kMidiOutOffset);
}

// --- Futex helpers --------------------------------------------------------
// Tiny wrappers around the Linux SYS_futex syscall. NOTE: the PRIVATE
// variants are process-local (futex address keyed by mm_struct), so they
// do NOT cross the parent/child fence even when both processes mmap the
// same memfd. Use the non-private FUTEX_WAIT / FUTEX_WAKE for our shared
// header words.

inline long futexWaitOnce (std::atomic<std::uint32_t>* addr,
                            std::uint32_t expected,
                            const struct timespec* absTimeout) noexcept
{
    // FUTEX_WAIT_BITSET with FUTEX_BITSET_MATCH_ANY = wake on any wake-call,
    // and the timespec is interpreted as ABSOLUTE CLOCK_MONOTONIC time.
    // Non-private so the address is hashed by physical page (works across
    // a shared memfd between two processes).
    return syscall (SYS_futex, addr,
                    FUTEX_WAIT_BITSET,
                    expected, absTimeout, nullptr,
                    FUTEX_BITSET_MATCH_ANY);
}

inline long futexWakeOne (std::atomic<std::uint32_t>* addr) noexcept
{
    return syscall (SYS_futex, addr,
                    FUTEX_WAKE, 1,
                    nullptr, nullptr, 0);
}

// Compute an absolute CLOCK_MONOTONIC timeout `nsFromNow` nanoseconds in
// the future. Used by the audio thread to bound how long it'll wait on
// the plugin process before falling back to bypass.
inline struct timespec absTimeoutFromNow (long long nsFromNow) noexcept
{
    struct timespec now {};
    clock_gettime (CLOCK_MONOTONIC, &now);
    long long total = (long long) now.tv_nsec + nsFromNow;
    long long secs  = total / 1000000000LL;
    long long rem   = total % 1000000000LL;
    return { now.tv_sec + (time_t) secs, rem };
}

} // namespace duskstudio::ipc
