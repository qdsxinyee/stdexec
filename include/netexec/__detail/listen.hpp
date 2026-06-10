// include/beman/net/detail/listen.hpp                                -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_INCLUDE_BEMAN_NET_DETAIL_LISTEN
#define INCLUDED_INCLUDE_BEMAN_NET_DETAIL_LISTEN

// ----------------------------------------------------------------------------

namespace netexec::detail {
struct listen_t {};
} // namespace netexec::detail

namespace netexec {
using listen_t = netexec::detail::listen_t;
inline constexpr listen_t listen{};
} // namespace netexec

// ----------------------------------------------------------------------------

#endif
