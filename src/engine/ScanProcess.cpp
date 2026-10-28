#include "ScanProcess.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <bcrypt.h>
#else
 #include <fcntl.h>
 #include <poll.h>
 #include <signal.h>
 #include <spawn.h>
 #include <sys/wait.h>
 #include <unistd.h>
 extern "C" char** environ;
#endif

namespace duskstudio
{
#if defined(_WIN32)
namespace
{
void closeHandle (HANDLE& handle) noexcept
{
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) ::CloseHandle (handle);
    handle = nullptr;
}

bool widen (const std::string& text, std::wstring& result)
{
    if (text.size() > static_cast<std::size_t> (std::numeric_limits<int>::max())) return false;
    if (text.empty()) { result.clear(); return true; }
    const auto size = static_cast<int> (text.size());
    const auto required = ::MultiByteToWideChar (CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0);
    if (required <= 0) return false;
    result.resize (static_cast<std::size_t> (required));
    return ::MultiByteToWideChar (CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, result.data(), required) == required;
}

void appendArgument (std::wstring& command, const std::wstring& argument)
{
    command.push_back (L'"');
    std::size_t backslashes = 0;
    for (const auto ch : argument)
    {
        if (ch == L'\\') { ++backslashes; continue; }
        command.append (ch == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        command.push_back (ch);
        backslashes = 0;
    }
    command.append (backslashes * 2, L'\\');
    command.push_back (L'"');
}
} // namespace

struct ScanProcess::Impl
{
    HANDLE output = nullptr, writer = nullptr, nullDevice = nullptr;
    HANDLE process = nullptr, thread = nullptr, job = nullptr;
    bool assigned = false;

    ~Impl()
    {
        stop();
        closeHandle (output);
        closeHandle (writer);
        closeHandle (nullDevice);
    }

    void stop() noexcept
    {
        if (job != nullptr) ::TerminateJobObject (job, 1);
        if (process != nullptr)
        {
            if (! assigned) ::TerminateProcess (process, 1);
            ::WaitForSingleObject (process, 200);
        }
        closeHandle (process);
        closeHandle (thread);
        closeHandle (job);
    }

