// counting_scope_server.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// This example is the manual-counting_scope counterpart of
// let_async_scope_server.cpp. It does exactly the same thing (accept one TCP
// connection on port 12348 and echo back every byte), but explicitly creates
// and joins a stdexec::counting_scope instead of using let_async_scope.
//
// Comparing the two files shows what let_async_scope automates:
//   - creating an internal counting_scope,
//   - passing a scope token to the user callback,
//   - joining the scope before completing.

#include <netexec/net.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include <iostream>
#include <string>

namespace ex  = stdexec;
namespace net = netexec;

// Handle one client: read bytes and echo them back until the peer closes.
auto echo_client(auto client) -> exec::task<void> {
    try {
        char buffer[64];
        while (auto n = co_await net::async_receive(client, net::buffer(buffer))) {
            co_await net::async_send(client, net::const_buffer(buffer, n));
        }
        std::cout << "client disconnected\n";
    } catch (const std::exception& e) {
        std::cerr << "client error: " << e.what() << "\n";
    }
}

// Accept one connection and spawn an echo handler into the scope.
auto accept_client(net::io_context& ctx, stdexec::counting_scope::token token, auto& env)
    -> exec::task<void> {
    net::ip::tcp::endpoint endpoint(net::ip::address_v4::any(), 12348);
    net::ip::tcp::acceptor acceptor(ctx, endpoint);

    std::cout << "counting_scope echo server listening on " << endpoint << "\n";

    auto [stream, address] = co_await net::async_accept(acceptor);
    std::cout << "connection from " << address << "\n";

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

        stdexec::counting_scope scope;

        // Manually spawn the accept loop and the IO runner into the scope,
        // explicitly wrap each task with write_env, and then sync_wait on
        // scope.join() to drain all spawned work.
        ex::spawn(
            ex::write_env(
                accept_client(ctx, scope.get_token(), env)
                    | ex::upon_error([](auto&&) noexcept {}),
                env),
            scope.get_token());

        // Drive the IO context and the counting scope together, just like
        // cat_image_server.cpp does.
        ex::sync_wait(ex::write_env(
            ex::when_all(ctx.async_run(), scope.join()),
            env));
    } catch (const std::exception& e) {
        std::cerr << "server error: " << e.what() << "\n";
        return 1;
    }
}
