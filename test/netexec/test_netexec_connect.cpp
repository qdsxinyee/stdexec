// test/netexec/test_netexec_connect.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "test_helpers.hpp"

namespace {

auto connect_and_close(netexec::scope& scope, std::uint16_t port, bool* ok) -> exec::task<void> {
    net::ip::tcp::socket socket(scope.get_context(), netexec_test::make_server_endpoint(port));
    co_await net::ip::tcp::async_connect(socket);
    *ok = true;
}

auto connect_refused(netexec::scope& scope, std::uint16_t port, bool* refused) -> exec::task<void> {
    net::ip::tcp::socket socket(scope.get_context(), netexec_test::make_server_endpoint(port));
    *refused = co_await (net::ip::tcp::async_connect(socket)
                           | ex::then([] { return false; })
                           | ex::upon_error([](auto&&) { return true; }));
}

auto serve_one_client(netexec::scope& scope, std::uint16_t port, bool* served) -> exec::task<void> {
    net::ip::tcp::acceptor acceptor(scope.get_context(), netexec_test::make_server_endpoint(port).protocol());
    co_await net::ip::tcp::async_listen(acceptor, netexec_test::make_server_endpoint(port));
    auto [client, client_ep] = co_await net::ip::tcp::async_accept(acceptor);
    *served = true;
    (void)client;
    (void)client_ep;
}

} // namespace

TEST_CASE("netexec - async_connect succeeds", "[netexec][connect]") {
    netexec::scope scope;
    auto       port = netexec_test::next_port();
    bool       connected = false;
    bool       served = false;

    ex::spawn(
        serve_one_client(scope, port, &served) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());
    ex::spawn(
        connect_and_close(scope, port, &connected) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
    CHECK(connected);
    CHECK(served);
}

TEST_CASE("netexec - async_connect refused", "[netexec][connect]") {
    netexec::scope scope;
    bool       refused = false;

    ex::spawn(
        connect_refused(scope, netexec_test::next_port(), &refused)
            | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
    CHECK(refused);
}
