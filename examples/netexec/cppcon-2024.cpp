// cppcon-2024.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <netexec/net.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>
#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace ex = stdexec;
namespace net = netexec::net;

std::unordered_map<std::string, std::string> files{
    {"/", "data/index.html"},
    {"/favicon.ico", "data/favicon.ico"},
    {"/logo.png", "data/logo.png"},
};

auto process(auto& stream, const auto& request) -> exec::task<void> {
    std::cout << "request=" << request << "\n";
    std::string        method, url, version;
    std::string        body;
    std::ostringstream out;
    if (std::istringstream(request) >> method >> url >> version && files.contains(url)) {
        std::cout << "url=" << url << "\n";
        std::ifstream fin(files[url], std::ios::binary);
        out << fin.rdbuf();
        body = out.str();
        out.str({});
    }

    out << "HTTP/1.1 " << (body.empty() ? "404 not found" : "200 found") << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "\r\n"
        << body;
    auto response = out.str();
    co_await net::ip::tcp::async_send_some(stream, net::buffer(response));
}

auto make_client_handler(auto stream) -> exec::task<void> {
    char        buffer[16];
    std::string request;
    while (true) {
        auto n = co_await net::ip::tcp::async_receive_some(stream, net::buffer(buffer));
        if (n == 0u)
            break;
        std::string_view data(buffer, n);
        request += data;
        if (request.find("\r\n\r\n") != request.npos) {
            co_await process(stream, request);
            break;
        }
    }
    co_return;
}

auto main() -> int {
    netexec::scope scope;

    net::ip::tcp::endpoint endpoint(net::ip::address_v4::any(), 12345);
    net::ip::tcp::acceptor acceptor(scope.get_context(), endpoint);

    ex::spawn(
        [](netexec::scope& scp, auto& acc) -> exec::task<void> {
            while (true) {
                auto [stream, address] = co_await net::ip::tcp::async_accept(acc);
                std::cout << "received client: " << address << "\n";
                ex::spawn(make_client_handler(std::move(stream)) | ex::upon_error([](auto&&) noexcept {}),
                          scp.get_token());
            }
        }(scope, acceptor) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
}
