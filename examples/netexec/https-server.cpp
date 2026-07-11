// https-server.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Minimal HTTPS server using the netexec TAPS API and the Windows Schannel
// TLS backend.  It generates a self-signed certificate on first use so that
// https://localhost:8443/ can be opened in a browser (browsers will warn about
// the untrusted issuer).
//
// This example mirrors examples/netexec/http-server.cpp; the only intentional
// differences are the higher-level netexec::net::tls API (TLS) versus the
// lower-level netexec::net::ip::tcp API (plain TCP).

#include <netexec/net.hpp>
#include <netexec/net/tls.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace ex  = stdexec;
namespace net = netexec::net;
namespace tls = netexec::net::tls;

// ---------------------------------------------------------------------------

std::unordered_map<std::string, std::string> files{
    {"/",            "data/index.html"},
    {"/favicon.ico", "data/favicon.ico"},
    {"/logo.png",    "data/logo.png"},
};

// ---------------------------------------------------------------------------
// process_request — handle one HTTP GET, send response
// ---------------------------------------------------------------------------

auto process_request(auto& strm, std::string request) -> exec::task<void> {
    std::istringstream in(request);
    std::string        method, url, version;
    if (!(in >> method >> url >> version) || method != "GET") {
        std::cout << "not a [supported] HTTP request\n";
        co_return;
    }
    auto it = files.find(url);
    std::cout << "url='" << url << "' -> "
              << (it == files.end() ? "not found" : it->second) << "\n";

    std::ostringstream out;
    out << std::ifstream(it == files.end() ? std::string() : it->second, std::ios::binary).rdbuf();
    auto body = out.str();
    out.clear();
    out.str({});

    auto content_type = [](std::string_view url) -> std::string_view {
        if (url.ends_with(".png")) {
            return "image/png";
        }
        if (url.ends_with(".ico")) {
            return "image/x-icon";
        }
        return "text/html";
    };

    if (it == files.end()) {
        out << "HTTP/1.1 404 Not Found\r\n"
            << "Content-Length: 0\r\n"
            << "Connection: close\r\n\r\n";
    } else {
        out << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: " << content_type(url) << "\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n\r\n"
            << body;
    }
    auto response = out.str();

    // TLS send is message-based and loops internally until the whole message
    // is encrypted and written to the TCP socket.
    std::cout << "process_request: sending response, length=" << response.size() << "\n" << std::flush;
    co_await tls::async_send(strm, net::message{response});
    std::cout << "sent " << response.size() << " bytes for " << url << "\n" << std::flush;
}

// ---------------------------------------------------------------------------
// make_client — read one HTTP request from a TLS stream, then reply and close
// ---------------------------------------------------------------------------

auto make_client(tls::stream strm) -> exec::task<void> {
    char        buffer[1024];
    std::string request;
    try {
        std::cout << "client: waiting for request\n" << std::flush;
        while (request.npos == request.find("\r\n\r\n")) {
            std::cout << "client: calling async_receive_some\n" << std::flush;
            auto n = co_await tls::async_receive_some(strm, net::buffer(buffer));
            std::cout << "client: received " << n << " bytes\n" << std::flush;
            if (n == 0) {
                std::cout << "client: peer closed connection before request complete\n" << std::flush;
                co_return;
            }
            request.append(buffer, n);
        }
        std::cout << "client: request complete, length=" << request.size() << "\n" << std::flush;
        co_await process_request(strm, std::move(request));
        try {
            co_await tls::async_shutdown(strm);
        } catch (...) {
            // Ignore shutdown errors; the response has already been sent.
        }
    } catch (const std::exception& e) {
        std::cerr << "client error: " << e.what() << "\n" << std::flush;
    }
    std::cout << "client done\n" << std::flush;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

// Accept loop: own a TLS acceptor and spawn a client handler for each new connection.
auto accept_loop(netexec::scope& scp, tls::acceptor acc, const char* name) -> exec::task<void> {
    while (true) {
        std::cout << name << " acceptor: waiting for connection\n" << std::flush;
        try {
            auto strm = co_await tls::async_accept(acc);
            std::cout << "received TLS connection on " << name << "\n" << std::flush;
            ex::spawn(
                make_client(std::move(strm)) | ex::upon_error([](auto&&) noexcept {}),
                scp.get_token());
        } catch (const std::exception& e) {
            std::cerr << name << " accept/handshake failed: " << e.what() << "\n" << std::flush;
        }
    }
}

// Set up the dual-stack listeners and hand their ownership off to accept loops.
auto run_server(netexec::scope& scope) -> exec::task<void> {
    // To use your own certificate instead of the self-signed fallback, add
    // tls::certificate("server.crt") and tls::private_key("server.key") here.
    //
    // Create two preconnections (IPv4 + IPv6) so browsers that resolve
    // "localhost" to ::1 can connect while still serving IPv4 clients.
    tls::preconnection pre_v4(ex::env{
        net::hostname("localhost"),
        net::port(8443),
        net::ip_address("0.0.0.0")});
    tls::preconnection pre_v6(ex::env{
        net::hostname("localhost"),
        net::port(8443),
        net::ip_address("::")});

    // Use the TAPS API to bind, listen, and wrap the raw acceptors with TLS.
    // tls::async_listen also warms up the TLS context so certificate
    // generation happens at startup rather than on the first client.
    auto acceptor_v4 = co_await tls::async_listen(pre_v4, scope.get_context());
    auto acceptor_v6 = co_await tls::async_listen(pre_v6, scope.get_context());

    std::cout << "listening on https://localhost:" << pre_v4.port() << "/ (IPv4 + IPv6)\n" << std::flush;

    ex::spawn(
        accept_loop(scope, std::move(acceptor_v4), "IPv4") | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());
    ex::spawn(
        accept_loop(scope, std::move(acceptor_v6), "IPv6") | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

auto main() -> int {
    try {
        netexec::scope scope;

        ex::spawn(
            run_server(scope) | ex::upon_error([](auto&&) noexcept {}),
            scope.get_token());

        ex::sync_wait(scope.run());
    } catch (const std::exception& e) {
        std::cerr << "exception: " << e.what() << "\n";
        return 1;
    }
}
