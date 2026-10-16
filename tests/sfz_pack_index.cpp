#include <catch2/catch_test_macros.hpp>

#include "engine/sfz/SfzPackIndex.h"
#include "foundation/Fs.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>

namespace
{
using namespace duskstudio::sfz;
namespace fs = std::filesystem;

struct IndexTree
{
    IndexTree() : root (fs::canonical (dusk::fs::createUniqueTempDirectory ("dusk-sfz-index-"))) {}
    ~IndexTree() { std::error_code ignored; fs::remove_all (root, ignored); }

    void write (const std::string& path, const std::string& contents)
    {
        std::error_code error;
        fs::create_directories ((root / fs::u8path (path)).parent_path(), error);
        REQUIRE_FALSE (error);
        REQUIRE (dusk::fs::writeStringToFile (root / fs::u8path (path), contents));
    }

    void instrument (const std::string& path, const std::string& name = "Receipt name")
    {
        write (path, "<region> sample=tone.wav");
        instruments.push_back ({ "receipt-id-" + std::to_string (instruments.size()), name, path, true });
    }

    fs::path root;
    std::vector<ProviderInstrument> instruments;
};

void checkUnchanged (const std::vector<ProviderInstrument>& actual,
                     const std::vector<ProviderInstrument>& expected)
{
    REQUIRE (actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i)
    {
        CHECK (actual[i].id == expected[i].id);
        CHECK (actual[i].name == expected[i].name);
        CHECK (actual[i].relativePath == expected[i].relativePath);
        CHECK (actual[i].candidateOnly == expected[i].candidateOnly);
    }
}

std::string bankProgram (const std::string& path, const std::string& name = "Bank program")
{
    return "<AriaProgram name=\"" + name + "\"><AriaElement path=\"" + path
         + "\"/></AriaProgram>";
}
} // namespace

#if DUSKSTUDIO_HAS_MULTISAMPLE
TEST_CASE ("SFZ browser programs project ARIA declarations without changing receipt identities", "[sfz][index]")
{
    IndexTree tree;
    const std::array<std::string, 8> paths {
        "01-full", "02-basic", "03-kick", "04-snare", "05-toms", "06-hihat", "07-cymbals", "08-noises"
    };
    std::string bank = "<?xml version=\"1.0\"?><Key>fixture</Key><!-- bank metadata -->"
                       "<AriaBank><AriaGUI>" + bankProgram ("Maps/0.sfz", "Hidden GUI program")
                     + "</AriaGUI><Script><![CDATA[" + bankProgram ("Maps/1.sfz") + "]]></Script>";
    for (std::size_t i = 0; i < paths.size(); ++i)
    {
        tree.instrument ("Programs/" + paths[i] + ".sfz");
        if (i == 0)
            bank += "<AriaProgram name='Full &amp; kit &#x266B;' gui='ignored.xml'>"
                    "<AriaElement path='PROGRAMS\\01-FULL.SFZ'/><AriaElement path='Maps/0.sfz'/>"
                    "</AriaProgram>";
        else if (i == 1)
            bank += bankProgram ("Programs/" + paths[i] + ".sfz", "");
        else
            bank += bankProgram ("Programs/" + paths[i] + ".sfz", "Program " + std::to_string (i));
    }
    bank += "</AriaBank>";
    tree.instruments[0].candidateOnly = false;
    for (unsigned i = 0; i < 339; ++i) tree.instrument ("Maps/" + std::to_string (i) + ".sfz");
    tree.write ("Drums.bank.xml", bank);
    const auto receipt = tree.instruments;
    const auto programs = indexPackPrograms (tree.root, tree.instruments);
    REQUIRE (receipt.size() == 347);
    REQUIRE (programs.size() == 8);
    for (std::size_t i = 0; i < programs.size(); ++i)
    {
        CHECK (programs[i].id == receipt[i].id);
        CHECK (programs[i].relativePath == receipt[i].relativePath);
        CHECK (programs[i].candidateOnly == receipt[i].candidateOnly);
    }
    CHECK (programs[0].name == u8"Full & kit ♫");
    CHECK (programs[1].name == receipt[1].name);
    CHECK (programs[7].name == "Program 7");
    checkUnchanged (tree.instruments, receipt);
}

