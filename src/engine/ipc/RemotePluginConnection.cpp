#include "RemotePluginConnection.h"

#include "platform/IpcSync.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace duskstudio::ipc
{
namespace
{
// Send a control request: [header][payload]. Returns true on success.
bool sendControl (platform::NativeHandle& ch, OpCode op,
                    const void* payload, std::uint32_t payloadLen) noexcept
{
    ControlMsgHeader hdr {};
    hdr.totalLen   = (std::uint32_t) sizeof (hdr) + payloadLen;
    hdr.op         = (std::uint32_t) op;
    hdr.status     = 0;
    hdr.payloadLen = payloadLen;
    if (! platform::writeExact (ch, &hdr, sizeof (hdr))) return false;
    if (payloadLen > 0 && ! platform::writeExact (ch, payload, payloadLen))
        return false;
    return true;
}

bool recvControl (platform::NativeHandle& ch,
                    ControlMsgHeader& hdrOut,
                    std::vector<std::uint8_t>& payloadOut) noexcept
{
    if (! platform::readExact (ch, &hdrOut, sizeof (hdrOut))) return false;
    payloadOut.resize (hdrOut.payloadLen);
    if (hdrOut.payloadLen > 0
        && ! platform::readExact (ch, payloadOut.data(), hdrOut.payloadLen))
        return false;
    return true;
}
} // namespace

RemotePluginConnection::~RemotePluginConnection()
{
    disconnect();
}

bool RemotePluginConnection::connect (const std::string& hostExecutablePath,
                                        const std::string& extraArg,
                                        std::string& errorOut)
{
    if (child.isAlive())
        return true;

    if (! shm.createAnonymous ("dusk-studio-plugin-shm", kTotalSize, errorOut))
        return false;

    auto* hdr = headerOf (shm.data());
    new (hdr) BlockHeader();
    hdr->magic   = kMagic;
    hdr->version = kVersion;
    hdr->cmdSeq.store (0, std::memory_order_relaxed);
    hdr->replySeq.store (0, std::memory_order_relaxed);
    hdr->state.store (kStateReady, std::memory_order_relaxed);

    platform::ChannelPair pair;
    if (! platform::createChannelPair (pair, errorOut))
    {
        disconnect();
        return false;
    }

    std::vector<std::string> args { extraArg };
    if (! child.spawn (hostExecutablePath, args, pair.childEnd, errorOut))
    {
        platform::closeHandle (pair.parentEnd);
        platform::closeHandle (pair.childEnd);
        disconnect();
        return false;
    }

    controlChannel = pair.parentEnd;

    if (! platform::sendHandle (controlChannel, shm.handle()))
    {
        errorOut = std::string ("sendHandle failed: ") + std::strerror (errno);
        disconnect();
        return false;
    }

    platform::setReadTimeout (controlChannel, 5000);
    char ack = 0;
    if (! platform::readExact (controlChannel, &ack, 1) || ack != 'k')
    {
        errorOut = "child did not send ready handshake";
        disconnect();
        return false;
    }

    return true;
}

bool RemotePluginConnection::processBlockSync (const float* const* inChannels,
                                                 int numIn, int numOut, int numSamples,
                                                 juce::MidiBuffer& midi,
                                                 long long timeoutNs) noexcept
{
    if (! shm.isMapped() || crashed.load (std::memory_order_acquire))
        return false;

    if (numSamples <= 0 || numSamples > kMaxBlock) return false;
    if (numIn     <  0 || numIn      > kMaxChans) return false;
    if (numOut    <  0 || numOut     > kMaxChans) return false;

    auto* hdr = headerOf (shm.data());

    for (int c = 0; c < numIn; ++c)
    {
        if (inChannels[c] != nullptr)
            std::memcpy (audioInChannel (shm.data(), c), inChannels[c],
                          (std::size_t) numSamples * sizeof (float));
    }

    {
        std::uint8_t* out = midiIn (shm.data());
        std::uint32_t written = 0;
        for (const auto meta : midi)
        {
            const auto m = meta.getMessage();
            const int len = m.getRawDataSize();
            if (len <= 0) continue;
            if (written + 4 + 2 + (std::uint32_t) len > kMidiBytes) break;
            const int sample = meta.samplePosition;
            std::memcpy (out + written, &sample, 4);             written += 4;
            const std::uint16_t l16 = (std::uint16_t) len;
            std::memcpy (out + written, &l16, 2);                written += 2;
            std::memcpy (out + written, m.getRawData(),
                         (std::size_t) len);                      written += (std::uint32_t) len;
        }
        hdr->midiInBytes = written;
    }

    hdr->numSamples  = (std::uint32_t) numSamples;
    hdr->numInChans  = (std::uint32_t) numIn;
    hdr->numOutChans = (std::uint32_t) numOut;
    hdr->midiOutBytes = 0;

    const std::uint32_t mySeq = ++localSeq;
    hdr->cmdSeq.store (mySeq, std::memory_order_release);
    platform::wakeOneAddress (&hdr->cmdSeq);

    auto deserialiseMidiOut = [&]
    {
        midi.clear();
        const std::uint32_t midiOutBytes = hdr->midiOutBytes;
        if (midiOutBytes == 0 || midiOutBytes > kMidiBytes) return;
        const std::uint8_t* base = midiOut (shm.data());
        std::uint32_t off = 0;
        std::uint8_t evBuf[256];
        while (off + 6 <= midiOutBytes)
        {
            int sample = 0;
            std::memcpy (&sample, base + off, 4); off += 4;
            std::uint16_t l16 = 0;
            std::memcpy (&l16, base + off, 2); off += 2;
            const int eventLen = (int) l16;
            if (eventLen <= 0 || eventLen > (int) sizeof (evBuf)) break;
            if (off + (std::uint32_t) eventLen > midiOutBytes)   break;
            std::memcpy (evBuf, base + off, (std::size_t) eventLen);
            off += (std::uint32_t) eventLen;
            midi.addEvent (juce::MidiMessage (evBuf, eventLen), sample);
        }
    };

    constexpr int kSpinIters = 2000;
    for (int i = 0; i < kSpinIters; ++i)
    {
        if (hdr->replySeq.load (std::memory_order_acquire) == mySeq)
        {
            deserialiseMidiOut();
            roundTrips.fetch_add (1, std::memory_order_relaxed);
            return true;
        }
        platform::cpuRelax();
    }

    const auto deadline = platform::deadlineFromNow (timeoutNs);
    for (;;)
    {
        const auto seen = hdr->replySeq.load (std::memory_order_acquire);
        if (seen == mySeq) break;
        // Fast crash detection: the child publishes kStateCrashed
        // when its handler trips before getting a chance to bump
        // replySeq. Without this check the parent would wait the
        // full futex deadline before noticing.
        if (hdr->state.load (std::memory_order_acquire) == kStateCrashed)
        {
            crashed.store (true, std::memory_order_release);
            return false;
        }
        // Pass the CURRENT replySeq as the "expected" value for the
        // futex. After a prior call's timeout, replySeq can be stuck
        // at an older value (e.g. mySeq-2); a hardcoded `mySeq-1`
        // would mismatch and cause waitOnAddress to spin-return
        // ValueChanged until the deadline. Reading the live value
        // makes the wait correctly block until the child actually
        // bumps replySeq.
        const auto r = platform::waitOnAddress (&hdr->replySeq, seen, &deadline);
        if (r == platform::WaitResult::Timeout)
        {
            crashed.store (true, std::memory_order_release);
            return false;
        }
        if (r == platform::WaitResult::Error)
        {
            crashed.store (true, std::memory_order_release);
            return false;
        }
        // Awoken / ValueChanged / Interrupted: re-loop and re-check.
    }

    deserialiseMidiOut();
    roundTrips.fetch_add (1, std::memory_order_relaxed);
    return true;
}

bool RemotePluginConnection::ping (int timeoutMs, std::string& errorOut)
{
    if (! platform::isValid (controlChannel)) { errorOut = "not connected"; return false; }

    platform::setReadTimeout (controlChannel, timeoutMs);

    if (! sendControl (controlChannel, OpCode::Ping, nullptr, 0))
    {
        errorOut = std::string ("Ping write failed: ") + std::strerror (errno);
        return false;
    }
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> payload;
    if (! recvControl (controlChannel, hdr, payload))
    {
        errorOut = std::string ("Ping read failed: ") + std::strerror (errno);
        return false;
    }
    if (hdr.status != 0)
    {
        errorOut = "Ping reply status != 0";
        return false;
    }
    return true;
}

bool RemotePluginConnection::loadPlugin (const std::string& pluginDescriptionXml,
                                          double sampleRate, int blockSize,
                                          int& numInOut, int& numOutOut,
                                          int& latencyOut, std::string& errorOut)
{
    if (! platform::isValid (controlChannel)) { errorOut = "not connected"; return false; }

    PrepareToPlayPayload header {};
    header.sampleRate = sampleRate;
    header.blockSize  = (std::int32_t) blockSize;
    header.reserved   = 0;

    std::vector<std::uint8_t> payload;
    payload.resize (sizeof (header) + pluginDescriptionXml.size());
    std::memcpy (payload.data(), &header, sizeof (header));
    std::memcpy (payload.data() + sizeof (header),
                  pluginDescriptionXml.data(), pluginDescriptionXml.size());

    platform::setReadTimeout (controlChannel, 30000);

    if (! sendControl (controlChannel, OpCode::LoadPlugin,
                        payload.data(), (std::uint32_t) payload.size()))
    {
        errorOut = std::string ("LoadPlugin write failed: ") + std::strerror (errno);
        return false;
    }

    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! recvControl (controlChannel, hdr, reply))
    {
        errorOut = std::string ("LoadPlugin read failed: ") + std::strerror (errno);
        return false;
    }
    if (hdr.status != 0)
    {
        errorOut = reply.empty()
                     ? "LoadPlugin failed (no message)"
                     : std::string (reinterpret_cast<const char*> (reply.data()),
                                      reply.size());
        return false;
    }
    if (reply.size() < sizeof (LoadPluginReply))
    {
        errorOut = "LoadPlugin reply too small";
        return false;
    }
    LoadPluginReply r {};
    std::memcpy (&r, reply.data(), sizeof (r));
    numInOut    = r.numInChans;
    numOutOut   = r.numOutChans;
    latencyOut  = r.latencySamples;
    return true;
}

