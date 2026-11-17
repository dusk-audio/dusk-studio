#pragma once

// Shared between the render-probe VST3 fixture and the tests that drive it.
// Each block the processor writes what it finds on the thread that called it
// into the first samples of its left output, and passes the right one through.
namespace duskstudio::test::renderprobevst3
{
constexpr int kFlushesResults = 0;   // 1 when a result below the normal range comes out as zero
constexpr int kFlushesInputs  = 1;   // 1 when an input below the normal range reads as zero
// The thread's scheduling, as each system states it:
//   Linux    its policy, and its realtime priority under SCHED_RR or SCHED_FIFO
//   Windows  GetThreadPriority, and the priority the scheduler runs it at, 1 to
//            31, where 16 and up is the realtime range MMCSS lifts its tasks into
//   macOS    1 under the time-constraint policy, else 0, and that policy's
//            period in microseconds, else -1
constexpr int kPolicy         = 2;
constexpr int kPriority       = 3;
constexpr int kThreadId       = 4;   // the calling thread, as the system names it within the process
constexpr int kReportLength   = 5;

// Linux: names a folder. initialize() writes its process id to `held` there,
// then waits until `release` appears, so a test can look at a scan child while
// it is still loading the plug-in.
constexpr const char* kHoldVariable = "DUSKSTUDIO_RENDER_PROBE_HOLD";
} // namespace duskstudio::test::renderprobevst3
