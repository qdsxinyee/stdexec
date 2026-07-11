// test/netexec/test_netexec_iocp_specific.cpp
// IOCP-backend specific tests. Only compiled when NET_EXEC_USE_IOCP is defined.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "test_helpers.hpp"

#ifdef NET_EXEC_USE_IOCP

#include <string>
#include <string_view>

namespace {

auto iocp_echo_server(net::ip::tcp::acceptor acceptor, std::string* received) -> exec::task<void> {
    auto [client, addr] = co_await net::ip::tcp::async_accept(acceptor);
    std::error_code ec;
    acceptor.close(ec);

    char buf[64];
    auto n = co_await net::ip::tcp::async_receive(client, net::buffer(buf));
    received->assign(buf, n);

    co_await net::ip::tcp::async_send(client, net::const_buffer(received->data(), received->size()));
}

auto iocp_echo_client(netexec::scope& scope, std::uint16_t port) -> exec::task<void> {
    net::ip::tcp::socket socket(scope.get_context(), netexec_test::make_server_endpoint(port));
    co_await (net::ip::tcp::async_connect(socket) | ex::upon_error([](auto&&) {}));

    co_await net::ip::tcp::async_send(socket, net::const_buffer("iocp", 4));

    char buf[64];
    auto n = co_await net::ip::tcp::async_receive(socket, net::buffer(buf));
    CHECK(std::string_view(buf, n) == "iocp");
}

} // namespace

TEST_CASE("netexec iocp - echo works", "[netexec][iocp]") {
    netexec::scope scope;
    auto       port = netexec_test::next_port();
    auto       ep = netexec_test::make_server_endpoint(port);
    net::ip::tcp::acceptor acceptor(scope.get_context(), ep.protocol());
    acceptor.bind(ep);
    acceptor.listen();
    std::string received;

    ex::spawn(
        iocp_echo_server(std::move(acceptor), &received) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::spawn(
        iocp_echo_client(scope, port) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
    CHECK(received == "iocp");
}

TEST_CASE("netexec iocp - timer fires", "[netexec][iocp]") {
    netexec::scope scope;
    bool       fired = false;

    ex::spawn(
        net::resume_after(scope.get_scheduler(), 20ms)
            | ex::then([&]() noexcept { fired = true; })
            | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
    CHECK(fired);
}

#endif // NET_EXEC_USE_IOCP
