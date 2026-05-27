#include "AriaBank.h"

namespace duskstudio
{
namespace
{
juce::File findBankFileNear(const juce::File& sfz)
{
    // Walk up from the .sfz's directory checking each level for a
    // *.bank.xml. Stop at /Users (or filesystem root) so we don't scan
    // someone's whole drive if they dropped a stray .sfz in $HOME.
    juce::File dir = sfz.getParentDirectory();
    for (int level = 0; level < 4 && dir.exists(); ++level)
    {
        const auto matches = dir.findChildFiles(juce::File::findFiles,
                                                 /*searchRecursively*/false,
                                                 "*.bank.xml");
        if (! matches.isEmpty())
            return matches.getFirst();
        const auto parent = dir.getParentDirectory();
        if (parent == dir)
            break;
        dir = parent;
    }
    return {};
}

void parseAriaProgramElement(juce::XmlElement& el,
                              const juce::File& bankDir,
                              std::vector<AriaProgram>& out)
{
    AriaProgram p;
    p.name = el.getStringAttribute("name");

    if (auto guiAttr = el.getStringAttribute("gui"); guiAttr.isNotEmpty())
        p.guiFile = bankDir.getChildFile(guiAttr);

    // Nested <AriaElement path="Programs/Foo.sfz" /> - first one wins.
    if (auto* child = el.getChildByName("AriaElement"))
    {
        if (auto pathAttr = child->getStringAttribute("path"); pathAttr.isNotEmpty())
            p.sfzFile = bankDir.getChildFile(pathAttr);
    }

    if (p.sfzFile != juce::File{})
        out.push_back(std::move(p));
}
}

std::optional<AriaBank> AriaBank::tryLoadFromSfz(const juce::File& sfzFile)
{
    if (! sfzFile.existsAsFile())
        return std::nullopt;

    // If the caller passed a *.bank.xml directly (e.g. the file browser's
    // bank-manifest path), use it as-is. Only fall back to the directory
    // walk for a plain .sfz — that scan can pick the wrong sibling manifest
    // when several live near each other.
    const auto bankFile = sfzFile.getFileName().toLowerCase().endsWith(".bank.xml")
                              ? sfzFile
                              : findBankFileNear(sfzFile);
    if (bankFile == juce::File{} || ! bankFile.existsAsFile())
        return std::nullopt;

    // Plogue ARIA bank files often ship as multi-root XML - a
    // <?xml ?> prolog followed by sibling <Key>... </Key> and
    // <AriaBank>... </AriaBank> elements at the top level. That's
    // not strictly well-formed, so juce::parseXML returns only the
    // first root (the <Key>) and we miss the <AriaBank>. Wrap the
    // file contents in a synthetic root so all siblings become
    // children of one element we can iterate.
    const auto rawText = bankFile.loadFileAsString();
    auto stripped = rawText.trim();
    if (stripped.startsWith("<?xml"))
    {
        const auto closeIdx = stripped.indexOf("?>");
        if (closeIdx >= 0)
            stripped = stripped.substring(closeIdx + 2).trim();
    }
    const auto wrapped = "<AriaBankFile>" + stripped + "</AriaBankFile>";

    auto xml = juce::parseXML(wrapped);
    if (xml == nullptr)
        return std::nullopt;

    juce::XmlElement* root = xml->hasTagName("AriaBank")
                                ? xml.get()
                                : xml->getChildByName("AriaBank");
    if (root == nullptr)
        return std::nullopt;

    AriaBank bank;
    bank.bankFile = bankFile;
    bank.bankDir  = bankFile.getParentDirectory();
    bank.bankName = root->getStringAttribute("name");
    bank.vendor   = root->getStringAttribute("vendor");

    for (auto* child : root->getChildIterator())
        if (child->hasTagName("AriaProgram"))
            parseAriaProgramElement(*child, bank.bankDir, bank.programs);

    bank.selectedIndex = -1;
    const auto target = sfzFile.getFullPathName();
    for (size_t i = 0; i < bank.programs.size(); ++i)
    {
        if (bank.programs[i].sfzFile.getFullPathName() == target)
        {
            bank.selectedIndex = (int) i;
            break;
        }
    }

    return bank;
}
} // namespace duskstudio
