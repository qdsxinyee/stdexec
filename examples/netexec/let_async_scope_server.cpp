// let_async_scope_server.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// A minimal stdexec + netexec example that uses stdexec::let_async_scope for
// structured concurrency.
//
// The server accepts one TCP connection on port 12347 and echoes back every
// byte it receives. When the client disconnects, the echo handler finishes,
// the internal counting_scope created by let_async_scope drains, and the
// program exits cleanly.

#include <netexec/net.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include <iostream>
#include <string>

namespace ex  = stdexec;
namespace net = netexec::net;

// Handle one client: read bytes and echo them back until the peer closes.
auto echo_client(auto client) -> exec::task<void> {
    try {
        char buffer[64];
        while (auto n = co_await net::ip::tcp::async_receive(client, net::buffer(buffer))) {
            co_await net::ip::tcp::async_send(client, net::const_buffer(buffer, n));
        }
        std::cout << "client disconnected\n";
    } catch (const std::exception& e) {
        std::cerr << "client error: " << e.what() << "\n";
    }
}

// Accept one connection and spawn an echo handler for it. The token type is
// left generic so it can be either a stdexec::counting_scope token or the
// custom token produced by let_async_scope.
auto accept_client(net::io_context& ctx, auto token, auto& env) -> exec::task<void> {
    net::ip::tcp::endpoint endpoint(net::ip::address_v4::any(), 12347);
    net::ip::tcp::acceptor acceptor(ctx, endpoint.protocol());
    co_await net::ip::tcp::async_listen(acceptor, endpoint);

    std::cout << "let_async_scope echo server listening on " << endpoint << "\n";

    auto [stream, address] = co_await net::ip::tcp::async_accept(acceptor);
    std::cout << "connection from " << address << "\n";

    // Spawned senders that need a scheduler (exec::task in this case) are
    // explicitly wrapped with write_env. let_async_scope still owns the
    // lifetime of everything spawned through `token`.
    ex::spawn(
        ex::write_env(
            echo_client(std::move(stream)) | ex::upon_error([](auto&&) noexcept {}),
            env),
        token);
}

auto main() -> int {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    try {
        net::io_context ctx;
        auto scheduler = ctx.get_scheduler();

        auto env = ex::env{
            ex::prop{ex::get_scheduler, scheduler},
            ex::prop{ex::get_start_scheduler, scheduler}};

        // let_async_scope creates an internal counting_scope. The lambda receives
        // a scope token; everything spawned through that token is joined before
        // the returned sender completes. If any spawned work errors, the recorded
        // error is delivered to sync_wait after the scope has drained.
        ex::sync_wait(ex::write_env(
            ex::just()
                | ex::let_async_scope([&](auto token) {
                      ex::spawn(
                          ex::write_env(
                              accept_client(ctx, token, env)
                                  | ex::upon_error([](auto&&) noexcept {}),
                              env),
                          token);
                      ex::spawn(ex::write_env(ctx.async_run(), env), token);
                      return ex::just();
                  }),
            env));
    } catch (const std::exception& e) {
        std::cerr << "server error: " << e.what() << "\n";
        return 1;
    }
}
