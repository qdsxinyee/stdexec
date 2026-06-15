// http-munich.cpp
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
namespace net = netexec;

std::unordered_map<std::string, std::string> files{
    {"/", "data/index-munich.html"},
    {"/favicon.ico", "data/favicon.ico"},
    {"/logo.png", "data/logo.png"},
    {"/muc.png", "data/muc.png"},
};

auto run_client(auto stream) -> exec::task<void> {
    std::cout << "started client\n";
    char buffer[1000];
    while (std::size_t n = co_await net::async_receive(stream, net::buffer(buffer))) {
        std::string request(buffer, n);
        std::istringstream in(request);
        std::string        method, url, version;
        if ((in >> method >> url >> version) && method == "GET" && files.contains(url)) {
            std::ifstream      fin(files[url], std::ios::binary);
            std::ostringstream out;
            out << fin.rdbuf();
            std::string data(out.str());

            out.str(std::string());
            out << "HTTP/1.1 200\r\n"
                << "Content-Length: " << data.size() << "\r\n"
                << "\r\n"
                << data;

            std::string response = out.str();
            co_await net::async_send(stream, net::buffer(response));
        }
    }
    std::cout << "client done\n";
}

auto main() -> int {
    std::cout << std::unitbuf;

    net::scope scope;
    net::ip::tcp::endpoint ep(net::ip::address_v4::any(), 12345);
    net::ip::tcp::acceptor server(scope.get_context(), ep);

    ex::spawn(
        [](net::scope& scp, auto& svr) -> exec::task<void> {
            while (true) {
                std::cout << "ready to receive a client\n";
                auto [client, addr] = co_await net::async_accept(svr);
                std::cout << "received connection from " << addr << "\n";
                ex::spawn(run_client(std::move(client)) | ex::upon_error([](auto&&) noexcept {}),
                          scp.get_token());
            }
        }(scope, server) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
}
