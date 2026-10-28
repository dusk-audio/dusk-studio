#include "LameMp3Writer.h"

#if DUSKSTUDIO_HAS_LAME
 #include <lame/lame.h>
#endif

#include <algorithm>
#include <cerrno>

#if defined (_WIN32)
 #include <io.h>
#else
 #include <unistd.h>
#endif

namespace duskstudio
{
namespace
{
// LAME's documented worst case for a block: 1.25 * frames + 7200 bytes.
int mp3BufferCapacity (int numFrames) noexcept
{
    return (int) (1.25 * (double) numFrames) + 7200;
}

// Deinterleave + encode this many frames per LAME call, so writeInterleaved
// stays allocation-free for any ring-chunk size the drain hands it.
constexpr int kEncodeChunkFrames = 1 << 14;

std::FILE* openOutputFile (const std::filesystem::path& path) noexcept
{
#if defined (_WIN32)
    std::FILE* output = nullptr;
    return ::_wfopen_s (&output, path.c_str(), L"wb") == 0 ? output : nullptr;
#else
    return std::fopen (path.c_str(), "wb");
#endif
}
} // namespace

LameMp3Writer::LameMp3Writer (const std::filesystem::path& path, double sampleRate,
                               int numChannels, int bitrateKbps)
    : channels (std::clamp (numChannels, 1, 2)),
      bitrate (bitrateKbps)
{
    file = openOutputFile (path);
    if (file == nullptr)
        return;
#if DUSKSTUDIO_HAS_LAME
    auto* g = lame_init();
    if (g == nullptr) return;
    lame_set_in_samplerate  (g, (int) sampleRate);
    lame_set_out_samplerate (g, (int) sampleRate);
    lame_set_num_channels   (g, channels);
    lame_set_mode           (g, channels >= 2 ? JOINT_STEREO : MONO);
    lame_set_brate          (g, bitrate);
    lame_set_quality        (g, 2);   // 0 = best/slowest .. 9 = worst; 2 is high
    // Emit the Info (CBR Xing) placeholder frame; flush() rewrites it with the
    // real frame/byte counts so players report duration and seek correctly.
    lame_set_bWriteVbrTag   (g, 1);
    if (lame_init_params (g) < 0) { lame_close (g); return; }
    encoder = g;
    mp3buf.resize ((size_t) mp3BufferCapacity (kEncodeChunkFrames));
    scratchL.resize ((size_t) kEncodeChunkFrames);
    scratchR.resize ((size_t) kEncodeChunkFrames);
#else
    (void) sampleRate;
#endif
}

LameMp3Writer::~LameMp3Writer()
{
    flush();   // best-effort; failure is unreportable from a destructor
#if DUSKSTUDIO_HAS_LAME
    if (encoder != nullptr)
        lame_close (static_cast<lame_global_flags*> (encoder));
#endif
    encoder = nullptr;
    if (file != nullptr)
    {
        std::fclose (file);
        file = nullptr;
    }
}

bool LameMp3Writer::flush()
{
#if DUSKSTUDIO_HAS_LAME
    if (flushed) return flushOk;
    flushed = true;
    bool okay = encoder != nullptr && file != nullptr;
    if (okay)
    {
        auto* g = static_cast<lame_global_flags*> (encoder);
        const int bytes = lame_encode_flush (g, mp3buf.data(), (int) mp3buf.size());
        if (bytes < 0 || (bytes > 0
            && std::fwrite (mp3buf.data(), 1, (size_t) bytes, file) != (size_t) bytes))
            okay = false;

        unsigned char tag[2880];
        const size_t tagSize = lame_get_lametag_frame (g, tag, sizeof (tag));
        if (tagSize == 0 || tagSize > sizeof (tag))
            okay = false;
        else
        {
            const long endPos = std::ftell (file);
            if (endPos < 0 || std::fseek (file, 0, SEEK_SET) != 0)
                okay = false;
            else
            {
                if (std::fwrite (tag, 1, tagSize, file) != tagSize) okay = false;
                if (std::fseek (file, endPos, SEEK_SET) != 0) okay = false;
            }
        }
    }
    if (file != nullptr)
    {
        if (std::ferror (file) != 0) okay = false;
        if (std::fflush (file) != 0) okay = false;
       #if defined (_WIN32)
        if (::_commit (::_fileno (file)) != 0) okay = false;
       #else
        int syncResult;
        do { syncResult = ::fsync (::fileno (file)); } while (syncResult < 0 && errno == EINTR);
        if (syncResult != 0) okay = false;
       #endif
        if (std::fclose (file) != 0) okay = false;
        file = nullptr;
    }
    flushOk = okay;
    return flushOk;
#else
    return false;
#endif
}

bool LameMp3Writer::writeInterleaved (const float* interleaved, std::int64_t numFrames) noexcept
{
#if DUSKSTUDIO_HAS_LAME
    if (encoder == nullptr || file == nullptr) return false;
    if (numFrames <= 0) return true;

    for (std::int64_t done = 0; done < numFrames; )
    {
        const int n = (int) std::min ((std::int64_t) kEncodeChunkFrames, numFrames - done);
        const float* src = interleaved + done * channels;
        if (channels >= 2)
        {
            for (int i = 0; i < n; ++i)
            {
                scratchL[(size_t) i] = src[(size_t) i * 2];
                scratchR[(size_t) i] = src[(size_t) i * 2 + 1];
            }
        }
        else
        {
            for (int i = 0; i < n; ++i)
                scratchL[(size_t) i] = scratchR[(size_t) i] = src[(size_t) i];
        }
        if (! encode (scratchL.data(), scratchR.data(), n))
            return false;
        done += n;
    }
    return true;
#else
    (void) interleaved; (void) numFrames;
    return false;
#endif
}

bool LameMp3Writer::encode (const float* left, const float* right, int numFrames) noexcept
{
#if DUSKSTUDIO_HAS_LAME
    if (numFrames <= 0) return true;
    auto* g = static_cast<lame_global_flags*> (encoder);
    const int bytes = lame_encode_buffer_ieee_float (g, left, right, numFrames,
                                                       mp3buf.data(), (int) mp3buf.size());
    if (bytes < 0) return false;
    return bytes == 0 || std::fwrite (mp3buf.data(), 1, (size_t) bytes, file) == (size_t) bytes;
#else
    (void) left; (void) right; (void) numFrames;
    return false;
#endif
}
} // namespace duskstudio
