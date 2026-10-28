#include "FileWriter.h"

#include <sndfile.h>

#include <algorithm>
#include <cerrno>
#include <system_error>

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <fcntl.h>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

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

// libsndfile can discard codec/header close errors; retain native I/O failures
// across those callbacks and keep the file alive until finalization is synced.
struct FileWriter::Output
{
   #if defined (_WIN32)
    HANDLE handle = INVALID_HANDLE_VALUE;
   #else
    int descriptor = -1;
   #endif
    bool failed = false;
    int error = 0;

    ~Output() { close(); }

    void fail() noexcept
    {
        failed = true;
       #if defined (_WIN32)
        if (error == 0) error = static_cast<int> (GetLastError());
       #else
        if (error == 0) error = errno != 0 ? errno : EIO;
       #endif
    }

    bool open (const std::filesystem::path& path)
    {
       #if defined (_WIN32)
        handle = CreateFileW (path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) { fail(); return false; }
       #else
        do { descriptor = ::open (path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666); }
        while (descriptor < 0 && errno == EINTR);
        if (descriptor < 0) { fail(); return false; }
       #endif
        return true;
    }

    sf_count_t length() noexcept
    {
       #if defined (_WIN32)
        LARGE_INTEGER size {};
        if (GetFileSizeEx (handle, &size)) return size.QuadPart;
       #else
        struct stat info {};
        if (::fstat (descriptor, &info) == 0) return static_cast<sf_count_t> (info.st_size);
       #endif
        fail();
        return -1;
    }

    sf_count_t seek (sf_count_t offset, int whence) noexcept
    {
       #if defined (_WIN32)
        LARGE_INTEGER distance {}, position {};
        distance.QuadPart = offset;
        const DWORD method = whence == SEEK_SET ? FILE_BEGIN : whence == SEEK_CUR ? FILE_CURRENT : FILE_END;
        if (SetFilePointerEx (handle, distance, &position, method)) return position.QuadPart;
       #else
        const auto position = ::lseek (descriptor, static_cast<off_t> (offset), whence);
        if (position >= 0) return static_cast<sf_count_t> (position);
       #endif
        fail();
        return -1;
    }

    sf_count_t write (const void* data, sf_count_t bytes) noexcept
    {
        sf_count_t done = 0;
        while (done < bytes)
        {
            const auto chunk = std::min<sf_count_t> (bytes - done, 1 << 30);
            const auto* source = static_cast<const char*> (data) + done;
           #if defined (_WIN32)
            DWORD written = 0;
            if (! WriteFile (handle, source, static_cast<DWORD> (chunk), &written, nullptr))
            { fail(); break; }
           #else
            const auto written = ::write (descriptor, source, static_cast<std::size_t> (chunk));
            if (written < 0 && errno == EINTR) continue;
            if (written < 0) { fail(); break; }
           #endif
            if (written == 0) { failed = true; break; }
            done += static_cast<sf_count_t> (written);
        }
        return done;
    }

    bool sync() noexcept
    {
       #if defined (_WIN32)
        if (FlushFileBuffers (handle)) return true;
       #else
        int result;
        do { result = ::fsync (descriptor); } while (result < 0 && errno == EINTR);
        if (result == 0) return true;
       #endif
        fail();
        return false;
    }

    bool close() noexcept
    {
       #if defined (_WIN32)
        if (handle == INVALID_HANDLE_VALUE) return true;
        const bool okay = CloseHandle (handle) != 0;
        handle = INVALID_HANDLE_VALUE;
       #else
        if (descriptor < 0) return true;
        const bool okay = ::close (descriptor) == 0;
        descriptor = -1;
       #endif
        if (! okay) fail();
        return okay;
    }
};

std::unique_ptr<FileWriter> FileWriter::create (const std::filesystem::path& path, const WriteSpec& spec,
                                                std::string* whyNot)
{
    SF_INFO si {};
    si.samplerate = (int) spec.sampleRate;
    si.channels   = spec.numChannels;
    si.format     = majorFormat (spec.format) | subFormat (spec.bitsPerSample);

    // FLAC has no 24-bit-float notion; clamp an accidental 32-bit request to PCM_24.
    if (spec.format == WriteSpec::Format::Flac && spec.bitsPerSample == 32)
        si.format = SF_FORMAT_FLAC | SF_FORMAT_PCM_24;

    if (sf_format_check (&si) == 0)
    {
        if (whyNot != nullptr) *whyNot = "unsupported sample format";
        return nullptr;
    }

    auto output = std::make_unique<Output>();
    if (! output->open (path))
    {
        if (whyNot != nullptr)
           #if defined (_WIN32)
            *whyNot = std::system_category().message (output->error);
           #else
            *whyNot = std::generic_category().message (output->error);
           #endif
        return nullptr;
    }

    auto writer = std::unique_ptr<FileWriter> (new FileWriter (std::move (output), spec.numChannels));
    SF_VIRTUAL_IO io {};
    io.get_filelen = [] (void* context) { return static_cast<Output*> (context)->length(); };
    io.seek = [] (sf_count_t offset, int whence, void* context)
    { return static_cast<Output*> (context)->seek (offset, whence); };
    io.write = [] (const void* data, sf_count_t bytes, void* context)
    { return static_cast<Output*> (context)->write (data, bytes); };
    io.tell = [] (void* context) { return static_cast<Output*> (context)->seek (0, SEEK_CUR); };
    errno = 0;
    writer->sf = sf_open_virtual (&io, SFM_WRITE, &si, writer->output.get());
    if (writer->sf == nullptr)
    {
        const int openErrno = errno;
        if (whyNot != nullptr)
            *whyNot = openErrno != 0 ? std::generic_category().message (openErrno)
                                     : std::string (sf_strerror (nullptr));
        return nullptr;
    }
    return writer;
}

FileWriter::FileWriter (std::unique_ptr<Output> file, int ch)
    : output (std::move (file)), channels (ch) {}

FileWriter::~FileWriter()
{
    flush();
}

bool FileWriter::write (const float* const* src, int numCh, int64_t numFrames)
{
    if (numFrames <= 0)
        return true;
    if (sf == nullptr || output->failed) return false;

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
    if (numFrames <= 0) return true;
    if (sf == nullptr || output->failed) return false;
    auto* h = static_cast<SNDFILE*> (sf);
    return sf_writef_float (h, data, (sf_count_t) numFrames) == (sf_count_t) numFrames
        && ! output->failed;
}

bool FileWriter::flush()
{
    if (finalized) return flushOkay;
    finalized = true;
    bool okay = sf != nullptr;
    if (sf != nullptr)
    {
        auto* handle = static_cast<SNDFILE*> (sf);
        okay = sf_error (handle) == SF_ERR_NO_ERROR;
        if (sf_close (handle) != 0) okay = false;
        sf = nullptr;
    }
    const bool synced = output->sync();
    const bool closed = output->close();
    flushOkay = okay && ! output->failed && synced && closed;
    return flushOkay;
}
} // namespace dusk::audio
