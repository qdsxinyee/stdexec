// include/beman/net/detail/internet.hpp                            -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_BEMAN_NET_DETAIL_INTERNET
#define INCLUDED_BEMAN_NET_DETAIL_INTERNET

#include <netexec/__detail/platform.hpp>
#include <netexec/__detail/netfwd.hpp>
#include <netexec/__detail/endpoint.hpp>
#include <array>
#include <compare>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <stdexcept>
#include <string>

#ifndef _MSC_VER
#include <arpa/inet.h>
#endif

// ----------------------------------------------------------------------------

namespace netexec::ip {
using port_type = ::std::uint_least16_t;

class tcp;
class address_v4;
class address_v6;
class address;
template <typename>
class basic_endpoint;
} // namespace netexec::ip

// ----------------------------------------------------------------------------

class netexec::ip::tcp {
  private:
    int d_family;

    constexpr tcp(int f) : d_family(f) {}

  public:
    using endpoint = basic_endpoint<tcp>;
    using socket   = basic_stream_socket<tcp>;
    using acceptor = basic_socket_acceptor<tcp>;

    tcp() = delete;

    static constexpr auto v4() -> tcp { return tcp(PF_INET); }
    static constexpr auto v6() -> tcp { return tcp(PF_INET6); }

    constexpr auto family() const -> int { return this->d_family; }
    constexpr auto type() const -> int { return SOCK_STREAM; }
    constexpr auto protocol() const -> int { return IPPROTO_TCP; }
};

// ----------------------------------------------------------------------------

class netexec::ip::address_v4 {
  public:
    using uint_type = uint_least32_t;
    struct bytes_type;

  private:
    uint_type d_address;

