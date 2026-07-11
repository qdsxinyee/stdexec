// include/beman/net/detail/tls/tls_context_base.hpp                  -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_INCLUDE_BEMAN_NET_DETAIL_TLS_TLS_CONTEXT_BASE
#define INCLUDED_INCLUDE_BEMAN_NET_DETAIL_TLS_TLS_CONTEXT_BASE

#include <memory>
#include <string_view>
#include <system_error>

// ----------------------------------------------------------------------------

namespace netexec::net::tls::__detail {

class session_base;

// Backend-agnostic TLS context interface.
// Each platform backend (Schannel, OpenSSL, mbedTLS, SecureTransport) provides
// a concrete implementation of this interface.
class context_base {
  public:
    virtual ~context_base() = default;

    // Load a PEM-encoded certificate chain from `path` (server or mutual TLS).
    virtual auto use_certificate_file(std::string_view path) -> std::error_code = 0;

    // Load a PEM-encoded private key from `path`.
    virtual auto use_private_key_file(std::string_view path) -> std::error_code = 0;

    // Load a PEM-encoded CA bundle used to verify the peer.
    virtual auto use_ca_bundle(std::string_view path) -> std::error_code = 0;

    // Use the platform's default trust store (e.g. Windows certificate store,
    // OpenSSL default CA paths, macOS keychain).
    virtual auto use_default_trust_store() -> std::error_code = 0;

    // Set the peer hostname (used for SNI and certificate verification).
    virtual auto set_hostname(std::string_view /*name*/) -> void {}

    // Create a client-side TLS session over the next layer.
    virtual auto create_client_session() -> std::unique_ptr<session_base> = 0;

    // Create a server-side TLS session over the next layer.
    virtual auto create_server_session() -> std::unique_ptr<session_base> = 0;
};

} // namespace netexec::net::tls::__detail

// ----------------------------------------------------------------------------

#endif
