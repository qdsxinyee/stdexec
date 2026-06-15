// test/netexec/test_netexec_poll_specific.cpp
// Poll/WSAPoll-backend specific tests. Only compiled when neither IOCP nor io_uring is selected.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "test_helpers.hpp"

#if !defined(NET_EXEC_USE_IOCP) && !defined(NET_EXEC_USE_URING)

#include <string>
#include <string_view>

namespace {

auto poll_echo_server(net::ip::tcp::acceptor acceptor, std::string* received) -> exec::task<void> {
    auto [client, addr] = co_await net::async_accept(acceptor);
    std::error_code ec;
    acceptor.close(ec);

    char buf[64];
    auto n = co_await net::async_receive(client, net::buffer(buf));
    received->assign(buf, n);

    co_await net::async_send(client, net::const_buffer(received->data(), received->size()));
}

auto poll_echo_client(net::scope& scope, std::uint16_t port) -> exec::task<void> {
    net::ip::tcp::socket socket(scope.get_context(), netexec_test::make_server_endpoint(port));
    co_await (net::async_connect(socket) | ex::upon_error([](auto&&) {}));

    co_await net::async_send(socket, net::const_buffer("poll", 4));

    char buf[64];
    auto n = co_await net::async_receive(socket, net::buffer(buf));
    CHECK(std::string_view(buf, n) == "poll");
}

} // namespace

TEST_CASE("netexec poll - echo works", "[netexec][poll]") {
    net::scope scope;
    auto       port = netexec_test::next_port();
    net::ip::tcp::acceptor acceptor(scope.get_context(), netexec_test::make_server_endpoint(port));
    std::string received;

    ex::spawn(
        poll_echo_server(std::move(acceptor), &received) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::spawn(
        poll_echo_client(scope, port) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
    CHECK(received == "poll");
}

TEST_CASE("netexec poll - timer fires", "[netexec][poll]") {
    net::scope scope;
    bool       fired = false;

    ex::spawn(
        net::resume_after(scope.get_scheduler(), 20ms)
            | ex::then([&]() noexcept { fired = true; })
            | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
    CHECK(fired);
}

#endif // !NET_EXEC_USE_IOCP && !NET_EXEC_USE_URING
