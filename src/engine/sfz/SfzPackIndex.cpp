#include "SfzPackIndex.h"

#if DUSKSTUDIO_HAS_MULTISAMPLE
#include "SfzPathPolicy.h"

#include <pugixml.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace duskstudio::sfz
{
namespace
{
namespace fs = std::filesystem;
constexpr std::size_t kMaxManifests = 64;
constexpr std::size_t kMaxManifestBytes = 1024 * 1024;
constexpr std::size_t kMaxTotalBytes = 8 * kMaxManifestBytes;
constexpr std::size_t kMaxVisitedEntries = 65536;
constexpr int kMaxDirectoryDepth = 32;

bool bankFilename (std::string_view name)
{
    constexpr std::string_view suffix = ".bank.xml";
    if (name.size() < suffix.size()) return false;
    name.remove_prefix (name.size() - suffix.size());
    return std::equal (name.begin(), name.end(), suffix.begin(), [] (char a, char b)
    {
        if (a >= 'A' && a <= 'Z') a = static_cast<char> (a - 'A' + 'a');
        return a == b;
    });
}

bool plainFile (const fs::path& root, const std::string& relative)
{
    std::string error;
    if (! validateRelativePath (relative, error)) return false;
    std::error_code ec;
    if (! fs::is_directory (fs::symlink_status (root, ec)) || ec) return false;
    auto current = root;
    auto status = fs::file_status {};
    for (const auto& component : fs::u8path (relative))
    {
        current /= component;
        status = fs::symlink_status (current, ec);
        if (ec || fs::is_symlink (status)) return false;
    }
    return fs::is_regular_file (status);
}

bool findManifests (const fs::path& directory, std::vector<fs::path>& manifests)
{
    std::error_code ec;
    if (! fs::is_directory (fs::symlink_status (directory, ec)) || ec) return false;
    std::size_t visited = 0;
    for (auto it = fs::recursive_directory_iterator (directory, ec);
         ! ec && it != fs::recursive_directory_iterator(); it.increment (ec))
    {
        if (++visited > kMaxVisitedEntries) return false;
        const auto status = it->symlink_status (ec);
        if (ec) return false;
        if (fs::is_directory (status))
        {
            if (it.depth() >= kMaxDirectoryDepth) return false;
            continue;
        }
        it.disable_recursion_pending();
        if (! fs::is_regular_file (status) || ! bankFilename (it->path().filename().u8string()))
            continue;
        manifests.push_back (it->path().lexically_relative (directory));
        if (manifests.size() > kMaxManifests) return false;
    }
    std::sort (manifests.begin(), manifests.end());
    return ! ec;
}

bool safeXmlText (std::string_view text)
{
    if (text.find ('\0') != std::string_view::npos) return false;
    for (auto offset = text.find ("&#"); offset != std::string_view::npos;
         offset = text.find ("&#", offset))
    {
        offset += 2;
        unsigned base = 10, value = 0;
        if (offset < text.size() && text[offset] == 'x') { base = 16; ++offset; }
        const auto begin = offset;
        for (; offset < text.size() && text[offset] != ';'; ++offset)
        {
            const auto c = text[offset];
            unsigned digit = base;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned> (c - '0');
            else if (base == 16 && c >= 'a' && c <= 'f') digit = static_cast<unsigned> (c - 'a' + 10);
            else if (base == 16 && c >= 'A' && c <= 'F') digit = static_cast<unsigned> (c - 'A' + 10);
            if (digit >= base || value > (0x10ffff - digit) / base) return false;
            value = value * base + digit;
        }
        if (offset == begin || offset == text.size()
            || (value < 0x20 && value != 9 && value != 10 && value != 13)
            || (value >= 0xd800 && value <= 0xdfff) || value == 0xfffe || value == 0xffff)
            return false;
    }
    return true;
}

bool safeDocument (const pugi::xml_document& document)
{
    unsigned depth = 0;
    for (auto node = document.first_child(); node;)
    {
        if (node.type() == pugi::node_doctype || depth > 64) return false;
        if (node.first_child()) { node = node.first_child(); ++depth; continue; }
        while (! node.next_sibling() && node.parent() != document)
        {
            node = node.parent();
            --depth;
        }
        node = node.next_sibling();
    }
    return true;
}
} // namespace

std::vector<ProviderInstrument> indexPackPrograms (
    const fs::path& directory, const std::vector<ProviderInstrument>& instruments)
{
    if (instruments.empty()) return instruments;
    std::vector<fs::path> manifests;
    if (! findManifests (directory, manifests) || manifests.empty()) return instruments;
    std::map<std::string, const ProviderInstrument*> candidates;
    for (const auto& instrument : instruments)
        if (const auto key = portablePathKey (instrument.relativePath))
            candidates.emplace (*key, &instrument);

    std::size_t totalBytes = 0;
    std::set<std::string> seen;
    std::vector<ProviderInstrument> programs;
    for (const auto& manifest : manifests)
    {
        if (! plainFile (directory, manifest.generic_u8string())) continue;
        std::error_code ec;
        const auto size = fs::file_size (directory / manifest, ec);
        if (ec || size > kMaxManifestBytes) continue;
        if (size > kMaxTotalBytes - totalBytes) return instruments;
        totalBytes += static_cast<std::size_t> (size);
        std::ifstream input (directory / manifest, std::ios::binary);
        if (! input) continue;
        std::string text (static_cast<std::size_t> (size) + 1, '\0');
        input.read (text.data(), static_cast<std::streamsize> (text.size()));
        if (input.bad() || input.gcount() != static_cast<std::streamsize> (size)) continue;
        text.resize (static_cast<std::size_t> (size));
        if (! safeXmlText (text)) continue;
        pugi::xml_document document;
        if (! document.load_buffer (text.data(), text.size(),
                                    pugi::parse_default | pugi::parse_fragment | pugi::parse_doctype,
                                    pugi::encoding_utf8)
            || ! safeDocument (document)) continue;
        for (const auto& bank : document.children ("AriaBank"))
            for (const auto& program : bank.children ("AriaProgram"))
            {
                std::string path = program.child ("AriaElement").attribute ("path").value();
                std::replace (path.begin(), path.end(), '\\', '/');
                std::string error;
                if (path.empty() || path.front() == '/' || path.find (':') != std::string::npos
                    || path.find ('\0') != std::string::npos) continue;
                path = (manifest.parent_path() / fs::u8path (path)).lexically_normal().generic_u8string();
                if (! validateRelativePath (path, error)) continue;
                const auto key = portablePathKey (path);
                if (! key) continue;
                const auto candidate = candidates.find (*key);
                if (candidate == candidates.end()
                    || ! plainFile (directory, candidate->second->relativePath)
                    || ! seen.insert (*key).second) continue;
                auto instrument = *candidate->second;
                const std::string name = program.attribute ("name").value();
                if (! name.empty()) instrument.name = name;
                programs.push_back (std::move (instrument));
            }
    }
    // A usable bank explicitly defines the browser list. The receipt retains raw access.
    if (programs.empty()) return instruments;
    return programs;
}
} // namespace duskstudio::sfz
#else
namespace duskstudio::sfz
{
std::vector<ProviderInstrument> indexPackPrograms (
    const std::filesystem::path& directory, const std::vector<ProviderInstrument>& instruments)
{
    (void) directory;
    return instruments;
}
} // namespace duskstudio::sfz
#endif
