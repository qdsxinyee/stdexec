// cat_image_server.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Example TCP server using stdexec::counting_scope + netexec directly.
// Accepts connections, reads a minimal HTTP GET request, and replies with a
// cat image from examples/netexec/data/itcpp.png.

#include <netexec/net.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

namespace ex  = stdexec;
namespace net = netexec::net;

// ---------------------------------------------------------------------------
// Read the whole contents of a binary file into a std::string.
// ---------------------------------------------------------------------------
auto read_file(const char* path) -> std::string {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

// ---------------------------------------------------------------------------
// Handle one client: read a request header, then send a PNG image.
// ---------------------------------------------------------------------------
auto cat_image_client(auto client) -> exec::task<void> {
    try {
        // Read until we see the end of the HTTP header.
        char        buffer[1024];
        std::string request;
        while (auto n = co_await net::ip::tcp::async_receive_some(client, net::buffer(buffer))) {
            request.append(buffer, n);
            if (request.find("\r\n\r\n") != std::string::npos) {
                break;
            }
        }

        // Build an HTTP response with the local cat image.
        auto body = read_file("data/itcpp.png");

        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n"
                 << "Content-Type: image/png\r\n"
                 << "Content-Length: " << body.size() << "\r\n"
                 << "Connection: close\r\n"
                 << "\r\n"
                 << body;

        auto response_str = response.str();
        co_await net::ip::tcp::async_send_some(client, net::buffer(response_str));
    } catch (const std::exception& e) {
        std::cerr << "client error: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// Accept loop.
// ---------------------------------------------------------------------------
auto run_server(net::io_context& ctx, stdexec::counting_scope::token token) -> exec::task<void> {
    net::ip::tcp::endpoint endpoint(net::ip::address_v4::any(), 12345);
    net::ip::tcp::acceptor server(ctx, endpoint);

    std::cout << "cat image server listening on " << endpoint << "\n";

    while (true) {
        auto [stream, address] = co_await net::ip::tcp::async_accept(server);
        std::cout << "connection from " << address << "\n";

        auto sched = ctx.get_scheduler();
        ex::spawn(
            ex::write_env(
                cat_image_client(std::move(stream)) | ex::upon_error([](auto&&) noexcept {}),
                ex::env{ex::prop{ex::get_scheduler, sched}, ex::prop{ex::get_start_scheduler, sched}}),
            token);
    }
}

// ---------------------------------------------------------------------------
// Main.
// ---------------------------------------------------------------------------
auto main() -> int {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    try {
        net::io_context       ctx;
        stdexec::counting_scope scope;

        auto scheduler = ctx.get_scheduler();

        auto server_env =
            ex::env{ex::prop{ex::get_scheduler, scheduler}, ex::prop{ex::get_start_scheduler, scheduler}};

        ex::spawn(
            ex::write_env(
                run_server(ctx, scope.get_token()) | ex::upon_error([](auto&&) noexcept {}),
                server_env),
            scope.get_token());

        // Drive the IO context and the counting scope together.
        ex::sync_wait(ex::write_env(
            ex::when_all(ctx.async_run(), scope.join()),
            server_env));
    } catch (const std::exception& e) {
        std::cerr << "server error: " << e.what() << "\n";
        return 1;
    }
}
