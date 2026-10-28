// The token comparison behind the unsaved-changes question for third-party
// plug-in state: which slot changes count, and how a restore that completes
// after its baseline was taken is acknowledged.

#include <catch2/catch_test_macros.hpp>

#include "engine/PluginStateManifest.h"

#include <array>
#include <vector>

namespace
{
using duskstudio::PluginStateReceipt;
using duskstudio::PluginStateToken;
using duskstudio::pluginStateMoved;
using Baseline = duskstudio::PluginStateManifest<3>;

constexpr PluginStateToken loaded (std::uint64_t generation, std::uint64_t revision)
{
    return { generation, revision, true };
}

constexpr PluginStateToken empty (std::uint64_t generation)
{
    return { generation, 0, false };
}
} // namespace

TEST_CASE ("plug-in state: an occupied slot changes with its owner or its revision", "[session][plugin][dirty]")
{
    CHECK_FALSE (pluginStateMoved (loaded (4, 7), loaded (4, 7)));
    CHECK (pluginStateMoved (loaded (4, 7), loaded (4, 8)));
    // A reload or an undo that restores the same plug-in is another owner,
    // whatever its revision happens to read.
    CHECK (pluginStateMoved (loaded (4, 7), loaded (6, 7)));
    CHECK (pluginStateMoved (empty (4), loaded (6, 0)));
    CHECK (pluginStateMoved (loaded (4, 7), empty (5)));
}

TEST_CASE ("plug-in state: a slot empty at both reads is unchanged", "[session][plugin][dirty]")
{
    CHECK_FALSE (pluginStateMoved (empty (4), empty (4)));
    // Loaded and removed again in between: the session JSON names the slot.
    CHECK_FALSE (pluginStateMoved (empty (4), empty (6)));
}

TEST_CASE ("plug-in state: a baseline differs when any slot changed", "[session][plugin][dirty]")
{
    Baseline baseline;
    baseline.tokens = { loaded (1, 2), empty (3), loaded (5, 0) };
    CHECK_FALSE (baseline.differsFrom ({ loaded (1, 2), empty (9), loaded (5, 0) }));
    CHECK (baseline.differsFrom ({ loaded (1, 2), empty (3), loaded (5, 1) }));
    CHECK (baseline.differsFrom ({ loaded (1, 2), loaded (4, 0), loaded (5, 0) }));
}

TEST_CASE ("plug-in state: a deferred restore is acknowledged only by the baseline it completed under",
           "[session][plugin][dirty]")
{
    std::vector<PluginStateReceipt> receipts;
    Baseline baseline;
    baseline.epoch = 2;
    baseline.tokens = { empty (3), empty (8), loaded (5, 1) };

    SECTION ("the restore's own empty slot takes the restored token")
    {
        receipts.push_back ({ 0, 3, loaded (5, 1) });
        baseline.adopt (2, receipts);
        CHECK (baseline.receipts == 1);
        CHECK_FALSE (baseline.differsFrom ({ loaded (5, 1), empty (8), loaded (5, 1) }));
        // An edit after the restore still counts.
        CHECK (baseline.differsFrom ({ loaded (5, 2), empty (8), loaded (5, 1) }));
    }
    SECTION ("a slot that changed hands since the baseline keeps its own token")
    {
        receipts.push_back ({ 1, 6, loaded (8, 1) });
        receipts.push_back ({ 2, 5, loaded (7, 1) });
        baseline.adopt (2, receipts);
        CHECK (baseline.differsFrom ({ empty (3), loaded (8, 1), loaded (5, 1) }));
        CHECK (baseline.differsFrom ({ empty (3), empty (8), loaded (7, 1) }));
    }
    SECTION ("another session's receipts never apply")
    {
        receipts.push_back ({ 0, 3, loaded (5, 1) });
        baseline.adopt (3, receipts);
        CHECK (baseline.receipts == 0);
        CHECK (baseline.differsFrom ({ loaded (5, 1), empty (8), loaded (5, 1) }));
    }
    SECTION ("each receipt applies once, and only those after the baseline")
    {
        receipts.push_back ({ 0, 3, loaded (5, 1) });
        baseline.receipts = 1;
        baseline.adopt (2, receipts);
        CHECK (baseline.differsFrom ({ loaded (5, 1), empty (8), loaded (5, 1) }));

        receipts.push_back ({ 1, 8, loaded (10, 1) });
        baseline.adopt (2, receipts);
        baseline.tokens[1] = loaded (10, 2);
        baseline.adopt (2, receipts);
        CHECK (baseline.receipts == 2);
        CHECK (baseline.tokens[1].revision == 2);
    }
    SECTION ("a receipt for a slot outside the baseline is skipped")
    {
        receipts.push_back ({ 7, 3, loaded (5, 1) });
        baseline.adopt (2, receipts);
        CHECK (baseline.receipts == 1);
        CHECK_FALSE (baseline.differsFrom ({ empty (3), empty (8), loaded (5, 1) }));
    }
}

TEST_CASE ("plug-in state: a slot a Save could not capture keeps its earlier baseline", "[session][plugin][dirty]")
{
    Baseline previous;
    previous.tokens = { loaded (1, 4), loaded (2, 9), empty (6) };

    Baseline saved;
    saved.tokens = { loaded (1, 7), loaded (2, 12), empty (6) };
    saved.uncaptured = { false, true, false };
    saved.keepUncapturedFrom (previous);

    CHECK (saved.tokens[0].revision == 7);
    CHECK (saved.tokens[1].revision == 9);
    CHECK_FALSE (saved.uncaptured[1]);
    // The failed slot's edits since the earlier Save stay unsaved; the others are saved.
    CHECK (saved.differsFrom ({ loaded (1, 7), loaded (2, 12), empty (6) }));
    CHECK_FALSE (saved.differsFrom ({ loaded (1, 7), loaded (2, 9), empty (6) }));
}
