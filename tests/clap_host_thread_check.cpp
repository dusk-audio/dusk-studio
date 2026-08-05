// The host's thread-check extension: a message thread that temporarily
// designates itself the audio thread (deactivate() calling [audio-thread]
// stop_processing) must hand the stamp back, or it answers true to both
// is_audio_thread and is_main_thread for the rest of the instance's life.

#include <catch2/catch_test_macros.hpp>

#include "engine/clap/ClapHost.h"

#include <thread>

using duskstudio::clap::ClapHost;

TEST_CASE ("CLAP host thread-check stamps and un-stamps the audio thread", "[clap][host]")
{
    ClapHost host;
    const auto* h = host.get();
    const auto* check = static_cast<const clap_host_thread_check_t*> (
        h->get_extension (h, CLAP_EXT_THREAD_CHECK));
    REQUIRE (check != nullptr);

    SECTION ("a fresh host treats the constructing thread as main, not audio")
    {
        REQUIRE (check->is_main_thread (h));
        REQUIRE_FALSE (check->is_audio_thread (h));
    }

    SECTION ("the process() stamp designates the calling thread")
    {
        host.setAudioThread (std::this_thread::get_id());
        REQUIRE (check->is_audio_thread (h));
    }

    SECTION ("a borrowed stamp is handed back")
    {
        // deactivate()'s shape on a slot the audio thread never started.
        const auto borrowed = host.exchangeAudioThread (std::this_thread::get_id());
        REQUIRE (check->is_audio_thread (h));

        host.restoreAudioThread (borrowed);
        REQUIRE_FALSE (check->is_audio_thread (h));
        REQUIRE (check->is_main_thread (h));
    }

    SECTION ("restore returns the previous owner's stamp, not a cleared one")
    {
        // This thread stands in for the prior owner, so the stamp being handed
        // back is provably non-zero - restoring a fresh host's 0 would pass the
        // section above either way. std::thread::id {} is the borrower here;
        // the direction doesn't matter, only that the exact hash comes back.
        host.setAudioThread (std::this_thread::get_id());

        const auto borrowed = host.exchangeAudioThread (std::thread::id {});
        REQUIRE_FALSE (check->is_audio_thread (h));

        host.restoreAudioThread (borrowed);
        REQUIRE (check->is_audio_thread (h));
        REQUIRE (host.exchangeAudioThread (std::thread::id {}) == borrowed);
    }
}
