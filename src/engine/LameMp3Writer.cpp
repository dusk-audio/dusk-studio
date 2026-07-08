#include "LameMp3Writer.h"

#if DUSKSTUDIO_HAS_LAME
 #include <lame/lame.h>
#endif

namespace duskstudio
{
namespace
{
constexpr float kInt32ToFloat = 1.0f / 2147483648.0f;   // 32-bit int -> [-1, 1)

int mp3BufferCapacity (int numSamples) noexcept
{
    // LAME's documented worst case for a block: 1.25 * samples + 7200 bytes.
    return (int) (1.25 * (double) numSamples) + 7200;
}
} // namespace

LameMp3Writer::LameMp3Writer (juce::OutputStream* destStream, double sampleRate,
                               unsigned int numChannels, int bitrateKbps)
    : juce::AudioFormatWriter (destStream, "MP3 file", sampleRate,
                                juce::jmax (1u, numChannels), 16),
      bitrate (bitrateKbps)
{
#if DUSKSTUDIO_HAS_LAME
    auto* g = lame_init();
    if (g == nullptr) return;
    lame_set_in_samplerate  (g, (int) sampleRate);
    lame_set_out_samplerate (g, (int) sampleRate);
    lame_set_num_channels   (g, (int) juce::jlimit (1u, 2u, numChannels));
    lame_set_mode           (g, numChannels >= 2 ? JOINT_STEREO : MONO);
    lame_set_brate          (g, bitrate);
    lame_set_quality        (g, 2);   // 0 = best/slowest .. 9 = worst; 2 is high
    // Emit the Info (CBR Xing) placeholder frame; finalize() rewrites it with
    // the real frame/byte counts so players report duration and seek
    // correctly. Headerless CBR made some players guess the length.
    lame_set_bWriteVbrTag   (g, 1);
    if (lame_init_params (g) < 0) { lame_close (g); return; }
    encoder = g;
    mp3buf.resize ((size_t) mp3BufferCapacity (1 << 16));
#else
    juce::ignoreUnused (sampleRate, numChannels);
#endif
}

LameMp3Writer::~LameMp3Writer()
{
#if DUSKSTUDIO_HAS_LAME
    if (encoder != nullptr)
    {
        finalize();   // best-effort; failure is unreportable from a destructor
        lame_close (static_cast<lame_global_flags*> (encoder));
        encoder = nullptr;
    }
#endif
    // base ~AudioFormatWriter deletes `output` (the FileOutputStream flushes
    // its remaining bytes and closes the file).
}

bool LameMp3Writer::finalize()
{
#if DUSKSTUDIO_HAS_LAME
    if (encoder == nullptr) return false;
    if (flushed) return true;
    flushed = true;
    auto* g = static_cast<lame_global_flags*> (encoder);
    if (mp3buf.size() < 7200) mp3buf.resize (7200);
    const int bytes = lame_encode_flush (g, mp3buf.data(), (int) mp3buf.size());
    if (bytes < 0)  return false;
    if (bytes > 0 && (output == nullptr || ! output->write (mp3buf.data(), (size_t) bytes)))
        return false;

    // Rewrite the Info placeholder (first frame) with the real frame/byte
    // counts. Best-effort: a stream that can't seek keeps the placeholder,
    // which decodes as silence and merely costs the duration metadata.
    if (output != nullptr)
    {
        unsigned char tag[2880];
        const size_t tagSize = lame_get_lametag_frame (g, tag, sizeof (tag));
        if (tagSize > 0 && tagSize <= sizeof (tag))
        {
            const std::int64_t endPos = output->getPosition();
            if (output->setPosition (0))
            {
                output->write (tag, tagSize);
                output->setPosition (endPos);
            }
        }
    }
    return true;
#else
    return false;
#endif
}

bool LameMp3Writer::encode (const float* left, const float* right, int numSamples)
{
#if DUSKSTUDIO_HAS_LAME
    if (encoder == nullptr) return false;
    if (numSamples <= 0)    return true;
    const int cap = mp3BufferCapacity (numSamples);
    if ((int) mp3buf.size() < cap) mp3buf.resize ((size_t) cap);
    auto* g = static_cast<lame_global_flags*> (encoder);
    const int bytes = lame_encode_buffer_ieee_float (g, left, right, numSamples,
                                                       mp3buf.data(), (int) mp3buf.size());
    if (bytes < 0) return false;
    return bytes == 0 || (output != nullptr && output->write (mp3buf.data(), (size_t) bytes));
#else
    juce::ignoreUnused (left, right, numSamples);
    return false;
#endif
}

bool LameMp3Writer::write (const int** samplesToWrite, int numSamples)
{
    if (numSamples <= 0)
        return true;    // nothing to write - a legitimate no-op, not a failure
    if (encoder == nullptr || samplesToWrite == nullptr)
        return false;   // can't encode (consistent with the null-data check below)

    // JUCE hands writers 32-bit-left-justified ints; convert to float for LAME.
    scratchL.resize ((size_t) numSamples);
    scratchR.resize ((size_t) numSamples);
    const int* l = samplesToWrite[0];
    const int* r = (numChannels > 1 && samplesToWrite[1] != nullptr) ? samplesToWrite[1] : l;
    if (l == nullptr) return false;
    for (int i = 0; i < numSamples; ++i)
    {
        scratchL[(size_t) i] = (float) l[i] * kInt32ToFloat;
        scratchR[(size_t) i] = (float) r[i] * kInt32ToFloat;
    }
    return encode (scratchL.data(), scratchR.data(), numSamples);
}
} // namespace duskstudio
