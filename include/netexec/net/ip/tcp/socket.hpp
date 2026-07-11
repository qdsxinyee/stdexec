// netexec/net/ip/tcp/socket.hpp                                         -*-C++-*-
// Low-level socket/acceptor/endpoint types for TCP.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once
#include <netexec/socket.hpp>

namespace netexec::net::ip::tcp {
using socket   = ::netexec::ip::tcp::socket;
using acceptor = ::netexec::ip::tcp::acceptor;
using endpoint = ::netexec::ip::tcp::endpoint;
} // namespace netexec::net::ip::tcp
