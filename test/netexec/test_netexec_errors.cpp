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
    net::ip::tcp::acceptor first(scope.get_context(), ep.protocol());
    first.bind(ep);
    first.listen();

    bool failed = false;
    try {
        net::ip::tcp::acceptor second(scope.get_context(), ep.protocol());
        second.bind(ep);
        second.listen();
    } catch (const std::system_error& e) {
#ifdef _WIN32
        failed = (e.code().value() == WSAEADDRINUSE);
#else
        failed = (e.code() == std::errc::address_in_use);
#endif
    }

    CHECK(failed);
}

TEST_CASE("netexec - connection reset is not graceful EOF", "[netexec][errors]") {
    netexec::scope scope;
    auto       port = netexec_test::next_port();
    auto       ep   = netexec_test::make_server_endpoint(port);
    net::ip::tcp::acceptor acceptor(scope.get_context(), ep.protocol());
    acceptor.bind(ep);
    acceptor.listen();
    std::error_code receive_error;

    auto reset_peer = [](net::ip::tcp::acceptor server) -> exec::task<void> {
        auto [client, address] = co_await net::ip::tcp::async_accept(server);
        auto handle = client.get_scheduler().get_context()->native_handle(client.id());
        ::linger option{.l_onoff = 1, .l_linger = 0};
        if (::setsockopt(
                handle,
                SOL_SOCKET,
                SO_LINGER,
                reinterpret_cast<const char*>(&option),
                static_cast<::socklen_t>(sizeof(option))) != 0) {
            throw std::system_error(sock_errno(), std::system_category(), "setsockopt(SO_LINGER) failed");
        }
    };
    auto receive_reset = [](netexec::scope& scope, std::uint16_t port, std::error_code* error) -> exec::task<void> {
        net::ip::tcp::socket socket(scope.get_context(), netexec_test::make_server_endpoint(port));
        co_await net::ip::tcp::async_connect(socket);
        char buffer[16];
        try {
            co_await net::ip::tcp::async_receive(socket, net::buffer(buffer));
        } catch (const std::system_error& ec) {
            *error = ec.code();
        }
    };

    ex::spawn(
        std::invoke(reset_peer, std::move(acceptor)) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());
    ex::spawn(
        std::invoke(receive_reset, scope, port, &receive_error) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
    REQUIRE(receive_error);
#ifdef _WIN32
    CHECK((receive_error.value() == WSAECONNRESET || receive_error.value() == ERROR_NETNAME_DELETED));
#else
    CHECK(receive_error == std::errc::connection_reset);
#endif
}
