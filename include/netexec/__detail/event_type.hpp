// include/beman/net/detail/event_type.hpp                            -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_INCLUDE_BEMAN_NET_DETAIL_EVENT_TYPE
#define INCLUDED_INCLUDE_BEMAN_NET_DETAIL_EVENT_TYPE

#include <cinttypes>

// ----------------------------------------------------------------------------

namespace netexec {
enum class event_type { none = 0x00, in = 0x01, out = 0x02, in_out = 0x03 };
constexpr ::netexec::event_type operator&(::netexec::event_type e0, ::netexec::event_type e1) {
    return ::netexec::event_type(::std::uint8_t(e0) & ::std::uint8_t(e1));
}
} // namespace netexec
// ----------------------------------------------------------------------------

#endif
