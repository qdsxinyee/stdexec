// netexec/net/tls/acceptor.hpp                                          -*-C++-*-
// High-level TAPS acceptor that may carry a TLS server context.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include <netexec/net/ip/tcp/socket.hpp>
#include <netexec/net/tls/__detail/tls_context_base.hpp>
#include <netexec/net/tls/stream.hpp>

#include <memory>
#include <utility>

namespace netexec::net::tls {

// An acceptor returned by async_listen. It wraps a TCP acceptor and an
// optional TLS server context. When no TLS context is present (secure=false),
// accepted connections are plain sockets.
class acceptor {
  public:
    acceptor() = default;

    acceptor(ip::tcp::acceptor acceptor, std::unique_ptr<__detail::context_base> context = nullptr)
        : base_(std::move(acceptor)), context_(std::move(context)) {}

    acceptor(acceptor&&)            = default;
    acceptor& operator=(acceptor&&) = default;

    auto secure() const noexcept -> bool { return this->context_ != nullptr; }

    auto base() noexcept -> ip::tcp::acceptor& { return this->base_; }
    auto base() const noexcept -> const ip::tcp::acceptor& { return this->base_; }

    auto context() noexcept -> __detail::context_base* { return this->context_.get(); }
    auto context() const noexcept -> const __detail::context_base* { return this->context_.get(); }

  private:
    ip::tcp::acceptor                          base_;
    std::unique_ptr<__detail::context_base>    context_;
};

} // namespace netexec::net::tls
