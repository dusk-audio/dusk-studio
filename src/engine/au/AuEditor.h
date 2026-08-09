#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace duskstudio::au
{
class AuInstance;

// AUv2 Cocoa view factory embedded into the same owned NSView-container shape
// used by the H5a CLAP/VST3/LV2 editors. All Cocoa work is message-thread-only.
class AuEditor
{
public:
    AuEditor();
    ~AuEditor();
    AuEditor (const AuEditor&) = delete;
    AuEditor& operator= (const AuEditor&) = delete;

    bool open (AuInstance& instance, std::string& errorOut);
    bool embed (std::uintptr_t parentHandle, int x, int y, int width, int height,
                std::string& errorOut);
    void setBounds (int x, int y, int width, int height);
    void reveal();
    void hide();
    void abandonPlugin() noexcept;
    void close();
    void pump();

    int preferredWidth() const noexcept;
    int preferredHeight() const noexcept;
    bool isOpen() const noexcept;
    bool isEmbedded() const noexcept;

    std::function<void (int, int)> onResize;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio::au