    bool start (const std::vector<std::string>& arguments)
    {
        std::array<unsigned char, 16> random {};
        if (! BCRYPT_SUCCESS (::BCryptGenRandom (nullptr, random.data(), static_cast<ULONG> (random.size()),
                                                 BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return false;
        std::wstring name = L"\\\\.\\pipe\\dusk-scan-";
        constexpr wchar_t digits[] = L"0123456789abcdef";
        for (const auto byte : random)
        {
            name.push_back (digits[byte >> 4]);
            name.push_back (digits[byte & 15]);
        }
        output = ::CreateNamedPipeW (name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                     PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                     1, 65536, 65536, 0, nullptr);
        if (output == INVALID_HANDLE_VALUE) return false;
        SECURITY_ATTRIBUTES security { sizeof (SECURITY_ATTRIBUTES), nullptr, TRUE };
        writer = ::CreateFileW (name.c_str(), GENERIC_WRITE, 0, &security, OPEN_EXISTING, 0, nullptr);
        nullDevice = ::CreateFileW (L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &security, OPEN_EXISTING, 0, nullptr);
        if (writer == INVALID_HANDLE_VALUE || nullDevice == INVALID_HANDLE_VALUE) return false;

        job = ::CreateJobObjectW (nullptr, nullptr);
        if (job == nullptr) return false;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (! ::SetInformationJobObject (job, JobObjectExtendedLimitInformation, &limits, sizeof (limits))) return false;

        std::wstring executable, command;
        if (! widen (arguments.front(), executable)) return false;
        for (const auto& argument : arguments)
        {
            std::wstring wide;
            if (! widen (argument, wide)) return false;
            if (! command.empty()) command.push_back (L' ');
            appendArgument (command, wide);
        }

        SIZE_T bytes = 0;
        ::InitializeProcThreadAttributeList (nullptr, 1, 0, &bytes);
        if (bytes == 0) return false;
        std::vector<unsigned char> storage (bytes);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST> (storage.data());
        if (! ::InitializeProcThreadAttributeList (attributes, 1, 0, &bytes)) return false;
        struct AttributeCleanup
        {
            LPPROC_THREAD_ATTRIBUTE_LIST value;
            ~AttributeCleanup() { ::DeleteProcThreadAttributeList (value); }
        } cleanup { attributes };
        HANDLE inherited[] { writer, nullDevice };
        if (! ::UpdateProcThreadAttribute (attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                           inherited, sizeof (inherited), nullptr, nullptr)) return false;
        STARTUPINFOEXW startup {};
        startup.StartupInfo.cb = sizeof (startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = nullDevice;
        startup.StartupInfo.hStdOutput = writer;
        startup.StartupInfo.hStdError = nullDevice;
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION created {};
        if (! ::CreateProcessW (executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                                CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                nullptr, nullptr, &startup.StartupInfo, &created)) return false;
        process = created.hProcess;
        thread = created.hThread;
        if (! ::AssignProcessToJobObject (job, process)) return false;
        assigned = true;
        if (::ResumeThread (thread) == static_cast<DWORD> (-1)) return false;
        closeHandle (writer);
        closeHandle (nullDevice);
        return true;
    }

    ReadResult read (void* buffer, std::size_t capacity, int waitMs)
    {
        OVERLAPPED operation {};
        operation.hEvent = ::CreateEventW (nullptr, TRUE, FALSE, nullptr);
        if (operation.hEvent == nullptr) return { ReadStatus::Failed };
        DWORD bytes = 0;
        const auto requested = static_cast<DWORD> (std::min (capacity,
            static_cast<std::size_t> (std::numeric_limits<DWORD>::max())));
        const auto started = ::ReadFile (output, buffer, requested, nullptr, &operation);
        auto error = started ? ERROR_SUCCESS : ::GetLastError();
        bool cancelled = false;
        if (! started && error == ERROR_IO_PENDING)
        {
            if (::WaitForSingleObject (operation.hEvent, static_cast<DWORD> (std::max (0, waitMs))) != WAIT_OBJECT_0)
            {
                cancelled = true;
                ::CancelIoEx (output, &operation);
            }
            // The caller's buffer and OVERLAPPED must outlive cancellation.
            if (! ::GetOverlappedResult (output, &operation, &bytes, TRUE)) error = ::GetLastError();
            else error = ERROR_SUCCESS;
        }
        else if (started && ! ::GetOverlappedResult (output, &operation, &bytes, FALSE))
            error = ::GetLastError();
        closeHandle (operation.hEvent);
        if (bytes > 0) return { ReadStatus::Data, bytes };
        if (cancelled && error == ERROR_OPERATION_ABORTED) return { ReadStatus::Waiting };
        if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_SUCCESS)
            return { ReadStatus::Closed };
        return { ReadStatus::Failed };
    }
};
#else
struct ScanProcess::Impl
{
    int output = -1;
    pid_t child = -1;

    ~Impl()
    {
        stop();
        if (output >= 0) ::close (output);
    }

    void stop() noexcept
    {
        if (child <= 0) return;
        // Keep the leader unreaped until its group is killed, reserving its ID.
        ::kill (-child, SIGKILL);
        int status = 0;
        while (::waitpid (child, &status, 0) < 0 && errno == EINTR) {}
        child = -1;
    }

    bool start (const std::vector<std::string>& arguments)
    {
        int endpoints[2] { -1, -1 };
       #if defined(__linux__)
        if (::pipe2 (endpoints, O_CLOEXEC) != 0) return false;
       #else
        if (::pipe (endpoints) != 0) return false;
       #endif
        output = endpoints[0];
        struct Descriptor
        {
            int value;
            ~Descriptor() { if (value >= 0) ::close (value); }
        } writer { endpoints[1] }, nullDevice { ::open ("/dev/null", O_RDWR | O_CLOEXEC) };
        if (nullDevice.value < 0) return false;
        const auto outputFlags = ::fcntl (output, F_GETFL);
        if (outputFlags < 0 || ::fcntl (output, F_SETFL, outputFlags | O_NONBLOCK) < 0) return false;
        for (const auto fd : endpoints)
        {
            const auto flags = ::fcntl (fd, F_GETFD);
            if (flags < 0 || ::fcntl (fd, F_SETFD, flags | FD_CLOEXEC) < 0) return false;
        }
        // Darwin's spawn dup2 action must use distinct source and destination FDs.
        Descriptor source { ::fcntl (writer.value, F_DUPFD_CLOEXEC, 3) };
        Descriptor nullSource { ::fcntl (nullDevice.value, F_DUPFD_CLOEXEC, 3) };
        if (source.value < 0 || nullSource.value < 0) return false;
        posix_spawn_file_actions_t actions;
        if (::posix_spawn_file_actions_init (&actions) != 0) return false;
        struct ActionCleanup
        {
            posix_spawn_file_actions_t& value;
            ~ActionCleanup() { ::posix_spawn_file_actions_destroy (&value); }
        } cleanup { actions };
        int result = ::posix_spawn_file_actions_addclose (&actions, output);
        if (result == 0) result = ::posix_spawn_file_actions_addclose (&actions, writer.value);
        if (result == 0) result = ::posix_spawn_file_actions_adddup2 (&actions, source.value, STDOUT_FILENO);
        if (result == 0) result = ::posix_spawn_file_actions_adddup2 (&actions, nullSource.value, STDIN_FILENO);
        if (result == 0) result = ::posix_spawn_file_actions_adddup2 (&actions, nullSource.value, STDERR_FILENO);
        if (result == 0) result = ::posix_spawn_file_actions_addclose (&actions, source.value);
        if (result == 0) result = ::posix_spawn_file_actions_addclose (&actions, nullSource.value);
        if (result != 0) return false;
        posix_spawnattr_t attributes;
        if (::posix_spawnattr_init (&attributes) != 0) return false;
        struct AttributeCleanup
        {
            posix_spawnattr_t& value;
            ~AttributeCleanup() { ::posix_spawnattr_destroy (&value); }
        } attributeCleanup { attributes };
        sigset_t empty, defaults;
        sigemptyset (&empty);
        sigfillset (&defaults);
        sigdelset (&defaults, SIGKILL);
        sigdelset (&defaults, SIGSTOP);
        result = ::posix_spawnattr_setpgroup (&attributes, 0);
        if (result == 0) result = ::posix_spawnattr_setsigmask (&attributes, &empty);
        if (result == 0) result = ::posix_spawnattr_setsigdefault (&attributes, &defaults);
        if (result == 0) result = ::posix_spawnattr_setflags (&attributes,
            POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
        if (result != 0) return false;
        std::vector<char*> argv;
        for (const auto& argument : arguments) argv.push_back (const_cast<char*> (argument.c_str()));
        argv.push_back (nullptr);
        pid_t spawned = -1;
        if (::posix_spawn (&spawned, arguments.front().c_str(), &actions, &attributes, argv.data(), environ) != 0)
            return false;
        child = spawned;
        return true;
    }

    ReadResult read (void* buffer, std::size_t capacity, int waitMs)
    {
        pollfd descriptor { output, POLLIN, 0 };
        const auto ready = ::poll (&descriptor, 1, std::max (0, waitMs));
        if (ready == 0 || (ready < 0 && errno == EINTR)) return { ReadStatus::Waiting };
        if (ready < 0) return { ReadStatus::Failed };
        const auto bytes = ::read (output, buffer, capacity);
        if (bytes > 0) return { ReadStatus::Data, static_cast<std::size_t> (bytes) };
        if (bytes == 0) return { ReadStatus::Closed };
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return { ReadStatus::Waiting };
        return { ReadStatus::Failed };
    }
};
#endif

ScanProcess::ScanProcess() = default;
ScanProcess::~ScanProcess() = default;

bool ScanProcess::start (const std::vector<std::string>& arguments)
{
    impl.reset();
    if (arguments.empty() || arguments.front().empty()) return false;
    for (const auto& argument : arguments)
        if (argument.find ('\0') != std::string::npos) return false;
    auto candidate = std::make_unique<Impl>();
    if (! candidate->start (arguments)) return false;
    impl = std::move (candidate);
    return true;
}

ScanProcess::ReadResult ScanProcess::readOutput (void* buffer, std::size_t capacity, int waitMs)
{
    if (impl == nullptr || buffer == nullptr || capacity == 0) return { ReadStatus::Failed };
    return impl->read (buffer, capacity, waitMs);
}

void ScanProcess::stop() noexcept
{
    if (impl != nullptr) impl->stop();
}
} // namespace duskstudio
