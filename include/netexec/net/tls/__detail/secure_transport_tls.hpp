// include/beman/net/detail/tls/secure_transport_tls.hpp               -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_INCLUDE_BEMAN_NET_DETAIL_TLS_SECURE_TRANSPORT_TLS
#define INCLUDED_INCLUDE_BEMAN_NET_DETAIL_TLS_SECURE_TRANSPORT_TLS

#include <netexec/net/tls/__detail/tls_context_base.hpp>
#include <netexec/net/tls/__detail/tls_session_base.hpp>

#if !defined(NETEXEC_TLS_BACKEND_SECURE_TRANSPORT)
#  error "secure_transport_tls.hpp should only be included when NETEXEC_TLS_BACKEND_SECURE_TRANSPORT is defined"
#endif

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>

// ----------------------------------------------------------------------------

namespace netexec::net::tls::__detail {

class secure_transport_tls_session : public session_base {
  public:
    auto handshake_step(std::error_code& ec) -> bool override {
        ec = make_error_code(tls_errc::unsupported_operation);
        return false;
    }

    auto outgoing_data() -> std::span<const std::byte> override { return {}; }

    auto consume_outgoing(std::size_t) -> void override {}

    auto feed_incoming(std::span<const std::byte>, std::size_t& consumed, std::error_code& ec) -> void override {
        consumed = 0;
        ec = make_error_code(tls_errc::unsupported_operation);
    }

    auto shutdown(std::error_code& ec) -> void override {
        ec = make_error_code(tls_errc::unsupported_operation);
    }

    auto encrypt(
        const void*,
        std::size_t,
        void*,
        std::size_t,
        std::size_t&,
        std::error_code& ec) -> void override {
        ec = make_error_code(tls_errc::unsupported_operation);
    }

    auto decrypt(
        const void*,
        std::size_t,
        void*,
        std::size_t,
        std::size_t&,
        std::error_code& ec) -> void override {
        ec = make_error_code(tls_errc::unsupported_operation);
    }
};

class secure_transport_tls_context : public context_base {
  public:
    auto use_certificate_file(std::string_view) -> std::error_code override {
        return make_error_code(tls_errc::unsupported_operation);
    }

    auto use_private_key_file(std::string_view) -> std::error_code override {
        return make_error_code(tls_errc::unsupported_operation);
    }

    auto use_ca_bundle(std::string_view) -> std::error_code override {
        return make_error_code(tls_errc::unsupported_operation);
    }

    auto use_default_trust_store() -> std::error_code override {
        return make_error_code(tls_errc::unsupported_operation);
    }

    auto create_client_session() -> std::unique_ptr<session_base> override {
        return std::make_unique<secure_transport_tls_session>();
    }

    auto create_server_session() -> std::unique_ptr<session_base> override {
        return std::make_unique<secure_transport_tls_session>();
    }
};

} // namespace netexec::net::tls::__detail

// ----------------------------------------------------------------------------

#endif