TEST_CASE ("SFZ browser programs merge all usable banks and resolve nested portable paths", "[sfz][index]")
{
    IndexTree tree;
    tree.instrument ("Programs/first.sfz");
    tree.instrument (u8"Collection/Programs/Étude.sfz");
    tree.instrument ("Programs/last.sfz");
    tree.instrument ("standalone.sfz");
    tree.write ("a.bank.xml", "<AriaBank>" + bankProgram ("Programs/first.sfz", "First") + "</AriaBank>");
    tree.write ("b.bank.xml", "<AriaBank>" + bankProgram ("PROGRAMS/FIRST.SFZ", "Duplicate") + "</AriaBank>");
    tree.write ("Banks/relative.bank.xml", "<AriaBank>"
                + bankProgram ("../Programs/last.sfz", "Last") + "</AriaBank>");
    tree.write ("Collection/c.BANK.XML", "<Key>another key</Key><AriaBank>"
                + bankProgram (u8"Programs\\ÉTUDE.sfz", "Nested &quot;study&quot;") + "</AriaBank>");
    tree.write ("broken.bank.xml", "<AriaBank><AriaProgram>");
    const auto programs = indexPackPrograms (tree.root, tree.instruments);
    REQUIRE (programs.size() == 3);
    for (std::size_t i = 0; i < 3; ++i)
    {
        const auto found = std::find_if (programs.begin(), programs.end(), [&] (const ProviderInstrument& program)
            { return program.id == tree.instruments[i].id; });
        REQUIRE (found != programs.end());
        CHECK (found->relativePath == tree.instruments[i].relativePath);
        CHECK (found->name == (i == 0 ? "First" : i == 1 ? "Nested \"study\"" : "Last"));
    }
    CHECK (tree.instruments.size() == 4);
    CHECK (tree.instruments.back().relativePath == "standalone.sfz");
}
#endif

