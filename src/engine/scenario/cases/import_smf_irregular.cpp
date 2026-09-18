#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../FileImporter.h"
#include "../../midi/MidiFileReader.h"
#include "../../../session/Session.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace duskstudio::scenario
{
namespace
{
// The fixtures ship as whitespace-separated hex so a byte-exact malformed file
// survives review and version control.
std::vector<std::uint8_t> decodeHex (const std::filesystem::path& source)
{
    std::vector<std::uint8_t> bytes;
    std::ifstream input (source);
    std::string token;
    while (input >> token)
        bytes.push_back ((std::uint8_t) std::stoul (token, nullptr, 16));
    return bytes;
}

ScenarioResult runImport (ScenarioContext& ctx)
{
    const auto scratch = ctx.tempDir();
    if (scratch.empty())
        return ScenarioResult::fail ("could not create the temporary directory");

    auto materialise = [&] (const char* logical, const char* name)
    {
        const auto bytes = decodeHex (*ctx.fixture (logical));
        const auto target = scratch / name;
        std::ofstream out (target, std::ios::binary | std::ios::trunc);
        out.write ((const char*) bytes.data(), (std::streamsize) bytes.size());
        out.close();
        return target;
    };

    // Session file import goes through the same reader, so the counts and
    // ordering below are the ones the reader's own tests pin.
    using SessionFile = decltype (ctx.session().getSessionDirectory());
    auto importNotes = [&] (const std::filesystem::path& file, int& noteCountOut)
    {
        fileimport::MidiImportRequest request;
        request.source = SessionFile (file.u8string().c_str());
        request.sessionSampleRate = ScenarioContext::kSampleRate;
        request.sessionBpm = 120.0f;
        const auto result = fileimport::importMidi (request);
        noteCountOut = (int) result.region.notes.size();
        if (! result.ok) ctx.note ("import error: " + result.errorMessage);
        return result.ok;
    };

    auto countNoteOns = [] (const midi::MidiFileReader& reader)
    {
        int total = 0;
        for (const auto& track : reader.tracks())
            for (const auto& event : track)
                if (event.isNoteOn()) ++total;
        return total;
    };

    {
        const auto file = materialise ("smf.vendor_chunk", "vendor-chunk.mid");
        midi::MidiFileReader reader;
        if (ctx.expect (reader.readFile (file), "the vendor-chunk file did not parse"))
        {
            if (ctx.expect (reader.tracks().size() == 2,
                        "a vendor chunk was counted as a track"))
            {
                const bool firstKept  = ctx.expect (reader.tracks()[0].size() == 2, "the first track lost events");
                const bool secondKept = ctx.expect (reader.tracks()[1].size() == 2, "the second track lost events");
                if (firstKept)
                    ctx.expect (reader.tracks()[0][0].noteNumber() == 60, "the first track's note changed");
                if (secondKept)
                    ctx.expect (reader.tracks()[1][0].noteNumber() == 64, "the second track's note changed");
            }

            int imported = 0;
            ctx.expect (importNotes (file, imported), "importing the vendor-chunk file failed");
            ctx.expect (imported == countNoteOns (reader),
                    "the import dropped notes the reader found: " + std::to_string (imported)
                        + " of " + std::to_string (countNoteOns (reader)));
        }
    }

    {
        // The header counts the vendor chunk as a track. What parsed before the
        // count runs out has to survive, as it did with the previous reader.
        const auto file = materialise ("smf.vendor_counted", "vendor-chunk-counted.mid");
        midi::MidiFileReader reader;
        if (ctx.expect (reader.readFile (file),
                        "a header that counts its vendor chunk failed the whole file"))
        {
            if (ctx.expect (reader.tracks().size() == 2,
                            "the over-counted file did not keep both tracks")
                && ctx.expect (! reader.tracks()[0].empty() && ! reader.tracks()[1].empty(),
                               "the over-counted file kept a track with no events"))
            {
                ctx.expect (reader.tracks()[0][0].noteNumber() == 60, "the first track's note changed");
                ctx.expect (reader.tracks()[1][0].noteNumber() == 64, "the second track's note changed");
            }

            int imported = 0;
            ctx.expect (importNotes (file, imported), "importing the over-counted file failed");
            ctx.expect (imported == countNoteOns (reader),
                        "the import dropped notes the reader found: " + std::to_string (imported)
                            + " of " + std::to_string (countNoteOns (reader)));
        }
    }

    {
        const auto file = materialise ("smf.same_tick", "same-tick-retrigger.mid");
        midi::MidiFileReader reader;
        if (ctx.expect (reader.readFile (file), "the same-tick file did not parse"))
        {
            if (ctx.expect (reader.tracks().size() == 1, "the same-tick file grew a track"))
            {
                const auto& events = reader.tracks()[0];
                if (ctx.expect (events.size() == 7, "the same-tick file lost events"))
                {
                    ctx.expect (events[1].tick == 100 && events[1].isNoteOn()
                                && events[1].noteNumber() == 60,
                            "the unmatched same-tick note-on moved");
                    ctx.expect (events[2].tick == 100 && events[2].isNoteOff()
                                && events[2].noteNumber() == 64,
                            "reordering stopped after the unmatched note-on");
                    ctx.expect (events[3].tick == 100 && events[3].isNoteOn()
                                && events[3].noteNumber() == 64,
                            "the retriggered note lost its ordering");
                }
            }

            int imported = 0;
            ctx.expect (importNotes (file, imported), "importing the same-tick file failed");
            ctx.expect (imported == countNoteOns (reader),
                    "the import dropped notes the reader found: " + std::to_string (imported)
                        + " of " + std::to_string (countNoteOns (reader)));
        }
    }

    return ctx.verdict();
}

const ScenarioRegistrar registrar { Scenario {
    "import.smf_irregular",
    { "import", "midi" },
    Needs::Engine,
    { "smf.vendor_chunk", "smf.same_tick", "smf.vendor_counted" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runImport (ctx); }
} };
} // namespace
} // namespace duskstudio::scenario
