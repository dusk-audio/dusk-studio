#pragma once

#include "audiofile/IFileWriteSink.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace duskstudio
{
// A dusk::audio::IFileWriteSink backed by libmp3lame (constant bitrate), writing
// to a stdio FILE it opens and owns. When the build has no LAME
// (DUSKSTUDIO_HAS_LAME unset), construction yields isOk() == false and the caller
// errors out / falls back to WAV - the class always compiles so call sites don't
// need their own #if guards.
class LameMp3Writer final : public dusk::audio::IFileWriteSink
{
public:
    // Opens path for writing (truncating). bitrateKbps is the CBR target (e.g. 320).
    LameMp3Writer (const std::filesystem::path& path, double sampleRate,
                   int numChannels, int bitrateKbps);
    ~LameMp3Writer() override;

    LameMp3Writer (const LameMp3Writer&)            = delete;
    LameMp3Writer& operator= (const LameMp3Writer&) = delete;

    bool isOk() const noexcept { return encoder != nullptr && file != nullptr; }

    int numChannels() const noexcept override { return channels; }

    // Interleaved float, matching the drain ring's storage. Deinterleaves in
    // fixed chunks and encodes to LAME; allocation-free (scratch pre-sized in the
    // ctor). The disk/pool thread is the only caller, never the audio thread.
    bool writeInterleaved (const float* interleaved, std::int64_t numFrames) noexcept override;

    // Flush the encoder's buffered frames and rewrite the Info (Xing) frame with
    // the real frame/byte counts so players report duration + seek. Idempotent.
    // The destructor also flushes, but it can't report a failure - call this
    // first when a truncated-file (disk full at flush) distinction matters.
    bool flush() override;

private:
    bool encode (const float* left, const float* right, int numFrames) noexcept;

    void*      encoder = nullptr;   // lame_global_flags* (opaque so lame.h stays in the .cpp)
    std::FILE* file    = nullptr;
    int        channels = 2;
    int        bitrate  = 320;
    std::vector<unsigned char> mp3buf;
    std::vector<float>         scratchL, scratchR;
    bool  flushed = false;
};
} // namespace duskstudio
