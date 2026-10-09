#include "RealtimeKit.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#if DUSKSTUDIO_HAS_RTKIT
 #include <dbus/dbus.h>
#endif

namespace duskstudio::rt
{
namespace
{
// RLIMIT_NICE is 20 minus the lowest nice level a thread may take without
// privilege, so its default of 0 leaves nothing below the normal 0.
int lowestNiceAllowed() noexcept
{
    struct rlimit rl {};
    if (getrlimit (RLIMIT_NICE, &rl) != 0)
        return 0;
    if (rl.rlim_cur == RLIM_INFINITY)
        return -20;
    return std::clamp (20 - (int) rl.rlim_cur, -20, 0);
}

#if DUSKSTUDIO_HAS_RTKIT
constexpr const char* kService   = "org.freedesktop.RealtimeKit1";
constexpr const char* kPath      = "/org/freedesktop/RealtimeKit1";
constexpr const char* kInterface = "org.freedesktop.RealtimeKit1";
// The bus starts RTKit on first use, which can take a moment. One that has not
// answered in this long is treated as having no RTKit.
constexpr int kCallTimeoutMs = 2000;

class RtKit
{
public:
    RtKit()
    {
        DBusError failure;
        dbus_error_init (&failure);
        bus = dbus_bus_get_private (DBUS_BUS_SYSTEM, &failure);
        if (bus == nullptr)
        {
            noteError (failure);
            return;
        }
        dbus_connection_set_exit_on_disconnect (bus, FALSE);
    }

    ~RtKit()
    {
        if (bus == nullptr) return;
        dbus_connection_close (bus);
        dbus_connection_unref (bus);
    }

    RtKit (const RtKit&) = delete;
    RtKit& operator= (const RtKit&) = delete;

    bool connected() const noexcept { return bus != nullptr; }
    const std::string& lastError() const noexcept { return error; }

    // One of RTKit's integer properties (MaxRealtimePriority, MinNiceLevel,
    // RTTimeUSecMax).
    bool property (const char* name, long long& out)
    {
        auto* call = dbus_message_new_method_call (kService, kPath,
                                                   "org.freedesktop.DBus.Properties", "Get");
        if (call == nullptr) return false;
        const char* interface = kInterface;
        if (! dbus_message_append_args (call, DBUS_TYPE_STRING, &interface,
                                        DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID))
        {
            dbus_message_unref (call);
            return false;
        }
        auto* reply = send (call);
        if (reply == nullptr) return false;

        bool read = false;
        DBusMessageIter args;
        if (dbus_message_iter_init (reply, &args)
            && dbus_message_iter_get_arg_type (&args) == DBUS_TYPE_VARIANT)
        {
            DBusMessageIter value;
            dbus_message_iter_recurse (&args, &value);
            const int type = dbus_message_iter_get_arg_type (&value);
            if (type == DBUS_TYPE_INT32)
            {
                dbus_int32_t v = 0;
                dbus_message_iter_get_basic (&value, &v);
                out = v;
                read = true;
            }
            else if (type == DBUS_TYPE_INT64)
            {
                dbus_int64_t v = 0;
                dbus_message_iter_get_basic (&value, &v);
                out = (long long) v;
                read = true;
            }
        }
        dbus_message_unref (reply);
        return read;
    }

    bool makeRealtime (std::int64_t threadId, int priority)
    {
        auto* call = dbus_message_new_method_call (kService, kPath, kInterface,
                                                   "MakeThreadRealtimeWithPID");
        if (call == nullptr) return false;
        dbus_uint64_t process = (dbus_uint64_t) getpid();
        dbus_uint64_t thread = (dbus_uint64_t) threadId;
        dbus_uint32_t level = (dbus_uint32_t) priority;
        return callWith (call, dbus_message_append_args (call, DBUS_TYPE_UINT64, &process,
                                                         DBUS_TYPE_UINT64, &thread,
                                                         DBUS_TYPE_UINT32, &level,
                                                         DBUS_TYPE_INVALID));
    }

    bool makeHighPriority (std::int64_t threadId, int nice)
    {
        auto* call = dbus_message_new_method_call (kService, kPath, kInterface,
                                                   "MakeThreadHighPriorityWithPID");
        if (call == nullptr) return false;
        dbus_uint64_t process = (dbus_uint64_t) getpid();
        dbus_uint64_t thread = (dbus_uint64_t) threadId;
        dbus_int32_t level = nice;
        return callWith (call, dbus_message_append_args (call, DBUS_TYPE_UINT64, &process,
                                                         DBUS_TYPE_UINT64, &thread,
                                                         DBUS_TYPE_INT32, &level,
                                                         DBUS_TYPE_INVALID));
    }

private:
    bool callWith (DBusMessage* call, dbus_bool_t appended)
    {
        if (! appended)
        {
            dbus_message_unref (call);
            return false;
        }
        auto* reply = send (call);
        if (reply == nullptr) return false;
        dbus_message_unref (reply);
        return true;
    }

