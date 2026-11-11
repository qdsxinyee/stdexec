// test/netexec/test_netexec_cancellation.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "test_helpers.hpp"

#include <optional>
#include <thread>
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
        net::ip::tcp::async_receive(socket, net::buffer(buffer))
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
            net::ip::tcp::async_send(socket, net::const_buffer(chunk.data(), chunk.size()))
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

auto receive_with_external_stop(
    netexec::scope&          scope,
    std::uint16_t        port,
    ex::inplace_stop_token token) -> exec::task<void> {
    net::ip::tcp::socket socket(scope.get_context(), netexec_test::make_server_endpoint(port));
    co_await net::ip::tcp::async_connect(socket);

    char buffer[64];
    co_await (net::ip::tcp::async_receive(socket, net::buffer(buffer))
              | ex::write_env(ex::prop{ex::get_stop_token, token}));
}

} // namespace

TEST_CASE("netexec - pending receive cancelled by timeout", "[netexec][cancellation]") {
    netexec::scope scope;
    auto       port = netexec_test::next_port();
    auto       ep = netexec_test::make_server_endpoint(port);
    net::ip::tcp::acceptor acceptor(scope.get_context(), ep.protocol());
    acceptor.bind(ep);
    acceptor.listen();
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

TEST_CASE("netexec - pre-stopped timer completes stopped inline", "[netexec][cancellation]") {
    net::io_context       context;
    ex::inplace_stop_source stop_source;
    stop_source.request_stop();
    bool stopped = false;

    ex::sync_wait(
        net::resume_after(context.get_scheduler(), 10s)
        | ex::upon_stopped([&stopped]() noexcept { stopped = true; })
        | ex::write_env(ex::prop{ex::get_stop_token, stop_source.get_token()}));

    CHECK(stopped);
}

TEST_CASE("netexec - timer cancelled from another thread", "[netexec][cancellation]") {
    netexec::scope          scope;
    ex::inplace_stop_source stop_source;
    bool                stopped = false;

    ex::spawn(
        net::resume_after(scope.get_scheduler(), 10s)
            | ex::upon_stopped([&stopped]() noexcept { stopped = true; })
            | ex::upon_error([](auto&&) noexcept {})
            | ex::write_env(ex::prop{ex::get_stop_token, stop_source.get_token()}),
        scope.get_token());
    std::jthread stopper([&stop_source] {
        std::this_thread::sleep_for(50ms);
        stop_source.request_stop();
    });

    const auto started = std::chrono::steady_clock::now();
    ex::sync_wait(scope.run());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    stopper.join();
    CHECK(stopped);
    // Without a working cross-thread wake-up the loop only notices the request
    // when the 10s timer expires.
    CHECK(elapsed < 5s);
}

TEST_CASE("netexec - pending receive cancelled from another thread", "[netexec][cancellation]") {
    netexec::scope          scope;
    ex::inplace_stop_source stop_source;
    auto                port = netexec_test::next_port();
    auto                ep   = netexec_test::make_server_endpoint(port);
    net::ip::tcp::acceptor acceptor(scope.get_context(), ep.protocol());
    acceptor.bind(ep);
    acceptor.listen();
    bool stopped = false;
    std::optional<net::ip::tcp::socket> peer;

    ex::spawn(
        net::ip::tcp::async_accept(acceptor)
            | ex::then([&peer](auto client, auto&&...) noexcept {
                  peer.emplace(std::move(client));
              })
            | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());
    ex::spawn(
        receive_with_external_stop(scope, port, stop_source.get_token())
            | ex::upon_stopped([&]() noexcept {
                  stopped = true;
                  // The IOCP context keeps run() alive while sockets are open;
                  // close everything so the loop can drain.
                  std::error_code ec;
                  acceptor.close(ec);
                  peer.reset();
              })
            | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());
    std::jthread stopper([&stop_source] {
        std::this_thread::sleep_for(100ms);
        stop_source.request_stop();
    });

    ex::sync_wait(scope.run());
    stopper.join();
    CHECK(stopped);
}

TEST_CASE("netexec - completion and external cancellation complete exactly once", "[netexec][cancellation]") {
    for (int i = 0; i < 25; ++i) {
        netexec::scope          scope;
        ex::inplace_stop_source stop_source;
        int                 completions = 0;

        ex::spawn(
            net::resume_after(scope.get_scheduler(), 2ms)
                | ex::then([&completions]() noexcept { ++completions; })
                | ex::upon_stopped([&completions]() noexcept { ++completions; })
                | ex::upon_error([](auto&&) noexcept {})
                | ex::write_env(ex::prop{ex::get_stop_token, stop_source.get_token()}),
            scope.get_token());
        std::jthread stopper([&stop_source] {
            std::this_thread::sleep_for(1ms);
            stop_source.request_stop();
            stop_source.request_stop();
        });

        ex::sync_wait(scope.run());
        stopper.join();
        CHECK(completions == 1);
    }
}

TEST_CASE("netexec - pending send cancelled by timeout", "[netexec][cancellation]") {
    netexec::scope scope;
    auto       port = netexec_test::next_port();
    auto       ep = netexec_test::make_server_endpoint(port);
    net::ip::tcp::acceptor acceptor(scope.get_context(), ep.protocol());
    acceptor.bind(ep);
    acceptor.listen();
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
