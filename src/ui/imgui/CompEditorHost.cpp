#include "CompEditorHost.h"
#include "../../dsp/MultiCompStage.h"
#include "../../engine/builtin/DafPlugin.h"
#include "../../foundation/MessageThread.h"

#include <cmath>
#include <utility>

namespace duskstudio::imgui
{
namespace
{
bool switchTurnedOff (CompParamId id, float value) noexcept
{
    const auto& spec = compParamSpec (id);
    return spec.integer && spec.hostMax - spec.hostMin < 1.5f && value < spec.hostMin + 0.5f;
}

int indexOfSymbol (MultiCompStage& stage, const std::string& symbol)
{
    const auto& params = stage.plugin().params();
    for (std::size_t i = 0; i < params.size(); ++i)
        if (params[i].symbol == symbol)
            return (int) i;
    return -1;
}
} // namespace

float compEditorValue (const MultiCompStage& stage, const CompEditorTarget& target, int index) noexcept
{
    const int mode = target.stripMode != nullptr ? target.stripMode->load (std::memory_order_relaxed) : 0;
    return stage.sessionValue (*target.comp, target.engaged->load (std::memory_order_relaxed), mode, index);
}

void compEditorEdit (const MultiCompStage& stage, const CompEditorTarget& target,
                     int index, float value) noexcept
{
    stage.setSessionValue (*target.comp, *target.engaged, target.stripMode, index, value);
    if (stage.isEngageParam (index))
        return;
    if (stage.isModeParam (index))
    {
        if (target.stripMode != nullptr && target.modePicked != nullptr && std::isfinite (value)
            && target.stripMode->load (std::memory_order_relaxed) == (int) std::lround (value))
            target.modePicked->store (true, std::memory_order_relaxed);
        return;
    }
    const auto id = stage.sessionParam (index);
    if (! id || ! target.editsEngage)
        return;
    const bool engages = target.engagedOnlyBy ? *id == *target.engagedOnlyBy
                                              : ! switchTurnedOff (*id, value);
    if (engages)
        target.engaged->store (true, std::memory_order_relaxed);
}

struct CompEditorHost::Impl final : private dusk::Timer
{
    Impl (std::function<MultiCompStage&()> running, CompEditorTarget bound)
        : runningStage (std::move (running)), target (bound),
          host ("comp-editor", "The compressor editor", firstFrameMarkerPath ("comp-editor"))
    {
        // Every call reads the stage the open editor was built from, never the
        // running one, so the values and the meters always come from one instance.
        DafEditorHost::Unit unit;
        unit.createEditor = [this] (std::uintptr_t parent, std::uint32_t width, std::uint32_t height,
                                    double scale, builtin::DafEditorCallbacks editorCallbacks,
                                    std::string& error) -> std::unique_ptr<builtin::DafEditor>
        {
            if (shown == nullptr)
            {
                error = "no compressor is bound.";
                return nullptr;
            }
            return shown->plugin().createEditor (parent, width, height, scale, std::move (editorCallbacks), error);
        };
        unit.paramCount = [this] { return shown != nullptr ? (int) shown->plugin().params().size() : 0; };
        unit.paramValue = [this] (int index)
            { return shown != nullptr ? compEditorValue (*shown, target, index) : 0.0f; };
        unit.setParam = [this] (int index, float value)
        {
            if (shown != nullptr)
                compEditorEdit (*shown, target, index, value);
        };
        host.setUnit (std::move (unit));
    }

    ~Impl() override
    {
        stopTimer();
        host.shutdown();
        show (nullptr);
    }

    // The stage the editor draws keeps the session's values in its plug-in,
    // which the editor reads back, for as long as the editor is up.
    void show (MultiCompStage* stage)
    {
        if (stage == shown) return;
        if (shown != nullptr) shown->setEditorOpen (false);
        shown = stage;
        if (shown != nullptr) shown->setEditorOpen (true);
    }

