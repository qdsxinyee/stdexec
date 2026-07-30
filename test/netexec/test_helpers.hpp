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

#if defined(_WIN32)
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace ex = stdexec;
namespace net = netexec::net;
using namespace std::chrono_literals;

namespace netexec_test {

// Each call returns a different loopback port so sequential tests don't
// interfere with each other if one test fails to clean up.  The base is
// derived from the process id because ctest runs every TEST_CASE in its own
// process, potentially in parallel: a fixed base would make concurrent
// processes collide on the same ports.
inline auto next_port() -> std::uint16_t {
#if defined(_WIN32)
    static const std::uint16_t base = static_cast<std::uint16_t>(20000u + (::_getpid() % 20000u));
#else
    static const std::uint16_t base = static_cast<std::uint16_t>(20000u + (::getpid() % 20000u));
#endif
    static std::uint16_t port = base;
    return port++;
}

inline auto make_server_endpoint(std::uint16_t port) -> net::ip::tcp::endpoint {
    return net::ip::tcp::endpoint(net::ip::address_v4::loopback(), port);
}

} // namespace netexec_test