TEST_CASE ("SFZ program indexing preserves raw candidates when bank metadata is unusable", "[sfz][index]")
{
    IndexTree tree;
    tree.instrument ("Programs/main.sfz", "Main");
    tree.instrument ("Programs/map.sfz", "Map");
    tree.write ("Programs/main.sfz", "#include \"map.sfz\"\n#define $path map.sfz\n<region>");
    tree.write ("Programs/map.sfz", "#include \"main.sfz\"\n<region>");

    SECTION ("No bank and uncertain include relationships") {}
#if ! DUSKSTUDIO_HAS_MULTISAMPLE
    SECTION ("Bank metadata without multisample support")
    {
        tree.write ("valid.bank.xml", "<AriaBank>" + bankProgram ("Programs/main.sfz") + "</AriaBank>");
    }
#endif
    SECTION ("Malformed XML") { tree.write ("bad.bank.xml", "<AriaBank><AriaProgram>"); }
    SECTION ("Unrelated XML root")
    {
        tree.write ("bad.bank.xml", "<GUI><AriaBank>" + bankProgram ("Programs/main.sfz") + "</AriaBank></GUI>");
    }
    SECTION ("Only a nested element")
    {
        tree.write ("bad.bank.xml", "<AriaBank><AriaProgram><Layer><AriaElement path='Programs/main.sfz'/>"
                                   "</Layer></AriaProgram></AriaBank>");
    }
    SECTION ("First element has no usable path")
    {
        tree.write ("bad.bank.xml", "<AriaBank><AriaProgram><AriaElement/><AriaElement path='Programs/main.sfz'/>"
                                   "</AriaProgram></AriaBank>");
    }
    SECTION ("Missing file")
    {
        tree.write ("bad.bank.xml", "<AriaBank>" + bankProgram ("Programs/missing.sfz") + "</AriaBank>");
    }
    SECTION ("Receipt entry is no longer a regular file")
    {
        tree.write ("bad.bank.xml", "<AriaBank>" + bankProgram ("Programs/main.sfz") + "</AriaBank>");
        REQUIRE (fs::remove (tree.root / "Programs/main.sfz"));
        REQUIRE (fs::create_directory (tree.root / "Programs/main.sfz"));
    }
    SECTION ("Unreceipted file")
    {
        tree.write ("unreceipted.sfz", "<region>");
        tree.write ("bad.bank.xml", "<AriaBank>" + bankProgram ("unreceipted.sfz") + "</AriaBank>");
    }
    SECTION ("Unsafe entrypoint")
    {
        std::string path;
        SECTION ("Parent traversal") { path = "../Programs/main.sfz"; }
        SECTION ("Backslash traversal") { path = "..\\Programs\\main.sfz"; }
        SECTION ("Absolute") { path = "/Programs/main.sfz"; }
        SECTION ("Drive path") { path = "C:\\Programs\\main.sfz"; }
        SECTION ("Colon") { path = "Programs/main.sfz:stream"; }
        SECTION ("Encoded null") { path = "Programs/main.sfz&#0;ignored"; }
        SECTION ("Overflowed character reference") { path = "Programs/main.sfz&#4294967296;ignored"; }
        tree.write ("bad.bank.xml", "<AriaBank>" + bankProgram (path) + "</AriaBank>");
    }
    SECTION ("DTD")
    {
        tree.write ("bad.bank.xml", "<!DOCTYPE AriaBank><AriaBank>"
                    + bankProgram ("Programs/main.sfz") + "</AriaBank>");
    }
    SECTION ("Entity declaration")
    {
        tree.write ("bad.bank.xml", "<!DOCTYPE AriaBank [<!ENTITY path SYSTEM 'file:///etc/passwd'>]>"
                                   "<AriaBank>" + bankProgram ("Programs/main.sfz") + "</AriaBank>");
    }
    SECTION ("Raw null")
    {
        tree.write ("bad.bank.xml", "<AriaBank>" + bankProgram ("Programs/main.sfz")
                    + "</AriaBank>" + std::string (1, '\0'));
    }
    SECTION ("Oversized manifest")
    {
        tree.write ("bad.bank.xml", "<AriaBank>" + bankProgram ("Programs/main.sfz")
                    + "</AriaBank><!--" + std::string (1024 * 1024, 'x') + "-->");
    }
    SECTION ("Too many manifests")
    {
        for (unsigned i = 0; i < 65; ++i)
            tree.write (std::to_string (i) + ".bank.xml", "<AriaBank>"
                        + bankProgram ("Programs/main.sfz") + "</AriaBank>");
    }
    SECTION ("Total metadata budget")
    {
        auto text = "<AriaBank>" + bankProgram ("Programs/main.sfz") + "</AriaBank><!--";
        text += std::string (1024 * 1024 - text.size() - 3, 'x') + "-->";
        for (unsigned i = 0; i < 9; ++i) tree.write (std::to_string (i) + ".bank.xml", text);
    }
    SECTION ("Directory depth budget")
    {
        tree.write ("valid.bank.xml", "<AriaBank>" + bankProgram ("Programs/main.sfz") + "</AriaBank>");
        std::string nested;
        for (unsigned i = 0; i < 34; ++i) nested += "deep/";
        tree.write (nested + "sample.wav", "fixture");
    }
    SECTION ("Symlinked manifest")
    {
        tree.write ("metadata.xml", "<AriaBank>" + bankProgram ("Programs/main.sfz") + "</AriaBank>");
        std::error_code error;
        fs::create_symlink (tree.root / "metadata.xml", tree.root / "linked.bank.xml", error);
        if (error) SKIP ("symlinks are unavailable");
    }
    SECTION ("Symlinked program")
    {
        tree.write ("bad.bank.xml", "<AriaBank>" + bankProgram ("Programs/main.sfz") + "</AriaBank>");
        REQUIRE (fs::remove (tree.root / "Programs/main.sfz"));
        std::error_code error;
        fs::create_symlink (tree.root / "Programs/map.sfz", tree.root / "Programs/main.sfz", error);
        if (error) SKIP ("symlinks are unavailable");
    }
    SECTION ("Symlinked program parent")
    {
        tree.write ("bad.bank.xml", "<AriaBank>" + bankProgram ("Programs/main.sfz") + "</AriaBank>");
        fs::rename (tree.root / "Programs", tree.root / "actual");
        std::error_code error;
        fs::create_directory_symlink (tree.root / "actual", tree.root / "Programs", error);
        if (error) SKIP ("symlinks are unavailable");
    }
    checkUnchanged (indexPackPrograms (tree.root, tree.instruments), tree.instruments);
}
