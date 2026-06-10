// netexec/task.hpp                                                   -*-C++-*-
// netexec::task<T> — the stdexec coroutine task.
//
// This is exactly exec::basic_task<T> from stdexec. There is no special
// netexec variant: any function returning exec::basic_task<T> can be used
// inside netexec coroutines and vice-versa without any conversion.
//
// Usage:
//   #include <netexec/task.hpp>   // or just #include <exec/task.hpp>
//
//   netexec::task<int> my_coroutine() {
//       co_return 42;
//   }
//   // Equivalently (same type):
//   exec::task<int> my_other_coroutine() {
//       co_return 42;
//   }
//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include <exec/task.hpp>

namespace netexec {

// netexec::task<T> is a direct alias for exec::basic_task<T>.
// They are the same type — no implicit conversion needed.
template <typename T = void>
using task = exec::basic_task<T>;

} // namespace netexec
