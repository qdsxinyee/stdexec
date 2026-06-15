// test/netexec/test_netexec_acceptor.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "test_helpers.hpp"

namespace {

auto run_acceptor(net::scope& scope, std::uint16_t port, bool* accepted) -> exec::task<void> {
    net::ip::tcp::acceptor acceptor(scope.get_context(), netexec_test::make_server_endpoint(port));
    net::ip::tcp::socket   client(scope.get_context(), netexec_test::make_server_endpoint(port));
    co_await net::async_connect(client);
    auto [server, server_ep] = co_await net::async_accept(acceptor);
    *accepted = true;
    (void)server_ep;
}

} // namespace

TEST_CASE("netexec - acceptor accepts one client", "[netexec][acceptor]") {
    net::scope scope;
    bool       accepted = false;

    ex::spawn(
        run_acceptor(scope, netexec_test::next_port(), &accepted)
            | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
    CHECK(accepted);
}

TEST_CASE("netexec - acceptor is move constructible", "[netexec][acceptor]") {
    net::scope scope;
    net::ip::tcp::acceptor a(scope.get_context(), netexec_test::make_server_endpoint(netexec_test::next_port()));
    net::ip::tcp::acceptor b(std::move(a));
    CHECK(b.is_open());
}
