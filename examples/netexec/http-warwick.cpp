// http-warwick.cpp
// Ported from beman/net examples to netexec (stdexec-based networking).
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <netexec/net.hpp>
#include <exec/when_any.hpp>      // exec::when_any
#include <stdexec/execution.hpp>  // stdexec::spawn, stdexec::then, ...

#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <system_error>

namespace ex  = stdexec;
namespace net = netexec::net;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------

std::unordered_map<std::string, std::string> files{
    {"/",                "data/index-warwick.html"},
    {"/favicon.ico",     "data/favicon.ico"},
    {"/logo.png",        "data/warwick-logo.png"},
    {"/warwick-qr1.png", "data/warwick-qr1.png"},
    {"/warwick-qr2.png", "data/warwick-qr2.png"},
};

// ---------------------------------------------------------------------------
// process_request — handle one HTTP GET, send response
// ---------------------------------------------------------------------------

auto process_request(auto& stream, std::string request) -> exec::task<void> {
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
    out << "HTTP/1.1 " << (it == files.end() ? "404 not found" : "200 found\r\n")
        << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    auto response = out.str();

    co_await net::ip::tcp::async_send_some(stream, net::buffer(response));
}

// ---------------------------------------------------------------------------
// timeout — race a sender against a timer; timer win → throw std::error_code
// ---------------------------------------------------------------------------

auto timeout(auto scheduler, auto duration, auto sender) {
    auto timer_branch =
        net::resume_after(scheduler, duration)
        | ex::then([]() -> std::size_t {
            throw std::system_error(std::make_error_code(std::errc::timed_out));
          });

    return exec::when_any(std::move(sender), std::move(timer_branch));
}

// ---------------------------------------------------------------------------
// make_client — read HTTP request from a connected socket, with timeout
// ---------------------------------------------------------------------------

auto make_client(auto scheduler, auto stream) -> exec::task<void> {
    char        buffer[16];
    std::string request;
    try {
        while (auto n = co_await timeout(scheduler, 3s,
                                         net::ip::tcp::async_receive_some(stream, net::buffer(buffer)))) {
            std::string_view sv(buffer, n);
            request += sv;
            if (request.npos != sv.find("\r\n\r\n")) {
                co_await process_request(stream, std::move(request));
                break;
            }
        }
    } catch (...) {
        std::cout << "ex: timeout\n";
    }
    std::cout << "client done\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

auto main() -> int {
    netexec::scope             scope;
    net::ip::tcp::endpoint ep(net::ip::address_v4::any(), 12345);
    net::ip::tcp::acceptor server(scope.get_context(), ep);
    std::cout << "listening on " << ep << "\n";

    ex::spawn(
        std::invoke(
            [](auto scheduler, netexec::scope& scp, auto& svr) -> exec::task<void> {
                while (true) {
                    auto [stream, address] = co_await net::ip::tcp::async_accept(svr);
                    std::cout << "received connection from " << address << "\n";
                    ex::spawn(
                        make_client(scheduler, std::move(stream))
                            | ex::upon_error([](auto&&) noexcept {}),
                        scp.get_token());
                }
            },
            scope.get_scheduler(),
            scope,
            server)
            | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
}