bool RemotePluginConnection::prepareToPlay (double sampleRate, int blockSize,
                                              std::string& errorOut)
{
    if (! platform::isValid (controlChannel)) { errorOut = "not connected"; return false; }
    PrepareToPlayPayload p {};
    p.sampleRate = sampleRate;
    p.blockSize  = (std::int32_t) blockSize;
    if (! sendControl (controlChannel, OpCode::PrepareToPlay, &p, sizeof (p)))
    {
        errorOut = std::string ("PrepareToPlay write failed: ") + std::strerror (errno);
        return false;
    }
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! recvControl (controlChannel, hdr, reply))
    {
        errorOut = std::string ("PrepareToPlay read failed: ") + std::strerror (errno);
        return false;
    }
    if (hdr.status != 0) { errorOut = "PrepareToPlay status != 0"; return false; }
    return true;
}

bool RemotePluginConnection::release (std::string& errorOut)
{
    if (! platform::isValid (controlChannel)) { errorOut = "not connected"; return false; }
    if (! sendControl (controlChannel, OpCode::Release, nullptr, 0))
    {
        errorOut = std::string ("Release write failed: ") + std::strerror (errno);
        return false;
    }
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! recvControl (controlChannel, hdr, reply))
    {
        errorOut = std::string ("Release read failed: ") + std::strerror (errno);
        return false;
    }
    if (hdr.status != 0) { errorOut = "Release status != 0"; return false; }
    return true;
}

