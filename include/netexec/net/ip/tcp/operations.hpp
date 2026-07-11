// netexec/net/ip/tcp/operations.hpp                                     -*-C++-*-
// Low-level socket CPOs. These map directly to the existing netexec
// implementation and operate on raw buffers (partial send/receive).
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once
#include <netexec/__detail/operations.hpp>
#include <netexec/net/buffer.hpp>
#include <netexec/net/ip/address.hpp>
#include <netexec/net/ip/tcp/socket.hpp>

#include <exec/task.hpp>
#ifndef _MSC_VER
#include <netdb.h>
#endif
#include <cstdint>
#include <string>
#include <vector>

namespace netexec::net::ip::tcp {

// Resolve a hostname and port to a list of endpoints.
inline constexpr struct async_resolve_t {
    using resolve_task_context =
        ::exec::__task::__default_task_context_impl<::exec::__task::__scheduler_affinity::__none>;

    auto operator()(io_context& ctx, const std::string& hostname, std::uint16_t port) const
        -> ::exec::basic_task<std::vector<ip::tcp::endpoint>, resolve_task_context> {
        // Run getaddrinfo on the io_context's blocking scheduler and return
        // the result back onto the io_context's I/O scheduler.
        auto        blocking_scheduler = ctx.get_blocking_scheduler();
        auto        io_scheduler       = ctx.get_scheduler();

        co_await stdexec::schedule(blocking_scheduler);

        ::addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        ::addrinfo* res     = nullptr;
        std::string service = std::to_string(port);
        int         err = ::getaddrinfo(hostname.c_str(), service.c_str(), &hints, &res);

        std::vector<ip::tcp::endpoint> endpoints;
        if (err == 0 && res != nullptr) {
            for (::addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
                if (ai->ai_family == AF_INET) {
                    auto* sin = reinterpret_cast<::sockaddr_in*>(ai->ai_addr);
                    endpoints.emplace_back(ip::address_v4(ntohl(sin->sin_addr.s_addr)), port);
                } else if (ai->ai_family == AF_INET6) {
                    auto* sin6 = reinterpret_cast<::sockaddr_in6*>(ai->ai_addr);
                    endpoints.emplace_back(ip::address_v6(sin6->sin6_addr.s6_addr), port);
                }
            }
            ::freeaddrinfo(res);
        }

        co_await stdexec::schedule(io_scheduler);
        co_return endpoints;
    }
} async_resolve{};

// Connect a socket to an endpoint.
struct async_connect_t : ::netexec::detail::sender_cpo<::netexec::detail::connect_desc> {};
inline constexpr async_connect_t async_connect{};

// Bind an acceptor to an endpoint and start listening.
// This used to be done implicitly by the acceptor constructor; it is now an
// explicit async operation so the inner socket API matches POSIX semantics.
inline constexpr struct async_listen_t {
    auto operator()(ip::tcp::acceptor& acc, const ip::tcp::endpoint& ep) const
        -> exec::task<void> {
        acc.bind(ep);
        acc.listen();
        co_return;
    }
} async_listen{};

// Accept a single incoming connection on an acceptor.
// Returns the connected socket and the peer endpoint.
struct async_accept_t : ::netexec::detail::sender_cpo<::netexec::detail::accept_desc> {};
inline constexpr async_accept_t async_accept{};

// async_send / async_receive are the low-level "some" variants of
// the original netexec::async_send / netexec::async_receive. They correspond
// to a single system call and may transfer fewer bytes than requested.
struct async_send_t : ::netexec::detail::sender_cpo<::netexec::detail::send_desc> {};
inline constexpr async_send_t async_send{};

struct async_receive_t : ::netexec::detail::sender_cpo<::netexec::detail::receive_desc> {};
inline constexpr async_receive_t async_receive{};

} // namespace netexec::net::ip::tcp
