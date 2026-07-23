#pragma once

#include <cstdint>

namespace dusk::audio
{
// Drain target for ThreadedFileWriter and WriterDrainPool, so the threaded ring
// and the pool stay format-agnostic: the libsndfile FileWriter implements it for
// WAV/FLAC/AIFF, and the libmp3lame writer for MP3. Interleaved float, matching
// the ring's storage; the disk/worker thread is the only caller.
class IFileWriteSink
{
public:
    virtual ~IFileWriteSink() = default;

    virtual int numChannels() const noexcept = 0;

    // interleaved holds numFrames * numChannels samples. Returns false on a
    // short or failed write, which latches the drain's failure flag. Must not
    // allocate.
    virtual bool writeInterleaved (const float* interleaved, std::int64_t numFrames) noexcept = 0;

    virtual bool flush() = 0;
};
} // namespace dusk::audio