bool RemotePluginConnection::getState (std::vector<std::uint8_t>& blobOut,
                                         std::string& errorOut)
{
    if (! platform::isValid (controlChannel)) { errorOut = "not connected"; return false; }
    if (! sendControl (controlChannel, OpCode::GetState, nullptr, 0))
    {
        errorOut = std::string ("GetState write failed: ") + std::strerror (errno);
        return false;
    }
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! recvControl (controlChannel, hdr, reply))
    {
        errorOut = std::string ("GetState read failed: ") + std::strerror (errno);
        return false;
    }
    if (hdr.status != 0) { errorOut = "GetState status != 0"; return false; }
    if (reply.size() != sizeof (std::uint32_t))
    {
        errorOut = "GetState reply expected uint32 size";
        return false;
    }
    std::uint32_t blobSize = 0;
    std::memcpy (&blobSize, reply.data(), sizeof (blobSize));
    if (blobSize > kStateBytes) { errorOut = "state blob exceeds staging area"; return false; }
    blobOut.assign (
        reinterpret_cast<const std::uint8_t*> (shm.data()) + kStateOffset,
        reinterpret_cast<const std::uint8_t*> (shm.data()) + kStateOffset + blobSize);
    return true;
}

bool RemotePluginConnection::setState (const std::uint8_t* data, std::size_t size,
                                         std::string& errorOut)
{
    if (! platform::isValid (controlChannel)) { errorOut = "not connected"; return false; }
    if (size > kStateBytes)
    {
        errorOut = "state blob exceeds staging area";
        return false;
    }
    std::memcpy (static_cast<char*> (shm.data()) + kStateOffset, data, size);
    const std::uint32_t sz = (std::uint32_t) size;
    if (! sendControl (controlChannel, OpCode::SetState, &sz, sizeof (sz)))
    {
        errorOut = std::string ("SetState write failed: ") + std::strerror (errno);
        return false;
    }
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! recvControl (controlChannel, hdr, reply))
    {
        errorOut = std::string ("SetState read failed: ") + std::strerror (errno);
        return false;
    }
    if (hdr.status != 0) { errorOut = "SetState status != 0"; return false; }
    return true;
}

