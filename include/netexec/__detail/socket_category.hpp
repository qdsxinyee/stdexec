// include/beman/net/detail/socket_category.hpp                     -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_BEMAN_NET_DETAIL_SOCKET_CATEGORY
#define INCLUDED_BEMAN_NET_DETAIL_SOCKET_CATEGORY

#include <system_error>

// ----------------------------------------------------------------------------

namespace netexec {
enum class socket_errc : int;
auto socket_category() noexcept -> const ::std::error_category&;
} // namespace netexec

// ----------------------------------------------------------------------------

enum class netexec::socket_errc : int { already_open = 1, not_found };

auto netexec::socket_category() noexcept -> const ::std::error_category& {
    struct category : ::std::error_category {
        auto name() const noexcept -> const char* override final { return "socket"; }
        auto message(int error) const -> ::std::string override final {
            switch (::netexec::socket_errc(error)) {
            default:
                return "none";
            case ::netexec::socket_errc::already_open:
                return "already open";
            case ::netexec::socket_errc::not_found:
                return "not found";
            }
        }
    };
    static const category rc{};
    return rc;
}

// ----------------------------------------------------------------------------

#endif
