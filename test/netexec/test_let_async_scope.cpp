// test/netexec/test_let_async_scope.cpp
// Minimal compile/run tests for stdexec::let_async_scope.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "test_helpers.hpp"

#include <stdexcept>

namespace {

// Pass the environment as a temporary to ex::write_env.  MSVC Debug builds
// otherwise report C4700 (uninitialized local variable) for a named env
// object and can hang at runtime.

TEST_CASE("let_async_scope - forwards value after spawned work", "[let_async_scope]") {
    bool ran = false;
    auto sched = ex::inline_scheduler{};

    auto result = ex::sync_wait(ex::write_env(
        ex::just(1)
            | ex::let_async_scope([&](auto token, int value) {
                  ex::spawn(
                      ex::just() | ex::then([&]() noexcept { ran = true; }),
                      token);
                  return ex::just(value + 1);
              }),
        ex::env{ex::prop{ex::get_scheduler, sched},
                ex::prop{ex::get_start_scheduler, sched}}));

    REQUIRE(result.has_value());
    REQUIRE(std::get<0>(*result) == 2);
    REQUIRE(ran);
}

TEST_CASE("let_async_scope - records spawned error", "[let_async_scope]") {
    auto sched = ex::inline_scheduler{};

    REQUIRE_THROWS_AS(
        ex::sync_wait(ex::write_env(
            ex::just()
                | ex::let_async_scope([&](auto token) {
                      ex::spawn(ex::just_error(std::runtime_error("boom")), token);
                      return ex::just(42);
                  }),
            ex::env{ex::prop{ex::get_scheduler, sched},
                    ex::prop{ex::get_start_scheduler, sched}})),
        std::runtime_error);
}

TEST_CASE("let_async_scope_with_error - custom error type", "[let_async_scope]") {
    bool ran = false;
    auto sched = ex::inline_scheduler{};

    auto result = ex::sync_wait(ex::write_env(
        ex::just()
            | ex::let_async_scope_with_error<std::runtime_error>([&](auto token) {
                  ex::spawn(
                      ex::just() | ex::then([&]() noexcept { ran = true; }),
                      token);
                  return ex::just(42);
              }),
        ex::env{ex::prop{ex::get_scheduler, sched},
                ex::prop{ex::get_start_scheduler, sched}}));

    REQUIRE(result.has_value());
    REQUIRE(std::get<0>(*result) == 42);
    REQUIRE(ran);
}

TEST_CASE("let_async_scope_with_error - records custom error", "[let_async_scope]") {
    auto sched = ex::inline_scheduler{};

    REQUIRE_THROWS_AS(
        ex::sync_wait(ex::write_env(
            ex::just()
                | ex::let_async_scope_with_error<std::runtime_error>([&](auto token) {
                      ex::spawn(ex::just_error(std::runtime_error("boom")), token);
                      return ex::just(42);
                  }),
            ex::env{ex::prop{ex::get_scheduler, sched},
                    ex::prop{ex::get_start_scheduler, sched}})),
        std::runtime_error);
}

} // namespace
