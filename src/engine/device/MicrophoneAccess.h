#pragma once

#include <functional>
#include <string>

namespace duskstudio::device
{
// Whether the operating system lets the app capture audio. Only macOS asks:
// the first input a process starts brings up its microphone prompt, and until
// the prompt is answered CoreAudio holds every call on that input. Left
// unanswered for about three minutes, the open fails with a timeout
// (0x10004003, MACH_RCV_TIMED_OUT) and takes the output paired with it down
// too. Everywhere else access is always Granted.
enum class MicrophoneAccess { Granted, Denied, Undecided };

MicrophoneAccess microphoneAccess();

// Shows the prompt while access is Undecided. onAnswer runs once, on an
// arbitrary thread, when the user answers; with access already decided it
// runs straight away with that decision.
void requestMicrophoneAccess (std::function<void (bool granted)> onAnswer);

// The line the transport bar shows while inputs are missing for want of
// access. Empty when access is Granted.
inline std::string microphoneAccessNotice (MicrophoneAccess access)
{
    switch (access)
    {
        case MicrophoneAccess::Undecided:
            return "Waiting for microphone access. Answer the macOS prompt to record.";
        case MicrophoneAccess::Denied:
            return "Microphone access is off. Allow Dusk Studio in System Settings > "
                   "Privacy & Security > Microphone to record.";
        case MicrophoneAccess::Granted:
            break;
    }
    return {};
}

#if ! defined (__APPLE__)
inline MicrophoneAccess microphoneAccess() { return MicrophoneAccess::Granted; }

inline void requestMicrophoneAccess (std::function<void (bool granted)> onAnswer)
{
    if (onAnswer) onAnswer (true);
}
#endif
} // namespace duskstudio::device
