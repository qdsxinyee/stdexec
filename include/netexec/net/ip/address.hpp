// netexec/net/ip/address.hpp                                            -*-C++-*-
// Internet protocol types exposed at the netexec::net::ip public layer.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once
#include <netexec/internet.hpp>

namespace netexec::net::ip {
using address    = ::netexec::ip::address;
using address_v4 = ::netexec::ip::address_v4;
using address_v6 = ::netexec::ip::address_v6;
using ::netexec::ip::make_address;
// Note: tcp lives in netexec::net::ip::tcp (a namespace), not as an alias here.
} // namespace netexec::net::ip