    void setCallbacks (DafEditorHost::Callbacks given)
    {
        callbacks = std::move (given);
        DafEditorHost::Callbacks wrapped;
        wrapped.closed = [this]
        {
            forget();
            if (callbacks.closed) callbacks.closed();
        };
        wrapped.gestureEnded = [this] { if (callbacks.gestureEnded) callbacks.gestureEnded(); };
        wrapped.geometry = [this]
        {
            return callbacks.geometry ? callbacks.geometry() : DafEditorHost::Geometry {};
        };
        host.setCallbacks (std::move (wrapped));
    }

    bool open (std::uintptr_t parent, DafEditorHost::Geometry geometry)
    {
        stopTimer();
        closing = false;
        show (&runningStage());
        if (! host.open (parent, geometry))
        {
            show (nullptr);
            return false;
        }
        nativeParent = parent;
        startTimerHz (10);
        return true;
    }

    void close()
    {
        if (! host.isOpen()) return;
        closing = true;
        stopTimer();
        host.close();
    }

    void shutdown()
    {
        host.shutdown();
        forget();
    }

    void forget()
    {
        stopTimer();
        show (nullptr);
        closing = false;
    }

    // A rebuild keeps the dim and everything else the popup put up: only a
    // rebuild that cannot open reports the editor closed.
    void timerCallback() override
    {
        if (closing || shown == nullptr || ! host.isOpen()) return;
        auto& running = runningStage();
        if (&running == shown) return;
        show (&running);
        const auto geometry = callbacks.geometry ? callbacks.geometry() : DafEditorHost::Geometry {};
        if (host.open (nativeParent, geometry)) return;
        forget();
        if (callbacks.closed) callbacks.closed();
    }

    std::function<MultiCompStage&()> runningStage;
    CompEditorTarget target;
    DafEditorHost host;
    DafEditorHost::Callbacks callbacks;
    MultiCompStage* shown = nullptr;
    std::uintptr_t nativeParent = 0;
    bool closing = false;
};

CompEditorHost::CompEditorHost (std::function<MultiCompStage&()> runningStage, CompEditorTarget target)
    : impl (std::make_unique<Impl> (std::move (runningStage), target))
{
}

CompEditorHost::~CompEditorHost() = default;

void CompEditorHost::setCallbacks (DafEditorHost::Callbacks callbacks)
{
    impl->setCallbacks (std::move (callbacks));
}

std::uint32_t CompEditorHost::designWidth() const { return impl->runningStage().plugin().editorWidth(); }

std::uint32_t CompEditorHost::designHeight() const { return impl->runningStage().plugin().editorHeight(); }

bool CompEditorHost::open (std::uintptr_t nativeParent, DafEditorHost::Geometry geometry)
{
    return impl->open (nativeParent, geometry);
}

const std::string& CompEditorHost::lastOpenFailure() const noexcept { return impl->host.lastOpenFailure(); }

void CompEditorHost::close() { impl->close(); }

void CompEditorHost::shutdown() { impl->shutdown(); }

bool CompEditorHost::isOpen() const noexcept { return impl->host.isOpen(); }

bool CompEditorHost::hasRenderedFrame() const noexcept { return impl->host.hasRenderedFrame(); }

std::uintptr_t CompEditorHost::nativeWindow() const noexcept { return impl->host.nativeWindow(); }

const MultiCompStage* CompEditorHost::shownStage() const noexcept
{
    return impl->host.isOpen() ? impl->shown : nullptr;
}

bool CompEditorHost::editForScenario (const std::string& symbol, float value)
{
    if (impl->shown == nullptr) return false;
    const int index = indexOfSymbol (*impl->shown, symbol);
    return index >= 0 && impl->host.editForScenario (index, value);
}

std::optional<float> CompEditorHost::shownValueForScenario (const std::string& symbol) const
{
    if (impl->shown == nullptr) return std::nullopt;
    const int index = indexOfSymbol (*impl->shown, symbol);
    return index >= 0 ? impl->host.shownValueForScenario (index) : std::nullopt;
}
} // namespace duskstudio::imgui
