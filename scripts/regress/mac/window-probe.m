// Reports what the window server shows, for the window-activation leg:
//
//   front <pid> <bundle id>
//   win <z> <pid> <layer> <onscreen> <alpha> <x> <y> <w> <h> <window id> <owner>
//
// "win" lines list every window the window server reports, of every layer, the
// on-screen ones first in front-to-back order; z counts from 0 at the front
// among the on-screen ones and is -1 for a window that is not on screen
// (minimized, hidden, or on another space). Owner names have spaces replaced
// by underscores. Window titles need Screen Recording access, so none are read.
// Built and run over ssh on the node; it never draws, activates or asks for
// any permission.

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <stdio.h>

static void printWindows (CGWindowListOption option, bool onScreen)
{
    CFArrayRef list = CGWindowListCopyWindowInfo (option, kCGNullWindowID);
    if (list == NULL) return;
    int z = 0;
    for (NSDictionary* w in (__bridge NSArray*) list)
    {
        const bool shown = [w[(id) kCGWindowIsOnscreen] boolValue];
        if (shown != onScreen) continue;
        CGRect r = CGRectZero;
        CGRectMakeWithDictionaryRepresentation ((__bridge CFDictionaryRef) w[(id) kCGWindowBounds], &r);
        NSString* owner = w[(id) kCGWindowOwnerName] ?: @"?";
        printf ("win %d %d %d %d %.2f %.0f %.0f %.0f %.0f %u %s\n",
                onScreen ? z : -1,
                [w[(id) kCGWindowOwnerPID] intValue], [w[(id) kCGWindowLayer] intValue], shown ? 1 : 0,
                [w[(id) kCGWindowAlpha] doubleValue],
                r.origin.x, r.origin.y, r.size.width, r.size.height,
                [w[(id) kCGWindowNumber] unsignedIntValue],
                [[owner stringByReplacingOccurrencesOfString: @" " withString: @"_"] UTF8String]);
        if (onScreen) ++z;
    }
    CFRelease (list);
}

int main (void)
{
    @autoreleasepool
    {
        NSRunningApplication* front = [[NSWorkspace sharedWorkspace] frontmostApplication];
        printf ("front %d %s\n", front != nil ? front.processIdentifier : -1,
                front != nil && front.bundleIdentifier != nil ? front.bundleIdentifier.UTF8String : "-");
        printWindows (kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, true);
        printWindows (kCGWindowListOptionAll | kCGWindowListExcludeDesktopElements, false);
    }
    return 0;
}
