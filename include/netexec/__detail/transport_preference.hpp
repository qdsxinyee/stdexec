// include/netexec/__detail/transport_preference.hpp            -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_INCLUDE_NETEXEC_DETAIL_TRANSPORT_PREFERENCE
#define INCLUDED_INCLUDE_NETEXEC_DETAIL_TRANSPORT_PREFERENCE

// ----------------------------------------------------------------------------

namespace netexec::detail {
enum class transport_preference { require, prefer, none, avoid, prohibit };
}

// ----------------------------------------------------------------------------

#endif
