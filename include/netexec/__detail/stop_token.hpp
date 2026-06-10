// netexec/__detail/stop_token.hpp                                   -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include <stdexec/stop_token.hpp>

namespace netexec::detail {
using stdexec::inplace_stop_source;
using stdexec::inplace_stop_token;
using stdexec::never_stop_token;
using stdexec::stop_callback_for_t;
} // namespace netexec::detail
