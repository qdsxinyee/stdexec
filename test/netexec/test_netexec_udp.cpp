// test/netexec/test_netexec_udp.cpp
// UDP placeholder. netexec currently does not expose a public UDP protocol type.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "test_helpers.hpp"

TEST_CASE("netexec - udp support not implemented", "[netexec][udp]") {
    // UDP protocol support is not yet implemented in netexec. Keep a passing
    // placeholder so the test list remains stable.
    INFO("UDP protocol support is not yet implemented in netexec");
    CHECK(true);
}
