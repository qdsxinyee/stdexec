// netexec/net/tls/context.hpp                                           -*-C++-*-
// TLS security properties and context configuration.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include <cstdint>
#include <stdexec/__detail/__query.hpp>
#include <string>

namespace netexec::net::tls {

namespace security_props {
struct secure_t : stdexec::__query<secure_t> {};
struct certificate_t : stdexec::__query<certificate_t> {};
struct private_key_t : stdexec::__query<private_key_t> {};
struct ca_bundle_t : stdexec::__query<ca_bundle_t> {};
struct use_system_trust_store_t : stdexec::__query<use_system_trust_store_t> {};

inline constexpr secure_t secure_tag{};
inline constexpr certificate_t certificate_tag{};
inline constexpr private_key_t private_key_tag{};
inline constexpr ca_bundle_t ca_bundle_tag{};
inline constexpr use_system_trust_store_t use_system_trust_store_tag{};
} // namespace security_props

struct secure : stdexec::prop<security_props::secure_t, bool> {
    explicit secure(bool value = true)
        : prop{security_props::secure_tag, value} {}
};

struct certificate : stdexec::prop<security_props::certificate_t, std::string> {
    explicit certificate(std::string value)
        : prop{security_props::certificate_tag, std::move(value)} {}
    explicit certificate(const char* value)
        : prop{security_props::certificate_tag, std::string(value)} {}
};

struct private_key : stdexec::prop<security_props::private_key_t, std::string> {
    explicit private_key(std::string value)
        : prop{security_props::private_key_tag, std::move(value)} {}
    explicit private_key(const char* value)
        : prop{security_props::private_key_tag, std::string(value)} {}
};

struct ca_bundle : stdexec::prop<security_props::ca_bundle_t, std::string> {
    explicit ca_bundle(std::string value)
        : prop{security_props::ca_bundle_tag, std::move(value)} {}
    explicit ca_bundle(const char* value)
        : prop{security_props::ca_bundle_tag, std::string(value)} {}
};

struct use_system_trust_store : stdexec::prop<security_props::use_system_trust_store_t, bool> {
    explicit use_system_trust_store(bool value = true)
        : prop{security_props::use_system_trust_store_tag, value} {}
};

} // namespace netexec::net::tls
