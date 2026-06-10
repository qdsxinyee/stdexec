// include/beman/net/detail/transport_props.hpp                       -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_INCLUDE_BEMAN_NET_DETAIL_TRANSPORT_PROPS
#define INCLUDED_INCLUDE_BEMAN_NET_DETAIL_TRANSPORT_PROPS

#include <netexec/__detail/transport_preference.hpp>

// ----------------------------------------------------------------------------

namespace netexec::detail {
class transport_props;
}
namespace netexec {
using transport_props = netexec::detail::transport_props;
}

class netexec::detail::transport_props {};

// ----------------------------------------------------------------------------

#endif
