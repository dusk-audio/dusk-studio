#include "MicrophoneAccess.h"

#import <AVFoundation/AVFoundation.h>

#include <memory>
#include <utility>

namespace duskstudio::device
{
MicrophoneAccess microphoneAccess()
{
    switch ([AVCaptureDevice authorizationStatusForMediaType: AVMediaTypeAudio])
    {
        case AVAuthorizationStatusAuthorized:    return MicrophoneAccess::Granted;
        case AVAuthorizationStatusNotDetermined: return MicrophoneAccess::Undecided;
        case AVAuthorizationStatusDenied:
        case AVAuthorizationStatusRestricted:    break;
    }
    return MicrophoneAccess::Denied;
}

void requestMicrophoneAccess (std::function<void (bool granted)> onAnswer)
{
    auto handler = std::make_shared<std::function<void (bool)>> (std::move (onAnswer));
    [AVCaptureDevice requestAccessForMediaType: AVMediaTypeAudio
                             completionHandler: ^(BOOL granted)
    {
        if (*handler) (*handler) (granted == YES);
    }];
}
} // namespace duskstudio::device
