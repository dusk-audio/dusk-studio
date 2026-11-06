#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Hebrew and Arabic for a renderer that draws code points left to right and joins
// nothing: Arabic letters become the presentation forms that join them, then each line
// is put in display order by the Unicode Bidirectional Algorithm (UAX #9). For drawing
// only. Stored, typed and searched text keeps typing order.
namespace dusk::text
{
// True when shaping or reordering would change the text: it is valid UTF-8 and holds a
// right-to-left letter, an Arabic number or an explicit direction mark. Text with no
// code point at or above U+0590 returns false after one pass over its bytes.
bool needsDisplayOrder (std::string_view utf8) noexcept;

// Arabic letters as their isolated, initial, medial or final presentation forms, and
// lam followed by alef as the lam-alef ligature, in typing order. A letter the bundled
// face has no presentation form for passes through unchanged, as does invalid UTF-8.
std::string shapeArabic (std::string_view utf8);

// A line as a byte range of the text it belongs to.
struct LineRange
{
    std::size_t begin = 0;
    std::size_t end = 0;
};

// Each line in display order, joined by '\n'. Embedding levels are resolved over the
// paragraph holding the line, so a paragraph broken into lines in typing order reads
// as it would unbroken. Lines run in order and never cross a newline. Each paragraph
// takes its direction from its first strong character, and in a right-to-left run a
// mark stays after its base and a mirrored character such as a bracket is swapped
// for its mirror. Invalid UTF-8 comes back unchanged.
std::string visualOrder (std::string_view utf8, const std::vector<LineRange>& lines);

// visualOrder with each newline-separated line of the text as one line.
std::string visualOrder (std::string_view utf8);

// shapeArabic, then visualOrder. Text that needsDisplayOrder rejects comes back as is.
std::string displayOrder (std::string_view utf8);

// Every code point shapeArabic can emit in place of a letter, ascending, so a face can
// bake them.
const std::vector<char32_t>& arabicPresentationForms();
} // namespace dusk::text
