// test/netexec/test_netexec_resolve.cpp
// Tests for hostname resolution and IP-address parsing.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "test_helpers.hpp"

#include <netexec/net/tls.hpp>

#include <algorithm>

namespace {

auto resolve_to_vector(
    netexec::io_context&                 ctx,
    const std::string&                   hostname,
    std::uint16_t                        port,
    std::vector<net::ip::tcp::endpoint>* out,
    bool*                                ok) -> exec::task<void> {
    *out = co_await net::ip::tcp::async_resolve(ctx, hostname, port);
    *ok  = !out->empty();
}

auto connect_by_preconnection(
    netexec::io_context&                    ctx,
    const netexec::net::tls::preconnection& pre,
    bool*                                   connected) -> exec::task<void> {
    auto stream = co_await netexec::net::tls::async_initiate(pre, ctx);
    *connected  = true;
    (void)stream;
}

auto serve_one_client(netexec::scope& scope, std::uint16_t port, bool* served) -> exec::task<void> {
    net::ip::tcp::acceptor acceptor(
        scope.get_context(), netexec_test::make_server_endpoint(port).protocol());
    co_await net::ip::tcp::async_listen(acceptor, netexec_test::make_server_endpoint(port));
    auto [client, client_ep] = co_await net::ip::tcp::async_accept(acceptor);
    *served = true;
    (void)client;
    (void)client_ep;
}

} // namespace

TEST_CASE("netexec - async_resolve localhost", "[netexec][resolve]") {
    netexec::scope scope;
    auto           port = netexec_test::next_port();

    std::vector<net::ip::tcp::endpoint> endpoints;
    bool                                ok = false;

    ex::spawn(
        resolve_to_vector(scope.get_context(), "localhost", port, &endpoints, &ok)
            | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
    CHECK(ok);
    CHECK(!endpoints.empty());
}

TEST_CASE("netexec - async_initiate resolves hostname", "[netexec][resolve]") {
    netexec::scope scope;
    auto           port = netexec_test::next_port();
    bool           connected = false;
    bool           served    = false;

    ex::spawn(
        serve_one_client(scope, port, &served) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    netexec::net::tls::preconnection pre(stdexec::env{
        net::hostname(std::string("localhost")),
        net::port(port),
        netexec::net::tls::secure(false)});
    ex::spawn(
        connect_by_preconnection(scope.get_context(), pre, &connected)
            | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
    CHECK(served);
    CHECK(connected);
}
