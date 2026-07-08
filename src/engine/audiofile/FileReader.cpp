#include "FileReader.h"

#include <algorithm>
#include <sndfile.h>

namespace dusk::audio
{
namespace
{
int bitsFromSubtype (int format) noexcept
{
    switch (format & SF_FORMAT_SUBMASK)
    {
        case SF_FORMAT_PCM_S8:
        case SF_FORMAT_PCM_U8: return 8;
        case SF_FORMAT_PCM_16: return 16;
        case SF_FORMAT_PCM_24: return 24;
        case SF_FORMAT_PCM_32:
        case SF_FORMAT_FLOAT:  return 32;
        case SF_FORMAT_DOUBLE: return 64;
        default:               return 0;
    }
}
} // namespace

std::unique_ptr<FileReader> FileReader::open (const std::filesystem::path& path)
{
    SF_INFO si {};
    SNDFILE* h = sf_open (path.string().c_str(), SFM_READ, &si);
    if (h == nullptr)
        return nullptr;

    FileInfo info;
    info.sampleRate    = (double) si.samplerate;
    info.numChannels   = si.channels;
    info.numFrames     = (int64_t) si.frames;
    info.bitsPerSample = bitsFromSubtype (si.format);

    return std::unique_ptr<FileReader> (new FileReader (h, info));
}

FileReader::FileReader (void* handle, const FileInfo& info)
    : sf (handle), fileInfo (info) {}

FileReader::~FileReader()
{
    if (sf != nullptr)
        sf_close (static_cast<SNDFILE*> (sf));
}

int64_t FileReader::read (float* const* dest, int numDestCh, int64_t startFrame, int64_t numFrames)
{
    if (numFrames <= 0 || numDestCh <= 0)
        return 0;

    auto* h = static_cast<SNDFILE*> (sf);
    const int fileCh = fileInfo.numChannels;

    if (startFrame != position)
    {
        if (sf_seek (h, (sf_count_t) startFrame, SEEK_SET) < 0)
            return 0;
        position = startFrame;
    }

    const size_t need = (size_t) numFrames * (size_t) fileCh;
    if (interleaved.size() < need)
        interleaved.resize (need);

    const sf_count_t got = sf_readf_float (h, interleaved.data(), (sf_count_t) numFrames);
    position += got;

    const int copyCh = std::min (numDestCh, fileCh);
    for (int c = 0; c < numDestCh; ++c)
    {
        if (c < copyCh)
            for (sf_count_t f = 0; f < got; ++f)
                dest[c][f] = interleaved[(size_t) f * (size_t) fileCh + (size_t) c];
        else
            std::fill (dest[c], dest[c] + got, 0.0f);
    }

    return (int64_t) got;
}
} // namespace dusk::audio
