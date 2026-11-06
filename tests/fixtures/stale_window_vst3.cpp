#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/common/pluginview.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include <X11/Xlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Stands in for a plug-in whose editor asks the X server about a window the
// host owns after the host has destroyed it. DUSKSTUDIO_STALE_WINDOW_PROBE picks
// the moment and the window:
//   editor:<xid>  query <xid> when the view is attached and again when removed
//   load:<xid>    query <xid> while the plug-in initialises, before any editor
//   own           query a window this plug-in created and destroyed, on attach
//   after:<xid>   query <xid> in terminate, once the host has dropped the editor
// Each query appends "<moment> querying" to the file named by
// DUSKSTUDIO_STALE_WINDOW_PROBE_LOG before it starts and, if it returns,
// "<moment> <XQueryTree result>" after it.
namespace Steinberg::Vst
{
namespace
{
std::string probeSetting()
{
    const char* value = std::getenv ("DUSKSTUDIO_STALE_WINDOW_PROBE");
    return value != nullptr ? value : "";
}

void record (const char* moment, const std::string& text)
{
    const char* path = std::getenv ("DUSKSTUDIO_STALE_WINDOW_PROBE_LOG");
    if (path == nullptr) return;
    if (auto* file = std::fopen (path, "a"))
    {
        std::fprintf (file, "%s %s\n", moment, text.c_str());
        std::fclose (file);
    }
}

int queryTree (Display* display, ::Window window)
{
    ::Window root = 0, parent = 0;
    ::Window* children = nullptr;
    unsigned int count = 0;
    const int result = XQueryTree (display, window, &root, &parent, &children, &count);
    if (children != nullptr) XFree (children);
    return result;
}

void probe (const char* moment, const std::string& prefix)
{
    const auto setting = probeSetting();
    const bool own = prefix == "editor:" && setting == "own";
    if (! own && setting.compare (0, prefix.size(), prefix) != 0) return;

    Display* display = XOpenDisplay (nullptr);
    if (display == nullptr) return;

    ::Window target = 0;
    if (own)
    {
        target = XCreateSimpleWindow (display, DefaultRootWindow (display), 0, 0, 8, 8, 0, 0, 0);
        XDestroyWindow (display, target);
        XSync (display, False);
    }
    else
    {
        target = static_cast<::Window> (std::strtoull (setting.c_str() + prefix.size(), nullptr, 0));
    }

    record (moment, "querying");
    record (moment, std::to_string (queryTree (display, target)));
    XCloseDisplay (display);
}

class StaleWindowView final : public CPluginView
{
public:
    tresult PLUGIN_API attached (void* parent, FIDString type) override
    {
        const auto result = CPluginView::attached (parent, type);
        probe ("attached", "editor:");
        return result;
    }

    tresult PLUGIN_API removed() override
    {
        probe ("removed", "editor:");
        return CPluginView::removed();
    }

    tresult PLUGIN_API getSize (ViewRect* size) override
    {
        if (size == nullptr) return kInvalidArgument;
        *size = ViewRect (0, 0, 120, 80);
        return kResultTrue;
    }
};

class StaleWindowEffect final : public SingleComponentEffect
{
public:
    static FUnknown* createInstance (void*)
    {
        return static_cast<IAudioProcessor*> (new StaleWindowEffect);
    }

    tresult PLUGIN_API initialize (FUnknown* context) override
    {
        const auto result = SingleComponentEffect::initialize (context);
        if (result != kResultOk) return result;
        addAudioInput (STR16 ("Input"), SpeakerArr::kStereo);
        addAudioOutput (STR16 ("Output"), SpeakerArr::kStereo);
        probe ("load", "load:");
        return kResultOk;
    }

    tresult PLUGIN_API terminate() override
    {
        probe ("after", "after:");
        return SingleComponentEffect::terminate();
    }

    IPlugView* PLUGIN_API createView (FIDString) override { return new StaleWindowView; }

    tresult PLUGIN_API setProcessing (TBool) override { return kResultOk; }

    tresult PLUGIN_API process (ProcessData& data) override
    {
        if (data.numInputs < 1 || data.numOutputs < 1) return kResultOk;
        for (int32 channel = 0; channel < 2; ++channel)
        {
            const auto* in = data.inputs[0].channelBuffers32[channel];
            auto* out = data.outputs[0].channelBuffers32[channel];
            if (in != nullptr && out != nullptr && in != out)
                std::memcpy (out, in, sizeof (float) * static_cast<size_t> (data.numSamples));
        }
        return kResultOk;
    }
};
} // namespace
} // namespace Steinberg::Vst

BEGIN_FACTORY_DEF ("Dusk Audio", "https://dusk.audio", "support@dusk.audio")
    DEF_CLASS2 (INLINE_UID (0x4B1D6C2E, 0x93A74F58, 0xB2E0C6D1, 0x5A8F3E47),
                PClassInfo::kManyInstances, kVstAudioEffectClass,
                "Dusk Stale Window Fixture", 0, "Fx", "1.0.0", kVstVersionString,
                Steinberg::Vst::StaleWindowEffect::createInstance)
END_FACTORY
