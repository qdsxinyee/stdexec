// include/beman/net/detail/io_context_scheduler.hpp                -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_BEMAN_NET_DETAIL_IO_CONTEXT_SCHEDULER
#define INCLUDED_BEMAN_NET_DETAIL_IO_CONTEXT_SCHEDULER

// ----------------------------------------------------------------------------

#include <netexec/__detail/execution.hpp>
#include <netexec/__detail/context_base.hpp>
#include <cassert>

// ----------------------------------------------------------------------------

namespace netexec::detail {
class io_context_scheduler;
}

// ----------------------------------------------------------------------------

class netexec::detail::io_context_scheduler {
  private:
    ::netexec::detail::context_base* d_context;

  public:
    using scheduler_concept = stdexec::scheduler_t;

    struct env {
        ::netexec::detail::context_base* d_context;

        template <typename Signal>
        auto query(const stdexec::get_completion_scheduler_t<Signal>&) const noexcept
            -> io_context_scheduler {
            return this->d_context;
        }
        template <typename Signal, typename Env>
        auto query(const stdexec::get_completion_scheduler_t<Signal>&,
                   [[maybe_unused]] const Env&) const noexcept
            -> io_context_scheduler {
            return this->d_context;
        }
    };
    struct sender {
        template <typename Receiver>
        struct state : ::netexec::detail::context_base::task {
            using operation_state_concept = stdexec::operation_state_t;

            ::std::remove_cvref_t<Receiver>     d_receiver;
            ::netexec::detail::context_base* d_context;

            state(Receiver&& receiver, ::netexec::detail::context_base* context)
                : d_receiver(::std::forward<Receiver>(receiver)), d_context(context) {}

            auto start() & noexcept -> void { this->d_context->schedule(this); }
            auto complete() -> void override { stdexec::set_value(::std::move(this->d_receiver)); }
        };

        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t()>;
        ::netexec::detail::context_base* d_context;

        template <typename Receiver>
        auto connect(Receiver&& receiver) -> state<Receiver> {
            return {::std::forward<Receiver>(receiver), this->d_context};
        }

        auto get_env() const noexcept -> env { return {this->d_context}; }
    };

    auto schedule() const noexcept -> sender { return {this->d_context}; }

    // Advertise that this scheduler completes on itself.
    // Required for stdexec's get_completion_scheduler domain-stability check
    // (stdexec::scheduler<T const&> is false, so the scheduler<_Attrs> branch in
    // __get_declfn is skipped; exposing query() directly on the scheduler makes
    // __read_query_t fire instead, which correctly terminates the recursion).
    template <typename Signal>
    auto query(const stdexec::get_completion_scheduler_t<Signal>&) const noexcept
        -> io_context_scheduler {
        return *this;
    }
    auto operator==(const io_context_scheduler&) const -> bool = default;

    io_context_scheduler(::netexec::detail::context_base* context) : d_context(context) { assert(this->d_context); }

    auto get_context() const { return this->d_context; }

    auto cancel(netexec::detail::io_base* cancel_op, netexec::detail::io_base* op) -> void {
        this->d_context->cancel(cancel_op, op);
    }
    auto poll(::netexec::detail::context_base::poll_operation* op) -> ::netexec::detail::submit_result {
        return this->d_context->poll(op);
    }
    auto accept(::netexec::detail::context_base::accept_operation* op) -> ::netexec::detail::submit_result {
        return this->d_context->accept(op);
    }
    auto connect(::netexec::detail::context_base::connect_operation* op) -> ::netexec::detail::submit_result {
        return this->d_context->connect(op);
    }
    auto receive(::netexec::detail::context_base::receive_operation* op) -> ::netexec::detail::submit_result {
        return this->d_context->receive(op);
    }
    auto send(::netexec::detail::context_base::send_operation* op) -> ::netexec::detail::submit_result {
        return this->d_context->send(op);
    }
    auto resume_at(::netexec::detail::context_base::resume_at_operation* op)
        -> ::netexec::detail::submit_result {
        return this->d_context->resume_at(op);
    }
};

static_assert(stdexec::sender<netexec::detail::io_context_scheduler::sender>);
static_assert(stdexec::scheduler<netexec::detail::io_context_scheduler>);

// ----------------------------------------------------------------------------

#endif
