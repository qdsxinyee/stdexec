// https-server-trusted.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// HTTPS server that loads a custom server certificate instead of relying on the
// Schannel self-signed fallback. Use this example when you want browsers to
// show a green lock for https://localhost:8443/.
//
// Setup (one-time):
//   1. Generate a development root CA + leaf certificate:
//        python examples/netexec/generate_dev_certs.py
//      This creates:
//        examples/netexec/certs/ca.crt      <-- import this into the OS trust store
//        examples/netexec/certs/server.crt  <-- used by this server
//        examples/netexec/certs/server.key  <-- used by this server
//
//   2. Import certs/ca.crt into your OS / browser trust store:
//        Windows (admin PowerShell):
//          Import-Certificate -FilePath certs/ca.crt -CertStoreLocation Cert:\LocalMachine\Root
//        Linux:
//          sudo cp certs/ca.crt /usr/local/share/ca-certificates/netexec-dev-ca.crt
//          sudo update-ca-certificates
//        macOS:
//          sudo security add-trusted-cert -d -r trustRoot -k /Library/Keychains/System.keychain certs/ca.crt
//
//   3. Run this server from the examples/netexec directory so it can find
//      certs/server.crt and certs/server.key.
//
//   4. Open https://localhost:8443/ in a browser. Because the server presents a
//      certificate signed by your imported dev CA, the page will be trusted.

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
// The leaf certificate is loaded from ./certs/server.crt + ./certs/server.key.
auto run_server(netexec::scope& scope) -> exec::task<void> {
    tls::preconnection pre_v4(ex::env{
        net::hostname("localhost"),
        net::port(8443),
        net::ip_address("0.0.0.0"),
        tls::certificate("certs/server.crt"),
        tls::private_key("certs/server.key")});
    tls::preconnection pre_v6(ex::env{
        net::hostname("localhost"),
        net::port(8443),
        net::ip_address("::"),
        tls::certificate("certs/server.crt"),
        tls::private_key("certs/server.key")});

    auto acceptor_v4 = co_await tls::async_listen(pre_v4, scope.get_context());
    auto acceptor_v6 = co_await tls::async_listen(pre_v6, scope.get_context());

    std::cout << "listening on https://localhost:" << pre_v4.port() << "/ (IPv4 + IPv6)\n" << std::flush;
    std::cout << "using certificate: certs/server.crt\n" << std::flush;

    ex::spawn(
        accept_loop(scope, std::move(acceptor_v4), "IPv4") | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());
    ex::spawn(
        accept_loop(scope, std::move(acceptor_v6), "IPv6") | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());
}

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
