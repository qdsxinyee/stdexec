// netexec/net/properties.hpp                                            -*-C++-*-
// Endpoint and transport properties for TAPS preconnections.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include <cstdint>
#include <stdexec/__detail/__query.hpp>
#include <string>

namespace netexec::net {

namespace endpoint_props {
struct hostname_t : stdexec::__query<hostname_t> {};
struct port_t : stdexec::__query<port_t> {};
struct ip_address_t : stdexec::__query<ip_address_t> {};

inline constexpr hostname_t hostname_tag{};
inline constexpr port_t port_tag{};
inline constexpr ip_address_t ip_address_tag{};
} // namespace endpoint_props

struct hostname : stdexec::prop<endpoint_props::hostname_t, std::string> {
    explicit hostname(std::string value)
        : prop{endpoint_props::hostname_tag, std::move(value)} {}
    explicit hostname(const char* value)
        : prop{endpoint_props::hostname_tag, std::string(value)} {}
};

struct port : stdexec::prop<endpoint_props::port_t, std::uint16_t> {
    explicit port(std::uint16_t value)
        : prop{endpoint_props::port_tag, value} {}
};

struct ip_address : stdexec::prop<endpoint_props::ip_address_t, std::string> {
    explicit ip_address(std::string value)
        : prop{endpoint_props::ip_address_tag, std::move(value)} {}
    explicit ip_address(const char* value)
        : prop{endpoint_props::ip_address_tag, std::string(value)} {}
};

namespace transport_props {
struct reliability_t : stdexec::__query<reliability_t> {};
inline constexpr reliability_t reliability_tag{};
} // namespace transport_props

struct reliability : stdexec::prop<transport_props::reliability_t, int> {
    explicit reliability(int value)
        : prop{transport_props::reliability_tag, value} {}
};

} // namespace netexec::net
