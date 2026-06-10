// netexec/__detail/task.hpp                                         -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include <netexec/__detail/netexec_detail.hpp>
#include <netexec/__detail/get_io_handle.hpp>
#include <netexec/__detail/get_scope_token.hpp>
#include <netexec/__detail/io_context.hpp>
#include <netexec/__detail/scope.hpp>
#include <system_error>

namespace netexec::detail {

// task_env: used only for the scope.hpp static_assert that validates the env
// query interface. Not exposed to users; netexec::task is exec::basic_task.
class task_env {
  public:
    using error_types =
        stdexec::completion_signatures<stdexec::set_error_t(std::error_code)>;

    auto query(const netexec::get_io_handle_t&)  const noexcept { return _handle; }
    auto query(const netexec::get_scope_token_t&) const noexcept { return _token;  }

    explicit task_env(const auto& env)
        : _handle(netexec::get_io_handle(env))
        , _token(netexec::get_scope_token(env)) {}
    task_env(task_env&&) = delete;

  private:
    netexec::io_context::handle _handle;
    netexec::scope::token       _token;
};

} // namespace netexec::detail
