// include/beman/net/detail/rendezvous.hpp                            -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_INCLUDE_BEMAN_NET_DETAIL_RENDEZVOUS
#define INCLUDED_INCLUDE_BEMAN_NET_DETAIL_RENDEZVOUS

// ----------------------------------------------------------------------------

namespace netexec::detail {
struct rendezvous_t {};
} // namespace netexec::detail

namespace netexec {
using rendezvous_t = netexec::detail::rendezvous_t;
inline constexpr rendezvous_t rendezvous{};
} // namespace netexec

// ----------------------------------------------------------------------------

#endif
