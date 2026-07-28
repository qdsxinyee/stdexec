// test/netexec/test_let_async_scope.cpp
// Minimal compile/run tests for stdexec::let_async_scope.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "test_helpers.hpp"

#include <stdexcept>

namespace
{

  // Pass the environment as a temporary to ex::write_env.  MSVC Debug builds
  // otherwise report C4700 (uninitialized local variable) for a named env
  // object and can hang at runtime.

  TEST_CASE("let_async_scope - forwards value after spawned work", "[let_async_scope]")
  {
    bool ran   = false;
    auto sched = ex::inline_scheduler{};

    auto result = ex::sync_wait(
      ex::write_env(ex::just(1)
                      | ex::let_async_scope(
                        [&](auto token, int value)
                        {
                          ex::spawn(ex::just() | ex::then([&]() noexcept { ran = true; }
                              ), token);
                          return ex::just(value + 1);
    }),
                    ex::env{ex::prop{ex::get_scheduler, sched},
                            ex::prop{ex::get_start_scheduler, sched}}));

    REQUIRE(result.has_value());
    REQUIRE(std::get<0>(*result) == 2);
    REQUIRE(ran);
  }

  TEST_CASE("let_async_scope - records spawned error", "[let_async_scope]")
  {
    auto sched = ex::inline_scheduler{};

    REQUIRE_THROWS_AS(ex::sync_wait(
                        ex::write_env(ex::just()
                                        | ex::let_async_scope(
                                          [&](auto token)
                                          {
                                            ex::spawn(ex::just_error(std::runtime_error("boom")),
                                                      token);
                                            return ex::just(42);
    }),
                                      ex::env{ex::prop{ex::get_scheduler, sched},
                                              ex::prop{ex::get_start_scheduler, sched}})),
                      std::runtime_error);
  }

  TEST_CASE("let_async_scope_with_error - custom error type", "[let_async_scope]")
  {
    bool ran   = false;
    auto sched = ex::inline_scheduler{};

    auto result = ex::sync_wait(
      ex::write_env(ex::just()
                      | ex::let_async_scope_with_error<std::runtime_error>(
                        [&](auto token) noexcept
                        {
                          ex::spawn(ex::just() | ex::then([&]() noexcept { ran = true; }
                              ), token);
                          return ex::just(42);
    }),
                    ex::env{ex::prop{ex::get_scheduler, sched},
                            ex::prop{ex::get_start_scheduler, sched}}));

    REQUIRE(result.has_value());
    REQUIRE(std::get<0>(*result) == 42);
    REQUIRE(ran);
  }

  TEST_CASE("let_async_scope_with_error - records custom error", "[let_async_scope]")
  {
    auto sched = ex::inline_scheduler{};

    REQUIRE_THROWS_AS(ex::sync_wait(
                        ex::write_env(ex::just()
                                        | ex::let_async_scope_with_error<std::runtime_error>(
                                          [&](auto token) noexcept
                                          {
                                            ex::spawn(ex::just_error(std::runtime_error("boom")),
                                                      token);
                                            return ex::just(42);
    }),
                                      ex::env{ex::prop{ex::get_scheduler, sched},
                                              ex::prop{ex::get_start_scheduler, sched}})),
                      std::runtime_error);
  }

  // P3296R6: the callable may return void instead of a sender; the scope sender
  // then has a void value completion.
  TEST_CASE("let_async_scope - function returning void", "[let_async_scope]")
  {
    bool ran   = false;
    auto sched = ex::inline_scheduler{};

    auto result = ex::sync_wait(
      ex::write_env(ex::just()
                      | ex::let_async_scope(
                        [&](auto token)
                        {
                          ex::spawn(ex::just() | ex::then([&]() noexcept { ran = true; }
                              ), token);
                          // no return statement: the function returns void
    }),
                    ex::env{ex::prop{ex::get_scheduler, sched},
                            ex::prop{ex::get_start_scheduler, sched}}));

    REQUIRE(result.has_value());
    REQUIRE(ran);
  }

  // P3296R6: an empty error list denotes a "noexcept" scope. The function must be
  // declared noexcept.
  TEST_CASE("let_async_scope_with_error - empty error list (noexcept scope)", "[let_async_scope]")
  {
    bool ran   = false;
    auto sched = ex::inline_scheduler{};

    auto result = ex::sync_wait(
      ex::write_env(ex::just()
                      | ex::let_async_scope_with_error<>(
                        [&](auto token) noexcept
                        {
                          ex::spawn(ex::just() | ex::then([&]() noexcept { ran = true; }
                              ), token);
                          return ex::just(42);
    }),
                    ex::env{ex::prop{ex::get_scheduler, sched},
                            ex::prop{ex::get_start_scheduler, sched}}));

    REQUIRE(result.has_value());
    REQUIRE(std::get<0>(*result) == 42);
    REQUIRE(ran);
  }

  // P3296R6: let-async-scope-env is computed with FWD-ENV, so for an inline
  // predecessor (no completion scheduler of its own) the scope env derives the
  // scheduler from the receiver's environment.
  TEST_CASE("let_async_scope - derives scope scheduler from receiver env", "[let_async_scope]")
  {
    auto sched = ex::inline_scheduler{};

    auto scope_env = ex::__let_async_scope::__make_let_scope_env(ex::just(),
                                                                 ex::env{
                                                                   ex::prop{ex::get_start_scheduler,
                                                                            sched}
    });

    // The derived scope env exposes the receiver env's start scheduler.
    static_assert(
      std::is_same_v<decltype(ex::get_start_scheduler(scope_env)), ex::inline_scheduler>);
    REQUIRE(ex::get_start_scheduler(scope_env) == sched);
  }

  // P3296R6: if the function passed to let_async_scope throws, the scope is
  // requested to stop and the exception is delivered as set_error after the
  // scope has drained.
  TEST_CASE("let_async_scope - function exception stops scope and propagates", "[let_async_scope]")
  {
    auto sched = ex::inline_scheduler{};

    REQUIRE_THROWS_AS(ex::sync_wait(
                        ex::write_env(ex::just()
                                        | ex::let_async_scope(
                                          [&](auto token)
                                          {
                                            ex::spawn(ex::just() | ex::then([]() noexcept { }
                                                ),
                                                      token);
                                            throw std::runtime_error("boom");
    }),
                                      ex::env{ex::prop{ex::get_scheduler, sched},
                                              ex::prop{ex::get_start_scheduler, sched}})),
                      std::runtime_error);
  }

}  // namespace
