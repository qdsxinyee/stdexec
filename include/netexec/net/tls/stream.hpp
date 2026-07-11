// netexec/net/tls/stream.hpp                                            -*-C++-*-
// High-level TAPS stream that carries a TLS session over a TCP socket.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include <netexec/net/ip/tcp/socket.hpp>
#include <netexec/net/tls/__detail/tls_session_base.hpp>

#include <memory>
#include <utility>

namespace netexec::net::tls {

// A stream returned by async_initiate. It wraps a TCP socket and an optional
// TLS session. When no TLS session is present (secure=false), operations
// degenerate to plain socket I/O.
class stream {
  public:
    stream() = default;

    stream(ip::tcp::socket socket, std::unique_ptr<__detail::session_base> session = nullptr)
        : socket_(std::move(socket)), session_(std::move(session)) {}

    stream(stream&&)            = default;
    stream& operator=(stream&&) = default;

    auto secure() const noexcept -> bool { return this->session_ != nullptr; }

    auto socket() noexcept -> ip::tcp::socket& { return this->socket_; }
    auto socket() const noexcept -> const ip::tcp::socket& { return this->socket_; }

    auto session() noexcept -> __detail::session_base* { return this->session_.get(); }
    auto session() const noexcept -> const __detail::session_base* { return this->session_.get(); }

  private:
    ip::tcp::socket                            socket_;
    std::unique_ptr<__detail::session_base>    session_;
};

} // namespace netexec::net::tls
