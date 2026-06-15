// test/netexec/test_helpers.hpp
// Common helpers for netexec tests.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <netexec/net.hpp>
#include <exec/task.hpp>
#include <exec/when_any.hpp>
#include <stdexec/execution.hpp>
#include <catch2/catch_all.hpp>

#include <chrono>
#include <cstdint>
#include <string>

namespace ex = stdexec;
namespace net = netexec;
using namespace std::chrono_literals;

namespace netexec_test {

// Each call returns a different loopback port so sequential tests don't
// interfere with each other if one test fails to clean up.
inline auto next_port() -> std::uint16_t {
    static std::uint16_t port = 54321;
    return port++;
}

inline auto make_server_endpoint(std::uint16_t port) -> net::ip::tcp::endpoint {
    return net::ip::tcp::endpoint(net::ip::address_v4::loopback(), port);
}

} // namespace netexec_test
