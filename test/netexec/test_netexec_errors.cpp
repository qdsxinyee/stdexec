// test/netexec/test_netexec_errors.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "test_helpers.hpp"

#include <system_error>

namespace {

auto connect_result(netexec::scope& scope, std::uint16_t port, bool* out) -> exec::task<void> {
    net::ip::tcp::socket socket(scope.get_context(), netexec_test::make_server_endpoint(port));
    *out = co_await (net::ip::tcp::async_connect(socket)
                       | ex::then([] { return true; })
                       | ex::upon_error([](auto&&) { return false; }));
}

} // namespace

TEST_CASE("netexec - async_connect refused", "[netexec][errors]") {
    netexec::scope scope;
    auto       port = netexec_test::next_port();
    bool       connected = false;

    ex::spawn(
        connect_result(scope, port, &connected) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
    CHECK_FALSE(connected);
}

TEST_CASE("netexec - address_in_use on duplicate bind", "[netexec][errors]") {
    netexec::scope scope;
    auto       port = netexec_test::next_port();
    net::ip::tcp::endpoint ep(net::ip::address_v4::loopback(), port);
    net::ip::tcp::acceptor first(scope.get_context(), ep);

    bool failed = false;
    try {
        net::ip::tcp::acceptor second(scope.get_context(), ep, false);
    } catch (const std::system_error& e) {
#ifdef _WIN32
        failed = (e.code().value() == WSAEADDRINUSE);
#else
        failed = (e.code() == std::errc::address_in_use);
#endif
    }

    CHECK(failed);
}
