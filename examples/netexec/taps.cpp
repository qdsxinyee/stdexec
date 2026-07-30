// taps.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Example of the high-level TAPS API in netexec::net::tls.
// TLS is explicitly disabled (plaintext) so it can talk to a plain HTTP
// server; set tls::secure(true) for TLS.

#include <netexec/net.hpp>
#include <netexec/net/tls.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>
#include <iostream>
#include <string>

namespace ex  = stdexec;
namespace net = netexec::net;
namespace tls = netexec::net::tls;

int main(int, char*[]) {
    std::cout << std::unitbuf;

    try {
        // netexec::scope drives the io_context event loop (scope.run()) and
        // returns once all spawned work has drained.
        netexec::scope scope;
        auto&          ctx = scope.get_context();

        auto remote = ex::env{
            net::hostname("localhost"),
            net::port(12345),
            tls::secure(false) // plaintext; use tls::secure(true) for TLS
        };
        tls::preconnection pre(remote);

        // NOTE: the lambda is captureless on purpose.  A coroutine lambda's
        // closure object is a temporary that dies at the end of the statement,
        // while the coroutine only starts when spawned below; capturing [&]
        // would leave the coroutine body accessing a dead closure
        // (stack-use-after-scope).  Reference parameters are stored in the
        // coroutine frame instead, and pre/ctx outlive the coroutine.
        auto client = [](tls::preconnection& pre, net::io_context& ctx) -> exec::task<void> {
            tls::stream conn = co_await tls::async_initiate(pre, ctx);

            std::string request =
                "GET / HTTP/1.1\r\n"
                "Host: example.com\r\n"
                "Connection: close\r\n"
                "\r\n";

            std::cout << "sending request\n";
            co_await tls::async_send(conn, net::message{request});

            std::cout << "reading response\n";
            auto reply = co_await tls::async_receive(conn);
            std::cout << "received " << reply.size() << " bytes\n";
            std::cout.write(reinterpret_cast<const char*>(reply.data()),
                            static_cast<std::streamsize>(reply.size()));
        };

        ex::spawn(
            client(pre, ctx) | ex::upon_error([](auto&&) noexcept {
                std::cout << "connection error\n";
            }),
            scope.get_token());

        ex::sync_wait(scope.run());
    } catch (const std::exception& e) {
        std::cout << "exception: " << e.what() << "\n";
    }
}
