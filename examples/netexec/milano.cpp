// milano.cpp
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
    {"/", "data/index-milano.html"},
    {"/favicon.ico", "data/favicon.ico"},
    {"/logo.png", "data/logo.png"},
    {"/itcpp.png", "data/itcpp.png"},
};

auto run_client(auto client) -> exec::task<void> {
    std::cout << "new client\n";
    char buffer[8194];
    try {
        while (true) {
            auto n = co_await net::ip::tcp::async_receive(client, net::buffer(buffer));
            if (n == 0u)
                co_return;
            std::istringstream in(std::string(buffer, n));
            std::string        method, url, version;
            if (!(in >> method >> url >> version))
                co_return;
            auto it = files.find(url);
            std::cout << "url=" << url << " found=" << (it == files.end() ? "no" : "yes") << "\n";
            std::string content;
            if (it != files.end()) {
                std::ifstream fin(it->second, std::ios::binary);
                std::ostringstream out;
                out << fin.rdbuf();
                content = out.str();
            }
            std::ostringstream out;
            out << "HTTP/1.1 200 found\r\n"
                << "Content-Length: " << content.size() << "\r\n"
                << "\r\n"
                << content;

            content = out.str();
            co_await net::ip::tcp::async_send(client, net::buffer(content));
        }
    } catch (const std::exception& ex) {
        std::cout << "exception: " << ex.what() << "\n";
    }
    std::cout << "client done\n";
}

auto main() -> int {
    std::cout << std::unitbuf;

    netexec::scope scope;

    ex::spawn(
        [](netexec::scope& scp) -> exec::task<void> {
            net::ip::tcp::endpoint ep(net::ip::address_v4::any(), 12345);
            net::ip::tcp::acceptor acceptor(scp.get_context(), ep.protocol());
            co_await net::ip::tcp::async_listen(acceptor, ep);
            while (true) {
                std::cout << "awaiting a connection\n";
                auto [client, address] = co_await net::ip::tcp::async_accept(acceptor);
                std::cout << "received a connection from " << address << "\n";
                ex::spawn(run_client(std::move(client)) | ex::upon_error([](auto&&) noexcept {}),
                          scp.get_token());
            }
        }(scope) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
}