  public:
    constexpr address_v4() noexcept : d_address() {}
    constexpr address_v4(const address_v4&) noexcept = default;
    constexpr address_v4(const bytes_type&);
    explicit constexpr address_v4(uint_type a) : d_address(a) {
        if (!(a <= 0xFF'FF'FF'FF)) {
            throw ::std::out_of_range("IPv4 address is out of range");
        }
    }

    auto operator=(const address_v4& a) noexcept -> address_v4& = default;

    constexpr auto is_unspecified() const noexcept -> bool { return this->to_uint() == 0u; }
    constexpr auto is_loopback() const noexcept -> bool { return (this->to_uint() & 0xFF'00'00'00) == 0x7F'00'00'00; }
    constexpr auto is_multicast() const noexcept -> bool { return (this->to_uint() & 0xF0'00'00'00) == 0xE0'00'00'00; }
    constexpr auto to_bytes() const noexcept -> bytes_type;
    constexpr auto to_uint() const noexcept -> uint_type { return this->d_address; }
    template <typename Allocator = ::std::allocator<char>>
    auto to_string(const Allocator& = Allocator()) const
        -> ::std::basic_string<char, ::std::char_traits<char>, Allocator>;

    static constexpr auto any() noexcept -> address_v4 { return address_v4(); }
    static constexpr auto loopback() noexcept -> address_v4 { return address_v4(0x7F'00'00'01u); }
    static constexpr auto broadcast() noexcept -> address_v4 { return address_v4(0xFF'FF'FF'FFu); }

    friend ::std::ostream& operator<<(::std::ostream& out, const address_v4& a) {
        return out << ((a.d_address >> 24) & 0xFFu) << '.' << ((a.d_address >> 16) & 0xFFu) << '.'
                   << ((a.d_address >> 8) & 0xFFu) << '.' << ((a.d_address >> 0) & 0xFFu);
    }
};

#if 0
constexpr bool operator==(const address_v4& a, const address_v4& b) noexcept;
constexpr bool operator!=(const address_v4& a, const address_v4& b) noexcept;
constexpr bool operator< (const address_v4& a, const address_v4& b) noexcept;
constexpr bool operator> (const address_v4& a, const address_v4& b) noexcept;
constexpr bool operator<=(const address_v4& a, const address_v4& b) noexcept;
constexpr bool operator>=(const address_v4& a, const address_v4& b) noexcept;
// 21.5.6, address_v4 creation:
constexpr address_v4 make_address_v4(const address_v4::bytes_type& bytes);
constexpr address_v4 make_address_v4(address_v4::uint_type val);
constexpr address_v4 make_address_v4(v4_mapped_t, const address_v6& a);
address_v4 make_address_v4(const char* str);
address_v4 make_address_v4(const char* str, error_code& ec) noexcept;
address_v4 make_address_v4(const string& str);
address_v4 make_address_v4(const string& str, error_code& ec) noexcept;
address_v4 make_address_v4(string_view str);
address_v4 make_address_v4(string_view str, error_code& ec) noexcept;
// 21.5.7, address_v4 I/O:
template<class CharT, class Traits>
basic_ostream<CharT, Traits>& operator<<(
basic_ostream<CharT, Traits>& os, const address_v4& addr);
#endif

// ----------------------------------------------------------------------------

class netexec::ip::address_v6 {
  public:
    struct bytes_type : ::std::array<unsigned char, 16> {
        template <typename... T>
        explicit constexpr bytes_type(T... t) : std::array<unsigned char, 16>{{static_cast<unsigned char>(t)...}} {}
    };

  private:
    bytes_type d_bytes;

  public:
    static constexpr auto any() noexcept -> address_v6;
    static constexpr auto loopback() noexcept -> address_v6;

    constexpr address_v6() noexcept;
    constexpr address_v6(const address_v6&) noexcept = default;
    constexpr address_v6(const unsigned char (&addr)[16]) noexcept {
        // std::memcpy is not constexpr on MSVC; use an explicit loop which is
        // constexpr on all three platforms under C++20 and later.
#ifdef _MSC_VER
        for (int i = 0; i < 16; ++i)
            d_bytes[i] = addr[i];
#else
        ::std::memcpy(d_bytes.data(), addr, 16);
#endif
    }

    auto           operator=(const address_v6&) noexcept -> address_v6& = default;
    constexpr auto operator==(const address_v6&) const -> bool          = default;
    constexpr auto operator<=>(const address_v6&) const -> ::std::strong_ordering;

    auto get_address(::sockaddr_in6& addr, ::netexec::ip::port_type port) const -> ::socklen_t {
        addr.sin6_family   = AF_INET6;
        addr.sin6_port     = htons(port);
        addr.sin6_flowinfo = 0;
        ::std::memcpy(addr.sin6_addr.s6_addr, this->d_bytes.data(), 16);
        addr.sin6_scope_id = 0;
        return sizeof(::sockaddr_in6);
    }

    constexpr auto is_unspecified() const noexcept -> bool;
    constexpr auto is_loopback() const noexcept -> bool;
    constexpr auto is_multicast() const noexcept -> bool;
    constexpr auto is_link_local() const noexcept -> bool;
    constexpr auto is_site_local() const noexcept -> bool;
    constexpr auto is_v4_mapped() const noexcept -> bool;
    constexpr auto is_multicast_node_local() const noexcept -> bool;
    constexpr auto is_multicast_link_local() const noexcept -> bool;
    constexpr auto is_multicast_site_local() const noexcept -> bool;
    constexpr auto is_multicast_org_local() const noexcept -> bool;
    constexpr auto is_multicast_global() const noexcept -> bool;
    constexpr auto to_bytes() const noexcept -> bytes_type;
    template <typename Allocator = ::std::allocator<char>>
    auto to_string(const Allocator& = {}) const -> ::std::basic_string<char, ::std::char_traits<char>, Allocator>;

    friend ::std::ostream& operator<<(::std::ostream& out, const address_v6&) {
        //-dk:TODO
        return out << "<TODO>::1";
    }
};

inline constexpr netexec::ip::address_v6::address_v6() noexcept : d_bytes() {}

inline constexpr auto netexec::ip::address_v6::any() noexcept -> ::netexec::ip::address_v6 {
    return ::netexec::ip::address_v6();
}

inline constexpr auto netexec::ip::address_v6::loopback() noexcept -> ::netexec::ip::address_v6 {
    const unsigned char bytes[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    return ::netexec::ip::address_v6(bytes);
}

inline constexpr auto netexec::ip::address_v6::is_unspecified() const noexcept -> bool {
    for (int i = 0; i < 16; ++i)
        if (this->d_bytes[i] != 0u)
            return false;
    return true;
}

inline constexpr auto netexec::ip::address_v6::is_loopback() const noexcept -> bool {
    for (int i = 0; i < 15; ++i)
        if (this->d_bytes[i] != 0u)
            return false;
    return this->d_bytes[15] == 1u;
}

inline constexpr auto netexec::ip::address_v6::is_multicast() const noexcept -> bool {
    return this->d_bytes[0] == 0xFFu;
}

// ----------------------------------------------------------------------------

class netexec::ip::address {
  private:
    union address_t {
        ::sockaddr_storage storage;
        ::sockaddr_in      inet;
        ::sockaddr_in6     inet6;
    };

    address_t d_address;

  public:
    constexpr address() noexcept : d_address() { this->d_address.storage.ss_family = PF_INET; }
    constexpr address(const address&) noexcept = default;
    /*-dk:TODO constexpr*/ address(const ::netexec::ip::address_v4& addr) noexcept {
        this->d_address.inet.sin_family      = AF_INET;
        this->d_address.inet.sin_addr.s_addr = htonl(addr.to_uint());
        this->d_address.inet.sin_port        = 0xFF'FF;
    }
    /*-dk:TODO constexpr*/ address(const ::netexec::ip::address_v6& addr) noexcept {
        addr.get_address(this->d_address.inet6, 0xFF'FF);
    }

    auto operator=(const address&) noexcept -> address& = default;
    auto operator=(const ::netexec::ip::address_v4&) noexcept -> address&;
    auto operator=(const ::netexec::ip::address_v6&) noexcept -> address&;

    auto           data() const -> const ::sockaddr_storage& { return this->d_address.storage; }
    constexpr auto is_v4() const noexcept -> bool { return this->d_address.storage.ss_family == PF_INET; }
    constexpr auto is_v6() const noexcept -> bool { return this->d_address.storage.ss_family == PF_INET6; }
    /*constexpr -dk:TODO*/ auto to_v4() const -> ::netexec::ip::address_v4 {
        return ::netexec::ip::address_v4(
            ntohl(reinterpret_cast<const ::sockaddr_in&>(this->d_address.storage).sin_addr.s_addr));
    }
    constexpr auto to_v6() const -> ::netexec::ip::address_v6 {
        return ::netexec::ip::address_v6(this->d_address.inet6.sin6_addr.s6_addr);
    }
    constexpr auto is_unspecified() const noexcept -> bool;
    constexpr auto is_loopback() const noexcept -> bool;
    constexpr auto is_multicast() const noexcept -> bool;
    template <class Allocator = ::std::allocator<char>>
    auto to_string(const Allocator& = Allocator()) const
        -> ::std::basic_string<char, ::std::char_traits<char>, Allocator>;
    friend ::std::ostream& operator<<(::std::ostream& out, const address& a) {
        if (a.is_v4())
            return out << a.to_v4();
        else
            return out << a.to_v6();
    }
};

inline constexpr auto netexec::ip::address::is_unspecified() const noexcept -> bool {
    return this->is_v4() ? this->to_v4().is_unspecified() : this->to_v6().is_unspecified();
}

inline constexpr auto netexec::ip::address::is_loopback() const noexcept -> bool {
    if (this->is_v4()) {
        auto raw = this->d_address.inet.sin_addr.s_addr;
        if constexpr (::std::endian::native == ::std::endian::little) {
            return (raw & 0xFFu) == 0x7Fu;
        } else {
            return (raw >> 24) == 0x7Fu;
        }
    }
    return this->to_v6().is_loopback();
}

inline constexpr auto netexec::ip::address::is_multicast() const noexcept -> bool {
    if (this->is_v4()) {
        auto raw = this->d_address.inet.sin_addr.s_addr;
        if constexpr (::std::endian::native == ::std::endian::little) {
            return (raw & 0xFFu) == 0xE0u;
        } else {
            return (raw >> 24) == 0xE0u;
        }
    }
    return this->to_v6().is_multicast();
}

// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------

namespace netexec::ip {

inline auto make_address(const ::std::string& str) -> address {
#ifdef _MSC_VER
    ::sockaddr_in sin{};
    if (::InetPtonA(AF_INET, str.c_str(), &sin.sin_addr) == 1) {
        return address(address_v4(ntohl(sin.sin_addr.s_addr)));
    }
    ::sockaddr_in6 sin6{};
    if (::InetPtonA(AF_INET6, str.c_str(), &sin6.sin6_addr) == 1) {
        return address(address_v6(sin6.sin6_addr.s6_addr));
    }
#else
    ::in_addr addr4{};
    if (::inet_pton(AF_INET, str.c_str(), &addr4) == 1) {
        return address(address_v4(ntohl(addr4.s_addr)));
    }
    ::in6_addr addr6{};
    if (::inet_pton(AF_INET6, str.c_str(), &addr6) == 1) {
        return address(address_v6(addr6.s6_addr));
    }
#endif
    throw ::std::invalid_argument("invalid IP address: " + str);
}

} // namespace netexec::ip

// ----------------------------------------------------------------------------

template <typename Protocol>
class netexec::ip::basic_endpoint : public ::netexec::detail::endpoint {
  public:
    using protocol_type = Protocol;

    constexpr basic_endpoint() noexcept : basic_endpoint(::netexec::ip::address(), ::netexec::ip::port_type()) {}
    constexpr basic_endpoint(const ::netexec::detail::endpoint& ep) noexcept : ::netexec::detail::endpoint(ep) {}
    constexpr basic_endpoint(const protocol_type&, ::netexec::ip::port_type) noexcept;
    constexpr basic_endpoint(const ip::address& address, ::netexec::ip::port_type port) noexcept
        : ::netexec::detail::endpoint(&address.data(),
                                         address.is_v4() ? sizeof(::sockaddr_in) : sizeof(::sockaddr_in6)) {
        (address.is_v4() ? reinterpret_cast<::sockaddr_in&>(this->storage()).sin_port
                         : reinterpret_cast<::sockaddr_in6&>(this->storage()).sin6_port) = htons(port);
    }

    constexpr auto protocol() const noexcept -> protocol_type {
        return this->storage().ss_family == PF_INET ? ::netexec::ip::tcp::v4() : ::netexec::ip::tcp::v6();
    }
    /*-dk:TODO constexpr*/ auto address() const noexcept -> ::netexec::ip::address {
        switch (this->storage().ss_family) {
        default:
            return {};
        case PF_INET:
            return ::netexec::ip::address_v4(
                ntohl(reinterpret_cast<const ::sockaddr_in&>(this->storage()).sin_addr.s_addr));
        case PF_INET6:
            return ::netexec::ip::address_v6(
                reinterpret_cast<const ::sockaddr_in6&>(this->storage()).sin6_addr.s6_addr);
        }
    }
    auto           address(const ::netexec::ip::address&) noexcept -> void;
    constexpr auto port() const noexcept -> ::netexec::ip::port_type {
        switch (this->storage().ss_family) {
        default:
            return {};
        case PF_INET:
            return ntohs(reinterpret_cast<const ::sockaddr_in&>(this->storage()).sin_port);
        case PF_INET6:
            return ntohs(reinterpret_cast<const ::sockaddr_in6&>(this->storage()).sin6_port);
        }
    }
    auto port(::netexec::ip::port_type) noexcept -> void;

    auto size() const -> ::socklen_t {
        return this->storage().ss_family == PF_INET ? sizeof(::sockaddr_in) : sizeof(::sockaddr_in6);
    }

    friend ::std::ostream& operator<<(std::ostream& out, const basic_endpoint& ep) {
        return out << ep.address() << ":" << ep.port();
    }
};

// ----------------------------------------------------------------------------

#endif
