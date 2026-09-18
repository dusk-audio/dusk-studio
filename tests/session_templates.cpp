#include <catch2/catch_test_macros.hpp>

#include "session/SessionTemplates.h"

#include <set>
#include <string>

using namespace duskstudio;

// The startup picker builds its list by walking this enum and asking for each
// label, so an unnamed or duplicated entry would reach the user as a blank or
// ambiguous row rather than as a compile error.
TEST_CASE ("every session template has a distinct label", "[session][templates]")
{
    std::set<std::string> labels;
    for (int i = 0; i < (int) SessionTemplate::kCount; ++i)
    {
        const std::string label = nameForTemplate ((SessionTemplate) i);
        CHECK_FALSE (label.empty());
        labels.insert (label);
    }
    CHECK (labels.size() == (size_t) SessionTemplate::kCount);
}

TEST_CASE ("applying a template names every track", "[session][templates]")
{
    for (int i = 0; i < (int) SessionTemplate::kCount; ++i)
    {
        Session session;
        applyTemplate (session, (SessionTemplate) i);

        for (int t = 0; t < Session::kNumTracks; ++t)
            CHECK_FALSE (session.track (t).name.isEmpty());
    }
}

TEST_CASE ("templates differ from Blank where they name instruments", "[session][templates]")
{
    Session blank;
    applyTemplate (blank, SessionTemplate::Blank);

    Session band;
    applyTemplate (band, SessionTemplate::Band);

    CHECK (blank.track (0).name != band.track (0).name);
}
