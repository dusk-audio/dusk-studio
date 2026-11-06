#include "BidiText.h"

#include <DuskSheenBidi.h>

#include <algorithm>
#include <iterator>

namespace dusk::text
{
namespace
{
bool decode (std::string_view utf8, std::size_t& at, char32_t& codepoint) noexcept
{
    const auto byte = [&] (std::size_t i) { return static_cast<unsigned char> (utf8[i]); };
    const auto lead = byte (at);
    if (lead < 0x80)
    {
        codepoint = lead;
        ++at;
        return true;
    }
    std::size_t length = 0;
    char32_t minimum = 0;
    if (lead >= 0xc2 && lead <= 0xdf) { length = 2; minimum = 0x80; codepoint = lead & 0x1fu; }
    else if (lead >= 0xe0 && lead <= 0xef) { length = 3; minimum = 0x800; codepoint = lead & 0x0fu; }
    else if (lead >= 0xf0 && lead <= 0xf4) { length = 4; minimum = 0x10000; codepoint = lead & 0x07u; }
    else return false;
    if (utf8.size() - at < length) return false;
    for (std::size_t i = 1; i < length; ++i)
    {
        if ((byte (at + i) & 0xc0) != 0x80) return false;
        codepoint = (codepoint << 6) | (byte (at + i) & 0x3fu);
    }
    if (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
    at += length;
    return true;
}

void append (std::string& out, char32_t c)
{
    if (c < 0x80)
        out += static_cast<char> (c);
    else if (c < 0x800)
    {
        out += static_cast<char> (0xc0 | (c >> 6));
        out += static_cast<char> (0x80 | (c & 0x3f));
    }
    else if (c < 0x10000)
    {
        out += static_cast<char> (0xe0 | (c >> 12));
        out += static_cast<char> (0x80 | ((c >> 6) & 0x3f));
        out += static_cast<char> (0x80 | (c & 0x3f));
    }
    else
    {
        out += static_cast<char> (0xf0 | (c >> 18));
        out += static_cast<char> (0x80 | ((c >> 12) & 0x3f));
        out += static_cast<char> (0x80 | ((c >> 6) & 0x3f));
        out += static_cast<char> (0x80 | (c & 0x3f));
    }
}

struct Decoded
{
    char32_t codepoint;
    std::size_t begin;
    std::size_t end;
};

bool isValid (std::string_view utf8) noexcept
{
    char32_t c = 0;
    for (std::size_t at = 0; at < utf8.size();)
        if (! decode (utf8, at, c)) return false;
    return true;
}

bool decodeAll (std::string_view utf8, std::vector<Decoded>& out)
{
    for (std::size_t at = 0; at < utf8.size();)
    {
        const auto begin = at;
        char32_t c = 0;
        if (! decode (utf8, at, c)) return false;
        out.push_back ({ c, begin, at });
    }
    return true;
}

// The forms of every letter the bundled DejaVu Sans draws a presentation form for,
// from Arabic Presentation Forms-A and -B, zero where Unicode encodes none.
struct ArabicForms
{
    char32_t letter, isolated, final, initial, medial;
};

constexpr ArabicForms kForms[] = {
    { 0x0621, 0xfe80, 0, 0, 0 },
    { 0x0622, 0xfe81, 0xfe82, 0, 0 },
    { 0x0623, 0xfe83, 0xfe84, 0, 0 },
    { 0x0624, 0xfe85, 0xfe86, 0, 0 },
    { 0x0625, 0xfe87, 0xfe88, 0, 0 },
    { 0x0626, 0xfe89, 0xfe8a, 0xfe8b, 0xfe8c },
    { 0x0627, 0xfe8d, 0xfe8e, 0, 0 },
    { 0x0628, 0xfe8f, 0xfe90, 0xfe91, 0xfe92 },
    { 0x0629, 0xfe93, 0xfe94, 0, 0 },
    { 0x062a, 0xfe95, 0xfe96, 0xfe97, 0xfe98 },
    { 0x062b, 0xfe99, 0xfe9a, 0xfe9b, 0xfe9c },
    { 0x062c, 0xfe9d, 0xfe9e, 0xfe9f, 0xfea0 },
    { 0x062d, 0xfea1, 0xfea2, 0xfea3, 0xfea4 },
    { 0x062e, 0xfea5, 0xfea6, 0xfea7, 0xfea8 },
    { 0x062f, 0xfea9, 0xfeaa, 0, 0 },
    { 0x0630, 0xfeab, 0xfeac, 0, 0 },
    { 0x0631, 0xfead, 0xfeae, 0, 0 },
    { 0x0632, 0xfeaf, 0xfeb0, 0, 0 },
    { 0x0633, 0xfeb1, 0xfeb2, 0xfeb3, 0xfeb4 },
    { 0x0634, 0xfeb5, 0xfeb6, 0xfeb7, 0xfeb8 },
    { 0x0635, 0xfeb9, 0xfeba, 0xfebb, 0xfebc },
    { 0x0636, 0xfebd, 0xfebe, 0xfebf, 0xfec0 },
    { 0x0637, 0xfec1, 0xfec2, 0xfec3, 0xfec4 },
    { 0x0638, 0xfec5, 0xfec6, 0xfec7, 0xfec8 },
    { 0x0639, 0xfec9, 0xfeca, 0xfecb, 0xfecc },
    { 0x063a, 0xfecd, 0xfece, 0xfecf, 0xfed0 },
    { 0x0641, 0xfed1, 0xfed2, 0xfed3, 0xfed4 },
    { 0x0642, 0xfed5, 0xfed6, 0xfed7, 0xfed8 },
    { 0x0643, 0xfed9, 0xfeda, 0xfedb, 0xfedc },
    { 0x0644, 0xfedd, 0xfede, 0xfedf, 0xfee0 },
    { 0x0645, 0xfee1, 0xfee2, 0xfee3, 0xfee4 },
    { 0x0646, 0xfee5, 0xfee6, 0xfee7, 0xfee8 },
    { 0x0647, 0xfee9, 0xfeea, 0xfeeb, 0xfeec },
    { 0x0648, 0xfeed, 0xfeee, 0, 0 },
    { 0x0649, 0xfeef, 0xfef0, 0xfbe8, 0xfbe9 },
    { 0x064a, 0xfef1, 0xfef2, 0xfef3, 0xfef4 },
    { 0x0679, 0xfb66, 0xfb67, 0xfb68, 0xfb69 },
    { 0x067a, 0xfb5e, 0xfb5f, 0xfb60, 0xfb61 },
    { 0x067b, 0xfb52, 0xfb53, 0xfb54, 0xfb55 },
    { 0x067e, 0xfb56, 0xfb57, 0xfb58, 0xfb59 },
    { 0x067f, 0xfb62, 0xfb63, 0xfb64, 0xfb65 },
    { 0x0680, 0xfb5a, 0xfb5b, 0xfb5c, 0xfb5d },
    { 0x0683, 0xfb76, 0xfb77, 0xfb78, 0xfb79 },
    { 0x0684, 0xfb72, 0xfb73, 0xfb74, 0xfb75 },
    { 0x0686, 0xfb7a, 0xfb7b, 0xfb7c, 0xfb7d },
    { 0x0687, 0xfb7e, 0xfb7f, 0xfb80, 0xfb81 },
    { 0x0688, 0xfb88, 0xfb89, 0, 0 },
    { 0x068c, 0xfb84, 0xfb85, 0, 0 },
    { 0x068d, 0xfb82, 0xfb83, 0, 0 },
    { 0x068e, 0xfb86, 0xfb87, 0, 0 },
    { 0x0691, 0xfb8c, 0xfb8d, 0, 0 },
    { 0x0698, 0xfb8a, 0xfb8b, 0, 0 },
    { 0x06a4, 0xfb6a, 0xfb6b, 0xfb6c, 0xfb6d },
    { 0x06a6, 0xfb6e, 0xfb6f, 0xfb70, 0xfb71 },
    { 0x06a9, 0xfb8e, 0xfb8f, 0xfb90, 0xfb91 },
    { 0x06ad, 0xfbd3, 0xfbd4, 0xfbd5, 0xfbd6 },
    { 0x06af, 0xfb92, 0xfb93, 0xfb94, 0xfb95 },
    { 0x06b1, 0xfb9a, 0xfb9b, 0xfb9c, 0xfb9d },
    { 0x06b3, 0xfb96, 0xfb97, 0xfb98, 0xfb99 },
    { 0x06ba, 0xfb9e, 0xfb9f, 0, 0 },
    { 0x06bb, 0xfba0, 0xfba1, 0xfba2, 0xfba3 },
    { 0x06be, 0xfbaa, 0xfbab, 0xfbac, 0xfbad },
    { 0x06c6, 0xfbd9, 0xfbda, 0, 0 },
    { 0x06cc, 0xfbfc, 0xfbfd, 0xfbfe, 0xfbff },
};

// Lam followed by these alefs: isolated and final ligature.
struct LamAlef
{
    char32_t alef, isolated, final;
};

constexpr LamAlef kLamAlef[] = {
    { 0x0622, 0xfef5, 0xfef6 },
    { 0x0623, 0xfef7, 0xfef8 },
    { 0x0625, 0xfef9, 0xfefa },
    { 0x0627, 0xfefb, 0xfefc },
};

constexpr char32_t kLam = 0x0644;

const ArabicForms* formsOf (char32_t c) noexcept
{
    const auto at = std::lower_bound (std::begin (kForms), std::end (kForms), c,
                                      [] (const ArabicForms& f, char32_t letter) { return f.letter < letter; });
    return at != std::end (kForms) && at->letter == c ? at : nullptr;
}

const LamAlef* lamAlefOf (char32_t alef) noexcept
{
    for (const auto& ligature : kLamAlef)
        if (ligature.alef == alef) return &ligature;
    return nullptr;
}

enum class Joining { none, right, dual, causing, transparent };

// Derived from the forms that can be drawn rather than from ArabicShaping.txt: a letter
// with no initial or medial form here (noon ghunna, say) would leave a gap beside a
// neighbour shaped to join it, so it joins as far as its forms reach.
Joining joiningOf (char32_t c) noexcept
{
    if (c == 0x0640 || c == 0x200d) return Joining::causing;
    if (const auto* forms = formsOf (c)) return forms->initial != 0 ? Joining::dual : forms->final != 0 ? Joining::right : Joining::none;
    // Format characters join transparently, except ZWNJ and the prepended concatenation
    // marks, which break joining.
    if (c == 0x200c || (c >= 0x0600 && c <= 0x0605) || c == 0x06dd || c == 0x070f || c == 0x0890 || c == 0x0891 || c == 0x08e2)
        return Joining::none;
    const auto category = SBCodepointGetGeneralCategory (static_cast<SBCodepoint> (c));
    if (category == SBGeneralCategoryMN || category == SBGeneralCategoryME || category == SBGeneralCategoryCF)
        return Joining::transparent;
    return Joining::none;
}

bool joinsForward (Joining j) noexcept { return j == Joining::dual || j == Joining::causing; }
bool joinsBackward (Joining j) noexcept { return j == Joining::dual || j == Joining::right || j == Joining::causing; }

bool isMark (char32_t c) noexcept
{
    return SBGeneralCategoryIsMark (SBCodepointGetGeneralCategory (static_cast<SBCodepoint> (c)));
}

bool mayHoldArabic (std::string_view utf8) noexcept
{
    for (const char c : utf8)
    {
        const auto byte = static_cast<unsigned char> (c);
        if (byte >= 0xd8 && byte <= 0xdb) return true;
    }
    return false;
}

std::vector<LineRange> newlineLines (std::string_view utf8)
{
    std::vector<LineRange> lines;
    std::size_t begin = 0;
    for (auto at = utf8.find ('\n'); at != std::string_view::npos; at = utf8.find ('\n', begin))
    {
        lines.push_back ({ begin, at });
        begin = at + 1;
    }
    lines.push_back ({ begin, utf8.size() });
    return lines;
}

std::string joined (std::string_view utf8, const std::vector<LineRange>& lines)
{
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        if (i > 0) out += '\n';
        const auto begin = std::min (lines[i].begin, utf8.size());
        out.append (utf8.substr (begin, std::clamp (lines[i].end, begin, utf8.size()) - begin));
    }
    return out;
}

void appendRun (std::string& out, std::string_view utf8, const SBRun& run)
{
    const auto text = utf8.substr (run.offset, run.length);
    if ((run.level & 1) == 0)
    {
        out.append (text);
        return;
    }
    std::vector<Decoded> codepoints;
    decodeAll (text, codepoints);
    // A base and the marks after it are one cluster, kept in typing order so a renderer
    // that places a mark over the glyph before it still puts the mark on its base.
    for (auto end = codepoints.size(); end > 0;)
    {
        auto begin = end - 1;
        while (begin > 0 && isMark (codepoints[begin].codepoint)) --begin;
        const auto mirror = SBCodepointGetMirror (static_cast<SBCodepoint> (codepoints[begin].codepoint));
        if (mirror != 0)
            append (out, static_cast<char32_t> (mirror));
        else
            out.append (text.substr (codepoints[begin].begin, codepoints[begin].end - codepoints[begin].begin));
        if (begin + 1 < end)
            out.append (text.substr (codepoints[begin + 1].begin, codepoints[end - 1].end - codepoints[begin + 1].begin));
        end = begin;
    }
}
} // namespace

bool needsDisplayOrder (std::string_view utf8) noexcept
{
    std::size_t at = 0;
    for (; at < utf8.size(); ++at)
    {
        const auto byte = static_cast<unsigned char> (utf8[at]);
        if (byte > 0xd6 || (byte == 0xd6 && at + 1 < utf8.size() && static_cast<unsigned char> (utf8[at + 1]) >= 0x90))
            break;
    }
    if (at == utf8.size()) return false;

    bool found = false;
    for (std::size_t i = 0; i < utf8.size();)
    {
        char32_t c = 0;
        if (! decode (utf8, i, c)) return false;
        if (found || c < 0x0590) continue;
        switch (SBCodepointGetBidiType (static_cast<SBCodepoint> (c)))
        {
            case SBBidiTypeR: case SBBidiTypeAL: case SBBidiTypeAN:
            case SBBidiTypeLRE: case SBBidiTypeRLE: case SBBidiTypeLRO: case SBBidiTypeRLO: case SBBidiTypePDF:
            case SBBidiTypeLRI: case SBBidiTypeRLI: case SBBidiTypeFSI: case SBBidiTypePDI:
                found = true;
                break;
            default:
                break;
        }
    }
    return found;
}

std::string shapeArabic (std::string_view utf8)
{
    std::vector<Decoded> codepoints;
    if (! mayHoldArabic (utf8) || ! decodeAll (utf8, codepoints)) return std::string (utf8);

    std::vector<Joining> joining;
    joining.reserve (codepoints.size());
    for (const auto& c : codepoints) joining.push_back (joiningOf (c.codepoint));

    const auto neighbour = [&] (std::size_t i, int step) -> std::size_t
    {
        for (auto j = static_cast<std::ptrdiff_t> (i) + step; j >= 0 && j < static_cast<std::ptrdiff_t> (codepoints.size()); j += step)
            if (joining[static_cast<std::size_t> (j)] != Joining::transparent) return static_cast<std::size_t> (j);
        return codepoints.size();
    };

    std::string out;
    out.reserve (utf8.size());
    for (std::size_t i = 0; i < codepoints.size(); ++i)
    {
        const auto c = codepoints[i].codepoint;
        const auto* forms = formsOf (c);
        if (forms == nullptr)
        {
            out.append (utf8.substr (codepoints[i].begin, codepoints[i].end - codepoints[i].begin));
            continue;
        }
        const auto before = neighbour (i, -1);
        const auto after = neighbour (i, 1);
        const bool joinsBefore = before < codepoints.size() && joinsForward (joining[before]) && joinsBackward (joining[i]);
        const bool joinsAfter = after < codepoints.size() && joinsForward (joining[i]) && joinsBackward (joining[after]);

        if (c == kLam && after < codepoints.size())
            if (const auto* ligature = lamAlefOf (codepoints[after].codepoint))
            {
                append (out, joinsBefore ? ligature->final : ligature->isolated);
                if (i + 1 < after)
                    out.append (utf8.substr (codepoints[i + 1].begin, codepoints[after - 1].end - codepoints[i + 1].begin));
                i = after;
                continue;
            }

        append (out, joinsBefore && joinsAfter ? forms->medial
                   : joinsBefore             ? forms->final
                   : joinsAfter              ? forms->initial
                                             : forms->isolated);
    }
    return out;
}

std::string visualOrder (std::string_view utf8, const std::vector<LineRange>& lines)
{
    if (! isValid (utf8)) return joined (utf8, lines);

    SBCodepointSequence sequence { SBStringEncodingUTF8, utf8.data(), utf8.size() };
    const auto algorithm = SBAlgorithmCreate (&sequence);
    if (algorithm == nullptr) return joined (utf8, lines);

    std::string out;
    out.reserve (utf8.size() + lines.size());
    SBParagraphRef paragraph = nullptr;
    std::size_t paragraphBegin = 0, paragraphEnd = 0;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        if (i > 0) out += '\n';
        auto begin = std::min (lines[i].begin, utf8.size());
        const auto end = std::clamp (lines[i].end, begin, utf8.size());
        if (begin < paragraphBegin)
        {
            if (paragraph != nullptr) SBParagraphRelease (paragraph);
            paragraph = nullptr;
            paragraphBegin = paragraphEnd = 0;
        }
        while (begin < end)
        {
            while (paragraph == nullptr || begin >= paragraphEnd)
            {
                if (paragraph != nullptr) SBParagraphRelease (paragraph);
                paragraphBegin = paragraphEnd;
                paragraph = SBAlgorithmCreateParagraph (algorithm, paragraphBegin, utf8.size() - paragraphBegin, SBLevelDefaultLTR);
                if (paragraph == nullptr) break;
                paragraphEnd = paragraphBegin + SBParagraphGetLength (paragraph);
            }
            if (paragraph == nullptr)
            {
                out.append (utf8.substr (begin, end - begin));
                break;
            }
            const auto pieceEnd = std::min (end, paragraphEnd);
            if (const auto line = SBParagraphCreateLine (paragraph, begin, pieceEnd - begin))
            {
                const auto* runs = SBLineGetRunsPtr (line);
                for (SBUInteger r = 0; r < SBLineGetRunCount (line); ++r)
                    appendRun (out, utf8, runs[r]);
                SBLineRelease (line);
            }
            else
            {
                out.append (utf8.substr (begin, pieceEnd - begin));
            }
            begin = pieceEnd;
        }
    }
    if (paragraph != nullptr) SBParagraphRelease (paragraph);
    SBAlgorithmRelease (algorithm);
    return out;
}

std::string visualOrder (std::string_view utf8)
{
    return visualOrder (utf8, newlineLines (utf8));
}

std::string displayOrder (std::string_view utf8)
{
    if (! needsDisplayOrder (utf8)) return std::string (utf8);
    return visualOrder (shapeArabic (utf8));
}

const std::vector<char32_t>& arabicPresentationForms()
{
    static const std::vector<char32_t> forms = []
    {
        std::vector<char32_t> all;
        for (const auto& f : kForms)
            for (const auto c : { f.isolated, f.final, f.initial, f.medial })
                if (c != 0) all.push_back (c);
        for (const auto& ligature : kLamAlef)
        {
            all.push_back (ligature.isolated);
            all.push_back (ligature.final);
        }
        std::sort (all.begin(), all.end());
        all.erase (std::unique (all.begin(), all.end()), all.end());
        return all;
    }();
    return forms;
}
} // namespace dusk::text
