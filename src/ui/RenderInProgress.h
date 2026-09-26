#pragma once

namespace duskstudio
{
// A modal body whose render runs on a worker thread that owns the transport
// and the audio callback until it stops: bounce, mixdown, mastering export and
// freeze. Anything that would stop the transport or detach audio from the
// message thread has to cancel the render through here and wait for it first.
struct RenderInProgress
{
    virtual ~RenderInProgress() = default;
    virtual bool isRenderRunning() const = 0;
    // Running, or stopped with its result not yet taken in on the message
    // thread (a freeze commits on the dialog's next tick). What a quit waits on.
    virtual bool hasUnfinishedRender() const = 0;
    // What the dialog's Cancel does mid-render. Returns at once: the worker
    // stops at its next block and hands the engine back on the message thread,
    // so the caller must not block that thread waiting for it.
    virtual void cancelRender() = 0;
};
} // namespace duskstudio
