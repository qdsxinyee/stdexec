// include/beman/net/detail/basic_socket.hpp                        -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_BEMAN_NET_DETAIL_BASIC_SOCKET
#define INCLUDED_BEMAN_NET_DETAIL_BASIC_SOCKET

// ----------------------------------------------------------------------------

#include <netexec/__detail/netfwd.hpp>
#include <netexec/__detail/socket_base.hpp>
#include <netexec/__detail/io_context_scheduler.hpp>
#include <netexec/__detail/internet.hpp>

// ----------------------------------------------------------------------------

template <typename Protocol>
class netexec::basic_socket : public ::netexec::socket_base {
  public:
    using scheduler_type = ::netexec::detail::io_context_scheduler;
    using protocol_type  = Protocol;

  private:
    ::netexec::detail::context_base* d_context;
    protocol_type                       d_protocol{::netexec::ip::tcp::v6()};
    ::netexec::detail::socket_id     d_id{::netexec::detail::socket_id::invalid};

  public:
    basic_socket() : d_context(nullptr) {}
    basic_socket(::netexec::detail::context_base* context, ::netexec::detail::socket_id id)
        : d_context(context), d_id(id) {}
    basic_socket(basic_socket&& other)
        : d_context(other.d_context),
          d_protocol(other.d_protocol),
          d_id(::std::exchange(other.d_id, ::netexec::detail::socket_id::invalid)) {}
    ~basic_socket() {
        if (this->d_id != ::netexec::detail::socket_id::invalid) {
            ::std::error_code error{};
            this->d_context->release(this->d_id, error);
        }
    }
    auto get_scheduler() noexcept -> scheduler_type { return scheduler_type{this->d_context}; }
    auto id() const -> ::netexec::detail::socket_id { return this->d_id; }
};

// ----------------------------------------------------------------------------

#endif
