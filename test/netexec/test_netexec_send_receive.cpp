// test/netexec/test_netexec_send_receive.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "test_helpers.hpp"

#include <string>
#include <string_view>

namespace {

auto echo_server(net::ip::tcp::acceptor acceptor, std::string* received, std::size_t expected) -> exec::task<void> {
    auto [client, addr] = co_await net::async_accept(acceptor);
    std::error_code ec;
    acceptor.close(ec);

    received->clear();
    char buffer[64];
    while (received->size() < expected) {
        auto n = co_await net::async_receive(client, net::buffer(buffer));
        if (n == 0) {
            break;
        }
        received->append(buffer, n);
    }

    co_await net::async_send(client, net::const_buffer(received->data(), received->size()));
}

auto echo_client(net::scope& scope, std::uint16_t port, std::string* response) -> exec::task<void> {
    net::ip::tcp::socket socket(scope.get_context(), netexec_test::make_server_endpoint(port));
    co_await net::async_connect(socket);

    co_await net::async_send(socket, net::const_buffer("hello", 5));

    char buffer[64];
    auto n = co_await net::async_receive(socket, net::buffer(buffer));
    response->assign(buffer, n);
}

auto multiple_send_client(net::scope& scope, std::uint16_t port, std::string* response) -> exec::task<void> {
    net::ip::tcp::socket socket(scope.get_context(), netexec_test::make_server_endpoint(port));
    co_await net::async_connect(socket);

    co_await net::async_send(socket, net::const_buffer("abc", 3));
    co_await net::async_send(socket, net::const_buffer("def", 3));
    co_await net::async_send(socket, net::const_buffer("ghi", 3));

    char buffer[64];
    auto n = co_await net::async_receive(socket, net::buffer(buffer));
    response->assign(buffer, n);
}

auto close_after_accept(net::ip::tcp::acceptor acceptor) -> exec::task<void> {
    auto [client, addr] = co_await net::async_accept(acceptor);
    std::error_code ec;
    acceptor.close(ec);
    // `client` is destroyed on return, closing the connection gracefully.
}

auto receive_until_close(net::scope& scope, std::uint16_t port, std::size_t* received) -> exec::task<void> {
    net::ip::tcp::socket socket(scope.get_context(), netexec_test::make_server_endpoint(port));
    co_await net::async_connect(socket);

    char buffer[64];
    *received = co_await net::async_receive(socket, net::buffer(buffer));
}

} // namespace

TEST_CASE("netexec - send and receive echo", "[netexec][send_receive]") {
    net::scope scope;
    auto       port = netexec_test::next_port();
    net::ip::tcp::acceptor acceptor(scope.get_context(), netexec_test::make_server_endpoint(port));
    std::string received;
    std::string response;

    ex::spawn(
        echo_server(std::move(acceptor), &received, 5) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::spawn(
        echo_client(scope, port, &response) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
    CHECK(received == "hello");
    CHECK(response == "hello");
}

TEST_CASE("netexec - receive zero bytes on graceful close", "[netexec][send_receive]") {
    net::scope scope;
    auto       port = netexec_test::next_port();
    net::ip::tcp::acceptor acceptor(scope.get_context(), netexec_test::make_server_endpoint(port));
    std::size_t received = 42;

    ex::spawn(
        close_after_accept(std::move(acceptor)) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::spawn(
        receive_until_close(scope, port, &received) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
    CHECK(received == 0u);
}

TEST_CASE("netexec - multiple sends are received", "[netexec][send_receive]") {
    net::scope scope;
    auto       port = netexec_test::next_port();
    net::ip::tcp::acceptor acceptor(scope.get_context(), netexec_test::make_server_endpoint(port));
    std::string received;
    std::string response;

    ex::spawn(
        echo_server(std::move(acceptor), &received, 9) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::spawn(
        multiple_send_client(scope, port, &response) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
    CHECK(received == "abcdefghi");
    CHECK(response == "abcdefghi");
}