    // Takes the call. An error reply comes back as null, its text kept.
    DBusMessage* send (DBusMessage* call)
    {
        DBusError failure;
        dbus_error_init (&failure);
        auto* reply = dbus_connection_send_with_reply_and_block (bus, call, kCallTimeoutMs, &failure);
        dbus_message_unref (call);
        if (reply == nullptr)
            noteError (failure);
        return reply;
    }

    void noteError (DBusError& failure)
    {
        error = failure.message != nullptr ? failure.message : "no reply";
        dbus_error_free (&failure);
    }

    DBusConnection* bus = nullptr;
    std::string error;
};

// RTKit grants realtime only to a process whose hard RLIMIT_RTTIME is at most
// RTTimeUSecMax, so a realtime thread that stops blocking is stopped instead of
// locking the machine up. PipeWire's client library sets the same limit when
// it takes realtime for its own data thread through RTKit. A tighter limit
// already in place is kept as it is.
bool limitRealtimeCpuTime (long long maxMicros) noexcept
{
    if (maxMicros <= 0) return false;
    const auto ceiling = (rlim_t) maxMicros;
    struct rlimit rl {};
    if (getrlimit (RLIMIT_RTTIME, &rl) != 0) return false;
    if (rl.rlim_max == RLIM_INFINITY || rl.rlim_max > ceiling)
    {
        rl.rlim_max = ceiling;
        rl.rlim_cur = std::min (rl.rlim_cur, ceiling);
        if (setrlimit (RLIMIT_RTTIME, &rl) != 0) return false;
    }
    guardRealtimeCpuTime();
    return true;
}
#endif

std::atomic<int>           demotedThreads { 0 };
std::atomic<int>           unattributedOverruns { 0 };
std::atomic<std::int64_t>  lastDemotedThread { 0 };
// The soft limit the guard keeps. The kernel moves it on by a second each time
// it sends SIGXCPU, which would leave the next overrun the hard limit alone.
std::atomic<std::uint64_t> keptSoftLimit { 0 };
static_assert (std::atomic<int>::is_always_lock_free
                   && std::atomic<std::int64_t>::is_always_lock_free
                   && std::atomic<std::uint64_t>::is_always_lock_free,
               "the SIGXCPU handler may only touch lock-free atomics");

// Signal context: raw system calls and lock-free atomics, nothing else. The
// kernel sends SIGXCPU to the process but hands it to the thread that ran over
// whenever that thread does not block it, so the thread taking it here is the
// one to move off realtime.
void onRealtimeOverrun (int) noexcept
{
    const int savedErrno = errno;
    const int scheduler = sched_getscheduler (0);
    const int policy = scheduler & ~SCHED_RESET_ON_FORK;
    sched_param normal {};
    // RTKit grants realtime with reset-on-fork set, which an unprivileged
    // thread may not clear, and a thread without it would be kept from ever
    // taking realtime again: the move leaves the flag as it was.
    if ((policy == SCHED_FIFO || policy == SCHED_RR)
        && sched_setscheduler (0, SCHED_OTHER | (scheduler & SCHED_RESET_ON_FORK), &normal) == 0)
    {
        lastDemotedThread.store ((std::int64_t) syscall (SYS_gettid), std::memory_order_relaxed);
        demotedThreads.fetch_add (1, std::memory_order_release);
    }
    else
    {
        unattributedOverruns.fetch_add (1, std::memory_order_relaxed);
    }

    const auto kept = (rlim_t) keptSoftLimit.load (std::memory_order_relaxed);
    struct rlimit rl {};
    if (kept != 0 && getrlimit (RLIMIT_RTTIME, &rl) == 0 && rl.rlim_cur != kept
        && (rl.rlim_max == RLIM_INFINITY || kept < rl.rlim_max))
    {
        rl.rlim_cur = kept;
        setrlimit (RLIMIT_RTTIME, &rl);
    }
    errno = savedErrno;
}

// True with the handler in place, put there over the default action or an
// ignore. False when SIGXCPU has a handler of someone else's, which keeps it.
bool ownRealtimeOverrunSignal() noexcept
{
    struct sigaction current {};
    if (sigaction (SIGXCPU, nullptr, &current) != 0) return false;
    const bool withInfo = (current.sa_flags & SA_SIGINFO) != 0;
    if (! withInfo && current.sa_handler == &onRealtimeOverrun) return true;
    if (withInfo || (current.sa_handler != SIG_DFL && current.sa_handler != SIG_IGN)) return false;
    struct sigaction ours {};
    ours.sa_handler = &onRealtimeOverrun;
    sigemptyset (&ours.sa_mask);
    ours.sa_flags = SA_RESTART;
    return sigaction (SIGXCPU, &ours, nullptr) == 0;
}
} // namespace

std::string raiseThreadsWithoutRtPrio (const std::vector<std::int64_t>& threadIds)
{
    if (threadIds.empty()) return {};

    int realtime = 0, raised = 0, untouched = 0;
    std::string detail;
    long long realtimePriority = 0;
    const int niceFloor = lowestNiceAllowed();

   #if DUSKSTUDIO_HAS_RTKIT
    // A bus that does not answer the first question is not asked again, so a
    // missing or wedged RTKit costs one timeout rather than one per thread.
    RtKit rtkit;
    long long rtTimeMax = 0, rtkitNice = 0;
    const bool rtkitAnswers = rtkit.connected()
                           && rtkit.property ("MaxRealtimePriority", realtimePriority);
    bool tryRealtime = rtkitAnswers && realtimePriority > 0
                    && rtkit.property ("RTTimeUSecMax", rtTimeMax)
                    && limitRealtimeCpuTime (rtTimeMax);
    bool tryNice = rtkitAnswers && rtkit.property ("MinNiceLevel", rtkitNice) && rtkitNice < 0;
   #endif

    for (const auto threadId : threadIds)
    {
       #if DUSKSTUDIO_HAS_RTKIT
        if (tryRealtime)
        {
            if (rtkit.makeRealtime (threadId, (int) realtimePriority))
            {
                ++realtime;
                continue;
            }
            tryRealtime = false;
        }
        if (tryNice)
        {
            if (rtkit.makeHighPriority (threadId, (int) rtkitNice))
            {
                ++raised;
                continue;
            }
            tryNice = false;
        }
       #endif
        if (niceFloor < 0 && setpriority (PRIO_PROCESS, (id_t) threadId, niceFloor) == 0)
            ++raised;
        else
            ++untouched;
    }

   #if DUSKSTUDIO_HAS_RTKIT
    if (realtime < (int) threadIds.size() && ! rtkit.lastError().empty())
        detail = " (RTKit: " + rtkit.lastError() + ")";
   #endif

    std::string summary;
    const auto add = [&summary] (int count, const std::string& what)
    {
        if (count == 0) return;
        summary += (summary.empty() ? "" : ", ") + std::to_string (count) + " " + what;
    };
    add (realtime, "realtime through RTKit at priority " + std::to_string (realtimePriority));
    add (raised, "at a raised nice level");
    add (untouched, "at default priority");
    return summary + detail;
}

bool guardRealtimeCpuTime() noexcept
{
    struct rlimit rl {};
    // The soft limit never exceeds the hard one, so an unlimited soft limit
    // means there is nothing to warn about or be killed by.
    if (getrlimit (RLIMIT_RTTIME, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY)
        return false;
    if (! ownRealtimeOverrunSignal())
        return false;
    if (rl.rlim_max != RLIM_INFINITY && rl.rlim_cur >= rl.rlim_max)
    {
        const auto kept = (rlim_t) keptSoftLimit.load (std::memory_order_relaxed);
        rl.rlim_cur = (kept != 0 && kept < rl.rlim_max) ? kept : rl.rlim_max - rl.rlim_max / 4;
        if (rl.rlim_cur == 0 || setrlimit (RLIMIT_RTTIME, &rl) != 0)
            return false;
    }
    keptSoftLimit.store ((std::uint64_t) rl.rlim_cur, std::memory_order_relaxed);
    return true;
}

RealtimeDemotions realtimeDemotions() noexcept
{
    RealtimeDemotions demotions;
    demotions.count        = demotedThreads.load (std::memory_order_acquire);
    demotions.lastThreadId = lastDemotedThread.load (std::memory_order_relaxed);
    demotions.unattributed = unattributedOverruns.load (std::memory_order_relaxed);
    return demotions;
}
} // namespace duskstudio::rt
