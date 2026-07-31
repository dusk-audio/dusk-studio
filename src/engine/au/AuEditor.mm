#include "AuEditor.h"

#include "AuInstance.h"

#import <AppKit/AppKit.h>
#import <AudioUnit/AUCocoaUIView.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace duskstudio::au
{
struct AuEditor::Impl
{
    NSView* container = nil;
    NSView* pluginView = nil;
    int preferredWidth = 0;
    int preferredHeight = 0;
    bool embedded = false;
    bool visible = false;
};

AuEditor::AuEditor() : impl (std::make_unique<Impl>()) {}
AuEditor::~AuEditor() { close(); }

int AuEditor::preferredWidth() const noexcept { return impl->preferredWidth; }
int AuEditor::preferredHeight() const noexcept { return impl->preferredHeight; }
bool AuEditor::isOpen() const noexcept { return impl->pluginView != nil; }
bool AuEditor::isEmbedded() const noexcept { return impl->embedded; }

bool AuEditor::open (AuInstance& instance, std::string& errorOut)
{
    if (impl->pluginView != nil) return true;
    auto unit = static_cast<AudioUnit> (instance.nativeAudioUnit());
    if (unit == nullptr)
    {
        errorOut = "null Audio Unit";
        return false;
    }

    UInt32 dataSize = 0;
    Boolean writable = false;
    if (AudioUnitGetPropertyInfo (unit, kAudioUnitProperty_CocoaUI,
                                  kAudioUnitScope_Global, 0,
                                  &dataSize, &writable) != noErr
        || dataSize < sizeof (AudioUnitCocoaViewInfo))
    {
        errorOut = "Audio Unit has no Cocoa view factory";
        return false;
    }

    std::vector<std::byte> storage (dataSize);
    auto* info = reinterpret_cast<AudioUnitCocoaViewInfo*> (storage.data());
    if (AudioUnitGetProperty (unit, kAudioUnitProperty_CocoaUI,
                              kAudioUnitScope_Global, 0, info, &dataSize) != noErr)
    {
        errorOut = "could not read Cocoa view factory";
        return false;
    }

    const auto classOffset = offsetof (AudioUnitCocoaViewInfo, mCocoaAUViewClass);
    const auto classCount = dataSize > classOffset
        ? (dataSize - classOffset) / sizeof (CFStringRef) : 0;
    NSBundle* bundle = info->mCocoaAUViewBundleLocation != nullptr
        ? [NSBundle bundleWithURL:(NSURL*) info->mCocoaAUViewBundleLocation] : nil;

    for (std::size_t i = 0; i < classCount && impl->pluginView == nil; ++i)
    {
        auto* className = (NSString*) info->mCocoaAUViewClass[i];
        Class viewClass = bundle != nil && className != nil
            ? [bundle classNamed:className] : Nil;
        if (viewClass == Nil || ! [viewClass conformsToProtocol:@protocol (AUCocoaUIBase)]
            || ! [viewClass instancesRespondToSelector:
                    @selector (uiViewForAudioUnit:withSize:)])
            continue;

        // A third-party factory that raises must not unwind through the C++
        // frames above (it would skip the CFRelease sweep below and reach the
        // message loop) - drop this candidate so a later class can still try.
        id<AUCocoaUIBase> factory = nil;
        @try
        {
            factory = [[viewClass alloc] init];
            NSView* view = [factory uiViewForAudioUnit:unit withSize:NSMakeSize (480, 320)];
            if (view != nil) impl->pluginView = [view retain];
        }
        @catch (NSException*)
        {
        }
        @finally
        {
            [(id) factory release];
        }
    }

    for (std::size_t i = 0; i < classCount; ++i)
        if (info->mCocoaAUViewClass[i] != nullptr)
            CFRelease (info->mCocoaAUViewClass[i]);
    if (info->mCocoaAUViewBundleLocation != nullptr)
        CFRelease (info->mCocoaAUViewBundleLocation);

    if (impl->pluginView == nil)
    {
        errorOut = "Audio Unit Cocoa view factory returned no view";
        return false;
    }

    const auto size = [impl->pluginView frame].size;
    impl->preferredWidth = std::max (1, static_cast<int> (size.width));
    impl->preferredHeight = std::max (1, static_cast<int> (size.height));
    return true;
}

bool AuEditor::embed (std::uintptr_t parentHandle, int x, int y, int width, int height,
                      std::string& errorOut)
{
    if (impl->pluginView == nil)
    {
        errorOut = "no Cocoa view open";
        return false;
    }
    if (impl->embedded) return true;
    if (parentHandle == 0)
    {
        errorOut = "null parent view";
        return false;
    }

    auto* parent = reinterpret_cast<NSView*> (parentHandle);
    const int w = std::max (1, width > 0 ? width : impl->preferredWidth);
    const int h = std::max (1, height > 0 ? height : impl->preferredHeight);
    impl->container = [[NSView alloc] initWithFrame:NSMakeRect (
        static_cast<CGFloat> (x), static_cast<CGFloat> (y),
        static_cast<CGFloat> (w), static_cast<CGFloat> (h))];
    if (impl->container == nil)
    {
        errorOut = "could not create Cocoa container";
        return false;
    }
    [impl->container setAutoresizingMask:NSViewNotSizable];
    [impl->container setHidden:YES];
    @try
    {
        [impl->pluginView setFrame:NSMakeRect (0, 0, w, h)];
        [impl->pluginView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [impl->container addSubview:impl->pluginView];
        [parent addSubview:impl->container];
    }
    @catch (NSException*)
    {
        [impl->pluginView removeFromSuperview];
        [impl->container removeFromSuperview];
        [impl->container release];
        impl->container = nil;
        errorOut = "Audio Unit Cocoa view could not be embedded";
        return false;
    }
    impl->embedded = true;
    return true;
}

void AuEditor::setBounds (int x, int y, int width, int height)
{
    if (impl->container == nil) return;
    const int w = std::max (1, width);
    const int h = std::max (1, height);
    [impl->container setFrame:NSMakeRect (x, y, w, h)];
    [impl->pluginView setFrame:NSMakeRect (0, 0, w, h)];
}

void AuEditor::reveal()
{
    if (impl->container == nil || impl->visible) return;
    [impl->container setHidden:NO];
    impl->visible = true;
}

void AuEditor::hide()
{
    if (impl->container == nil || ! impl->visible) return;
    [impl->container setHidden:YES];
    impl->visible = false;
}

void AuEditor::pump()
{
    if (impl->pluginView == nil) return;
    const auto size = [impl->pluginView frame].size;
    const int width = std::max (1, static_cast<int> (size.width));
    const int height = std::max (1, static_cast<int> (size.height));
    if (width == impl->preferredWidth && height == impl->preferredHeight) return;
    impl->preferredWidth = width;
    impl->preferredHeight = height;
    if (onResize) onResize (width, height);
}

void AuEditor::close()
{
    if (impl->pluginView != nil)
    {
        [impl->pluginView removeFromSuperview];
        [impl->pluginView release];
        impl->pluginView = nil;
    }
    if (impl->container != nil)
    {
        [impl->container removeFromSuperview];
        [impl->container release];
        impl->container = nil;
    }
    impl->preferredWidth = impl->preferredHeight = 0;
    impl->embedded = impl->visible = false;
}
} // namespace duskstudio::au
