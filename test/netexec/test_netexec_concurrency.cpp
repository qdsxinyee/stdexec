// test/netexec/test_netexec_concurrency.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "test_helpers.hpp"

namespace {

auto accept_n(net::ip::tcp::acceptor acceptor, int n, int* accepted) -> exec::task<void> {
    for (int i = 0; i < n; ++i) {
        auto [client, addr] = co_await net::async_accept(acceptor);
        ++*accepted;
    }
    std::error_code ec;
    acceptor.close(ec);
}

auto client_send(net::scope& scope, std::uint16_t port, int index) -> exec::task<void> {
    net::ip::tcp::socket socket(scope.get_context(), netexec_test::make_server_endpoint(port));
    auto ok = co_await (net::async_connect(socket)
                        | ex::then([] { return true; })
                        | ex::upon_error([](auto&&) { return false; }));
    CHECK(ok);

    char c = static_cast<char>('a' + index);
    co_await net::async_send(socket, net::const_buffer(&c, 1));
}

} // namespace

TEST_CASE("netexec - multiple concurrent clients", "[netexec][concurrency]") {
    constexpr int N = 3;
    net::scope scope;
    auto       port = netexec_test::next_port();
    net::ip::tcp::acceptor acceptor(scope.get_context(), netexec_test::make_server_endpoint(port));
    int accepted = 0;

    ex::spawn(
        accept_n(std::move(acceptor), N, &accepted) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    for (int i = 0; i < N; ++i) {
        ex::spawn(
            client_send(scope, port, i) | ex::upon_error([](auto&&) noexcept {}),
            scope.get_token());
    }

    ex::sync_wait(scope.run());
    CHECK(accepted == N);
}
