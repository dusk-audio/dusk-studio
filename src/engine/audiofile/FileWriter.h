#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "IFileWriteSink.h"

namespace dusk::audio
{
struct WriteSpec
{
    enum class Format { Wav, Flac, Aiff };

    double  sampleRate    = 44100.0;
    int     numChannels   = 2;
    int     bitsPerSample = 24;   // 16 / 24 -> integer PCM; 32 -> 32-bit float
    Format  format        = Format::Wav;
};

// Sound-file writer backed by libsndfile. Container + sample format come from
// WriteSpec. MP3 is deliberately absent: libsndfile MP3 support is build
// dependent, so MP3 stays on the dedicated libmp3lame path.
class FileWriter final : public IFileWriteSink
{
public:
    static std::unique_ptr<FileWriter> create (const std::filesystem::path& path, const WriteSpec& spec);
    ~FileWriter() override;

    FileWriter (const FileWriter&)            = delete;
    FileWriter& operator= (const FileWriter&) = delete;

    int numChannels() const noexcept override { return channels; }

    // Deinterleaved. src carries numCh channel pointers of >= numFrames.
    bool write (const float* const* src, int numCh, int64_t numFrames);

    // Interleaved, allocation-free: the real-time drain path uses this so the
    // disk thread never touches the heap mid-record.
    bool writeInterleaved (const float* interleaved, int64_t numFrames) noexcept override;

    bool flush() override;

private:
    FileWriter (void* handle, int channels);

    void*              sf;           // SNDFILE*
    int                channels;
    std::vector<float> interleaved;  // scratch for the deinterleaved entry point
};
} // namespace dusk::audio