bool RemotePluginConnection::showEditor (std::uint64_t& windowIdOut,
                                            int& widthOut, int& heightOut,
                                            std::string& errorOut)
{
    windowIdOut = 0; widthOut = 0; heightOut = 0;
    if (! platform::isValid (controlChannel)) { errorOut = "not connected"; return false; }
    if (! sendControl (controlChannel, OpCode::ShowEditor, nullptr, 0))
    {
        errorOut = std::string ("ShowEditor write failed: ") + std::strerror (errno);
        return false;
    }
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! recvControl (controlChannel, hdr, reply))
    {
        errorOut = std::string ("ShowEditor read failed: ") + std::strerror (errno);
        return false;
    }
    if (hdr.status != 0) { errorOut = "ShowEditor status != 0"; return false; }
    if (reply.size() != sizeof (ShowEditorReply))
    {
        errorOut = "ShowEditor reply size mismatch";
        return false;
    }
    ShowEditorReply r {};
    std::memcpy (&r, reply.data(), sizeof (r));
    windowIdOut = r.windowId;
    widthOut    = (int) r.width;
    heightOut   = (int) r.height;
    return true;
}

bool RemotePluginConnection::hideEditor (std::string& errorOut)
{
    if (! platform::isValid (controlChannel)) { errorOut = "not connected"; return false; }
    if (! sendControl (controlChannel, OpCode::HideEditor, nullptr, 0))
    {
        errorOut = std::string ("HideEditor write failed: ") + std::strerror (errno);
        return false;
    }
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! recvControl (controlChannel, hdr, reply))
    {
        errorOut = std::string ("HideEditor read failed: ") + std::strerror (errno);
        return false;
    }
    if (hdr.status != 0) { errorOut = "HideEditor status != 0"; return false; }
    return true;
}

bool RemotePluginConnection::resizeEditor (int width, int height, std::string& errorOut)
{
    if (! platform::isValid (controlChannel)) { errorOut = "not connected"; return false; }
    ResizeEditorPayload p {};
    p.width  = (std::int32_t) width;
    p.height = (std::int32_t) height;
    if (! sendControl (controlChannel, OpCode::ResizeEditor, &p, sizeof (p)))
    {
        errorOut = std::string ("ResizeEditor write failed: ") + std::strerror (errno);
        return false;
    }
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! recvControl (controlChannel, hdr, reply))
    {
        errorOut = std::string ("ResizeEditor read failed: ") + std::strerror (errno);
        return false;
    }
    if (hdr.status != 0) { errorOut = "ResizeEditor status != 0"; return false; }
    return true;
}

bool RemotePluginConnection::pollReaper() noexcept
{
    if (! child.isAlive()) return false;
    if (! child.pollExit()) return false;
    crashed.store (true, std::memory_order_release);
    return true;
}

void RemotePluginConnection::disconnect()
{
    if (child.isAlive())
    {
        if (shm.isMapped())
        {
            auto* hdr = headerOf (shm.data());
            hdr->state.store (kStateTeardown, std::memory_order_release);
            platform::wakeOneAddress (&hdr->cmdSeq);
        }
        child.terminate (500);
    }
    platform::closeHandle (controlChannel);
    shm.close();
}

} // namespace duskstudio::ipc
