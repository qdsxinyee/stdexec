#include <cassert>
// include/beman/net/detail/io_context.hpp                          -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_BEMAN_NET_DETAIL_IO_CONTEXT
#define INCLUDED_BEMAN_NET_DETAIL_IO_CONTEXT
#include <netexec/__detail/netexec_detail.hpp>

// ----------------------------------------------------------------------------

#include <netexec/__detail/platform.hpp>
#include <netexec/__detail/netfwd.hpp>
#include <netexec/__detail/container.hpp>
#include <netexec/__detail/context_base.hpp>
#include <netexec/__detail/internet.hpp>
#include <netexec/__detail/io_context_scheduler.hpp>
#ifdef NET_EXEC_USE_URING
#include <netexec/__detail/uring_context.hpp>
#elif defined(NET_EXEC_USE_IOCP)
#include <netexec/__detail/iocp_context.hpp>
#else
#include <netexec/__detail/poll_context.hpp>
#endif
#include <netexec/__detail/repeat_effect_until.hpp>
#include <exec/static_thread_pool.hpp>
#include <cstdint>
#include <cerrno>
#include <csignal>
#include <limits>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>

// ----------------------------------------------------------------------------

namespace netexec {
class io_context;
}

// ----------------------------------------------------------------------------

class netexec::io_context {
  private:
#ifdef NET_EXEC_USE_URING
    ::std::unique_ptr<::netexec::detail::context_base> d_owned{new ::netexec::detail::uring_context()};
#elif defined(NET_EXEC_USE_IOCP)
    ::std::unique_ptr<::netexec::detail::context_base> d_owned{new ::netexec::detail::iocp_context()};
#else
    ::std::unique_ptr<::netexec::detail::context_base> d_owned{new ::netexec::detail::poll_context()};
#endif
    ::netexec::detail::context_base& d_context{*this->d_owned};

    mutable ::std::mutex d_blocking_pool_mutex;
    mutable ::std::optional<::exec::static_thread_pool> d_blocking_pool;
    int d_blocking_thread_count{};

  public:
    using scheduler_type = ::netexec::detail::io_context_scheduler;
    class executor_type {};

    class handle {
      public:
        explicit handle(netexec::io_context* ctxt) : context(ctxt) {}
        auto get_io_context() const -> netexec::io_context& { return *this->context; }

      private:
        netexec::io_context* context{};
    };
    auto get_handle() -> handle { return handle(this); }

    io_context() {
#ifndef _MSC_VER
        // SIGPIPE does not exist on Windows; suppress it on POSIX so that
        // writing to a closed socket returns an error rather than terminating
        // the process.
        std::signal(SIGPIPE, SIG_IGN);
#endif
    }
    io_context(::netexec::detail::context_base& context) : d_owned(), d_context(context) {}
    io_context(io_context&&) = delete;

    auto make_socket(int d, int t, int p, ::std::error_code& error) -> ::netexec::detail::socket_id {
        return this->d_context.make_socket(d, t, p, error);
    }
    auto release(::netexec::detail::socket_id id, ::std::error_code& error) -> void {
        return this->d_context.release(id, error);
    }
    auto native_handle(::netexec::detail::socket_id id) -> ::netexec::detail::native_handle_type {
        return this->d_context.native_handle(id);
    }
    auto set_option(::netexec::detail::socket_id id,
                    int                             level,
                    int                             name,
                    const void*                     data,
                    ::socklen_t                     size,
                    ::std::error_code&              error) -> void {
        this->d_context.set_option(id, level, name, data, size, error);
    }
    auto bind(::netexec::detail::socket_id                                id,
              const ::netexec::ip::basic_endpoint<::netexec::ip::tcp>& endpoint,
              ::std::error_code&                                             error) {
        this->d_context.bind(id, ::netexec::detail::endpoint(endpoint), error);
    }
    auto listen(::netexec::detail::socket_id id, int no, ::std::error_code& error) {
        this->d_context.listen(id, no, error);
    }
    auto get_scheduler() -> scheduler_type { return scheduler_type(&this->d_context); }

    auto add_work() -> void { this->d_context.add_work(); }
    auto remove_work() -> void { this->d_context.remove_work(); }

    auto set_blocking_thread_count(int thread_count) -> void {
        ::std::lock_guard lock(this->d_blocking_pool_mutex);
        if (this->d_blocking_pool.has_value()) {
            throw ::std::logic_error("blocking thread count must be set before first use of the blocking scheduler");
        }
        this->d_blocking_thread_count = thread_count;
    }

    auto get_blocking_scheduler() const -> ::exec::static_thread_pool::scheduler {
        ::std::lock_guard lock(this->d_blocking_pool_mutex);
        if (!this->d_blocking_pool.has_value()) {
            int count = this->d_blocking_thread_count;
            if (count <= 0) {
                count = static_cast<int>(::std::thread::hardware_concurrency());
                if (count <= 0) {
                    count = 1;
                }
            }
            this->d_blocking_pool.emplace(static_cast<::std::uint32_t>(count));
        }
        return this->d_blocking_pool->get_scheduler();
    }

    template <stdexec::receiver Receiver>
    struct run_one_state {
        using operation_state_concept = ::stdexec::operation_state_t;

        netexec::io_context*         _context;
        ::std::remove_cvref_t<Receiver> _receiver;

        run_one_state(netexec::io_context* context, Receiver&& receiver) noexcept
            : _context(context), _receiver(::std::forward<Receiver>(receiver)) {}
        run_one_state(run_one_state&&) = delete;
        auto start() & noexcept -> void {
            try {
                ::stdexec::set_value(::std::move(this->_receiver), this->_context->run_one());
            } catch (...) {
                //-dk:TODO deal with exceptions in async_run_one
                std::cout << "run_one_state exception caught\n";
            }
        }
    };

    struct run_one_sender {
        using sender_concept = ::stdexec::sender_t;
        using completion_signatures =
            ::stdexec::completion_signatures<::stdexec::set_value_t(std::size_t),
                                                      ::stdexec::set_stopped_t()>;

        netexec::io_context* _context;
        template <stdexec::receiver Receiver>
        auto connect(Receiver&& receiver) {
            return run_one_state<Receiver>(this->_context, ::std::forward<Receiver>(receiver));
        }
    };

    auto async_run_one() { return run_one_sender{this}; }
    auto async_run() {
        return stdexec::let_value(stdexec::just(), [this, last_count = std::size_t(1)]() mutable {
            (void)last_count; //-dk:TODO remove this once no compiler complains about last_count being unused
            return netexec::repeat_effect_until(
                stdexec::just(),
                [this] { return this->async_run_one(); }() |
                    stdexec::then([&last_count](std::size_t count) { last_count = count; }),
                [&last_count] { return last_count == 0; });
        });
    }
    ::std::size_t run_one() { return this->d_context.run_one(); }
    ::std::size_t run() {
        ::std::size_t count{};
        while (::std::size_t c = this->run_one()) {
            count += c;
        }
        return count;
    }
};

// ----------------------------------------------------------------------------

#endif
