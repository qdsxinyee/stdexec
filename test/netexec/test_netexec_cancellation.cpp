// test/netexec/test_netexec_cancellation.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "test_helpers.hpp"

#include <optional>
#include <vector>

namespace {

auto receive_with_timeout(
    netexec::scope&                          scope,
    std::uint16_t                        port,
    bool*                                timed_out,
    net::ip::tcp::acceptor*              acceptor,
    std::optional<net::ip::tcp::socket>* peer) -> exec::task<void> {
    net::ip::tcp::socket socket(scope.get_context(), netexec_test::make_server_endpoint(port));
    co_await net::ip::tcp::async_connect(socket);

    char buffer[64];
    *timed_out = co_await exec::when_any(
        net::ip::tcp::async_receive_some(socket, net::buffer(buffer))
            | ex::then([](std::size_t) noexcept { return false; }),
        net::resume_after(scope.get_scheduler(), 50ms)
            | ex::then([acceptor, peer]() noexcept {
                  std::error_code ec;
                  acceptor->close(ec);
                  peer->reset(); // close peer socket -> client receive completes
                  return true;
              }));
}

auto send_with_timeout(
    netexec::scope&                          scope,
    std::uint16_t                        port,
    bool*                                timed_out,
    net::ip::tcp::acceptor*              acceptor,
    std::optional<net::ip::tcp::socket>* peer) -> exec::task<void> {
    net::ip::tcp::socket socket(scope.get_context(), netexec_test::make_server_endpoint(port));
    co_await net::ip::tcp::async_connect(socket);

    // Keep sending until the send pends and the timeout fires.
    std::vector<char> chunk(16 * 1024 * 1024, 'x');
    for (int i = 0; i < 200; ++i) {
        bool this_timeout = co_await exec::when_any(
            net::ip::tcp::async_send_some(socket, net::const_buffer(chunk.data(), chunk.size()))
                | ex::then([](std::size_t) noexcept { return false; }),
            net::resume_after(scope.get_scheduler(), 10ms)
                | ex::then([acceptor, peer]() noexcept {
                      std::error_code ec;
                      acceptor->close(ec);
                      peer->reset(); // close peer socket -> client send fails
                      return true;
                  }));
        if (this_timeout) {
            *timed_out = true;
            break;
        }
    }
}

} // namespace

TEST_CASE("netexec - pending receive cancelled by timeout", "[netexec][cancellation]") {
    netexec::scope scope;
    auto       port = netexec_test::next_port();
    net::ip::tcp::acceptor acceptor(scope.get_context(), netexec_test::make_server_endpoint(port));
    bool timed_out = false;

    // Keep the accepted peer socket alive (but never sending) so the client receive pends.
    std::optional<net::ip::tcp::socket> peer;
    ex::spawn(
        net::ip::tcp::async_accept(acceptor)
            | ex::then([&peer](auto client, auto&&...) noexcept {
                  peer.emplace(std::move(client));
              })
            | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::spawn(
        receive_with_timeout(scope, port, &timed_out, &acceptor, &peer)
            | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
    CHECK(timed_out);
}

TEST_CASE("netexec - pending send cancelled by timeout", "[netexec][cancellation]") {
    netexec::scope scope;
    auto       port = netexec_test::next_port();
    net::ip::tcp::acceptor acceptor(scope.get_context(), netexec_test::make_server_endpoint(port));
    bool timed_out = false;

    // Keep the accepted peer socket alive and not reading so the client send pends.
    std::optional<net::ip::tcp::socket> peer;
    ex::spawn(
        net::ip::tcp::async_accept(acceptor)
            | ex::then([&peer](auto client, auto&&...) noexcept {
                  peer.emplace(std::move(client));
              })
            | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::spawn(
        send_with_timeout(scope, port, &timed_out, &acceptor, &peer)
            | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
    CHECK(timed_out);
}
