#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace dusk::audio
{
struct FileInfo
{
    double  sampleRate    = 0.0;
    int     numChannels   = 0;
    int64_t numFrames     = 0;
    int     bitsPerSample = 0;
};

// Random-access sound-file reader backed by libsndfile. Format is sniffed on
// open (WAV/AIFF/FLAC and anything else the linked libsndfile handles). Reads
// are deinterleaved float, which is the shape the rest of the engine wants.
class FileReader
{
public:
    static std::unique_ptr<FileReader> open (const std::filesystem::path& path);
    ~FileReader();

    FileReader (const FileReader&)            = delete;
    FileReader& operator= (const FileReader&) = delete;

    const FileInfo& info() const noexcept { return fileInfo; }

    // dest carries numDestCh channel pointers, each with room for numFrames.
    // Destination channels past the file's channel count are zero-filled; file
    // channels past numDestCh are dropped. Returns frames actually produced
    // (fewer than requested at end of file).
    int64_t read (float* const* dest, int numDestCh, int64_t startFrame, int64_t numFrames);

private:
    FileReader (void* handle, const FileInfo& info);

    void*              sf;            // SNDFILE*
    FileInfo           fileInfo;
    std::vector<float> interleaved;   // scratch, grown to numFrames * numChannels
    int64_t            position = 0;  // current sf frame cursor; skips no-op seeks
};
} // namespace dusk::audio
