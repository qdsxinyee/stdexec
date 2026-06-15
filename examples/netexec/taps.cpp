// taps.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <netexec/net.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>
#include <iostream>
#include <system_error>
#include <string>

namespace ex = stdexec;
namespace net = netexec;

int main(int, char*[]) {
    std::cout << std::unitbuf;
    net::scope scope;

    std::cout << "spawning\n";
    ex::spawn(
        [&scope]() -> exec::task<void> {
            try {
                net::preconnection pre(net::remote_endpoint().with_hostname("localhost").with_port(12345));
                auto socket = co_await net::initiate(pre, scope.get_context());

                std::string request = "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n";
                std::cout << "async_send\n";
                co_await net::async_send(socket, net::buffer(request));

                char buffer[1024];
                std::cout << "reading\n";
                for (std::size_t n; (n = co_await net::async_receive(socket, net::buffer(buffer)));) {
                    std::cout << "read n=" << n << "\n";
                    std::cout.write(buffer, static_cast<std::streamsize>(n));
                }
            } catch (const std::exception& e) {
                std::cout << "exception: " << e.what() << "\n";
            }
        }() | ex::upon_error([](auto&&) noexcept {}) | ex::then([]() noexcept { std::cout << "running connection DONE!\n"; }),
        scope.get_token());

    std::cout << "spawned\n";

    ex::sync_wait(scope.run());
}
