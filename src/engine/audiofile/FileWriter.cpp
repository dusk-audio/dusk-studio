#include "FileWriter.h"

#include <sndfile.h>

namespace dusk::audio
{
namespace
{
int majorFormat (WriteSpec::Format f) noexcept
{
    switch (f)
    {
        case WriteSpec::Format::Flac: return SF_FORMAT_FLAC;
        case WriteSpec::Format::Aiff: return SF_FORMAT_AIFF;
        case WriteSpec::Format::Wav:
        default:                      return SF_FORMAT_WAV;
    }
}

int subFormat (int bits) noexcept
{
    switch (bits)
    {
        case 16: return SF_FORMAT_PCM_16;
        case 24: return SF_FORMAT_PCM_24;
        case 32: return SF_FORMAT_FLOAT;
        default: return 0;   // unsupported depth: no subtype, so sf_format_check rejects it
    }
}
} // namespace

std::unique_ptr<FileWriter> FileWriter::create (const std::filesystem::path& path, const WriteSpec& spec)
{
    SF_INFO si {};
    si.samplerate = (int) spec.sampleRate;
    si.channels   = spec.numChannels;
    si.format     = majorFormat (spec.format) | subFormat (spec.bitsPerSample);

    // FLAC has no 24-bit-float notion; clamp an accidental 32-bit request to PCM_24.
    if (spec.format == WriteSpec::Format::Flac && spec.bitsPerSample == 32)
        si.format = SF_FORMAT_FLAC | SF_FORMAT_PCM_24;

    if (sf_format_check (&si) == 0)
        return nullptr;

    SNDFILE* h = sf_open (path.string().c_str(), SFM_WRITE, &si);
    if (h == nullptr)
        return nullptr;

    return std::unique_ptr<FileWriter> (new FileWriter (h, spec.numChannels));
}

FileWriter::FileWriter (void* handle, int ch)
    : sf (handle), channels (ch) {}

FileWriter::~FileWriter()
{
    if (sf != nullptr)
        sf_close (static_cast<SNDFILE*> (sf));
}

bool FileWriter::write (const float* const* src, int numCh, int64_t numFrames)
{
    if (numFrames <= 0)
        return true;

    const size_t need = (size_t) numFrames * (size_t) channels;
    if (interleaved.size() < need)
        interleaved.resize (need);

    for (int64_t f = 0; f < numFrames; ++f)
        for (int c = 0; c < channels; ++c)
            interleaved[(size_t) f * (size_t) channels + (size_t) c]
                = (c < numCh) ? src[c][f] : 0.0f;

    return writeInterleaved (interleaved.data(), numFrames);
}

bool FileWriter::writeInterleaved (const float* data, int64_t numFrames) noexcept
{
    auto* h = static_cast<SNDFILE*> (sf);
    return sf_writef_float (h, data, (sf_count_t) numFrames) == (sf_count_t) numFrames;
}

bool FileWriter::flush()
{
    sf_write_sync (static_cast<SNDFILE*> (sf));
    return true;
}
} // namespace dusk::audio
