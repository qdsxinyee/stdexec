#include <cassert>
// include/beman/net/detail/scope.hpp                                 -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_INCLUDE_BEMAN_NET_DETAIL_SCOPE
#define INCLUDED_INCLUDE_BEMAN_NET_DETAIL_SCOPE
#include <netexec/__detail/netexec_detail.hpp>

#include <netexec/__detail/io_context.hpp>
#include <netexec/__detail/get_io_handle.hpp>
#include <netexec/__detail/get_scope_token.hpp>

// ----------------------------------------------------------------------------

namespace netexec::detail {
class scope;
}
namespace netexec {
using scope = netexec::detail::scope;
}

// ----------------------------------------------------------------------------

class netexec::detail::scope {
  public:
    class token;
    class env {
        friend class token;

      public:
        auto query(const netexec::get_io_handle_t&) const noexcept {
            return this->_scope->_io_context.get_handle();
        }
        auto query(const stdexec::get_scheduler_t&) const noexcept {
            return this->_scope->_io_context.get_scheduler();
        }
        auto query(const stdexec::get_start_scheduler_t&) const noexcept {
            return this->_scope->_io_context.get_scheduler();
        }
        template <typename Signal>
        auto query(const stdexec::get_completion_scheduler_t<Signal>&) const noexcept {
            return this->_scope->_io_context.get_scheduler();
        }
        template <typename Signal, typename Env>
        auto query(const stdexec::get_completion_scheduler_t<Signal>&,
                   [[maybe_unused]] const Env&) const noexcept {
            return this->_scope->_io_context.get_scheduler();
        }
        auto query(const netexec::get_scope_token_t&) const noexcept { return this->_scope->get_token(); }

      private:
        env(scope* s) : _scope(s) {}
        scope* _scope;
    };
    class token {
      public:
        token(scope* s) : _scope(s), _counting_token(s->_counting_scope.get_token()) {}

        auto try_associate() const noexcept -> auto { return this->_counting_token.try_associate(); }
        template <stdexec::sender Sender, typename... Env>
        auto wrap(Sender&& sndr, const Env&... ev) const noexcept -> stdexec::sender auto {
            return this->_counting_token.wrap(
                stdexec::write_env(std::forward<Sender>(sndr), env(this->_scope)), ev...);
        }

      private:
        scope*                                  _scope;
        stdexec::counting_scope::token _counting_token;
    };

    auto run() -> auto {
        return stdexec::when_all(this->_counting_scope.join(), this->_io_context.async_run());
    }

    auto get_context() -> netexec::io_context& { return this->_io_context; }
    auto get_scheduler() -> netexec::io_context::scheduler_type { return this->_io_context.get_scheduler(); }
    auto get_token() -> token { return {this}; }

  private:
    netexec::io_context           _io_context;
    stdexec::counting_scope _counting_scope;
};
static_assert(stdexec::scope_token<netexec::detail::scope::token>);

// ----------------------------------------------------------------------------

#endif
