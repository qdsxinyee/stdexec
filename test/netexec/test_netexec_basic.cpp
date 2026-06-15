// test/netexec/test_netexec_basic.cpp
// Basic Catch2 tests for the netexec networking layer.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "test_helpers.hpp"

namespace {

auto resume_and_set(net::io_context::scheduler_type scheduler, bool* fired) -> exec::task<void> {
    co_await net::resume_after(scheduler, 20ms);
    *fired = true;
}

} // namespace

TEST_CASE("netexec - schedule on context scheduler", "[netexec]") {
    net::scope scope;
    bool       called = false;

    ex::spawn(
        ex::schedule(scope.get_scheduler()) | ex::then([&]() noexcept { called = true; }),
        scope.get_token());

    ex::sync_wait(scope.run());
    CHECK(called);
}

TEST_CASE("netexec - resume_after fires", "[netexec]") {
    net::scope scope;
    bool       fired = false;

    ex::spawn(
        resume_and_set(scope.get_scheduler(), &fired) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
    CHECK(fired);
}
