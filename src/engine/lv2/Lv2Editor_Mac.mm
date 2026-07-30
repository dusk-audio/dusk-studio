#include "Lv2Editor.h"

// macOS LV2 editor. The suil Cocoa embed is not implemented: open() reports no
// embeddable UI, so an LV2 plugin loads and processes on macOS but has no
// editor. The Linux X11+suil implementation lives in Lv2Editor.cpp.
namespace duskstudio::lv2
{
namespace
{
constexpr const char* kNoCocoaUi = "LV2 plugin UIs are not embeddable on macOS yet";
} // namespace

struct Lv2Editor::Impl {};

Lv2Editor::Lv2Editor()  = default;
Lv2Editor::~Lv2Editor() = default;

bool Lv2Editor::open (Lv2Instance&, std::string& errorOut)
{
    errorOut = kNoCocoaUi;
    return false;
}

bool Lv2Editor::embed (unsigned long, int, int, int, int, std::string& errorOut)
{
    errorOut = kNoCocoaUi;
    return false;
}

void Lv2Editor::setBounds (int, int, int, int) {}
void Lv2Editor::reveal() {}
void Lv2Editor::hide() {}
void Lv2Editor::close() {}

bool Lv2Editor::getRootRelativePosition (unsigned long, int&, int&) const { return false; }
bool Lv2Editor::getActualGeometry (int&, int&, int&, int&) const { return false; }

void Lv2Editor::setLeakOnClose (bool) noexcept {}
void Lv2Editor::pump() {}

int  Lv2Editor::preferredWidth()  const noexcept { return 0; }
int  Lv2Editor::preferredHeight() const noexcept { return 0; }
bool Lv2Editor::isOpen()     const noexcept { return false; }
bool Lv2Editor::isEmbedded() const noexcept { return false; }
} // namespace duskstudio::lv2
