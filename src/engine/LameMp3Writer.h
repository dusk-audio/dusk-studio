#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>

namespace duskstudio
{
// A juce::AudioFormatWriter backed by libmp3lame (constant bitrate). Owns the
// OutputStream like every JUCE writer (the base destructor deletes it). When the
// build has no LAME (DUSKSTUDIO_HAS_LAME unset), construction yields
// isOk() == false and the caller errors out / falls back to WAV — the class
// always compiles so call sites don't need their own #if guards.
class LameMp3Writer final : public juce::AudioFormatWriter
{
public:
    // Takes ownership of destStream. bitrateKbps is the CBR target (e.g. 320).
    LameMp3Writer (juce::OutputStream* destStream, double sampleRate,
                   unsigned int numChannels, int bitrateKbps);
    ~LameMp3Writer() override;

    bool isOk() const noexcept { return encoder != nullptr; }

    // Flush the encoder's buffered frames to the stream and report whether the
    // write succeeded. The destructor also flushes, but it can't report a
    // failure — call this first when the caller needs to distinguish a
    // truncated file (disk full at flush) from a good one. Idempotent.
    bool finalize();

    // Only write(int**) is virtual in juce::AudioFormatWriter; the float-array
    // path (writeFromFloatArrays) is non-virtual and routes here after JUCE's
    // float->32-bit-int conversion, which we convert back to float for LAME.
    bool write (const int** samplesToWrite, int numSamples) override;

private:
    bool encode (const float* left, const float* right, int numSamples);

    void* encoder = nullptr;            // lame_global_flags* (opaque here so
                                        // lame.h stays confined to the .cpp)
    int   bitrate = 320;
    std::vector<unsigned char> mp3buf;
    std::vector<float>         scratchL, scratchR;
    bool  flushed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LameMp3Writer)
};
} // namespace duskstudio
