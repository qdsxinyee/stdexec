// server.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <netexec/net.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>
#include <iostream>
#include <string_view>

namespace ex = stdexec;
namespace net = netexec::net;

auto make_client(auto client) -> exec::task<void> {
    try {
        char buffer[8];
        while (auto size = co_await net::ip::tcp::async_receive_some(client, net::buffer(buffer))) {
            std::string_view message(buffer, size);
            std::cout << "received<" << size << ">(" << message << ")\n";
            auto ssize = co_await net::ip::tcp::async_send_some(client, net::const_buffer(buffer, size));
            std::cout << "sent<" << ssize << "/" << message.size() << ">(" << std::string_view(buffer, ssize) << ")\n";
        }
        std::cout << "client done\n";
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
    }
}

auto main() -> int {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    std::cout << "example server\n";

    try {
        netexec::scope scope;

        ex::spawn(
            [](netexec::scope& scp) -> exec::task<void> {
                net::ip::tcp::endpoint endpoint(net::ip::address_v4::any(), 12345);
                net::ip::tcp::acceptor acceptor(scp.get_context(), endpoint);

                while (true) {
                    auto [stream, ep] = co_await net::ip::tcp::async_accept(acceptor);
                    std::cout << "received connection from " << ep << "\n";
                    ex::spawn(make_client(std::move(stream)) | ex::upon_error([](auto&&) noexcept {}),
                              scp.get_token());
                }
            }(scope) | ex::upon_error([](auto&&) noexcept {}),
            scope.get_token());

        ex::sync_wait(scope.run());
    } catch (const std::exception& ex) {
        std::cout << "EXCEPTION: " << ex.what() << "\n";
        abort();
    }
}
