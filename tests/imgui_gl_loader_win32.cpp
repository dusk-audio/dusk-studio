#include <catch2/catch_test_macros.hpp>

#include "ui/WglProcAddressSentinel.h"

TEST_CASE ("Windows GL loader rejects every documented failure sentinel", "[notepad][windows]")
{
    using duskstudio::glloader::isInvalidWglProcAddressValue;

    REQUIRE (isInvalidWglProcAddressValue (0));
    REQUIRE (isInvalidWglProcAddressValue (1));
    REQUIRE (isInvalidWglProcAddressValue (2));
    REQUIRE (isInvalidWglProcAddressValue (3));
    REQUIRE (isInvalidWglProcAddressValue (-1));

    REQUIRE_FALSE (isInvalidWglProcAddressValue (4));
    REQUIRE_FALSE (isInvalidWglProcAddressValue (0x1000));
}
