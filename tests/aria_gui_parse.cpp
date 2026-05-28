#include <catch2/catch_test_macros.hpp>

#include "engine/multisample/AriaBank.h"
#include "engine/multisample/AriaGui.h"

#include <juce_core/juce_core.h>

#include <algorithm>

namespace
{
juce::File makeTempFile(const juce::String& contents, const juce::String& suffix)
{
    auto f = juce::File::createTempFile(suffix);
    f.replaceWithText(contents);
    return f;
}

int countKind(const std::vector<duskstudio::AriaWidget>& ws,
              duskstudio::AriaWidgetKind k)
{
    return (int) std::count_if(ws.begin(), ws.end(),
                                [k](const auto& w) { return w.kind == k; });
}
}

TEST_CASE("AriaGui: parses GUI root + StaticImage + StaticText", "[aria_gui]")
{
    const auto xml = juce::String(R"(<GUI w="800" h="400">
        <StaticImage x="0" y="0" w="800" h="400" image="bg.png" transparent="0" />
        <StaticText x="10" y="12" w="60" h="20" text="Hello"
                    color_text="#FFCC00FF" transparent="1" />
    </GUI>)");

    auto file = makeTempFile(xml, ".xml");
    auto docOpt = duskstudio::AriaGuiDoc::parse(file);
    REQUIRE(docOpt.has_value());

    auto& d = *docOpt;
    REQUIRE(d.width  == 800);
    REQUIRE(d.height == 400);
    REQUIRE(d.widgets.size() == 2);

    REQUIRE(d.widgets[0].kind == duskstudio::AriaWidgetKind::StaticImage);
    REQUIRE(d.widgets[0].image == "bg.png");
    REQUIRE_FALSE(d.widgets[0].transparent);
    REQUIRE(d.widgets[0].bounds.getWidth()  == 800);
    REQUIRE(d.widgets[0].bounds.getHeight() == 400);

    REQUIRE(d.widgets[1].kind == duskstudio::AriaWidgetKind::StaticText);
    REQUIRE(d.widgets[1].text == "Hello");
    REQUIRE(d.widgets[1].textColour.getARGB() == 0xFFFFCC00u);
    REQUIRE(d.widgets[1].transparent);

    file.deleteFile();
}

TEST_CASE("AriaGui: parses Slider with filmstrip handle/bg + orientation",
          "[aria_gui]")
{
    const auto xml = juce::String(R"(<GUI w="200" h="100">
        <Slider param="38" x="21" y="28" w="12" h="69"
                orientation="vertical"
                image_handle="fader_handle.png" image_bg="fader_slot_69.png" />
        <Slider param="42" x="10" y="50" w="100" h="12"
                orientation="horizontal" />
    </GUI>)");

    auto file = makeTempFile(xml, ".xml");
    auto docOpt = duskstudio::AriaGuiDoc::parse(file);
    REQUIRE(docOpt.has_value());
    auto& d = *docOpt;

    REQUIRE(d.widgets.size() == 2);
    REQUIRE(d.widgets[0].kind        == duskstudio::AriaWidgetKind::Slider);
    REQUIRE(d.widgets[0].paramCC     == 38);
    REQUIRE(d.widgets[0].orient      == duskstudio::AriaOrientation::Vertical);
    REQUIRE(d.widgets[0].imageHandle == "fader_handle.png");
    REQUIRE(d.widgets[0].imageBg     == "fader_slot_69.png");

    REQUIRE(d.widgets[1].orient == duskstudio::AriaOrientation::Horizontal);

    file.deleteFile();
}

TEST_CASE("AriaGui: parses Knob with filmstrip image + frame count",
          "[aria_gui]")
{
    const auto xml = juce::String(R"(<GUI w="200" h="100">
        <Knob param="40" x="14" y="110" image="Drum_knob.png" frames="128" />
        <Knob param="41" x="54" y="110" image="Drum_knob.png" frames="64"  />
    </GUI>)");

    auto file = makeTempFile(xml, ".xml");
    auto docOpt = duskstudio::AriaGuiDoc::parse(file);
    REQUIRE(docOpt.has_value());
    auto& d = *docOpt;

    REQUIRE(d.widgets.size() == 2);
    REQUIRE(d.widgets[0].kind    == duskstudio::AriaWidgetKind::Knob);
    REQUIRE(d.widgets[0].paramCC == 40);
    REQUIRE(d.widgets[0].image   == "Drum_knob.png");
    REQUIRE(d.widgets[0].frames  == 128);
    REQUIRE(d.widgets[1].frames  == 64);

    file.deleteFile();
}

