#pragma once

#include <DuskWidgets.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace duskstudio::imgui
{
// The form controls a settings panel needs and the console widget kit deliberately
// does not carry: a dropdown, a tick box, a horizontal slider, a section rule and
// the scrolling body they sit in. Dear ImGui draws them; this dresses them in the
// console palette so a settings row reads as part of the same desk.
//
// Every control takes its rectangle in screen pixels, the way the widget kit does,
// so a view lays a panel out with one set of coordinates rather than two.

// Pushes the console styling over Dear ImGui's own for as long as it is in scope.
// Every control below expects it.
class ScopedFormStyle final
{
public:
    explicit ScopedFormStyle (const DuskWidgets::Context& ctx);
    ~ScopedFormStyle();

    ScopedFormStyle (const ScopedFormStyle&) = delete;
    ScopedFormStyle& operator= (const ScopedFormStyle&) = delete;

private:
    int colours = 0;
    int vars = 0;
};

// A row caption, vertically centred in a row of `height`.
void formLabel (const DuskWidgets::Context& ctx, ImVec2 at, float width, float height,
                const char* text, DuskWidgets::Align align = DuskWidgets::Align::right);

// A section heading and the rule that closes the section off.
void formHeading (const DuskWidgets::Context& ctx, ImVec2 at, float width, float height,
                  const char* text);
void formRule (const DuskWidgets::Context& ctx, ImVec2 at, float width);

// A dropdown filling tl..br. `items` are NUL-terminated strings; `selected` indexes
// them, or is -1 for "nothing chosen". True when the user picked a different one.
// `openItems`, when given, is filled with each entry's screen rectangle (x0, y0, x1,
// y1) while the list is open and emptied while it is closed, for a scenario to click.
// `repick` reports a pick of the entry already selected as well, for a list whose
// entries are actions rather than a setting.
bool formCombo (DuskWidgets::Context& ctx, const char* id, ImVec2 tl, ImVec2 br,
                const char* const* items, int count, int& selected, bool enabled = true,
                std::vector<ImVec4>* openItems = nullptr, bool repick = false);

// The entry a scenario's "list:<n>" names in a list of `count`: empty for any other
// name, for n that is not a whole number, and for an entry the list does not have.
std::optional<std::size_t> formListEntry (const std::string& name, std::size_t count);

// Closes every list and menu open in the frame being drawn, as a press outside them
// would, unless a button is held in the panel. Called from a view's draw().
void formCloseLists();

// A dropdown whose items are built at runtime. The pointers formCombo needs cannot be
// taken while the list is still growing, so add() every entry and then finish() once.
struct ComboModel
{
    std::vector<std::string> labels;
    std::vector<const char*> items;
    int selected = -1;

    void clear();
    void add (std::string label);
    void finish (int selectedIndex);

    int count() const noexcept { return static_cast<int> (items.size()); }
    const std::string& label (int index) const;
};

bool formCombo (DuskWidgets::Context& ctx, const char* id, ImVec2 tl, ImVec2 br,
                ComboModel& model, bool enabled = true);

// A push button in the same chrome as the fields beside it.
bool formButton (DuskWidgets::Context& ctx, const char* id, ImVec2 tl, ImVec2 br,
                 const char* label);

// A tick box with its label to the right, vertically centred in a row of `height`.
// True when it was toggled.
bool formCheckbox (DuskWidgets::Context& ctx, const char* id, ImVec2 at, float height,
                   const char* label, bool& value);

struct FormSliderResult
{
    bool changed = false;    // the value moved this frame
    bool released = false;   // the drag or the keyboard edit finished
};

FormSliderResult formSlider (DuskWidgets::Context& ctx, const char* id, ImVec2 tl, ImVec2 br,
                             float& value, float minimum, float maximum, const char* format);

// Attaches a tooltip to the control submitted immediately before. Wrapped, because
// the panel's help text is prose rather than a caption.
void formTooltip (const char* text);
} // namespace duskstudio::imgui
