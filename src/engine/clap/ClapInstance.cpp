#include "ClapInstance.h"
#include "ClapBundle.h"

#include <clap/clap.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace duskstudio::clap
{
ClapInstance::ClapInstance()
{
    // Empty event lists reused by every process() call (no params/events yet).
    emptyIn.ctx  = nullptr;
    emptyIn.size = [] (const clap_input_events*) -> uint32_t { return 0; };
    emptyIn.get  = [] (const clap_input_events*, uint32_t) -> const clap_event_header_t* { return nullptr; };

    emptyOut.ctx      = nullptr;
    emptyOut.try_push = [] (const clap_output_events*, const clap_event_header_t*) -> bool { return true; };
}

ClapInstance::~ClapInstance()
{
    deactivate();
    if (plugin != nullptr)
    {
        plugin->destroy (plugin);
        plugin = nullptr;
    }
}

bool ClapInstance::create (const ClapBundle& bundle, const std::string& pluginId,
                           std::string& errorOut)
{
    const auto* factory = bundle.getFactory();
    if (factory == nullptr || factory->create_plugin == nullptr)
    { errorOut = "no plugin factory"; return false; }
    owningBundle = &bundle;   // the .clap backing the plugin vtable must stay loaded

    plugin = factory->create_plugin (factory, hostObj.get(), pluginId.c_str());
    if (plugin == nullptr) { errorOut = "create_plugin returned null"; return false; }

    if (plugin->init == nullptr || ! plugin->init (plugin))
    {
        errorOut = "plugin init() failed";
        plugin->destroy (plugin);
        plugin = nullptr;
        return false;
    }

    // Channel counts of the first audio port in / out (the aux path is stereo;
    // a full port-config negotiation comes with the multi-port increment).
    inCh = outCh = 0;
    if (const auto* ap = static_cast<const clap_plugin_audio_ports_t*> (
            plugin->get_extension (plugin, CLAP_EXT_AUDIO_PORTS)))
    {
        clap_audio_port_info_t info {};
        if (ap->count (plugin, true)  > 0 && ap->get (plugin, 0, true,  &info)) inCh  = (int) info.channel_count;
        if (ap->count (plugin, false) > 0 && ap->get (plugin, 0, false, &info)) outCh = (int) info.channel_count;
    }

    // processStereo drives a single stereo in + stereo out port. Reject anything
    // else for now (the aux path is stereo); multi-port support is a later step.
    if (inCh != 2 || outCh != 2)
    {
        errorOut = "plugin is not stereo-in/stereo-out (got "
                 + std::to_string (inCh) + " in / " + std::to_string (outCh) + " out)";
        plugin->destroy (plugin);
        plugin = nullptr;
        owningBundle = nullptr;
        return false;
    }
    return true;
}

bool ClapInstance::activate (double sampleRate, int maxBlock, std::string& errorOut)
{
    if (plugin == nullptr) { errorOut = "not created"; return false; }
    if (active) return true;

    maxFrames = std::max (1, maxBlock);
    if (plugin->activate == nullptr
        || ! plugin->activate (plugin, sampleRate, 1, (uint32_t) maxFrames))
    { errorOut = "activate() failed"; return false; }

    active = true;
    inScratchL .assign ((size_t) maxFrames, 0.0f);
    inScratchR .assign ((size_t) maxFrames, 0.0f);
    outScratchL.assign ((size_t) maxFrames, 0.0f);
    outScratchR.assign ((size_t) maxFrames, 0.0f);
    return true;
}

void ClapInstance::deactivate()
{
    if (plugin != nullptr && active)
    {
        if (processing && plugin->stop_processing != nullptr)
            plugin->stop_processing (plugin);
        processing = false;
        if (plugin->deactivate != nullptr)
            plugin->deactivate (plugin);
    }
    active = false;
}

void ClapInstance::processStereo (const float* inL, const float* inR,
                                  float* outL, float* outR, int numFrames) noexcept
{
    if (! active || plugin == nullptr || plugin->process == nullptr
        || numFrames <= 0 || numFrames > maxFrames
        || inL == nullptr || outL == nullptr || outR == nullptr)
    {
        // Clear the outputs so a reused buffer can't leak stale audio on a no-op.
        if (numFrames > 0)
        {
            if (outL != nullptr) std::memset (outL, 0, sizeof (float) * (size_t) numFrames);
            if (outR != nullptr) std::memset (outR, 0, sizeof (float) * (size_t) numFrames);
        }
        return;
    }

    if (! processing)
    {
        // The calling thread is the audio thread for this instance.
        hostObj.setAudioThread (std::this_thread::get_id());
        if (plugin->start_processing != nullptr && ! plugin->start_processing (plugin))
        {
            std::memset (outL, 0, sizeof (float) * (size_t) numFrames);
            std::memset (outR, 0, sizeof (float) * (size_t) numFrames);
            return;
        }
        processing = true;
    }

    const auto n = (size_t) numFrames;
    std::memcpy (inScratchL.data(), inL, sizeof (float) * n);
    std::memcpy (inScratchR.data(), inR != nullptr ? inR : inL, sizeof (float) * n);

    float* inPtrs[2]  = { inScratchL.data(),  inScratchR.data()  };
    float* outPtrs[2] = { outScratchL.data(), outScratchR.data() };

    clap_audio_buffer_t inBuf {};
    inBuf.data32       = inPtrs;
    inBuf.channel_count = 2;

    clap_audio_buffer_t outBuf {};
    outBuf.data32       = outPtrs;
    outBuf.channel_count = 2;

    clap_process_t p {};
    p.steady_time         = -1;          // free-running
    p.frames_count        = (uint32_t) numFrames;
    p.transport           = nullptr;
    p.audio_inputs        = &inBuf;
    p.audio_inputs_count  = 1;
    p.audio_outputs       = &outBuf;
    p.audio_outputs_count = 1;
    p.in_events           = &emptyIn;
    p.out_events          = &emptyOut;

    const auto status = plugin->process (plugin, &p);
    if (status == CLAP_PROCESS_ERROR)
    {
        std::fill (outScratchL.begin(), outScratchL.begin() + numFrames, 0.0f);
        std::fill (outScratchR.begin(), outScratchR.begin() + numFrames, 0.0f);
    }

    std::memcpy (outL, outScratchL.data(), sizeof (float) * n);
    std::memcpy (outR, outScratchR.data(), sizeof (float) * n);
}
} // namespace duskstudio::clap