TEST_CASE("AriaGui: parses OptionMenu + nested OptionItem children",
          "[aria_gui]")
{
    // Sforzando-style OptionItem uses `name=` for display + `value=`
    // as a normalized float 0..1. Older fixtures (and a few non-ARIA
    // libraries) may still write `text=` - the parser accepts either.
    const auto xml = juce::String(R"(<GUI w="200" h="100">
        <OptionMenu param="100" x="10" y="20" w="80" h="20">
            <OptionItem name="Off"      value="0"     />
            <OptionItem name="Moderate" value="0.5"   />
            <OptionItem text="Heavy"    value="1"     />
        </OptionMenu>
    </GUI>)");

    auto file = makeTempFile(xml, ".xml");
    auto docOpt = duskstudio::AriaGuiDoc::parse(file);
    REQUIRE(docOpt.has_value());
    auto& d = *docOpt;

    REQUIRE(d.widgets.size() == 1);
    REQUIRE(d.widgets[0].kind           == duskstudio::AriaWidgetKind::OptionMenu);
    REQUIRE(d.widgets[0].paramCC        == 100);
    REQUIRE(d.widgets[0].options.size() == 3);
    REQUIRE(d.widgets[0].options[0].text  == "Off");
    REQUIRE(d.widgets[0].options[0].value == 0.0f);
    REQUIRE(d.widgets[0].options[1].text  == "Moderate");
    REQUIRE(d.widgets[0].options[1].value == 0.5f);
    REQUIRE(d.widgets[0].options[2].text  == "Heavy");  // text= fallback
    REQUIRE(d.widgets[0].options[2].value == 1.0f);

    file.deleteFile();
}

TEST_CASE("AriaGui: parses CommandButton with launch_url command",
          "[aria_gui]")
{
    const auto xml = juce::String(R"(<GUI w="200" h="100">
        <CommandButton command="launch_url"
                       data0="http://www.example.com/"
                       x="17" y="58" w="78" h="60" image="btn.png" />
    </GUI>)");

    auto file = makeTempFile(xml, ".xml");
    auto docOpt = duskstudio::AriaGuiDoc::parse(file);
    REQUIRE(docOpt.has_value());

    auto& w = docOpt->widgets.at(0);
    REQUIRE(w.kind    == duskstudio::AriaWidgetKind::CommandButton);
    REQUIRE(w.command == "launch_url");
    REQUIRE(w.data0   == "http://www.example.com/");
    REQUIRE(w.image   == "btn.png");

    file.deleteFile();
}

TEST_CASE("AriaGui: unknown tags survive as Unknown widget for placeholder",
          "[aria_gui]")
{
    const auto xml = juce::String(R"(<GUI w="100" h="100">
        <FrobnicateNozzle x="0" y="0" w="50" h="50" />
    </GUI>)");

    auto file = makeTempFile(xml, ".xml");
    auto docOpt = duskstudio::AriaGuiDoc::parse(file);
    REQUIRE(docOpt.has_value());
    auto& d = *docOpt;

    REQUIRE(d.widgets.size() == 1);
    REQUIRE(d.widgets[0].kind    == duskstudio::AriaWidgetKind::Unknown);
    REQUIRE(d.widgets[0].tagName == "FrobnicateNozzle");

    file.deleteFile();
}

TEST_CASE("AriaGui: invalid root or missing dimensions returns nullopt",
          "[aria_gui]")
{
    SECTION("non-GUI root")
    {
        auto file = makeTempFile("<NotGui w=\"100\" h=\"100\" />", ".xml");
        REQUIRE_FALSE(duskstudio::AriaGuiDoc::parse(file).has_value());
        file.deleteFile();
    }
    SECTION("missing w/h")
    {
        auto file = makeTempFile("<GUI />", ".xml");
        REQUIRE_FALSE(duskstudio::AriaGuiDoc::parse(file).has_value());
        file.deleteFile();
    }
    SECTION("non-existent file")
    {
        REQUIRE_FALSE(duskstudio::AriaGuiDoc::parse(
            juce::File("/nonexistent/path/that/cannot/exist.xml")).has_value());
    }
}

