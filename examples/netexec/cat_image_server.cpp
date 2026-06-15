// cat_image_server.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Example TCP server using stdexec + netexec.
// Accepts connections, reads a minimal HTTP GET request, and replies with a
// cat image from examples/netexec/data/itcpp.png.
//
// This maps the user's pseudo-code to netexec's API:
//   - netexec::scope  == io_context + counting_scope
//   - scope.get_context() gives the io_context
//   - scope.get_token() gives the counting_scope token
//   - scope.run()     == when_all(context.run(), scope.join())

#include <netexec/net.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

namespace ex = stdexec;
namespace net = netexec;

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
        char buffer[1024];
        std::string request;
        while (auto n = co_await net::async_receive(client, net::buffer(buffer))) {
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
        co_await net::async_send(client, net::buffer(response_str));
    } catch (const std::exception& e) {
        std::cerr << "client error: " << e.what() << "\n";
    }
}

// ---------------------------------------------------------------------------
// Accept loop.
// ---------------------------------------------------------------------------
auto run_server(net::scope& scope) -> exec::task<void> {
    net::ip::tcp::endpoint endpoint(net::ip::address_v4::any(), 12345);
    net::ip::tcp::acceptor server(scope.get_context(), endpoint);

    std::cout << "cat image server listening on " << endpoint << "\n";

    while (true) {
        auto [stream, address] = co_await net::async_accept(server);
        std::cout << "connection from " << address << "\n";

        ex::spawn(
            cat_image_client(std::move(stream)) | ex::upon_error([](auto&&) noexcept {}),
            scope.get_token());
    }
}

// ---------------------------------------------------------------------------
// Main.
// ---------------------------------------------------------------------------
auto main() -> int {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    try {
        net::scope scope;

        ex::spawn(
            run_server(scope) | ex::upon_error([](auto&&) noexcept {}),
            scope.get_token());

        ex::sync_wait(scope.run());
    } catch (const std::exception& e) {
        std::cerr << "server error: " << e.what() << "\n";
        return 1;
    }
}
