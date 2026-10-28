#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/ipc/platform/IpcChannel.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
namespace ipcp = duskstudio::ipc::platform;

struct ScopedChannelPair
{
    ~ScopedChannelPair()
    {
        ipcp::closeHandle (value.parentEnd);
        ipcp::closeHandle (value.childEnd);
    }

    ipcp::ChannelPair value;
};

struct ScopedHandle
{
    ~ScopedHandle() { ipcp::closeHandle (value); }

    ipcp::NativeHandle value;
};
}

TEST_CASE ("POSIX IPC sends survive a closed peer", "[ipc][posix]")
{
    const bool useChildEnd = GENERATE (false, true);
    bool sendDescriptor = false;
    SECTION ("control bytes") {}
    SECTION ("descriptor handoff") { sendDescriptor = true; }

    ScopedChannelPair channels;
    std::string error;
    REQUIRE (ipcp::createChannelPair (channels.value, error));
    auto& sender = useChildEnd ? channels.value.childEnd : channels.value.parentEnd;
    auto& peer = useChildEnd ? channels.value.parentEnd : channels.value.childEnd;
    ipcp::closeHandle (peer);

    const pid_t probe = ::fork();
    if (probe == 0)
    {
        struct sigaction defaultAction {};
        defaultAction.sa_handler = SIG_DFL;
        sigemptyset (&defaultAction.sa_mask);
        if (::sigaction (SIGPIPE, &defaultAction, nullptr) != 0) ::_exit (2);

        sigset_t pipeSignal;
        sigemptyset (&pipeSignal);
        sigaddset (&pipeSignal, SIGPIPE);
        if (::sigprocmask (SIG_UNBLOCK, &pipeSignal, nullptr) != 0) ::_exit (3);

        const char byte = 'x';
        const bool sent = sendDescriptor ? ipcp::sendHandle (sender, sender)
                                         : ipcp::writeExact (sender, &byte, 1);
        const int sendError = errno;
        struct sigaction currentAction {};
        sigset_t currentMask;
        if (::sigaction (SIGPIPE, nullptr, &currentAction) != 0
            || ::sigprocmask (SIG_SETMASK, nullptr, &currentMask) != 0)
            ::_exit (4);

        ::_exit (! sent && sendError == EPIPE
                 && currentAction.sa_handler == SIG_DFL
                 && sigismember (&currentMask, SIGPIPE) == 0 ? 0 : 1);
    }

    REQUIRE (probe > 0);
    int status = 0;
    pid_t waited = -1;
    do
    {
        waited = ::waitpid (probe, &status, 0);
    }
    while (waited < 0 && errno == EINTR);

    REQUIRE (waited == probe);
    CAPTURE (useChildEnd, sendDescriptor);
    INFO ("sender signal " << (WIFSIGNALED (status) ? WTERMSIG (status) : 0));
    CHECK (WIFEXITED (status));
    if (WIFEXITED (status))
        CHECK (WEXITSTATUS (status) == 0);
}

TEST_CASE ("POSIX IPC channels preserve control bytes and descriptor transfers", "[ipc][posix]")
{
    for (const bool reverse : { false, true })
    {
        ScopedChannelPair channels;
        ScopedChannelPair payload;
        std::string error;
        REQUIRE (ipcp::createChannelPair (channels.value, error));
        REQUIRE (ipcp::createChannelPair (payload.value, error));
        auto& sender = reverse ? channels.value.childEnd : channels.value.parentEnd;
        auto& receiver = reverse ? channels.value.parentEnd : channels.value.childEnd;

        const std::array<char, 5> expected { 'd', '\0', 'u', 's', 'k' };
        std::array<char, 5> received {};
        REQUIRE (ipcp::writeExact (sender, expected.data(), expected.size()));
        REQUIRE (ipcp::readExact (receiver, received.data(), received.size()));
        REQUIRE (received == expected);

        ScopedHandle receivedHandle;
        REQUIRE (ipcp::sendHandle (sender, payload.value.parentEnd));
        REQUIRE (ipcp::recvHandle (receiver, receivedHandle.value));
        ipcp::closeHandle (payload.value.parentEnd);
        REQUIRE (ipcp::writeExact (receivedHandle.value, expected.data(), expected.size()));
        received.fill (0);
        REQUIRE (ipcp::readExact (payload.value.childEnd, received.data(), received.size()));
        REQUIRE (received == expected);
    }
}
