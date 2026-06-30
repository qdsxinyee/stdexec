// client.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <netexec/net.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>
#include <iostream>
#include <string_view>

namespace ex = stdexec;
namespace net = netexec::net;

auto main() -> int {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    netexec::scope scope;

    ex::spawn(
        [](netexec::scope& scp) -> exec::task<void> {
            try {
                net::ip::tcp::endpoint ep(net::ip::address_v4::loopback(), 12345);
                net::ip::tcp::socket   client(scp.get_context(), ep);

                auto connected = co_await (net::ip::tcp::async_connect(client) |
                                           ex::then([](auto&&...) { return true; }) |
                                           ex::upon_error([](auto&&) { return false; }));
                if (!connected) {
                    std::cout << "connect failed\n";
                    co_return;
                }
                std::cout << "connected\n";

                char message[] = "hello, world\n";
                co_await net::ip::tcp::async_send_some(client, net::buffer(message));

                char buffer[20];
                while (auto size = co_await net::ip::tcp::async_receive_some(client, net::buffer(buffer))) {
                    std::string_view response(buffer, size);
                    std::cout << "received='" << response << "'\n";
                    if (response.find('\n') != response.npos) {
                        break;
                    }
                }
                std::cout << "done\n";
            } catch (const std::exception& e) {
                std::cerr << "error: " << e.what() << "\n";
            }
        }(scope) | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
}
