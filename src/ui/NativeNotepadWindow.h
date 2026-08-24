#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace duskstudio
{
class NativeNotepadWindow final
{
public:
    static constexpr std::uint32_t kPreferredWidth = 940;
    static constexpr std::uint32_t kPreferredHeight = 700;

    struct EmbeddedGeometry
    {
        int x = 0;
        int y = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        double scaleFactor = 1.0;
    };

    using TextChangedCallback = std::function<void (const std::string&, bool dirty)>;
    using ClosedCallback = std::function<void()>;
    using LinkOpenedCallback = std::function<void (const std::string&)>;

    NativeNotepadWindow();
    ~NativeNotepadWindow();

    void setCallbacks (TextChangedCallback textChanged, ClosedCallback closed,
                       LinkOpenedCallback linkOpened = {});
    // False when the embedded native child could not be created - no usable GL
    // context, or a display backend that cannot embed a foreign surface. The
    // window owns nothing afterwards, so isOpen() also reads false.
    bool open (std::uintptr_t nativeParent, EmbeddedGeometry geometry,
               const std::string& markdown,
               bool hasSessionFile, bool hasUnsavedChanges);
    // Why the last open() returned false, phrased for the user. Empty when
    // open() succeeded or was never called.
    const std::string& lastOpenFailure() const noexcept;
    void setEmbeddedGeometry (EmbeddedGeometry geometry);
    // Teardown is deferred over two event-pump ticks; close() only asks for it.
    void close();
    // True from a successful open() until the closed callback has run, so a
    // toggle during that deferred teardown cannot restart the window
    // underneath the pending close - and its save.
    bool isOpen() const noexcept;
    void markSaved();
    void markSaveFailed();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio
