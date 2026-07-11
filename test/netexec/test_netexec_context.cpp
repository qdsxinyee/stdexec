// test/netexec/test_netexec_context.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "test_helpers.hpp"

TEST_CASE("netexec - empty scope exits", "[netexec][context]") {
    netexec::scope scope;
    ex::sync_wait(scope.run());
    CHECK(true);
}

TEST_CASE("netexec - schedule runs on context scheduler", "[netexec][context]") {
    netexec::scope scope;
    bool       called = false;

    ex::spawn(
        ex::schedule(scope.get_scheduler()) | ex::then([&]() noexcept { called = true; }),
        scope.get_token());

    ex::sync_wait(scope.run());
    CHECK(called);
}

TEST_CASE("netexec - multiple spawned tasks run", "[netexec][context]") {
    netexec::scope scope;
    int        count = 0;

    for (int i = 0; i < 3; ++i) {
        ex::spawn(
            ex::schedule(scope.get_scheduler()) | ex::then([&]() noexcept { ++count; }),
            scope.get_token());
    }

    ex::sync_wait(scope.run());
    CHECK(count == 3);
}
