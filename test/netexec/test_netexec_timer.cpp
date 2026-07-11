// test/netexec/test_netexec_timer.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "test_helpers.hpp"

#include <chrono>

TEST_CASE("netexec - resume_after fires", "[netexec][timer]") {
    netexec::scope scope;
    bool       fired = false;
    auto       start = std::chrono::steady_clock::now();

    ex::spawn(
        net::resume_after(scope.get_scheduler(), 20ms)
            | ex::then([&]() noexcept { fired = true; })
            | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
    auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(fired);
    CHECK(elapsed >= 15ms);
}

TEST_CASE("netexec - resume_at fires", "[netexec][timer]") {
    netexec::scope scope;
    bool       fired = false;
    auto       at = std::chrono::system_clock::now() + 20ms;

    ex::spawn(
        net::resume_at(scope.get_scheduler(), at)
            | ex::then([&]() noexcept { fired = true; })
            | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
    CHECK(fired);
}

TEST_CASE("netexec - when_any timeout beats slow timer", "[netexec][timer]") {
    netexec::scope scope;
    bool       fast_fired = false;

    auto slow = net::resume_after(scope.get_scheduler(), 10s)
                | ex::then([]() noexcept { return false; });
    auto fast = net::resume_after(scope.get_scheduler(), 20ms)
                | ex::then([&]() noexcept {
                      fast_fired = true;
                      return true;
                  });

    ex::spawn(
        exec::when_any(std::move(slow), std::move(fast))
            | ex::then([](auto&&...) noexcept {})
            | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
    CHECK(fast_fired);
}
