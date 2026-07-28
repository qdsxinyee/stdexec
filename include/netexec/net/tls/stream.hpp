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

    stream(stream&&) = default;
    // ip::tcp::socket has no move assignment, so stream cannot be
    // move-assigned either; declare this explicitly instead of relying on
    // a defaulted operator that is defined-as-deleted.
    stream& operator=(stream&&) = delete;

    auto secure() const noexcept -> bool { return this->session_ != nullptr; }

    auto socket() noexcept -> ip::tcp::socket& { return this->socket_; }
    auto socket() const noexcept -> const ip::tcp::socket& { return this->socket_; }

    auto session() noexcept -> __detail::session_base* { return this->session_.get(); }
    auto session() const noexcept -> const __detail::session_base* { return this->session_.get(); }

    // Extract the underlying TCP socket, transferring its ownership to the
    // caller.  Any TLS session is discarded without a close_notify, so for a
    // secure stream callers should normally run async_shutdown first; any
    // plaintext still buffered inside the session is lost.  The stream is
    // left empty (secure() == false) and must not be used for I/O afterwards.
    auto release_socket() -> ip::tcp::socket {
        this->session_.reset();
        return std::move(this->socket_);
    }

  private:
    ip::tcp::socket                            socket_;
    std::unique_ptr<__detail::session_base>    session_;
};

} // namespace netexec::net::tls
