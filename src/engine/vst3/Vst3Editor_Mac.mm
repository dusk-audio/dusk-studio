#include "Vst3Editor.h"

// macOS VST3 editor. The kPlatformTypeNSView attach is not implemented: open()
// reports no embeddable view, so a VST3 plugin loads and processes on macOS but
// has no editor. The Linux X11 implementation lives in Vst3Editor.cpp.
namespace duskstudio::vst3
{
namespace
{
constexpr const char* kNoCocoaView = "VST3 plugin editors are not embeddable on macOS yet";
} // namespace

struct Vst3Editor::Impl {};

Vst3Editor::Vst3Editor()  = default;
Vst3Editor::~Vst3Editor() = default;

bool Vst3Editor::open (Vst3Instance&, std::string& errorOut)
{
    errorOut = kNoCocoaView;
    return false;
}

bool Vst3Editor::embed (unsigned long, int, int, int, int, std::string& errorOut)
{
    errorOut = kNoCocoaView;
    return false;
}

void Vst3Editor::setBounds (int, int, int, int) {}
void Vst3Editor::setContentScale (float) {}
void Vst3Editor::reveal() {}
void Vst3Editor::hide() {}
void Vst3Editor::close() {}

bool Vst3Editor::getRootRelativePosition (unsigned long, int&, int&) const { return false; }
bool Vst3Editor::getActualGeometry (int&, int&, int&, int&) const { return false; }

void Vst3Editor::pump (double) {}

int  Vst3Editor::preferredWidth()  const noexcept { return 0; }
int  Vst3Editor::preferredHeight() const noexcept { return 0; }
bool Vst3Editor::isOpen()     const noexcept { return false; }
bool Vst3Editor::isEmbedded() const noexcept { return false; }
} // namespace duskstudio::vst3
