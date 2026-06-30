// taps.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Example of the high-level TAPS API in netexec::net.
// TLS is explicitly disabled because it is not yet implemented.

#include <netexec/net.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>
#include <iostream>
#include <string>

namespace ex = stdexec;
namespace net = netexec::net;

int main(int, char*[]) {
    std::cout << std::unitbuf;

    try {
        net::io_context ctx;

        auto remote = net::env{
            net::hostname("localhost"),
            net::port(12345),
            net::secure(false) // TLS not implemented in Phase 3
        };
        net::preconnection pre(remote);

        auto task = [&]() -> exec::task<void> {
            auto conn = co_await net::async_initiate(pre, ctx);

            std::string request =
                "GET / HTTP/1.1\r\n"
                "Host: example.com\r\n"
                "Connection: close\r\n"
                "\r\n";

            std::cout << "sending request\n";
            co_await net::async_send(conn, net::message{request});

            std::cout << "reading response\n";
            auto reply = co_await net::async_receive(conn);
            std::cout << "received " << reply.size() << " bytes\n";
            std::cout.write(reinterpret_cast<const char*>(reply.data()),
                            static_cast<std::streamsize>(reply.size()));
        }() | ex::upon_error([](auto&& e) noexcept {
            std::cout << "connection error\n";
            (void)e;
        });

        ex::sync_wait(std::move(task));
    } catch (const std::exception& e) {
        std::cout << "exception: " << e.what() << "\n";
    }
}
