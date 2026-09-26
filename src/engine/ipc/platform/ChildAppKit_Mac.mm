#include "ChildAppKit.h"

#import <AppKit/AppKit.h>

namespace duskstudio::ipc::platform
{
void prepareChildAppKit()
{
    @autoreleasepool
    {
        [NSApplication sharedApplication];
        // The child runs from inside the app bundle and would otherwise show a
        // second Dock icon. An accessory app can still own the fallback editor window.
        [NSApp setActivationPolicy: NSApplicationActivationPolicyAccessory];
    }
}
} // namespace duskstudio::ipc::platform