TEST_CASE("AriaBank: parses programs + resolves sfz / gui paths against bank dir",
          "[aria_bank]")
{
    // Lay out a minimal Swirly-shaped tree in a temp dir:
    //   tmp/Pack.bank.xml
    //   tmp/Programs/A.sfz
    //   tmp/Programs/B.sfz
    //   tmp/GUI/A.xml
    auto root = juce::File::createTempFile("");
    root.deleteFile();
    root.createDirectory();

    auto programsDir = root.getChildFile("Programs");
    auto guiDir      = root.getChildFile("GUI");
    programsDir.createDirectory();
    guiDir.createDirectory();
    auto sfzA = programsDir.getChildFile("A.sfz");  sfzA.replaceWithText("// A");
    auto sfzB = programsDir.getChildFile("B.sfz");  sfzB.replaceWithText("// B");
    auto guiA = guiDir     .getChildFile("A.xml");
    guiA.replaceWithText("<GUI w=\"100\" h=\"100\" />");

    auto bankFile = root.getChildFile("Pack.bank.xml");
    bankFile.replaceWithText(R"(<AriaBank name="Test" vendor="Acme">
        <AriaProgram name="Prog A" gui="GUI/A.xml">
            <AriaElement path="Programs/A.sfz" />
        </AriaProgram>
        <AriaProgram name="Prog B" gui="GUI/B.xml">
            <AriaElement path="Programs/B.sfz" />
        </AriaProgram>
    </AriaBank>)");

    auto bankOpt = duskstudio::AriaBank::tryLoadFromSfz(sfzB);
    REQUIRE(bankOpt.has_value());
    auto& b = *bankOpt;
    REQUIRE(b.bankName == "Test");
    REQUIRE(b.vendor   == "Acme");
    REQUIRE(b.programs.size() == 2);
    REQUIRE(b.programs[0].name == "Prog A");
    REQUIRE(b.programs[0].sfzFile == sfzA);
    REQUIRE(b.programs[0].guiFile == guiA);
    REQUIRE(b.programs[1].sfzFile == sfzB);
    REQUIRE(b.selectedIndex == 1);   // we loaded B

    root.deleteRecursively();
}

TEST_CASE("AriaBank: returns nullopt when no *.bank.xml is found",
          "[aria_bank]")
{
    auto root = juce::File::createTempFile("");
    root.deleteFile();
    root.createDirectory();
    auto sfz = root.getChildFile("lonely.sfz");
    sfz.replaceWithText("// no bank manifest in this tree");

    REQUIRE_FALSE(duskstudio::AriaBank::tryLoadFromSfz(sfz).has_value());

    root.deleteRecursively();
}

// Optional fixture validation - runs only when the Swirly Drums package
// is present at the path the developer that wrote this test had on disk
// (the spec mentioned this exact location). Skipped silently elsewhere
// so the test binary stays portable.
TEST_CASE("AriaGui: Swirly fixture parses with expected widget counts",
          "[aria_gui][.fixture]")
{
    const juce::File swirlyRoot { "/Users/marckorte/Downloads/Swirly.Drums_1104" };
    if (! swirlyRoot.isDirectory())
        SUCCEED("Swirly fixture not present at " + swirlyRoot.getFullPathName()
                + " - skipping");
    else
    {
        auto fullKitSfz = swirlyRoot.getChildFile("Programs/Full_kit.sfz");
        REQUIRE(fullKitSfz.existsAsFile());

        auto bankOpt = duskstudio::AriaBank::tryLoadFromSfz(fullKitSfz);
        REQUIRE(bankOpt.has_value());
        REQUIRE(bankOpt->programs.size() == 8);
        REQUIRE(bankOpt->bankName == "Swirly Drums");
        REQUIRE(bankOpt->selectedIndex >= 0);
        REQUIRE(bankOpt->programs[(size_t) bankOpt->selectedIndex].sfzFile
                == fullKitSfz);

        auto fullKitXml = swirlyRoot.getChildFile("GUI/Full_kit.xml");
        REQUIRE(fullKitXml.existsAsFile());

        auto docOpt = duskstudio::AriaGuiDoc::parse(fullKitXml);
        REQUIRE(docOpt.has_value());
        REQUIRE(docOpt->width  == 775);
        REQUIRE(docOpt->height == 335);
        REQUIRE(countKind(docOpt->widgets,
                          duskstudio::AriaWidgetKind::StaticImage) == 1);
        REQUIRE(countKind(docOpt->widgets,
                          duskstudio::AriaWidgetKind::StaticText)  == 72);
        REQUIRE(countKind(docOpt->widgets,
                          duskstudio::AriaWidgetKind::Slider)      == 27);
        REQUIRE(countKind(docOpt->widgets,
                          duskstudio::AriaWidgetKind::Knob)        == 34);
        REQUIRE(countKind(docOpt->widgets,
                          duskstudio::AriaWidgetKind::OptionMenu)  == 2);

        // Image paths in XML are relative to the GUI dir.
        REQUIRE(docOpt->resourceDir == swirlyRoot.getChildFile("GUI"));
    }
}
