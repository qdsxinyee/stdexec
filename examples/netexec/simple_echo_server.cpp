// simple_echo_server.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// A minimal stdexec + netexec example:
//   - accept one TCP connection at a time
//   - echo back whatever the client sends
//   - stop when the client disconnects
//
// This shows the core paradigm: coroutines + senders/receivers + structured
// concurrency via netexec::scope.

#include <netexec/net.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include <iostream>
#include <string>

namespace ex = stdexec;
namespace net = netexec;

// Handle one client: read bytes and send them back until the client closes.
auto echo_client(auto client) -> exec::task<void> {
    char buffer[64];
    while (auto n = co_await net::async_receive(client, net::buffer(buffer))) {
        co_await net::async_send(client, net::const_buffer(buffer, n));
    }
    std::cout << "client disconnected\n";
}

// Accept loop: wait for connections and spawn an echo handler for each.
auto accept_loop(net::scope& scope) -> exec::task<void> {
    net::ip::tcp::endpoint endpoint(net::ip::address_v4::any(), 12346);
    net::ip::tcp::acceptor acceptor(scope.get_context(), endpoint);

    std::cout << "echo server listening on " << endpoint << "\n";

    while (true) {
        auto [stream, address] = co_await net::async_accept(acceptor);
        std::cout << "connection from " << address << "\n";

        // Fire-and-forget the client handler into the scope.
        ex::spawn(
            echo_client(std::move(stream)) | ex::upon_error([](auto&&) noexcept {}),
            scope.get_token());
    }
}

int main() {
    std::cout << std::unitbuf;

    net::scope scope;

    // Spawn the accept loop into the scope, then run everything.
    ex::spawn(
        accept_loop(scope) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
}
