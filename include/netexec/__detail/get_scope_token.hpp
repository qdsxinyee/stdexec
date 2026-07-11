// include/beman/net/detail/get_scope_token.hpp                       -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_INCLUDE_BEMAN_NET_DETAIL_GET_SCOPE_TOKEN
#define INCLUDED_INCLUDE_BEMAN_NET_DETAIL_GET_SCOPE_TOKEN
#include <netexec/__detail/netexec_detail.hpp>


// ----------------------------------------------------------------------------

namespace netexec::detail {
struct get_scope_token_t : stdexec::forwarding_query_t {
    template <typename Object>
    auto operator()(const Object& obj) const -> decltype(obj.query(*this)) {
        return obj.query(*this);
    }
};
} // namespace netexec::detail
namespace netexec {
using get_scope_token_t = netexec::detail::get_scope_token_t;
inline constexpr get_scope_token_t get_scope_token{};
} // namespace netexec

// ----------------------------------------------------------------------------

#endif
