// netexec/net/operations.hpp                                            -*-C++-*-
// High-level TAPS CPOs built on top of the low-level socket API.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include <netexec/net/ip/tcp/operations.hpp>

namespace netexec::net {

// Resolve a hostname and port to a list of endpoints.
using netexec::net::ip::tcp::async_resolve;

} // namespace netexec::net
