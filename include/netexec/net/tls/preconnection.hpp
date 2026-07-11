// netexec/net/tls/preconnection.hpp                                     -*-C++-*-
// TAPS preconnection object built from a property environment.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include <cstdint>
#include <netexec/net/properties.hpp>
#include <netexec/net/tls/__detail/tls.hpp>
#include <netexec/net/tls/context.hpp>
#include <memory>
#include <string>

namespace netexec::net::tls {

class preconnection {
  public:
    template <class... Props>
    explicit preconnection(const stdexec::env<Props...>& e) {
        hostname_                = query_or(e, netexec::net::endpoint_props::hostname_tag, std::string{});
        port_                    = query_or(e, netexec::net::endpoint_props::port_tag, std::uint16_t{});
        ip_address_              = query_or(e, netexec::net::endpoint_props::ip_address_tag, std::string{});
        secure_                  = query_or(e, security_props::secure_tag, true);
        certificate_file_        = query_or(e, security_props::certificate_tag, std::string{});
        private_key_file_        = query_or(e, security_props::private_key_tag, std::string{});
        ca_bundle_file_          = query_or(e, security_props::ca_bundle_tag, std::string{});
        use_system_trust_store_  = query_or(e, security_props::use_system_trust_store_tag, true);
        reliability_             = query_or(e, netexec::net::transport_props::reliability_tag, 0);
    }

    auto hostname() const -> const std::string& { return hostname_; }
    auto port() const -> std::uint16_t { return port_; }
    auto ip_address() const -> const std::string& { return ip_address_; }
    auto secure() const -> bool { return secure_; }
    auto reliability() const -> int { return reliability_; }

    auto certificate_file() const -> const std::string& { return certificate_file_; }
    auto private_key_file() const -> const std::string& { return private_key_file_; }
    auto ca_bundle_file() const -> const std::string& { return ca_bundle_file_; }
    auto use_system_trust_store() const -> bool { return use_system_trust_store_; }

    // Create a TLS context appropriate for this preconnection's security
    // configuration. The concrete backend is selected at compile time.
    auto make_context() const -> std::unique_ptr<netexec::net::tls::__detail::context_base> {
        auto ctx = std::make_unique<netexec::net::tls::__detail::context>();
        ctx->set_hostname(this->hostname_);

        if (!this->certificate_file_.empty()) {
            if (auto ec = ctx->use_certificate_file(this->certificate_file_)) {
                // TODO: propagate error to caller instead of ignoring.
                (void)ec;
            }
        }
        if (!this->private_key_file_.empty()) {
            if (auto ec = ctx->use_private_key_file(this->private_key_file_)) {
                (void)ec;
            }
        }
        if (!this->ca_bundle_file_.empty()) {
            if (auto ec = ctx->use_ca_bundle(this->ca_bundle_file_)) {
                // TODO: propagate error to caller instead of ignoring.
                (void)ec;
            }
        }
        if (this->use_system_trust_store_ && this->ca_bundle_file_.empty()) {
            // Only use the default trust store when no custom CA bundle is given.
            (void)ctx->use_default_trust_store();
        }

        return ctx;
    }

  private:
    template <class E, class Query, class Default>
    static auto query_or(const E& e, Query q, Default default_value) -> Default {
        if constexpr (requires { e.query(q); }) {
            return e.query(q);
        } else {
            return default_value;
        }
    }

    std::string hostname_;
    std::uint16_t port_{};
    std::string ip_address_;
    bool secure_ = true;
    std::string certificate_file_;
    std::string private_key_file_;
    std::string ca_bundle_file_;
    bool use_system_trust_store_ = true;
    int reliability_ = 0;
};

} // namespace netexec::net::tls
